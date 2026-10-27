/*
 * Project : wiz3D
 * Copyright (C) wiz3D contributors 2026
 *
 * NV3D-Lib output helper -- DX11 variant. See NV3DOutput.h for the API
 * contract and the Vision Restoration cooperation story.
 */
#include "NV3DOutput.h"
#include "../lib/NV3D-Lib/NV3D-Lib-fresh/NV3DLib/include/NV3D.hpp"

#include <d3d11.h>

namespace wiz3d {

NV3DOutputDX11::NV3DOutputDX11() = default;

NV3DOutputDX11::~NV3DOutputDX11()
{
    Shutdown();
}

bool NV3DOutputDX11::TryInit(ID3D11Device* device, HWND hostHwnd, bool eyeSwap)
{
    if (m_iface)              return true;
    if (m_initAttempted && !m_iface) return false;    // one-shot: don't retry on failure
    m_initAttempted = true;

    if (!device) return false;

    NV3D::InitParams params = {};
    params.host_hwnd         = hostHwnd;      // host owns the HWND; audio/cursor stay native
    params.eye_swap          = eyeSwap;
    params.enable_lightboost = true;          // let NV3D-Lib pick a LightBoost timing if the EDID matches
    params.enable_suppressor = true;          // mute depth-slider OSD + Ctrl+F hijacks
    params.on_top            = true;
    // Leave the rest at defaults -- VsyncOn, max_frame_latency=1, retry budget 60.

    HRESULT hr = NV3D::CreateInterfaceDX11(device, &params, &m_iface);
    if (FAILED(hr) || !m_iface)
    {
        m_iface = nullptr;
        // NV3D-Lib logs its own init failure reason via its LogSink. The
        // caller sees TryInit=false; the diagnostic is in the NV3D-Lib
        // log stream (OutputDebugStringW by default; wrappers can wire
        // their own sink via NV3D::SetLogSink).
        return false;
    }

    // Diagnostic only: we don't gate on this. NV3D-Lib's DIRECT mode works
    // whether Vision Restoration is installed or not; this field is for
    // the host wrapper to log so the user can tell which 3D Vision path
    // is live on their machine.
    m_visionRestoration = false;    // TODO: optional NvAPI_Stereo_IsEnabled probe at the wrapper layer
    return true;
}

bool NV3DOutputDX11::Present(ID3D11Texture2D* sbsTex)
{
    if (!m_iface || !sbsTex) return false;
    HRESULT hr = m_iface->SetInputTexture(sbsTex);
    if (FAILED(hr)) return false;
    return SUCCEEDED(m_iface->Present());
}

void NV3DOutputDX11::SetEyeSwap(bool enable)
{
    if (m_iface) m_iface->SetEyeSwap(enable);
}

void NV3DOutputDX11::NotifyDeviceLost()
{
    if (m_iface) m_iface->NotifyDeviceLost();
}

void NV3DOutputDX11::Shutdown()
{
    if (m_iface)
    {
        m_iface->Delete();
        m_iface = nullptr;
    }
    m_initAttempted = false;
}

} // namespace wiz3d
