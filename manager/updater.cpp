#include "updater.h"

#include <windows.h>
#include <bcrypt.h>
#include <winhttp.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <fstream>
#include <sstream>
#include <thread>
#include <utility>

#include "integrity.h"
#include "settings.h"
#include "version.h"
#include "win_util.h"
#include "zip.h"

namespace fs = std::filesystem;

namespace resorep::ui {
namespace {
constexpr char kLatestReleaseApi[] = "https://api.github.com/repos/SosigDOGGO/ReResorep/releases/latest";
// Download URLs from the API must be this repository's release assets.
constexpr char kAssetPrefix[] = "https://github.com/SosigDOGGO/ReResorep/releases/download/";
constexpr uint64_t kMaxApiBytes = 1u << 20;
constexpr uint64_t kMaxChecksumBytes = 64u << 10;
constexpr uint64_t kMaxZipBytes = 256u << 20;
constexpr uint32_t kMaxFileBytes = 128u << 20;  // ReResorep.exe or d3d11.dll inside the zip

// --- A small JSON reader: enough for GitHub's release object --------------------------

struct Json {
    enum class Type { Null, Bool, Number, String, Array, Object } type = Type::Null;
    bool boolean = false;
    std::string text;  // strings
    std::vector<Json> items;
    std::vector<std::pair<std::string, Json>> fields;

    const Json* Get(const char* key) const {
        if (type != Type::Object) return nullptr;
        for (const auto& [k, v] : fields)
            if (k == key) return &v;
        return nullptr;
    }
    std::string String(const char* key) const {
        const Json* v = Get(key);
        return v && v->type == Type::String ? v->text : std::string();
    }
};

class JsonReader {
public:
    explicit JsonReader(const std::string& s) : p_(s.data()), end_(s.data() + s.size()) {}

    bool ReadDocument(Json& out) {
        if (!Value(out, 0)) return false;
        Space();
        return p_ == end_;
    }

private:
    void Space() {
        while (p_ < end_ && (*p_ == ' ' || *p_ == '\t' || *p_ == '\r' || *p_ == '\n')) ++p_;
    }
    bool Literal(const char* word) {
        const size_t n = std::strlen(word);
        if (size_t(end_ - p_) < n || std::memcmp(p_, word, n) != 0) return false;
        p_ += n;
        return true;
    }
    static void PutUtf8(std::string& s, uint32_t c) {
        if (c < 0x80) {
            s += char(c);
        } else if (c < 0x800) {
            s += char(0xC0 | (c >> 6));
            s += char(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            s += char(0xE0 | (c >> 12));
            s += char(0x80 | ((c >> 6) & 0x3F));
            s += char(0x80 | (c & 0x3F));
        } else {
            s += char(0xF0 | (c >> 18));
            s += char(0x80 | ((c >> 12) & 0x3F));
            s += char(0x80 | ((c >> 6) & 0x3F));
            s += char(0x80 | (c & 0x3F));
        }
    }
    bool Hex4(uint32_t& out) {
        if (end_ - p_ < 4) return false;
        out = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = *p_++;
            out <<= 4;
            if (c >= '0' && c <= '9') out |= uint32_t(c - '0');
            else if (c >= 'a' && c <= 'f') out |= uint32_t(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') out |= uint32_t(c - 'A' + 10);
            else return false;
        }
        return true;
    }
    bool StringBody(std::string& out) {  // after the opening quote
        while (p_ < end_) {
            const char c = *p_++;
            if (c == '"') return true;
            if (uint8_t(c) < 0x20) return false;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (p_ == end_) return false;
            switch (const char e = *p_++) {
                case '"': case '\\': case '/': out += e; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    uint32_t c1;
                    if (!Hex4(c1)) return false;
                    if (c1 >= 0xD800 && c1 < 0xDC00) {  // a surrogate pair
                        uint32_t c2;
                        if (!Literal("\\u") || !Hex4(c2) || c2 < 0xDC00 || c2 > 0xDFFF) return false;
                        c1 = 0x10000 + ((c1 - 0xD800) << 10) + (c2 - 0xDC00);
                    } else if (c1 >= 0xDC00 && c1 <= 0xDFFF) {
                        return false;
                    }
                    PutUtf8(out, c1);
                    break;
                }
                default: return false;
            }
        }
        return false;
    }
    bool Value(Json& out, int depth) {
        if (depth > 64) return false;
        Space();
        if (p_ == end_) return false;
        switch (*p_) {
            case '{': {
                ++p_;
                out.type = Json::Type::Object;
                Space();
                if (p_ < end_ && *p_ == '}') return ++p_, true;
                for (;;) {
                    Space();
                    if (p_ == end_ || *p_++ != '"') return false;
                    std::string key;
                    if (!StringBody(key)) return false;
                    Space();
                    if (p_ == end_ || *p_++ != ':') return false;
                    Json value;
                    if (!Value(value, depth + 1)) return false;
                    out.fields.emplace_back(std::move(key), std::move(value));
                    Space();
                    if (p_ == end_) return false;
                    if (*p_ == ',') { ++p_; continue; }
                    if (*p_ == '}') return ++p_, true;
                    return false;
                }
            }
            case '[': {
                ++p_;
                out.type = Json::Type::Array;
                Space();
                if (p_ < end_ && *p_ == ']') return ++p_, true;
                for (;;) {
                    Json value;
                    if (!Value(value, depth + 1)) return false;
                    out.items.push_back(std::move(value));
                    Space();
                    if (p_ == end_) return false;
                    if (*p_ == ',') { ++p_; continue; }
                    if (*p_ == ']') return ++p_, true;
                    return false;
                }
            }
            case '"':
                ++p_;
                out.type = Json::Type::String;
                return StringBody(out.text);
            case 't': out.type = Json::Type::Bool; out.boolean = true; return Literal("true");
            case 'f': out.type = Json::Type::Bool; return Literal("false");
            case 'n': return Literal("null");
            default: {
                // Numbers are only skipped: nothing read here needs their value.
                if (*p_ == '-') ++p_;
                if (p_ == end_ || *p_ < '0' || *p_ > '9') return false;
                while (p_ < end_ && ((*p_ >= '0' && *p_ <= '9') || *p_ == '.' || *p_ == 'e' || *p_ == 'E' ||
                                     *p_ == '+' || *p_ == '-'))
                    ++p_;
                out.type = Json::Type::Number;
                return true;
            }
        }
    }

