#include <windows.h>
#include <d3d11.h>
#include <dwmapi.h>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#include "dx11_helpers.h"
#include "app.h"
#include "engine_client.h"
#include "engine_process.h"
#include "theme.h"

// Forward declare ImGui Win32 message handler
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Shared fonts (accessible from app.cpp)
ImFont* g_bannerFont = nullptr;

// DX11 globals
static ID3D11Device*           g_pd3dDevice = nullptr;
static ID3D11DeviceContext*    g_pd3dDeviceContext = nullptr;
static IDXGISwapChain*         g_pSwapChain = nullptr;
static ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;
static UINT                    g_ResizeWidth = 0;
static UINT                    g_ResizeHeight = 0;

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg)
    {
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED)
            return 0;
        g_ResizeWidth = LOWORD(lParam);
        g_ResizeHeight = HIWORD(lParam);
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU)
            return 0;
        break;
    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;
    }
    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}

int main(int, char**)
{
    // Register window class
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.hIcon = LoadIconW(wc.hInstance, L"IDI_ICON1");
    wc.hIconSm = LoadIconW(wc.hInstance, L"IDI_ICON1");
    wc.lpszClassName = L"MurmurClass";
    RegisterClassExW(&wc);

    // Create window
    HWND hwnd = CreateWindowExW(
        0, wc.lpszClassName, L"Murmur",
        WS_OVERLAPPEDWINDOW,
        100, 100, 1100, 825,
        nullptr, nullptr, wc.hInstance, nullptr);

    // Dark title bar to match the dark UI (no-op on pre-1809 Windows 10)
    {
        BOOL dark = TRUE;
        DwmSetWindowAttribute(hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/,
                              &dark, sizeof(dark));
    }

    // Initialize DX11
    if (!DX11::CreateDevice(hwnd, &g_pd3dDevice, &g_pd3dDeviceContext,
                            &g_pSwapChain, &g_mainRenderTargetView))
    {
        MessageBoxW(hwnd, L"Failed to create DirectX 11 device", L"Error", MB_OK);
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    // Setup Dear ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

    ImGui::StyleColorsDark();

    // --- Murmur theme: Classic (default) or Studio, persisted in ui_settings.json ---
    SetActiveTheme(LoadThemePreference());

    // --- Load fonts ---
    {
        // Primary UI font: Segoe UI (ships with Windows 10/11)
        ImFont* uiFont = io.Fonts->AddFontFromFileTTF(
            "C:\\Windows\\Fonts\\segoeui.ttf", 15.0f);
        if (!uiFont) {
            // Fallback: use default font with better size
            io.Fonts->AddFontDefault();
        }

        // Semibold banner font for the voice-to-type hero button
        g_bannerFont = io.Fonts->AddFontFromFileTTF(
            "C:\\Windows\\Fonts\\seguisb.ttf", 17.0f);
        if (!g_bannerFont)
            g_bannerFont = io.Fonts->Fonts[0];  // fallback to regular
    }

    // Setup backends
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    // Discover engine directory and create process manager
    std::wstring engineDir = EngineProcess::DiscoverEngineDirW();
    EngineProcess engineProc(engineDir, 8899);

    // Create engine client
    EngineClient engine("127.0.0.1", 8899);

    // Auto-launch engine if not already running (non-blocking)
    if (!engineDir.empty() && !engine.PollHealthOnce()) {
        engineProc.Launch();
    }

    engine.StartPolling();

    DictationApp app(engine, engineProc);

    // Render loop (clear color tracks the active theme each frame)
    bool running = true;

    while (running)
    {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT)
                running = false;
        }
        if (!running)
            break;

        // Handle resize
        if (g_ResizeWidth != 0 && g_ResizeHeight != 0)
        {
            DX11::CleanupRenderTarget(&g_mainRenderTargetView);
            g_pSwapChain->ResizeBuffers(0, g_ResizeWidth, g_ResizeHeight,
                                        DXGI_FORMAT_UNKNOWN, 0);
            g_ResizeWidth = g_ResizeHeight = 0;
            DX11::CreateRenderTarget(g_pd3dDevice, g_pSwapChain,
                                     &g_mainRenderTargetView);
        }

        // Start ImGui frame
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // Render UI
        app.Render();

        if (app.ShouldQuit())
            break;

        // Finalize
        ImGui::Render();
        const ImVec4 clearColor = g_theme.Bg0;
        const float cc[4] = {
            clearColor.x * clearColor.w,
            clearColor.y * clearColor.w,
            clearColor.z * clearColor.w,
            clearColor.w
        };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, cc);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        g_pSwapChain->Present(1, 0);  // vsync
    }

    // Cleanup — only shut down an engine we launched. If the user started
    // `python app.py --server` themselves, closing the UI must not kill it.
    engine.StopPolling();
    if (engineProc.Launched()) {
        engine.Shutdown();
        engineProc.Terminate();
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    DX11::CleanupDevice(&g_pd3dDevice, &g_pd3dDeviceContext,
                        &g_pSwapChain, &g_mainRenderTargetView);

    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);

    return 0;
}
