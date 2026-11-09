// Self-contained checks for the core library. Run: build\bin\core_tests.exe
#include <windows.h>
#include <crtdbg.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <vector>

#include "blowfish.h"
#include "dds.h"
#include "labels.h"
#include "mod_package.h"
#include "mod_state.h"
#include "resorep_crypto.h"
#include "resorep_hash.h"
#include "texture_index.h"
#include "zip.h"

using namespace resorep;

static int g_failures = 0;
#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                              \
        }                                                              \
    } while (0)

static std::vector<uint8_t> Hex(const char* s) {
    std::vector<uint8_t> out;
    for (; s[0] && s[1]; s += 2) out.push_back(uint8_t(std::stoul(std::string(s, 2), nullptr, 16)));
    return out;
}

static void TestBlowfishEcb() {
    // Eric Young's published Blowfish test vectors.
    struct V { const char *key, *plain, *cipher; };
    const V vectors[] = {
        {"0000000000000000", "0000000000000000", "4EF997456198DD78"},
        {"FFFFFFFFFFFFFFFF", "FFFFFFFFFFFFFFFF", "51866FD5B85ECB8A"},
        {"3000000000000000", "1000000000000001", "7D856F9A613063F2"},
        {"0123456789ABCDEF", "1111111111111111", "61F9C3802281B096"},
    };
    for (const V& v : vectors) {
        auto key = Hex(v.key), pt = Hex(v.plain), ct = Hex(v.cipher);
        Blowfish bf(key.data(), key.size());
        uint32_t l = (pt[0] << 24) | (pt[1] << 16) | (pt[2] << 8) | pt[3];
        uint32_t r = (pt[4] << 24) | (pt[5] << 16) | (pt[6] << 8) | pt[7];
        bf.EncryptBlock(l, r);
        CHECK(l == uint32_t((ct[0] << 24) | (ct[1] << 16) | (ct[2] << 8) | ct[3]));
        CHECK(r == uint32_t((ct[4] << 24) | (ct[5] << 16) | (ct[6] << 8) | ct[7]));
        bf.DecryptBlock(l, r);
        CHECK(l == uint32_t((pt[0] << 24) | (pt[1] << 16) | (pt[2] << 8) | pt[3]));
    }
}

static void TestBlowfishCbc() {
    // CBC vector from the same test suite (16-byte key, as Resorep uses).
    auto key = Hex("0123456789ABCDEFF0E1D2C3B4A59687");
    auto iv = Hex("FEDCBA9876543210");
    std::vector<uint8_t> data(32, 0);
    std::memcpy(data.data(), "7654321 Now is the time for ", 28);
    const auto expected = Hex("6B77B4D63006DEE605B156E27403979358DEB9E7154616D959F1652BD5FF92CC");
    Blowfish bf(key.data(), key.size());
    auto copy = data;
    bf.EncryptCbc(iv.data(), copy.data(), copy.size());
    CHECK(copy == expected);
    bf.DecryptCbc(iv.data(), copy.data(), copy.size());
    CHECK(copy == data);
}

static void TestHash() {
    CHECK(ComputeTextureHash(nullptr, 0) == 1);
    const uint8_t neg[] = {0x80};
    CHECK(ComputeTextureHash(neg, 1) == uint32_t(1 - 128));
    const uint8_t mixed[] = {1, 2, 0xFF, 0x7F};
    CHECK(ComputeTextureHash(mixed, 4) == uint32_t(1 + 1 + 2 - 1 + 127));

    std::mt19937 rng(1234);
    for (size_t len : {1u, 15u, 16u, 17u, 31u, 1000u, 65536u + 7u}) {
        std::vector<uint8_t> buf(len);
        for (auto& b : buf) b = uint8_t(rng());
        CHECK(ComputeTextureHash(buf.data(), len) == ComputeTextureHashScalar(buf.data(), len));
    }
}