    const char* p_;
    const char* end_;
};

std::string Lower(std::string s) {
    for (char& c : s) c = char(std::tolower(uint8_t(c)));
    return s;
}

bool IsSha256Hex(const std::string& s) {
    return s.size() == 64 && s.find_first_not_of("0123456789abcdef") == std::string::npos;
}

std::wstring CurrentExePath() {
    wchar_t buf[MAX_PATH * 4];
    const DWORD n = GetModuleFileNameW(nullptr, buf, DWORD(std::size(buf)));
    return std::wstring(buf, n);
}

Version CurrentVersion() {
    return {RERESOREP_VERSION_MAJOR, RERESOREP_VERSION_MINOR, RERESOREP_VERSION_PATCH};
}

std::wstring WithSuffix(const std::wstring& exeName, const wchar_t* suffix) {  // "ReResorep.exe" -> "ReResorep.old.exe"
    const fs::path p(exeName);
    return p.stem().wstring() + suffix + p.extension().wstring();
}

bool WriteFile(const fs::path& path, const std::vector<uint8_t>& data) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
    out.close();
    return bool(out);
}

bool ReadFile(const fs::path& path, std::vector<uint8_t>& out) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) return false;
    const std::streamoff size = in.tellg();
    if (size < 0) return false;
    out.resize(size_t(size));
    in.seekg(0);
    return bool(in.read(reinterpret_cast<char*>(out.data()), size));
}

std::string WinHttpMessage(DWORD error) {
    switch (error) {
        case ERROR_WINHTTP_NAME_NOT_RESOLVED:
        case ERROR_WINHTTP_CANNOT_CONNECT:
        case ERROR_WINHTTP_CONNECTION_ERROR:
            return "Could not reach GitHub. Check your internet connection.";
        case ERROR_WINHTTP_TIMEOUT: return "GitHub did not answer in time. Try again later.";
        case ERROR_WINHTTP_SECURE_FAILURE: return "The secure connection to GitHub failed.";
        case ERROR_WINHTTP_OPERATION_CANCELLED: return "Cancelled.";
        default: return "The download failed (error " + std::to_string(error) + ").";
    }
}

