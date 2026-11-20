#include "analyzer.h"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <vector>

#include "dds.h"

namespace resorep::ui {
namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;

namespace {
constexpr int kSample = 32;   // GPU sample size
constexpr int kDetail = 16;   // fingerprint size
constexpr uint32_t kLoadMax = 64;  // largest mip read from disk for analysis
constexpr char kCacheMagic[4] = {'R', 'R', 'A', '8'};  // bump when classification changes

}  // namespace

int ChannelCount(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R8_UNORM: case DXGI_FORMAT_R8_SNORM: case DXGI_FORMAT_R8_TYPELESS:
        case DXGI_FORMAT_R16_UNORM: case DXGI_FORMAT_R16_SNORM: case DXGI_FORMAT_R16_FLOAT:
        case DXGI_FORMAT_R16_TYPELESS: case DXGI_FORMAT_R32_FLOAT: case DXGI_FORMAT_R32_TYPELESS:
        case DXGI_FORMAT_BC4_UNORM: case DXGI_FORMAT_BC4_SNORM: case DXGI_FORMAT_BC4_TYPELESS:
        case DXGI_FORMAT_A8_UNORM:
            return 1;
        case DXGI_FORMAT_R8G8_UNORM: case DXGI_FORMAT_R8G8_SNORM: case DXGI_FORMAT_R8G8_TYPELESS:
        case DXGI_FORMAT_R16G16_UNORM: case DXGI_FORMAT_R16G16_SNORM: case DXGI_FORMAT_R16G16_FLOAT:
        case DXGI_FORMAT_R16G16_TYPELESS: case DXGI_FORMAT_BC5_UNORM: case DXGI_FORMAT_BC5_SNORM:
        case DXGI_FORMAT_BC5_TYPELESS:
            return 2;
        default:
            return 4;
    }
}

namespace {

const char kShader[] = R"(
Texture2D tex : register(t0);
SamplerState samp : register(s0);
cbuffer Params : register(b0) { float lod; float3 pad; };
float4 vs(uint id : SV_VertexID, out float2 uv : TEXCOORD0) : SV_Position {
    uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 ps(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    return tex.SampleLevel(samp, uv, lod);
}
)";
}  // namespace

const char* KindName(TextureKind kind) {
    switch (kind) {
        case TextureKind::Color: return "Color";
        case TextureKind::Normal: return "Normal map";
        case TextureKind::Grayscale: return "Depth / single channel (mask, roughness, height)";
        case TextureKind::Packed: return "Packed material map / ID mask";
        default: return "Not analysed yet";
    }
}

