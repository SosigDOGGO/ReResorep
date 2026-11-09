#pragma once
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <imgui.h>

#include "analyzer.h"
#include "games.h"
#include "integrity.h"
#include "labels.h"
#include "mod_package.h"
#include "library.h"
#include "thumbnails.h"
#include "tint.h"
#include "updater.h"
#include "win_util.h"

namespace resorep::ui {

class App {
public:
    App(HWND hwnd, ID3D11Device* device);
    void Frame();  // builds one ImGui frame
    void OnDropFiles(const std::vector<std::filesystem::path>& files);

private:
    enum class Tab { Mods, Dumps, Games, Settings, About };
    struct Selection {
        std::filesystem::path path;
        bool isMod = false;
    };
    enum class ModView { All, Loose, Folder };
    struct PendingImport {
        std::filesystem::path source;
        std::wstring relDir;
        char hash[16] = {};
        char label[64] = {};
    };

    void DrawToolbarMods();
    void DrawSidebar();
    void DrawFolderNode(size_t& index);
    void DrawFolderPopups();
    // These act on every texture given (the grid's selection, or the one right-clicked).
    void DrawMoveMenu(const std::vector<const TextureFile*>& files, bool copy);  // "Move to" or "Copy to"
    void DrawMoveItems(const std::vector<const TextureFile*>& files, bool copy);  // the menu's entries
    void MoveOrCopy(const std::vector<TextureFile>& files, const std::wstring& rel, bool copy);
    void DrawAddToModMenu(const std::vector<const TextureFile*>& dumps);
    void DrawAddToModItems(const std::vector<const TextureFile*>& dumps);  // the menu's entries, also used by the details button
    void AddDumpsToMod(const std::vector<TextureFile>& dumps, const std::wstring& rel);
    void SetEnabled(const std::vector<TextureFile>& mods, bool on);
    void Recycle(const std::vector<TextureFile>& files);
    bool InView(const TextureFile& file) const;
    std::wstring ImportDir() const { return modView_ == ModView::Folder ? viewFolder_ : std::wstring(); }
    void DrawToolbarDumps();
    void DrawViewOptions(bool isMod);
    bool DrawFormatFilter(TextureFilters& filters, const std::vector<TextureFile>& files);  // true when changed
    void SortVisible(std::vector<const TextureFile*>& files, bool isMod) const;
    // Draws a texture with the chosen preview rotation/mirroring, fitted into a box.
    // extraTurns/extraMirror add to the global setting (used by the details preview's own buttons).
    void DrawPreview(ImDrawList* dl, ID3D11ShaderResourceView* srv, uint32_t w, uint32_t h, ImVec2 boxMin,
                     ImVec2 boxSize, ImU32 tint, int extraTurns = 0, bool extraMirror = false) const;
    ImVec2 PreviewSize(uint32_t w, uint32_t h, float box, int extraTurns = 0) const;
    void DrawGrid(const std::vector<TextureFile>& files, bool isMod);
    void DrawCard(const TextureFile& file, bool isMod, float size);
    // Middle click in the grid: Chrome-style auto-scroll, or back to the selected texture.
    // `selectedY` is where the selected texture's row starts (scroll space), or < 0 if not shown.
    void HandleGridMiddleClick(float selectedY, float rowHeight);
    void DrawCardMenu(const TextureFile& file, bool isMod, const TextureInfo* info);  // right-click on one
    void DrawSelectionMenu(bool isMod);  // right-click on one of several selected
    void DrawDetails();
    void DrawSelectionDetails(const std::vector<const TextureFile*>& files);  // when several are selected
    void DrawGames();
    void DrawSettings();
    void DrawAbout();
    void DrawUpdates();  // the About tab's Updates section
    void DrawImportPopup();
    void DrawStatusBar();
    void DrawDeleteDumpsPopup();
    void Redump();
    void DrawSimilarWindow();
    void DrawLabelEditor(const TextureFile& file, float width);
    void DrawLabelPopup();

