/*
 * Project : wiz3D
 * Copyright (C) wiz3D contributors 2026
 */
#include "NV3DOutputDX9.h"
#include "../lib/NV3D-Lib/NV3D-Lib-fresh/NV3DLib/include/NV3D.hpp"

namespace wiz3d {

NV3DOutputDX9::NV3DOutputDX9() = default;

NV3DOutputDX9::~NV3DOutputDX9()
{
    Shutdown();
}

bool NV3DOutputDX9::TryInit(IDirect3DDevice9* device, HWND hostHwnd, bool eyeSwap)
{
    if (m_iface) return true;
    if (m_initAttempted && !m_iface) return false;
    m_initAttempted = true;

    NV3D::InitParams params = {};
    params.host_hwnd         = hostHwnd;
    params.eye_swap          = eyeSwap;
    params.enable_lightboost = true;
    params.enable_suppressor = true;
    params.on_top            = true;

    HRESULT hr = NV3D::CreateInterfaceDX9(device, &params, &m_iface);
    if (FAILED(hr) || !m_iface)
    {
        m_iface = nullptr;
        return false;
    }
    return true;
}

bool NV3DOutputDX9::Present(HANDLE sharedHandle, UINT width, UINT height, D3DFORMAT format)
{
    if (!m_iface || !sharedHandle) return false;
    HRESULT hr = m_iface->SetInputSharedHandle(sharedHandle, width, height, (uint32_t)format);
    if (FAILED(hr)) return false;
    return SUCCEEDED(m_iface->Present());
}

void NV3DOutputDX9::SetEyeSwap(bool enable)
{
    if (m_iface) m_iface->SetEyeSwap(enable);
}

void NV3DOutputDX9::NotifyDeviceLost()
{
    if (m_iface) m_iface->NotifyDeviceLost();
}

void NV3DOutputDX9::Shutdown()
{
    if (m_iface)
    {
        m_iface->Delete();
        m_iface = nullptr;
    }
    m_initAttempted = false;
}

} // namespace wiz3d
