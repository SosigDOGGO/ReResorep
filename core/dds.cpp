#include "dds.h"

#include <algorithm>
#include <cstring>
#include <fstream>

#include "resorep_crypto.h"

namespace resorep {
namespace {

constexpr uint32_t kDdsMagic = 0x20534444;  // "DDS "

constexpr uint32_t DDSD_CAPS = 0x1, DDSD_HEIGHT = 0x2, DDSD_WIDTH = 0x4, DDSD_PITCH = 0x8,
                   DDSD_PIXELFORMAT = 0x1000, DDSD_MIPMAPCOUNT = 0x20000,
                   DDSD_LINEARSIZE = 0x80000;
constexpr uint32_t DDPF_ALPHAPIXELS = 0x1, DDPF_ALPHA = 0x2, DDPF_FOURCC = 0x4, DDPF_RGB = 0x40,
                   DDPF_LUMINANCE = 0x20000;
constexpr uint32_t DDSCAPS_COMPLEX = 0x8, DDSCAPS_TEXTURE = 0x1000, DDSCAPS_MIPMAP = 0x400000;
constexpr uint32_t DDSCAPS2_CUBEMAP = 0x200, DDSCAPS2_VOLUME = 0x200000;
constexpr uint32_t kDimTexture2D = 3, kDimTexture3D = 4, kMiscTextureCube = 0x4;

#pragma pack(push, 1)
struct DdsPixelFormat {
    uint32_t size, flags, fourCC, rgbBitCount, rMask, gMask, bMask, aMask;
};
struct DdsHeader {
    uint32_t size, flags, height, width, pitchOrLinearSize, depth, mipMapCount;
    uint32_t reserved1[11];
    DdsPixelFormat ddspf;
    uint32_t caps, caps2, caps3, caps4, reserved2;
};
struct DdsHeaderDx10 {
    uint32_t dxgiFormat, resourceDimension, miscFlag, arraySize, miscFlags2;
};
#pragma pack(pop)
static_assert(sizeof(DdsHeader) == 124 && sizeof(DdsHeaderDx10) == 20);

constexpr uint32_t FourCC(char a, char b, char c, char d) {
    return uint32_t(uint8_t(a)) | (uint32_t(uint8_t(b)) << 8) | (uint32_t(uint8_t(c)) << 16) |
           (uint32_t(uint8_t(d)) << 24);
}

bool MaskIs(const DdsPixelFormat& pf, uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
    return pf.rMask == r && pf.gMask == g && pf.bMask == b && pf.aMask == a;
}

DXGI_FORMAT LegacyFormat(const DdsPixelFormat& pf) {
    if (pf.flags & DDPF_FOURCC) {
        switch (pf.fourCC) {
            case FourCC('D', 'X', 'T', '1'): return DXGI_FORMAT_BC1_UNORM;
            case FourCC('D', 'X', 'T', '2'):
            case FourCC('D', 'X', 'T', '3'): return DXGI_FORMAT_BC2_UNORM;
            case FourCC('D', 'X', 'T', '4'):
            case FourCC('D', 'X', 'T', '5'): return DXGI_FORMAT_BC3_UNORM;
            case FourCC('A', 'T', 'I', '1'):
            case FourCC('B', 'C', '4', 'U'): return DXGI_FORMAT_BC4_UNORM;
            case FourCC('B', 'C', '4', 'S'): return DXGI_FORMAT_BC4_SNORM;
            case FourCC('A', 'T', 'I', '2'):
            case FourCC('B', 'C', '5', 'U'): return DXGI_FORMAT_BC5_UNORM;
            case FourCC('B', 'C', '5', 'S'): return DXGI_FORMAT_BC5_SNORM;
            case 36: return DXGI_FORMAT_R16G16B16A16_UNORM;
            case 110: return DXGI_FORMAT_R16G16B16A16_SNORM;
            case 111: return DXGI_FORMAT_R16_FLOAT;
            case 112: return DXGI_FORMAT_R16G16_FLOAT;
            case 113: return DXGI_FORMAT_R16G16B16A16_FLOAT;
            case 114: return DXGI_FORMAT_R32_FLOAT;
            case 115: return DXGI_FORMAT_R32G32_FLOAT;
            case 116: return DXGI_FORMAT_R32G32B32A32_FLOAT;
        }
        return DXGI_FORMAT_UNKNOWN;
    }
    if (pf.flags & DDPF_RGB) {
        switch (pf.rgbBitCount) {
            case 32:
                if (MaskIs(pf, 0xff, 0xff00, 0xff0000, 0xff000000) || MaskIs(pf, 0xff, 0xff00, 0xff0000, 0))
                    return DXGI_FORMAT_R8G8B8A8_UNORM;
                if (MaskIs(pf, 0xff0000, 0xff00, 0xff, 0xff000000)) return DXGI_FORMAT_B8G8R8A8_UNORM;
                if (MaskIs(pf, 0xff0000, 0xff00, 0xff, 0)) return DXGI_FORMAT_B8G8R8X8_UNORM;
                if (MaskIs(pf, 0x3ff, 0xffc00, 0x3ff00000, 0xc0000000) ||
                    MaskIs(pf, 0x3ff00000, 0xffc00, 0x3ff, 0xc0000000))
                    return DXGI_FORMAT_R10G10B10A2_UNORM;
                if (MaskIs(pf, 0xffff, 0xffff0000, 0, 0)) return DXGI_FORMAT_R16G16_UNORM;
                if (MaskIs(pf, 0xffffffff, 0, 0, 0)) return DXGI_FORMAT_R32_FLOAT;
                break;
            case 16:
                if (MaskIs(pf, 0xf800, 0x7e0, 0x1f, 0)) return DXGI_FORMAT_B5G6R5_UNORM;
                if (MaskIs(pf, 0x7c00, 0x3e0, 0x1f, 0x8000)) return DXGI_FORMAT_B5G5R5A1_UNORM;
                if (MaskIs(pf, 0xf00, 0xf0, 0xf, 0xf000)) return DXGI_FORMAT_B4G4R4A4_UNORM;
                break;
        }
        return DXGI_FORMAT_UNKNOWN;
    }
    if (pf.flags & DDPF_LUMINANCE) {
        if (pf.rgbBitCount == 8) return DXGI_FORMAT_R8_UNORM;
        if (pf.rgbBitCount == 16 && MaskIs(pf, 0xffff, 0, 0, 0)) return DXGI_FORMAT_R16_UNORM;
        if (pf.rgbBitCount == 16 && MaskIs(pf, 0xff, 0, 0, 0xff00)) return DXGI_FORMAT_R8G8_UNORM;
        return DXGI_FORMAT_UNKNOWN;
    }
    if ((pf.flags & DDPF_ALPHA) && pf.rgbBitCount == 8) return DXGI_FORMAT_A8_UNORM;
    return DXGI_FORMAT_UNKNOWN;
}

void Fail(std::string* error, const char* msg) {
    if (error) *error = msg;
}

}  // namespace

FormatInfo GetFormatInfo(DXGI_FORMAT f) {
    auto px = [](uint32_t bytes) { return FormatInfo{true, false, bytes}; };
    auto bc = [](uint32_t bytes) { return FormatInfo{true, true, bytes}; };
    if (f >= DXGI_FORMAT_R32G32B32A32_TYPELESS && f <= DXGI_FORMAT_R32G32B32A32_SINT) return px(16);
    if (f >= DXGI_FORMAT_R32G32B32_TYPELESS && f <= DXGI_FORMAT_R32G32B32_SINT) return px(12);
    if (f >= DXGI_FORMAT_R16G16B16A16_TYPELESS && f <= DXGI_FORMAT_X32_TYPELESS_G8X24_UINT) return px(8);
    if (f >= DXGI_FORMAT_R10G10B10A2_TYPELESS && f <= DXGI_FORMAT_X24_TYPELESS_G8_UINT) return px(4);
    if (f >= DXGI_FORMAT_R8G8_TYPELESS && f <= DXGI_FORMAT_R16_SINT) return px(2);
    if (f >= DXGI_FORMAT_R8_TYPELESS && f <= DXGI_FORMAT_A8_UNORM) return px(1);
    if (f == DXGI_FORMAT_R9G9B9E5_SHAREDEXP) return px(4);
    if (f >= DXGI_FORMAT_BC1_TYPELESS && f <= DXGI_FORMAT_BC1_UNORM_SRGB) return bc(8);
    if (f >= DXGI_FORMAT_BC2_TYPELESS && f <= DXGI_FORMAT_BC3_UNORM_SRGB) return bc(16);
    if (f >= DXGI_FORMAT_BC4_TYPELESS && f <= DXGI_FORMAT_BC4_SNORM) return bc(8);
    if (f >= DXGI_FORMAT_BC5_TYPELESS && f <= DXGI_FORMAT_BC5_SNORM) return bc(16);
    if (f == DXGI_FORMAT_B5G6R5_UNORM || f == DXGI_FORMAT_B5G5R5A1_UNORM || f == DXGI_FORMAT_B4G4R4A4_UNORM)
        return px(2);
    if (f >= DXGI_FORMAT_B8G8R8A8_UNORM && f <= DXGI_FORMAT_B8G8R8X8_UNORM_SRGB) return px(4);
    if (f >= DXGI_FORMAT_BC6H_TYPELESS && f <= DXGI_FORMAT_BC7_UNORM_SRGB) return bc(16);
    return {};
}

const char* FormatName(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R32G32B32A32_FLOAT: return "RGBA32F";
        case DXGI_FORMAT_R16G16B16A16_FLOAT: return "RGBA16F";
        case DXGI_FORMAT_R16G16B16A16_UNORM: return "RGBA16";
        case DXGI_FORMAT_R10G10B10A2_UNORM: return "RGB10A2";
        case DXGI_FORMAT_R11G11B10_FLOAT: return "R11G11B10F";
        case DXGI_FORMAT_R8G8B8A8_UNORM: return "RGBA8";
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return "RGBA8 sRGB";
        case DXGI_FORMAT_R8G8B8A8_TYPELESS: return "RGBA8 typeless";
        case DXGI_FORMAT_R16G16_FLOAT: return "RG16F";
        case DXGI_FORMAT_R16G16_UNORM: return "RG16";
        case DXGI_FORMAT_R32_FLOAT: return "R32F";
        case DXGI_FORMAT_R8G8_UNORM: return "RG8";
        case DXGI_FORMAT_R16_FLOAT: return "R16F";
        case DXGI_FORMAT_R16_UNORM: return "R16";
        case DXGI_FORMAT_R8_UNORM: return "R8";
        case DXGI_FORMAT_A8_UNORM: return "A8";
        case DXGI_FORMAT_BC1_TYPELESS: return "BC1 typeless";
        case DXGI_FORMAT_BC1_UNORM: return "BC1/DXT1";
        case DXGI_FORMAT_BC1_UNORM_SRGB: return "BC1 sRGB";
        case DXGI_FORMAT_BC2_UNORM: return "BC2/DXT3";
        case DXGI_FORMAT_BC2_UNORM_SRGB: return "BC2 sRGB";
        case DXGI_FORMAT_BC3_TYPELESS: return "BC3 typeless";
        case DXGI_FORMAT_BC3_UNORM: return "BC3/DXT5";
        case DXGI_FORMAT_BC3_UNORM_SRGB: return "BC3 sRGB";
        case DXGI_FORMAT_BC4_UNORM: return "BC4";
        case DXGI_FORMAT_BC4_SNORM: return "BC4 snorm";
        case DXGI_FORMAT_BC5_UNORM: return "BC5";
        case DXGI_FORMAT_BC5_SNORM: return "BC5 snorm";
        case DXGI_FORMAT_B5G6R5_UNORM: return "B5G6R5";
        case DXGI_FORMAT_B5G5R5A1_UNORM: return "B5G5R5A1";
        case DXGI_FORMAT_B4G4R4A4_UNORM: return "B4G4R4A4";
        case DXGI_FORMAT_B8G8R8A8_UNORM: return "BGRA8";
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return "BGRA8 sRGB";
        case DXGI_FORMAT_B8G8R8X8_UNORM: return "BGRX8";
        case DXGI_FORMAT_BC6H_UF16: return "BC6H";
        case DXGI_FORMAT_BC6H_SF16: return "BC6H signed";
        case DXGI_FORMAT_BC7_TYPELESS: return "BC7 typeless";
        case DXGI_FORMAT_BC7_UNORM: return "BC7";
        case DXGI_FORMAT_BC7_UNORM_SRGB: return "BC7 sRGB";
        default: return "other";
    }
}

