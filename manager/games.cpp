#include "games.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cstdlib>
#include <cwctype>
#include <fstream>
#include <system_error>

#include "integrity.h"
#include "mod_state.h"
#include "theme.h"
#include "win_util.h"

namespace resorep::ui {
namespace fs = std::filesystem;

namespace {
constexpr char kProxyMarker[] = "ReResorepProxyVersion";
// File sizes of the original Resorep proxies (its application.properties), so we
// can recognise and replace them.
constexpr uintmax_t kLegacyProxySizes[] = {63488,  1944576, 1945088, 2001408, 504832, 2001920, 505344,
                                           506368, 124416,  109056,  110592,  132608, 115712};

bool IsLegacyProxy(const fs::path& dll) {
    std::error_code ec;
    const uintmax_t size = fs::file_size(dll, ec);
    return !ec && std::find(std::begin(kLegacyProxySizes), std::end(kLegacyProxySizes), size) !=
                      std::end(kLegacyProxySizes);
}

bool IsOurProxy(const fs::path& dll) { return FileContains(dll, kProxyMarker); }
bool IsAnyProxy(const fs::path& dll) { return IsOurProxy(dll) || IsLegacyProxy(dll); }
bool IsMicrosoftDll(const fs::path& dll) { return FileCompanyName(dll) == L"Microsoft Corporation"; }

bool IsLowerHex(const std::string& s, size_t length) {
    return s.size() == length && s.find_first_not_of("0123456789abcdef") == std::string::npos;
}

std::string Describe(const char* what, const std::error_code& ec) {
    std::string msg = std::string(what) + ": " + ec.message();
    if (ec == std::errc::permission_denied) msg += " (try running ReResorep as administrator)";
    return msg;
}
}  // namespace

fs::path ManagerSettings::File() { return ExeDirectory() / L"reresorep.ini"; }

void ManagerSettings::Load() {
    IniFile ini;
    ini.Load(File());
    const fs::path root = ExeDirectory();
    auto folder = [&](const char* key, const wchar_t* fallback) {
        const std::string v = ini.GetOr(key, "");
        return v.empty() ? root / fallback : fs::path(Utf8ToWide(v));
    };
    moddedFolder = folder(keys::kModdedFolder, L"modded");
    originalFolder = folder(keys::kOriginalFolder, L"original");
    modCreatorMode = ini.GetBool(keys::kModCreatorMode, false);
    saveTextures = ini.GetBool(keys::kSaveTextures, false);
    lowImpact = ini.GetBool(keys::kLowImpact, false);
    logEnabled = ini.GetBool(keys::kLogEnabled, false);
    logDebug = ini.GetOr(keys::kLogLevel, "") == "debug";
    sort = {ini.GetOr("ui_sort", "hash"), ini.GetBool("ui_sort_descending", false)};
    dumpSort = {ini.GetOr("ui_dumps_sort", "hash"), ini.GetBool("ui_dumps_sort_descending", false)};
    separateSort = ini.GetBool("ui_separate_sort", false);
    previewRotation = ((std::atoi(ini.GetOr("ui_preview_rotation", "180").c_str()) / 90 % 4 + 4) % 4) * 90;
    previewMirror = ini.GetBool("ui_preview_mirror", false);
    sharpZoom = ini.GetBool("ui_sharp_zoom", true);
    shiftClickToType = ini.GetBool("ui_shift_click_to_type", false);
    middleClickJumps = ini.GetOr("ui_middle_click", "autoscroll") == "selected";
    accentColor = ParseHexColor(ini.GetOr("ui_accent_color", ""), kDefaultAccent);
    auto loadFilters = [&](const std::string& prefix) {
        TextureFilters f;
        f.hideNormals = ini.GetBool(prefix + "hide_normals", false);
        f.hideGrayscale = ini.GetBool(prefix + "hide_grayscale", false);
        f.hidePacked = ini.GetBool(prefix + "hide_packed", false);
        f.onlyBlackIce = ini.GetBool(prefix + "only_black_ice", false);
        // "BC4,BC5"
        const std::string hidden = ini.GetOr(prefix + "hide_formats", "");
        for (size_t start = 0; start <= hidden.size();) {
            size_t comma = hidden.find(',', start);
            if (comma == std::string::npos) comma = hidden.size();
            if (comma > start) f.hiddenFormats.insert(hidden.substr(start, comma - start));
            start = comma + 1;
        }
        return f;
    };
    filters = loadFilters("ui_");
    dumpFilters = loadFilters("ui_dumps_");
    separateFilters = ini.GetBool("ui_separate_filters", false);
    games.clear();
    for (const std::string& g : ini.GetAll("game")) games.emplace_back(Utf8ToWide(g));
    // "<sha256>|<size>|<modified>|<previous sha256>|<exe path>"
    exeHashes.clear();
    for (const std::string& line : ini.GetAll("game_sha256")) {
        size_t a = line.find('|'), b = line.find('|', a + 1), c = line.find('|', b + 1), d = line.find('|', c + 1);
        if (a == std::string::npos || b == std::string::npos || c == std::string::npos || d == std::string::npos)
            continue;
        ExeHash h{std::strtoull(line.c_str() + a + 1, nullptr, 10), std::strtoll(line.c_str() + b + 1, nullptr, 10),
                  line.substr(0, a), line.substr(c + 1, d - c - 1)};
        // A hash names folders (games\<hash>), so only real ones are taken; anything else is
        // worked out again from the exe.
        // ("" is a hash that could not be worked out: not retried until the exe changes.)
        if (!h.sha256.empty() && !IsLowerHex(h.sha256, 64)) h = {};
        if (!IsLowerHex(h.previous, 64)) h.previous.clear();
        exeHashes[GameKey(Utf8ToWide(line.substr(d + 1)))] = std::move(h);
    }
    // "<its folder in games>|<exe path>"
    ownFolders.clear();
    for (const std::string& line : ini.GetAll("game_own_folders")) {
        const size_t bar = line.find('|');
        // Only a folder name ReResorep makes (16 hex digits): an edited value such as ".." would
        // point games\<it> elsewhere, and "Remove completely" recycles that folder.
        if (bar != std::string::npos && IsLowerHex(line.substr(0, bar), 16))
            ownFolders[GameKey(Utf8ToWide(line.substr(bar + 1)))] = line.substr(0, bar);
    }
    profile = Utf8ToWide(ini.GetOr("ui_profile", ""));
}

bool ManagerSettings::Save() const {
    IniFile ini;
    ini.Load(File());
    ini.Set(keys::kModdedFolder, WideToUtf8(moddedFolder.wstring()));
    ini.Set(keys::kOriginalFolder, WideToUtf8(originalFolder.wstring()));
    ini.SetBool(keys::kModCreatorMode, modCreatorMode);
    ini.SetBool(keys::kSaveTextures, saveTextures);
    ini.SetBool(keys::kLowImpact, lowImpact);
    ini.SetBool(keys::kLogEnabled, logEnabled);
    ini.Set(keys::kLogLevel, logDebug ? "debug" : "info");
    ini.Set("ui_sort", sort.key);
    ini.SetBool("ui_sort_descending", sort.descending);
    ini.Set("ui_dumps_sort", dumpSort.key);
    ini.SetBool("ui_dumps_sort_descending", dumpSort.descending);
    ini.SetBool("ui_separate_sort", separateSort);
    ini.Set("ui_preview_rotation", std::to_string(previewRotation));
    ini.SetBool("ui_preview_mirror", previewMirror);
    ini.SetBool("ui_sharp_zoom", sharpZoom);
    ini.SetBool("ui_shift_click_to_type", shiftClickToType);
    ini.Set("ui_middle_click", middleClickJumps ? "selected" : "autoscroll");
    ini.Set("ui_accent_color", HexColor(accentColor));
    auto saveFilters = [&](const std::string& prefix, const TextureFilters& f) {
        ini.SetBool(prefix + "hide_normals", f.hideNormals);
        ini.SetBool(prefix + "hide_grayscale", f.hideGrayscale);
        ini.SetBool(prefix + "hide_packed", f.hidePacked);
        ini.SetBool(prefix + "only_black_ice", f.onlyBlackIce);
        std::string hidden;
        for (const std::string& g : f.hiddenFormats) hidden += (hidden.empty() ? "" : ",") + g;
        ini.Set(prefix + "hide_formats", hidden);
    };
    saveFilters("ui_", filters);
    saveFilters("ui_dumps_", dumpFilters);
    ini.SetBool("ui_separate_filters", separateFilters);
    std::vector<std::string> list, hashes, own;
    std::map<std::wstring, std::string> paths;  // GameKey -> the path as added
    for (const auto& g : games) {
        list.push_back(WideToUtf8(g.wstring()));
        paths[GameKey(g)] = list.back();
    }
    // Games removed from the list are remembered too, so adding one back finds its own folders
    // and notices if its exe changed meanwhile.
    auto path = [&](const std::wstring& key) {
        auto it = paths.find(key);
        return it == paths.end() ? WideToUtf8(key) : it->second;
    };
    for (const auto& [key, h] : exeHashes)
        hashes.push_back(h.sha256 + "|" + std::to_string(h.size) + "|" + std::to_string(h.modified) + "|" +
                         h.previous + "|" + path(key));
    for (const auto& [key, folder] : ownFolders) own.push_back(folder + "|" + path(key));
    ini.SetAll("game", list);
    ini.SetAll("game_sha256", hashes);
    ini.SetAll("game_own_folders", own);
    ini.Set("ui_profile", WideToUtf8(profile.wstring()));
    return ini.Save(File());
}

std::wstring GameKey(const fs::path& exe) {
    std::wstring k = exe.lexically_normal().wstring();
    for (wchar_t& c : k) c = wchar_t(std::towlower(c));
    return k;
}

fs::path GameFolder(const std::string& exeSha256) {
    // Callers pass a checked hash; anything else still stays inside games\.
    const std::string name = exeSha256.substr(0, 16);
    return ExeDirectory() / L"games" / Utf8ToWide(IsLowerHex(name, 16) ? name : "invalid");
}

std::string ManagerSettings::ExeSha(const fs::path& exe) const {
    auto it = exeHashes.find(GameKey(exe));
    return it == exeHashes.end() ? std::string() : it->second.sha256;
}

bool ManagerSettings::NeedsHash(const fs::path& exe) const {
    std::error_code ec;
    const uint64_t size = fs::file_size(exe, ec);
    if (ec) return false;  // missing: nothing to hash
    const int64_t modified = fs::last_write_time(exe, ec).time_since_epoch().count();
    auto it = exeHashes.find(GameKey(exe));
    return it == exeHashes.end() || it->second.size != size || it->second.modified != modified;
}

bool ManagerSettings::UsesOwnFolders(const fs::path& exe) const { return ownFolders.count(GameKey(exe)) != 0; }

std::pair<fs::path, fs::path> ManagerSettings::FoldersFor(const fs::path& exe) const {
    auto it = ownFolders.find(GameKey(exe));
    if (it == ownFolders.end()) return {moddedFolder, originalFolder};
    const fs::path root = GameFolder(it->second);
    return {root / L"modded", root / L"original"};
}

bool ManagerSettings::SharesOwnFolders(const fs::path& exe) const {
    auto it = ownFolders.find(GameKey(exe));
    if (it == ownFolders.end()) return false;
    return std::any_of(ownFolders.begin(), ownFolders.end(),
                       [&](const auto& other) { return other.first != it->first && other.second == it->second; });
}

void ManagerSettings::RemoveGame(const fs::path& exe, bool forget) {
    const std::wstring key = GameKey(exe);
    std::erase_if(games, [&](const fs::path& g) { return GameKey(g) == key; });
    if (forget) {
        exeHashes.erase(key);
        ownFolders.erase(key);
    }
    if (!profile.empty() && GameKey(profile) == key) profile.clear();
}

size_t RemarkMods(const fs::path& moddedRoot, const std::string& from, const std::string& to,
                  const std::string& gameName) {
    size_t count = 0;
    std::error_code ec;
    for (fs::directory_iterator it(moddedRoot, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_directory(ec) && ReadModGame(it->path()) == from && WriteModGame(it->path(), to, gameName)) ++count;
    return count;
}

std::string GameLabel(const GameState& g) {
    std::string name = ToUtf8(g.exe.parent_path().filename());
    if (name.empty()) name = ToUtf8(g.exe.filename());
    return g.sha256.empty() ? name : name + " (" + g.sha256.substr(0, 8) + ")";
}

ExeHash HashExe(const fs::path& exe) {
    ExeHash h;
    std::error_code ec;
    h.size = fs::file_size(exe, ec);
    if (ec) return {};
    h.modified = fs::last_write_time(exe, ec).time_since_epoch().count();

    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) return h;
    std::ifstream in(exe, std::ios::binary);
    bool ok = in && BCRYPT_SUCCESS(BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0));
    std::vector<char> buf(4 << 20);
    while (ok && in) {
        in.read(buf.data(), std::streamsize(buf.size()));
        if (in.gcount() > 0)
            ok = BCRYPT_SUCCESS(BCryptHashData(hash, reinterpret_cast<PUCHAR>(buf.data()), ULONG(in.gcount()), 0));
    }
    uint8_t digest[32];
    ok = ok && in.eof() && BCRYPT_SUCCESS(BCryptFinishHash(hash, digest, sizeof(digest), 0));
    if (hash) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);
    if (!ok) return h;  // no hash, but not tried again until the exe changes
    static constexpr char kHex[] = "0123456789abcdef";
    for (uint8_t b : digest) {
        h.sha256 += kHex[b >> 4];
        h.sha256 += kHex[b & 15];
    }
    return h;
}

