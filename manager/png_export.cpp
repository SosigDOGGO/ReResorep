#include "png_export.h"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "analyzer.h"
#include "dds.h"
#include "settings.h"

namespace resorep::ui {
namespace {
using Microsoft::WRL::ComPtr;

// Every texel copied as it is: one full-screen triangle, Load() instead of sampling.
const char kShader[] = R"(
Texture2D tex : register(t0);
float4 vs(uint id : SV_VertexID) : SV_Position {
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 ps(float4 pos : SV_Position) : SV_Target {
    return tex.Load(int3(pos.xy, 0));
}
)";

// The same bytes read without sRGB decoding, so the PNG gets exactly the stored values.
DXGI_FORMAT WithoutSrgb(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_BC1_UNORM_SRGB: return DXGI_FORMAT_BC1_UNORM;
        case DXGI_FORMAT_BC2_UNORM_SRGB: return DXGI_FORMAT_BC2_UNORM;
        case DXGI_FORMAT_BC3_UNORM_SRGB: return DXGI_FORMAT_BC3_UNORM;
        case DXGI_FORMAT_BC7_UNORM_SRGB: return DXGI_FORMAT_BC7_UNORM;
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8X8_UNORM;
        default: return f;
    }
}

uint8_t ToByte(float v) { return uint8_t(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); }

std::string WritePng(const std::filesystem::path& path, uint32_t width, uint32_t height, std::vector<uint8_t>& px) {
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;  // the PNG encoder's native order
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
        FAILED(factory->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)))
        return "Could not create " + WideToUtf8(path.filename().wstring()) + ".";
    const bool ok = SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
                    SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
                    SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
                    SUCCEEDED(frame->SetSize(width, height)) && SUCCEEDED(frame->SetPixelFormat(&format)) &&
                    format == GUID_WICPixelFormat32bppBGRA &&
                    SUCCEEDED(frame->WritePixels(height, width * 4, UINT(px.size()), px.data())) &&
                    SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
    return ok ? std::string() : "Could not write " + WideToUtf8(path.filename().wstring()) + ".";
}

}  // namespace

std::string DecodeMips(const DdsImage& img, uint32_t mips, std::vector<std::vector<float>>& out) {
    mips = std::min(mips, img.mipLevels);
    if (img.subresources.size() < mips || !mips) return "This texture has no image data.";
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION,
                                   &device, nullptr, &context);
    if (FAILED(hr))
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &device,
                               nullptr, &context);
    if (FAILED(hr)) return "Could not start Direct3D to decode the texture.";

    ComPtr<ID3DBlob> vsBlob, psBlob;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    if (FAILED(D3DCompile(kShader, sizeof(kShader) - 1, nullptr, nullptr, nullptr, "vs", "vs_4_0", 0, 0, &vsBlob,
                          nullptr)) ||
        FAILED(D3DCompile(kShader, sizeof(kShader) - 1, nullptr, nullptr, nullptr, "ps", "ps_4_0", 0, 0, &psBlob,
                          nullptr)) ||
        FAILED(device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &vs)) ||
        FAILED(device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &ps)))
        return "Could not prepare the texture decoder.";

    // The first slice's mips are the first subresources.
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = img.width;
    desc.Height = img.height;
    desc.MipLevels = mips;
    desc.ArraySize = 1;
    desc.Format = WithoutSrgb(ShaderViewFormat(img.format));
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    std::vector<D3D11_SUBRESOURCE_DATA> data;
    for (uint32_t m = 0; m < mips; ++m) {
        const DdsSubresource& s = img.subresources[m];
        data.push_back({img.data.data() + s.offset, s.rowPitch, s.slicePitch});
    }
    ComPtr<ID3D11Texture2D> tex;
    if (FAILED(device->CreateTexture2D(&desc, data.data(), &tex)))
        return std::string("This texture's format (") + FormatName(img.format) + ") can't be decoded on this PC.";

    out.assign(mips, {});
    for (uint32_t m = 0; m < mips; ++m) {
        const uint32_t w = std::max(1u, img.width >> m), h = std::max(1u, img.height >> m);
        // A view of just this mip, so Load(pos, 0) reads it.
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = desc.Format;
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MostDetailedMip = m;
        sd.Texture2D.MipLevels = 1;
        ComPtr<ID3D11ShaderResourceView> srv;
        if (FAILED(device->CreateShaderResourceView(tex.Get(), &sd, &srv)))
            return std::string("This texture's format (") + FormatName(img.format) + ") can't be decoded on this PC.";

        D3D11_TEXTURE2D_DESC td{w, h, 1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT, {1, 0}, D3D11_USAGE_DEFAULT,
                                D3D11_BIND_RENDER_TARGET};
        ComPtr<ID3D11Texture2D> target, staging;
        ComPtr<ID3D11RenderTargetView> rtv;
        if (FAILED(device->CreateTexture2D(&td, nullptr, &target)) ||
            FAILED(device->CreateRenderTargetView(target.Get(), nullptr, &rtv)))
            return "Not enough video memory to decode this texture.";
        td.Usage = D3D11_USAGE_STAGING;
        td.BindFlags = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(device->CreateTexture2D(&td, nullptr, &staging))) return "Not enough memory to decode this texture.";

        const D3D11_VIEWPORT vp{0, 0, float(w), float(h), 0, 1};
        context->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
        context->RSSetViewports(1, &vp);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vs.Get(), nullptr, 0);
        context->PSSetShader(ps.Get(), nullptr, 0);
        context->PSSetShaderResources(0, 1, srv.GetAddressOf());
        context->Draw(3, 0);
        context->CopyResource(staging.Get(), target.Get());

        D3D11_MAPPED_SUBRESOURCE mapped;
        if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
            return "Could not read the decoded texture.";
        const size_t rowFloats = size_t(w) * 4;
        std::vector<float>& rgba = out[m];
        rgba.resize(rowFloats * h);
        for (uint32_t y = 0; y < h; ++y)
            std::memcpy(&rgba[y * rowFloats], static_cast<const uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch,
                        rowFloats * sizeof(float));
        context->Unmap(staging.Get(), 0);
    }
    return {};
}

