// Checks for the manager's per-game logic: which mod wins per Siege build (it must match the
// proxy's index), the saved game settings, exe hashing and moving mod marks.
// Run: build\Release\manager_test.exe
#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "games.h"
#include "library.h"
#include "mod_state.h"
#include "settings.h"
#include "texture_index.h"
#include "updater.h"
#include "version.h"
#include "win_util.h"
#include "zip.h"

namespace fs = std::filesystem;
using namespace resorep;
using namespace resorep::ui;

static int g_failures = 0;
#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                              \
        }                                                              \
    } while (0)

static fs::path TempDir(const wchar_t* name) {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    const fs::path dir = fs::path(tmp) / name;
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

static void Touch(const fs::path& p, const std::string& text = "x") {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

static std::wstring ToUpperForTest(std::wstring s) {
    for (wchar_t& c : s) c = wchar_t(towupper(c));
    return s;
}

static const TextureFile* Find(const Library& lib, const wchar_t* name) {
    for (const TextureFile& m : lib.mods())
        if (m.path.filename() == name) return &m;
    return nullptr;
}

// Several mods replace hash 100: one for any game, two marked for build A, one for build B.
static void TestWinnersPerBuild() {
    const fs::path root = TempDir(L"reresorep_manager_test");
    const fs::path modded = root / L"modded", original = root / L"original";
    const std::string buildA(64, 'a'), buildB(64, 'b');
    Touch(modded / L"Shared/100~shared.dds");
    Touch(modded / L"A1/100~a1.dds");
    Touch(modded / L"A2/100~a2.dds");
    Touch(modded / L"B/100~b.dds");
    WriteModGame(modded / L"A1", buildA, "");
    WriteModGame(modded / L"A2", buildA, "");
    WriteModGame(modded / L"B", buildB, "");

    // Default folders: every build counts. "a1" sorts before "shared", which sorts after "b".
    Library lib;
    lib.SetFolders(modded, original);
    CHECK(lib.winners().size() == 3);  // "", A, B
    CHECK(Find(lib, L"100~a1.dds") && !Find(lib, L"100~a1.dds")->unused);    // wins in A
    CHECK(Find(lib, L"100~a2.dds") && Find(lib, L"100~a2.dds")->unused);     // loses to a1 in A
    CHECK(Find(lib, L"100~b.dds") && !Find(lib, L"100~b.dds")->unused);      // wins in B
    CHECK(Find(lib, L"100~shared.dds") && !Find(lib, L"100~shared.dds")->unused);  // wins in unmarked builds
    CHECK(lib.ActiveMod(100) && lib.ActiveMod(100)->filename() == L"100~shared.dds");
    for (const TextureFile& m : lib.mods()) CHECK(!m.otherBuild);

    // Each build's winner is what that game's proxy would load.
    for (const auto& [build, winners] : lib.winners()) {
        TextureIndex index;
        index.Scan(modded, build.empty() ? std::string(64, 'f') : build);
        CHECK(index.FindExact(100) && winners.count(100) && *index.FindExact(100) == winners.at(100));
    }

    // As build A's own folders: B's mod is for another build, the shared one loses to a1.
    lib.SetGame(buildA);
    CHECK(lib.winners().size() == 1);
    CHECK(Find(lib, L"100~b.dds")->otherBuild && !Find(lib, L"100~b.dds")->unused);
    CHECK(Find(lib, L"100~shared.dds")->unused && !Find(lib, L"100~a1.dds")->unused);
    CHECK(lib.ActiveMod(100) && lib.ActiveMod(100)->filename() == L"100~a1.dds");

    // A turned-off mod is neither the winner nor "unused".
    DisabledSet off;
    off.Set(L"A1", true);
    off.Save(modded);
    lib.Refresh();
    CHECK(!Find(lib, L"100~a1.dds")->unused && Find(lib, L"100~a1.dds")->enabled == false);
    CHECK(lib.ActiveMod(100) && lib.ActiveMod(100)->filename() == L"100~a2.dds");

    // Moving marks after a patched exe: only mods marked for the old hash change.
    const std::string patched(64, 'c');
    CHECK(RemarkMods(modded, buildA, patched, "Patched") == 2);
    CHECK(ReadModGame(modded / L"A1") == patched && ReadModGame(modded / L"B") == buildB);
    CHECK(ReadModGame(modded / L"Shared").empty());
    fs::remove_all(root);
}

static void TestSettingsAndHashing() {
    const fs::path root = TempDir(L"reresorep_manager_settings_test");
    const fs::path exe = root / L"Y9S2" / L"RainbowSix.exe";
    Touch(exe, "abc");
    const ExeHash h = HashExe(exe);
    // SHA-256("abc")
    CHECK(h.sha256 == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(h.size == 3);

    const fs::path iniFile = ManagerSettings::File();
    const bool hadIni = fs::exists(iniFile);
    CHECK(!hadIni);  // the test runs from build output, never from a real install
    if (hadIni) return;

    ManagerSettings s;
    s.moddedFolder = root / L"modded";
    s.originalFolder = root / L"original";
    s.games = {exe};
    CHECK(!s.ExeSha(exe).size() && s.NeedsHash(exe));
    s.exeHashes[GameKey(exe)] = h;
    CHECK(!s.NeedsHash(exe));
    CHECK(s.FoldersFor(exe).first == s.moddedFolder);  // Default until Own folders is ticked

    s.ownFolders[GameKey(exe)] = h.sha256.substr(0, 16);
    s.profile = exe;
    s.filters.hiddenFormats = {"BC4", "BC5"};
    s.middleClickJumps = true;
    CHECK(s.FoldersFor(exe).first == GameFolder(h.sha256) / L"modded");
    CHECK(GameFolder(h.sha256).filename() == L"ba7816bf8f01cfea");

    // The exe is patched: a new hash, but the game keeps its folder.
    ExeHash patched = h;
    patched.sha256 = std::string(64, 'd');
    patched.previous = h.sha256;
    s.exeHashes[GameKey(exe)] = patched;
    CHECK(s.FoldersFor(exe).first == GameFolder(h.sha256) / L"modded");
    CHECK(s.Save());

    ManagerSettings loaded;
    loaded.Load();
    CHECK(loaded.games.size() == 1 && GameKey(loaded.games[0]) == GameKey(exe));
    CHECK(loaded.ExeSha(exe) == patched.sha256);
    CHECK(loaded.exeHashes[GameKey(exe)].previous == h.sha256);
    CHECK(!loaded.NeedsHash(exe));
    CHECK(loaded.UsesOwnFolders(exe) && loaded.FoldersFor(exe) == s.FoldersFor(exe));
    CHECK(GameKey(loaded.profile) == GameKey(exe));
    CHECK((loaded.filters.hiddenFormats == std::set<std::string>{"BC4", "BC5"}));
    CHECK(loaded.dumpFilters.hiddenFormats.empty());
    CHECK(loaded.middleClickJumps);
    // Path keys ignore case.
    CHECK(loaded.UsesOwnFolders(fs::path(ToUpperForTest(exe.wstring()))));

    // The proxy's settings carry the game's own folders and its current hash.
    loaded.modCreatorMode = loaded.saveTextures = loaded.lowImpact = true;
    CHECK(!loaded.Dumping());  // low impact mode wins
    CHECK(WriteGameSettings(exe, loaded).empty());
    IniFile dll;
    CHECK(dll.Load(exe.parent_path() / kDllSettingsFile));
    CHECK(dll.GetBool(keys::kLowImpact, false));
    CHECK(!dll.GetBool(keys::kSaveTextures, true));  // older proxies don't know low_impact_mode
    loaded.lowImpact = false;
    CHECK(dll.GetOr(keys::kGameSha256, "") == patched.sha256);
    CHECK(fs::path(Utf8ToWide(dll.GetOr(keys::kModdedFolder, ""))) == GameFolder(h.sha256) / L"modded");
    CHECK(fs::is_directory(GameFolder(h.sha256) / L"original"));

    // Removed from the list and added back: it still has its own folders and its hash.
    loaded.RemoveGame(exe, false);
    CHECK(loaded.games.empty() && loaded.profile.empty());
    CHECK(loaded.Save());
    ManagerSettings removed;
    removed.Load();
    CHECK(removed.games.empty());
    removed.games = {exe};
    CHECK(removed.UsesOwnFolders(exe) && removed.FoldersFor(exe) == s.FoldersFor(exe));
    CHECK(removed.ExeSha(exe) == patched.sha256 && removed.exeHashes[GameKey(exe)].previous == h.sha256);

    // A second copy of the same build shares its folders, even while the first is off the list.
    const fs::path copy = root / L"Copy" / L"RainbowSix.exe";
    removed.ownFolders[GameKey(copy)] = h.sha256.substr(0, 16);
    removed.RemoveGame(exe, false);
    CHECK(removed.SharesOwnFolders(copy) && removed.SharesOwnFolders(exe));
    removed.ownFolders.erase(GameKey(copy));
    CHECK(!removed.SharesOwnFolders(exe));
    removed.games = {exe};

    // Removed completely: added back, it starts over on the Default folders.
    removed.RemoveGame(fs::path(ToUpperForTest(exe.wstring())), true);
    CHECK(removed.Save());
    ManagerSettings forgotten;
    forgotten.Load();
    forgotten.games = {exe};
    CHECK(!forgotten.UsesOwnFolders(exe) && forgotten.FoldersFor(exe).first == s.moddedFolder);
    CHECK(forgotten.ExeSha(exe).empty() && forgotten.NeedsHash(exe));

    fs::remove(iniFile);
    fs::remove_all(GameFolder(h.sha256).parent_path());
    fs::remove_all(root);
}

// Changes to several selected textures: saved and rescanned once, at the end of the batch.
static void TestBatch() {
    const fs::path root = TempDir(L"reresorep_batch_test");
    const fs::path modded = root / L"modded", original = root / L"original";
    Touch(modded / L"201.dds");
    Touch(modded / L"202.dds");
    Touch(modded / L"203.dds");
    fs::create_directories(modded / L"Mod");
    Library lib;
    lib.SetFolders(modded, original);
    const std::vector<TextureFile> mods = lib.mods();
    CHECK(mods.size() == 3);

    const std::string e = lib.Batch([&] {
        for (const TextureFile& m : mods) CHECK(lib.SetEnabled(m, false).empty());
        CHECK(lib.mods().size() == 3 && lib.mods()[0].enabled);  // not rescanned yet
        CHECK(lib.MoveTo(mods[0], L"Mod").empty());
        CHECK(lib.MoveTo(mods[1], L"Mod").empty());
    });
    CHECK(e.empty());
    CHECK(lib.mods().size() == 3);
    for (const TextureFile& m : lib.mods()) CHECK(!m.enabled);  // moved ones stay turned off
    CHECK(Find(lib, L"201.dds") && Find(lib, L"201.dds")->relDir == L"Mod");
    CHECK(Find(lib, L"203.dds") && Find(lib, L"203.dds")->relDir.empty());
    DisabledSet saved;
    saved.Load(modded);
    CHECK(saved.IsDisabled(L"Mod/202.dds") && saved.IsDisabled(L"203.dds"));
    fs::remove_all(root);
}

// A proxy in the game folder that fails its seal shows as Modified; an intact one doesn't.
static void TestProxyStatus(const fs::path& builtProxy) {
    const fs::path root = TempDir(L"reresorep_proxy_status_test");
    const fs::path exe = root / L"Game" / L"RainbowSix.exe";
    Touch(exe, "game");
    const fs::path dll = exe.parent_path() / L"d3d11.dll";
    fs::copy_file(builtProxy, dll);
    ManagerSettings s;
    // Not the bundled copy (there is none next to this test), but intact.
    CHECK(InspectGame(exe, s).status == ProxyStatus::Outdated);
    std::ofstream(dll, std::ios::binary | std::ios::app) << "appended";
    const GameState g = InspectGame(exe, s);
    CHECK(g.status == ProxyStatus::Modified && HasOurProxy(g.status));
    CHECK(!InstallProxy(exe, s).empty());  // no intact bundled proxy to install
    CHECK(UninstallProxy(exe).empty() && !fs::exists(dll));  // still recognised as ours
    fs::remove_all(root);
}

static std::vector<uint8_t> ReadBytes(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

static std::string ReadText(const fs::path& p) {
    const auto b = ReadBytes(p);
    return std::string(b.begin(), b.end());
}

static void TestUpdateParsing() {
    CHECK(ParseVersion("v2.5.4") && ParseVersion("v2.5.4")->ToString() == "2.5.4");
    CHECK(ParseVersion("10.0.12") && ParseVersion("10.0.12")->major == 10);
    CHECK(!ParseVersion("2.5") && !ParseVersion("2.5.4-beta") && !ParseVersion("") && !ParseVersion("v2.x.1"));
    CHECK(CompareVersions(*ParseVersion("2.5.10"), *ParseVersion("2.5.9")) > 0);
    CHECK(CompareVersions(*ParseVersion("2.6.0"), *ParseVersion("3.0.0")) < 0);
    CHECK(CompareVersions(*ParseVersion("2.5.4"), *ParseVersion("v2.5.4")) == 0);

    const std::string base = "https://github.com/SosigDOGGO/ReResorep/releases/download/v9.1.0/";
    const std::string json =
        "{\"html_url\":\"https://github.com/SosigDOGGO/ReResorep/releases/tag/v9.1.0\",\"tag_name\":\"v9.1.0\","
        "\"name\":\"ReResorep 9.1.0\",\"draft\":false,\"prerelease\":false,\"id\":123,\"assets\":["
        "{\"name\":\"ReResorep-9.1.0-win64.zip.sha256.txt\",\"size\":90,\"browser_download_url\":\"" + base +
        "ReResorep-9.1.0-win64.zip.sha256.txt\"},"
        "{\"name\":\"ReResorep-9.1.0-win64.zip\",\"browser_download_url\":\"" + base +
        "ReResorep-9.1.0-win64.zip\",\"uploader\":{\"x\":[1,2.5e3,-4,null,true,{}]}}],"
        "\"body\":\"## Fixes\\r\\n- **Bold** `code` caf\\u00e9 \\ud83d\\ude00 [the page](https://x)\"}";
    ReleaseInfo r;
    CHECK(ParseLatestRelease(json, r).empty());
    CHECK(r.version.ToString() == "9.1.0" && r.title == "ReResorep 9.1.0");
    CHECK(r.zipUrl == base + "ReResorep-9.1.0-win64.zip" && r.checksumUrl == r.zipUrl + ".sha256.txt");
    CHECK(NotesForDisplay(r.notes) == "Fixes\n- Bold code caf\xC3\xA9 \xF0\x9F\x98\x80 the page");

    // Assets elsewhere, unfinished releases and broken JSON are refused.
    std::string other = json;
    for (size_t at; (at = other.find("SosigDOGGO/ReResorep/releases/download")) != std::string::npos;)
        other.replace(at, 10, "SomeoneElse");
    CHECK(!ParseLatestRelease(other, r).empty());
    std::string pre = json;
    pre.replace(pre.find("\"prerelease\":false"), 18, "\"prerelease\":true");
    CHECK(!ParseLatestRelease(pre, r).empty());
    CHECK(!ParseLatestRelease(json.substr(0, json.size() - 1), r).empty());
    CHECK(!ParseLatestRelease("{\"tag_name\":\"v1.0.0\",\"assets\":[]}", r).empty());

    const std::string hash(64, 'A');
    const std::string sums = hash + "  ReResorep-9.1.0-win64.zip\n" + std::string(64, 'b') + " *proxy/d3d11.dll\r\n";
    CHECK(ParseChecksumFile(sums, "ReResorep-9.1.0-win64.zip") == std::string(64, 'a'));
    CHECK(ParseChecksumFile(sums, "proxy/d3d11.dll") == std::string(64, 'b'));
    CHECK(ParseChecksumFile(sums, "ReResorep.exe").empty());
    CHECK(ParseChecksumFile("xyz  ReResorep.exe\n", "ReResorep.exe").empty());
}

// A release zip like the ship step makes, from this build's sealed exe and proxy.
static void WriteReleaseZip(const fs::path& zip, const std::string& version, const std::vector<uint8_t>& exe,
                            const std::vector<uint8_t>& proxy, bool matchingSums = true) {
    const std::string root = "ReResorep-" + version + "/";
    std::string sums = Sha256Hex(exe.data(), exe.size()) + "  ReResorep.exe\n" +
                       Sha256Hex(proxy.data(), proxy.size()) + "  proxy/d3d11.dll\n";
    if (!matchingSums) sums = std::string(64, '0') + "  ReResorep.exe\n" + sums.substr(sums.find('\n') + 1);
    ZipWriter w;
    CHECK(w.Open(zip));
    w.AddDirectory(root);
    w.AddFile(root + "ReResorep.exe", exe.data(), exe.size());
    w.AddDirectory(root + "proxy");
    w.AddFile(root + "proxy/d3d11.dll", proxy.data(), proxy.size());
    w.AddFile(root + "SHA256SUMS.txt", reinterpret_cast<const uint8_t*>(sums.data()), sums.size());
    CHECK(w.Finish());
}

static std::string FileSha(const fs::path& p) {
    const auto b = ReadBytes(p);
    return Sha256Hex(b.data(), b.size());
}

static void TestUpdateInstall(const fs::path& builtProxy, const fs::path& builtExe) {
    const fs::path root = TempDir(L"reresorep_update_test");
    const fs::path install = root / L"ReResorep";
    const std::vector<uint8_t> exe = ReadBytes(builtExe), proxy = ReadBytes(builtProxy);
    CHECK(!exe.empty() && !proxy.empty());
    const Version version = *ParseVersion(RERESOREP_VERSION_STRING);
    const fs::path zip = root / L"update.zip";
    WriteReleaseZip(zip, version.ToString(), exe, proxy);
    const std::string zipSha = FileSha(zip);

    // The proxy in the release is the one already there: only the exe changes.
    Touch(install / L"ReResorep.exe", "old exe");
    fs::create_directories(install / L"proxy");
    fs::copy_file(builtProxy, install / L"proxy" / L"d3d11.dll");
    StagedUpdate staged;
    CHECK(!StageUpdate(zip, std::string(64, '0'), version, install, L"ReResorep.exe", staged).empty());
    CHECK(!StageUpdate(zip, zipSha, *ParseVersion("99.0.0"), install, L"ReResorep.exe", staged).empty());
    CHECK(StageUpdate(zip, zipSha, version, install, L"ReResorep.exe", staged).empty());
    CHECK(staged.exe == install / L"ReResorep.new.exe" && fs::exists(staged.exe) && staged.proxy.empty());
    CHECK(ReadText(install / L"ReResorep.exe") == "old exe");  // nothing replaced yet
    CHECK(ApplyUpdate(staged, install, L"ReResorep.exe").empty());
    CHECK(ReadBytes(install / L"ReResorep.exe") == exe && ReadText(install / L"ReResorep.old.exe") == "old exe");
    CHECK(FilesEqual(install / L"proxy" / L"d3d11.dll", builtProxy) && !fs::exists(install / L"proxy" / L"d3d11.old.dll"));
    CHECK(CleanUpAfterUpdate(install, L"ReResorep.exe") && !fs::exists(install / L"ReResorep.old.exe"));

    // A different proxy is replaced and reported; a renamed exe keeps its name.
    Touch(install / L"proxy" / L"d3d11.dll", "old proxy");
    fs::rename(install / L"ReResorep.exe", install / L"Manager.exe");
    CHECK(StageUpdate(zip, zipSha, version, install, L"Manager.exe", staged).empty());
    CHECK(staged.exe == install / L"Manager.new.exe" && staged.proxy == install / L"proxy" / L"d3d11.new.dll");
    CHECK(staged.proxySha256 == FileSha(builtProxy) && staged.proxyVersion == RERESOREP_PROXY_VERSION_STRING);
    CHECK(ApplyUpdate(staged, install, L"Manager.exe").empty());
    CHECK(FilesEqual(install / L"proxy" / L"d3d11.dll", builtProxy));
    CHECK(ReadText(install / L"proxy" / L"d3d11.old.dll") == "old proxy");
    CHECK(CleanUpAfterUpdate(install, L"Manager.exe") && !fs::exists(install / L"proxy" / L"d3d11.old.dll"));

    // Checksums that don't match and files that aren't intact builds are refused, staging nothing.
    WriteReleaseZip(zip, version.ToString(), exe, proxy, false);
    CHECK(!StageUpdate(zip, FileSha(zip), version, install, L"Manager.exe", staged).empty());
    std::vector<uint8_t> tampered = exe;
    tampered[tampered.size() / 2] ^= 1;
    WriteReleaseZip(zip, version.ToString(), tampered, proxy);
    CHECK(!StageUpdate(zip, FileSha(zip), version, install, L"Manager.exe", staged).empty());
    CHECK(!fs::exists(install / L"Manager.new.exe") && !fs::exists(install / L"proxy" / L"d3d11.new.dll"));
    CHECK(!ApplyUpdate(StagedUpdate{}, install, L"Manager.exe").empty());  // nothing staged
    CHECK(FilesEqual(install / L"Manager.exe", builtExe));
    fs::remove_all(root);
}

// Talks to GitHub: only with RERESOREP_NETWORK_TEST=1. Downloads the latest release and stages it.
static void TestUpdateFromGitHub() {
    wchar_t flag[8] = {};
    if (!GetEnvironmentVariableW(L"RERESOREP_NETWORK_TEST", flag, 8) || std::wstring(flag) != L"1") return;
    std::vector<uint8_t> body;
    std::string error = HttpGet("https://api.github.com/repos/SosigDOGGO/ReResorep/releases/latest", body, 1 << 20,
                                nullptr, true);
    ReleaseInfo r;
    if (error.empty()) error = ParseLatestRelease(std::string(body.begin(), body.end()), r);
    std::printf("network: latest is %s (%s)\n", r.version.ToString().c_str(), error.c_str());
    CHECK(error.empty());
    std::vector<uint8_t> sums, zip;
    CHECK(HttpGet(r.checksumUrl, sums, 1 << 16).empty());
    const std::string expected = ParseChecksumFile(std::string(sums.begin(), sums.end()), r.zipName);
    DownloadProgress progress;
    CHECK(HttpGet(r.zipUrl, zip, 256u << 20, &progress).empty() && progress.received == zip.size());
    const fs::path root = TempDir(L"reresorep_update_net_test");
    std::ofstream(root / L"update.zip", std::ios::binary)
        .write(reinterpret_cast<const char*>(zip.data()), std::streamsize(zip.size()));
    StagedUpdate staged;
    error = StageUpdate(root / L"update.zip", expected, r.version, root, L"ReResorep.exe", staged);
    std::printf("network: staged %s, proxy %s %s (%s)\n", ToUtf8(staged.exe).c_str(), staged.proxyVersion.c_str(),
                staged.proxySha256.c_str(), error.c_str());
    CHECK(error.empty() && fs::exists(staged.exe) && !staged.proxySha256.empty());
    fs::remove_all(root);
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    TestWinnersPerBuild();
    TestSettingsAndHashing();
    TestBatch();
    if (argc > 1) TestProxyStatus(argv[1]);
    else CHECK(!"pass the built d3d11.dll");
    TestUpdateParsing();
    if (argc > 2) TestUpdateInstall(argv[1], argv[2]);
    else CHECK(!"pass the built ReResorep.exe");
    TestUpdateFromGitHub();
    fs::remove(ExeDirectory() / L"texture_info.cache");  // written by the Library above
    std::printf(g_failures ? "%d check(s) failed\n" : "all manager checks passed\n", g_failures);
    return g_failures ? 1 : 0;
}
