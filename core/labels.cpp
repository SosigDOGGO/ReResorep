#include "labels.h"

#include <windows.h>

#include <fstream>
#include <iterator>
#include <sstream>

namespace resorep {
namespace {
std::string Trim(std::string s) {
    const size_t b = s.find_first_not_of(" \t\r\n\"");
    if (b == std::string::npos) return {};
    const size_t e = s.find_last_not_of(" \t\r\n\"");
    return s.substr(b, e - b + 1);
}
}  // namespace

std::vector<std::pair<uint32_t, std::string>> LabelStore::Parse(const std::string& text) {
    std::vector<std::pair<uint32_t, std::string>> out;
    std::istringstream in(text);
    std::string line;
    bool first = true;
    while (std::getline(in, line)) {
        if (first && line.compare(0, 3, "\xEF\xBB\xBF") == 0) line.erase(0, 3);
        first = false;
        line = Trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        const size_t sep = line.find_first_of("=,\t");
        if (sep == std::string::npos || sep == 0) continue;
        const std::string number = Trim(line.substr(0, sep));
        if (number.empty() || number.size() > 10 || number.find_first_not_of("0123456789") != std::string::npos) continue;
        const unsigned long long hash = std::stoull(number);
        if (hash > 0xFFFFFFFFull) continue;
        std::string label = Trim(line.substr(sep + 1));
        if (!label.empty()) out.emplace_back(uint32_t(hash), std::move(label));
    }
    return out;
}

bool LabelStore::Load(const std::filesystem::path& file) {
    labels_.clear();
    std::ifstream in(file, std::ios::binary);
    if (!in) return false;
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    Merge(Parse(text), true);
    return true;
}

std::string LabelStore::Serialize() const {
    std::string out = "# ReResorep texture labels: <hash>=<label>\r\n";
    for (const auto& [hash, label] : labels_) out += std::to_string(hash) + "=" + label + "\r\n";
    return out;
}

bool LabelStore::Save(const std::filesystem::path& file) const {
    const std::filesystem::path tmp = file.wstring() + L".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        const std::string text = Serialize();
        out.write(text.data(), std::streamsize(text.size()));
        if (!out) return false;
    }
    return MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

const std::string* LabelStore::Get(uint32_t hash) const {
    auto it = labels_.find(hash);
    return it == labels_.end() ? nullptr : &it->second;
}

void LabelStore::Set(uint32_t hash, std::string label) {
    // Labels are one line in the file.
    for (char& c : label)
        if (c == '\r' || c == '\n') c = ' ';
    label = Trim(label);
    if (label.empty()) labels_.erase(hash);
    else labels_[hash] = std::move(label);
}

size_t LabelStore::Merge(const std::vector<std::pair<uint32_t, std::string>>& entries, bool overwrite) {
    size_t changed = 0;
    for (const auto& [hash, label] : entries) {
        auto it = labels_.find(hash);
        if (it == labels_.end()) {
            labels_.emplace(hash, label);
            ++changed;
        } else if (overwrite && it->second != label) {
            it->second = label;
            ++changed;
        }
    }
    return changed;
}

}  // namespace resorep
