#pragma once
#include <cstdint>
#include <filesystem>
#include <string>

#include "labels.h"

namespace resorep {

// A mod package is a plain .zip:
//   <Name>/...                   the mod folder: categories and .dds files
//   reresorep_mod.ini            name=..., game_exe_sha256=... (if the mod is for one game),
//                                plus disabled=<path inside the mod> lines ("." for the mod itself)
//   labels.txt                   labels for hashes in the package (optional)
// Zips made by other tools work too: any .dds files are imported, keeping their folders.

struct ExportOptions {
    bool includeTurnedOff = true;  // also pack textures/categories that are off (and keep them off)
    bool includeLabels = true;
};

struct PackageResult {
    size_t textures = 0;
    size_t folders = 0;
    size_t labels = 0;       // exported, or newly added on import
    size_t skipped = 0;      // existing files kept, or unsafe/non-texture entries ignored
    uint64_t bytes = 0;
    std::string error;       // empty on success
};

// Exports modded/<rel> (a mod or a category) to `zipPath`.
PackageResult ExportModPackage(const std::filesystem::path& moddedRoot, const std::wstring& rel,
                               const std::filesystem::path& zipPath, const ExportOptions& options,
                               const LabelStore& labels);

struct PackageInfo {
    std::string name;  // suggested mod name
    size_t textures = 0;
    uint64_t bytes = 0;
    size_t labels = 0;
    bool fromReResorep = false;  // has our manifest
    std::string game;            // exe SHA-256 of the game it was made for, "" = any
    std::string error;
};
PackageInfo InspectModPackage(const std::filesystem::path& zipPath);

// Extracts into modded/<modName>/. Existing files are replaced only if `overwrite`.
// Labels are added for hashes that have none yet (yours are never replaced).
// A non-empty `gameSha256` marks the imported mod as made for that game (!reresorep_game.ini).
PackageResult ImportModPackage(const std::filesystem::path& zipPath, const std::filesystem::path& moddedRoot,
                               const std::string& modName, bool overwrite, LabelStore& labels,
                               const std::string& gameSha256 = {});

// Folder name safe to create on Windows ("" if nothing usable is left).
std::wstring SafeFolderName(const std::string& name);

}  // namespace resorep