const char* StatusText(ProxyStatus s) {
    switch (s) {
        case ProxyStatus::NotInstalled: return "Not installed";
        case ProxyStatus::Installed: return "Installed";
        case ProxyStatus::Outdated: return "Update available";
        case ProxyStatus::LegacyResorep: return "Old Resorep proxy";
        case ProxyStatus::Missing: return "Game not found";
        case ProxyStatus::Modified: return "Proxy modified";
    }
    return "";
}

fs::path BundledProxy() { return ExeDirectory() / L"proxy" / L"d3d11.dll"; }

GameState InspectGame(const fs::path& exe, const ManagerSettings& settings) {
    GameState st;
    st.exe = exe;
    st.sha256 = settings.ExeSha(exe);
    if (auto it = settings.exeHashes.find(GameKey(exe)); it != settings.exeHashes.end())
        st.previousSha256 = it->second.previous;
    st.ownFolders = settings.UsesOwnFolders(exe);
    std::error_code ec;
    if (!fs::exists(exe, ec)) {
        st.status = ProxyStatus::Missing;
        return st;
    }
    const fs::path dir = exe.parent_path();
    const fs::path dll = dir / L"d3d11.dll";
    const fs::path ori = dir / L"ori_d3d11.dll";
    if (fs::exists(dll, ec)) {
        // A build from before sealing has no seal (NotSealed): that is just an older proxy.
        if (IsOurProxy(dll))
            st.status = CheckSealedFile(dll) == SealState::Invalid ? ProxyStatus::Modified
                        : FilesEqual(dll, BundledProxy())             ? ProxyStatus::Installed
                                                                      : ProxyStatus::Outdated;
        else if (IsLegacyProxy(dll)) st.status = ProxyStatus::LegacyResorep;
    }
    st.chainsOtherD3D11 = fs::exists(ori, ec) && !IsMicrosoftDll(ori) && !IsAnyProxy(ori);
    if (HasOurProxy(st.status)) {
        st.proxyVersion = FileVersionString(dll);
        if (st.proxyVersion.empty()) st.proxyVersion = "pre-2.0 build";  // built before version info existed
    } else if (st.status == ProxyStatus::LegacyResorep) {
        st.proxyVersion = "1.x";
    }
    return st;
}

