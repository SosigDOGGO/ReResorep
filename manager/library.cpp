#include "library.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <set>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "dds.h"
#include "mod_package.h"
#include "resorep_hash.h"
#include "settings.h"
#include "texture_index.h"
#include "win_util.h"

namespace resorep::ui {
namespace fs = std::filesystem;

namespace {
constexpr char kInfoCacheMagic[4] = {'R', 'R', 'T', '1'};  // bump when CachedInfo changes

fs::path InfoCacheFile() { return ExeDirectory() / L"texture_info.cache"; }

std::string ErrorText(const std::error_code& ec) { return ec ? ec.message() : std::string(); }

// Folder and label names end up on disk; keep to characters Windows allows.
std::wstring SanitizeName(const std::string& name) {
    std::wstring w = Utf8ToWide(name);
    std::erase_if(w, [](wchar_t c) { return c < 32 || wcschr(L"<>:\"/\\|?*", c); });
    while (!w.empty() && (w.back() == L' ' || w.back() == L'.')) w.pop_back();
    return w;
}

// Sorts "a", "a/b", "a b" so children directly follow their parent.
std::wstring TreeKey(const std::wstring& rel) {
    std::wstring k = DisabledSet::Key(rel);
    std::replace(k.begin(), k.end(), L'/', L'\x01');
    return k;
}
}  // namespace

Library::~Library() {
    CloseWatches();
    if (infoUnsaved_) SaveInfoCache();
}

void Library::LoadInfoCache() {
    std::ifstream in(InfoCacheFile(), std::ios::binary);
    char magic[4];
    if (!in.read(magic, 4) || std::memcmp(magic, kInfoCacheMagic, 4) != 0) return;
    for (;;) {
        uint16_t len = 0;
        if (!in.read(reinterpret_cast<char*>(&len), 2)) break;
        std::wstring path(len, L'\0');
        CachedInfo c{};
        uint32_t format = 0;
        in.read(reinterpret_cast<char*>(path.data()), std::streamsize(len) * 2);
        in.read(reinterpret_cast<char*>(&c.modified), 8);
        in.read(reinterpret_cast<char*>(&c.size), 8);
        in.read(reinterpret_cast<char*>(&c.width), 4);
        in.read(reinterpret_cast<char*>(&c.height), 4);
        in.read(reinterpret_cast<char*>(&format), 4);
        if (!in) break;
        c.format = DXGI_FORMAT(format);
        infoCache_[path] = c;
    }
}

void Library::SaveInfoCache() {
    // Entries under the folders shown drop out once their file is gone; other profiles' stay.
    std::unordered_set<std::wstring> scanned;
    for (const auto* list : {&mods_, &dumps_})
        for (const TextureFile& f : *list) scanned.insert(f.path.wstring());
    auto under = [](const std::wstring& path, const fs::path& folder) {
        const std::wstring prefix = folder.wstring() + L"\\";
        return _wcsnicmp(path.c_str(), prefix.c_str(), prefix.size()) == 0;
    };
    std::vector<char> out(kInfoCacheMagic, kInfoCacheMagic + 4);
    auto put = [&](const void* p, size_t n) {
        out.insert(out.end(), static_cast<const char*>(p), static_cast<const char*>(p) + n);
    };
    for (const auto& [path, c] : infoCache_) {
        if (path.size() > 0xFFFF) continue;
        if (!scanned.count(path) && (under(path, modded_) || under(path, original_))) continue;
        const uint16_t len = uint16_t(path.size());
        const uint32_t format = uint32_t(c.format);
        put(&len, 2);
        put(path.data(), path.size() * 2);
        put(&c.modified, 8);
        put(&c.size, 8);
        put(&c.width, 4);
        put(&c.height, 4);
        put(&format, 4);
    }
    infoUnsaved_ = 0;
    const fs::path file = InfoCacheFile();
    const fs::path tmp = file.wstring() + L".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f.write(out.data(), std::streamsize(out.size()));
        if (!f) return;
    }
    std::error_code ec;
    fs::rename(tmp, file, ec);
}