static void TestContainer() {
    std::vector<uint8_t> dds(1000);
    for (size_t i = 0; i < dds.size(); ++i) dds[i] = uint8_t(i * 7);
    const uint8_t iv[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    const auto enc = EncryptTexture(dds.data(), dds.size(), iv);
    CHECK(IsEncryptedTexture(enc.data(), enc.size()));
    CHECK(std::memcmp(enc.data(), "resorep1", 8) == 0 && enc[8] == 8);
    std::vector<uint8_t> dec;
    CHECK(DecryptTexture(enc.data(), enc.size(), dec));
    CHECK(dec.size() >= dds.size() && std::memcmp(dec.data(), dds.data(), dds.size()) == 0);
    CHECK(!IsEncryptedTexture(dds.data(), dds.size()));
}

static void TestFileNames() {
    CHECK(ParseModFileName(L"12345.dds") == 12345u);
    CHECK(ParseModFileName(L"12345~hair.DDS") == 12345u);
    CHECK(ParseModFileName(L"4294967295.dds") == 4294967295u);
    CHECK(!ParseModFileName(L"4294967296.dds"));
    CHECK(!ParseModFileName(L"012.dds"));
    CHECK(ParseModFileName(L"0.dds") == 0u);
    CHECK(!ParseModFileName(L"123abc.dds"));
    CHECK(!ParseModFileName(L"123.png"));
    CHECK(!ParseModFileName(L"hair.dds"));
}

static void TestDdsRoundTrip() {
    for (DXGI_FORMAT fmt : {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_BC1_UNORM, DXGI_FORMAT_BC7_UNORM_SRGB}) {
        const uint32_t w = 64, h = 32;
        std::vector<std::vector<uint8_t>> levels;
        std::vector<MipData> mips;
        for (uint32_t m = 0; m < 4; ++m) {
            SurfaceLayout l;
            CHECK(ComputeSurfaceLayout(fmt, w >> m, h >> m, l));
            levels.emplace_back(l.size(), uint8_t(m + 1));
        }
        for (uint32_t m = 0; m < 4; ++m) {
            SurfaceLayout l;
            ComputeSurfaceLayout(fmt, w >> m, h >> m, l);
            mips.push_back({levels[m].data(), l.rowPitch});
        }
        auto file = WriteDds(fmt, w, h, mips);
        DdsImage img;
        std::string err;
        CHECK(ParseDds(std::move(file), img, &err));
        CHECK(img.width == w && img.height == h && img.mipLevels == 4 && img.format == fmt);
        CHECK(img.subresources.size() == 4);
        for (uint32_t m = 0; m < img.subresources.size() && m < 4; ++m)
            CHECK(img.data[img.subresources[m].offset] == m + 1);
    }
}

static void TestDdsArrayAndVolumeHeaders() {
    const uint8_t pixel = 1;
    const auto single = WriteDds(DXGI_FORMAT_R8_UNORM, 1, 1, {{&pixel, 1}});
    auto read = [&](uint32_t dimension, uint32_t flags, uint32_t count, uint32_t depth, size_t pixels) {
        auto file = single;
        std::memcpy(file.data() + 24, &depth, 4);       // DDS_HEADER.depth
        std::memcpy(file.data() + 132, &dimension, 4);  // DDS_HEADER_DXT10.resourceDimension
        std::memcpy(file.data() + 136, &flags, 4);
        std::memcpy(file.data() + 140, &count, 4);
        file.resize(148 + pixels, pixel);
        DdsImage image;
        return ParseDds(std::move(file), image);
    };
    CHECK(read(3, 4, 1, 0, 6));    // a cube has six faces
    CHECK(read(4, 0, 1, 2, 2));    // a volume has a single array element
    CHECK(!read(3, 0, 0, 0, 1));
    CHECK(!read(4, 0, 2, 1, 2));
    CHECK(!read(4, 4, 1, 1, 6));
}

static void TestCategories() {
    namespace fs = std::filesystem;
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    const fs::path root = fs::path(tmp) / L"reresorep_core_test";
    fs::remove_all(root);
    for (const wchar_t* dir : {L"ModA/Cat1", L"ModA/Cat2", L"ModB"}) fs::create_directories(root / dir);
    for (const wchar_t* file : {L"100.dds", L"ModA/Cat1/200.dds", L"ModA/Cat2/300.dds", L"ModB/100~alt.dds",
                                L"ModB/400.dds", L"ModA/notes.txt"})
        std::ofstream(root / file) << "x";

    TextureIndex index;
    index.Scan(root);
    CHECK(index.size() == 4);
    CHECK(index.FindExact(100) && index.FindExact(100)->filename() == L"100.dds");  // loose beats ModB
    CHECK(index.FindExact(300) != nullptr);

    DisabledSet off;
    off.Set(L"ModA\\Cat2", true);  // a category
    off.Set(L"100.dds", true);     // a single texture
    CHECK(off.Save(root));
    index.Scan(root);
    CHECK(index.FindExact(300) == nullptr);
    CHECK(index.FindExact(200) != nullptr);
    CHECK(index.FindExact(100) && index.FindExact(100)->filename() == L"100~alt.dds");  // falls back to ModB

    DisabledSet loaded;
    CHECK(loaded.Load(root));
    CHECK(loaded.IsDisabled(L"moda/cat2"));  // case-insensitive
    CHECK(loaded.IsEffectivelyDisabled(L"ModA/Cat2/300.dds"));
    CHECK(!loaded.IsEffectivelyDisabled(L"ModA/Cat1/200.dds"));
    loaded.Set(L"moda", true);  // whole mod
    CHECK(loaded.Save(root));
    index.Scan(root);
    CHECK(index.FindExact(200) == nullptr && index.size() == 2);

    loaded.Rename(L"ModA", L"Renamed");
    CHECK(loaded.IsDisabled(L"Renamed") && loaded.IsDisabled(L"Renamed/Cat2") && !loaded.IsDisabled(L"ModA"));
    loaded.RemoveUnder(L"Renamed");
    CHECK(!loaded.IsDisabled(L"Renamed/Cat2") && loaded.IsDisabled(L"100.dds"));
    fs::remove_all(root);
}

static void TestPartialMips() {
    namespace fs = std::filesystem;
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    for (DXGI_FORMAT fmt : {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_BC1_UNORM}) {
        // 256x128 with a full chain; every mip filled with a distinct byte pattern.
        const uint32_t w = 256, h = 128;
        std::vector<std::vector<uint8_t>> levels;
        std::vector<MipData> mips;
        for (uint32_t m = 0; (w >> m) || (h >> m); ++m) {
            SurfaceLayout l;
            ComputeSurfaceLayout(fmt, w >> m, h >> m, l);
            std::vector<uint8_t> level(l.size());
            for (size_t i = 0; i < level.size(); ++i) level[i] = uint8_t(m * 31 + i * 7);
            levels.push_back(std::move(level));
        }
        for (uint32_t m = 0; m < levels.size(); ++m) {
            SurfaceLayout l;
            ComputeSurfaceLayout(fmt, w >> m, h >> m, l);
            mips.push_back({levels[m].data(), l.rowPitch});
        }
        const auto dds = WriteDds(fmt, w, h, mips);
        const uint8_t iv[8] = {1, 1, 2, 3, 5, 8, 13, 21};
        const auto enc = EncryptTexture(dds.data(), dds.size(), iv);
        const fs::path plainPath = fs::path(tmp) / L"reresorep_mips_plain.dds";
        const fs::path encPath = fs::path(tmp) / L"reresorep_mips_enc.dds";
        std::ofstream(plainPath, std::ios::binary).write(reinterpret_cast<const char*>(dds.data()), dds.size());
        std::ofstream(encPath, std::ios::binary).write(reinterpret_cast<const char*>(enc.data()), enc.size());

        for (const fs::path& path : {plainPath, encPath}) {
            DdsImage part;
            bool encrypted = false;
            std::string err;
            CHECK(LoadTextureMips(path.wstring(), 32, part, &encrypted, &err));
            CHECK(encrypted == (path == encPath));
            CHECK(part.width == 32 && part.height == 16 && part.format == fmt);  // mip 3
            CHECK(part.mipLevels == levels.size() - 3);
            for (uint32_t m = 0; m < part.mipLevels && m + 3 < levels.size(); ++m) {
                const DdsSubresource& s = part.subresources[m];
                CHECK(std::memcmp(part.data.data() + s.offset, levels[m + 3].data(), levels[m + 3].size()) == 0);
            }
            DdsImage info;
            CHECK(ReadTextureInfo(path.wstring(), info) && info.width == w && info.height == h);
        }
        auto longIv = enc;
        longIv.insert(longIv.begin() + 17, 247, 0);
        longIv[8] = 255;
        std::ofstream(encPath, std::ios::binary).write(reinterpret_cast<const char*>(longIv.data()), longIv.size());
        DdsImage info;
        CHECK(ReadTextureInfo(encPath.wstring(), info) && info.width == w && info.height == h);

        fs::remove(plainPath);
        fs::remove(encPath);
    }
}

static void TestLabels() {
    const auto parsed = LabelStore::Parse(
        "\xEF\xBB\xBF# comment\r\n123=Ash helmet\r\n456,\"Thermite vest\"\n789\tUI icon\n"
        "notahash=x\n4294967296=too big\n999=\n; other comment\n");
    CHECK(parsed.size() == 3);
    if (parsed.size() == 3) {
        CHECK(parsed[0].first == 123 && parsed[0].second == "Ash helmet");
        CHECK(parsed[1].first == 456 && parsed[1].second == "Thermite vest");
        CHECK(parsed[2].first == 789 && parsed[2].second == "UI icon");
    }
    const auto quoted = LabelStore::Parse("\"4294967295\",\"Last hash\"\n3123456789 = Spaced\n");
    CHECK(quoted.size() == 2);
    if (quoted.size() == 2) {
        CHECK(quoted[0].first == UINT32_MAX && quoted[0].second == "Last hash");
        CHECK(quoted[1].first == 3123456789u && quoted[1].second == "Spaced");
    }
    LabelStore store;
    CHECK(store.Merge(parsed, true) == 3);
    CHECK(store.Merge({{123, "Ash helmet"}, {123, "Other"}}, false) == 0);
    store.Set(456, "line\nbreak");
    CHECK(*store.Get(456) == "line break");
    store.Set(789, "  ");
    CHECK(!store.Get(789) && store.size() == 2);
    LabelStore round;
    round.Merge(LabelStore::Parse(store.Serialize()), true);
    CHECK(round.size() == 2 && *round.Get(123) == "Ash helmet");
}

#include "zip_fixture.inc"

static std::vector<uint8_t> Pattern(size_t n, int seed) {
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = uint8_t(i * 31 + (i >> 6) * seed);
    return v;
}

static void WriteBytes(const std::filesystem::path& p, const std::vector<uint8_t>& d) {
    std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(d.data()), std::streamsize(d.size()));
}

