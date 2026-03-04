#include <windows.h>
#include <d3d11.h>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#include "dx11_helpers.h"
#include "app.h"
#include "engine_client.h"
#include "engine_process.h"

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

    // --- Murmur custom theme ---
    {
        ImGuiStyle& s = ImGui::GetStyle();

        // Geometry: subtle rounding, clean spacing
        s.WindowRounding    = 4.0f;
        s.ChildRounding     = 3.0f;
        s.FrameRounding     = 3.0f;
        s.PopupRounding     = 4.0f;
        s.ScrollbarRounding = 6.0f;
        s.GrabRounding      = 3.0f;
        s.TabRounding       = 3.0f;

        s.WindowPadding     = ImVec2(10, 10);
        s.FramePadding      = ImVec2(8, 4);
        s.ItemSpacing       = ImVec2(8, 5);
        s.ItemInnerSpacing  = ImVec2(6, 4);
        s.ScrollbarSize     = 12.0f;
        s.GrabMinSize       = 8.0f;

        s.WindowBorderSize  = 1.0f;
        s.ChildBorderSize   = 1.0f;
        s.PopupBorderSize   = 1.0f;
        s.FrameBorderSize   = 0.0f;
        s.TabBorderSize     = 0.0f;

        s.SeparatorTextBorderSize = 2.0f;

        // Colors: deep dark base with cyan/teal accent
        ImVec4* c = s.Colors;
        c[ImGuiCol_Text]                  = ImVec4(0.92f, 0.93f, 0.94f, 1.00f);
        c[ImGuiCol_TextDisabled]          = ImVec4(0.42f, 0.44f, 0.47f, 1.00f);
        c[ImGuiCol_WindowBg]              = ImVec4(0.06f, 0.06f, 0.07f, 1.00f);
        c[ImGuiCol_ChildBg]               = ImVec4(0.07f, 0.07f, 0.08f, 1.00f);
        c[ImGuiCol_PopupBg]               = ImVec4(0.08f, 0.08f, 0.09f, 0.98f);
        c[ImGuiCol_Border]                = ImVec4(0.18f, 0.19f, 0.22f, 0.60f);
        c[ImGuiCol_BorderShadow]          = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
        c[ImGuiCol_FrameBg]               = ImVec4(0.11f, 0.12f, 0.14f, 1.00f);
        c[ImGuiCol_FrameBgHovered]        = ImVec4(0.15f, 0.16f, 0.19f, 1.00f);
        c[ImGuiCol_FrameBgActive]         = ImVec4(0.18f, 0.20f, 0.24f, 1.00f);
        c[ImGuiCol_TitleBg]               = ImVec4(0.05f, 0.05f, 0.06f, 1.00f);
        c[ImGuiCol_TitleBgActive]         = ImVec4(0.07f, 0.07f, 0.09f, 1.00f);
        c[ImGuiCol_TitleBgCollapsed]      = ImVec4(0.05f, 0.05f, 0.06f, 0.50f);
        c[ImGuiCol_MenuBarBg]             = ImVec4(0.08f, 0.08f, 0.10f, 1.00f);
        c[ImGuiCol_ScrollbarBg]           = ImVec4(0.05f, 0.05f, 0.06f, 0.50f);
        c[ImGuiCol_ScrollbarGrab]         = ImVec4(0.22f, 0.23f, 0.26f, 1.00f);
        c[ImGuiCol_ScrollbarGrabHovered]  = ImVec4(0.30f, 0.32f, 0.36f, 1.00f);
        c[ImGuiCol_ScrollbarGrabActive]   = ImVec4(0.38f, 0.40f, 0.45f, 1.00f);
        c[ImGuiCol_CheckMark]             = ImVec4(0.24f, 0.78f, 0.78f, 1.00f);
        c[ImGuiCol_SliderGrab]            = ImVec4(0.24f, 0.70f, 0.70f, 1.00f);
        c[ImGuiCol_SliderGrabActive]      = ImVec4(0.30f, 0.85f, 0.85f, 1.00f);
        c[ImGuiCol_Button]                = ImVec4(0.13f, 0.14f, 0.17f, 1.00f);
        c[ImGuiCol_ButtonHovered]         = ImVec4(0.18f, 0.20f, 0.25f, 1.00f);
        c[ImGuiCol_ButtonActive]          = ImVec4(0.24f, 0.70f, 0.70f, 0.60f);
        c[ImGuiCol_Header]               = ImVec4(0.13f, 0.14f, 0.17f, 1.00f);
        c[ImGuiCol_HeaderHovered]         = ImVec4(0.18f, 0.20f, 0.25f, 1.00f);
        c[ImGuiCol_HeaderActive]          = ImVec4(0.22f, 0.24f, 0.30f, 1.00f);
        c[ImGuiCol_Separator]             = ImVec4(0.18f, 0.19f, 0.22f, 0.60f);
        c[ImGuiCol_SeparatorHovered]      = ImVec4(0.24f, 0.70f, 0.70f, 0.60f);
        c[ImGuiCol_SeparatorActive]       = ImVec4(0.24f, 0.78f, 0.78f, 1.00f);
        c[ImGuiCol_ResizeGrip]            = ImVec4(0.22f, 0.23f, 0.26f, 0.50f);
        c[ImGuiCol_ResizeGripHovered]     = ImVec4(0.24f, 0.70f, 0.70f, 0.60f);
        c[ImGuiCol_ResizeGripActive]      = ImVec4(0.24f, 0.78f, 0.78f, 1.00f);
        c[ImGuiCol_Tab]                   = ImVec4(0.11f, 0.12f, 0.14f, 1.00f);
        c[ImGuiCol_TabHovered]            = ImVec4(0.24f, 0.70f, 0.70f, 0.40f);
        c[ImGuiCol_TabSelected]           = ImVec4(0.18f, 0.50f, 0.50f, 1.00f);
        c[ImGuiCol_TableHeaderBg]         = ImVec4(0.10f, 0.11f, 0.13f, 1.00f);
        c[ImGuiCol_TableBorderStrong]     = ImVec4(0.18f, 0.19f, 0.22f, 1.00f);
        c[ImGuiCol_TableBorderLight]      = ImVec4(0.14f, 0.15f, 0.17f, 1.00f);
        c[ImGuiCol_TableRowBg]            = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
        c[ImGuiCol_TableRowBgAlt]         = ImVec4(0.10f, 0.10f, 0.12f, 0.40f);
        c[ImGuiCol_TextSelectedBg]        = ImVec4(0.24f, 0.70f, 0.70f, 0.30f);
        c[ImGuiCol_NavHighlight]          = ImVec4(0.24f, 0.78f, 0.78f, 1.00f);
        c[ImGuiCol_ModalWindowDimBg]      = ImVec4(0.00f, 0.00f, 0.00f, 0.60f);
    }

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
    std::string engineDir = EngineProcess::DiscoverEngineDir();
    EngineProcess engineProc(engineDir, 8899);

    // Create engine client
    EngineClient engine("127.0.0.1", 8899);

    // Auto-launch engine if not already running (non-blocking)
    if (!engineDir.empty() && !engine.PollHealthOnce()) {
        engineProc.Launch();
    }

    engine.StartPolling();

    DictationApp app(engine, engineProc);

    // Render loop
    ImVec4 clearColor = ImVec4(0.06f, 0.06f, 0.06f, 1.0f);
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

    // Cleanup
    engine.StopPolling();
    engine.Shutdown();
    engineProc.Terminate();

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    DX11::CleanupDevice(&g_pd3dDevice, &g_pd3dDeviceContext,
                        &g_pSwapChain, &g_mainRenderTargetView);

    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);

    return 0;
}