struct Handle {
    HINTERNET h = nullptr;
    ~Handle() {
        if (h) WinHttpCloseHandle(h);
    }
};
}  // namespace

// --- Versions and release info ---------------------------------------------------------

std::string Version::ToString() const {
    return std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
}

std::optional<Version> ParseVersion(const std::string& text) {
    std::string s = text;
    if (!s.empty() && (s[0] == 'v' || s[0] == 'V')) s.erase(0, 1);
    Version v;
    int* parts[] = {&v.major, &v.minor, &v.patch};
    size_t pos = 0;
    for (int i = 0; i < 3; ++i) {
        const size_t start = pos;
        while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9' && pos - start < 5) ++pos;
        if (pos == start) return std::nullopt;
        *parts[i] = std::stoi(s.substr(start, pos - start));
        if (i < 2) {
            if (pos >= s.size() || s[pos] != '.') return std::nullopt;
            ++pos;
        }
    }
    if (pos != s.size()) return std::nullopt;  // no "-beta" and the like
    return v;
}

int CompareVersions(const Version& a, const Version& b) {
    if (a.major != b.major) return a.major < b.major ? -1 : 1;
    if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
    if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
    return 0;
}

std::string ParseLatestRelease(const std::string& json, ReleaseInfo& out) {
    out = {};
    Json doc;
    if (!JsonReader(json).ReadDocument(doc) || doc.type != Json::Type::Object)
        return "GitHub sent an answer ReResorep could not read.";
    const Json* draft = doc.Get("draft");
    const Json* prerelease = doc.Get("prerelease");
    if ((draft && draft->boolean) || (prerelease && prerelease->boolean)) return "The latest release is not final yet.";
    const auto version = ParseVersion(doc.String("tag_name"));
    if (!version) return "The latest release has no version number ReResorep understands.";
    out.version = *version;
    out.title = doc.String("name");
    if (out.title.empty()) out.title = "ReResorep " + version->ToString();
    out.notes = doc.String("body");
    out.pageUrl = doc.String("html_url");
    if (out.pageUrl.rfind("https://github.com/SosigDOGGO/ReResorep/", 0) != 0)
        out.pageUrl = "https://github.com/SosigDOGGO/ReResorep/releases/latest";

    out.zipName = "ReResorep-" + version->ToString() + "-win64.zip";
    const std::string checksumName = out.zipName + ".sha256.txt";
    const Json* assets = doc.Get("assets");
    if (assets && assets->type == Json::Type::Array) {
        for (const Json& a : assets->items) {
            const std::string name = a.String("name"), url = a.String("browser_download_url");
            if (url.rfind(kAssetPrefix, 0) != 0) continue;
            if (name == out.zipName) out.zipUrl = url;
            else if (name == checksumName) out.checksumUrl = url;
        }
    }
    if (out.zipUrl.empty() || out.checksumUrl.empty())
        return "Release " + version->ToString() + " has no " + out.zipName + " with its checksum file.";
    return {};
}

std::string ParseChecksumFile(const std::string& text, const std::string& fileName) {
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t space = line.find(' ');
        if (space == std::string::npos) continue;
        std::string name = line.substr(space);
        name.erase(0, name.find_first_not_of(" *"));
        if (name != fileName) continue;
        const std::string hash = Lower(line.substr(0, space));
        return IsSha256Hex(hash) ? hash : std::string();
    }
    return {};
}

