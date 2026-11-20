#include "tint.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <vector>

#include "analyzer.h"
#include "bc_encode.h"
#include "dds.h"
#include "png_export.h"
#include "settings.h"

namespace resorep::ui {
namespace {

// h in 0..1 (turns), s and v 0..1.
void RgbToHsv(const float* c, float& h, float& s, float& v) {
    const float hi = std::max({c[0], c[1], c[2]}), lo = std::min({c[0], c[1], c[2]}), d = hi - lo;
    v = hi;
    s = hi > 0 ? d / hi : 0;
    if (d <= 0) h = 0;
    else if (hi == c[0]) h = std::fmod((c[1] - c[2]) / d + 6.0f, 6.0f) / 6.0f;
    else if (hi == c[1]) h = ((c[2] - c[0]) / d + 2.0f) / 6.0f;
    else h = ((c[0] - c[1]) / d + 4.0f) / 6.0f;
}

void HsvToRgb(float h, float s, float v, float* c) {
    for (int i = 0; i < 3; ++i) {
        // Each channel's distance round the hue circle from its own colour, as a ramp.
        const float k = std::fmod(5.0f - 2.0f * i + h * 6.0f, 6.0f);
        c[i] = v - v * s * std::clamp(std::min(k, 4.0f - k), 0.0f, 1.0f);
    }
}

void HslToRgb(float h, float s, float l, float* c) {
    const float a = s * std::min(l, 1 - l);
    for (int i = 0; i < 3; ++i) {
        const float k = std::fmod((i == 0 ? 0.0f : i == 1 ? 8.0f : 4.0f) + h * 12.0f, 12.0f);
        c[i] = l - a * std::clamp(std::min(k - 3, 9 - k), -1.0f, 1.0f);
    }
}

float Luma(const float* c) { return 0.299f * c[0] + 0.587f * c[1] + 0.114f * c[2]; }

}  // namespace

void TintPixels(const TintSettings& s, float* rgba, size_t count) {
    float th, ts, tv;
    RgbToHsv(s.color, th, ts, tv);
    for (size_t i = 0; i < count; ++i) {
        float* px = rgba + i * 4;
        float c[3];
        float h, sat, v;
        switch (s.mode) {
            case TintSettings::Mode::Recolor:
                RgbToHsv(px, h, sat, v);
                HsvToRgb(th, ts > 0.01f ? sat : 0.0f, v, c);  // a grey colour leaves just grey
                break;
            case TintSettings::Mode::Colorize: {
                const float hi = std::max({px[0], px[1], px[2]}), lo = std::min({px[0], px[1], px[2]});
                HslToRgb(th, ts, (hi + lo) / 2, c);
                break;
            }
            default:
                for (int k = 0; k < 3; ++k) c[k] = px[k] * s.color[k];
                break;
        }
        const float grey = Luma(c);
        for (int k = 0; k < 3; ++k) {
            const float adjusted = (grey + (c[k] - grey) * s.saturation) * s.brightness;
            px[k] = std::clamp(px[k] + (adjusted - px[k]) * s.strength, 0.0f, 1.0f);
        }
    }
}

const char* TintUnsupported(DXGI_FORMAT format) {
    if (ChannelCount(format) < 4) return "it has fewer than three colour channels (a mask or normal map)";
    if (!CanEncode(format)) return "its format can't be written back (only BC1, BC2, BC3, BC7 and 8-bit RGBA)";
    return nullptr;
}

std::string TintTexture(const std::filesystem::path& in, const std::filesystem::path& out, const TintSettings& s) {
    const std::string name = WideToUtf8(in.filename().wstring());
    DdsImage img;
    bool encrypted = false;
    std::string error;
    if (!LoadTextureFile(in.wstring(), img, &encrypted, &error))
        return name + ": " + (error.empty() ? "could not be read" : error);
    // Writing it back decrypted would give away a pack its author chose to protect.
    if (encrypted) return name + ": encrypted (resorep1) textures can't be tinted.";
    if (img.isVolume || img.isCubemap || img.arraySize != 1) return name + ": only plain 2D textures can be tinted.";
    if (const char* why = TintUnsupported(img.format)) return name + " can't be tinted: " + why + ".";

    std::vector<std::vector<float>> mips;
    if (std::string e = DecodeMips(img, img.mipLevels, mips); !e.empty()) return name + ": " + e;
    std::vector<std::vector<uint8_t>> encoded;
    std::vector<MipData> data;
    for (uint32_t m = 0; m < mips.size(); ++m) {
        const uint32_t w = std::max(1u, img.width >> m), h = std::max(1u, img.height >> m);
        TintPixels(s, mips[m].data(), size_t(w) * h);
        encoded.push_back(EncodeSurface(img.format, w, h, mips[m].data()));
        SurfaceLayout layout;
        ComputeSurfaceLayout(img.format, w, h, layout);
        data.push_back({encoded.back().data(), layout.rowPitch});
    }
    const std::vector<uint8_t> dds = WriteDds(img.format, img.width, img.height, data);
    std::ofstream file(out, std::ios::binary | std::ios::trunc);
    if (!file.write(reinterpret_cast<const char*>(dds.data()), std::streamsize(dds.size())) || !file.flush())
        return name + ": could not write the tinted texture.";
    return {};
}

}  // namespace resorep::ui
