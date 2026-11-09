#pragma once
// Updates ReResorep from its latest GitHub release, only when the user asks: nothing here runs
// on its own, and nothing is sent but the HTTPS requests themselves.
//
//   1. Check:    ask GitHub for the latest release; compare its version with ours.
//   2. Download: fetch the release zip and its .sha256.txt; check the zip's hash, the
//                SHA256SUMS.txt inside it, the new files' seals and the exe's version; unpack
//                them beside the current ones as ReResorep.new.exe and proxy\d3d11.new.dll.
//   3. Apply:    rename the running exe to ReResorep.old.exe (Windows allows renaming a running
//                exe, not overwriting it), move the new one in, restart. The next start deletes
//                the .old files.
//
// The proxy next to ReResorep.exe is replaced only when the release's copy differs. Proxies in
// game folders are never touched: the Games tab shows them as outdated and the user installs
// the new one there, so a proxy hash that mods whitelist never changes behind their back.
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace resorep::ui {

struct Version {
    int major = 0, minor = 0, patch = 0;
    std::string ToString() const;
};
std::optional<Version> ParseVersion(const std::string& text);  // "2.5.4" or "v2.5.4"
int CompareVersions(const Version& a, const Version& b);          // <0, 0, >0

struct ReleaseInfo {
    Version version;
    std::string title;        // "ReResorep 2.5.4"
    std::string notes;        // the release notes (Markdown)
    std::string pageUrl;      // the release on GitHub
    std::string zipName;      // ReResorep-<version>-win64.zip
    std::string zipUrl;
    std::string checksumUrl;  // <zip>.sha256.txt
};

// Reads GitHub's "latest release" JSON. Download URLs must point at this repository's release
// assets. Returns an error message, or "".
std::string ParseLatestRelease(const std::string& json, ReleaseInfo& out);
// The SHA-256 from a "<hex>  <file name>" line, lower-case; "" if there isn't one for `fileName`.
std::string ParseChecksumFile(const std::string& text, const std::string& fileName);
// Release notes without Markdown marks, for plain text.
std::string NotesForDisplay(const std::string& markdown);

struct DownloadProgress {
    std::atomic<uint64_t> received{0};
    std::atomic<uint64_t> total{0};  // 0 while unknown
    std::atomic<bool> cancel{false};
};
// HTTPS GET (WinHTTP; follows GitHub's redirect to its download host). Error message or "".
std::string HttpGet(const std::string& url, std::vector<uint8_t>& body, uint64_t maxBytes,
                    DownloadProgress* progress = nullptr, bool githubApi = false);

struct StagedUpdate {
    Version version;
    std::filesystem::path exe;        // ReResorep.new.exe, beside the running exe
    std::filesystem::path proxy;      // proxy\d3d11.new.dll, or empty when the proxy is unchanged
    std::string proxyVersion;         // version of the new proxy, when it changes
    std::string proxySha256;          // its SHA-256, when it changes
};
// Checks the downloaded zip (its SHA-256 must be `zipSha256`) and unpacks the update into
// `installDir`, where `exeName` is the running exe's file name. Error message, or "".
std::string StageUpdate(const std::filesystem::path& zip, const std::string& zipSha256, const Version& version,
                        const std::filesystem::path& installDir, const std::wstring& exeName, StagedUpdate& out);
// Moves the staged files in place of the current ones, putting everything back on failure.
std::string ApplyUpdate(const StagedUpdate& staged, const std::filesystem::path& installDir,
                        const std::wstring& exeName);
// Deletes what an update leaves behind (.old and unused .new files). True when nothing is left.
bool CleanUpAfterUpdate(const std::filesystem::path& installDir, const std::wstring& exeName);
// Starts `exe` from its own folder. Error message or "".
std::string StartProgram(const std::filesystem::path& exe);
// After Updater::InstallAndRestart: starts the new exe. main() calls it once the window is
// closed, so the two versions never run side by side. Error message, or "" (also when there
// is nothing to start).
std::string RunPendingRestart();

std::string Sha256Hex(const uint8_t* data, size_t size);

// The update steps for the About tab, run off the UI thread one at a time.
class Updater {
public:
    enum class Step { Idle, Checking, UpToDate, Available, Downloading, Ready, Failed };

    Updater();
    ~Updater();  // cancels a running download and waits for it

    void Check();     // Idle/UpToDate/Available/Failed -> Checking
    void Download();  // Available -> Downloading -> Ready
    void Poll();      // call every frame the About tab is shown
    // Ready: swaps the files in. Error message, or "": then close the app, and main() starts
    // the new version (RunPendingRestart).
    std::string InstallAndRestart();
    void Cancel();    // stops a download and goes back to Available

    Step step() const { return step_; }
    const ReleaseInfo& release() const { return release_; }
    const StagedUpdate& staged() const { return staged_; }
    const std::string& error() const { return error_; }
    const DownloadProgress& progress() const { return *progress_; }
    bool updatedThisStart() const { return updatedThisStart_; }

private:
    struct CheckResult {
        std::string error;
        ReleaseInfo release;
    };
    struct DownloadResult {
        std::string error;
        StagedUpdate staged;
    };

    Step step_ = Step::Idle;
    ReleaseInfo release_;
    StagedUpdate staged_;
    std::string error_;
    bool updatedThisStart_ = false;
    std::shared_ptr<DownloadProgress> progress_ = std::make_shared<DownloadProgress>();
    std::future<CheckResult> checkJob_;
    std::future<DownloadResult> downloadJob_;
    std::future<void> cleanupJob_;
    std::shared_ptr<std::atomic<bool>> stopCleanup_ = std::make_shared<std::atomic<bool>>(false);
};

}  // namespace resorep::ui