bool ComputeSurfaceLayout(DXGI_FORMAT format, uint32_t width, uint32_t height, SurfaceLayout& out) {
    const FormatInfo info = GetFormatInfo(format);
    if (!info.known) return false;
    width = std::max(width, 1u);
    height = std::max(height, 1u);
    if (info.blockCompressed) {
        out.rowPitch = std::max(1u, (width + 3) / 4) * info.bytesPerBlock;
        out.rows = std::max(1u, (height + 3) / 4);
    } else {
        out.rowPitch = width * info.bytesPerBlock;
        out.rows = height;
    }
    return true;
}

namespace {
// Fills everything but subresources/data; `offset` returns where pixel data starts.
bool ParseHeader(const uint8_t* file, size_t size, DdsImage& out, size_t& offset, std::string* error) {
    out = DdsImage{};
    if (size < 4 + sizeof(DdsHeader)) return Fail(error, "file too small"), false;

    uint32_t magic;
    std::memcpy(&magic, file, 4);
    if (magic != kDdsMagic) return Fail(error, "not a DDS file"), false;

    DdsHeader hdr;
    std::memcpy(&hdr, file + 4, sizeof(hdr));
    if (hdr.size != sizeof(DdsHeader) || hdr.ddspf.size != sizeof(DdsPixelFormat))
        return Fail(error, "corrupt DDS header"), false;

    offset = 4 + sizeof(DdsHeader);
    out.width = hdr.width;
    out.height = hdr.height;
    out.mipLevels = (hdr.flags & DDSD_MIPMAPCOUNT) ? std::max(1u, hdr.mipMapCount) : 1;

    if ((hdr.ddspf.flags & DDPF_FOURCC) && hdr.ddspf.fourCC == FourCC('D', 'X', '1', '0')) {
        if (size < offset + sizeof(DdsHeaderDx10)) return Fail(error, "truncated DX10 header"), false;
        DdsHeaderDx10 ext;
        std::memcpy(&ext, file + offset, sizeof(ext));
        offset += sizeof(ext);
        out.format = DXGI_FORMAT(ext.dxgiFormat);
        out.arraySize = ext.arraySize;
        if (ext.resourceDimension == kDimTexture3D) {
            out.isVolume = true;
            out.depth = std::max(1u, hdr.depth);
        } else if (ext.resourceDimension != kDimTexture2D) {
            return Fail(error, "only 2D and 3D textures are supported"), false;
        }
        out.isCubemap = (ext.miscFlag & kMiscTextureCube) != 0;
    } else {
        out.format = LegacyFormat(hdr.ddspf);
        if (hdr.caps2 & DDSCAPS2_VOLUME) {
            out.isVolume = true;
            out.depth = std::max(1u, hdr.depth);
        }
        out.isCubemap = (hdr.caps2 & DDSCAPS2_CUBEMAP) != 0;
    }

    if (!GetFormatInfo(out.format).known) return Fail(error, "unsupported pixel format"), false;
    if (out.width == 0 || out.height == 0 || out.width > 16384 || out.height > 16384 || out.mipLevels > 15)
        return Fail(error, "invalid dimensions"), false;
    // D3D11 limits apply to faces (not cubes) and to every dimension of a volume.
    if (out.arraySize == 0 || out.arraySize > 2048u / (out.isCubemap ? 6u : 1u))
        return Fail(error, "invalid array size"), false;
    if (out.isVolume && (out.arraySize != 1 || out.isCubemap || hdr.depth == 0 ||
                         std::max({out.width, out.height, out.depth}) > 2048))
        return Fail(error, "invalid volume dimensions"), false;
    return true;
}
}  // namespace

