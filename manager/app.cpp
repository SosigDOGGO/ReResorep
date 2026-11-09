#include "app.h"

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_internal.h>  // GImGui->NavActivateId: Shift+click types into the size slider

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iterator>

#include "dds.h"
#include "theme.h"
#include "licenses.h"
#include "version.h"
#include "mod_state.h"
#include "png_export.h"
#include "texture_index.h"
#include "win_util.h"

namespace resorep::ui {
namespace fs = std::filesystem;

namespace {
const ImVec4 kErrorColor(1.0f, 0.45f, 0.4f, 1.0f);
const ImVec4 kOkColor(0.5f, 0.9f, 0.5f, 1.0f);
const ImVec4 kDimColor(0.6f, 0.6f, 0.6f, 1.0f);
constexpr float kDetailsWidth = 340.0f;
constexpr float kSidebarWidth = 260.0f;

ImTextureID ToTexId(ID3D11ShaderResourceView* srv) { return (ImTextureID)(uintptr_t)srv; }

struct SortOption {
    const char* key;
    const char* label;
    bool modsOnly;
};
constexpr SortOption kSortOptions[] = {
    {"hash", "Hash", false},          {"name", "Name", false},
    {"date", "Date modified", false}, {"filesize", "File size", false},
    {"resolution", "Resolution", false}, {"format", "Format", false},
    {"category", "Mod / category", true},
};

std::string FormatDate(int64_t fileTimeTicks) {
    // MSVC's file_clock counts 100 ns ticks since 1601, exactly like FILETIME.
    FILETIME ft{DWORD(uint64_t(fileTimeTicks)), DWORD(uint64_t(fileTimeTicks) >> 32)};
    SYSTEMTIME utc, local;
    if (!fileTimeTicks || !FileTimeToSystemTime(&ft, &utc) || !SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local))
        return "-";
    char buf[32];
    snprintf(buf, sizeof(buf), "%04u-%02u-%02u %02u:%02u", local.wYear, local.wMonth, local.wDay, local.wHour,
             local.wMinute);
    return buf;
}

std::optional<uint32_t> ParseHash(const char* text) {
    if (!*text) return std::nullopt;
    char* end = nullptr;
    const unsigned long long v = strtoull(text, &end, 10);
    if (*end || v > 0xFFFFFFFFull) return std::nullopt;
    return uint32_t(v);
}

std::string Lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

fs::path LabelsFile() { return ExeDirectory() / LabelStore::kFileName; }

const ImU32 kKindColors[] = {0, 0, IM_COL32(120, 150, 255, 255), IM_COL32(200, 200, 200, 255),
                             IM_COL32(90, 210, 255, 255)};
const char* const kKindBadges[] = {"", "", "normal", "depth", "packed"};

// The Formats dropdown's groups; their names are also what reresorep.ini stores.
struct FormatGroup {
    const char* name;
    const char* tooltip;
};
constexpr FormatGroup kFormatGroups[] = {
    {"BC1", "BC1 / DXT1: colour, no or 1-bit alpha"},
    {"BC2", "BC2 / DXT3: colour with sharp alpha"},
    {"BC3", "BC3 / DXT5: colour with smooth alpha"},
    {"BC4", "BC4: one channel (depth, roughness, masks)"},
    {"BC5", "BC5: two channels (usually normal maps)"},
    {"BC6H", "BC6H: HDR colour"},
    {"BC7", "BC7: high quality colour (+ alpha)"},
    {"Uncompressed", "RGBA8, BGRA8, R8, float formats and the like"},
    {"Unknown", "The header could not be read (e.g. encrypted resorep1 files)"},
};

constexpr float kFormatFilterWidth = 150.0f;  // the dropdown on the toolbar

// Index into kFormatGroups.
size_t FormatGroupOf(DXGI_FORMAT f) {
    if (f >= DXGI_FORMAT_BC1_TYPELESS && f <= DXGI_FORMAT_BC1_UNORM_SRGB) return 0;
    if (f >= DXGI_FORMAT_BC2_TYPELESS && f <= DXGI_FORMAT_BC2_UNORM_SRGB) return 1;
    if (f >= DXGI_FORMAT_BC3_TYPELESS && f <= DXGI_FORMAT_BC3_UNORM_SRGB) return 2;
    if (f >= DXGI_FORMAT_BC4_TYPELESS && f <= DXGI_FORMAT_BC4_SNORM) return 3;
    if (f >= DXGI_FORMAT_BC5_TYPELESS && f <= DXGI_FORMAT_BC5_SNORM) return 4;
    if (f >= DXGI_FORMAT_BC6H_TYPELESS && f <= DXGI_FORMAT_BC6H_SF16) return 5;
    if (f >= DXGI_FORMAT_BC7_TYPELESS && f <= DXGI_FORMAT_BC7_UNORM_SRGB) return 6;
    if (f == DXGI_FORMAT_UNKNOWN) return 8;
    return 7;
}

// Fits a w x h image into a box, keeping aspect ratio.
ImVec2 Fit(uint32_t w, uint32_t h, float box) {
    if (!w || !h) return ImVec2(box, box);
    const float scale = box / float(std::max(w, h));
    return ImVec2(std::max(1.0f, w * scale), std::max(1.0f, h * scale));
}

// fs::exists that never throws (it can, e.g. on access denied); false when it can't tell.
bool Exists(const fs::path& path) {
    std::error_code ec;
    return fs::exists(path, ec);
}

// Deferred actions keep their own copies: the library is rescanned before they run.
std::vector<TextureFile> Copies(const std::vector<const TextureFile*>& files) {
    std::vector<TextureFile> copies;
    copies.reserve(files.size());
    for (const TextureFile* f : files) copies.push_back(*f);
    return copies;
}

void HelpMarker(const char* text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}
// Draw-list callback: images after it sample with the sampler in UserCallbackData,
// until an ImDrawCallback_ResetRenderState puts the backend's own back.
void UseSampler(const ImDrawList*, const ImDrawCmd* cmd) {
    auto* state = static_cast<ImGui_ImplDX11_RenderState*>(ImGui::GetPlatformIO().Renderer_RenderState);
    ID3D11SamplerState* sampler = static_cast<ID3D11SamplerState*>(cmd->UserCallbackData);
    state->DeviceContext->PSSetSamplers(0, 1, &sampler);
}
}  // namespace

App::App(HWND hwnd, ID3D11Device* device) : hwnd_(hwnd), device_(device), thumbs_(device) {
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    device->CreateSamplerState(&sd, &pointSampler_);  // on failure the zoom just stays smooth
    settings_.Load();
    ApplyAccentColor(settings_.accentColor);
    labels_.Load(LabelsFile());
    analyzer_ = std::make_unique<TextureAnalyzer>(ExeDirectory() / L"texture_analysis.cache");
    RefreshGames();
    ApplyProfile();
}

const GameState* App::ProfileGame() const {
    if (settings_.profile.empty()) return nullptr;
    for (const GameState& g : games_)
        if (GameKey(g.exe) == GameKey(settings_.profile)) return g.ownFolders && !g.sha256.empty() ? &g : nullptr;
    return nullptr;
}

void App::ApplyProfile() {
    const GameState* game = ProfileGame();
    const auto [modded, original] =
        game ? settings_.FoldersFor(game->exe) : std::pair{settings_.moddedFolder, settings_.originalFolder};
    const std::string build = game ? game->sha256 : std::string();
    if (modded == library_.moddedFolder() && original == library_.originalFolder()) {
        library_.SetGame(build);
        return;
    }
    library_.SetFolders(modded, original, build);
    ClearSelection();
    modView_ = ModView::All;
    QueueAnalysis();
}

void App::DrawProfilePicker() {
    const GameState* current = ProfileGame();
    ImGui::SetNextItemWidth(260);
    if (ImGui::BeginCombo("##profile", current ? GameLabel(*current).c_str() : "Default (shared folders)")) {
        auto pick = [&](const char* label, const fs::path& exe, bool selected) {
            if (ImGui::Selectable(label, selected) && !selected) {
                Defer([this, exe] {
                    settings_.profile = exe;
                    settings_.Save();
                    ApplyProfile();
                });
            }
        };
        pick("Default (shared folders)", {}, !current);
        for (const GameState& g : games_)
            if (g.ownFolders && !g.sha256.empty()) {
                ImGui::PushID(g.exe.c_str());
                pick(GameLabel(g).c_str(), g.exe, current == &g);
                ImGui::PopID();
            }
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("Which folders these tabs show: the Default ones (Settings tab), or a game's own.\n"
                          "Give a game its own folders on the Games tab.\n\nMods:  %s\nDumps: %s",
                          ToUtf8(library_.moddedFolder()).c_str(), ToUtf8(library_.originalFolder()).c_str());
    ImGui::SameLine();
}

void App::SetOwnFolders(const fs::path& exe, bool own) {
    if (own) settings_.ownFolders[GameKey(exe)] = settings_.ExeSha(exe).substr(0, 16);
    else settings_.ownFolders.erase(GameKey(exe));
    if (own) settings_.profile = exe;  // show the new folders right away
    std::string error = settings_.Save() ? "" : "Could not save reresorep.ini";
    for (const GameState& g : games_)
        if (GameKey(g.exe) == GameKey(exe) && HasOurProxy(g.status))
            if (const std::string e = WriteGameSettings(exe, settings_); !e.empty()) error = e;
    RefreshGames();
    ApplyProfile();
    const auto [modded, original] = settings_.FoldersFor(exe);
    Status(error, ToUtf8(exe.parent_path().filename()) + " now uses " + ToUtf8(modded.parent_path()) +
                      (own ? "" : " (Default)") + ". A running game picks this up within a second.");
}

void App::RemoveGame(const fs::path& exe, bool forget, bool recycleFolders) {
    const std::string name = ToUtf8(exe.parent_path().filename());
    const fs::path own = settings_.UsesOwnFolders(exe) && !settings_.SharesOwnFolders(exe)
                             ? settings_.FoldersFor(exe).first.parent_path()
                             : fs::path();
    if (forget) {
        for (const GameState& g : games_) {
            if (GameKey(g.exe) != GameKey(exe) || g.status == ProxyStatus::NotInstalled || g.status == ProxyStatus::Missing)
                continue;
            if (const std::string e = UninstallProxy(exe); !e.empty()) {
                Status(e + " - " + name + " was not removed", "");
                return;
            }
        }
    }
    settings_.RemoveGame(exe, forget);
    std::string error = settings_.Save() ? "" : "Could not save reresorep.ini";
    RefreshGames();
    ApplyProfile();  // back to Default if that game was picked, before its folders go
    if (recycleFolders && !own.empty() && Exists(own) && !MoveToRecycleBin(own))
        error = "Could not move " + ToUtf8(own) + " to the Recycle Bin";
    Status(error, forget ? "Removed " + name + " completely" + (recycleFolders && !own.empty() ? ", its folders are in the Recycle Bin" : "")
                         : "Removed " + name + " from the list. Add it again to get its own folders back.");
}

std::string App::ModGameName(const std::string& sha) const {
    for (const GameState& g : games_)
        if (g.sha256 == sha) return GameLabel(g);
    return "a game not in your list (" + sha.substr(0, 8) + ")";
}

void App::SetModGame(const std::wstring& rel, const GameState* game) {
    const fs::path folder = library_.moddedFolder() / rel;
    const bool ok = WriteModGame(folder, game ? game->sha256 : std::string(), game ? GameLabel(*game) : std::string());
    library_.Refresh();
    Status(ok ? "" : "Could not write " + WideToUtf8(kModGameFile),
           game ? "\"" + WideToUtf8(rel) + "\" is now only for " + GameLabel(*game) : "\"" + WideToUtf8(rel) + "\" is now for any game");
}

void App::ResolveExeChange(const fs::path& exe, bool moveMarks) {
    ExeHash& h = settings_.exeHashes[GameKey(exe)];
    size_t moved = 0;
    if (moveMarks && !h.previous.empty()) {
        std::string label;
        for (const GameState& g : games_)
            if (GameKey(g.exe) == GameKey(exe)) label = GameLabel(g);
        // Every modded folder: the Default one and every game's own.
        std::vector<fs::path> roots{settings_.moddedFolder};
        for (const fs::path& g : settings_.games) roots.push_back(settings_.FoldersFor(g).first);
        std::set<std::wstring> done;
        for (const fs::path& root : roots)
            if (done.insert(GameKey(root)).second) moved += RemarkMods(root, h.previous, h.sha256, label);
    }
    h.previous.clear();
    settings_.Save();
    RefreshGames();
    library_.Refresh();
    Status("", moveMarks ? "Moved " + std::to_string(moved) + " mod(s) to the new exe" : "Marks left as they are");
}

void App::StartHashing() {
    if (hashJob_.valid()) return;  // picked up again when it finishes
    std::vector<fs::path> todo;
    for (const fs::path& exe : settings_.games)
        if (settings_.NeedsHash(exe)) todo.push_back(exe);
    if (todo.empty()) return;
    hashJob_ = std::async(std::launch::async, [todo] {
        std::vector<std::pair<fs::path, ExeHash>> done;
        for (const fs::path& exe : todo) done.emplace_back(exe, HashExe(exe));
        return done;
    });
}

void App::QueueAnalysis() {
    // Kind() queues anything not analysed yet; results are cached on disk.
    for (const auto& f : library_.mods()) KindOf(f);
    for (const auto& f : library_.dumps()) KindOf(f);
}

bool App::IsBlackIce(const TextureFile& file) {
    // Black Ice palette, on a power-of-two sheet of 256+ (weapon skins), which leaves out UI
    // icons and loading screens in the same colours.
    auto pow2 = [](uint32_t v) { return v >= 256 && (v & (v - 1)) == 0; };
    if (!pow2(file.width) || !pow2(file.height)) return false;
    const auto traits = analyzer_->Traits(file.path, file.modified, file.size);
    return traits && traits->icy;
}

const std::string& App::DisplayLabel(const TextureFile& file) const {
    if (const std::string* user = labels_.Get(file.hash)) return *user;
    return file.label;
}

bool App::MatchesFilter(const TextureFile& f) const {
    if (!filter_[0]) return true;
    if (std::to_string(f.hash).find(filter_) != std::string::npos) return true;
    const std::string needle = Lower(filter_);
    if (Lower(f.label).find(needle) != std::string::npos) return true;
    // A mod's textures are found by the mod/category name too.
    if (!f.relDir.empty() && Lower(WideToUtf8(f.relDir)).find(needle) != std::string::npos) return true;
    const std::string* user = labels_.Get(f.hash);
    return user && Lower(*user).find(needle) != std::string::npos;
}

size_t App::CountMatches(const std::vector<TextureFile>& files) const {
    return size_t(std::count_if(files.begin(), files.end(), [this](const TextureFile& f) { return MatchesFilter(f); }));
}

void App::DrawSearch(float y) {
    constexpr float kWidth = 280.0f;
    ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - kWidth, y));
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_F)) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(kWidth);
    const bool edited = ImGui::InputTextWithHint("##search", "Search mods and dumps (name or hash)", filter_,
                                                 sizeof(filter_), ImGuiInputTextFlags_EscapeClearsAll);
    ImGui::SetItemTooltip("Matches hashes, file names, labels and mod/category names.\nCtrl+F to search, Esc to clear.");
    // Typing on another tab jumps to where the results are.
    if (edited && filter_[0] && tab_ != Tab::Mods && tab_ != Tab::Dumps)
        selectTab_ = CountMatches(library_.mods()) || !CountMatches(library_.dumps()) ? Tab::Mods : Tab::Dumps;
}

void App::SetLabel(uint32_t hash, const std::string& label) {
    labels_.Set(hash, label);
    Status(labels_.Save(LabelsFile()) ? "" : "Could not write labels.txt next to ReResorep.exe",
           label.empty() ? "Label removed" : "Label saved");
}

