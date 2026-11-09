#include "mod_package.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <fstream>
#include <iterator>
#include <optional>
#include <set>
#include <sstream>
#include <vector>

#include "mod_state.h"
#include "settings.h"
#include "texture_index.h"
#include "zip.h"

namespace resorep {
namespace fs = std::filesystem;

namespace {
constexpr char kManifest[] = "reresorep_mod.ini";
constexpr char kLabels[] = "labels.txt";
constexpr uint64_t kMaxPackageBytes = 8ull << 30;  // refuse to unpack more than 8 GB

bool IsDds(const std::string& name) {
    return name.size() > 4 && _stricmp(name.c_str() + name.size() - 4, ".dds") == 0;
}

std::string Slashed(const fs::path& p) {
    std::string s = WideToUtf8(p.wstring());
    std::replace(s.begin(), s.end(), '\\', '/');
    return s;
}

// Splits a zip path into components, refusing anything that could land outside the
// target folder: "..", ".", drive letters, absolute paths, reserved characters.
std::optional<fs::path> SafeRelative(const std::string& name) {
    std::wstring w = Utf8ToWide(name);
    std::replace(w.begin(), w.end(), L'\\', L'/');
    if (w.empty() || w[0] == L'/') return std::nullopt;
    fs::path out;
    std::wstringstream ss(w);
    std::wstring part;
    while (std::getline(ss, part, L'/')) {
        if (part.empty()) continue;
        if (part == L"." || part == L".." || part.find_first_of(L":<>\"|?*") != std::wstring::npos) return std::nullopt;
        if (std::any_of(part.begin(), part.end(), [](wchar_t c) { return c < 32; })) return std::nullopt;
        if (part.back() == L' ' || part.back() == L'.') return std::nullopt;  // Windows would silently trim these
        out /= part;
    }
    if (out.empty()) return std::nullopt;
    return out;
}

// Stage beside the destination so a failed export/import leaves the previous file intact.
class TempOutput {
public:
    explicit TempOutput(const fs::path& target) {
        static std::atomic<unsigned> serial{0};
        for (;;) {
            auto candidate = target.parent_path() / (L".reresorep-" + std::to_wstring(GetCurrentProcessId()) +
                                                     L"-" + std::to_wstring(serial++) + L".tmp");
            HANDLE h = CreateFileW(candidate.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h != INVALID_HANDLE_VALUE) {
                CloseHandle(h);
                path = std::move(candidate);
                return;
            }
            if (GetLastError() != ERROR_FILE_EXISTS) return;
        }
    }
    ~TempOutput() { if (!path.empty()) DeleteFileW(path.c_str()); }
    fs::path path;
};

bool WriteFileAtomic(const fs::path& target, const std::vector<uint8_t>& data, bool overwrite) {
    TempOutput tmp(target);
    if (tmp.path.empty()) return false;
    {
        std::ofstream out(tmp.path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
        out.close();
        if (!out) return false;
    }
    return MoveFileExW(tmp.path.c_str(), target.c_str(), overwrite ? MOVEFILE_REPLACE_EXISTING : 0) != 0;
}

// Where the mod's files start inside the zip: "<manifest name>/", or a single top-level
// folder every texture shares, or the zip root.
std::string PackageRoot(const std::vector<ZipReader::Entry>& entries, const std::string& manifestName) {
    std::set<std::string> tops;
    bool looseFiles = false;
    for (const auto& e : entries) {
        if (!IsDds(e.name)) continue;
        std::string n = e.name;
        std::replace(n.begin(), n.end(), '\\', '/');
        const size_t slash = n.find('/');
        if (slash == std::string::npos) looseFiles = true;
        else tops.insert(n.substr(0, slash + 1));
    }
    if (!manifestName.empty() && tops.size() == 1 && *tops.begin() == manifestName + "/" && !looseFiles)
        return *tops.begin();
    if (tops.size() == 1 && !looseFiles) return *tops.begin();
    return {};
}

struct Manifest {
    std::string name;
    std::string game;  // exe SHA-256 of the game the mod is for, "" = any
    std::vector<std::string> disabled;
};

std::optional<Manifest> ReadManifest(ZipReader& zip) {
    for (const auto& e : zip.entries()) {
        if (e.name != kManifest) continue;
        std::vector<uint8_t> data;
        if (!zip.Extract(e, data, 1 << 20)) return std::nullopt;
        Manifest m;
        std::istringstream in(std::string(data.begin(), data.end()));
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            const std::string key = line.substr(0, eq), value = line.substr(eq + 1);
            if (key == "name") m.name = value;
            else if (key == keys::kGameSha256) m.game = value;
            else if (key == "disabled" && !value.empty()) m.disabled.push_back(value);
        }
        return m;
    }
    return std::nullopt;
}
}  // namespace

std::wstring SafeFolderName(const std::string& name) {
    std::wstring w = Utf8ToWide(name);
    std::erase_if(w, [](wchar_t c) { return c < 32 || wcschr(L"<>:\"/\\|?*", c); });
    while (!w.empty() && (w.back() == L' ' || w.back() == L'.')) w.pop_back();
    while (!w.empty() && w.front() == L' ') w.erase(w.begin());
    if (w == L"." || w == L"..") w.clear();
    return w;
}

PackageResult ExportModPackage(const fs::path& moddedRoot, const std::wstring& rel, const fs::path& zipPath,
                               const ExportOptions& options, const LabelStore& labels) {
    PackageResult r;
    const fs::path folder = moddedRoot / rel;
    const std::string name = WideToUtf8(fs::path(rel).filename().wstring());
    std::error_code dirError;
    if (rel.empty() || name.empty() || !fs::is_directory(folder, dirError)) {
        r.error = "Pick a mod or category to export.";
        return r;
    }
    DisabledSet disabled;
    disabled.Load(moddedRoot);

    TempOutput tmp(zipPath);
    if (tmp.path.empty()) {
        r.error = "Could not create a temporary file for the package.";
        return r;
    }
    ZipWriter zip;
    if (!zip.Open(tmp.path)) {
        r.error = "Could not create " + WideToUtf8(zipPath.filename().wstring()) + ": " + zip.error();
        return r;
    }
    std::string manifest = "format=1\r\nname=" + name + "\r\n";
    // A category exported as a mod is for the same game as the mod it is in.
    const fs::path mod = fs::path(rel).lexically_normal().begin()->wstring();
    if (const std::string game = ReadModGame(moddedRoot / mod); !game.empty())
        manifest += std::string(keys::kGameSha256) + "=" + game + "\r\n";
    if (options.includeTurnedOff && disabled.IsEffectivelyDisabled(rel)) manifest += "disabled=.\r\n";
    LabelStore packed;
    zip.AddDirectory(name);

    std::error_code ec;
    std::vector<fs::path> paths;
    for (fs::recursive_directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) paths.push_back(it->path());
    if (ec) {
        r.error = "Could not list the files to export: " + ec.message();
        return r;
    }
    std::sort(paths.begin(), paths.end());
    for (const fs::path& path : paths) {
        const fs::path inMod = path.lexically_relative(folder);              // path inside the package's mod
        const fs::path inModded = path.lexically_relative(moddedRoot);       // path for the on/off state
        const bool off = disabled.IsEffectivelyDisabled(inModded);
        if (off && !options.includeTurnedOff) continue;
        const bool directory = fs::is_directory(path, ec);
        if (ec) {
            r.error = "Could not inspect " + WideToUtf8(path.wstring()) + ": " + ec.message();
            return r;
        }
        if (directory) {
            if (!zip.AddDirectory(name + "/" + Slashed(inMod))) break;
            ++r.folders;
        } else {
            const std::wstring file = path.filename().wstring();
            if (_wcsicmp(fs::path(file).extension().c_str(), L".dds") != 0) continue;
            std::ifstream in(path, std::ios::binary | std::ios::ate);
            const std::streamoff size = in.tellg();
            if (size < 0 || size > (1ll << 30)) {
                r.error = "Could not read " + WideToUtf8(path.wstring()) + " (1 GB limit per texture).";
                return r;
            }
            std::vector<uint8_t> data(static_cast<size_t>(size));
            in.seekg(0);
            if (!in.read(reinterpret_cast<char*>(data.data()), size)) {
                r.error = "Could not read " + WideToUtf8(path.wstring());
                return r;
            }
            if (!zip.AddFile(name + "/" + Slashed(inMod), data.data(), data.size())) break;
            ++r.textures;
            r.bytes += data.size();
            if (auto hash = ParseModFileName(file); hash && options.includeLabels)
                if (const std::string* label = labels.Get(*hash)) packed.Set(*hash, *label);
        }
        // Only what is switched off itself is recorded; everything under it follows.
        if (disabled.IsDisabled(inModded)) manifest += "disabled=" + Slashed(inMod) + "\r\n";
    }
    if (!zip.error().empty()) {
        r.error = "Export failed: " + zip.error();
        return r;
    }
    if (!r.textures) {
        r.error = "No .dds textures were selected for export.";
        return r;
    }
    zip.AddFile(kManifest, reinterpret_cast<const uint8_t*>(manifest.data()), manifest.size());
    if (packed.size()) {
        const std::string text = packed.Serialize();
        zip.AddFile(kLabels, reinterpret_cast<const uint8_t*>(text.data()), text.size());
        r.labels = packed.size();
    }
    if (!zip.Finish()) r.error = "Export failed: " + zip.error();
    else if (!MoveFileExW(tmp.path.c_str(), zipPath.c_str(), MOVEFILE_REPLACE_EXISTING))
        r.error = "Could not replace " + WideToUtf8(zipPath.wstring());
    return r;
}

PackageInfo InspectModPackage(const fs::path& zipPath) {
    PackageInfo info;
    ZipReader zip;
    if (!zip.Open(zipPath)) {
        info.error = "Could not read " + WideToUtf8(zipPath.filename().wstring()) + ": " + zip.error();
        return info;
    }
    const auto manifest = ReadManifest(zip);
    info.fromReResorep = manifest.has_value();
    const std::string root = PackageRoot(zip.entries(), manifest ? manifest->name : "");
    for (const auto& e : zip.entries()) {
        if (IsDds(e.name) && !e.isDirectory()) {
            ++info.textures;
            info.bytes += e.size;
        }
        if (e.name == kLabels) {
            std::vector<uint8_t> data;
            if (zip.Extract(e, data, 16 << 20)) info.labels = LabelStore::Parse(std::string(data.begin(), data.end())).size();
        }
    }
    if (manifest) info.game = manifest->game;
    if (manifest && !manifest->name.empty()) info.name = manifest->name;
    else if (!root.empty()) info.name = root.substr(0, root.size() - 1);
    else info.name = WideToUtf8(zipPath.stem().wstring());
    if (!info.textures) info.error = "This zip contains no .dds textures.";
    return info;
}

PackageResult ImportModPackage(const fs::path& zipPath, const fs::path& moddedRoot, const std::string& modName,
                               bool overwrite, LabelStore& labels, const std::string& gameSha256) {
    PackageResult r;
    const std::wstring folderName = SafeFolderName(modName);
    if (folderName.empty()) {
        r.error = "Enter a name for the mod.";
        return r;
    }
    ZipReader zip;
    if (!zip.Open(zipPath)) {
        r.error = "Could not read the package: " + zip.error();
        return r;
    }
    const auto manifest = ReadManifest(zip);
    const std::string root = PackageRoot(zip.entries(), manifest ? manifest->name : "");
    const fs::path target = moddedRoot / folderName;

    uint64_t total = 0;
    for (const auto& e : zip.entries()) total += e.size;
    if (total > kMaxPackageBytes) {
        r.error = "The package is larger than 8 GB unpacked; refusing to import it.";
        return r;
    }

    std::error_code ec;
    fs::create_directories(target, ec);
    if (ec) {
        r.error = "Could not create " + WideToUtf8(target.wstring()) + ": " + ec.message();
        return r;
    }
    for (const auto& e : zip.entries()) {
        std::string name = e.name;
        std::replace(name.begin(), name.end(), '\\', '/');
        if (name == kManifest || name == kLabels) continue;
        if (!root.empty()) {
            if (name.compare(0, root.size(), root) != 0) {
                ++r.skipped;
                continue;
            }
            name = name.substr(root.size());
        }
        if (name.empty()) continue;
        const bool dir = e.isDirectory();
        if (!dir && !IsDds(name)) {
            ++r.skipped;  // only textures are unpacked
            continue;
        }
        const auto rel = SafeRelative(name);
        if (!rel) {
            ++r.skipped;
            continue;
        }
        const fs::path out = target / *rel;
        if (dir) {
            fs::create_directories(out, ec);
            if (ec) {
                r.error = "Could not create " + WideToUtf8(out.wstring()) + ": " + ec.message();
                return r;
            }
            ++r.folders;
            continue;
        }
        if (!overwrite && (fs::exists(out, ec) || ec)) {  // can't tell: keep what may be there
            ++r.skipped;
            continue;
        }
        std::vector<uint8_t> data;
        if (!zip.Extract(e, data)) {
            r.error = "\"" + name + "\": " + zip.error();
            return r;
        }
        fs::create_directories(out.parent_path(), ec);
        if (ec || !WriteFileAtomic(out, data, overwrite)) {
            r.error = "Could not write " + WideToUtf8(out.wstring());
            return r;
        }
        ++r.textures;
        r.bytes += data.size();
    }

    if (!gameSha256.empty() && !WriteModGame(target, gameSha256, ""))
        r.error = "Imported, but could not save which game the mod is for.";
    if (manifest && !manifest->disabled.empty()) {
        DisabledSet state;
        state.Load(moddedRoot);
        for (const std::string& p : manifest->disabled) {
            if (p == ".") state.Set(folderName, true);
            else if (const auto rel = SafeRelative(p)) state.Set(fs::path(folderName) / *rel, true);
        }
        if (!state.Save(moddedRoot)) r.error = "Imported, but could not save which textures are turned off.";
    }
    for (const auto& e : zip.entries()) {
        if (e.name != kLabels) continue;
        std::vector<uint8_t> data;
        if (zip.Extract(e, data, 16 << 20))
            r.labels = labels.Merge(LabelStore::Parse(std::string(data.begin(), data.end())), false);
    }
    return r;
}

}  // namespace resorep
