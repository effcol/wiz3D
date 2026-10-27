/*
 * Project : wiz3D
 * Copyright (C) wiz3D contributors 2026
 *
 * NV3D-Lib output helper for DX11-side wrappers (DX11 Stereo-Injector,
 * HD3D AmdQbProxy, 3DV Direct DX11). Wraps NV3D::InterfaceDX11 behind a
 * compact RAII shell matching the pattern of KatangaProducer / SRWeave.
 *
 * What NV3D-Lib does (lib/NV3D-Lib/NV3D-Lib-fresh/NV3DLib/include/NV3D.hpp):
 *   Creates a hidden D3D9Ex device in NVAPI_STEREO_DRIVER_MODE_DIRECT and
 *   per-eye SetActiveEye + StretchRects each half of a 2W x H SBS source
 *   into its back buffer, then PresentEx -- shutter glasses light up via
 *   the driver's DIRECT mode, bypassing the legacy AUTOMATIC-mode
 *   signature-row scan entirely.
 *
 * Vision Restoration cooperation:
 *   Oscar's Vision Restoration restores NVIDIA 3D Vision support on
 *   modern drivers; a system with VR installed can also drive shutter
 *   glasses natively via the wrapper's existing per-eye stereo bridge
 *   (DX11 reverse-blit, DX9 NvAPI_Stereo_SetActiveEye) under the driver's
 *   AUTOMATIC mode. NV3D-Lib's DIRECT mode and the driver's AUTOMATIC
 *   mode are mutually exclusive within a process, so the two paths do
 *   not stack: the user picks via OutputMode.
 *     * OutputMode = HalfSBS / FullSBS + VR installed  -> driver AUTOMATIC path, glasses lit
 *     * OutputMode = 3DVisionGlasses (any driver state) -> NV3D-Lib DIRECT path, glasses lit
 *   The helper probes NvAPI_Stereo_IsEnabled on init purely for diagnostic
 *   logging; it does not gate initialization on the result (NV3D-Lib
 *   itself handles the "no 3D Vision hardware" case by returning failure
 *   from CreateInterfaceDX11, which this helper reports via TryInit).
 *
 * Fallback: if TryInit fails, the wrapper's regular SBS composite still
 * runs, so the user sees SBS on-screen even with no glasses driver.
 */
#pragma once

#include <windows.h>

struct ID3D11Device;
struct ID3D11Texture2D;

namespace NV3D { class InterfaceDX11; }

namespace wiz3d {

class NV3DOutputDX11
{
public:
    NV3DOutputDX11();
    ~NV3DOutputDX11();

    // Create the NV3D::InterfaceDX11 on `device`, binding to `hostHwnd`
    // (passed as host_hwnd so audio routing / cursor stay native). Logs
    // Vision Restoration probe outcome. Idempotent: a successful init
    // short-circuits on retry.
    //
    // Returns false if NV3D-Lib couldn't come up (no 3D Vision hardware,
    // nvapi64.dll missing, D3D9Ex refused the window, etc.). Caller should
    // fall through to its normal on-screen SBS composite.
    bool TryInit(ID3D11Device* device, HWND hostHwnd, bool eyeSwap);

    // Per-frame: hand the wrapper's wide-SBS source texture to NV3D-Lib
    // and let it present via DIRECT-mode StretchRects. The source must be
    // 2W x H with left eye in [0..W), right eye in [W..2W). BGRA fast
    // path when MISC_SHARED is set; non-MISC_SHARED is handled by an
    // internal mirror. Returns false on error.
    bool Present(ID3D11Texture2D* sbsTex);

    // Live swap-eyes toggle. Updates NV3D-Lib's signature row for the
    // next frame; no teardown needed.
    void SetEyeSwap(bool enable);

    // Host-side notification: the DX11 device backing this helper lost
    // its adapter (TDR / DXGI_ERROR_DEVICE_REMOVED). NV3D-Lib switches to
    // the non-blocking teardown path -- call this BEFORE Shutdown
    // whenever the host observes device removal.
    void NotifyDeviceLost();

    // Full teardown. Safe to call multiple times.
    void Shutdown();

    bool IsReady() const { return m_iface != nullptr; }
    bool VisionRestorationDetected() const { return m_visionRestoration; }

private:
    NV3DOutputDX11(const NV3DOutputDX11&) = delete;
    NV3DOutputDX11& operator=(const NV3DOutputDX11&) = delete;

    NV3D::InterfaceDX11* m_iface = nullptr;
    bool m_initAttempted      = false;
    bool m_visionRestoration  = false;    // diagnostic only
};

} // namespace wiz3d
