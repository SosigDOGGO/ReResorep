// ReResorep texture manager: Win32 + D3D11 + Dear ImGui.
#include <windows.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <wrl/client.h>

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include <filesystem>
#include <memory>
#include <vector>

#include "app.h"
#include "integrity.h"
#include "updater.h"
#include "version.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace {
using Microsoft::WRL::ComPtr;

ComPtr<ID3D11Device> g_device;
ComPtr<ID3D11DeviceContext> g_context;
ComPtr<IDXGISwapChain> g_swapChain;
ComPtr<ID3D11RenderTargetView> g_rtv;
UINT g_resizeWidth = 0, g_resizeHeight = 0;
resorep::ui::App* g_app = nullptr;
bool g_sizing = false;   // the user is dragging the window's edge (Windows' own modal loop runs)
bool g_inFrame = false;  // RenderFrame is running; a dialog it opens can send WM_SIZE

bool CreateDevice(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2,
                                               D3D11_SDK_VERSION, &sd, &g_swapChain, &g_device, nullptr, &g_context);
    if (hr == DXGI_ERROR_UNSUPPORTED)  // no usable GPU: software rasteriser
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2, D3D11_SDK_VERSION,
                                           &sd, &g_swapChain, &g_device, nullptr, &g_context);
    return SUCCEEDED(hr);
}

void CreateRenderTarget() {
    ComPtr<ID3D11Texture2D> back;
    g_swapChain->GetBuffer(0, IID_PPV_ARGS(&back));
    g_device->CreateRenderTargetView(back.Get(), nullptr, &g_rtv);
}

std::vector<std::filesystem::path> DroppedFiles(HDROP drop) {
    std::vector<std::filesystem::path> files;
    const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    for (UINT i = 0; i < count; ++i) {
        std::wstring name(DragQueryFileW(drop, i, nullptr, 0) + 1, L'\0');
        DragQueryFileW(drop, i, name.data(), UINT(name.size()));
        name.resize(name.size() - 1);
        files.emplace_back(name);
    }
    DragFinish(drop);
    return files;
}

// Gives the window its icons at the exact pixel size for its monitor's DPI. The class
// icon alone is loaded at 96-DPI size, which the taskbar then shows too small.
void SetWindowIcons(HWND hwnd) {
    static HICON bigIcon = nullptr, smallIcon = nullptr;
    const UINT dpi = GetDpiForWindow(hwnd);
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    auto load = [&](int metricX, int metricY) {
        return static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                             GetSystemMetricsForDpi(metricX, dpi),
                                             GetSystemMetricsForDpi(metricY, dpi), LR_DEFAULTCOLOR));
    };
    HICON newBig = load(SM_CXICON, SM_CYICON), newSmall = load(SM_CXSMICON, SM_CYSMICON);
    SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(newBig));
    SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(newSmall));
    if (bigIcon) DestroyIcon(bigIcon);
    if (smallIcon) DestroyIcon(smallIcon);
    bigIcon = newBig;
    smallIcon = newSmall;
}

