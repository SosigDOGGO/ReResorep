#include "texture_index.h"

#include <cstring>
#include <cwctype>
#include <system_error>

#include "mod_state.h"
#include "resorep_hash.h"

namespace resorep {

std::optional<uint32_t> ParseModFileName(const std::wstring& fileName) {
    const size_t dot = fileName.rfind(L'.');
    if (dot == std::wstring::npos || _wcsicmp(fileName.c_str() + dot, L".dds") != 0) return std::nullopt;

    size_t end = 0;
    while (end < dot && std::iswdigit(fileName[end])) ++end;
    if (end == 0 || end > 10) return std::nullopt;
    if (end != dot && fileName[end] != L'~') return std::nullopt;
    // The original compared against "%u", so a leading zero never matched.
    if (fileName[0] == L'0' && end > 1) return std::nullopt;

    const unsigned long long value = std::stoull(fileName.substr(0, end));
    if (value > 0xFFFFFFFFull) return std::nullopt;
    return uint32_t(value);
}

void TextureIndex::Scan(const std::filesystem::path& folder, const std::string& gameSha256) {
    files_.clear();
    DisabledSet disabled;
    disabled.Load(folder);

    // Sort key of the file currently chosen for each hash.
    std::unordered_map<uint32_t, std::wstring> chosenKey;
    std::error_code ec;
    for (std::filesystem::recursive_directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
        const std::filesystem::path rel = it->path().lexically_relative(folder);
        if (it->is_directory(ec)) {
            if (disabled.IsEffectivelyDisabled(rel)) {
                it.disable_recursion_pending();
            } else if (it.depth() == 0 && !gameSha256.empty()) {
                const std::string modGame = ReadModGame(it->path());
                if (!modGame.empty() && _stricmp(modGame.c_str(), gameSha256.c_str()) != 0) it.disable_recursion_pending();
            }
            continue;
        }
        if (!it->is_regular_file(ec)) continue;
        const auto hash = ParseModFileName(it->path().filename().wstring());
        if (!hash || disabled.IsDisabled(rel)) continue;

        // Several files for one hash: the first by relative path wins (loose files
        // before folders sharing a prefix), matching the original's sorted lookup
        // for files in the root.
        const std::wstring key = DisabledSet::Key(rel);
        auto [pos, inserted] = chosenKey.try_emplace(*hash, key);
        if (inserted || key < pos->second) {
            pos->second = key;
            files_[*hash] = it->path();
        }
    }
}

const std::filesystem::path* TextureIndex::FindExact(uint32_t hash) const {
    auto it = files_.find(hash);
    return it == files_.end() ? nullptr : &it->second;
}

const std::filesystem::path* TextureIndex::Find(uint32_t hash) const {
    if (files_.empty()) return nullptr;
    for (int delta : kHashTolerance)
        if (auto* p = FindExact(hash + uint32_t(delta))) return p;
    return nullptr;
}

}  // namespace resorep