bool ParseDds(std::vector<uint8_t>&& file, DdsImage& out, std::string* error) {
    const size_t size = file.size();
    size_t offset = 0;
    if (!ParseHeader(file.data(), size, out, offset, error)) return false;

    const uint32_t slices = out.arraySize * (out.isCubemap ? 6 : 1);
    for (uint32_t item = 0; item < slices; ++item) {
        for (uint32_t mip = 0; mip < out.mipLevels; ++mip) {
            SurfaceLayout layout;
            ComputeSurfaceLayout(out.format, out.width >> mip, out.height >> mip, layout);
            const uint32_t d = out.isVolume ? std::max(1u, out.depth >> mip) : 1;
            const size_t bytes = layout.size() * d;
            if (bytes > size - offset || layout.size() > UINT32_MAX) return Fail(error, "truncated pixel data"), false;
            out.subresources.push_back({offset, layout.rowPitch, uint32_t(layout.size())});
            offset += bytes;
        }
    }

    out.data = std::move(file);
    return true;
}

bool LoadTextureFile(const std::wstring& path, DdsImage& out, bool* wasEncrypted, std::string* error) {
    std::ifstream in(path.c_str(), std::ios::binary | std::ios::ate);
    if (!in) return Fail(error, "cannot open file"), false;
    const std::streamoff len = in.tellg();
    if (len <= 0 || len > (512ll << 20)) return Fail(error, "bad file size"), false;
    std::vector<uint8_t> bytes(static_cast<size_t>(len));
    in.seekg(0);
    if (!in.read(reinterpret_cast<char*>(bytes.data()), len)) return Fail(error, "read failed"), false;

    const bool encrypted = IsEncryptedTexture(bytes.data(), bytes.size());
    if (wasEncrypted) *wasEncrypted = encrypted;
    if (encrypted) {
        std::vector<uint8_t> plain;
        if (!DecryptTexture(bytes.data(), bytes.size(), plain)) return Fail(error, "bad resorep1 header"), false;
        bytes.swap(plain);
    }
    return ParseDds(std::move(bytes), out, error);
}