// Resizes the swap chain if the window changed size, then draws and shows one frame.
void RenderFrame() {
    if (!g_app || g_inFrame) return;
    g_inFrame = true;
    if (g_resizeWidth && g_resizeHeight) {
        g_rtv.Reset();
        g_swapChain->ResizeBuffers(0, g_resizeWidth, g_resizeHeight, DXGI_FORMAT_UNKNOWN, 0);
        g_resizeWidth = g_resizeHeight = 0;
        CreateRenderTarget();
    }

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    g_app->Frame();
    ImGui::Render();

    const float clear[4] = {0.09f, 0.09f, 0.10f, 1.0f};
    g_context->OMSetRenderTargets(1, g_rtv.GetAddressOf(), nullptr);
    g_context->ClearRenderTargetView(g_rtv.Get(), clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    g_swapChain->Present(1, 0);  // vsync keeps the idle UI cheap
    g_inFrame = false;
}

LRESULT WINAPI WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam)) return true;
    switch (msg) {
        case WM_DPICHANGED:
            SetWindowIcons(hwnd);
            break;
        case WM_ENTERSIZEMOVE:
            g_sizing = true;
            return 0;
        case WM_EXITSIZEMOVE:
            g_sizing = false;
            return 0;
        case WM_SIZE:
            if (wParam != SIZE_MINIMIZED) {
                g_resizeWidth = LOWORD(lParam);
                g_resizeHeight = HIWORD(lParam);
                // While the edge is dragged the main loop is stuck in Windows' sizing loop, and the
                // last frame would just be stretched until the mouse is released.
                if (g_sizing) RenderFrame();
            }
            return 0;
        case WM_GETMINMAXINFO:
            reinterpret_cast<MINMAXINFO*>(lParam)->ptMinTrackSize = {900, 560};
            return 0;
        case WM_DROPFILES:
            if (g_app) g_app->OnDropFiles(DroppedFiles(reinterpret_cast<HDROP>(wParam)));
            return 0;
        case WM_SYSCOMMAND:
            if ((wParam & 0xfff0) == SC_KEYMENU) return 0;  // no Alt menu beep
            break;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void SetupStyle(float scale) {
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 0.0f;
    style.FrameRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.TabRounding = 4.0f;
    style.ChildRounding = 4.0f;
    style.FramePadding = ImVec2(8, 5);
    style.ItemSpacing = ImVec2(8, 7);
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.09f, 0.09f, 0.10f, 1.0f);
    style.Colors[ImGuiCol_ChildBg] = ImVec4(0.11f, 0.11f, 0.12f, 1.0f);
    style.ScaleAllSizes(scale);

    ImGuiIO& io = ImGui::GetIO();
    wchar_t fonts[MAX_PATH];
    GetWindowsDirectoryW(fonts, MAX_PATH);
    const std::filesystem::path segoe = std::filesystem::path(fonts) / L"Fonts" / L"segoeui.ttf";
    std::error_code ec;
    if (std::filesystem::exists(segoe, ec))
        io.Fonts->AddFontFromFileTTF(segoe.string().c_str(), 17.0f * scale);
    else
        io.Fonts->AddFontDefault();
}
}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    // A damaged or modified copy doesn't start (core/integrity.h).
    if (const resorep::SealState seal = resorep::CheckOwnSeal(); seal != resorep::SealState::Valid) {
        MessageBoxW(nullptr,
                    seal == resorep::SealState::Unreadable
                        ? L"ReResorep could not read its own file to check it, so it won't start.\n\n"
                          L"Make sure ReResorep.exe isn't blocked by another program, then try again."
                        : L"ReResorep.exe is damaged or was modified, so it won't start.\n\n"
                          L"Download ReResorep again from its release page.",
                    L"ReResorep " RERESOREP_VERSION_WSTRING, MB_ICONERROR | MB_OK);
        return 1;
    }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    ImGui_ImplWin32_EnableDpiAwareness();

    WNDCLASSEXW wc{sizeof(wc), CS_CLASSDC, WndProc, 0, 0, instance};
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    wc.hIconSm = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                               GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"ReResorepManager";
    RegisterClassExW(&wc);

    const float scale = ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY));
    HWND hwnd = CreateWindowExW(WS_EX_ACCEPTFILES, wc.lpszClassName, L"ReResorep " RERESOREP_VERSION_WSTRING L" - Texture Manager", WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, int(1280 * scale), int(800 * scale), nullptr, nullptr,
                                instance, nullptr);
    SetWindowIcons(hwnd);
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));

    if (!CreateDevice(hwnd)) {
        MessageBoxW(nullptr, L"Could not create a Direct3D 11 device.", L"ReResorep", MB_ICONERROR);
        return 1;
    }
    CreateRenderTarget();
    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;  // layout is fixed; no imgui.ini clutter
    SetupStyle(scale);
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_device.Get(), g_context.Get());

    {
        auto app = std::make_unique<resorep::ui::App>(hwnd, g_device.Get());
        g_app = app.get();

        bool running = true;
        while (running) {
            MSG msg;
            // Block while minimised instead of spinning.
            if (IsIconic(hwnd)) WaitMessage();
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
                if (msg.message == WM_QUIT) running = false;
            }
            if (!running) break;
            RenderFrame();
        }
        g_app = nullptr;
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, instance);
    // After "Restart and update" in the About tab: start the new version now that this one is closed.
    if (const std::string error = resorep::ui::RunPendingRestart(); !error.empty())
        MessageBoxW(nullptr, L"ReResorep was updated, but the new version could not be started. Start it again yourself.",
                    L"ReResorep", MB_ICONWARNING | MB_OK);
    CoUninitialize();
    return 0;
}