    // Labels and similarity search.
    enum class SimilarMode { Normals, Colors, Any };
    struct SimilarResult {
        std::filesystem::path path;
        bool isMod;
        uint32_t hash;
        float score;
        TextureKind kind;
        uint32_t width, height;
        int64_t modified;
    };
    struct SimilarSearch {
        TextureFile source;
        bool sourceIsMod = false;
        SimilarMode mode = SimilarMode::Normals;
        bool computed = false;
        size_t notAnalysed = 0;
        std::vector<SimilarResult> results;
    };
    const std::string& DisplayLabel(const TextureFile& file) const;
    bool MatchesFilter(const TextureFile& file) const;
    size_t CountMatches(const std::vector<TextureFile>& files) const;
    void DrawSearch(float y);
    TextureKind KindOf(const TextureFile& file) { return analyzer_->Kind(file.path, file.modified, file.size); }
    bool IsBlackIce(const TextureFile& file);
    void SetLabel(uint32_t hash, const std::string& label);
    void ImportLabels();
    void ExportLabels();
    void OpenSimilar(const TextureFile& file, bool isMod, SimilarMode mode);
    void ComputeSimilar();
    void QueueAnalysis();

    // Mod packages (.zip).
    struct ExportDialog {
        std::wstring rel;
        std::string name;
        size_t textures = 0;
        bool includeTurnedOff = true;
        bool includeLabels = true;
    };
    struct ImportModDialog {
        std::filesystem::path zip;
        PackageInfo info;
        char name[96] = {};
        bool merge = false;
    };
    void BeginImportMod(const std::filesystem::path& zip);
    void DrawPackagePopups();

    void ImportFiles(const std::vector<std::filesystem::path>& files, const std::wstring& relDir);
    void ReplaceDump(uint32_t hash);
    void ExportAsPng(const TextureFile& file);  // asks where, then writes the PNG
    // Tint: a dialog with a live preview, then the mods are rewritten in the background.
    struct TintDialog {
        std::vector<TextureFile> files;  // what will be tinted
        std::vector<TextureFile> maps;   // normal and packed maps among those picked, left out unless asked
        bool includeMaps = false;
        size_t unsupported = 0;          // can't be tinted at all (format, encrypted)
        std::string unsupportedReason;   // the first one's reason
        TintSettings settings;
        std::vector<float> previewPixels;  // the first texture, small, as decoded
        uint32_t previewW = 0, previewH = 0;
        std::string previewError;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> before, after;
        bool previewDirty = true;
    };
    struct TintResult {
        size_t done = 0, failed = 0;
        std::string error;  // the first failure
    };
    void OpenTint(const std::vector<const TextureFile*>& mods);
    void DrawTintPopup();
    void StartTint();
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> UploadPreview(const std::vector<float>& rgba, uint32_t w,
                                                                   uint32_t h);
    void ToggleEnabled(const TextureFile& file);
    void Defer(std::function<void()> action) { deferred_.push_back(std::move(action)); }
    void ApplySettings();
    void RefreshGames();
    // Profiles: the Mods/Dumps tabs show the Default folders or one game's own folders.
    void ApplyProfile();                   // points the library at the picked profile's folders
    const GameState* ProfileGame() const;  // the picked game, nullptr for Default
    void DrawProfilePicker();
    void SetOwnFolders(const std::filesystem::path& exe, bool own);
    // forget: also uninstall the proxy and forget the game's hash and own folders;
    // recycleFolders: and move its own folders to the Recycle Bin.
    void RemoveGame(const std::filesystem::path& exe, bool forget, bool recycleFolders);
    void SetModGame(const std::wstring& rel, const GameState* game);  // nullptr = any game
    std::string ModGameName(const std::string& sha) const;            // for tooltips and menus
    void StartHashing();                   // works out missing exe hashes in the background
    // After a game's exe changed: move mods marked for its old hash to the new one, or not.
    void ResolveExeChange(const std::filesystem::path& exe, bool moveMarks);
    void Status(const std::string& error, const std::string& success);
    const TextureFile* FindSelected() const;
    // Multi-selection: picked_ holds every texture selected in the grid, selection_ the one the
    // details panel shows (one of them).
    std::vector<const TextureFile*> Picked() const;  // in library order
    void Select(const std::filesystem::path& path, bool isMod);  // just this one
    void ClearSelection();
    void PruneSelection();  // drops what is no longer in the library