static void TestZipFromOtherTool() {
    namespace fs = std::filesystem;
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    const fs::path base = fs::path(tmp) / L"reresorep_zip_test";
    fs::remove_all(base);
    fs::create_directories(base / L"modded");
    const fs::path zipPath = base / L"fixture.zip";
    WriteBytes(zipPath, std::vector<uint8_t>(std::begin(kFixtureZip), std::end(kFixtureZip)));

    {  // Reader + inflate against Python's zipfile output.
        ZipReader zip;
        CHECK(zip.Open(zipPath));
        CHECK(zip.entries().size() == 7);
        for (const auto& e : zip.entries()) {
            if (e.name != "Pack/Cat/123.dds") continue;
            std::vector<uint8_t> data;
            CHECK(e.method == 8 && zip.Extract(e, data));
            auto expected = Pattern(20000, 3);
            expected.insert(expected.begin(), {'D', 'D', 'S', ' '});
            CHECK(data == expected);
        }
    }

    // The EOCD signature is legal text in an archive comment.
    auto commented = std::vector<uint8_t>(std::begin(kFixtureZip), std::end(kFixtureZip));
    const std::string comment = "notes PK\x05\x06 followed by more than twenty bytes of text";
    commented[commented.size() - 2] = uint8_t(comment.size());
    commented.insert(commented.end(), comment.begin(), comment.end());
    const fs::path withComment = base / L"comment.zip";
    WriteBytes(withComment, commented);
    {
        ZipReader reader;
        CHECK(reader.Open(withComment) && reader.entries().size() == 7);
        CHECK(reader.Open(zipPath) && reader.entries().size() == 7);
    }

    {
        ZipWriter writer;
        CHECK(writer.Open(base / L"invalid.zip"));
        CHECK(!writer.AddDirectory(std::string(65536, 'x')));
        CHECK(!writer.Finish());
        CHECK(writer.Open(base / L"retry.zip"));
        CHECK(writer.AddDirectory("Empty") && writer.Finish());
        ZipReader reader;
        CHECK(reader.Open(base / L"retry.zip") && reader.entries().size() == 1);
    }

    // Import: textures land inside the mod folder; traversal entries and non-textures are refused.
    const PackageInfo info = InspectModPackage(zipPath);
    CHECK(info.error.empty() && info.textures == 5 && !info.fromReResorep);
    LabelStore labels;
    const PackageResult r = ImportModPackage(zipPath, base / L"modded", "Fixture", false, labels);
    CHECK(r.error.empty());
    CHECK(r.textures == 3);
    CHECK(r.skipped >= 3);  // notes.txt, ../evil.dds, C:/evil2.dds
    CHECK(fs::exists(base / L"modded/Fixture/Pack/Cat/123.dds"));
    CHECK(fs::exists(base / L"modded/Fixture/Pack/456~blue.dds"));
    CHECK(fs::exists(base / L"modded/Fixture/Pack/789.dds"));
    CHECK(!fs::exists(base / L"modded/Fixture/Pack/notes.txt"));
    CHECK(!fs::exists(base / L"evil.dds") && !fs::exists(base / L"modded/evil.dds"));
    fs::remove_all(base);
}