std::string WriteGameSettings(const fs::path& exe, const ManagerSettings& s) {
    const auto [modded, original] = s.FoldersFor(exe);
    std::error_code ec;
    fs::create_directories(modded, ec);
    fs::create_directories(original, ec);
    IniFile ini;
    ini.Set(keys::kOriginalFolder, WideToUtf8(original.wstring()));
    ini.Set(keys::kModdedFolder, WideToUtf8(modded.wstring()));
    if (const std::string sha = s.ExeSha(exe); !sha.empty()) ini.Set(keys::kGameSha256, sha);
    ini.SetBool(keys::kModCreatorMode, s.modCreatorMode);
    // Off in low impact mode too, for proxies from before it that don't know that key.
    ini.SetBool(keys::kSaveTextures, s.saveTextures && !s.lowImpact);
    ini.SetBool(keys::kLowImpact, s.lowImpact);
    ini.SetBool(keys::kLogEnabled, s.logEnabled);
    ini.Set(keys::kLogFile, WideToUtf8((exe.parent_path() / L"dll.log").wstring()));
    ini.Set(keys::kLogLevel, s.logDebug ? "debug" : "info");
    return ini.Save(exe.parent_path() / kDllSettingsFile) ? std::string() : "Could not write dllsettings.ini";
}