TextureTraits AnalyzePixels(const float* rgba, DXGI_FORMAT format) {
    constexpr int n = kSample * kSample;
    const int channels = ChannelCount(format);
    const bool snorm = IsSnormFormat(format);
    std::vector<float> px(rgba, rgba + n * 4);
    for (float& v : px) v = std::clamp(snorm ? (v + 1.0f) * 0.5f : v, 0.0f, 1.0f);

    TextureTraits t;
    std::vector<float> detail(n * 3);  // up to 3 values per pixel whose edges we fingerprint
    int planes = 1;
    bool detailIsSlope = false;

    if (channels == 1) {
        t.kind = TextureKind::Grayscale;
        const int c = format == DXGI_FORMAT_A8_UNORM ? 3 : 0;
        for (int i = 0; i < n; ++i) detail[i * 3] = px[i * 4 + c];
    } else {
        int unitVectors = 0, transparent = 0, blueFull = 0, redEmpty = 0, flat = 0, primary = 0;
        double meanX = 0, meanY = 0, meanB = 0, blueLift = 0;
        for (int i = 0; i < n; ++i) {
            const float r = px[i * 4], g = px[i * 4 + 1], b = px[i * 4 + 2];
            transparent += px[i * 4 + 3] < 0.9f;
            blueFull += b > 0.8f;
            blueLift += b - std::min(r, g);
            redEmpty += r < 0.16f;
            // Every channel off or on (black, white, pure red/green/blue...); "primary" if coloured.
            auto offOrOn = [](float c) { return c < 0.2f || c > 0.8f; };
            if (offOrOn(r) && offOrOn(g) && offOrOn(b)) {
                ++flat;
                primary += std::max({r, g, b}) - std::min({r, g, b}) > 0.6f;
            }
            const float x = r * 2 - 1, y = g * 2 - 1;
            meanX += x;
            meanY += y;
            meanB += b;
            if (channels == 2) {
                unitVectors += (x * x + y * y) <= 1.1f;  // z is reconstructed, so just |xy| <= 1
            } else {
                const float z = b * 2 - 1;
                const float len = std::sqrt(x * x + y * y + z * z);
                unitVectors += std::fabs(len - 1.0f) < 0.25f && z > 0.2f;
            }
        }
        meanX /= n;
        meanY /= n;
        meanB /= n;
        blueLift /= n;
        const bool centred = std::fabs(meanX) < 0.35 && std::fabs(meanY) < 0.35;
        if (channels == 2 && unitVectors > n * 0.95 && centred) t.kind = TextureKind::Normal;
        else if (channels == 4 && unitVectors > n * 0.9 && meanB > 0.6 && centred) t.kind = TextureKind::Normal;
        // Packed maps and masks, checked against ~5300 Siege dumps. None of them use alpha.
        // - Blue near full and well above red or green. White, grey and pale-pink art has blue
        //   near full too, but not above the others.
        // - Darker variants of the same: red empty almost everywhere, blue far above the rest.
        // - Material ID masks: nearly all pixels pure colours, a good part of them red/green/blue.
        else if (channels == 4 && transparent < n * 0.03 &&
                 ((blueFull >= n * 0.9 && blueLift > 0.25) || (redEmpty >= n * 0.7 && blueLift > 0.45) ||
                  (flat >= n * 0.85 && primary >= n * 0.3)))
            t.kind = TextureKind::Packed;
        // Colourless 4-channel textures are not grouped with depth maps: in Siege those are
        // grey art (concrete, metal, logos, icons). The game keeps its single-channel data
        // (depth, roughness, masks) in single-channel formats such as BC4, handled above.
        else t.kind = TextureKind::Color;

        // Colour: edges in each of R, G, B (on ~800 colour/normal pairs from Siege dumps this
        // matched normal maps better than luminance edges did). Normal maps: below.
        planes = 3;
        for (int i = 0; i < n; ++i)
            for (int c = 0; c < planes; ++c) detail[i * 3 + c] = px[i * 4 + c];
        detailIsSlope = t.kind == TextureKind::Normal;
    }

    // Colour/grayscale: edge strength (gradient magnitude). Normal maps: how far the surface
    // tilts, which is strongest where the colour texture has its edges.
    std::vector<float> structure(n);
    for (int y = 0; y < kSample; ++y) {
        for (int x = 0; x < kSample; ++x) {
            if (detailIsSlope) {
                const int i = (y * kSample + x) * 3;
                const float nx = detail[i] * 2 - 1, ny = detail[i + 1] * 2 - 1;
                structure[y * kSample + x] = std::sqrt(nx * nx + ny * ny);
                continue;
            }
            float sum = 0;
            for (int c = 0; c < planes; ++c) {
                auto at = [&](int xx, int yy) {
                    return detail[(std::clamp(yy, 0, kSample - 1) * kSample + std::clamp(xx, 0, kSample - 1)) * 3 + c];
                };
                const float gx = at(x + 1, y) - at(x - 1, y), gy = at(x, y + 1) - at(x, y - 1);
                sum += gx * gx + gy * gy;
            }
            structure[y * kSample + x] = std::sqrt(sum);
        }
    }
    float reduced[kDetail * kDetail];
    float peak = 0;
    for (int y = 0; y < kDetail; ++y) {
        for (int x = 0; x < kDetail; ++x) {
            const int s = (y * 2) * kSample + x * 2;
            const float v = (structure[s] + structure[s + 1] + structure[s + kSample] + structure[s + kSample + 1]) / 4;
            reduced[y * kDetail + x] = v;
            peak = std::max(peak, v);
        }
    }
    // Square root, so a few strong edges don't drown out the rest.
    for (int i = 0; i < kDetail * kDetail; ++i)
        t.detail[i] = peak > 0 ? uint8_t(std::lround(std::sqrt(reduced[i] / peak) * 255.0f)) : 0;

    if (t.kind == TextureKind::Color) {
        // Black Ice: at least 20% saturated cyan, 5% near-white and 2% near-black. Checked
        // against ~5300 Siege dumps together with the caller's size check: it picked out
        // the Black Ice weapon sheets, and not blue walls, teal UI art or icy loading screens
        // (those have no dark cracks, or are not power-of-two sized). Under 5% of strong colours
        // outside cyan to blue, so sheets that only have a cyan, white and navy part (charm and
        // sticker sheets with red or pink art beside it) stay out.
        int cyan = 0, white = 0, dark = 0, other = 0;
        for (int i = 0; i < n; ++i) {
            const float r = px[i * 4], g = px[i * 4 + 1], b = px[i * 4 + 2];
            const float v = std::max({r, g, b}), lo = std::min({r, g, b});
            const float s = v > 0 ? (v - lo) / v : 0;
            dark += v < 0.2f;
            white += v > 0.85f && s < 0.15f;
            if (s > 0.25f && v > 0.35f) {
                float h;  // hue in turns (0..1)
                if (v == r) h = (g - b) / (v - lo) / 6.0f;
                else if (v == g) h = (2.0f + (b - r) / (v - lo)) / 6.0f;
                else h = (4.0f + (r - g) / (v - lo)) / 6.0f;
                if (h < 0) h += 1.0f;
                cyan += h >= 0.45f && h <= 0.58f;
                other += h < 0.40f || h > 0.72f;
            }
        }
        t.icy = cyan >= n * 0.2 && white >= n * 0.05 && dark >= n * 0.02 && other < n * 0.05;
    }
    return t;
}