static void TestModPackageRoundTrip() {
    namespace fs = std::filesystem;
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    const fs::path base = fs::path(tmp) / L"reresorep_pkg_test";
    fs::remove_all(base);
    const fs::path modded = base / L"modded";
    for (const wchar_t* d : {L"Ash Elite/Uniform", L"Ash Elite/Weapons", L"Ash Elite/Empty"}) fs::create_directories(modded / d);
    WriteBytes(modded / L"Ash Elite/Uniform/200.dds", Pattern(5000, 1));
    WriteBytes(modded / L"Ash Elite/Weapons/300~gun.dds", Pattern(7000, 2));
    WriteBytes(modded / L"Ash Elite/Weapons/readme.txt", Pattern(10, 0));
    DisabledSet off;
    off.Set(L"Ash Elite/Weapons", true);
    off.Save(modded);
    LabelStore labels;
    labels.Set(200, "Ash uniform");
    labels.Set(300, "Ash gun");
    labels.Set(999, "Not in this mod");

    const fs::path full = base / L"full.zip";
    PackageResult ex = ExportModPackage(modded, L"Ash Elite", full, {true, true}, labels);
    CHECK(ex.error.empty() && ex.textures == 2 && ex.labels == 2);

    // Import under a new name into a fresh library with one label of its own.
    const fs::path other = base / L"other";
    fs::create_directories(other);
    LabelStore mine;
    mine.Set(200, "My own name");
    PackageInfo info = InspectModPackage(full);
    CHECK(info.fromReResorep && info.name == "Ash Elite" && info.textures == 2 && info.labels == 2);
    PackageResult im = ImportModPackage(full, other, "Ash Copy", false, mine);
    CHECK(im.error.empty() && im.textures == 2);
    CHECK(fs::exists(other / L"Ash Copy/Uniform/200.dds") && fs::exists(other / L"Ash Copy/Weapons/300~gun.dds"));
    CHECK(fs::is_directory(other / L"Ash Copy/Empty"));  // empty categories survive
    CHECK(!fs::exists(other / L"Ash Copy/Weapons/readme.txt"));
    CHECK(im.labels == 1 && *mine.Get(200) == "My own name" && *mine.Get(300) == "Ash gun");
    DisabledSet state;
    state.Load(other);
    CHECK(state.IsDisabled(L"Ash Copy/Weapons") && !state.IsDisabled(L"Ash Copy/Uniform"));
    {
        auto read = [](const fs::path& p) {
            std::ifstream in(p, std::ios::binary);
            return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        };
        const std::string original = read(modded / L"Ash Elite/Weapons/300~gun.dds");
        CHECK(!original.empty() && original == read(other / L"Ash Copy/Weapons/300~gun.dds"));
    }

    // Importing again without overwrite keeps existing files.
    PackageResult again = ImportModPackage(full, other, "Ash Copy", false, mine);
    CHECK(again.error.empty() && again.textures == 0 && again.skipped >= 2);

    // Only what is turned on.
    const fs::path onOnly = base / L"on.zip";
    ex = ExportModPackage(modded, L"Ash Elite", onOnly, {false, false}, labels);
    CHECK(ex.error.empty() && ex.textures == 1 && ex.labels == 0);
    // An exported category can inherit its disabled state from the parent mod.
    off.Set(L"Ash Elite", true);
    CHECK(off.Save(modded));
    CHECK(ExportModPackage(modded, L"Ash Elite/Uniform", full, {true, false}, labels).error.empty());
    CHECK(ImportModPackage(full, other, "Uniform copy", false, mine).error.empty());
    CHECK(state.Load(other) && state.IsDisabled(L"Uniform copy"));

    CHECK(!ExportModPackage(modded, L"Ash Elite", full, {false, false}, labels).error.empty());
    CHECK(InspectModPackage(full).name == "Uniform");

    // A sharing violation must leave the existing zip untouched.
    HANDLE lock = CreateFileW((modded / L"Ash Elite/Uniform/200.dds").c_str(), GENERIC_READ, 0, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(lock != INVALID_HANDLE_VALUE);
    if (lock != INVALID_HANDLE_VALUE) {
        CHECK(!ExportModPackage(modded, L"Ash Elite", full, {true, false}, labels).error.empty());
        CloseHandle(lock);
        CHECK(InspectModPackage(full).name == "Uniform");
    }

    // An existing file blocks an empty category in the package.
    fs::remove(other / L"Ash Copy/Empty");
    WriteBytes(other / L"Ash Copy/Empty", {});
    CHECK(!ImportModPackage(onOnly, other, "Ash Copy", false, mine).error.empty());
    fs::remove_all(base);
}

static void TestModGame() {
    namespace fs = std::filesystem;
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    const fs::path root = fs::path(tmp) / L"reresorep_game_test";
    fs::remove_all(root);
    const std::string buildA(64, 'a'), buildB(64, 'b');
    for (const wchar_t* dir : {L"ForA/Cat", L"ForB", L"AnyGame"}) fs::create_directories(root / dir);
    for (const wchar_t* file : {L"ForA/Cat/100.dds", L"ForB/200.dds", L"AnyGame/300.dds", L"400.dds"})
        std::ofstream(root / file) << "x";

    CHECK(ReadModGame(root / L"ForA").empty());
    CHECK(WriteModGame(root / L"ForA", std::string(64, 'A'), "Build A"));  // stored and compared in lower case
    CHECK(WriteModGame(root / L"ForB", buildB, ""));
    CHECK(fs::exists(root / L"ForA" / L"!reresorep_game.ini"));
    CHECK(ReadModGame(root / L"ForA") == buildA && ReadModGame(root / L"ForB") == buildB);

    TextureIndex index;
    index.Scan(root);  // no game given: every mod applies
    CHECK(index.size() == 4);
    index.Scan(root, buildA);
    CHECK(index.FindExact(100) && !index.FindExact(200) && index.FindExact(300) && index.FindExact(400));
    index.Scan(root, buildB);
    CHECK(!index.FindExact(100) && index.FindExact(200) && index.FindExact(300) && index.FindExact(400));

    // The game travels with an exported mod (and a category of it), and comes back on import.
    LabelStore labels;
    const fs::path zip = root.parent_path() / L"reresorep_game_test.zip";
    CHECK(ExportModPackage(root, L"ForA", zip, {true, false}, labels).error.empty());
    CHECK(InspectModPackage(zip).game == buildA);
    CHECK(ExportModPackage(root, L"ForA/Cat", zip, {true, false}, labels).error.empty());
    CHECK(InspectModPackage(zip).game == buildA);
    const fs::path other = root / L"other";
    fs::create_directories(other);
    CHECK(ImportModPackage(zip, other, "Copy", false, labels, buildB).error.empty());
    CHECK(ReadModGame(other / L"Copy") == buildB);
    CHECK(ImportModPackage(zip, other, "Untagged", false, labels).error.empty());
    CHECK(ReadModGame(other / L"Untagged").empty());

    CHECK(WriteModGame(root / L"ForA", "", ""));  // back to any game
    CHECK(!fs::exists(root / L"ForA" / L"!reresorep_game.ini"));
    fs::remove(zip);
    fs::remove_all(root);
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);  // keep output if a test crashes
    // Debug-CRT assertions go to stderr instead of a dialog nobody sees in CI.
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    struct { const char* name; void (*run)(); } tests[] = {
        {"TestBlowfishEcb", TestBlowfishEcb},
        {"TestBlowfishCbc", TestBlowfishCbc},
        {"TestHash", TestHash},
        {"TestContainer", TestContainer},
        {"TestFileNames", TestFileNames},
        {"TestDdsRoundTrip", TestDdsRoundTrip},
        {"TestDdsArrayAndVolumeHeaders", TestDdsArrayAndVolumeHeaders},
        {"TestCategories", TestCategories},
        {"TestPartialMips", TestPartialMips},
        {"TestLabels", TestLabels},
        {"TestZipFromOtherTool", TestZipFromOtherTool},
        {"TestModPackageRoundTrip", TestModPackageRoundTrip},
        {"TestModGame", TestModGame},
    };
    for (const auto& t : tests) {
        std::printf("%s\n", t.name);
        try {
            t.run();
        } catch (const std::exception& e) {
            std::printf("FAIL %s threw: %s\n", t.name, e.what());
            ++g_failures;
        }
    }
    std::printf(g_failures ? "%d check(s) failed\n" : "all checks passed\n", g_failures);
    return g_failures ? 1 : 0;
}
