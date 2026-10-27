/*
 * Project : wiz3D
 * Copyright (C) wiz3D contributors 2026
 *
 * NV3D-Lib shutter-glasses output helper -- DX9 variant.
 *
 * Mirrors NV3DOutput (DX11). Uses the native NV3D::InterfaceDX9 backend
 * added in NV3D-Lib Phase 2 (no DX11 bridge). Caller owns a wide-SBS
 * texture it created with DX9's cross-device shared handle:
 *
 *     HANDLE h = nullptr;
 *     device9->CreateTexture(2W, H, 1, D3DUSAGE_RENDERTARGET,
 *         D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &tex, &h);
 *
 * Pass h + dims + format to Present(). NV3D-Lib's internal D3D9Ex
 * device opens the same handle on first call and StretchRect-s per-eye
 * halves into its shutter back buffer each PresentEx.
 */
#pragma once

#include <windows.h>
#include <d3d9.h>

namespace NV3D { class InterfaceDX9; }

namespace wiz3d {

class NV3DOutputDX9
{
public:
    NV3DOutputDX9();
    ~NV3DOutputDX9();

    // Create the NV3D::InterfaceDX9 on `device`. `hostHwnd` is passed as
    // host_hwnd so audio routing and the cursor stay on the game's window
    // (nullptr -> library owns its own FSE popup on the primary 3D-Vision
    // display). Idempotent on retry.
    //
    // Returns false if NV3D-Lib couldn't come up (no 3D Vision hardware,
    // nvapi64.dll missing, D3D9Ex create refused, etc.). Caller falls
    // through to its normal on-screen SBS composite.
    bool TryInit(IDirect3DDevice9* device, HWND hostHwnd, bool eyeSwap);

    // Per-frame: hand the KMT handle from the caller's own CreateTexture
    // call to NV3D-Lib (cached internally; a repeat call with the same
    // tuple is cheap) and Present. `sharedHandle` must have been produced
    // by the host device via CreateTexture's pSharedHandle out-param.
    bool Present(HANDLE sharedHandle, UINT width, UINT height, D3DFORMAT format);

    void SetEyeSwap(bool enable);
    void NotifyDeviceLost();
    void Shutdown();

    bool IsReady() const { return m_iface != nullptr; }

private:
    NV3DOutputDX9(const NV3DOutputDX9&) = delete;
    NV3DOutputDX9& operator=(const NV3DOutputDX9&) = delete;

    NV3D::InterfaceDX9* m_iface = nullptr;
    bool m_initAttempted = false;
};

} // namespace wiz3d
