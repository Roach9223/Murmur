#pragma once

#include <d3d11.h>
#include <dxgi.h>

namespace DX11 {

bool CreateDevice(HWND hwnd,
                  ID3D11Device** ppDevice,
                  ID3D11DeviceContext** ppContext,
                  IDXGISwapChain** ppSwapChain,
                  ID3D11RenderTargetView** ppRTV);

void CreateRenderTarget(ID3D11Device* pDevice,
                        IDXGISwapChain* pSwapChain,
                        ID3D11RenderTargetView** ppRTV);

void CleanupRenderTarget(ID3D11RenderTargetView** ppRTV);

void CleanupDevice(ID3D11Device** ppDevice,
                   ID3D11DeviceContext** ppContext,
                   IDXGISwapChain** ppSwapChain,
                   ID3D11RenderTargetView** ppRTV);

}  // namespace DX11