bool LoadTextureMips(const std::wstring& path, uint32_t maxSize, DdsImage& out, bool* wasEncrypted,
                     std::string* error) {
    if (maxSize == 0) return LoadTextureFile(path, out, wasEncrypted, error);
    std::ifstream in(path.c_str(), std::ios::binary | std::ios::ate);
    if (!in) return Fail(error, "cannot open file"), false;
    const std::streamoff length = in.tellg();
    if (length <= 0) return Fail(error, "bad file size"), false;
    const uint64_t fileSize = uint64_t(length);
    in.seekg(0);
    std::vector<uint8_t> head(size_t(std::min<uint64_t>(fileSize, 512)));
    if (!in.read(reinterpret_cast<char*>(head.data()), std::streamsize(head.size())))
        return Fail(error, "read failed"), false;

    uint8_t iv[8] = {};
    size_t payload = 0;
    const bool encrypted = IsEncryptedTexture(head.data(), head.size());
    if (wasEncrypted) *wasEncrypted = encrypted;
    std::vector<uint8_t> plainHead = head;
    if (encrypted) {
        payload = EncryptedPayloadOffset(head.data(), head.size(), iv);
        if (!payload || !DecryptTexture(head.data(), head.size(), plainHead))
            return Fail(error, "bad resorep1 header"), false;
    }
    DdsImage meta;
    size_t offset = 0;
    if (!ParseHeader(plainHead.data(), plainHead.size(), meta, offset, error)) return false;
    if (meta.isVolume) return LoadTextureFile(path, out, wasEncrypted, error);

    // Largest mip within maxSize; for block-compressed formats the new top level must be
    // made of whole 4x4 blocks.
    const bool bc = GetFormatInfo(meta.format).blockCompressed;
    uint32_t start = 0;
    while (start + 1 < meta.mipLevels && std::max(meta.width >> start, meta.height >> start) > maxSize) ++start;
    while (bc && start > 0 && (((meta.width >> start) % 4) || ((meta.height >> start) % 4))) --start;

    std::vector<size_t> mipOffset;
    size_t end = offset;
    for (uint32_t mip = 0; mip < meta.mipLevels; ++mip) {
        SurfaceLayout layout;
        ComputeSurfaceLayout(meta.format, meta.width >> mip, meta.height >> mip, layout);
        mipOffset.push_back(end);
        end += layout.size();
    }
    const size_t begin = mipOffset[start];
    const uint64_t available = encrypted ? fileSize - payload : fileSize;
    if (end > available) return Fail(error, "truncated pixel data"), false;

    std::vector<uint8_t> data(end - begin);
    if (!encrypted) {
        in.seekg(std::streamoff(begin));
        in.read(reinterpret_cast<char*>(data.data()), std::streamsize(data.size()));
    } else {
        // Whole cipher blocks covering [begin, end), plus the block before for chaining.
        const size_t blockBegin = begin & ~size_t(7);
        const size_t blockEnd = std::min<size_t>((end + 7) & ~size_t(7), size_t(available));
        uint8_t prev[8];
        if (blockBegin == 0) {
            std::memcpy(prev, iv, 8);
        } else {
            in.seekg(std::streamoff(payload + blockBegin - 8));
            in.read(reinterpret_cast<char*>(prev), 8);
        }
        std::vector<uint8_t> buf(blockEnd - blockBegin);
        in.seekg(std::streamoff(payload + blockBegin));
        in.read(reinterpret_cast<char*>(buf.data()), std::streamsize(buf.size()));
        DecryptTextureRange(prev, buf.data(), buf.size());  // a partial tail block stays as stored
        std::memcpy(data.data(), buf.data() + (begin - blockBegin), data.size());
    }
    if (!in) return Fail(error, "read failed"), false;

    out = DdsImage{};
    out.width = std::max(1u, meta.width >> start);
    out.height = std::max(1u, meta.height >> start);
    out.mipLevels = meta.mipLevels - start;
    out.format = meta.format;
    for (uint32_t mip = start; mip < meta.mipLevels; ++mip) {
        SurfaceLayout layout;
        ComputeSurfaceLayout(meta.format, meta.width >> mip, meta.height >> mip, layout);
        out.subresources.push_back({mipOffset[mip] - begin, layout.rowPitch, uint32_t(layout.size())});
    }
    out.data = std::move(data);
    return true;
}