    HWND hwnd_;
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    ThumbnailCache thumbs_;
    Library library_;
    ManagerSettings settings_;
    Updater updater_;  // only goes online when the user presses Check for updates
    std::vector<GameState> games_;
    resorep::SealState bundledSeal_ = resorep::SealState::Unreadable;  // proxy\d3d11.dll, checked in RefreshGames
    LabelStore labels_;
    std::unique_ptr<TextureAnalyzer> analyzer_;
    std::optional<SimilarSearch> similar_;
    char labelEdit_[128] = {};
    std::optional<uint32_t> labelEditHash_;
    // Right-click > Edit label: a small dialog for that hash.
    uint32_t labelDialogHash_ = 0;
    char labelDialog_[128] = {};
    bool openLabelPopup_ = false;
    struct PreviewTweak {
        int quarterTurns = 0;  // clockwise
        bool mirror = false;
        float zoom = 1.0f;                // 1 = whole texture fits the preview
        ImVec2 centre = {0.5f, 0.5f};     // point of the picture (0..1 across it) at the middle of the view
    };
    std::unordered_map<uint32_t, PreviewTweak> previewTweaks_;  // per hash, this session only
    Microsoft::WRL::ComPtr<ID3D11SamplerState> pointSampler_;   // for the sharp zoomed preview
    std::optional<ExportDialog> exportDialog_;
    std::optional<ImportModDialog> importModDialog_;
    bool openExportPopup_ = false;
    bool openImportModPopup_ = false;
    bool openDeleteDumpsPopup_ = false;
    std::optional<TintDialog> tintDialog_;
    TintSettings lastTint_;  // the dialog opens with the colour and sliders used last
    bool openTintPopup_ = false;
    std::future<TintResult> tintJob_;
    std::shared_ptr<std::atomic<size_t>> tintProgress_;  // textures finished so far
    size_t tintTotal_ = 0;
    std::future<size_t> deleteDumpsJob_;  // result: how many dumps could not be deleted
    std::shared_ptr<std::atomic<size_t>> deleteProgress_;  // files processed so far
    // Games tab: running games and the Close button's background job.
    static constexpr DWORD kCloseGraceMs = 10000;
    std::unordered_map<std::wstring, bool> gameRunning_;
    std::chrono::steady_clock::time_point lastRunningCheck_;
    std::future<CloseResult> closeJob_;
    std::filesystem::path closingExe_;
    bool removeRecycleFolders_ = false;  // the Remove popup's "also move its folders" box
    size_t deleteTotal_ = 0;
    std::future<std::vector<std::pair<std::filesystem::path, ExeHash>>> hashJob_;

    Tab tab_ = Tab::Mods;
    std::optional<Selection> selection_;
    std::unordered_set<std::wstring> picked_;  // paths (native strings) of the selected textures
    char filter_[64] = {};          // the search field in the tab row; applies to Mods and Dumps
    std::optional<Tab> selectTab_;  // switch to this tab on the next frame
    bool showDisabled_ = true;
    bool hideModdedDumps_ = false;
    float thumbSize_ = 128.0f;
    struct AutoScroll {
        bool active = false;
        bool held = true;       // the middle button is still down from the click that started it
        bool dragged = false;   // moved away while held: releasing stops it (else it stays on)
        ImVec2 anchor;
        int frame = -1;         // last frame it ran; a gap (the grid wasn't drawn) stops it
    } autoScroll_;
    int swallowButtons_ = 0;  // bit per mouse button: its click stopped auto-scroll, so the grid ignores it until released
    ModView modView_ = ModView::All;
    std::wstring viewFolder_;
    std::wstring newFolderParent_;
    std::vector<TextureFile> addAfterCreate_;  // dumps to add once the "New mod" popup creates it
    char newFolderName_[64] = {};
    bool openNewFolderPopup_ = false;
    std::wstring deleteFolder_;
    bool openDeleteFolderPopup_ = false;
    std::wstring renameFolder_;
    char renameName_[64] = {};
    bool openRenameFolderPopup_ = false;
    std::vector<PendingImport> pendingImports_;
    bool openImportPopup_ = false;
    std::vector<std::function<void()>> deferred_;

    std::string statusText_;
    bool statusIsError_ = false;
    std::chrono::steady_clock::time_point statusTime_;
};

}  // namespace resorep::ui