void App::ImportLabels() {
    auto files = PickFiles(hwnd_, L"Import labels", {{L"Label files", L"*.txt;*.ini;*.csv"}, {L"All files", L"*.*"}},
                           false);
    if (files.empty()) return;
    std::ifstream in(files[0], std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const auto entries = LabelStore::Parse(text);
    if (entries.empty()) {
        Status("No labels found in that file (expected lines like 3123456789=Ash helmet)", "");
        return;
    }
    const size_t changed = labels_.Merge(entries, true);
    labelEditHash_.reset();  // the label editor reloads, in case its hash got a new label
    const bool saved = labels_.Save(LabelsFile());
    Status(saved ? "" : "Could not write labels.txt next to ReResorep.exe",
           "Imported " + std::to_string(entries.size()) + " labels (" + std::to_string(changed) + " new or changed)");
}

void App::ExportLabels() {
    auto target = PickSaveFile(hwnd_, L"Export labels", L"reresorep_labels.txt", {{L"Label files", L"*.txt"}});
    if (!target) return;
    std::ofstream out(*target, std::ios::binary | std::ios::trunc);
    const std::string text = labels_.Serialize();
    out.write(text.data(), std::streamsize(text.size()));
    Status(out ? "" : "Could not write " + ToUtf8(*target),
           "Exported " + std::to_string(labels_.size()) + " labels to " + ToUtf8(target->filename()));
}

void App::OpenSimilar(const TextureFile& file, bool isMod, SimilarMode mode) {
    similar_ = SimilarSearch{file, isMod, mode};
    ComputeSimilar();
}

void App::ComputeSimilar() {
    SimilarSearch& s = *similar_;
    const TextureFile& src = s.source;
    const auto srcTraits = analyzer_->Traits(src.path, src.modified, src.size);
    s.results.clear();
    s.notAnalysed = 0;
    s.computed = srcTraits.has_value();
    if (!srcTraits) return;

    // Games dump textures one after another as they load them, and a material's textures
    // load together: in Siege its colour texture, right after it the normal map, then the
    // packed map. So for dumps, the position in that order says the most.
    std::vector<int64_t> dumpTimes;
    dumpTimes.reserve(library_.dumps().size());
    for (const auto& f : library_.dumps()) dumpTimes.push_back(f.modified);
    std::sort(dumpTimes.begin(), dumpTimes.end());
    auto position = [&](int64_t modified) {
        return std::lower_bound(dumpTimes.begin(), dumpTimes.end(), modified) - dumpTimes.begin();
    };
    const ptrdiff_t srcPosition = position(src.modified);

    auto consider = [&](const TextureFile& f, bool isMod) {
        if (f.path == src.path || f.hash == src.hash) return;
        // Material textures share an aspect ratio; usually the exact size.
        if (src.width && f.width && uint64_t(src.width) * f.height != uint64_t(f.width) * src.height) return;
        const auto t = analyzer_->Traits(f.path, f.modified, f.size);
        if (!t || t->kind == TextureKind::Unknown) {
            ++s.notAnalysed;
            return;
        }
        if (s.mode == SimilarMode::Normals && t->kind != TextureKind::Normal) return;
        if (s.mode == SimilarMode::Colors && t->kind != TextureKind::Color) return;

        const float detail = std::max(0.0f, DetailSimilarity(*srcTraits, *t));
        // Siege's normal and packed maps are often a half or a quarter of the colour texture's size.
        const uint32_t wider = std::max(f.width, src.width), narrower = std::min(f.width, src.width);
        const uint32_t ratio = narrower && wider % narrower == 0 ? wider / narrower : 0;
        const bool sizeFits = ratio == 1 || ratio == 2 || ratio == 4;
        const float sizeScore = sizeFits ? 1.0f : 0.6f;
        float score;
        if (!isMod && !s.sourceIsMod) {
            // The next dump scores 1, two away 0.5, three away 0.33...; the wrong side half that.
            const ptrdiff_t d = position(f.modified) - srcPosition;
            float orderScore = 1.0f / float(1 + std::max<ptrdiff_t>(0, std::abs(d) - 1));
            if ((s.mode == SimilarMode::Normals && d < 0) || (s.mode == SimilarMode::Colors && d > 0)) orderScore *= 0.5f;
            score = 0.25f * detail + 0.10f * sizeScore + 0.65f * orderScore;
        } else {
            score = 0.8f * detail + 0.2f * sizeScore;
        }
        if (score > 0.05f) s.results.push_back({f.path, isMod, f.hash, score, t->kind, f.width, f.height, f.modified});
    };
    for (const auto& f : library_.dumps()) consider(f, false);
    for (const auto& f : library_.mods()) consider(f, true);
    std::sort(s.results.begin(), s.results.end(), [](const auto& a, const auto& b) { return a.score > b.score; });
    if (s.results.size() > 50) s.results.resize(50);
}

void App::Status(const std::string& error, const std::string& success) {
    statusIsError_ = !error.empty();
    statusText_ = statusIsError_ ? error : success;
    statusTime_ = std::chrono::steady_clock::now();
}

void App::RefreshGames() {
    bundledSeal_ = CheckSealedFile(BundledProxy());
    games_.clear();
    for (const auto& exe : settings_.games) games_.push_back(InspectGame(exe, settings_));
    StartHashing();
}

void App::ApplySettings() {
    if (!settings_.Save()) {
        Status("Could not save reresorep.ini", "");
        return;
    }
    ApplyProfile();  // the Default folders may have changed
    // Installed proxies watch their dllsettings.ini, so this reaches a running game.
    std::string error;
    for (const GameState& g : games_) {
        if (HasOurProxy(g.status)) {
            const std::string e = WriteGameSettings(g.exe, settings_);
            if (!e.empty()) error = ToUtf8(g.exe.filename()) + ": " + e;
        }
    }
    Status(error, "Settings saved");
}

const TextureFile* App::FindSelected() const {
    if (!selection_) return nullptr;
    const auto& list = selection_->isMod ? library_.mods() : library_.dumps();
    for (const auto& f : list)
        if (f.path == selection_->path) return &f;
    return nullptr;
}

std::vector<const TextureFile*> App::Picked() const {
    std::vector<const TextureFile*> files;
    if (!selection_) return files;
    for (const auto& f : selection_->isMod ? library_.mods() : library_.dumps())
        if (picked_.count(f.path.native())) files.push_back(&f);
    return files;
}

void App::Select(const fs::path& path, bool isMod) {
    selection_ = Selection{path, isMod};
    picked_ = {path.native()};
}

void App::ClearSelection() {
    selection_.reset();
    picked_.clear();
}

void App::PruneSelection() {
    if (!selection_) {
        picked_.clear();
        return;
    }
    const auto& list = selection_->isMod ? library_.mods() : library_.dumps();
    size_t found = 0;
    for (const auto& f : list) found += picked_.count(f.path.native());
    if (found == picked_.size() && picked_.count(selection_->path.native())) return;  // all still there

    // A move or rename puts paths together with '/', so they are matched normalised. The details
    // panel moves on to another selected texture when its own one is gone.
    std::unordered_set<std::wstring> wanted;
    for (const std::wstring& p : picked_) wanted.insert(fs::path(p).lexically_normal().native());
    const std::wstring focus = selection_->path.lexically_normal().native();
    wanted.insert(focus);
    picked_.clear();
    const TextureFile* first = nullptr;
    bool focusFound = false;
    for (const auto& f : list) {
        const std::wstring key = f.path.lexically_normal().native();
        if (!wanted.count(key)) continue;
        picked_.insert(f.path.native());
        if (!first) first = &f;
        if (key == focus) {
            selection_->path = f.path;
            focusFound = true;
        }
    }
    if (focusFound) return;
    if (first) selection_->path = first->path;
    else selection_.reset();
}

void App::OnDropFiles(const std::vector<fs::path>& files) {
    std::vector<fs::path> textures;
    for (const fs::path& f : files) {
        if (_wcsicmp(f.extension().c_str(), L".zip") == 0) BeginImportMod(f);  // one package at a time
        else textures.push_back(f);
    }
    if (!textures.empty()) ImportFiles(textures, ImportDir());
}

void App::BeginImportMod(const fs::path& zip) {
    ImportModDialog d;
    d.zip = zip;
    d.info = InspectModPackage(zip);
    if (!d.info.error.empty()) {
        Status(d.info.error, "");
        return;
    }
    snprintf(d.name, sizeof(d.name), "%s", d.info.name.c_str());
    importModDialog_ = std::move(d);
    openImportModPopup_ = true;
}

void App::DrawPackagePopups() {
    if (openExportPopup_) {
        ImGui::OpenPopup("Export mod");
        openExportPopup_ = false;
    }
    if (exportDialog_ && ImGui::BeginPopupModal("Export mod", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ExportDialog& d = *exportDialog_;
        ImGui::Text("Export \"%s\" (%zu textures) as a .zip package.", d.name.c_str(), d.textures);
        ImGui::Checkbox("Include textures and categories that are turned off", &d.includeTurnedOff);
        ImGui::SetItemTooltip("They stay turned off for whoever imports the package.");
        ImGui::Checkbox("Include my labels for these textures", &d.includeLabels);
        ImGui::Spacing();
        if (ImGui::Button("Export...", ImVec2(120, 0))) {
            const ExportDialog copy = d;
            ImGui::CloseCurrentPopup();
            exportDialog_.reset();
            Defer([this, copy] {
                const std::wstring fileName = SafeFolderName(copy.name) + L".zip";
                auto target = PickSaveFile(hwnd_, L"Export mod", fileName.c_str(), {{L"Mod package (.zip)", L"*.zip"}}, L"zip");
                if (!target) return;
                const PackageResult r = ExportModPackage(library_.moddedFolder(), copy.rel, *target,
                                                         {copy.includeTurnedOff, copy.includeLabels}, labels_);
                Status(r.error, "Exported " + std::to_string(r.textures) + " textures (" + FormatBytes(r.bytes) +
                                    (r.labels ? ", " + std::to_string(r.labels) + " labels" : std::string()) + ") to " +
                                    ToUtf8(target->filename()));
            });
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) {
            ImGui::CloseCurrentPopup();
            exportDialog_.reset();
        }
        ImGui::EndPopup();
    }

    if (openImportModPopup_) {
        ImGui::OpenPopup("Import mod");
        openImportModPopup_ = false;
    }
    if (importModDialog_ && ImGui::BeginPopupModal("Import mod", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImportModDialog& d = *importModDialog_;
        ImGui::Text("%s", ToUtf8(d.zip.filename()).c_str());
        ImGui::TextColored(kDimColor, "%zu textures, %s%s%s", d.info.textures, FormatBytes(d.info.bytes).c_str(),
                           d.info.labels ? (", " + std::to_string(d.info.labels) + " labels").c_str() : "",
                           d.info.fromReResorep ? "" : " (plain zip)");
        ImGui::Spacing();
        ImGui::SetNextItemWidth(320);
        ImGui::InputText("Mod name", d.name, sizeof(d.name));
        const std::wstring folder = SafeFolderName(d.name);
        const bool exists = !folder.empty() && Exists(library_.moddedFolder() / folder);
        if (folder.empty()) {
            ImGui::TextColored(kErrorColor, "Enter a name for the mod.");
        } else if (exists) {
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "A mod named \"%s\" already exists.", WideToUtf8(folder).c_str());
            ImGui::Checkbox("Merge into it (replace files with the same name)", &d.merge);
        }
        if (d.info.labels)
            ImGui::TextColored(kDimColor, "Labels are added only for hashes you have not labelled yourself.");
        ImGui::Spacing();
        ImGui::BeginDisabled(folder.empty() || (exists && !d.merge));
        if (ImGui::Button("Import", ImVec2(120, 0))) {
            const ImportModDialog copy = d;
            ImGui::CloseCurrentPopup();
            importModDialog_.reset();
            Defer([this, copy] {
                // Into a game's own folders: that game's. Into Default: whatever game the package was made
                // for, unless it is merged into a mod that is already marked.
                const GameState* g = ProfileGame();
                const bool marked = !ReadModGame(library_.moddedFolder() / SafeFolderName(copy.name)).empty();
                const PackageResult r = ImportModPackage(copy.zip, library_.moddedFolder(), copy.name, copy.merge,
                                                         labels_, g ? g->sha256 : marked ? "" : copy.info.game);
                if (r.labels) {
                    labels_.Save(LabelsFile());
                    labelEditHash_.reset();  // as for ImportLabels
                }
                library_.Refresh();
                QueueAnalysis();
                std::string msg = "Imported " + std::to_string(r.textures) + " textures into \"" + copy.name + "\"";
                if (r.labels) msg += ", " + std::to_string(r.labels) + " new labels";
                if (r.skipped) msg += " (" + std::to_string(r.skipped) + " entries skipped)";
                Status(r.error, msg);
                if (r.error.empty()) {
                    tab_ = Tab::Mods;
                    modView_ = ModView::Folder;
                    viewFolder_ = SafeFolderName(copy.name);
                }
            });
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) {
            ImGui::CloseCurrentPopup();
            importModDialog_.reset();
        }
        ImGui::EndPopup();
    }
}

void App::ImportFiles(const std::vector<fs::path>& files, const std::wstring& relDir) {
    int imported = 0;
    std::string error;
    for (const fs::path& file : files) {
        const std::wstring name = file.filename().wstring();
        if (auto hash = ParseModFileName(name)) {
            const size_t tilde = name.find(L'~');
            const std::string label =
                tilde == std::wstring::npos ? "" : WideToUtf8(name.substr(tilde + 1, name.rfind(L'.') - tilde - 1));
            const std::string e = library_.Import(file, *hash, label, true, relDir);
            if (e.empty()) ++imported;
            else error = e;
            thumbs_.Invalidate(library_.TargetPath(*hash, label, relDir));
            continue;
        }
        // Not named after a hash: ask. When a dump is selected, assume it replaces that one.
        PendingImport p;
        p.source = file;
        p.relDir = relDir;
        if (const TextureFile* sel = FindSelected(); sel && !selection_->isMod)
            snprintf(p.hash, sizeof(p.hash), "%u", sel->hash);
        snprintf(p.label, sizeof(p.label), "%s", WideToUtf8(file.stem().wstring()).c_str());
        pendingImports_.push_back(p);
        openImportPopup_ = true;
    }
    if (imported || !error.empty()) Status(error, "Imported " + std::to_string(imported) + " texture(s)");
}

void App::ToggleEnabled(const TextureFile& file) {
    Status(library_.SetEnabled(file, file.selfDisabled), file.selfDisabled ? "Turned on" : "Turned off");
}

bool App::InView(const TextureFile& file) const {
    switch (modView_) {
        case ModView::All: return true;
        case ModView::Loose: return file.relDir.empty();
        case ModView::Folder: {
            const std::wstring dir = DisabledSet::Key(file.relDir), view = DisabledSet::Key(viewFolder_);
            return dir == view || dir.starts_with(view + L"/");
        }
    }
    return true;
}

void App::ReplaceDump(uint32_t hash) {
    auto files = PickFiles(hwnd_, L"Choose the replacement texture", {{L"DirectDraw Surface", L"*.dds"}}, false);
    if (files.empty()) return;
    const fs::path target = library_.TargetPath(hash, "");
    const std::string e = library_.Import(files[0], hash, "", true);
    thumbs_.Invalidate(target);
    Status(e, "Texture " + std::to_string(hash) + " now replaced by " + ToUtf8(files[0].filename()));
}

void App::Frame() {
    if (library_.RefreshIfChanged()) {
        PruneSelection();
        QueueAnalysis();
    }
    if (similar_ && !similar_->computed) ComputeSimilar();  // waiting for the source's analysis

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("ReResorep", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);

    const float statusHeight = ImGui::GetFrameHeightWithSpacing();
    const ImVec2 tabsPos = ImGui::GetCursorPos();
    if (ImGui::BeginTabBar("tabs")) {
        const std::optional<Tab> select = std::exchange(selectTab_, std::nullopt);
        auto tab = [&](const char* name, Tab t) {
            if (ImGui::BeginTabItem(name, nullptr, select == t ? ImGuiTabItemFlags_SetSelected : 0)) {
                if (tab_ != t) ClearSelection();
                tab_ = t;
                ImGui::EndTabItem();
            }
        };
        // While searching, the counts are the matches, so both tabs show where the results are.
        auto label = [&](char (&out)[64], const char* name, const char* id, const std::vector<TextureFile>& files) {
            if (filter_[0]) snprintf(out, sizeof(out), "%s (%zu of %zu)###%s", name, CountMatches(files), files.size(), id);
            else snprintf(out, sizeof(out), "%s (%zu)###%s", name, files.size(), id);
        };
        char modsLabel[64], dumpsLabel[64];
        label(modsLabel, "Mods", "mods", library_.mods());
        label(dumpsLabel, "Dumps", "dumps", library_.dumps());
        tab(modsLabel, Tab::Mods);
        tab(dumpsLabel, Tab::Dumps);
        tab("Games", Tab::Games);
        tab("Settings", Tab::Settings);
        tab("About", Tab::About);
        ImGui::EndTabBar();
    }
    const ImVec2 afterTabs = ImGui::GetCursorPos();
    DrawSearch(tabsPos.y);
    ImGui::SetCursorPos(afterTabs);

    ImGui::BeginChild("content", ImVec2(0, -statusHeight));
    switch (tab_) {
        case Tab::Mods:
        case Tab::Dumps: {
            const bool mods = tab_ == Tab::Mods;
            mods ? DrawToolbarMods() : DrawToolbarDumps();
            if (mods) {
                DrawSidebar();
                ImGui::SameLine();
            }
            ImGui::BeginChild("grid", ImVec2(-kDetailsWidth - ImGui::GetStyle().ItemSpacing.x, 0),
                              ImGuiChildFlags_Borders);
            DrawGrid(mods ? library_.mods() : library_.dumps(), mods);
            ImGui::EndChild();
            ImGui::SameLine();
            // Always-on scrollbar: otherwise a tall preview adds a scrollbar, which narrows the
            // panel, which shrinks the preview, which removes the scrollbar... every frame.
            ImGui::BeginChild("details", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_AlwaysVerticalScrollbar);
            DrawDetails();
            ImGui::EndChild();
            break;
        }
        case Tab::Games: DrawGames(); break;
        case Tab::Settings: DrawSettings(); break;
        case Tab::About: DrawAbout(); break;
    }
    ImGui::EndChild();
    DrawStatusBar();
    DrawImportPopup();
    DrawFolderPopups();
    DrawPackagePopups();
    DrawDeleteDumpsPopup();
    DrawLabelPopup();
    DrawTintPopup();
    ImGui::End();
    DrawSimilarWindow();

    if (hashJob_.valid() && hashJob_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        for (auto& [exe, hash] : hashJob_.get()) {
            // Removed while it was being hashed: don't bring a forgotten game back.
            if (std::none_of(settings_.games.begin(), settings_.games.end(),
                             [&](const fs::path& g) { return GameKey(g) == GameKey(exe); }))
                continue;
            ExeHash& stored = settings_.exeHashes[GameKey(exe)];
            // A patched or replaced exe: mods marked for the old hash no longer load until the user
            // moves the marks (Games tab). The oldest unresolved hash is the one mods carry.
            hash.previous = stored.previous;
            if (hash.previous.empty() && !stored.sha256.empty() && !hash.sha256.empty() && stored.sha256 != hash.sha256)
                hash.previous = stored.sha256;
            if (hash.previous == hash.sha256) hash.previous.clear();  // changed back
            if (!hash.previous.empty() && hash.previous != stored.previous)
                Status(ToUtf8(exe.parent_path().filename()) + ": the game's exe changed. Mods marked for the old one "
                                                              "no longer load - see the Games tab.",
                       "");
            stored = std::move(hash);
        }
        settings_.Save();
        // Installed proxies learn their game's hash (and own folders) from dllsettings.ini.
        for (const GameState& g : games_)
            if (HasOurProxy(g.status)) WriteGameSettings(g.exe, settings_);
        RefreshGames();  // hashes anything added meanwhile
        ApplyProfile();
    }
    if (closeJob_.valid() && closeJob_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        const CloseResult r = closeJob_.get();
        const std::string name = ToUtf8(closingExe_.filename());
        std::string msg = r.killed ? name + " did not close within " + std::to_string(kCloseGraceMs / 1000) +
                                         " seconds, so it was ended forcefully."
                          : r.closed ? "Closed " + name + "."
                                     : name + " was not running.";
        Status(r.error, msg);
        lastRunningCheck_ = {};
    }

    if (deleteDumpsJob_.valid() &&
        deleteDumpsJob_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        const size_t failed = deleteDumpsJob_.get();
        const size_t deleted = deleteProgress_ ? deleteProgress_->load() - failed : 0;
        library_.Refresh();
        Status(failed ? std::to_string(failed) + " dump(s) could not be deleted (in use by the game?); " +
                            std::to_string(deleted) + " deleted."
                      : "",
               "Deleted " + std::to_string(deleted) + " dumps. Labels were kept.");
    }

    // Actions that rescan the library run here, after nothing points into it any more.
    auto actions = std::move(deferred_);
    deferred_.clear();
    for (auto& action : actions) action();
    if (!actions.empty()) PruneSelection();  // the textures may have moved or gone

    thumbs_.EndFrame();
}

void App::DrawToolbarMods() {
    DrawProfilePicker();
    if (ImGui::Button("Import .dds...")) {
        ImportFiles(PickFiles(hwnd_, L"Import modded textures", {{L"DirectDraw Surface", L"*.dds"}}, true),
                    ImportDir());
    }
    ImGui::SetItemTooltip("Imports into the mod/category selected on the left (or as loose textures).\n"
                          "Files named <hash>.dds or <hash>~name.dds are imported directly.\n"
                          "You can also drag files onto this window.");
    ImGui::SameLine();
    if (ImGui::Button("Open folder")) OpenWithShell(library_.moddedFolder());
    ImGui::SameLine();
    ImGui::Checkbox("Show turned off", &showDisabled_);
    DrawViewOptions(true);
}

void App::DrawSidebar() {
    ImGui::BeginChild("sidebar", ImVec2(kSidebarWidth, 0), ImGuiChildFlags_Borders);
    size_t loose = 0;
    for (const auto& m : library_.mods()) loose += m.relDir.empty();
    char text[64];
    snprintf(text, sizeof(text), "All textures (%zu)", library_.mods().size());
    if (ImGui::Selectable(text, modView_ == ModView::All)) modView_ = ModView::All;
    snprintf(text, sizeof(text), "Loose textures (%zu)", loose);
    if (ImGui::Selectable(text, modView_ == ModView::Loose)) modView_ = ModView::Loose;
    ImGui::SetItemTooltip("Textures directly in the modded folder, not in any mod.");

    ImGui::SeparatorText("Mods");
    const auto& folders = library_.folders();
    if (folders.empty())
        ImGui::TextWrapped("No mods yet. A mod is a folder; its sub-folders are categories you can turn off.");
    for (size_t i = 0; i < folders.size();) DrawFolderNode(i);

    ImGui::Spacing();
    const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    if (ImGui::Button("+ New mod", ImVec2(half, 0))) {
        newFolderParent_.clear();
        newFolderName_[0] = 0;
        openNewFolderPopup_ = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Import mod...", ImVec2(-1, 0))) {
        auto zips = PickFiles(hwnd_, L"Import a mod package", {{L"Mod packages (.zip)", L"*.zip"}}, false);
        if (!zips.empty()) BeginImportMod(zips[0]);
    }
    ImGui::SetItemTooltip("Import a .zip exported from ReResorep, or any zip of .dds textures.\n"
                          "You can also drop a .zip onto this window.");
    ImGui::EndChild();
}

void App::DrawFolderNode(size_t& index) {
    const auto& folders = library_.folders();
    const ModFolder& f = folders[index];
    const bool hasChildren = index + 1 < folders.size() && folders[index + 1].depth > f.depth;
    ImGui::PushID(f.rel.c_str());

    bool on = !f.selfDisabled;
    if (ImGui::Checkbox("##on", &on)) {
        const std::wstring rel = f.rel;
        Defer([this, rel, on] { Status(library_.SetFolderEnabled(rel, on), on ? "Turned on" : "Turned off"); });
    }
    ImGui::SetItemTooltip(f.depth == 1 ? "Turn this whole mod on or off" : "Turn this category on or off");
    ImGui::SameLine(0, 4);

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick |
                               ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_DefaultOpen;
    if (!hasChildren) flags |= ImGuiTreeNodeFlags_Leaf;
    if (modView_ == ModView::Folder && DisabledSet::Key(viewFolder_) == DisabledSet::Key(f.rel))
        flags |= ImGuiTreeNodeFlags_Selected;
    if (!f.enabled) ImGui::PushStyleColor(ImGuiCol_Text, kDimColor);
    const bool open = ImGui::TreeNodeEx("node", flags, "%s  %zu/%zu", f.name.c_str(), f.active, f.total);
    if (!f.enabled) ImGui::PopStyleColor();
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
        modView_ = ModView::Folder;
        viewFolder_ = f.rel;
    }
    if (!f.enabled && !f.selfDisabled) ImGui::SetItemTooltip("Off because a folder above it is turned off.");
    else if (!f.game.empty()) ImGui::SetItemTooltip("Only for %s", ModGameName(f.game).c_str());

    if (ImGui::BeginPopupContextItem("ctx")) {
        const std::wstring rel = f.rel;
        if (ImGui::MenuItem(f.selfDisabled ? "Turn on" : "Turn off"))
            Defer([this, rel, on = f.selfDisabled] { Status(library_.SetFolderEnabled(rel, on), "Updated"); });
        if (ImGui::MenuItem("Rename...")) {
            renameFolder_ = rel;
            snprintf(renameName_, sizeof(renameName_), "%s", f.name.c_str());
            openRenameFolderPopup_ = true;
        }
        if (ImGui::MenuItem("New category inside...")) {
            newFolderParent_ = rel;
            newFolderName_[0] = 0;
            openNewFolderPopup_ = true;
        }
        if (ImGui::MenuItem("Import .dds here..."))
            Defer([this, rel] {
                ImportFiles(PickFiles(hwnd_, L"Import modded textures", {{L"DirectDraw Surface", L"*.dds"}}, true), rel);
            });
        if (ImGui::MenuItem("Open folder")) OpenWithShell(library_.moddedFolder() / rel);
        if (f.depth == 1 && ImGui::BeginMenu("Made for")) {
            if (ImGui::MenuItem("Any game", nullptr, f.game.empty())) Defer([this, rel] { SetModGame(rel, nullptr); });
            if (!f.game.empty() && std::none_of(games_.begin(), games_.end(), [&](const GameState& g) { return g.sha256 == f.game; }))
                ImGui::MenuItem(ModGameName(f.game).c_str(), nullptr, true, false);
            for (const GameState& g : games_) {
                if (g.sha256.empty()) continue;
                ImGui::PushID(g.exe.c_str());
                if (ImGui::MenuItem(GameLabel(g).c_str(), nullptr, g.sha256 == f.game))
                    Defer([this, rel, exe = g.exe] {
                        for (const GameState& game : games_)
                            if (game.exe == exe) SetModGame(rel, &game);
                    });
                ImGui::PopID();
            }
            ImGui::Separator();
            ImGui::TextColored(kDimColor, "A game skips mods made for another build.");
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem(f.depth == 1 ? "Export mod..." : "Export category as mod...")) {
            exportDialog_ = ExportDialog{rel, f.name, f.total};
            openExportPopup_ = true;
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Move to Recycle Bin...")) {
            deleteFolder_ = rel;
            openDeleteFolderPopup_ = true;
        }
        ImGui::EndPopup();
    }
    ImGui::PopID();

    ++index;
    if (open) {
        while (index < folders.size() && folders[index].depth > f.depth) DrawFolderNode(index);
        ImGui::TreePop();
    } else {
        while (index < folders.size() && folders[index].depth > f.depth) ++index;
    }
}

void App::ExportAsPng(const TextureFile& file) {
    const std::wstring name = file.path.stem().wstring() + L".png";
    auto target = PickSaveFile(hwnd_, L"Export as PNG", name.c_str(), {{L"PNG image", L"*.png"}}, L"png");
    if (!target) return;
    // Upright the way the preview shows it: the global Rotate/Mirror plus this texture's own buttons.
    PreviewTweak tweak;
    if (auto it = previewTweaks_.find(file.hash); it != previewTweaks_.end()) tweak = it->second;
    const int turns = settings_.previewRotation / 90 + tweak.quarterTurns;
    const bool mirror = settings_.previewMirror != tweak.mirror;
    Status(ExportPng(file.path, *target, turns, mirror), "Exported " + ToUtf8(target->filename()));
}

void App::OpenTint(const std::vector<const TextureFile*>& mods) {
    TintDialog d;
    d.settings = lastTint_;
    for (const TextureFile* f : mods) {
        DdsImage header;
        bool encrypted = false;
        const char* why = nullptr;
        if (!ReadTextureInfo(f->path.wstring(), header, &encrypted)) why = "it could not be read";
        else if (encrypted) why = "it is encrypted (resorep1)";
        else if (header.isVolume || header.isCubemap || header.arraySize != 1) why = "it is not a plain 2D texture";
        else why = TintUnsupported(header.format);
        if (why) {
            if (!d.unsupported++) d.unsupportedReason = ToUtf8(f->path.filename()) + ": " + why;
            continue;
        }
        // Tinting these changes lighting or material, not colour: left out unless asked for.
        const TextureKind kind = KindOf(*f);
        (kind == TextureKind::Normal || kind == TextureKind::Packed ? d.maps : d.files).push_back(*f);
    }
    // The preview shows the first texture, small.
    const std::vector<TextureFile>& shown = d.files.empty() ? d.maps : d.files;
    if (!shown.empty()) {
        DdsImage img;
        std::vector<std::vector<float>> mips;
        std::string error;
        if (!LoadTextureMips(shown[0].path.wstring(), 256, img, nullptr, &error))
            d.previewError = error.empty() ? "Could not read the texture." : error;
        else if (std::string e = DecodeMips(img, 1, mips); !e.empty())
            d.previewError = e;
        else {
            d.previewPixels = std::move(mips[0]);
            d.previewW = img.width;
            d.previewH = img.height;
            d.before = UploadPreview(d.previewPixels, d.previewW, d.previewH);
        }
    }
    tintDialog_ = std::move(d);
    openTintPopup_ = true;
}

Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> App::UploadPreview(const std::vector<float>& rgba, uint32_t w,
                                                                    uint32_t h) {
    std::vector<uint8_t> px(size_t(w) * h * 4);
    for (size_t i = 0; i < px.size(); ++i)
        px[i] = (i & 3) == 3 ? 255 : uint8_t(std::lround(std::clamp(rgba[i], 0.0f, 1.0f) * 255));  // shown opaque
    D3D11_TEXTURE2D_DESC desc{w, h, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_IMMUTABLE,
                              D3D11_BIND_SHADER_RESOURCE};
    const D3D11_SUBRESOURCE_DATA data{px.data(), w * 4, 0};
    Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
    if (SUCCEEDED(device_->CreateTexture2D(&desc, &data, &tex))) device_->CreateShaderResourceView(tex.Get(), nullptr, &srv);
    return srv;
}

void App::DrawTintPopup() {
    if (openTintPopup_) {
        ImGui::OpenPopup("Tint textures");
        openTintPopup_ = false;
    }
    if (!tintDialog_ || !ImGui::BeginPopupModal("Tint textures", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    TintDialog& d = *tintDialog_;

    if (tintJob_.valid() && tintJob_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        Defer([this, r = tintJob_.get()] {
            library_.Refresh();
            std::string error = r.error;
            if (r.failed > 1) error = std::to_string(r.failed) + " textures could not be tinted. First: " + error;
            if (r.failed && r.done) error = "Tinted " + std::to_string(r.done) + ", but " + error;
            Status(error, "Tinted " + std::to_string(r.done) + (r.done == 1 ? " texture" : " textures") +
                              ". The originals are in the Recycle Bin.");
        });
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        tintDialog_.reset();
        return;
    }
    if (tintJob_.valid()) {
        const size_t done = tintProgress_ ? tintProgress_->load() : 0;
        ImGui::Text("Tinting %zu of %zu...", std::min(done + 1, tintTotal_), tintTotal_);
        ImGui::ProgressBar(tintTotal_ ? float(done) / float(tintTotal_) : 0.0f, ImVec2(440, 0));
        ImGui::EndPopup();
        return;
    }
    if (!d.previewPixels.empty() && d.previewDirty) {
        std::vector<float> px = d.previewPixels;
        TintPixels(d.settings, px.data(), size_t(d.previewW) * d.previewH);
        d.after = UploadPreview(px, d.previewW, d.previewH);
        d.previewDirty = false;
    }

    // Before and after, turned the way the grid shows textures.
    constexpr float kBox = 216.0f;
    auto preview = [&](const char* caption, ID3D11ShaderResourceView* srv) {
        ImGui::BeginGroup();
        ImGui::TextColored(kDimColor, "%s", caption);
        const ImVec2 at = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(kBox, kBox));
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(at, ImVec2(at.x + kBox, at.y + kBox), IM_COL32(28, 28, 32, 255), 3.0f);
        if (srv) DrawPreview(dl, srv, d.previewW, d.previewH, at, ImVec2(kBox, kBox), IM_COL32_WHITE);
        else if (!d.previewError.empty()) {
            ImGui::SetCursorScreenPos(ImVec2(at.x + 6, at.y + 6));
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + kBox - 12);
            ImGui::TextColored(kErrorColor, "%s", d.previewError.c_str());
            ImGui::PopTextWrapPos();
        }
        ImGui::EndGroup();
    };
    preview("Before", d.before.Get());
    ImGui::SameLine();
    preview("After", d.after.Get());

    TintSettings& s = d.settings;
    bool changed = false;
    ImGui::SetNextItemWidth(kBox);
    changed |= ImGui::ColorPicker3("##color", s.color, ImGuiColorEditFlags_NoSidePreview | ImGuiColorEditFlags_DisplayHex);
    ImGui::SameLine();
    ImGui::BeginGroup();
    const float controls = kBox - ImGui::CalcTextSize("Saturation").x - ImGui::GetStyle().ItemInnerSpacing.x;
    static const char* const kModes[] = {"Recolor", "Colorize", "Multiply"};
    int mode = int(s.mode);
    ImGui::SetNextItemWidth(controls);
    if (ImGui::Combo("Mode", &mode, kModes, IM_ARRAYSIZE(kModes))) {
        s.mode = TintSettings::Mode(mode);
        changed = true;
    }
    ImGui::SetItemTooltip("Recolor: takes the colour's hue; whites, blacks and shading stay as they are.\n"
                          "    Best for Black Ice and other one-colour skins.\n"
                          "Colorize: the colour's hue and saturation on every pixel, keeping only its lightness.\n"
                          "    Also colours grey and white parts.\n"
                          "Multiply: the texture times the colour, like tinted glass. Darkens.");
    auto percent = [&](const char* label, float& value, float max) {
        float v = value * 100.0f;
        ImGui::SetNextItemWidth(controls);
        if (ImGui::SliderFloat(label, &v, 0.0f, max, "%.0f%%", ImGuiSliderFlags_AlwaysClamp)) {
            value = v / 100.0f;
            changed = true;
        }
    };
    percent("Strength", s.strength, 100.0f);
    percent("Saturation", s.saturation, 200.0f);
    percent("Brightness", s.brightness, 200.0f);
    if (ImGui::Button("Reset sliders")) {
        s.strength = s.saturation = s.brightness = 1.0f;
        changed = true;
    }
    ImGui::EndGroup();
    d.previewDirty |= changed;

    ImGui::Spacing();
    const size_t count = d.files.size() + (d.includeMaps ? d.maps.size() : 0);
    ImGui::Text("%zu %s will be tinted.", count, count == 1 ? "texture" : "textures");
    if (!d.maps.empty()) {
        ImGui::Checkbox(("Also tint the " + std::to_string(d.maps.size()) + " normal / packed map(s)").c_str(),
                        &d.includeMaps);
        ImGui::SetItemTooltip("Those hold lighting and material data, not colour: tinting them usually breaks how "
                              "the surface looks. Leave this off unless you know you want it.");
    }
    if (d.unsupported)
        ImGui::TextColored(kDimColor, "%zu skipped (%s%s)", d.unsupported, d.unsupportedReason.c_str(),
                           d.unsupported > 1 ? ", ..." : "");
    ImGui::PushTextWrapPos(kBox * 2 + ImGui::GetStyle().ItemSpacing.x);
    ImGui::TextColored(kDimColor, "Each texture keeps its format and mips; BC formats are compressed again, which can "
                                  "soften fine detail a little. The originals go to the Recycle Bin.");
    ImGui::PopTextWrapPos();
    ImGui::Spacing();

    bool close = false;
    ImGui::BeginDisabled(count == 0);
    if (ImGui::Button("Tint", ImVec2(120, 0))) StartTint();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) close = true;
    lastTint_ = s;
    if (close) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
    if (close) tintDialog_.reset();
}