DXGI_FORMAT ShaderViewFormat(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_UNORM;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
        case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_R16G16_TYPELESS: return DXGI_FORMAT_R16G16_UNORM;
        case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_R32_FLOAT;
        case DXGI_FORMAT_R8G8_TYPELESS: return DXGI_FORMAT_R8G8_UNORM;
        case DXGI_FORMAT_R16_TYPELESS: return DXGI_FORMAT_R16_UNORM;
        case DXGI_FORMAT_R8_TYPELESS: return DXGI_FORMAT_R8_UNORM;
        case DXGI_FORMAT_BC1_TYPELESS: return DXGI_FORMAT_BC1_UNORM;
        case DXGI_FORMAT_BC2_TYPELESS: return DXGI_FORMAT_BC2_UNORM;
        case DXGI_FORMAT_BC3_TYPELESS: return DXGI_FORMAT_BC3_UNORM;
        case DXGI_FORMAT_BC4_TYPELESS: return DXGI_FORMAT_BC4_UNORM;
        case DXGI_FORMAT_BC5_TYPELESS: return DXGI_FORMAT_BC5_UNORM;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_B8G8R8X8_TYPELESS: return DXGI_FORMAT_B8G8R8X8_UNORM;
        case DXGI_FORMAT_BC6H_TYPELESS: return DXGI_FORMAT_BC6H_UF16;
        case DXGI_FORMAT_BC7_TYPELESS: return DXGI_FORMAT_BC7_UNORM;
        default: return f;
    }
}