TextureFile Library::MakeFile(const fs::directory_entry& e, uint32_t hash) {
    TextureFile f;
    f.path = e.path();
    f.hash = hash;
    std::error_code ec;
    f.size = e.file_size(ec);
    f.modified = e.last_write_time(ec).time_since_epoch().count();
    const std::wstring name = e.path().filename().wstring();
    const size_t tilde = name.find(L'~');
    if (tilde != std::wstring::npos) f.label = WideToUtf8(name.substr(tilde + 1, name.rfind(L'.') - tilde - 1));

    auto [it, inserted] = infoCache_.try_emplace(f.path.wstring());
    CachedInfo& c = it->second;
    if (inserted || c.modified != f.modified || c.size != f.size) {
        DdsImage header;
        const bool ok = ReadTextureInfo(f.path.wstring(), header);
        c = {f.modified, f.size, ok ? header.width : 0, ok ? header.height : 0, ok ? header.format : DXGI_FORMAT_UNKNOWN};
        ++infoUnsaved_;
    }
    f.width = c.width;
    f.height = c.height;
    f.format = c.format;
    return f;
}

void Library::SetFolders(const fs::path& modded, const fs::path& original, const std::string& game) {
    modded_ = modded;
    game_ = game;
    original_ = original;
    std::error_code ec;
    fs::create_directories(modded_, ec);
    fs::create_directories(original_, ec);
    if (!infoCacheLoaded_) {
        LoadInfoCache();
        infoCacheLoaded_ = true;
    }
    Watch();
    Refresh();
}

void Library::CloseWatches() {
    for (HANDLE& h : watches_) {
        if (h != INVALID_HANDLE_VALUE) FindCloseChangeNotification(h);
        h = INVALID_HANDLE_VALUE;
    }
}

void Library::Watch() {
    CloseWatches();
    constexpr DWORD kFilter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_SIZE |
                              FILE_NOTIFY_CHANGE_LAST_WRITE;
    watches_[0] = FindFirstChangeNotificationW(modded_.c_str(), TRUE, kFilter);
    watches_[1] = FindFirstChangeNotificationW(original_.c_str(), FALSE, kFilter);
}

bool Library::RefreshIfChanged() {
    // A running game dumps textures one after another; rescan once the folder has been quiet
    // for a moment instead of on every file, but at least once a second while it keeps going.
    constexpr auto kQuiet = std::chrono::milliseconds(250);
    constexpr auto kMaxWait = std::chrono::seconds(1);
    const auto now = std::chrono::steady_clock::now();
    for (HANDLE h : watches_) {
        if (h != INVALID_HANDLE_VALUE && WaitForSingleObject(h, 0) == WAIT_OBJECT_0) {
            FindNextChangeNotification(h);
            if (!changePending_) firstChange_ = now;
            changePending_ = true;
            lastChange_ = now;
        }
    }
    if (!changePending_ || (now - lastChange_ < kQuiet && now - firstChange_ < kMaxWait)) return false;
    changePending_ = false;
    Refresh();
    return true;
}

