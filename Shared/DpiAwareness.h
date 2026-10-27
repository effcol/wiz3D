/*
 * Project : wiz3D
 * Copyright (C) wiz3D contributors 2026
 *
 * Process-level DPI awareness + the "SR service one-shot resize" fix.
 *
 * Why this matters, per oneup03/3D-Skills (leiasr-integration SKILL.md
 * sections A.2.2 and pitfall #11):
 *
 *   On a monitor at >100% Windows display scale, a non-DPI-aware host
 *   gets its window virtualised: the OS multiplies physical coordinates
 *   by the scale factor on the way out, so the swap-chain back buffer
 *   is DPI-virtualised sub-region of the native panel that then gets
 *   stretched up by the compositor. For any output mode that needs 1:1
 *   per-panel-pixel landing (SR weave, row/column interlaced,
 *   checkerboard), this collapses the stereo pattern into blur/ghost.
 *
 *   Even a correctly PerMonitorV2-aware host still gets its fullscreen
 *   window resized ~1 s after SR init by the SimulatedReality service,
 *   which issues SetWindowPos from a DPI-UNAWARE thread. The OS
 *   multiplies that request by the display scale: a 3840x2160 fullscreen
 *   on a 150% display becomes a 5760x3240 window with only the top-left
 *   panel-sized region visible. Weave misalign, 3D collapses.
 *
 * Both fixes are small, cheap, and SAFE for every output mode (process
 * DPI awareness just makes GetSystemMetrics / client-rect numbers
 * physical instead of virtual; the external-resize fix is a no-op unless
 * the window is actually oversized). Wire into every wrapper that opens
 * a swap chain / window.
 *
 * Reference implementation: fix_dpi_unaware_external_resize() in
 * aurora3D/lib/window.cpp (Stereo3D-rebased branch).
 */
#pragma once

#include <windows.h>

namespace wiz3d {

// Call from DllMain's DLL_PROCESS_ATTACH (or as early as possible --
// before the host creates its window or swap chain). Tries, in order:
//   1. SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)
//      -- Win10 1703+
//   2. SetProcessDpiAwareness(PROCESS_PER_MONITOR_DPI_AWARE) -- Win8.1+
//   3. SetProcessDPIAware() -- Vista+
// Idempotent: process DPI awareness is one-shot, so a second call
// silently returns success. Loads both entry points via LoadLibrary +
// GetProcAddress so the DLL still links on older SDKs.
void SetProcessDpiAware();

// Call from your resize / SR init completion / periodic health probe
// path. If `hwnd` is a fullscreen window whose client area exceeds the
// bounds of the monitor it sits on, re-asserts the correct bounds via
// SetWindowPos. Returns true if a correction was issued (expect a
// follow-up WM_SIZE from the OS — the caller's own resize handler will
// pick up the corrected dims next frame).
//
// Safe to call every frame: the oversize-check is cheap, and the
// SetWindowPos path only runs when correction is actually needed. The
// SR service's resize is a one-shot — once we re-assert the correct
// bounds, it stays corrected.
bool FixDpiUnawareExternalResize(HWND hwnd);

} // namespace wiz3d
