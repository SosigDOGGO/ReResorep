#pragma once
#include <windows.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include <dxgiformat.h>

#include "mod_state.h"

namespace resorep::ui {

struct TextureFile {
    std::filesystem::path path;
    uint32_t hash = 0;
    std::string label;      // text after "~" in the file name
    std::wstring relDir;    // folder relative to modded/ ("" = loose), '/' separated
    bool selfDisabled = false;  // switched off on its own
    bool enabled = true;        // on, and no folder above it is off
    uint64_t size = 0;
    int64_t modified = 0;  // last write time, file_time ticks
    // From the DDS header (0 / UNKNOWN if it could not be read).
    uint32_t width = 0, height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    // Mods only.
    std::string game;         // exe SHA-256 its mod is marked for, "" = any game
    bool unused = false;      // on, but another texture with the same hash wins in every build that loads it
    bool otherBuild = false;  // marked for a build other than the one these folders belong to
};

// A mod (top-level folder of modded/) or a category inside one (any deeper folder).
struct ModFolder {
    std::wstring rel;  // '/' separated, relative to modded/
    std::string name;
    int depth = 0;     // 1 = mod, 2+ = category
    bool selfDisabled = false;
    bool enabled = true;
    size_t total = 0, active = 0;  // textures below it, and how many of them are on
    std::string game;              // mods only: exe SHA-256 from its !reresorep_game.ini, "" = any game
};

// The modded/ and original/ folders as the UI sees them.
class Library {
public:
    // `game` as for SetGame.
    void SetFolders(const std::filesystem::path& modded, const std::filesystem::path& original,
                    const std::string& game = {});
    ~Library();

    // Rescans once a folder changed on disk and then went quiet (cheap to call every frame).
    bool RefreshIfChanged();
    void Refresh();

    const std::vector<TextureFile>& mods() const { return mods_; }
    const std::vector<TextureFile>& dumps() const { return dumps_; }
    const std::vector<ModFolder>& folders() const { return folders_; }  // depth-first order
    // Files in either folder whose names crash older proxies (IsUnsafeTextureName).
    const std::vector<std::filesystem::path>& unsafeNames() const { return unsafeNames_; }
    // The build these folders belong to (a game's own folders), "" for the shared Default ones.
    // Decides which mods count as active, the same way that game's proxy does.
    void SetGame(const std::string& exeSha256);
    // Per build ("" = a build no mod is marked for): hash -> the file that build's game uses.
    const std::map<std::string, std::unordered_map<uint32_t, std::filesystem::path>>& winners() const {
        return winners_;
    }
    static bool LoadsIn(const TextureFile& mod, const std::string& build) {
        return mod.game.empty() || mod.game == build;
    }
    // The active mod file with exactly this hash (the one that wins among duplicates), or nullptr.
    const std::filesystem::path* ActiveMod(uint32_t hash) const;
    // The mod the proxy uses for a game texture with this hash: exact, or within the original's
    // +-2 tolerance, tried in the same order as the proxy. nullptr if none.
    const std::filesystem::path* ModFor(uint32_t gameHash) const;
    bool IsModded(uint32_t gameHash) const { return ModFor(gameHash) != nullptr; }
    const std::filesystem::path& moddedFolder() const { return modded_; }
    const std::filesystem::path& originalFolder() const { return original_; }

    // All return an error message, empty on success.
    std::string SetEnabled(const TextureFile& mod, bool enabled);
    std::string SetFolderEnabled(const std::wstring& rel, bool enabled);
    std::string CreateFolder(const std::wstring& parentRel, const std::string& name);
    // Renames the mod/category `rel` in place; what was turned off inside it stays off.
    std::string RenameFolder(const std::wstring& rel, const std::string& name);
    std::string MoveTo(const TextureFile& mod, const std::wstring& relDir);
    std::string CopyTo(const TextureFile& mod, const std::wstring& relDir);  // same file name, turned on
    std::string Delete(const TextureFile& file);
    std::string DeleteFolder(const std::wstring& rel);
    std::string Import(const std::filesystem::path& source, uint32_t hash, const std::string& label, bool overwrite,
                       const std::wstring& relDir = {});
    std::filesystem::path TargetPath(uint32_t hash, const std::string& label, const std::wstring& relDir = {}) const;
    // Runs several of the changes above with their saving and rescanning held back until the
    // end, for changes to many files at once. Returns the error from that final save.
    std::string Batch(const std::function<void()>& changes);

private:
    TextureFile MakeFile(const std::filesystem::directory_entry& entry, uint32_t hash);
    void Watch();
    void CloseWatches();
    std::string SaveState();
    void LoadInfoCache();
    void SaveInfoCache();

    std::filesystem::path modded_, original_;
    DisabledSet disabled_;
    std::vector<TextureFile> mods_, dumps_;
    std::vector<ModFolder> folders_;
    std::vector<std::filesystem::path> unsafeNames_;
    std::unordered_map<uint32_t, std::filesystem::path> active_;
    std::string game_;
    std::map<std::string, std::unordered_map<uint32_t, std::filesystem::path>> winners_;
    // Header info per file, reused while its size and timestamp are unchanged. Kept on disk
    // (texture_info.cache) so a launch does not open every texture to read its header.
    struct CachedInfo {
        int64_t modified;
        uint64_t size;
        uint32_t width, height;
        DXGI_FORMAT format;
    };
    std::unordered_map<std::wstring, CachedInfo> infoCache_;
    bool infoCacheLoaded_ = false;
    size_t infoUnsaved_ = 0;  // headers read since the cache file was last written
    HANDLE watches_[2] = {INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE};
    bool changePending_ = false;  // a folder changed since the last rescan
    bool batching_ = false, saveHeld_ = false, refreshHeld_ = false;  // see Batch()
    std::chrono::steady_clock::time_point firstChange_, lastChange_;
};

}  // namespace resorep::ui