std::string NotesForDisplay(const std::string& markdown) {
    std::string out;
    std::istringstream in(markdown);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t hashes = 0;
        while (hashes < line.size() && line[hashes] == '#') ++hashes;
        if (hashes && hashes < line.size() && line[hashes] == ' ') line.erase(0, hashes + 1);
        std::string clean;
        for (size_t i = 0; i < line.size(); ++i) {
            if (line[i] == '*' && i + 1 < line.size() && line[i + 1] == '*') { ++i; continue; }
            if (line[i] == '`') continue;
            if (line[i] == '[') {  // [text](link) -> text
                const size_t close = line.find("](", i);
                const size_t end = close == std::string::npos ? close : line.find(')', close);
                if (end != std::string::npos) {
                    clean += line.substr(i + 1, close - i - 1);
                    i = end;
                    continue;
                }
            }
            clean += line[i];
        }
        out += clean + "\n";
    }
    while (!out.empty() && out.back() == '\n') out.pop_back();
    return out;
}

std::string Sha256Hex(const uint8_t* data, size_t size) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) return {};
    uint8_t digest[32];
    bool ok = BCRYPT_SUCCESS(BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0));
    for (size_t done = 0; ok && done < size;) {
        const ULONG chunk = ULONG(std::min<size_t>(size - done, 1u << 30));
        ok = BCRYPT_SUCCESS(BCryptHashData(hash, const_cast<PUCHAR>(data + done), chunk, 0));
        done += chunk;
    }
    ok = ok && BCRYPT_SUCCESS(BCryptFinishHash(hash, digest, sizeof(digest), 0));
    if (hash) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);
    if (!ok) return {};
    static constexpr char kHex[] = "0123456789abcdef";
    std::string hex;
    for (uint8_t b : digest) {
        hex += kHex[b >> 4];
        hex += kHex[b & 15];
    }
    return hex;
}

// --- Network ---------------------------------------------------------------------------

std::string HttpGet(const std::string& url, std::vector<uint8_t>& body, uint64_t maxBytes, DownloadProgress* progress,
                    bool githubApi) {
    body.clear();
    if (url.rfind("https://", 0) != 0) return "Only HTTPS downloads are allowed.";
    const std::wstring wurl = Utf8ToWide(url);
    URL_COMPONENTS parts{sizeof(parts)};
    wchar_t host[256] = {}, path[2048] = {};
    parts.lpszHostName = host;
    parts.dwHostNameLength = DWORD(std::size(host));
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = DWORD(std::size(path));
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS)
        return "Bad download address.";

    const std::wstring agent = L"ReResorep/" RERESOREP_VERSION_WSTRING;
    Handle session{WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                               WINHTTP_NO_PROXY_BYPASS, 0)};
    if (!session.h) return WinHttpMessage(GetLastError());
    WinHttpSetTimeouts(session.h, 15000, 15000, 15000, 30000);
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
#ifdef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
    protocols |= WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
#endif
    if (!WinHttpSetOption(session.h, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols))) {
        protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;  // Windows 10 before TLS 1.3 support
        WinHttpSetOption(session.h, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
    }
    Handle connection{WinHttpConnect(session.h, host, parts.nPort, 0)};
    if (!connection.h) return WinHttpMessage(GetLastError());
    Handle request{WinHttpOpenRequest(connection.h, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                      WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)};
    if (!request.h) return WinHttpMessage(GetLastError());
    const wchar_t* headers = githubApi ? L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n"
                                       : L"Accept: application/octet-stream\r\n";
    if (!WinHttpSendRequest(request.h, headers, DWORD(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.h, nullptr))
        return WinHttpMessage(GetLastError());

    DWORD status = 0, size = sizeof(status);
    WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &size, WINHTTP_NO_HEADER_INDEX);
    if (status == 403 || status == 429) return "GitHub is limiting requests right now. Try again in a while.";
    if (status == 404) return githubApi ? "No release was found on GitHub." : "The release file was not found on GitHub.";
    if (status != 200) return "GitHub answered with HTTP status " + std::to_string(status) + ".";

    uint64_t length = 0;
    wchar_t lengthText[32] = {};
    size = sizeof(lengthText);
    if (WinHttpQueryHeaders(request.h, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, lengthText, &size,
                            WINHTTP_NO_HEADER_INDEX))
        length = _wcstoui64(lengthText, nullptr, 10);
    if (length > maxBytes) return "The file on GitHub is larger than expected.";
    if (progress) {
        progress->total = length;
        progress->received = 0;
    }
    if (length) body.reserve(size_t(length));

    std::vector<uint8_t> chunk(64 << 10);
    for (;;) {
        if (progress && progress->cancel) return WinHttpMessage(ERROR_WINHTTP_OPERATION_CANCELLED);
        DWORD read = 0;
        if (!WinHttpReadData(request.h, chunk.data(), DWORD(chunk.size()), &read)) return WinHttpMessage(GetLastError());
        if (read == 0) break;
        if (body.size() + read > maxBytes) return "The file on GitHub is larger than expected.";
        body.insert(body.end(), chunk.begin(), chunk.begin() + read);
        if (progress) progress->received = body.size();
    }
    if (length && body.size() != length) return "The download was cut short. Try again.";
    return {};
}