void Library::Refresh() {
    if (batching_) {
        refreshHeld_ = true;
        return;
    }
    mods_.clear();
    dumps_.clear();
    folders_.clear();
    unsafeNames_.clear();
    active_.clear();
    disabled_.Load(modded_);

    std::error_code ec;
    for (fs::recursive_directory_iterator it(modded_, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path rel = it->path().lexically_relative(modded_);
        if (it->is_directory(ec)) {
            ModFolder f;
            f.rel = DisabledSet::Slashed(rel);
            f.name = WideToUtf8(it->path().filename().wstring());
            f.depth = int(std::count(f.rel.begin(), f.rel.end(), L'/')) + 1;
            f.selfDisabled = disabled_.IsDisabled(rel);
            f.enabled = !disabled_.IsEffectivelyDisabled(rel);
            if (f.depth == 1) f.game = ReadModGame(it->path());
            folders_.push_back(std::move(f));
            continue;
        }
        if (!it->is_regular_file(ec)) continue;
        const std::wstring name = it->path().filename().wstring();
        const auto hash = ParseModFileName(name);
        if (!hash) {
            if (IsUnsafeTextureName(name)) unsafeNames_.push_back(it->path());
            continue;
        }
        TextureFile f = MakeFile(*it, *hash);
        f.relDir = DisabledSet::Slashed(rel.parent_path());
        f.selfDisabled = disabled_.IsDisabled(rel);
        f.enabled = !disabled_.IsEffectivelyDisabled(rel);
        mods_.push_back(std::move(f));
    }
    for (fs::directory_iterator it(original_, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const std::wstring name = it->path().filename().wstring();
        if (const auto hash = ParseModFileName(name)) dumps_.push_back(MakeFile(*it, *hash));
        else if (IsUnsafeTextureName(name)) unsafeNames_.push_back(it->path());
    }

    std::sort(folders_.begin(), folders_.end(),
              [](const ModFolder& a, const ModFolder& b) { return TreeKey(a.rel) < TreeKey(b.rel); });
    auto byHash = [](const TextureFile& a, const TextureFile& b) {
        return a.hash != b.hash ? a.hash < b.hash : a.path < b.path;
    };
    std::sort(mods_.begin(), mods_.end(), byHash);
    std::sort(dumps_.begin(), dumps_.end(), byHash);
    // Written in batches: a running game dumps textures one at a time; the rest is saved on exit.
    if (infoUnsaved_ >= 250) SaveInfoCache();

    // Per-folder counts, and the build each texture's mod is marked for.
    std::unordered_map<std::wstring, size_t> folderIndex;
    for (size_t i = 0; i < folders_.size(); ++i) folderIndex[DisabledSet::Key(folders_[i].rel)] = i;
    for (TextureFile& m : mods_) {
        for (std::wstring dir = DisabledSet::Key(m.relDir); !dir.empty();) {
            if (auto it = folderIndex.find(dir); it != folderIndex.end()) {
                folders_[it->second].total++;
                folders_[it->second].active += m.enabled;
                if (folders_[it->second].depth == 1) m.game = folders_[it->second].game;
            }
            const size_t slash = dir.rfind(L'/');
            dir.resize(slash == std::wstring::npos ? 0 : slash);
        }
    }

    // Which file wins per hash (the proxy's rule), for every build that can load it. A game loads
    // the unmarked mods plus those marked for its build; "" stands for a build no mod is marked for.
    std::set<std::string> builds;
    if (!game_.empty()) {
        builds.insert(game_);
    } else {
        builds.insert("");
        for (const TextureFile& m : mods_)
            if (!m.game.empty()) builds.insert(m.game);
    }
    winners_.clear();
    for (const std::string& build : builds) {
        auto& winners = winners_[build];
        std::unordered_map<uint32_t, std::wstring> bestKey;
        for (const TextureFile& m : mods_) {
            if (!m.enabled || !LoadsIn(m, build)) continue;
            const std::wstring key = DisabledSet::Key(m.path.lexically_relative(modded_));
            auto [pos, inserted] = bestKey.try_emplace(m.hash, key);
            if (inserted || key < pos->second) {
                pos->second = key;
                winners[m.hash] = m.path;
            }
        }
    }
    // What the tabs show as "the" mod for a hash: this game's choice, else an unmarked build's,
    // else whichever build's.
    for (const auto& [build, winners] : winners_)
        for (const auto& [hash, path] : winners)
            if (build.empty() || build == game_ || !active_.count(hash)) active_[hash] = path;
    for (TextureFile& m : mods_) {
        m.otherBuild = !game_.empty() && !LoadsIn(m, game_);
        m.unused = m.enabled && !m.otherBuild;
        for (const auto& [build, winners] : winners_) {
            auto it = winners.find(m.hash);
            if (LoadsIn(m, build) && it != winners.end() && it->second == m.path) m.unused = false;
        }
    }
}

void Library::SetGame(const std::string& exeSha256) {
    if (game_ == exeSha256) return;
    game_ = exeSha256;
    if (!modded_.empty()) Refresh();
}

const fs::path* Library::ActiveMod(uint32_t hash) const {
    auto it = active_.find(hash);
    return it == active_.end() ? nullptr : &it->second;
}

const fs::path* Library::ModFor(uint32_t gameHash) const {
    for (int delta : kHashTolerance)
        if (const fs::path* p = ActiveMod(gameHash + uint32_t(delta))) return p;
    return nullptr;
}

std::string Library::SaveState() {
    if (batching_) {
        saveHeld_ = true;
        return {};
    }
    const bool ok = disabled_.Save(modded_);
    Refresh();
    return ok ? std::string() : "Could not write " + WideToUtf8(DisabledSet::kFileName);
}

std::string Library::SetEnabled(const TextureFile& mod, bool enabled) {
    disabled_.Set(mod.path.lexically_relative(modded_), !enabled);
    return SaveState();
}

std::string Library::SetFolderEnabled(const std::wstring& rel, bool enabled) {
    disabled_.Set(rel, !enabled);
    return SaveState();
}

std::string Library::CreateFolder(const std::wstring& parentRel, const std::string& name) {
    const std::wstring clean = SafeFolderName(name);  // same rules as mod import
    if (clean.empty()) return "Enter a name.";
    const fs::path dir = modded_ / parentRel / clean;
    std::error_code ec;
    if (fs::exists(dir, ec)) return "A folder with that name already exists.";
    if (ec) return ErrorText(ec);
    fs::create_directories(dir, ec);
    Refresh();
    return ErrorText(ec);
}

std::string Library::RenameFolder(const std::wstring& rel, const std::string& name) {
    if (rel.empty()) return "Pick a mod or category.";
    const std::wstring clean = SafeFolderName(name);
    if (clean.empty()) return "Enter a name.";
    const fs::path from = modded_ / rel;
    const fs::path to = from.parent_path() / clean;
    if (from.filename() == to.filename()) return {};
    // A change of case only is the same folder on Windows, so it "exists" already.
    std::error_code ec;
    if (DisabledSet::Key(from.filename()) != DisabledSet::Key(to.filename())) {
        if (fs::exists(to, ec)) return "A folder with that name already exists.";
        if (ec) return ErrorText(ec);
    }
    fs::rename(from, to, ec);
    if (ec) return ErrorText(ec);
    disabled_.Rename(from.lexically_relative(modded_), to.lexically_relative(modded_));
    return SaveState();
}

std::string Library::MoveTo(const TextureFile& mod, const std::wstring& relDir) {
    const fs::path target = modded_ / relDir / mod.path.filename();
    std::error_code ec;
    if (fs::equivalent(target.parent_path(), mod.path.parent_path(), ec)) return {};
    if (fs::exists(target, ec)) return "That category already has a file with this name.";
    if (ec) return ErrorText(ec);  // rename would replace a file it couldn't see
    fs::rename(mod.path, target, ec);
    if (ec) return ErrorText(ec);
    disabled_.Rename(mod.path.lexically_relative(modded_), target.lexically_relative(modded_));
    return SaveState();
}

std::string Library::CopyTo(const TextureFile& mod, const std::wstring& relDir) {
    const fs::path target = modded_ / relDir / mod.path.filename();
    std::error_code ec;
    if (fs::exists(target, ec)) return "That category already has a file with this name.";
    if (ec) return ErrorText(ec);
    fs::create_directories(target.parent_path(), ec);
    fs::copy_file(mod.path, target, ec);
    Refresh();
    return ErrorText(ec);
}

std::string Library::Delete(const TextureFile& file) {
    const bool ok = MoveToRecycleBin(file.path);
    if (ok && file.path.parent_path() != original_) disabled_.Set(file.path.lexically_relative(modded_), false);
    const std::string e = SaveState();
    return ok ? e : "Could not move the file to the Recycle Bin.";
}

std::string Library::DeleteFolder(const std::wstring& rel) {
    if (rel.empty()) return "Pick a mod or category.";
    const bool ok = MoveToRecycleBin(modded_ / rel);
    if (ok) disabled_.RemoveUnder(rel);
    const std::string e = SaveState();
    return ok ? e : "Could not move the folder to the Recycle Bin.";
}

std::string Library::Batch(const std::function<void()>& changes) {
    // Cleared even if `changes` throws, or the library would never save or rescan again.
    struct Guard {
        bool& flag;
        ~Guard() { flag = false; }
    };
    batching_ = true;
    {
        Guard guard{batching_};
        changes();
    }
    if (std::exchange(saveHeld_, false)) {
        refreshHeld_ = false;
        return SaveState();  // rescans too
    }
    if (std::exchange(refreshHeld_, false)) Refresh();
    return {};
}

fs::path Library::TargetPath(uint32_t hash, const std::string& label, const std::wstring& relDir) const {
    std::wstring name = std::to_wstring(hash);
    const std::wstring clean = SanitizeName(label);
    if (!clean.empty()) name += L"~" + clean;
    return modded_ / relDir / (name + L".dds");
}

std::string Library::Import(const fs::path& source, uint32_t hash, const std::string& label, bool overwrite,
                            const std::wstring& relDir) {
    const fs::path target = TargetPath(hash, label, relDir);
    std::error_code ec;
    if (fs::equivalent(source, target, ec)) return {};
    // A texture it replaces goes to the Recycle Bin, like every other replace or delete here,
    // so dropping a file of the same name over a mod can be undone.
    if (overwrite) {
        const bool exists = fs::exists(target, ec);
        if (ec) return ErrorText(ec);  // can't tell whether there is a file to keep
        if (exists && !MoveToRecycleBin(target))
            return "Could not move the existing " + WideToUtf8(target.filename().wstring()) +
                   " to the Recycle Bin (in use by the game?).";
    }
    fs::create_directories(target.parent_path(), ec);
    fs::copy_file(source, target, overwrite ? fs::copy_options::overwrite_existing : fs::copy_options::none, ec);
    Refresh();
    return ErrorText(ec);
}

}  // namespace resorep::ui