bool IsSnormFormat(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R16G16B16A16_SNORM:
        case DXGI_FORMAT_R8G8B8A8_SNORM:
        case DXGI_FORMAT_R16G16_SNORM:
        case DXGI_FORMAT_R8G8_SNORM:
        case DXGI_FORMAT_R16_SNORM:
        case DXGI_FORMAT_R8_SNORM:
        case DXGI_FORMAT_BC4_SNORM:
        case DXGI_FORMAT_BC5_SNORM: return true;
        default: return false;
    }
}

bool ReadTextureInfo(const std::wstring& path, DdsImage& out, bool* wasEncrypted) {
    // The header (and DX10 extension) fits in the first 148 bytes; an encrypted pack adds
    // its own header in front, so read a little more and decrypt just that.
    std::ifstream in(path.c_str(), std::ios::binary);
    if (!in) return false;
    std::vector<uint8_t> bytes(512);  // also fits the longest resorep1 IV and a full DX10 header
    in.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size()));
    bytes.resize(size_t(in.gcount()));

    const bool encrypted = IsEncryptedTexture(bytes.data(), bytes.size());
    if (wasEncrypted) *wasEncrypted = encrypted;
    if (encrypted) {
        std::vector<uint8_t> plain;
        if (!DecryptTexture(bytes.data(), bytes.size(), plain)) return false;
        bytes.swap(plain);
    }
    size_t offset = 0;
    return ParseHeader(bytes.data(), bytes.size(), out, offset, nullptr);
}