// --- Staging and swapping files ---------------------------------------------------------

std::string StageUpdate(const fs::path& zip, const std::string& zipSha256, const Version& version,
                        const fs::path& installDir, const std::wstring& exeName, StagedUpdate& out) {
    out = {};
    out.version = version;
    std::vector<uint8_t> zipBytes;
    if (!ReadFile(zip, zipBytes)) return "Could not read the downloaded update.";
    if (Sha256Hex(zipBytes.data(), zipBytes.size()) != Lower(zipSha256))
        return "The downloaded update does not match its checksum. Try again.";
    zipBytes.clear();

    ZipReader reader;
    if (!reader.Open(zip)) return "The downloaded update is not a valid zip: " + reader.error();
    const std::string root = "ReResorep-" + version.ToString() + "/";
    auto extract = [&](const std::string& name, std::vector<uint8_t>& data) -> std::string {
        for (const auto& e : reader.entries()) {
            if (e.name != root + name) continue;
            if (!reader.Extract(e, data, kMaxFileBytes)) return name + " in the update is damaged: " + reader.error();
            return {};
        }
        return "The update has no " + name + ".";
    };
    std::vector<uint8_t> exe, proxy, sums;
    if (std::string e = extract("SHA256SUMS.txt", sums); !e.empty()) return e;
    if (std::string e = extract("ReResorep.exe", exe); !e.empty()) return e;
    if (std::string e = extract("proxy/d3d11.dll", proxy); !e.empty()) return e;

    const std::string sumsText(sums.begin(), sums.end());
    const std::string proxySha = Sha256Hex(proxy.data(), proxy.size());
    if (ParseChecksumFile(sumsText, "ReResorep.exe") != Sha256Hex(exe.data(), exe.size()) ||
        ParseChecksumFile(sumsText, "proxy/d3d11.dll") != proxySha)
        return "The files in the update do not match its SHA256SUMS.txt.";
    if (CheckSeal(exe) != SealState::Valid || CheckSeal(proxy) != SealState::Valid)
        return "The files in the update are not intact ReResorep builds.";

    std::error_code ec;
    const fs::path newExe = installDir / WithSuffix(exeName, L".new");
    if (!WriteFile(newExe, exe)) {
        fs::remove(newExe, ec);
        return "Could not write to the ReResorep folder (" + ToUtf8(installDir) +
               "). Download the update from the release page instead.";
    }
    // The version resource has to match what GitHub says this release is.
    if (FileVersionString(newExe) != version.ToString()) {
        fs::remove(newExe, ec);
        return "The update's ReResorep.exe is not version " + version.ToString() + ".";
    }
    out.exe = newExe;

    // Only a different proxy is staged; an unchanged one stays exactly as it is.
    std::vector<uint8_t> current;
    const fs::path currentProxy = installDir / L"proxy" / L"d3d11.dll";
    if (!ReadFile(currentProxy, current) || current != proxy) {
        fs::create_directories(installDir / L"proxy", ec);
        const fs::path newProxy = installDir / L"proxy" / L"d3d11.new.dll";
        if (!WriteFile(newProxy, proxy)) {
            fs::remove(newProxy, ec);
            fs::remove(newExe, ec);
            out = {};
            return "Could not write the new proxy to " + ToUtf8(installDir / L"proxy") + ".";
        }
        out.proxy = newProxy;
        out.proxyVersion = FileVersionString(newProxy);
        out.proxySha256 = proxySha;
    }
    return {};
}