void App::StartTint() {
    const TintDialog& d = *tintDialog_;
    std::vector<TextureFile> files = d.files;
    if (d.includeMaps) files.insert(files.end(), d.maps.begin(), d.maps.end());
    tintTotal_ = files.size();
    tintProgress_ = std::make_shared<std::atomic<size_t>>(0);
    tintJob_ = std::async(std::launch::async, [files = std::move(files), s = d.settings, progress = tintProgress_] {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);  // the Recycle Bin is the shell's
        TintResult r;
        for (const TextureFile& f : files) {
            fs::path tinted = f.path;
            tinted += L".tinting";
            std::string e = TintTexture(f.path, tinted, s);
            std::error_code ec;
            if (e.empty() && !MoveToRecycleBin(f.path)) {
                e = ToUtf8(f.path.filename()) + ": could not be replaced (in use by the game?).";
            } else if (e.empty()) {
                // The original is in the Recycle Bin now, so a tint can be undone from there.
                fs::rename(tinted, f.path, ec);
                if (ec)
                    e = ToUtf8(f.path.filename()) + ": the tinted copy could not be put in place; it was left as " +
                        ToUtf8(tinted.filename()) + " and the original is in the Recycle Bin.";
            }
            if (!e.empty()) {
                if (!r.failed++) r.error = e;
                if (Exists(f.path)) fs::remove(tinted, ec);  // nothing was replaced
            } else {
                ++r.done;
            }
            ++*progress;
        }
        if (SUCCEEDED(com)) CoUninitialize();
        return r;
    });
}

void App::DrawMoveMenu(const std::vector<const TextureFile*>& files, bool copy) {
    if (!ImGui::BeginMenu(copy ? "Copy to" : "Move to")) return;
    DrawMoveItems(files, copy);
    ImGui::EndMenu();
}

void App::DrawMoveItems(const std::vector<const TextureFile*>& files, bool copy) {
    auto item = [&](const char* label, const std::wstring& rel, int depth) {
        ImGui::Indent(float(depth) * 10.0f);
        // Ticked and greyed where the textures already are (copying: any file of the same name).
        const bool there = std::all_of(files.begin(), files.end(), [&](const TextureFile* f) {
            return copy ? Exists(library_.moddedFolder() / rel / f->path.filename())
                        : DisabledSet::Key(rel) == DisabledSet::Key(f->relDir);
        });
        if (ImGui::MenuItem(label, nullptr, there, !there))
            Defer([this, files = Copies(files), rel, copy] { MoveOrCopy(files, rel, copy); });
        ImGui::Unindent(float(depth) * 10.0f);
    };
    item("(loose, no mod)", L"", 0);
    for (const ModFolder& f : library_.folders()) {
        ImGui::PushID(f.rel.c_str());
        item(f.name.c_str(), f.rel, f.depth - 1);
        ImGui::PopID();
    }
}

void App::MoveOrCopy(const std::vector<TextureFile>& files, const std::wstring& rel, bool copy) {
    const std::string where = rel.empty() ? "loose textures" : "\"" + WideToUtf8(rel) + "\"";
    std::string error;
    size_t done = 0, failed = 0;
    const std::string saveError = library_.Batch([&] {
        for (const TextureFile& f : files) {
            if (!copy && DisabledSet::Key(rel) == DisabledSet::Key(f.relDir)) continue;  // already there
            const std::string e = copy ? library_.CopyTo(f, rel) : library_.MoveTo(f, rel);
            if (!e.empty()) {
                error = e;
                ++failed;
                continue;
            }
            ++done;
            if (copy) continue;
            // Moved ones stay selected (PruneSelection matches the new paths up after the rescan).
            const fs::path target = library_.moddedFolder() / rel / f.path.filename();
            if (picked_.erase(f.path.native())) picked_.insert(target.native());
            if (selection_ && selection_->path == f.path) selection_->path = target;
        }
    });
    if (failed && files.size() > 1)
        error = std::to_string(failed) + " of " + std::to_string(files.size()) + " could not be " +
                (copy ? "copied" : "moved") + ": " + error;
    if (error.empty()) error = saveError;
    const std::string what = files.size() == 1 ? "" : std::to_string(done) + " textures ";
    Status(error, (copy ? "Copied " : "Moved ") + what + "to " + where);
}

void App::SetEnabled(const std::vector<TextureFile>& mods, bool on) {
    std::string error;
    size_t changed = 0;
    const std::string saveError = library_.Batch([&] {
        for (const TextureFile& m : mods) {
            if (m.selfDisabled != on) continue;  // already that way
            const std::string e = library_.SetEnabled(m, on);
            if (e.empty()) ++changed;
            else error = e;
        }
    });
    Status(error.empty() ? saveError : error,
           (on ? "Turned on " : "Turned off ") + std::to_string(changed) + " texture(s)");
}

void App::Recycle(const std::vector<TextureFile>& files) {
    std::string error;
    size_t failed = 0;
    const std::string saveError = library_.Batch([&] {
        for (const TextureFile& f : files)
            if (const std::string e = library_.Delete(f); !e.empty()) {
                error = e;
                ++failed;
            }
    });
    if (failed && files.size() > 1)
        error = std::to_string(failed) + " of " + std::to_string(files.size()) +
                " textures could not be moved to the Recycle Bin (in use by the game?).";
    Status(error.empty() ? saveError : error,
           files.size() == 1 ? "Moved to Recycle Bin"
                             : "Moved " + std::to_string(files.size()) + " textures to the Recycle Bin");
}

void App::AddDumpsToMod(const std::vector<TextureFile>& dumps, const std::wstring& rel) {
    const std::string where = rel.empty() ? "loose textures" : "\"" + WideToUtf8(rel) + "\"";
    if (dumps.size() == 1 && Exists(library_.TargetPath(dumps[0].hash, "", rel))) {
        Status(where + " already has a texture for " + std::to_string(dumps[0].hash), "");
        return;
    }
    std::string error;
    size_t added = 0, already = 0;
    library_.Batch([&] {
        for (const TextureFile& d : dumps) {
            if (Exists(library_.TargetPath(d.hash, "", rel))) {
                ++already;
                continue;
            }
            const std::string e = library_.Import(d.path, d.hash, "", false, rel);
            if (e.empty()) ++added;
            else error = e;
        }
    });
    if (dumps.size() == 1) {
        Status(error, "Added " + std::to_string(dumps[0].hash) + " to " + where +
                          ". It looks the same in-game until you edit that copy.");
        return;
    }
    Status(error, "Added " + std::to_string(added) + " textures to " + where +
                      (already ? " (" + std::to_string(already) + " were already there)" : "") +
                      ". They look the same in-game until you edit the copies.");
}