std::string ExportPng(const std::filesystem::path& texture, const std::filesystem::path& png, int quarterTurns,
                      bool mirror) {
    DdsImage img;
    bool encrypted = false;
    std::string error;
    if (!LoadTextureFile(texture.wstring(), img, &encrypted, &error))
        return error.empty() ? "Could not read " + WideToUtf8(texture.filename().wstring()) + "." : error;
    if (encrypted) return "Encrypted (resorep1) textures can't be exported.";
    if (img.isVolume) return "3D (volume) textures can't be exported as PNG.";
    if (img.subresources.empty() || !img.width || !img.height) return "This texture has no image data.";

    std::vector<std::vector<float>> decoded;
    if (std::string e = DecodeMips(img, 1, decoded); !e.empty()) return e;
    const std::vector<float>& rgba = decoded[0];

    const int channels = ChannelCount(img.format);
    const bool snorm = IsSnormFormat(img.format);
    const bool alphaOnly = img.format == DXGI_FORMAT_A8_UNORM;
    std::vector<uint8_t> px(size_t(img.width) * img.height * 4);
    for (size_t i = 0; i < px.size() / 4; ++i) {
        float c[4] = {rgba[i * 4], rgba[i * 4 + 1], rgba[i * 4 + 2], rgba[i * 4 + 3]};
        if (snorm)
            for (float& v : c) v = (v + 1.0f) * 0.5f;
        if (channels == 1) {
            const float v = alphaOnly ? c[3] : c[0];
            c[0] = c[1] = c[2] = v;
            c[3] = 1.0f;
        } else if (channels == 2) {
            // Two-channel normal map: z = sqrt(1 - x^2 - y^2), stored like the other two.
            const float x = c[0] * 2 - 1, y = c[1] * 2 - 1;
            c[2] = std::sqrt(std::max(0.0f, 1.0f - x * x - y * y)) * 0.5f + 0.5f;
            c[3] = 1.0f;
        }
        px[i * 4 + 0] = ToByte(c[2]);  // BGRA
        px[i * 4 + 1] = ToByte(c[1]);
        px[i * 4 + 2] = ToByte(c[0]);
        px[i * 4 + 3] = ToByte(c[3]);
    }

    const int k = quarterTurns & 3;
    if (k == 0 && !mirror) return WritePng(png, img.width, img.height, px);
    // Each output pixel reads the source pixel that the turn (and flip) puts there.
    const uint32_t w = img.width, h = img.height;
    const uint32_t outW = k % 2 ? h : w, outH = k % 2 ? w : h;
    std::vector<uint8_t> turned(px.size());
    for (uint32_t y = 0; y < outH; ++y) {
        for (uint32_t x0 = 0; x0 < outW; ++x0) {
            const uint32_t x = mirror ? outW - 1 - x0 : x0;
            uint32_t sx = x, sy = y;
            if (k == 1) sx = y, sy = h - 1 - x;
            else if (k == 2) sx = w - 1 - x, sy = h - 1 - y;
            else if (k == 3) sx = w - 1 - y, sy = x;
            std::memcpy(&turned[(size_t(y) * outW + x0) * 4], &px[(size_t(sy) * w + sx) * 4], 4);
        }
    }
    return WritePng(png, outW, outH, turned);
}

}  // namespace resorep::ui