std::string ApplyUpdate(const StagedUpdate& staged, const fs::path& installDir, const std::wstring& exeName) {
    const fs::path exe = installDir / exeName, oldExe = installDir / WithSuffix(exeName, L".old");
    const fs::path proxy = installDir / L"proxy" / L"d3d11.dll", oldProxy = installDir / L"proxy" / L"d3d11.old.dll";
    std::error_code ec;
    if (staged.exe.empty() || !fs::exists(staged.exe, ec)) return "The downloaded update is gone. Download it again.";
    if (!staged.proxy.empty() && !fs::exists(staged.proxy, ec)) return "The downloaded update is gone. Download it again.";

    bool hadProxy = false, proxySwapped = false;
    auto restoreProxy = [&] {
        if (!proxySwapped) return;
        if (hadProxy) MoveFileExW(oldProxy.c_str(), proxy.c_str(), MOVEFILE_REPLACE_EXISTING);
        else DeleteFileW(proxy.c_str());
    };
    if (!staged.proxy.empty()) {
        hadProxy = fs::exists(proxy, ec);
        if (hadProxy && !MoveFileExW(proxy.c_str(), oldProxy.c_str(), MOVEFILE_REPLACE_EXISTING))
            return "Could not replace proxy\\d3d11.dll (error " + std::to_string(GetLastError()) + ").";
        if (!MoveFileExW(staged.proxy.c_str(), proxy.c_str(), 0)) {
            const DWORD err = GetLastError();
            if (hadProxy) MoveFileExW(oldProxy.c_str(), proxy.c_str(), MOVEFILE_REPLACE_EXISTING);
            return "Could not replace proxy\\d3d11.dll (error " + std::to_string(err) + ").";
        }
        proxySwapped = true;
    }
    // A running exe can be renamed, though not overwritten or deleted.
    if (!MoveFileExW(exe.c_str(), oldExe.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        const DWORD err = GetLastError();
        restoreProxy();
        return "Could not move the current ReResorep.exe aside (error " + std::to_string(err) + ").";
    }
    if (!MoveFileExW(staged.exe.c_str(), exe.c_str(), 0)) {
        const DWORD err = GetLastError();
        MoveFileExW(oldExe.c_str(), exe.c_str(), MOVEFILE_REPLACE_EXISTING);
        restoreProxy();
        return "Could not put the new ReResorep.exe in place (error " + std::to_string(err) + ").";
    }
    return {};
}

bool CleanUpAfterUpdate(const fs::path& installDir, const std::wstring& exeName) {
    bool clean = true;
    for (const fs::path& p : {installDir / WithSuffix(exeName, L".old"), installDir / WithSuffix(exeName, L".new"),
                              installDir / L"proxy" / L"d3d11.old.dll", installDir / L"proxy" / L"d3d11.new.dll"}) {
        std::error_code ec;
        if (!fs::exists(p, ec)) continue;
        if (!DeleteFileW(p.c_str())) clean = false;
    }
    return clean;
}

std::string StartProgram(const fs::path& exe) {
    std::wstring commandLine = L"\"" + exe.wstring() + L"\"";
    const std::wstring dir = exe.parent_path().wstring();
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), commandLine.data(), nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &si, &pi))
        return "error " + std::to_string(GetLastError());
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return {};
}

namespace {
fs::path g_pendingRestart;  // set once the files are swapped
}  // namespace

std::string RunPendingRestart() {
    if (g_pendingRestart.empty()) return {};
    const fs::path exe = std::exchange(g_pendingRestart, {});
    return StartProgram(exe);
}

// --- The About tab's steps --------------------------------------------------------------

Updater::Updater() {
    const fs::path exe = CurrentExePath();
    const fs::path dir = exe.parent_path();
    const std::wstring name = exe.filename().wstring();
    std::error_code ec;
    updatedThisStart_ = fs::exists(dir / WithSuffix(name, L".old"), ec);
    // The previous exe may still be closing after a restart: keep trying for a while.
    if (!CleanUpAfterUpdate(dir, name)) {
        cleanupJob_ = std::async(std::launch::async, [dir, name, stop = stopCleanup_] {
            for (int i = 0; i < 60 && !*stop; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                std::error_code e;
                const fs::path old = dir / WithSuffix(name, L".old");
                if (fs::exists(old, e) && !DeleteFileW(old.c_str())) continue;
                const fs::path oldProxy = dir / L"proxy" / L"d3d11.old.dll";
                if (fs::exists(oldProxy, e) && !DeleteFileW(oldProxy.c_str())) continue;
                return;
            }
        });
    }
}