void App::DrawAddToModMenu(const std::vector<const TextureFile*>& dumps) {
    if (!ImGui::BeginMenu("Add to mod")) return;
    DrawAddToModItems(dumps);
    ImGui::EndMenu();
}

void App::DrawAddToModItems(const std::vector<const TextureFile*>& dumps) {
    // A copy of each dump goes into the mod as <hash>.dds, ready to be edited.
    auto item = [&](const char* label, const std::wstring& rel, int depth) {
        ImGui::Indent(float(depth) * 10.0f);
        const bool already = std::all_of(dumps.begin(), dumps.end(), [&](const TextureFile* d) {
            return Exists(library_.TargetPath(d->hash, "", rel));
        });
        if (ImGui::MenuItem(label, nullptr, already, !already))
            Defer([this, dumps = Copies(dumps), rel] { AddDumpsToMod(dumps, rel); });
        ImGui::Unindent(float(depth) * 10.0f);
    };
    for (const ModFolder& f : library_.folders()) {
        ImGui::PushID(f.rel.c_str());
        item(f.name.c_str(), f.rel, f.depth - 1);
        ImGui::PopID();
    }
    if (!library_.folders().empty()) ImGui::Separator();
    item("(loose, no mod)", L"", 0);
    if (ImGui::MenuItem("New mod...")) {
        addAfterCreate_ = Copies(dumps);
        newFolderParent_.clear();
        newFolderName_[0] = 0;
        openNewFolderPopup_ = true;
    }
}