float DetailSimilarity(const TextureTraits& a, const TextureTraits& b) {
    constexpr int n = kDetail * kDetail;
    double ma = 0, mb = 0;
    for (int i = 0; i < n; ++i) {
        ma += a.detail[i];
        mb += b.detail[i];
    }
    ma /= n;
    mb /= n;
    double cov = 0, va = 0, vb = 0;
    for (int i = 0; i < n; ++i) {
        const double da = a.detail[i] - ma, db = b.detail[i] - mb;
        cov += da * db;
        va += da * da;
        vb += db * db;
    }
    if (va < n || vb < n) return 0.0f;  // (nearly) flat: nothing to compare
    return float(cov / std::sqrt(va * vb));
}

// --- GPU sampling -------------------------------------------------------------------

struct TextureAnalyzer::Gpu {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11Buffer> params;
    ComPtr<ID3D11Texture2D> target, staging;
    ComPtr<ID3D11RenderTargetView> rtv;

    bool Init(bool warp) {
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
        HRESULT hr = E_FAIL;
        if (!warp)
            hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION,
                                   &device, nullptr, &context);
        if (FAILED(hr))
            hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &device,
                                   nullptr, &context);
        if (FAILED(hr)) return false;

        ComPtr<ID3DBlob> vsBlob, psBlob;
        if (FAILED(D3DCompile(kShader, sizeof(kShader) - 1, nullptr, nullptr, nullptr, "vs", "vs_4_0", 0, 0, &vsBlob,
                              nullptr)) ||
            FAILED(D3DCompile(kShader, sizeof(kShader) - 1, nullptr, nullptr, nullptr, "ps", "ps_4_0", 0, 0, &psBlob,
                              nullptr)))
            return false;
        device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &vs);
        device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &ps);

        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        device->CreateSamplerState(&sd, &sampler);

        D3D11_BUFFER_DESC bd{16, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE};
        device->CreateBuffer(&bd, nullptr, &params);

        D3D11_TEXTURE2D_DESC td{kSample, kSample, 1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT, {1, 0},
                                D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET};
        device->CreateTexture2D(&td, nullptr, &target);
        device->CreateRenderTargetView(target.Get(), nullptr, &rtv);
        td.Usage = D3D11_USAGE_STAGING;
        td.BindFlags = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        device->CreateTexture2D(&td, nullptr, &staging);
        return vs && ps && sampler && params && rtv && staging;
    }

    bool Sample(const DdsImage& img, std::vector<float>& rgba) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = img.width;
        desc.Height = img.height;
        desc.MipLevels = img.mipLevels;
        desc.ArraySize = 1;
        desc.Format = img.format;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_IMMUTABLE;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        std::vector<D3D11_SUBRESOURCE_DATA> sub;
        for (uint32_t m = 0; m < img.mipLevels; ++m) {
            const DdsSubresource& s = img.subresources[m];
            sub.push_back({img.data.data() + s.offset, s.rowPitch, s.slicePitch});
        }
        ComPtr<ID3D11Texture2D> tex;
        ComPtr<ID3D11ShaderResourceView> srv;
        D3D11_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = ShaderViewFormat(img.format);
        view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        view.Texture2D.MipLevels = desc.MipLevels;
        if (FAILED(device->CreateTexture2D(&desc, sub.data(), &tex)) ||
            FAILED(device->CreateShaderResourceView(tex.Get(), &view, &srv)))
            return false;

        // Pick the mip closest to the sample size; bilinear filtering averages the rest.
        float lod = 0;
        for (uint32_t m = std::max(img.width, img.height); m > kSample * 1.5f && lod + 1 < img.mipLevels; m >>= 1) lod += 1;
        D3D11_MAPPED_SUBRESOURCE mapped;
        if (SUCCEEDED(context->Map(params.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            const float data[4] = {lod, 0, 0, 0};
            std::memcpy(mapped.pData, data, sizeof(data));
            context->Unmap(params.Get(), 0);
        }

        const D3D11_VIEWPORT vp{0, 0, float(kSample), float(kSample), 0, 1};
        context->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
        context->RSSetViewports(1, &vp);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->IASetInputLayout(nullptr);
        context->VSSetShader(vs.Get(), nullptr, 0);
        context->PSSetShader(ps.Get(), nullptr, 0);
        context->PSSetShaderResources(0, 1, srv.GetAddressOf());
        context->PSSetSamplers(0, 1, sampler.GetAddressOf());
        context->PSSetConstantBuffers(0, 1, params.GetAddressOf());
        context->Draw(3, 0);
        context->CopyResource(staging.Get(), target.Get());

        if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return false;
        rgba.resize(kSample * kSample * 4);
        for (int y = 0; y < kSample; ++y)
            std::memcpy(&rgba[y * kSample * 4], static_cast<const uint8_t*>(mapped.pData) + y * mapped.RowPitch,
                        kSample * 4 * sizeof(float));
        context->Unmap(staging.Get(), 0);
        return true;
    }
};