Updater::~Updater() {
    progress_->cancel = true;
    *stopCleanup_ = true;
}

void Updater::Check() {
    if (step_ == Step::Checking || step_ == Step::Downloading || step_ == Step::Ready) return;
    step_ = Step::Checking;
    error_.clear();
    checkJob_ = std::async(std::launch::async, [] {
        CheckResult r;
        std::vector<uint8_t> body;
        r.error = HttpGet(kLatestReleaseApi, body, kMaxApiBytes, nullptr, true);
        if (r.error.empty()) r.error = ParseLatestRelease(std::string(body.begin(), body.end()), r.release);
        return r;
    });
}

void Updater::Download() {
    if (step_ != Step::Available) return;
    step_ = Step::Downloading;
    error_.clear();
    progress_ = std::make_shared<DownloadProgress>();
    downloadJob_ = std::async(std::launch::async, [release = release_, progress = progress_] {
        DownloadResult r;
        std::vector<uint8_t> checksum, zip;
        r.error = HttpGet(release.checksumUrl, checksum, kMaxChecksumBytes);
        if (!r.error.empty()) return r;
        const std::string expected = ParseChecksumFile(std::string(checksum.begin(), checksum.end()), release.zipName);
        if (expected.empty()) {
            r.error = "The release's checksum file has no hash for " + release.zipName + ".";
            return r;
        }
        r.error = HttpGet(release.zipUrl, zip, kMaxZipBytes, progress.get());
        if (!r.error.empty()) return r;

        wchar_t temp[MAX_PATH + 1];
        GetTempPathW(MAX_PATH + 1, temp);
        const fs::path zipPath = fs::path(temp) / (L"ReResorep-update-" + std::to_wstring(GetCurrentProcessId()) + L".zip");
        if (!WriteFile(zipPath, zip)) {
            r.error = "Could not save the download to " + ToUtf8(zipPath.parent_path()) + ".";
            return r;
        }
        const fs::path exe = CurrentExePath();
        r.error = StageUpdate(zipPath, expected, release.version, exe.parent_path(), exe.filename().wstring(), r.staged);
        std::error_code ec;
        fs::remove(zipPath, ec);
        return r;
    });
}

void Updater::Cancel() {
    if (step_ == Step::Downloading) progress_->cancel = true;
}

void Updater::Poll() {
    using namespace std::chrono_literals;
    if (checkJob_.valid() && checkJob_.wait_for(0s) == std::future_status::ready) {
        CheckResult r = checkJob_.get();
        if (!r.error.empty()) {
            error_ = r.error;
            step_ = Step::Failed;
        } else {
            release_ = std::move(r.release);
            step_ = CompareVersions(release_.version, CurrentVersion()) > 0 ? Step::Available : Step::UpToDate;
        }
    }
    if (downloadJob_.valid() && downloadJob_.wait_for(0s) == std::future_status::ready) {
        DownloadResult r = downloadJob_.get();
        if (progress_->cancel) {
            const fs::path exe = CurrentExePath();
            CleanUpAfterUpdate(exe.parent_path(), exe.filename().wstring());
            step_ = Step::Available;
        } else if (!r.error.empty()) {
            error_ = r.error;
            step_ = Step::Available;  // the user can try again
        } else {
            staged_ = std::move(r.staged);
            step_ = Step::Ready;
        }
    }
}

std::string Updater::InstallAndRestart() {
    if (step_ != Step::Ready) return "There is no update ready to install.";
    const fs::path exe = CurrentExePath();
    if (std::string e = ApplyUpdate(staged_, exe.parent_path(), exe.filename().wstring()); !e.empty()) {
        error_ = e;
        return e;
    }
    step_ = Step::Idle;
    g_pendingRestart = exe;
    return {};
}

}  // namespace resorep::ui