std::string RequestRedump(const fs::path& exe) {
    std::ofstream out(exe.parent_path() / kRedumpRequestFile, std::ios::binary | std::ios::trunc);
    return out ? std::string() : "Could not write to the game folder";
}

std::string InstallProxy(const fs::path& exe, const ManagerSettings& settings) {
    const fs::path bundled = BundledProxy();
    std::error_code ec;
    if (!fs::exists(bundled, ec)) return "proxy\\d3d11.dll is missing next to ReResorep.exe";
    if (CheckSealedFile(bundled) != SealState::Valid)
        return "proxy\\d3d11.dll next to ReResorep.exe is damaged or was modified - download ReResorep again";
    if (!fs::exists(exe, ec)) return "Game executable not found";

    const fs::path dir = exe.parent_path();
    const fs::path dll = dir / L"d3d11.dll";
    const fs::path ori = dir / L"ori_d3d11.dll";

    if (fs::exists(dll, ec) && !IsAnyProxy(dll)) {
        // The game (or a tool like ReShade) already has a d3d11.dll: keep it and chain to it.
        if (fs::exists(ori, ec)) {
            // Never over an earlier backup: ori_d3d11.dll.bak, then .bak2, .bak3...
            fs::path bak = dir / L"ori_d3d11.dll.bak";
            for (int i = 2; fs::exists(bak, ec) && !ec; ++i) bak = dir / (L"ori_d3d11.dll.bak" + std::to_wstring(i));
            if (!ec) fs::rename(ori, bak, ec);
        }
        if (!ec) fs::rename(dll, ori, ec);
        if (ec) return Describe("Could not keep the existing d3d11.dll", ec);
    }
    // The old Resorep copied System32's d3d11.dll here; a stale copy breaks after
    // Windows updates, and our proxy loads the system one directly.
    if (fs::exists(ori, ec) && IsMicrosoftDll(ori)) fs::remove(ori, ec);

    fs::copy_file(bundled, dll, fs::copy_options::overwrite_existing, ec);
    if (ec) return Describe("Could not copy d3d11.dll", ec);
    if (CheckSealedFile(dll) != SealState::Valid) return "The copied d3d11.dll does not match - try installing again";
    return WriteGameSettings(exe, settings);
}

std::string UninstallProxy(const fs::path& exe) {
    const fs::path dir = exe.parent_path();
    const fs::path dll = dir / L"d3d11.dll";
    const fs::path ori = dir / L"ori_d3d11.dll";
    std::error_code ec;

    if (fs::exists(dll, ec)) {
        if (!IsAnyProxy(dll)) return "d3d11.dll in the game folder is not a Resorep proxy; leaving it alone";
        fs::remove(dll, ec);
        if (ec) return Describe("Could not remove d3d11.dll", ec);
    }
    if (fs::exists(ori, ec)) {
        if (IsMicrosoftDll(ori) || IsAnyProxy(ori)) fs::remove(ori, ec);
        else fs::rename(ori, dll, ec);  // restore the game's own / other tool's d3d11.dll
        if (ec) return Describe("Could not restore ori_d3d11.dll", ec);
    }
    fs::remove(dir / kDllSettingsFile, ec);
    return {};
}

}  // namespace resorep::ui