void App::DrawFolderPopups() {
    if (openNewFolderPopup_) {
        ImGui::OpenPopup("New folder");
        openNewFolderPopup_ = false;
    }
    if (ImGui::BeginPopupModal("New folder", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (newFolderParent_.empty())
            ImGui::TextUnformatted(addAfterCreate_.size() > 1   ? "Name of the new mod (the textures are added to it):"
                                   : !addAfterCreate_.empty() ? "Name of the new mod (the texture is added to it):"
                                                              : "Name of the new mod:");
        else ImGui::Text("New category inside %s:", WideToUtf8(newFolderParent_).c_str());
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(300);
        const bool enter =
            ImGui::InputText("##name", newFolderName_, sizeof(newFolderName_), ImGuiInputTextFlags_EnterReturnsTrue);
        if (ImGui::Button("Create", ImVec2(120, 0)) || enter) {
            const std::wstring parent = newFolderParent_;
            const std::string name = newFolderName_;
            std::vector<TextureFile> add = std::exchange(addAfterCreate_, {});
            Defer([this, parent, name, add] {
                const std::string e = library_.CreateFolder(parent, name);
                // Same cleanup of the name as the folder on disk got.
                const std::wstring folder = SafeFolderName(name);
                const std::wstring rel = parent.empty() ? folder : parent + L"/" + folder;
                Status(e, "Created " + name);
                if (!e.empty()) return;
                // A new mod in a game's own folders is marked as that game's.
                if (const GameState* g = parent.empty() ? ProfileGame() : nullptr) {
                    WriteModGame(library_.moddedFolder() / rel, g->sha256, GameLabel(*g));
                    library_.Refresh();
                }
                modView_ = ModView::Folder;
                viewFolder_ = rel;
                if (!add.empty()) AddDumpsToMod(add, rel);
            });
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) {
            addAfterCreate_.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (openRenameFolderPopup_) {
        ImGui::OpenPopup("Rename folder");
        openRenameFolderPopup_ = false;
    }
    if (ImGui::BeginPopupModal("Rename folder", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        const bool isMod = renameFolder_.find(L'/') == std::wstring::npos;
        ImGui::Text("New name for the %s \"%s\":", isMod ? "mod" : "category", WideToUtf8(renameFolder_).c_str());
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(300);
        const bool enter = ImGui::InputText("##name", renameName_, sizeof(renameName_),
                                            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
        if (ImGui::Button("Rename", ImVec2(120, 0)) || enter) {
            const std::wstring from = renameFolder_;
            const std::string name = renameName_;
            Defer([this, from, name] {
                const std::string e = library_.RenameFolder(from, name);
                Status(e, "Renamed to " + name);
                if (!e.empty()) return;
                // Keep the sidebar and the selected texture pointing at the same things.
                const size_t slash = from.rfind(L'/');
                const std::wstring to =
                    (slash == std::wstring::npos ? L"" : from.substr(0, slash + 1)) + SafeFolderName(name);
                const std::wstring fromKey = DisabledSet::Key(from);
                const std::wstring viewKey = DisabledSet::Key(viewFolder_);
                if (viewKey == fromKey || viewKey.starts_with(fromKey + L"/"))
                    viewFolder_ = to + viewFolder_.substr(from.size());
                auto renamed = [&](fs::path& path) {
                    const fs::path rel = path.lexically_relative(library_.moddedFolder());
                    const std::wstring relSlashed = DisabledSet::Slashed(rel);
                    if (DisabledSet::Key(rel).starts_with(fromKey + L"/"))
                        path = library_.moddedFolder() / (to + relSlashed.substr(from.size()));
                };
                if (selection_ && selection_->isMod) {
                    renamed(selection_->path);
                    std::unordered_set<std::wstring> picked;
                    for (const std::wstring& p : picked_) {
                        fs::path path = p;
                        renamed(path);
                        picked.insert(path.native());
                    }
                    picked_ = std::move(picked);
                }
            });
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (openDeleteFolderPopup_) {
        ImGui::OpenPopup("Delete folder");
        openDeleteFolderPopup_ = false;
    }
    if (ImGui::BeginPopupModal("Delete folder", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        size_t count = 0;
        for (const ModFolder& f : library_.folders())
            if (DisabledSet::Key(f.rel) == DisabledSet::Key(deleteFolder_)) count = f.total;
        ImGui::Text("Move \"%s\" and its %zu texture(s) to the Recycle Bin?", WideToUtf8(deleteFolder_).c_str(), count);
        if (ImGui::Button("Move to Recycle Bin", ImVec2(180, 0))) {
            const std::wstring rel = deleteFolder_;
            Defer([this, rel] {
                Status(library_.DeleteFolder(rel), "Moved to Recycle Bin");
                if (modView_ == ModView::Folder) modView_ = ModView::All;
            });
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

void App::DrawToolbarDumps() {
    DrawProfilePicker();
    if (ImGui::Button("Open folder")) OpenWithShell(library_.originalFolder());
    ImGui::SameLine();
    ImGui::Checkbox("Hide already modded", &hideModdedDumps_);
    if (!settings_.Dumping()) {
        // Short, so the first row keeps room for Rotate / Mirror / Size on the right.
        ImGui::SameLine();
        ImGui::TextColored(kDimColor, "Dumping off (?)");
        ImGui::SetItemTooltip(settings_.lowImpact
                                  ? "Dumping is off because Low impact mode is on (Settings).\n"
                                    "Turn Low impact mode off to dump textures the game loads."
                                  : "Dumping is off. Turn on Mod creator mode and \"Dump textures the game loads\"\n"
                                    "in Settings to save the textures the game loads here.");
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!settings_.Dumping());
    if (ImGui::Button("Redump")) Redump();
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Makes a running game dump every texture again, overwriting the files here, as it\n"
                          "loads it. Textures the game already has loaded come in on the next map or menu.\n\n"
                          "Until then, a texture the game has dumped is not dumped again, even if you\n"
                          "delete its file.");
    ImGui::SameLine();
    const bool deleting = deleteDumpsJob_.valid();
    ImGui::BeginDisabled(deleting || library_.dumps().empty());
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.18f, 0.18f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.70f, 0.24f, 0.24f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.45f, 0.14f, 0.14f, 1.0f));
    char deleteLabel[64] = "Delete all dumps...";
    if (deleting && deleteProgress_)
        snprintf(deleteLabel, sizeof(deleteLabel), "Deleting %zu / %zu...###deleteall", deleteProgress_->load(),
                 deleteTotal_);
    if (ImGui::Button(deleteLabel)) openDeleteDumpsPopup_ = true;
    ImGui::PopStyleColor(3);
    ImGui::EndDisabled();
    DrawViewOptions(false);
}

void App::Redump() {
    std::string sent, error;
    bool outdated = false, modified = false;
    for (const GameState& g : games_) {
        // Only games that dump into the folder these tabs show.
        if (settings_.FoldersFor(g.exe).second != library_.originalFolder() || FindProcesses(g.exe).empty()) continue;
        if (g.status == ProxyStatus::Outdated) outdated = true;  // an older proxy ignores the request
        if (g.status == ProxyStatus::Modified) modified = true;  // does nothing at all
        if (g.status != ProxyStatus::Installed) continue;
        const std::string e = RequestRedump(g.exe);
        if (!e.empty()) error = ToUtf8(g.exe.filename()) + ": " + e;
        else sent += (sent.empty() ? "" : ", ") + ToUtf8(g.exe.filename());
    }
    if (error.empty() && sent.empty())
        error = modified   ? "The running game's proxy is damaged or modified - repair it on the Games tab, then restart the game."
                : outdated ? "The running game has an older proxy - update it on the Games tab, then restart the game."
                           : "Start the game first (with the proxy installed, using the folders shown here).";
    Status(error, "Redump requested for " + sent + ": textures are dumped again as the game loads them");
}

void App::DrawDeleteDumpsPopup() {
    if (openDeleteDumpsPopup_) {
        ImGui::OpenPopup("Delete all dumps");
        openDeleteDumpsPopup_ = false;
    }
    if (!ImGui::BeginPopupModal("Delete all dumps", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;

    uint64_t bytes = 0;
    for (const auto& d : library_.dumps()) bytes += d.size;
    ImGui::Text("Permanently delete all %zu dumped textures (%s)?", library_.dumps().size(), FormatBytes(bytes).c_str());
    ImGui::TextColored(kDimColor, "%s", ToUtf8(library_.originalFolder()).c_str());
    ImGui::Spacing();
    ImGui::TextColored(kErrorColor, "They are not moved to the Recycle Bin. This cannot be undone.");
    ImGui::BulletText("Your labels are kept (labels.txt). They show again when a texture is dumped again.");
    ImGui::BulletText("Your mods are not touched.");
    ImGui::BulletText("Other files in that folder are left alone.");
    if (settings_.Dumping())
        ImGui::BulletText("A running game won't dump these again until you press Redump.");
    ImGui::Spacing();

    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.18f, 0.18f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.70f, 0.24f, 0.24f, 1.0f));
    if (ImGui::Button("Delete permanently", ImVec2(200, 0))) {
        std::vector<fs::path> paths;
        for (const auto& d : library_.dumps()) paths.push_back(d.path);
        deleteTotal_ = paths.size();
        deleteProgress_ = std::make_shared<std::atomic<size_t>>(0);
        // Off the UI thread so thousands of files don't freeze the window; the button shows progress.
        deleteDumpsJob_ = std::async(std::launch::async, [paths = std::move(paths), progress = deleteProgress_] {
            size_t failed = 0;
            for (const fs::path& p : paths) {
                std::error_code ec;
                if (!fs::remove(p, ec) && fs::exists(p, ec)) ++failed;  // already gone counts as done
                ++*progress;
            }
            return failed;
        });
        if (selection_ && !selection_->isMod) ClearSelection();
        ImGui::CloseCurrentPopup();
    }
    ImGui::PopStyleColor(2);
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void App::SortVisible(std::vector<const TextureFile*>& files, bool isMod) const {
    const SortOrder& order = settings_.SortFor(isMod);
    const std::string& key = order.key;
    // Returns <0, 0, >0 like strcmp, before applying the direction.
    auto compare = [&](const TextureFile& a, const TextureFile& b) -> int {
        auto cmp = [](auto x, auto y) { return x < y ? -1 : (y < x ? 1 : 0); };
        if (key == "name") {
            const int c = cmp(Lower(DisplayLabel(a)), Lower(DisplayLabel(b)));
            return c ? c : cmp(a.path.filename().wstring(), b.path.filename().wstring());
        }
        if (key == "date") return cmp(a.modified, b.modified);
        if (key == "filesize") return cmp(a.size, b.size);
        if (key == "resolution") {
            const int c = cmp(uint64_t(a.width) * a.height, uint64_t(b.width) * b.height);
            return c ? c : cmp(a.width, b.width);
        }
        if (key == "format") return cmp(std::string(FormatName(a.format)), std::string(FormatName(b.format)));
        if (key == "category" && isMod) return cmp(DisabledSet::Key(a.relDir), DisabledSet::Key(b.relDir));
        return 0;  // "hash": the tie-break below
    };
    const bool desc = order.descending;
    std::stable_sort(files.begin(), files.end(), [&](const TextureFile* a, const TextureFile* b) {
        int c = compare(*a, *b);
        if (!c) c = a->hash < b->hash ? -1 : (a->hash > b->hash ? 1 : 0);
        if (!c) return a->path < b->path;
        return desc ? c > 0 : c < 0;
    });
}

ImVec2 App::PreviewSize(uint32_t w, uint32_t h, float box, int extraTurns) const {
    // A quarter turn swaps the on-screen aspect ratio.
    const int k = (settings_.previewRotation / 90 + extraTurns) & 3;
    return k % 2 ? Fit(h, w, box) : Fit(w, h, box);
}

void App::DrawPreview(ImDrawList* dl, ID3D11ShaderResourceView* srv, uint32_t w, uint32_t h, ImVec2 boxMin,
                      ImVec2 boxSize, ImU32 tint, int extraTurns, bool extraMirror) const {
    const ImVec2 fit = PreviewSize(w, h, std::min(boxSize.x, boxSize.y), extraTurns);
    const ImVec2 a(boxMin.x + (boxSize.x - fit.x) * 0.5f, boxMin.y + (boxSize.y - fit.y) * 0.5f);
    const ImVec2 b(a.x + fit.x, a.y + fit.y);
    // Texture corners in clockwise order starting top-left; rotating the picture
    // clockwise by k quarter turns shows corner (i - k) at screen corner i.
    static const ImVec2 kCorners[4] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    const int k = (settings_.previewRotation / 90 + extraTurns) & 3;
    ImVec2 uv[4];
    for (int i = 0; i < 4; ++i) uv[i] = kCorners[(i - k + 4) & 3];
    if (settings_.previewMirror != extraMirror) {
        std::swap(uv[0], uv[1]);
        std::swap(uv[2], uv[3]);
    }
    dl->AddImageQuad(ToTexId(srv), a, ImVec2(b.x, a.y), b, ImVec2(a.x, b.y), uv[0], uv[1], uv[2], uv[3], tint);
}

void App::DrawViewOptions(bool isMod) {
    // Called right after the tab's first toolbar row. How previews are shown (rotation, mirror,
    // size) goes at the right end of that row when it fits; the second row holds sorting and
    // filters. Groups that don't fit start a new line, so a narrow window never pushes any out
    // of sight.
    SortOrder& order = settings_.SortFor(isMod);
    const SortOption* current = &kSortOptions[0];
    for (const SortOption& o : kSortOptions)
        if (order.key == o.key && (isMod || !o.modsOnly)) current = &o;
    bool changed = false;

    const ImGuiStyle& style = ImGui::GetStyle();
    const float rowRight = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
    auto fits = [&](float width, float gap) { return ImGui::GetItemRectMax().x + gap + width <= rowRight; };
    auto flow = [&](float width, float gap) {
        if (fits(width, gap)) ImGui::SameLine(0, gap);
    };
    auto checkboxWidth = [&](const char* label) {
        return ImGui::GetFrameHeight() + style.ItemInnerSpacing.x + ImGui::CalcTextSize(label, nullptr, true).x;
    };

    constexpr float kRotateWidth = 80.0f, kSizeWidth = 120.0f, kGroupGap = 20.0f;
    const float displayWidth = ImGui::CalcTextSize("Rotate").x + style.ItemSpacing.x + kRotateWidth +
                               style.ItemSpacing.x + checkboxWidth("Mirror") + kGroupGap + kSizeWidth;
    // On the current line against the right edge if there is room, else at the start of a new line.
    auto placeDisplay = [&] {
        if (!fits(displayWidth, kGroupGap)) return;
        ImGui::SameLine();
        ImGui::SetCursorScreenPos(ImVec2(rowRight - displayWidth, ImGui::GetCursorScreenPos().y));
    };
    auto drawDisplay = [&] {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Rotate");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(kRotateWidth);
        static const char* kRotations[] = {"0\xC2\xB0", "90\xC2\xB0", "180\xC2\xB0", "270\xC2\xB0"};
        int rotation = settings_.previewRotation / 90;
        if (ImGui::Combo("##rotate", &rotation, kRotations, 4)) {
            settings_.previewRotation = rotation * 90;
            changed = true;
        }
        ImGui::SetItemTooltip("Turns previews clockwise. Only changes how they are shown, not the files.");
        ImGui::SameLine();
        changed |= ImGui::Checkbox("Mirror", &settings_.previewMirror);
        ImGui::SetItemTooltip("Flips previews left-to-right (after rotating).");

        ImGui::SameLine(0, kGroupGap);
        ImGui::SetNextItemWidth(kSizeWidth);
        // Ctrl+click (Dear ImGui's own) or Shift+click (Settings) turns the slider into a box to type a size in.
        ImGuiSliderFlags sizeFlags = ImGuiSliderFlags_AlwaysClamp;
        if (settings_.shiftClickToType) {
            const ImGuiID id = ImGui::GetID("##size");
            const ImGuiIO& io = ImGui::GetIO();
            const ImVec2 min = ImGui::GetCursorScreenPos();
            if (io.KeyShift && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ImGui::IsWindowHovered() &&
                ImGui::IsMouseHoveringRect(min, ImVec2(min.x + kSizeWidth, min.y + ImGui::GetFrameHeight()))) {
                GImGui->NavActivateId = id;
                GImGui->NavActivateFlags = ImGuiActivateFlags_PreferInput;
            } else if (io.KeyCtrl && !ImGui::TempInputIsActive(id)) {
                sizeFlags |= ImGuiSliderFlags_NoInput;  // Ctrl+click just drags; Ctrl+A etc. still work once typing
            }
        }
        ImGui::SliderFloat("##size", &thumbSize_, 64.0f, 256.0f, "Size %.0f", sizeFlags);
        ImGui::SetItemTooltip(settings_.shiftClickToType ? "Shift+click to type a size (64-256)."
                                                         : "Ctrl+click to type a size (64-256).");
    };

    // End of the first row.
    const bool displayOnTop = fits(displayWidth, kGroupGap);
    if (displayOnTop) {
        placeDisplay();
        drawDisplay();
    }

    // Second row: sorting, then filters.
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Sort by");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(150);
    if (ImGui::BeginCombo("##sort", current->label)) {
        for (const SortOption& o : kSortOptions) {
            if (o.modsOnly && !isMod) continue;
            if (ImGui::Selectable(o.label, &o == current)) {
                order.key = o.key;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine(0, 4);
    if (ImGui::ArrowButton("##dir", order.descending ? ImGuiDir_Down : ImGuiDir_Up)) {
        order.descending = !order.descending;
        changed = true;
    }
    ImGui::SetItemTooltip(order.descending ? "Descending (largest / newest first)"
                                           : "Ascending (smallest / oldest first)");

    TextureFilters& flt = settings_.FiltersFor(isMod);
    flow(kFormatFilterWidth, kGroupGap);
    changed |= DrawFormatFilter(flt, isMod ? library_.mods() : library_.dumps());
    flow(checkboxWidth("Hide normal maps"), style.ItemSpacing.x);
    changed |= ImGui::Checkbox("Hide normal maps", &flt.hideNormals);
    flow(checkboxWidth("Hide depth / grayscale"), style.ItemSpacing.x);
    changed |= ImGui::Checkbox("Hide depth / grayscale", &flt.hideGrayscale);
    ImGui::SetItemTooltip("Single-channel textures (BC4 and similar): depth/height, roughness, masks.\n"
                          "Grey-looking colour textures (concrete, metal, logos) are not hidden.\n"
                          "Texture types are detected automatically in the background.");
    flow(checkboxWidth("Hide packed / masks"), style.ItemSpacing.x);
    changed |= ImGui::Checkbox("Hide packed / masks", &flt.hidePacked);
    ImGui::SetItemTooltip("Data textures that are not pictures:\n"
                          "- the blue/cyan/teal ones: grayscale maps (roughness, AO...) packed into one texture's channels\n"
                          "- flat red/green/blue material ID masks\n"
                          "Texture types are detected automatically in the background.");
    flow(checkboxWidth("Black Ice only"), style.ItemSpacing.x);
    changed |= ImGui::Checkbox("Black Ice only", &flt.onlyBlackIce);
    ImGui::SetItemTooltip("Only textures of the Black Ice weapon skin: icy cyan with white highlights and dark\n"
                          "cracks, on weapon-sized texture sheets. Detected by colour, so a few may be missed\n"
                          "or slip in.");

    if (!displayOnTop) {  // the first row was full: the right end of this one, or a line of its own
        placeDisplay();
        drawDisplay();
    }
    if (changed) settings_.Save();
}

bool App::DrawFormatFilter(TextureFilters& flt, const std::vector<TextureFile>& files) {
    constexpr size_t kGroups = IM_ARRAYSIZE(kFormatGroups);
    std::string preview;
    size_t shown = 0;
    for (const FormatGroup& g : kFormatGroups) {
        if (flt.hiddenFormats.count(g.name)) continue;
        if (shown++ < 3) preview += (preview.empty() ? "" : ", ") + std::string(g.name);
    }
    if (shown == kGroups) preview = "All formats";
    else if (shown == 0) preview = "No formats";
    else if (shown > 3) preview = std::to_string(shown) + " of " + std::to_string(kGroups) + " formats";

    bool changed = false;
    ImGui::SetNextItemWidth(kFormatFilterWidth);
    if (shown != kGroups) ImGui::PushStyleColor(ImGuiCol_Text, AccentTextColor());  // a reminder that it hides some
    const bool open = ImGui::BeginCombo("##formats", preview.c_str(), ImGuiComboFlags_HeightLargest);
    if (shown != kGroups) ImGui::PopStyleColor();
    if (!open) {
        ImGui::SetItemTooltip("Which texture formats are shown (BC1, BC3, BC7...).");
        return false;
    }
    size_t counts[kGroups] = {};
    for (const TextureFile& f : files) ++counts[FormatGroupOf(f.format)];
    if (ImGui::SmallButton("All")) {
        flt.hiddenFormats.clear();
        changed = true;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("None")) {
        for (const FormatGroup& g : kFormatGroups) flt.hiddenFormats.insert(g.name);
        changed = true;
    }
    ImGui::Separator();
    for (size_t i = 0; i < kGroups; ++i) {
        const FormatGroup& g = kFormatGroups[i];
        bool on = !flt.hiddenFormats.count(g.name);
        ImGui::PushID(g.name);
        if (ImGui::Checkbox(g.name, &on)) {
            if (on) flt.hiddenFormats.erase(g.name);
            else flt.hiddenFormats.insert(g.name);
            changed = true;
        }
        ImGui::SetItemTooltip("%s", g.tooltip);
        ImGui::SameLine(ImGui::GetFontSize() * 9);
        ImGui::TextColored(kDimColor, "%zu", counts[i]);
        ImGui::PopID();
    }
    ImGui::EndCombo();
    return changed;
}

void App::DrawGrid(const std::vector<TextureFile>& files, bool isMod) {
    const TextureFilters& flt = settings_.FiltersFor(isMod);
    std::vector<const TextureFile*> visible;
    visible.reserve(files.size());
    for (const auto& f : files) {
        if (isMod && !showDisabled_ && !f.enabled) continue;
        if (isMod && !InView(f)) continue;
        if (!isMod && hideModdedDumps_ && library_.IsModded(f.hash)) continue;
        if (flt.hideNormals || flt.hideGrayscale || flt.hidePacked) {
            const TextureKind kind = KindOf(f);
            if (flt.hideNormals && kind == TextureKind::Normal) continue;
            if (flt.hideGrayscale && kind == TextureKind::Grayscale) continue;
            if (flt.hidePacked && kind == TextureKind::Packed) continue;
        }
        if (!flt.hiddenFormats.empty() && flt.hiddenFormats.count(kFormatGroups[FormatGroupOf(f.format)].name)) continue;
        if (flt.onlyBlackIce && !IsBlackIce(f)) continue;
        if (MatchesFilter(f)) visible.push_back(&f);
    }
    SortVisible(visible, isMod);

    // The details panel shows one of the selected textures: kept while it is still selected,
    // else the first one shown.
    auto follow = [&] {
        if (selection_ && picked_.count(selection_->path.native())) return;
        selection_.reset();
        for (const TextureFile* f : visible)
            if (picked_.count(f->path.native())) {
                selection_ = Selection{f->path, isMod};
                break;
            }
    };
    // A selection of several covers only what is shown: changing the view or the search drops the
    // rest (all of it when nothing is shown).
    if (picked_.size() > 1) {
        std::unordered_set<std::wstring> shown;
        for (const TextureFile* f : visible)
            if (picked_.count(f->path.native())) shown.insert(f->path.native());
        if (shown.size() < picked_.size()) {
            picked_ = std::move(shown);
            follow();
        }
    }

    if (visible.empty() && filter_[0]) {
        ImGui::TextColored(kDimColor, "Nothing here matches \"%s\".", filter_);
        const size_t other = CountMatches(isMod ? library_.dumps() : library_.mods());
        if (other) {
            ImGui::TextColored(kDimColor, "%zu match(es) in %s.", other, isMod ? "Dumps" : "Mods");
            ImGui::SameLine();
            if (ImGui::SmallButton(isMod ? "Show dumps" : "Show mods")) selectTab_ = isMod ? Tab::Dumps : Tab::Mods;
        }
        if (isMod && modView_ != ModView::All)
            ImGui::TextColored(kDimColor, "Only the part selected on the left is searched; pick All textures to search every mod.");
        return;
    }
    if (visible.empty() && flt.onlyBlackIce && !files.empty()) {
        ImGui::TextColored(kDimColor, analyzer_->Pending() ? "No Black Ice textures found yet; still analysing textures..."
                                                           : "No Black Ice textures here. Untick \"Black Ice only\" to see all.");
        return;
    }
    if (visible.empty() && !flt.hiddenFormats.empty() && !files.empty()) {
        ImGui::TextColored(kDimColor, "No textures here in the formats ticked under Formats.");
        return;
    }
    if (visible.empty()) {
        ImGui::TextColored(kDimColor, isMod ? "No textures here yet. Import .dds files or drop them here."
                                            : "No dumped textures. Turn on dumping in Settings and play the game.");
        return;
    }

    // Several can be selected: Ctrl+click, Shift+click, dragging a box, Ctrl+A. Requests use
    // indices into `visible`.
    auto apply = [&](ImGuiMultiSelectIO* io) {
        for (const ImGuiSelectionRequest& r : io->Requests) {
            if (r.Type == ImGuiSelectionRequestType_SetAll) {
                picked_.clear();
                if (r.Selected)
                    for (const TextureFile* f : visible) picked_.insert(f->path.native());
            } else if (r.Type == ImGuiSelectionRequestType_SetRange) {
                const auto last = std::min<ImGuiSelectionUserData>(r.RangeLastItem, ImGuiSelectionUserData(visible.size()) - 1);
                for (ImGuiSelectionUserData i = r.RangeFirstItem; i <= last; ++i) {
                    if (r.Selected) picked_.insert(visible[size_t(i)]->path.native());
                    else picked_.erase(visible[size_t(i)]->path.native());
                }
            }
        }
        if (!io->Requests.empty()) follow();
    };

    const ImGuiStyle& style = ImGui::GetStyle();
    const float cell = thumbSize_ + style.ItemSpacing.x;
    const int perRow = std::max(1, int((ImGui::GetContentRegionAvail().x + style.ItemSpacing.x) / cell));
    const int rows = int((visible.size() + perRow - 1) / perRow);
    const float rowHeight = thumbSize_ + ImGui::GetTextLineHeight() * 2 + style.ItemSpacing.y * 2;

    // Resizing the cards (or the panel) changes the rows, so the same scroll offset would show
    // other textures. Keep the texture at the top of the view there instead.
    {
        ImGuiStorage* storage = ImGui::GetStateStorage();
        const ImGuiID kPerRow = ImGui::GetID("##gridPerRow"), kRowHeight = ImGui::GetID("##gridRowHeight");
        const int oldPerRow = storage->GetInt(kPerRow, 0);
        const float oldRowHeight = storage->GetFloat(kRowHeight, 0.0f);
        if (oldPerRow > 0 && oldRowHeight > 0.0f && (oldPerRow != perRow || oldRowHeight != rowHeight)) {
            const float top = ImGui::GetCursorPosY();
            const float rowPos = std::max(0.0f, (ImGui::GetScrollY() - top) / oldRowHeight);
            const int first = int(rowPos) * oldPerRow;  // first texture of the top row
            const float into = rowPos - float(int(rowPos));
            if (rowPos > 0.0f) ImGui::SetScrollY(top + (float(first / perRow) + into) * rowHeight);
        }
        storage->SetInt(kPerRow, perRow);
        storage->SetFloat(kRowHeight, rowHeight);
    }

    // Before the cards, so a click that stops auto-scroll doesn't also select one.
    float selectedY = -1.0f;
    if (selection_)
        for (size_t i = 0; i < visible.size(); ++i)
            if (visible[i]->path == selection_->path) {
                selectedY = ImGui::GetCursorPosY() + float(i / size_t(perRow)) * rowHeight;
                break;
            }
    HandleGridMiddleClick(selectedY, rowHeight);

    ImGuiMultiSelectIO* ms = ImGui::BeginMultiSelect(ImGuiMultiSelectFlags_ClearOnEscape |
                                                         ImGuiMultiSelectFlags_ClearOnClickVoid |
                                                         ImGuiMultiSelectFlags_BoxSelect2d,
                                                     int(picked_.size()), int(visible.size()));
    apply(ms);
    ImGuiListClipper clipper;  // only rows on screen are laid out and thumbnailed
    clipper.Begin(rows, rowHeight);
    if (ms->RangeSrcItem >= 0 && ms->RangeSrcItem < ImGuiSelectionUserData(visible.size()))
        clipper.IncludeItemByIndex(int(ms->RangeSrcItem) / perRow);  // Shift+click needs it laid out
    while (clipper.Step()) {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
            for (int col = 0; col < perRow; ++col) {
                const size_t i = size_t(row) * perRow + col;
                if (i >= visible.size()) break;
                if (col) ImGui::SameLine();
                ImGui::SetNextItemSelectionUserData(ImGuiSelectionUserData(i));
                DrawCard(*visible[i], isMod, thumbSize_);
            }
        }
    }
    apply(ImGui::EndMultiSelect());

    if (!picked_.empty() && ImGui::Shortcut(ImGuiKey_Delete))
        Defer([this, files = Copies(Picked())] { Recycle(files); });
}

void App::HandleGridMiddleClick(float selectedY, float rowHeight) {
    const ImGuiIO& io = ImGui::GetIO();
    AutoScroll& as = autoScroll_;
    const int frame = ImGui::GetFrameCount();
    if (as.active && as.frame != frame - 1) as.active = false;
    as.frame = frame;

    // A click that stopped auto-scroll belongs to it: hide it from the cards and box selection,
    // its release included (that is what opens the right-click menu).
    for (int b = 0; b < 3; ++b) {
        if (!(swallowButtons_ & (1 << b))) continue;
        if (io.MouseDown[b] || io.MouseReleased[b])
            ImGui::SetKeyOwner(ImGui::MouseButtonToKey(b), ImGuiKeyOwner_Any, ImGuiInputFlags_LockThisFrame);
        else
            swallowButtons_ &= ~(1 << b);
    }

    if (!as.active) {
        if (!ImGui::IsWindowHovered() || !ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) return;
        if (settings_.middleClickJumps) {
            if (selectedY >= 0.0f)
                ImGui::SetScrollY(selectedY - (ImGui::GetWindowHeight() - rowHeight) * 0.5f);  // centred
            else if (selection_)
                Status("", "The selected texture is not shown here (search, filters or the part picked on the left).");
            return;
        }
        if (ImGui::GetScrollMaxY() <= 0.0f) return;  // nothing to scroll
        as.active = true;
        as.held = true;
        as.dragged = false;
        as.anchor = io.MousePos;
        return;
    }

    // Like Chrome: a quick middle click leaves it on until the next click; holding the button
    // and moving away scrolls until it is let go.
    constexpr float kDeadZone = 10.0f;
    const float dy = io.MousePos.y - as.anchor.y;
    bool stop = false;
    if (as.held) {
        if (std::fabs(dy) > kDeadZone || std::fabs(io.MousePos.x - as.anchor.x) > kDeadZone) as.dragged = true;
        if (!io.MouseDown[ImGuiMouseButton_Middle]) {
            as.held = false;
            stop = as.dragged;
        }
    }
    for (int b = 0; b < 3; ++b)
        if (io.MouseClicked[b]) {
            stop = true;
            swallowButtons_ |= 1 << b;
            ImGui::SetKeyOwner(ImGui::MouseButtonToKey(b), ImGuiKeyOwner_Any, ImGuiInputFlags_LockThisFrame);
        }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        stop = true;
        ImGui::SetKeyOwner(ImGuiKey_Escape, ImGuiKeyOwner_Any, ImGuiInputFlags_LockThisFrame);  // keep the selection
    }
    if (io.MouseWheel != 0.0f) stop = true;
    if (stop) {
        as.active = false;
        return;
    }

    // Faster the further from where it started; nothing inside the dead zone.
    const float away = std::max(0.0f, std::fabs(dy) - kDeadZone);
    const float speed = away * 5.0f + away * away * 0.03f;  // pixels per second
    if (away > 0.0f) ImGui::SetScrollY(ImGui::GetScrollY() + std::copysign(speed * io.DeltaTime, dy));

    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const ImVec2 c = as.anchor;
    const ImU32 fill = IM_COL32(30, 30, 34, 220), line = IM_COL32(220, 220, 225, 255);
    dl->AddCircleFilled(c, 15.0f, fill);
    dl->AddCircle(c, 15.0f, line, 0, 1.5f);
    dl->AddCircleFilled(c, 2.5f, line);
    dl->AddTriangleFilled(ImVec2(c.x, c.y - 11), ImVec2(c.x - 5, c.y - 5), ImVec2(c.x + 5, c.y - 5),
                          ImGui::GetScrollY() > 0.0f ? line : IM_COL32(120, 120, 125, 255));
    dl->AddTriangleFilled(ImVec2(c.x, c.y + 11), ImVec2(c.x + 5, c.y + 5), ImVec2(c.x - 5, c.y + 5),
                          ImGui::GetScrollY() < ImGui::GetScrollMaxY() ? line : IM_COL32(120, 120, 125, 255));
}

void App::DrawCard(const TextureFile& file, bool isMod, float size) {
    ImGui::PushID(file.path.c_str());
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float textH = ImGui::GetTextLineHeight();
    const ImVec2 cardSize(size, size + textH * 2 + 4);
    const bool selected = picked_.count(file.path.native()) > 0;
    // A selectable for the multi-selection; the card draws its own highlight (the border below).
    ImGui::PushStyleColor(ImGuiCol_Header, 0);
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, 0);
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, 0);
    const bool clicked = ImGui::Selectable("##card", selected, ImGuiSelectableFlags_None, cardSize);
    ImGui::PopStyleColor(3);
    const bool hovered = ImGui::IsItemHovered();
    if (clicked) selection_ = Selection{file.path, isMod};

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(pos, ImVec2(pos.x + size, pos.y + size), IM_COL32(28, 28, 32, 255), 4.0f);

    const TextureInfo* info = nullptr;
    ID3D11ShaderResourceView* srv = thumbs_.Get(file.path, file.modified, uint32_t(size), &info);
    if (srv && info) {
        DrawPreview(dl, srv, info->width, info->height, ImVec2(pos.x + 4, pos.y + 4), ImVec2(size - 8, size - 8),
                    (isMod && !file.enabled) ? IM_COL32(255, 255, 255, 70) : IM_COL32_WHITE);
    } else {
        const char* msg = info ? "can't preview" : "loading...";
        const ImVec2 ts = ImGui::CalcTextSize(msg);
        dl->AddText(ImVec2(pos.x + (size - ts.x) * 0.5f, pos.y + (size - ts.y) * 0.5f), IM_COL32(140, 140, 140, 255), msg);
    }

    // Badges
    if (isMod && !file.enabled)
        dl->AddText(ImVec2(pos.x + 6, pos.y + 4), IM_COL32(255, 180, 80, 255),
                    file.selfDisabled ? "off" : "category off");
    else if (isMod && file.otherBuild)
        dl->AddText(ImVec2(pos.x + 6, pos.y + 4), IM_COL32(160, 160, 160, 255), "other build");
    else if (isMod && file.unused)
        dl->AddText(ImVec2(pos.x + 6, pos.y + 4), IM_COL32(255, 120, 110, 255), "unused (duplicate)");
    if (!isMod && library_.IsModded(file.hash))
        dl->AddText(ImVec2(pos.x + 6, pos.y + 4), IM_COL32(110, 200, 255, 255), "modded");
    if (info && info->encrypted) {
        const float w = ImGui::CalcTextSize("enc").x;
        dl->AddText(ImVec2(pos.x + size - w - 6, pos.y + 4), IM_COL32(255, 220, 120, 255), "enc");
    }

    if (const TextureKind kind = KindOf(file);
        kind == TextureKind::Normal || kind == TextureKind::Grayscale || kind == TextureKind::Packed) {
        const char* badge = kKindBadges[int(kind)];
        const float w = ImGui::CalcTextSize(badge).x;
        dl->AddRectFilled(ImVec2(pos.x + size - w - 10, pos.y + size - textH - 6), ImVec2(pos.x + size - 2, pos.y + size - 2),
                          IM_COL32(20, 20, 24, 200), 3.0f);
        dl->AddText(ImVec2(pos.x + size - w - 6, pos.y + size - textH - 4), kKindColors[int(kind)], badge);
    }

    const ImU32 border = selected ? ImGui::GetColorU32(AccentColor()) : hovered ? IM_COL32(120, 120, 130, 255) : 0;
    if (border) dl->AddRect(pos, ImVec2(pos.x + size, pos.y + size), border, 4.0f, 0, selected ? 2.5f : 1.0f);

    // A label of your own takes the hash's place on top; the hash moves to the second line.
    char line[64];
    snprintf(line, sizeof(line), "%u", file.hash);
    const std::string* userLabel = labels_.Get(file.hash);
    dl->PushClipRect(ImVec2(pos.x, pos.y + size), ImVec2(pos.x + size, pos.y + cardSize.y), true);
    if (userLabel) dl->AddText(ImVec2(pos.x + 2, pos.y + size + 2), ImGui::GetColorU32(AccentTextColor()), userLabel->c_str());
    else dl->AddText(ImVec2(pos.x + 2, pos.y + size + 2), IM_COL32(230, 230, 230, 255), line);
    const ImVec2 line2(pos.x + 2, pos.y + size + 2 + textH);
    const std::string& sortKey = settings_.SortFor(isMod).key;
    if (sortKey == "date") {
        dl->AddText(line2, IM_COL32(150, 150, 160, 255), FormatDate(file.modified).c_str());
    } else if (sortKey == "filesize") {
        dl->AddText(line2, IM_COL32(150, 150, 160, 255), FormatBytes(file.size).c_str());
    } else if (userLabel) {
        dl->AddText(line2, IM_COL32(150, 150, 160, 255), line);
    } else if (!file.label.empty())
        dl->AddText(line2, IM_COL32(150, 150, 160, 255), file.label.c_str());
    else if (info && !info->error.empty())
        dl->AddText(ImVec2(pos.x + 2, pos.y + size + 2 + textH), IM_COL32(255, 120, 110, 255), info->error.c_str());
    else if (info) {
        snprintf(line, sizeof(line), "%ux%u %s", file.width, file.height, FormatName(file.format));
        dl->AddText(ImVec2(pos.x + 2, pos.y + size + 2 + textH), IM_COL32(150, 150, 160, 255), line);
    }
    dl->PopClipRect();

    if (ImGui::BeginPopupContextItem("ctx")) {
        // Right-clicking one of several selected acts on all of them.
        if (selected && picked_.size() > 1) DrawSelectionMenu(isMod);
        else DrawCardMenu(file, isMod, info);
        ImGui::EndPopup();
    }
    ImGui::PopID();
}

void App::DrawCardMenu(const TextureFile& file, bool isMod, const TextureInfo* info) {
    if (!selection_ || selection_->path != file.path) Select(file.path, isMod);
    if (isMod) {
        if (ImGui::MenuItem(file.selfDisabled ? "Turn on" : "Turn off")) Defer([this, file] { ToggleEnabled(file); });
        DrawMoveMenu({&file}, false);
        DrawMoveMenu({&file}, true);
        const bool encrypted = info && info->encrypted;
        if (ImGui::MenuItem("Open in editor", nullptr, false, !encrypted)) OpenWithShell(file.path);
        if (encrypted) ImGui::SetItemTooltip("Encrypted (resorep1) textures can't be opened by image editors.");
        if (ImGui::MenuItem("Tint...", nullptr, false, !encrypted && !tintJob_.valid())) OpenTint({&file});
        ImGui::SetItemTooltip(encrypted ? "Encrypted (resorep1) textures can't be tinted."
                                        : "Recolor this texture, e.g. turn Black Ice red. The original goes to the Recycle Bin.");
    } else {
        if (ImGui::MenuItem("Replace with .dds...")) Defer([this, hash = file.hash] { ReplaceDump(hash); });
        DrawAddToModMenu({&file});
        if (ImGui::MenuItem("Open in editor")) OpenWithShell(file.path);
    }
    if (ImGui::MenuItem("Show in Explorer")) ShowInExplorer(file.path);
    const bool encryptedFile = info && info->encrypted;
    if (ImGui::MenuItem("Export PNG...", nullptr, false, !encryptedFile)) Defer([this, file] { ExportAsPng(file); });
    ImGui::SetItemTooltip(encryptedFile ? "Encrypted (resorep1) textures can't be exported."
                                        : "Full resolution, turned and mirrored the way the preview shows it.");
    ImGui::Separator();
    if (ImGui::MenuItem("Edit label...")) {
        const std::string* current = labels_.Get(file.hash);
        snprintf(labelDialog_, sizeof(labelDialog_), "%s", current ? current->c_str() : "");
        labelDialogHash_ = file.hash;
        openLabelPopup_ = true;
    }
    const bool isNormal = KindOf(file) == TextureKind::Normal;
    if (ImGui::MenuItem(isNormal ? "Find its color texture" : "Find its normal map"))
        OpenSimilar(file, isMod, isNormal ? SimilarMode::Colors : SimilarMode::Normals);
    if (ImGui::MenuItem("Find similar textures")) OpenSimilar(file, isMod, SimilarMode::Any);
    ImGui::Separator();
    if (ImGui::MenuItem("Move to Recycle Bin", "Del")) Defer([this, file] { Recycle({file}); });
}

void App::DrawSelectionMenu(bool isMod) {
    const std::vector<const TextureFile*> files = Picked();
    ImGui::TextColored(kDimColor, "%zu textures selected", files.size());
    ImGui::Separator();
    if (isMod) {
        const bool anyOff = std::any_of(files.begin(), files.end(), [](const TextureFile* f) { return f->selfDisabled; });
        const bool anyOn = std::any_of(files.begin(), files.end(), [](const TextureFile* f) { return !f->selfDisabled; });
        if (ImGui::MenuItem("Turn on", nullptr, false, anyOff)) Defer([this, c = Copies(files)] { SetEnabled(c, true); });
        if (ImGui::MenuItem("Turn off", nullptr, false, anyOn)) Defer([this, c = Copies(files)] { SetEnabled(c, false); });
        DrawMoveMenu(files, false);
        DrawMoveMenu(files, true);
        if (ImGui::MenuItem("Tint...", nullptr, false, !tintJob_.valid())) OpenTint(files);
        ImGui::SetItemTooltip("Recolor all of these, e.g. turn Black Ice red. The originals go to the Recycle Bin.");
    } else {
        DrawAddToModMenu(files);
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Move to Recycle Bin", "Del")) Defer([this, c = Copies(files)] { Recycle(c); });
}

void App::DrawDetails() {
    if (picked_.size() > 1) {
        if (const std::vector<const TextureFile*> files = Picked(); files.size() > 1) {
            DrawSelectionDetails(files);
            return;
        }
    }
    const TextureFile* file = FindSelected();
    if (!file) {
        ImGui::TextColored(kDimColor, "Select a texture to see details.");
        ImGui::TextColored(kDimColor, "Ctrl+click, Shift+click or drag a box to select several.");
        ImGui::Spacing();
        ImGui::TextWrapped(tab_ == Tab::Mods
                               ? "Mods are .dds files named after the hash of the game texture they replace."
                               : "Dumps are the game's own textures, saved while you play with dumping on. "
                                 "Pick one and choose a replacement to create a mod.");
        return;
    }
    const bool isMod = selection_->isMod;
    const float width = ImGui::GetContentRegionAvail().x;

    // The panel always reserves its scrollbar (see BeginChild), so `width` cannot change
    // with the preview's height; and the box is capped so the details stay in view.
    const float box = std::min(width, ImGui::GetWindowHeight() * 0.5f);
    PreviewTweak& tweak = previewTweaks_[file->hash];
    const TextureInfo* info = nullptr;
    ID3D11ShaderResourceView* srv = thumbs_.Get(file->path, file->modified, 0, &info);
    if (srv && info) {
        // The view is always the whole width x box area, so a tall or wide texture can use
        // all of it once zoomed in; at 100% fit it sits in the middle.
        const ImVec2 fit = PreviewSize(info->width, info->height, box, tweak.quarterTurns);
        const ImVec2 view(width, box);
        const ImVec2 at = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##preview", view, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
        ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);  // the wheel zooms here instead of scrolling the panel

        // Zoom: wheel steps of 25% around the cursor, up to 16 screen pixels per texel.
        const int turns = (settings_.previewRotation / 90 + tweak.quarterTurns) & 3;
        const float texelsAcross = float(turns % 2 ? info->height : info->width);
        const float maxZoom = std::max(1.0f, 16.0f * texelsAcross / fit.x);
        const ImGuiIO& io = ImGui::GetIO();
        auto imageSize = [&] { return ImVec2(fit.x * tweak.zoom, fit.y * tweak.zoom); };
        auto imageMin = [&] {
            const ImVec2 s = imageSize();
            return ImVec2(at.x + view.x * 0.5f - tweak.centre.x * s.x, at.y + view.y * 0.5f - tweak.centre.y * s.y);
        };
        // Smaller than the view along an axis: centred. Larger: no empty space at its edges.
        auto clampCentre = [&] {
            const ImVec2 s = imageSize();
            auto axis = [](float& c, float viewLen, float len) {
                const float half = viewLen * 0.5f / len;
                c = len <= viewLen ? 0.5f : std::clamp(c, half, 1.0f - half);
            };
            axis(tweak.centre.x, view.x, s.x);
            axis(tweak.centre.y, view.y, s.y);
        };
        if (ImGui::IsItemHovered() && io.MouseWheel != 0.0f) {
            // Keep the texel under the cursor where it is.
            const ImVec2 m0 = imageMin(), s0 = imageSize();
            const ImVec2 u((io.MousePos.x - m0.x) / s0.x, (io.MousePos.y - m0.y) / s0.y);
            tweak.zoom = std::clamp(tweak.zoom * std::pow(1.25f, io.MouseWheel), 1.0f, maxZoom);
            const ImVec2 s1 = imageSize();
            tweak.centre.x = u.x + (at.x + view.x * 0.5f - io.MousePos.x) / s1.x;
            tweak.centre.y = u.y + (at.y + view.y * 0.5f - io.MousePos.y) / s1.y;
            clampCentre();
        }
        if (ImGui::IsItemActive() && (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f)) {
            const ImVec2 s = imageSize();
            tweak.centre.x -= io.MouseDelta.x / s.x;
            tweak.centre.y -= io.MouseDelta.y / s.y;
            clampCentre();
        }
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            tweak.zoom = 1.0f;
            tweak.centre = ImVec2(0.5f, 0.5f);
        }
        if (tweak.zoom > 1.0f && ImGui::IsItemHovered()) {
            ImGui::SetMouseCursor(ImGui::IsItemActive() ? ImGuiMouseCursor_ResizeAll : ImGuiMouseCursor_Hand);
        }
        ImGui::SetItemTooltip("Scroll to zoom, drag to move around, double-click to fit.");

        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->PushClipRect(at, ImVec2(at.x + view.x, at.y + view.y), true);
        const ImVec2 zoomed = imageSize();
        // Past 100% each texel covers several screen pixels; point sampling keeps their edges hard.
        const bool sharp = settings_.sharpZoom && pointSampler_ && zoomed.x > texelsAcross;
        if (sharp) dl->AddCallback(UseSampler, pointSampler_.Get());
        DrawPreview(dl, srv, info->width, info->height, imageMin(), zoomed, IM_COL32_WHITE, tweak.quarterTurns,
                    tweak.mirror);
        if (sharp) dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
        if (tweak.zoom > 1.0f) {
            // Size on screen relative to the texture's own pixels.
            char text[32];
            snprintf(text, sizeof(text), "%.0f%%", 100.0f * zoomed.x / std::max(1.0f, texelsAcross));
            const ImVec2 size = ImGui::CalcTextSize(text);
            const ImVec2 p(at.x + view.x - size.x - 8, at.y + 4);
            dl->AddRectFilled(ImVec2(p.x - 4, p.y - 2), ImVec2(p.x + size.x + 4, p.y + size.y + 2), IM_COL32(0, 0, 0, 170), 4.0f);
            dl->AddText(p, IM_COL32_WHITE, text);
        }
        dl->PopClipRect();
    } else {
        ImGui::Dummy(ImVec2(width, box));
    }

    // This preview's own orientation, on top of the grid's Rotate/Mirror setting.
    // Turning the picture would leave a zoomed view looking somewhere else, so it goes back to fit.
    const float buttonW = (width - ImGui::GetStyle().ItemSpacing.x * 3) / 4;
    auto unzoom = [&] {
        tweak.zoom = 1.0f;
        tweak.centre = ImVec2(0.5f, 0.5f);
    };
    if (ImGui::Button("Rotate left", ImVec2(buttonW, 0))) {
        tweak.quarterTurns = (tweak.quarterTurns + 3) & 3;
        unzoom();
    }
    ImGui::SameLine();
    if (ImGui::Button("Rotate right", ImVec2(buttonW, 0))) {
        tweak.quarterTurns = (tweak.quarterTurns + 1) & 3;
        unzoom();
    }
    ImGui::SameLine();
    if (ImGui::Button(tweak.mirror ? "Unmirror" : "Mirror", ImVec2(buttonW, 0))) {
        tweak.mirror = !tweak.mirror;
        unzoom();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(tweak.quarterTurns == 0 && !tweak.mirror && tweak.zoom == 1.0f);
    if (ImGui::Button("Reset", ImVec2(buttonW, 0))) tweak = {};
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("These buttons turn only this preview (remembered per texture while the app is open).\n"
                          "The Rotate / Mirror options above apply to every preview. Reset also undoes zoom.");

    ImGui::SeparatorText(isMod ? "Mod" : "Original texture");
    ImGui::Text("Hash: %u", file->hash);
    DrawLabelEditor(*file, width);
    if (!file->label.empty()) ImGui::Text("File name label: %s", file->label.c_str());
    ImGui::Text("Type: %s%s", KindName(KindOf(*file)), IsBlackIce(*file) ? " (Black Ice skin)" : "");
    if (info) {
        if (!info->error.empty()) ImGui::TextColored(kErrorColor, "%s", info->error.c_str());
        else ImGui::Text("%u x %u, %u mip(s), %s", info->width, info->height, info->mipLevels, FormatName(info->format));
        if (info->encrypted) ImGui::TextColored(AccentTextColor(), "Encrypted (resorep1 pack)");
    }
    ImGui::Text("File size: %s", FormatBytes(file->size).c_str());
    ImGui::Text("Modified: %s", FormatDate(file->modified).c_str());
    if (isMod) ImGui::Text("Mod / category: %s", file->relDir.empty() ? "(loose)" : WideToUtf8(file->relDir).c_str());
    ImGui::TextColored(kDimColor, "%s", ToUtf8(file->path.filename()).c_str());
    ImGui::Spacing();

    if (isMod) {
        if (ImGui::Button(file->selfDisabled ? "Turn on" : "Turn off", ImVec2(width * 0.5f - 4, 0)))
            Defer([this, copy = *file] { ToggleEnabled(copy); });
        ImGui::SameLine();
        const bool encrypted = info && info->encrypted;
        ImGui::BeginDisabled(encrypted);
        if (ImGui::Button("Open in editor", ImVec2(-1, 0))) OpenWithShell(file->path);
        ImGui::EndDisabled();
        ImGui::SetItemTooltip(encrypted ? "Encrypted (resorep1) textures can't be opened by image editors."
                                        : "Opens the .dds in your default editor. Save it there; the preview and a "
                                          "running game pick up the change.");
        if (ImGui::Button("Show in Explorer", ImVec2(-1, 0))) ShowInExplorer(file->path);
        if (!file->selfDisabled && !file->enabled)
            ImGui::TextColored(kErrorColor, "Off because its mod or category is turned off.");
        ImGui::PushStyleColor(ImGuiCol_Text, kErrorColor);  // wrapped: the details panel is narrow
        if (file->otherBuild)
            ImGui::TextWrapped("Its mod is only for %s, so this game skips it.", ModGameName(file->game).c_str());
        // Where another texture with the same hash wins, per build that loads this one.
        std::vector<std::pair<std::string, const fs::path*>> lost;
        for (const auto& [build, winners] : library_.winners()) {
            auto it = winners.find(file->hash);
            if (file->enabled && Library::LoadsIn(*file, build) && it != winners.end() && it->second != file->path)
                lost.emplace_back(build, &it->second);
        }
        if (!lost.empty())
            ImGui::TextWrapped("%s", file->unused ? "Another active texture has this hash, so this one is unused:"
                                                  : "Another active texture with this hash wins in:");
        ImGui::PopStyleColor();
        if (!lost.empty()) {
            for (const auto& [build, winner] : lost) {
                const std::string where = ProfileGame()  ? "This game"
                                          : build.empty() ? "Builds with no mods marked for them"
                                                          : ModGameName(build);
                ImGui::TextWrapped("%s: %s", where.c_str(),
                                   ToUtf8(winner->lexically_relative(library_.moddedFolder())).c_str());
            }
            ImGui::TextColored(kDimColor, "Turn the others off to choose which one wins.");
        }
    } else {
        if (ImGui::Button("Replace with .dds...", ImVec2(width * 0.5f - 4, 0)))
            Defer([this, hash = file->hash] { ReplaceDump(hash); });
        ImGui::SameLine();
        if (ImGui::Button("Add to mod...", ImVec2(-1, 0))) ImGui::OpenPopup("addtomod");
        ImGui::SetItemTooltip("Copies this texture into a mod as <hash>.dds, ready to edit.");
        if (ImGui::BeginPopup("addtomod")) {
            DrawAddToModItems({file});
            ImGui::EndPopup();
        }
        if (ImGui::Button("Open in editor", ImVec2(width * 0.5f - 4, 0))) OpenWithShell(file->path);
        ImGui::SameLine();
        if (ImGui::Button("Show in Explorer", ImVec2(-1, 0))) ShowInExplorer(file->path);

        // Side-by-side with the mod that currently replaces it (hash tolerance included).
        for (const auto& m : library_.mods()) {
            const fs::path* active = library_.ModFor(file->hash);
            if (!active || m.path != *active) continue;
            ImGui::SeparatorText("Replaced by");
            const TextureInfo* modInfo = nullptr;
            if (ID3D11ShaderResourceView* modSrv = thumbs_.Get(m.path, m.modified, 256, &modInfo); modSrv && modInfo) {
                const ImVec2 fit = PreviewSize(modInfo->width, modInfo->height, std::min(width, 256.0f));
                const ImVec2 at = ImGui::GetCursorScreenPos();
                ImGui::Dummy(fit);
                DrawPreview(ImGui::GetWindowDrawList(), modSrv, modInfo->width, modInfo->height, at, fit,
                            IM_COL32_WHITE);
            }
            ImGui::TextColored(kDimColor, "%s", ToUtf8(m.path.lexically_relative(library_.moddedFolder())).c_str());
            if (ImGui::Button("Go to mod")) {
                tab_ = Tab::Mods;
                modView_ = ModView::All;
                Select(m.path, true);
            }
            break;
        }
    }
}

void App::DrawSelectionDetails(const std::vector<const TextureFile*>& files) {
    const bool isMod = selection_->isMod;
    const float width = ImGui::GetContentRegionAvail().x;
    uint64_t bytes = 0;
    size_t off = 0, categoryOff = 0;  // turned off themselves / on, but their mod or category is off
    for (const TextureFile* f : files) {
        bytes += f->size;
        off += f->selfDisabled;
        categoryOff += !f->selfDisabled && !f->enabled;
    }
    ImGui::SeparatorText(isMod ? "Selected mods" : "Selected dumps");
    ImGui::Text("%zu textures, %s", files.size(), FormatBytes(bytes).c_str());
    if (isMod) {
        ImGui::Text("%zu on, %zu turned off", files.size() - off - categoryOff, off);
        if (categoryOff)
            ImGui::TextColored(kDimColor, "%zu more off because their mod or category is off", categoryOff);
    }
    ImGui::Spacing();

    const ImVec2 half(width * 0.5f - 4, 0);
    if (isMod) {
        ImGui::BeginDisabled(off == 0);
        if (ImGui::Button("Turn all on", half)) Defer([this, c = Copies(files)] { SetEnabled(c, true); });
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(off == files.size());
        if (ImGui::Button("Turn all off", ImVec2(-1, 0))) Defer([this, c = Copies(files)] { SetEnabled(c, false); });
        ImGui::EndDisabled();
        if (ImGui::Button("Move to...", half)) ImGui::OpenPopup("moveto");
        ImGui::SameLine();
        if (ImGui::Button("Copy to...", ImVec2(-1, 0))) ImGui::OpenPopup("copyto");
        if (ImGui::BeginPopup("moveto")) {
            DrawMoveItems(files, false);
            ImGui::EndPopup();
        }
        if (ImGui::BeginPopup("copyto")) {
            DrawMoveItems(files, true);
            ImGui::EndPopup();
        }
        ImGui::BeginDisabled(tintJob_.valid());
        if (ImGui::Button("Tint...", ImVec2(-1, 0))) OpenTint(files);
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Recolor all of these, e.g. turn Black Ice red. The originals go to the Recycle Bin.");
    } else {
        if (ImGui::Button("Add to mod...", ImVec2(-1, 0))) ImGui::OpenPopup("addtomod");
        ImGui::SetItemTooltip("Copies these textures into a mod as <hash>.dds, ready to edit.");
        if (ImGui::BeginPopup("addtomod")) {
            DrawAddToModItems(files);
            ImGui::EndPopup();
        }
    }
    if (ImGui::Button("Move to Recycle Bin", half)) Defer([this, c = Copies(files)] { Recycle(c); });
    ImGui::SameLine();
    if (ImGui::Button("Clear selection", ImVec2(-1, 0))) {
        ClearSelection();
        return;
    }
    ImGui::TextColored(kDimColor, "Right-click a selected texture for the same actions.");

    // Small previews of the selection; clicking one selects just that one.
    ImGui::SeparatorText("Textures");
    constexpr size_t kMaxShown = 60;
    const float box = 56.0f;
    const ImGuiStyle& style = ImGui::GetStyle();
    const int perRow = std::max(1, int((width + style.ItemSpacing.x) / (box + style.ItemSpacing.x)));
    for (size_t i = 0; i < files.size() && i < kMaxShown; ++i) {
        const TextureFile& f = *files[i];
        if (i % perRow) ImGui::SameLine();
        ImGui::PushID(f.path.c_str());
        const ImVec2 at = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("thumb", ImVec2(box, box))) Select(f.path, isMod);
        ImGui::SetItemTooltip("%u%s%s", f.hash, DisplayLabel(f).empty() ? "" : "  ", DisplayLabel(f).c_str());
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(at, ImVec2(at.x + box, at.y + box), IM_COL32(28, 28, 32, 255), 3.0f);
        const TextureInfo* info = nullptr;
        // Same size as the grid's, so these come from the thumbnails it already made.
        if (ID3D11ShaderResourceView* srv = thumbs_.Get(f.path, f.modified, uint32_t(thumbSize_), &info); srv && info)
            DrawPreview(dl, srv, info->width, info->height, ImVec2(at.x + 2, at.y + 2), ImVec2(box - 4, box - 4),
                        (isMod && !f.enabled) ? IM_COL32(255, 255, 255, 70) : IM_COL32_WHITE);
        if (ImGui::IsItemHovered()) dl->AddRect(at, ImVec2(at.x + box, at.y + box), IM_COL32(120, 120, 130, 255), 3.0f);
        ImGui::PopID();
    }
    if (files.size() > kMaxShown) ImGui::TextColored(kDimColor, "and %zu more", files.size() - kMaxShown);
}

void App::DrawLabelEditor(const TextureFile& file, float width) {
    if (labelEditHash_ != file.hash) {
        const std::string* current = labels_.Get(file.hash);
        snprintf(labelEdit_, sizeof(labelEdit_), "%s", current ? current->c_str() : "");
        labelEditHash_ = file.hash;
    }
    ImGui::SetNextItemWidth(width - 70);
    ImGui::InputTextWithHint("##label", "Label this hash (e.g. Ash helmet)", labelEdit_, sizeof(labelEdit_));
    if (ImGui::IsItemDeactivatedAfterEdit()) SetLabel(file.hash, labelEdit_);
    ImGui::SetItemTooltip("Your own name for this texture hash. Saved in labels.txt next to ReResorep.exe;\n"
                          "applies to the dump and every mod for this hash. Press Enter to save.");
    ImGui::SameLine();
    ImGui::BeginDisabled(!labels_.Get(file.hash));
    if (ImGui::Button("Clear", ImVec2(-1, 0))) {
        labelEdit_[0] = 0;
        SetLabel(file.hash, "");
    }
    ImGui::EndDisabled();
}

void App::DrawLabelPopup() {
    if (openLabelPopup_) {
        ImGui::OpenPopup("Edit label");
        openLabelPopup_ = false;
    }
    if (!ImGui::BeginPopupModal("Edit label", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::Text("Label for hash %u:", labelDialogHash_);
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(300);
    const bool enter = ImGui::InputTextWithHint("##label", "e.g. Ash helmet (empty removes it)", labelDialog_,
                                                sizeof(labelDialog_),
                                                ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
    if (ImGui::Button("Save", ImVec2(120, 0)) || enter) {
        SetLabel(labelDialogHash_, labelDialog_);
        labelEditHash_.reset();  // the details panel's editor shows the new label
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void App::DrawSimilarWindow() {
    if (!similar_) return;
    bool open = true;
    ImGui::SetNextWindowSize(ImVec2(560, 640), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Similar textures###similar", &open, ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::End();
        if (!open) similar_.reset();
        return;
    }
    SimilarSearch& s = *similar_;
    const TextureFile& src = s.source;

    // Source header
    const TextureInfo* info = nullptr;
    if (ID3D11ShaderResourceView* srv = thumbs_.Get(src.path, src.modified, 96, &info); srv && info) {
        const ImVec2 at = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(96, 96));
        DrawPreview(ImGui::GetWindowDrawList(), srv, info->width, info->height, at, ImVec2(96, 96), IM_COL32_WHITE);
    } else {
        ImGui::Dummy(ImVec2(96, 96));
    }
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::Text("%u", src.hash);
    if (const std::string& label = DisplayLabel(src); !label.empty()) ImGui::TextColored(AccentTextColor(), "%s", label.c_str());
    ImGui::TextColored(kDimColor, "%s, %ux%u", KindName(KindOf(src)), src.width, src.height);
    static const char* kModes[] = {"Normal maps", "Color textures", "Any kind"};
    int mode = int(s.mode);
    ImGui::SetNextItemWidth(160);
    if (ImGui::Combo("Look for", &mode, kModes, 3)) {
        s.mode = SimilarMode(mode);
        ComputeSimilar();
    }
    ImGui::SameLine();
    if (ImGui::Button("Refresh")) ComputeSimilar();
    ImGui::EndGroup();

    ImGui::TextWrapped("For dumps, ranked mostly by dump order: games load a material's textures together, and "
                       "a normal map is usually dumped right after its colour texture. Also by matching detail and "
                       "size. Check the top results by eye.");
    if (!s.computed) ImGui::TextColored(AccentTextColor(), "Analysing this texture...");
    else if (s.notAnalysed)
        ImGui::TextColored(kDimColor, "%zu candidate(s) not analysed yet; Refresh later for more results.", s.notAnalysed);
    ImGui::Separator();

    if (s.computed && s.results.empty()) ImGui::TextColored(kDimColor, "No matching textures found.");
    ImGui::BeginChild("results");
    std::optional<SimilarResult> show;
    for (size_t i = 0; i < s.results.size(); ++i) {
        const SimilarResult& r = s.results[i];
        ImGui::PushID(int(i));
        const ImVec2 at = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(72, 72));
        const TextureInfo* ri = nullptr;
        if (ID3D11ShaderResourceView* srv = thumbs_.Get(r.path, r.modified, 72, &ri); srv && ri)
            DrawPreview(ImGui::GetWindowDrawList(), srv, ri->width, ri->height, at, ImVec2(72, 72), IM_COL32_WHITE);
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::Text("%u  %s", r.hash, r.isMod ? "(mod)" : "(dump)");
        const std::string* label = labels_.Get(r.hash);
        if (label) ImGui::TextColored(AccentTextColor(), "%s", label->c_str());
        ImGui::TextColored(kDimColor, "%s, %ux%u", KindName(r.kind), r.width, r.height);
        ImGui::ProgressBar(r.score, ImVec2(120, 0), (std::to_string(int(r.score * 100)) + "% match").c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Show")) show = r;
        const std::string* srcLabel = labels_.Get(src.hash);
        if (srcLabel && !label) {
            const std::string suggested =
                *srcLabel + (r.kind == TextureKind::Normal ? " (normal)" : r.kind == TextureKind::Grayscale ? " (depth)" : "");
            ImGui::SameLine();
            if (ImGui::SmallButton(("Label \"" + suggested + "\"").c_str())) SetLabel(r.hash, suggested);
        }
        ImGui::EndGroup();
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::End();

    if (show) {
        tab_ = show->isMod ? Tab::Mods : Tab::Dumps;
        modView_ = ModView::All;
        Select(show->path, show->isMod);
    }
    if (!open) similar_.reset();
}

void App::DrawGames() {
    // Which games are running, checked about once a second while this tab is open.
    if (std::chrono::steady_clock::now() - lastRunningCheck_ > std::chrono::seconds(1)) {
        lastRunningCheck_ = std::chrono::steady_clock::now();
        gameRunning_.clear();
        for (const GameState& g : games_) gameRunning_[g.exe.wstring()] = !FindProcesses(g.exe).empty();
    }

    if (ImGui::Button("Add game...")) {
        auto exe = PickFiles(hwnd_, L"Choose the game executable", {{L"Programs", L"*.exe"}}, false);
        if (!exe.empty()) {
            if (std::find(settings_.games.begin(), settings_.games.end(), exe[0]) != settings_.games.end()) {
                Status("That game is already in the list", "");
            } else {
                settings_.games.push_back(exe[0]);
                settings_.Save();
                RefreshGames();
                Status("", "Added " + ToUtf8(exe[0].filename()) + ". Click Install to enable texture replacement.");
            }
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Refresh")) RefreshGames();
    ImGui::SameLine();
    ImGui::TextColored(kDimColor, "Close the game before installing or removing. For Siege pick RainbowSix.exe.");
    if (!Exists(BundledProxy()))
        ImGui::TextColored(kErrorColor, "proxy\\d3d11.dll is missing next to ReResorep.exe - reinstall ReResorep.");
    else if (bundledSeal_ != SealState::Valid)
        ImGui::TextColored(kErrorColor, "proxy\\d3d11.dll next to ReResorep.exe is damaged or was modified - "
                                        "download ReResorep again. Installing is blocked until then.");
    ImGui::Spacing();

    if (games_.empty()) {
        ImGui::TextColored(kDimColor, "No games yet. Add the game's .exe; the proxy d3d11.dll goes next to it.");
        return;
    }
    // One line per game: name and folder, status, then the buttons, all centred on the buttons'
    // height. The buttons column is as wide as one row of buttons (at most half the window, so
    // they wrap on narrow windows); the folder path is what gets cut short.
    const ImGuiStyle& style = ImGui::GetStyle();
    float buttonsWidth = 0;
    for (const char* label : {"Start", "Closing...", "Reinstall", "Uninstall", "Open folder", "[x] Own folders", "Remove..."})
        buttonsWidth += ImGui::CalcTextSize(label).x + style.FramePadding.x * 2 + style.ItemSpacing.x;
    buttonsWidth = std::min(buttonsWidth - style.ItemSpacing.x, ImGui::GetContentRegionAvail().x * 0.5f);
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(style.CellPadding.x * 2, 6.0f));
    const bool table = ImGui::BeginTable("games", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH);
    ImGui::PopStyleVar();
    if (!table) return;
    ImGui::TableSetupColumn("Game", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, buttonsWidth);
    ImGui::TableHeadersRow();
    // Puts the next button beside the last one if it fits in the column, else on a new line.
    auto flow = [](const char* nextLabel) {
        const ImGuiStyle& st = ImGui::GetStyle();
        const float next = ImGui::CalcTextSize(nextLabel, nullptr, true).x + st.FramePadding.x * 2;
        const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
        if (ImGui::GetItemRectMax().x + st.ItemSpacing.x + next <= right) ImGui::SameLine();
    };
    // "A  ·  B" pieces on one line.
    auto part = [](const ImVec4& c, const char* text) {
        ImGui::SameLine(0, 0);
        ImGui::TextColored(kDimColor, "  \xC2\xB7  ");
        ImGui::SameLine(0, 0);
        ImGui::TextColored(c, "%s", text);
    };

    for (size_t i = 0; i < games_.size(); ++i) {
        const GameState& g = games_[i];
        const bool running = gameRunning_.count(g.exe.wstring()) && gameRunning_[g.exe.wstring()];
        ImGui::PushID(int(i));
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        // The game's folder name first: every Siege build is a RainbowSix.exe.
        const std::string name = ToUtf8(g.exe.parent_path().filename());
        ImGui::TextUnformatted(name.empty() ? ToUtf8(g.exe.filename()).c_str() : name.c_str());
        ImGui::SameLine(0, 8);
        if (!g.sha256.empty()) {
            ImGui::TextColored(kDimColor, "%s", g.sha256.substr(0, 8).c_str());
            ImGui::SetItemTooltip("SHA-256 of the exe, which tells Siege builds apart:\n%s", g.sha256.c_str());
        } else if (g.status != ProxyStatus::Missing) {
            ImGui::TextColored(kDimColor, "hashing...");
        }
        ImGui::SameLine(0, 12);
        const std::string path = ToUtf8(g.exe);
        ImGui::TextColored(kDimColor, "%s", path.c_str());
        ImGui::SetItemTooltip("%s", path.c_str());

        ImGui::TableNextColumn();
        const ImVec4 color = g.status == ProxyStatus::Installed ? kOkColor
                             : g.status == ProxyStatus::NotInstalled ? kDimColor
                                                                     : ImVec4(1.0f, 0.75f, 0.3f, 1.0f);
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(color, "%s", StatusText(g.status));
        if (running) part(kOkColor, "Running");
        if (!g.proxyVersion.empty()) {
            part(kDimColor, ("proxy " + g.proxyVersion).c_str());
            if (g.status == ProxyStatus::Outdated)
                ImGui::SetItemTooltip("This ReResorep has proxy %s. Click Update to install it.", RERESOREP_PROXY_VERSION_STRING);
        }
        if (g.status == ProxyStatus::Modified)
            ImGui::SetItemTooltip("The d3d11.dll in the game folder was damaged or changed after it was built, so it\n"
                                  "does nothing: the game runs without mods. Click Repair to install a fresh copy.");
        if (g.chainsOtherD3D11) {
            part(kDimColor, "+ ori_d3d11.dll");
            ImGui::SetItemTooltip("Another d3d11.dll (for example ReShade) was kept as ori_d3d11.dll and still loads.");
        }
        if (!g.previousSha256.empty()) {
            part(kErrorColor, "exe changed");
            ImGui::SameLine();
            if (ImGui::SmallButton("Fix...")) ImGui::OpenPopup("exechanged");
            if (ImGui::BeginPopup("exechanged")) {
                ImGui::Text("%s's exe changed since mods were marked for it (%s -> %s).", ToUtf8(g.exe.filename()).c_str(),
                            g.previousSha256.substr(0, 8).c_str(), g.sha256.substr(0, 8).c_str());
                ImGui::TextColored(kDimColor, "Mods marked for the old exe no longer load in this game.");
                if (ImGui::Button("Same build (patched): move the marks to the new exe"))
                    Defer([this, exe = g.exe] { ResolveExeChange(exe, true); });
                if (ImGui::Button("Different build: leave the marks as they are"))
                    Defer([this, exe = g.exe] { ResolveExeChange(exe, false); });
                ImGui::EndPopup();
            }
        }

        ImGui::TableNextColumn();
        const bool installed = g.status == ProxyStatus::Installed;
        const char* installLabel = installed                                  ? "Reinstall"
                                   : g.status == ProxyStatus::Modified    ? "Repair"
                                   : g.status == ProxyStatus::NotInstalled ? "Install"
                                                                           : "Update";
        const bool closingThis = closeJob_.valid() && closingExe_ == g.exe;
        const char* closeLabel = closingThis ? "Closing...###close" : "Close###close";
        if (g.status != ProxyStatus::Missing) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.48f, 0.26f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.22f, 0.60f, 0.32f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.15f, 0.40f, 0.22f, 1.0f));
            ImGui::BeginDisabled(running);
            if (ImGui::Button("Start")) {
                const std::string e = LaunchProgram(g.exe);
                Status(e, "Started " + ToUtf8(g.exe.filename()) +
                              (installed ? "" : " (ReResorep is not installed for it, so no mods apply)"));
                lastRunningCheck_ = {};  // pick the new process up right away
            }
            ImGui::EndDisabled();
            ImGui::PopStyleColor(3);
            ImGui::SetItemTooltip(running     ? "The game is already running"
                                  : installed ? "Launch the game with your mods"
                                              : "Launch the game (install ReResorep first for mods to apply)");

            flow(closeLabel);
            ImGui::BeginDisabled(!running || closeJob_.valid());
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.18f, 0.18f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.70f, 0.24f, 0.24f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.45f, 0.14f, 0.14f, 1.0f));
            if (ImGui::Button(closeLabel)) {
                closingExe_ = g.exe;
                // Ask nicely, give it time to shut down, then end it; off the UI thread.
                closeJob_ = std::async(std::launch::async, [exe = g.exe] { return CloseProgram(exe, kCloseGraceMs); });
                Status("", "Closing " + ToUtf8(g.exe.filename()) + "...");
            }
            ImGui::PopStyleColor(3);
            ImGui::EndDisabled();
            ImGui::SetItemTooltip("Asks the game to exit. If it is still running after %u seconds, it is ended "
                                  "forcefully (unsaved progress in the game is lost).",
                                  unsigned(kCloseGraceMs / 1000));
            // These rebuild games_, so they run after the table is drawn.
            flow(installLabel);
            if (ImGui::Button(installLabel)) {
                Defer([this, exe = g.exe] {
                    Status(InstallProxy(exe, settings_), "Installed into " + ToUtf8(exe.parent_path()));
                    RefreshGames();
                });
            }
            flow("Uninstall");
            ImGui::BeginDisabled(g.status == ProxyStatus::NotInstalled);
            if (ImGui::Button("Uninstall")) {
                Defer([this, exe = g.exe] {
                    Status(UninstallProxy(exe), "Removed from " + ToUtf8(exe.filename()));
                    RefreshGames();
                });
            }
            ImGui::EndDisabled();
            flow("Open folder");
            if (ImGui::Button("Open folder")) OpenWithShell(g.exe.parent_path());
            flow("[x] Own folders");
            bool own = g.ownFolders;
            ImGui::BeginDisabled(g.sha256.empty());
            if (ImGui::Checkbox("Own folders", &own)) Defer([this, exe = g.exe, own] { SetOwnFolders(exe, own); });
            ImGui::EndDisabled();
            if (g.sha256.empty()) {
                ImGui::SetItemTooltip("Working out the exe's hash first...");
            } else {
                ImGui::SetItemTooltip("On: this game uses its own mods and dumps in\n%s\n"
                                      "Off: it uses the Default (shared) folders from the Settings tab, where it\n"
                                      "skips mods marked for another game.",
                                      ToUtf8(g.ownFolders ? settings_.FoldersFor(g.exe).first.parent_path()
                                                          : GameFolder(g.sha256)).c_str());
            }
            flow("Remove...");
        }
        if (ImGui::Button("Remove...")) {
            removeRecycleFolders_ = false;
            ImGui::OpenPopup("remove");
        }
        ImGui::SetItemTooltip("Remove from this list, or remove completely.");
        if (ImGui::BeginPopup("remove")) {
            const std::string label = name.empty() ? ToUtf8(g.exe.filename()) : name;
            ImGui::Text("Remove %s?", label.c_str());
            ImGui::Spacing();
            if (ImGui::Button("Remove from list")) {
                Defer([this, exe = g.exe] { RemoveGame(exe, false, false); });
                ImGui::CloseCurrentPopup();
            }
            ImGui::TextColored(kDimColor, "The proxy stays installed. Its own folders and exe hash are remembered\n"
                                          "for when you add it again.");
            ImGui::Separator();
            ImGui::BeginDisabled(running);
            if (ImGui::Button("Remove completely")) {
                Defer([this, exe = g.exe, recycle = g.ownFolders && removeRecycleFolders_] { RemoveGame(exe, true, recycle); });
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndDisabled();
            if (running) ImGui::SetItemTooltip("Close the game first: its proxy is in use.");
            ImGui::TextColored(kDimColor, "Uninstalls the proxy and forgets its own folders and exe hash.");
            if (g.ownFolders) {
                // Two copies of the same build share games\<hash>: leave it for the other one.
                const fs::path own = settings_.FoldersFor(g.exe).first;
                const bool shared = settings_.SharesOwnFolders(g.exe);
                ImGui::BeginDisabled(shared || running);
                if (shared) removeRecycleFolders_ = false;
                ImGui::Checkbox("Also move its own mods and dumps to the Recycle Bin", &removeRecycleFolders_);
                ImGui::EndDisabled();
                ImGui::SetItemTooltip("%s", shared ? "Another game uses the same folders (it may be one you removed from the list)."
                                                   : ToUtf8(own.parent_path()).c_str());
            }
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
}

void App::DrawSettings() {
    bool changed = false;
    auto folderRow = [&](const char* label, fs::path& folder, const wchar_t* title) {
        ImGui::PushID(label);
        ImGui::TextUnformatted(label);
        char buf[1024];
        snprintf(buf, sizeof(buf), "%s", ToUtf8(folder).c_str());
        ImGui::SetNextItemWidth(-180);
        if (ImGui::InputText("##path", buf, sizeof(buf), ImGuiInputTextFlags_EnterReturnsTrue)) {
            folder = fs::path(Utf8ToWide(buf));
            changed = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Browse...")) {
            if (auto picked = PickFolder(hwnd_, title)) {
                folder = *picked;
                changed = true;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Open")) OpenWithShell(folder);
        ImGui::PopID();
    };

    ImGui::SeparatorText("Default folders");
    ImGui::TextColored(kDimColor, "Shared by every game without its own folders (Games tab -> Own folders).");
    folderRow("Load modded textures from", settings_.moddedFolder, L"Modded textures folder");
    folderRow("Save original (dumped) textures to", settings_.originalFolder, L"Original textures folder");

    ImGui::SeparatorText("Performance");
    changed |= ImGui::Checkbox("Low impact mode", &settings_.lowImpact);
    HelpMarker("For just playing with your mods. The proxy only replaces textures: no dumping and no Redump, "
               "and it reads your mods when the game starts instead of watching the folder, so nothing runs "
               "in the background while you play.\n\nMods you add or switch on or off apply the next time "
               "the game starts, or when you change a setting here.");

    ImGui::SeparatorText("Mod creator");
    ImGui::BeginDisabled(settings_.lowImpact);
    changed |= ImGui::Checkbox("Mod creator mode", &settings_.modCreatorMode);
    HelpMarker("Needed for dumping. Replacement works with or without it.");
    ImGui::BeginDisabled(!settings_.modCreatorMode);
    changed |= ImGui::Checkbox("Dump textures the game loads", &settings_.saveTextures);
    HelpMarker("Each texture is saved once as <hash>.dds into the original folder. "
               "Writing happens on a background thread so the game does not stall.");
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    if (settings_.lowImpact) ImGui::TextColored(kDimColor, "Dumping is off while Low impact mode is on.");

    ImGui::SeparatorText("Appearance");
    {
        // Applied live while dragging; saved once the colour is chosen.
        uint32_t& rgb = settings_.accentColor;
        float col[3] = {((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f};
        auto set = [&](uint32_t value, bool save) {
            rgb = value;
            ApplyAccentColor(rgb);
            if (save) settings_.Save();
        };
        ImGui::SetNextItemWidth(260);
        if (ImGui::ColorEdit3("Accent colour", col, ImGuiColorEditFlags_DisplayHex))
            set((uint32_t(col[0] * 255 + 0.5f) << 16) | (uint32_t(col[1] * 255 + 0.5f) << 8) | uint32_t(col[2] * 255 + 0.5f),
                false);
        if (ImGui::IsItemDeactivatedAfterEdit()) settings_.Save();

        struct Preset {
            const char* name;
            uint32_t rgb;
        };
        static constexpr Preset kPresets[] = {{"Blue", 0x4296FA},   {"Orange", 0xFF8C38}, {"Teal", 0x2EC4B6},
                                              {"Green", 0x4CB04F},  {"Purple", 0x9B6BFF}, {"Pink", 0xFF5FA2},
                                              {"Red", 0xE5484D},    {"Grey", 0x8A8F98}};
        for (const Preset& pr : kPresets) {
            ImGui::PushID(pr.name);
            const ImVec4 c(((pr.rgb >> 16) & 0xFF) / 255.0f, ((pr.rgb >> 8) & 0xFF) / 255.0f, (pr.rgb & 0xFF) / 255.0f, 1.0f);
            if (ImGui::ColorButton(pr.name, c, ImGuiColorEditFlags_NoTooltip, ImVec2(26, 26))) set(pr.rgb, true);
            ImGui::SetItemTooltip("%s", pr.name);
            ImGui::PopID();
            ImGui::SameLine();
        }
        ImGui::BeginDisabled(rgb == kDefaultAccent);
        if (ImGui::Button("Reset")) set(kDefaultAccent, true);
        ImGui::EndDisabled();
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Zoomed-in preview");
    ImGui::SameLine();
    bool zoomChanged = ImGui::RadioButton("Sharp pixels", settings_.sharpZoom) && !settings_.sharpZoom;
    ImGui::SameLine();
    zoomChanged |= ImGui::RadioButton("Smooth", !settings_.sharpZoom) && settings_.sharpZoom;
    if (zoomChanged) {
        settings_.sharpZoom = !settings_.sharpZoom;
        settings_.Save();
    }
    HelpMarker("How the details preview looks past 100% zoom. Sharp shows every texel as a hard-edged "
               "square, which is easiest for checking pixel-level edits. Smooth blends neighbouring texels.");
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Type a thumbnail size with");
    ImGui::SameLine();
    bool clickChanged = ImGui::RadioButton("Ctrl+click", !settings_.shiftClickToType) && settings_.shiftClickToType;
    ImGui::SameLine();
    clickChanged |= ImGui::RadioButton("Shift+click", settings_.shiftClickToType) && !settings_.shiftClickToType;
    if (clickChanged) {
        settings_.shiftClickToType = !settings_.shiftClickToType;
        settings_.Save();
    }
    HelpMarker("Clicking the Size slider with this key held opens a box to type an exact size in.");
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Middle click in the texture list");
    ImGui::SameLine();
    bool middleChanged = ImGui::RadioButton("Auto-scroll", !settings_.middleClickJumps) && settings_.middleClickJumps;
    ImGui::SameLine();
    middleChanged |= ImGui::RadioButton("Jump to selected", settings_.middleClickJumps) && !settings_.middleClickJumps;
    if (middleChanged) {
        settings_.middleClickJumps = !settings_.middleClickJumps;
        settings_.Save();
    }
    HelpMarker("Auto-scroll works like in a web browser: middle click, then move the mouse up or down to "
               "scroll, faster the further you go; click again to stop. Or hold the middle button, move, "
               "and let go to stop.\n\nJump to selected scrolls the list back to the texture shown in "
               "the details panel.");
    if (ImGui::Checkbox("Separate sorting for Mods and Dumps", &settings_.separateSort)) {
        if (settings_.separateSort) settings_.dumpSort = settings_.sort;  // Dumps start from what is set now
        settings_.Save();
    }
    HelpMarker("Sort by and its direction are set separately on the Mods and Dumps tabs. "
               "Off: both tabs are sorted the same way.");
    if (ImGui::Checkbox("Separate filters for Mods and Dumps", &settings_.separateFilters)) {
        if (settings_.separateFilters) settings_.dumpFilters = settings_.filters;  // Dumps start from what is set now
        settings_.Save();
    }
    HelpMarker("The Hide normal maps / depth / packed and Black Ice only boxes and the Formats dropdown are "
               "set separately on the Mods and Dumps tabs. Off: one set of filters for both. The search box is always shared.");

    ImGui::SeparatorText("Labels");
    ImGui::Text("%zu label(s) in %s", labels_.size(), ToUtf8(LabelsFile()).c_str());
    if (ImGui::Button("Import labels...")) ImportLabels();
    ImGui::SetItemTooltip("Merges a labels file (lines like 3123456789=Ash helmet; CSV also works).\n"
                          "Imported labels replace existing labels for the same hash.");
    ImGui::SameLine();
    if (ImGui::Button("Export labels...")) ExportLabels();
    ImGui::SameLine();
    if (ImGui::Button("Open labels file")) {
        if (!Exists(LabelsFile())) labels_.Save(LabelsFile());
        OpenWithShell(LabelsFile());
    }

    ImGui::SeparatorText("Logging");
    changed |= ImGui::Checkbox("Write dll.log in the game folder", &settings_.logEnabled);
    ImGui::BeginDisabled(!settings_.logEnabled);
    changed |= ImGui::Checkbox("Verbose (every texture load)", &settings_.logDebug);
    ImGui::EndDisabled();

    ImGui::Spacing();
    ImGui::TextColored(kDimColor, "Changes are saved immediately and reach a running game within a second.");
    if (changed) ApplySettings();
}

void App::DrawAbout() {
    ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x * 0.95f);
    ImGui::SetWindowFontScale(1.6f);
    ImGui::TextUnformatted("ReResorep " RERESOREP_VERSION_STRING);
    ImGui::SetWindowFontScale(1.0f);
    ImGui::TextColored(kDimColor, "Texture replacer and texture manager for Direct3D 11 games.");
    ImGui::Text("Made by %s.", RERESOREP_AUTHOR);
    ImGui::Spacing();

    DrawUpdates();
    ImGui::Spacing();

    ImGui::SeparatorText("Licence");
    ImGui::Text("Copyright (C) 2026 %s", RERESOREP_AUTHOR);
    ImGui::TextWrapped("ReResorep is free software under the GNU General Public License, version 3. You can "
                       "use, change and share it; if you share a changed version, it has to stay under the GPL "
                       "with its source available. It comes with no warranty: see the licence for details.");
    if (ImGui::SmallButton("Source code on GitHub")) OpenWithShell(L"https://github.com/SosigDOGGO/ReResorep");
    ImGui::SameLine();
    static bool showGpl = false;
    if (ImGui::SmallButton(showGpl ? "Hide licence##gpl" : "Show licence##gpl")) showGpl = !showGpl;
    if (showGpl) {
        ImGui::BeginChild("gpl", ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * 16), ImGuiChildFlags_Borders);
        ImGui::TextUnformatted(kGplLicense);
        ImGui::EndChild();
    }
    ImGui::Spacing();

    ImGui::SeparatorText("Credits");
    ImGui::TextColored(AccentTextColor(), "Resorep by RayWing (2017)");
    ImGui::TextWrapped("The original DirectX 11 texture replacer that ReResorep rewrites. To keep existing "
                       "mods working, the texture hash, the encrypted resorep1 pack format and the mod file "
                       "naming follow Resorep exactly; they were worked out from its proxy DLL. "
                       "No Resorep code is included.");
    if (ImGui::SmallButton("Original Resorep release thread"))
        OpenWithShell(L"https://www.undertow.club/threads/tool-resorep-directx-11-texture-replacer.8402/");
    ImGui::Spacing();

    ImGui::TextColored(AccentTextColor(), "Dear ImGui by Omar Cornut and contributors");
    ImGui::TextWrapped("The user interface library. MIT License.");
    if (ImGui::SmallButton("Dear ImGui on GitHub")) OpenWithShell(L"https://github.com/ocornut/imgui");
    ImGui::SameLine();
    static bool showLicense = false;
    if (ImGui::SmallButton(showLicense ? "Hide licence" : "Show licence")) showLicense = !showLicense;
    if (showLicense) {
        ImGui::BeginChild("license", ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * 12), ImGuiChildFlags_Borders);
        ImGui::TextUnformatted(kImGuiLicense);
        ImGui::EndChild();
    }
    ImGui::Spacing();

    ImGui::TextColored(AccentTextColor(), "Blowfish by Bruce Schneier");
    ImGui::TextWrapped("The public-domain cipher used by Resorep's encrypted packs. Its tables are computed "
                       "from the digits of pi (tools/gen_blowfish_tables.py).");

    ImGui::SeparatorText("Please note");
    ImGui::TextWrapped("ReResorep is a fan-made modding tool and is not affiliated with or endorsed by Ubisoft "
                       "or any game developer. Games with anti-cheat (for example BattlEye in online Rainbow Six "
                       "Siege) can detect a replacement d3d11.dll and ban accounts: use it for offline and modded "
                       "setups only.");
    ImGui::PopTextWrapPos();
}

void App::DrawUpdates() {
    using Step = Updater::Step;
    updater_.Poll();
    ImGui::SeparatorText("Updates");
    if (updater_.updatedThisStart() && updater_.step() == Step::Idle)
        ImGui::TextColored(kOkColor, "Updated to %s.", RERESOREP_VERSION_STRING);

    const Step step = updater_.step();
    if (step == Step::Idle || step == Step::Checking || step == Step::UpToDate || step == Step::Failed) {
        ImGui::BeginDisabled(step == Step::Checking);
        if (ImGui::Button(step == Step::Checking ? "Checking..." : "Check for updates")) updater_.Check();
        ImGui::EndDisabled();
        if (step == Step::UpToDate) {
            ImGui::SameLine();
            ImGui::TextColored(kOkColor, "You have the latest version.");
        } else if (step == Step::Failed) {
            ImGui::TextColored(kErrorColor, "%s", updater_.error().c_str());
        }
    } else {
        const ReleaseInfo& release = updater_.release();
        ImGui::TextColored(AccentTextColor(), "%s is available (you have %s).", release.title.c_str(),
                           RERESOREP_VERSION_STRING);
        ImGui::SameLine();
        if (ImGui::SmallButton("Release page")) OpenWithShell(Utf8ToWide(release.pageUrl));
        if (!release.notes.empty()) {
            ImGui::BeginChild("releasenotes", ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * 10),
                              ImGuiChildFlags_Borders);
            ImGui::TextWrapped("%s", NotesForDisplay(release.notes).c_str());
            ImGui::EndChild();
        }
        if (step == Step::Available) {
            if (ImGui::Button("Download update")) updater_.Download();
            if (!updater_.error().empty()) ImGui::TextColored(kErrorColor, "%s", updater_.error().c_str());
        } else if (step == Step::Downloading) {
            const DownloadProgress& p = updater_.progress();
            const uint64_t got = p.received, total = p.total;
            const std::string label = total ? FormatBytes(got) + " of " + FormatBytes(total) : FormatBytes(got);
            ImGui::ProgressBar(total ? float(double(got) / double(total)) : 0.0f, ImVec2(320, 0), label.c_str());
            ImGui::SameLine();
            if (ImGui::Button("Cancel##update")) updater_.Cancel();
        } else if (step == Step::Ready) {
            const StagedUpdate& staged = updater_.staged();
            ImGui::TextColored(kOkColor, "Downloaded and checked. Restart ReResorep to finish.");
            if (staged.proxy.empty()) {
                ImGui::TextColored(kDimColor, "The proxy (proxy\\d3d11.dll) is unchanged, so your games need nothing new.");
            } else {
                ImGui::TextColored(kErrorColor, "This update has a new proxy%s.",
                                   staged.proxyVersion.empty() ? "" : (" (" + staged.proxyVersion + ")").c_str());
                ImGui::TextWrapped("Games keep their current d3d11.dll until you press Update for each game in the Games tab. "
                                   "Mods that allow the proxy by its hash need the new one: SHA-256 %s",
                                   staged.proxySha256.c_str());
            }
            if (ImGui::Button("Restart and update")) {
                const std::string error = updater_.InstallAndRestart();
                if (error.empty()) PostMessageW(hwnd_, WM_CLOSE, 0, 0);
                else Status(error, "");
            }
            if (!updater_.error().empty()) ImGui::TextColored(kErrorColor, "%s", updater_.error().c_str());
        }
    }
    ImGui::TextColored(kDimColor, "ReResorep only goes online when you press Check for updates, and then only to "
                                  "GitHub. Your settings, mods and labels are kept.");
}

void App::DrawImportPopup() {
    if (openImportPopup_) {
        ImGui::OpenPopup("Import textures");
        openImportPopup_ = false;
    }
    ImGui::SetNextWindowSize(ImVec2(560, 0), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Import textures", nullptr, ImGuiWindowFlags_NoSavedSettings)) return;

    ImGui::TextWrapped("These files are not named after a texture hash. Enter the hash of the game texture "
                       "each one replaces (you can see hashes in the Dumps tab).");
    ImGui::Spacing();
    bool allValid = true;
    for (size_t i = 0; i < pendingImports_.size(); ++i) {
        PendingImport& p = pendingImports_[i];
        ImGui::PushID(int(i));
        ImGui::SeparatorText(ToUtf8(p.source.filename()).c_str());
        ImGui::SetNextItemWidth(140);
        ImGui::InputTextWithHint("Hash", "e.g. 3123456789", p.hash, sizeof(p.hash), ImGuiInputTextFlags_CharsDecimal);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(200);
        ImGui::InputText("Name (optional)", p.label, sizeof(p.label));
        if (!ParseHash(p.hash)) {
            allValid = false;
            ImGui::TextColored(kErrorColor, "Enter a number between 0 and 4294967295");
        }
        ImGui::PopID();
    }
    ImGui::Spacing();
    ImGui::BeginDisabled(!allValid);
    if (ImGui::Button("Import", ImVec2(120, 0))) {
        std::string error;
        for (const PendingImport& p : pendingImports_) {
            const uint32_t hash = *ParseHash(p.hash);
            const std::string e = library_.Import(p.source, hash, p.label, true, p.relDir);
            thumbs_.Invalidate(library_.TargetPath(hash, p.label, p.relDir));
            if (!e.empty()) error = e;
        }
        Status(error, "Imported " + std::to_string(pendingImports_.size()) + " texture(s)");
        pendingImports_.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120, 0))) {
        pendingImports_.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void App::DrawStatusBar() {
    ImGui::Separator();
    const auto age = std::chrono::steady_clock::now() - statusTime_;
    if (!statusText_.empty() && (statusIsError_ || age < std::chrono::seconds(8))) {
        ImGui::TextColored(statusIsError_ ? kErrorColor : kOkColor, "%s", statusText_.c_str());
    } else {
        ImGui::TextColored(kDimColor, "%zu mods (%zu enabled) | %zu dumps | modded folder: %s", library_.mods().size(),
                           size_t(std::count_if(library_.mods().begin(), library_.mods().end(),
                                                [](const TextureFile& f) { return f.enabled; })),
                           library_.dumps().size(), ToUtf8(library_.moddedFolder()).c_str());
    }
    if (const size_t pending = analyzer_->Pending()) {
        ImGui::SameLine();
        ImGui::TextColored(AccentTextColor(), "| detecting texture types: %zu left", pending);
    }
}

}  // namespace resorep::ui
