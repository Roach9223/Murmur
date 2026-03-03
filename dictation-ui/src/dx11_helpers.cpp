#include "dx11_helpers.h"

namespace DX11 {

bool CreateDevice(HWND hwnd,
                  ID3D11Device** ppDevice,
                  ID3D11DeviceContext** ppContext,
                  IDXGISwapChain** ppSwapChain,
                  ID3D11RenderTargetView** ppRTV)
{
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[] = {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_0,
    };

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags,
        featureLevelArray, 2, D3D11_SDK_VERSION, &sd,
        ppSwapChain, ppDevice, &featureLevel, ppContext);

    if (hr == DXGI_ERROR_UNSUPPORTED) {
        // Try WARP (software) driver
        hr = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, createDeviceFlags,
            featureLevelArray, 2, D3D11_SDK_VERSION, &sd,
            ppSwapChain, ppDevice, &featureLevel, ppContext);
    }

    if (FAILED(hr))
        return false;

    CreateRenderTarget(*ppDevice, *ppSwapChain, ppRTV);
    return true;
}

void CreateRenderTarget(ID3D11Device* pDevice,
                        IDXGISwapChain* pSwapChain,
                        ID3D11RenderTargetView** ppRTV)
{
    ID3D11Texture2D* pBackBuffer = nullptr;
    pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    if (pBackBuffer) {
        pDevice->CreateRenderTargetView(pBackBuffer, nullptr, ppRTV);
        pBackBuffer->Release();
    }
}

void CleanupRenderTarget(ID3D11RenderTargetView** ppRTV)
{
    if (*ppRTV) {
        (*ppRTV)->Release();
        *ppRTV = nullptr;
    }
}

void CleanupDevice(ID3D11Device** ppDevice,
                   ID3D11DeviceContext** ppContext,
                   IDXGISwapChain** ppSwapChain,
                   ID3D11RenderTargetView** ppRTV)
{
    CleanupRenderTarget(ppRTV);
    if (*ppSwapChain)   { (*ppSwapChain)->Release();   *ppSwapChain = nullptr; }
    if (*ppContext)      { (*ppContext)->Release();      *ppContext = nullptr; }
    if (*ppDevice)       { (*ppDevice)->Release();       *ppDevice = nullptr; }
}

}  // namespace DX11