std::vector<uint8_t> WriteDds(DXGI_FORMAT format, uint32_t width, uint32_t height,
                              const std::vector<MipData>& mips) {
    DdsHeader hdr{};
    hdr.size = sizeof(DdsHeader);
    hdr.flags = DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PIXELFORMAT;
    hdr.width = width;
    hdr.height = height;
    hdr.mipMapCount = uint32_t(mips.size());
    hdr.caps = DDSCAPS_TEXTURE;
    if (mips.size() > 1) {
        hdr.flags |= DDSD_MIPMAPCOUNT;
        hdr.caps |= DDSCAPS_COMPLEX | DDSCAPS_MIPMAP;
    }
    hdr.ddspf.size = sizeof(DdsPixelFormat);

    SurfaceLayout top;
    ComputeSurfaceLayout(format, width, height, top);
    const bool bc = GetFormatInfo(format).blockCompressed;
    hdr.flags |= bc ? DDSD_LINEARSIZE : DDSD_PITCH;
    hdr.pitchOrLinearSize = bc ? uint32_t(top.size()) : top.rowPitch;

    bool dx10 = false;
    auto rgb = [&](uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
        hdr.ddspf.flags = DDPF_RGB | (a ? DDPF_ALPHAPIXELS : 0);
        hdr.ddspf.rgbBitCount = 32;
        hdr.ddspf.rMask = r;
        hdr.ddspf.gMask = g;
        hdr.ddspf.bMask = b;
        hdr.ddspf.aMask = a;
    };
    auto cc = [&](uint32_t code) {
        hdr.ddspf.flags = DDPF_FOURCC;
        hdr.ddspf.fourCC = code;
    };
    switch (format) {
        case DXGI_FORMAT_BC1_UNORM: cc(FourCC('D', 'X', 'T', '1')); break;
        case DXGI_FORMAT_BC2_UNORM: cc(FourCC('D', 'X', 'T', '3')); break;
        case DXGI_FORMAT_BC3_UNORM: cc(FourCC('D', 'X', 'T', '5')); break;
        case DXGI_FORMAT_BC4_UNORM: cc(FourCC('A', 'T', 'I', '1')); break;
        case DXGI_FORMAT_BC5_UNORM: cc(FourCC('A', 'T', 'I', '2')); break;
        case DXGI_FORMAT_R8G8B8A8_UNORM: rgb(0xff, 0xff00, 0xff0000, 0xff000000); break;
        case DXGI_FORMAT_B8G8R8A8_UNORM: rgb(0xff0000, 0xff00, 0xff, 0xff000000); break;
        case DXGI_FORMAT_B8G8R8X8_UNORM: rgb(0xff0000, 0xff00, 0xff, 0); break;
        default: cc(FourCC('D', 'X', '1', '0')); dx10 = true; break;
    }

    std::vector<uint8_t> out;
    auto append = [&](const void* p, size_t n) {
        const uint8_t* b = static_cast<const uint8_t*>(p);
        out.insert(out.end(), b, b + n);
    };
    append(&kDdsMagic, 4);
    append(&hdr, sizeof(hdr));
    if (dx10) {
        DdsHeaderDx10 ext{uint32_t(format), kDimTexture2D, 0, 1, 0};
        append(&ext, sizeof(ext));
    }
    for (size_t mip = 0; mip < mips.size(); ++mip) {
        SurfaceLayout layout;
        ComputeSurfaceLayout(format, width >> mip, height >> mip, layout);
        const uint8_t* src = static_cast<const uint8_t*>(mips[mip].data);
        const uint32_t copy = std::min(layout.rowPitch, mips[mip].rowPitch);
        for (uint32_t row = 0; row < layout.rows; ++row) {
            append(src + size_t(row) * mips[mip].rowPitch, copy);
            out.resize(out.size() + (layout.rowPitch - copy), 0);
        }
    }
    return out;
}

}  // namespace resorep