// --- Analyzer -----------------------------------------------------------------------

TextureAnalyzer::TextureAnalyzer(fs::path cacheFile, bool useWarp) : cacheFile_(std::move(cacheFile)), useWarp_(useWarp) {
    LoadCache();
    worker_ = std::thread([this] { Worker(); });
}

TextureAnalyzer::~TextureAnalyzer() {
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
    }
    wake_.notify_all();
    worker_.join();
    SaveCache();
}

TextureAnalyzer::Entry* TextureAnalyzer::Lookup(const fs::path& path, int64_t modified, uint64_t size) {
    auto [it, inserted] = entries_.try_emplace(path.wstring());
    Entry& e = it->second;
    if (inserted || e.modified != modified || e.size != size) {
        e = Entry{modified, size, false, {}};
        queue_.push_back(it->first);
        wake_.notify_one();
    }
    return &e;
}

TextureKind TextureAnalyzer::Kind(const fs::path& path, int64_t modified, uint64_t size) {
    std::lock_guard lock(mutex_);
    const Entry* e = Lookup(path, modified, size);
    return e->done ? e->traits.kind : TextureKind::Unknown;
}

std::optional<TextureTraits> TextureAnalyzer::Traits(const fs::path& path, int64_t modified, uint64_t size) {
    std::lock_guard lock(mutex_);
    const Entry* e = Lookup(path, modified, size);
    if (!e->done) return std::nullopt;
    return e->traits;
}

size_t TextureAnalyzer::Pending() {
    std::lock_guard lock(mutex_);
    return queue_.size();
}

bool TextureAnalyzer::AnalyzeNow(const fs::path& path, TextureTraits& out) {
    Gpu gpu;
    DdsImage img;
    std::vector<float> rgba;
    if (!gpu.Init(useWarp_) || !LoadTextureMips(path.wstring(), kLoadMax, img) || !gpu.Sample(img, rgba)) return false;
    out = AnalyzePixels(rgba.data(), img.format);
    return true;
}

