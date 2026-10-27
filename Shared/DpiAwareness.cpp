/*
 * Project : wiz3D
 * Copyright (C) wiz3D contributors 2026
 */
#include "DpiAwareness.h"

namespace wiz3d {

namespace {

// Mirror of DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2's handle value.
// Not available on all SDK versions; defined as a reserved HANDLE value
// (see MSDN / shellscalingapi.h). Avoids pulling the shellscalingapi
// header into every TU.
#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 ((HANDLE)-4)
#endif
#ifndef PROCESS_PER_MONITOR_DPI_AWARE
#define PROCESS_PER_MONITOR_DPI_AWARE 2
#endif

typedef BOOL  (WINAPI *PFN_SetProcessDpiAwarenessContext)(HANDLE);
typedef HRESULT (WINAPI *PFN_SetProcessDpiAwareness)(int);
typedef BOOL  (WINAPI *PFN_SetProcessDPIAware)(void);

} // namespace

void SetProcessDpiAware()
{
    // Try PerMonitorV2 (Win10 1703+) first.
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32)
    {
        auto p = (PFN_SetProcessDpiAwarenessContext)
                 GetProcAddress(user32, "SetProcessDpiAwarenessContext");
        if (p && p(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
            return;
    }
    // Fall back to PerMonitor (Win8.1+).
    HMODULE shcore = LoadLibraryW(L"Shcore.dll");
    if (shcore)
    {
        auto p = (PFN_SetProcessDpiAwareness)
                 GetProcAddress(shcore, "SetProcessDpiAwareness");
        if (p && SUCCEEDED(p(PROCESS_PER_MONITOR_DPI_AWARE)))
            return;
    }
    // Fall back to system-DPI-aware (Vista+). Last resort -- a system-
    // DPI-aware process still virtualises windows that move to a monitor
    // with a different scale, but that's rarer than >100% on primary.
    if (user32)
    {
        auto p = (PFN_SetProcessDPIAware)
                 GetProcAddress(user32, "SetProcessDPIAware");
        if (p) (void)p();
    }
}

bool FixDpiUnawareExternalResize(HWND hwnd)
{
    if (!hwnd || !IsWindow(hwnd)) return false;

    // Only act on fullscreen windows. A windowed host can legitimately
    // be any size; the pathology only triggers when the host is
    // fullscreen and someone (the SR service, typically) resizes it from
    // a DPI-unaware context so the physical window exceeds the panel.
    LONG style = GetWindowLongW(hwnd, GWL_STYLE);
    // Fullscreen heuristic: no caption + no overlapped style, OR window
    // size matches the monitor. We only need to catch the "oversize"
    // case, so just check client-vs-monitor dims and skip windowed.
    RECT clientRc = {};
    if (!GetClientRect(hwnd, &clientRc)) return false;
    const LONG clientW = clientRc.right  - clientRc.left;
    const LONG clientH = clientRc.bottom - clientRc.top;

    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    if (!mon || !GetMonitorInfoW(mon, &mi)) return false;
    const LONG monW = mi.rcMonitor.right  - mi.rcMonitor.left;
    const LONG monH = mi.rcMonitor.bottom - mi.rcMonitor.top;

    if (clientW <= monW && clientH <= monH) return false;    // nothing to fix

    // Window exceeds its monitor. Re-assert the monitor's own bounds
    // from our (per-monitor-DPI-aware) context. SR service's one-shot
    // resize does not re-fight this -- confirmed by aurora3D in the
    // field. SWP_NOACTIVATE keeps focus; SWP_NOZORDER avoids changing
    // the z-order stack. (void)style silences the unused-var warning
    // without complicating the fullscreen heuristic above.
    (void)style;
    SetWindowPos(hwnd, NULL,
                 mi.rcMonitor.left, mi.rcMonitor.top, monW, monH,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    return true;
}

} // namespace wiz3d