void TextureAnalyzer::PruneMissing() {
    // Results for files that are gone (deleted dumps, renamed mods) would otherwise stay in the
    // cache file for good. Checked here, off the UI thread, once per start.
    std::vector<std::wstring> paths;
    {
        std::lock_guard lock(mutex_);
        for (const auto& [path, e] : entries_)
            if (e.done) paths.push_back(path);
    }
    std::vector<std::wstring> missing;
    for (const std::wstring& p : paths) {
        std::error_code ec;
        if (!fs::exists(p, ec) && !ec) missing.push_back(p);
    }
    if (missing.empty()) return;
    std::lock_guard lock(mutex_);
    for (const std::wstring& p : missing)
        if (auto it = entries_.find(p); it != entries_.end() && it->second.done) entries_.erase(it);
    unsaved_ += missing.size();
}

void TextureAnalyzer::Worker() {
    Gpu gpu;
    const bool gpuOk = gpu.Init(useWarp_);
    PruneMissing();
    for (;;) {
        std::wstring path;
        {
            std::unique_lock lock(mutex_);
            // While a game dumps textures one at a time the queue keeps running dry: results are
            // written once 250 have piled up, or after a few quiet seconds, not after every file.
            if (!wake_.wait_for(lock, std::chrono::seconds(5), [this] { return stop_ || !queue_.empty(); })) {
                if (unsaved_) {
                    lock.unlock();
                    SaveCache();
                }
                continue;
            }
            if (stop_) return;
            path = std::move(queue_.front());
            queue_.pop_front();
        }

        TextureTraits traits;  // stays Unknown when the file cannot be read or sampled
        DdsImage img;
        std::vector<float> rgba;
        if (gpuOk && LoadTextureMips(path, kLoadMax, img) && !img.isVolume && gpu.Sample(img, rgba))
            traits = AnalyzePixels(rgba.data(), img.format);

        bool save = false;
        {
            std::lock_guard lock(mutex_);
            auto it = entries_.find(path);
            if (it != entries_.end()) {
                it->second.traits = traits;
                it->second.done = true;
            }
            save = ++unsaved_ >= 250;
        }
        if (save) SaveCache();
    }
}

void TextureAnalyzer::LoadCache() {
    std::ifstream in(cacheFile_, std::ios::binary);
    char magic[4];
    if (!in.read(magic, 4) || std::memcmp(magic, kCacheMagic, 4) != 0) return;
    for (;;) {
        uint16_t len = 0;
        if (!in.read(reinterpret_cast<char*>(&len), 2)) break;
        std::wstring path(len, L'\0');
        Entry e{};
        uint8_t kind = 0;
        in.read(reinterpret_cast<char*>(path.data()), std::streamsize(len) * 2);
        in.read(reinterpret_cast<char*>(&e.modified), 8);
        in.read(reinterpret_cast<char*>(&e.size), 8);
        in.read(reinterpret_cast<char*>(&kind), 1);
        in.read(reinterpret_cast<char*>(e.traits.detail.data()), e.traits.detail.size());
        if (!in || (kind & 0x7F) > uint8_t(TextureKind::Packed)) break;
        e.traits.kind = TextureKind(kind & 0x7F);
        e.traits.icy = (kind & 0x80) != 0;
        e.done = true;
        entries_[path] = e;
    }
}

void TextureAnalyzer::SaveCache() {
    std::vector<char> out(kCacheMagic, kCacheMagic + 4);
    {
        std::lock_guard lock(mutex_);
        unsaved_ = 0;
        for (const auto& [path, e] : entries_) {
            if (!e.done || path.size() > 0xFFFF) continue;
            const uint16_t len = uint16_t(path.size());
            const uint8_t kind = uint8_t(e.traits.kind) | (e.traits.icy ? 0x80 : 0);  // top bit: icy
            auto put = [&](const void* p, size_t n) {
                out.insert(out.end(), static_cast<const char*>(p), static_cast<const char*>(p) + n);
            };
            put(&len, 2);
            put(path.data(), path.size() * 2);
            put(&e.modified, 8);
            put(&e.size, 8);
            put(&kind, 1);
            put(e.traits.detail.data(), e.traits.detail.size());
        }
    }
    const fs::path tmp = cacheFile_.wstring() + L".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f.write(out.data(), std::streamsize(out.size()));
        if (!f) return;
    }
    std::error_code ec;
    fs::rename(tmp, cacheFile_, ec);
}

}  // namespace resorep::ui
