/*
* Project : wiz3D
* Copyright (C) wiz3D contributors 2026
*/

#include "PanelNativeSize.h"
#include <windows.h>

namespace wiz3d
{

// LeiaSR SDK C-API types. Both are opaque void* in the C header, so we
// forward-declare them here and skip pulling in the SDK headers.
typedef void* SRContext_t;
typedef void* SRDisplay_t;

typedef SRContext_t  (*PFN_newSRContext)        (void);
typedef void         (*PFN_initializeSRContext) (SRContext_t);
typedef void         (*PFN_deleteSRContext)     (SRContext_t);
typedef SRDisplay_t  (*PFN_createDisplay)       (SRContext_t);
typedef int          (*PFN_getRecommendedWidth) (SRDisplay_t);
typedef int          (*PFN_getRecommendedHeight)(SRDisplay_t);
typedef int          (*PFN_getPhysicalWidth)    (SRDisplay_t);
typedef int          (*PFN_getPhysicalHeight)   (SRDisplay_t);

static bool                     s_resolved             = false;
static bool                     s_available            = false;
static PFN_newSRContext         s_newSRContext         = nullptr;
static PFN_initializeSRContext  s_initializeSRContext  = nullptr;
static PFN_deleteSRContext      s_deleteSRContext      = nullptr;
static PFN_createDisplay        s_createDisplay        = nullptr;
static PFN_getRecommendedWidth  s_getRecommendedWidth  = nullptr;
static PFN_getRecommendedHeight s_getRecommendedHeight = nullptr;
static PFN_getPhysicalWidth     s_getPhysicalWidth     = nullptr;
static PFN_getPhysicalHeight    s_getPhysicalHeight    = nullptr;

// Resolve the SR runtime function pointers on first call. Idempotent and
// cheap after the first call (two bool checks). Only entered from the
// game's Present thread via SwapChain11Proxy, so thread-safety isn't a
// concern for the sentinel.
static void ResolveOnce()
{
    if (s_resolved) return;
    s_resolved = true;

    HMODULE hCore     = LoadLibraryW(L"SimulatedRealityCore.dll");
    HMODULE hDisplays = LoadLibraryW(L"SimulatedRealityDisplays.dll");
    if (!hCore || !hDisplays) return;

    s_newSRContext         = reinterpret_cast<PFN_newSRContext>        (GetProcAddress(hCore,     "newSRContext"));
    s_initializeSRContext  = reinterpret_cast<PFN_initializeSRContext> (GetProcAddress(hCore,     "initializeSRContext"));
    s_deleteSRContext      = reinterpret_cast<PFN_deleteSRContext>     (GetProcAddress(hCore,     "deleteSRContext"));
    s_createDisplay        = reinterpret_cast<PFN_createDisplay>       (GetProcAddress(hDisplays, "createDisplay"));
    s_getRecommendedWidth  = reinterpret_cast<PFN_getRecommendedWidth> (GetProcAddress(hDisplays, "getRecommendedViewsTextureWidth"));
    s_getRecommendedHeight = reinterpret_cast<PFN_getRecommendedHeight>(GetProcAddress(hDisplays, "getRecommendedViewsTextureHeight"));
    s_getPhysicalWidth     = reinterpret_cast<PFN_getPhysicalWidth>    (GetProcAddress(hDisplays, "getPhysicalResolutionWidth"));
    s_getPhysicalHeight    = reinterpret_cast<PFN_getPhysicalHeight>   (GetProcAddress(hDisplays, "getPhysicalResolutionHeight"));

    // Core context + createDisplay are the hard requirement. The two size
    // getters are independent; either or both may fail GetProcAddress on
    // older SR runtimes. Callers check s_getPhysicalWidth/Height /
    // s_getRecommendedWidth/Height individually before using them.
    s_available = s_newSRContext && s_initializeSRContext && s_deleteSRContext && s_createDisplay;
}

bool QueryRecommendedSRViewsSize(int& outWidthSBS, int& outHeight)
{
    ResolveOnce();
    if (!s_available || !s_getRecommendedWidth || !s_getRecommendedHeight) return false;

    SRContext_t ctx = s_newSRContext();
    if (!ctx) return false;
    s_initializeSRContext(ctx);

    SRDisplay_t display = s_createDisplay(ctx);
    if (!display)
    {
        s_deleteSRContext(ctx);
        return false;
    }

    const int w = s_getRecommendedWidth(display);
    const int h = s_getRecommendedHeight(display);
    s_deleteSRContext(ctx);

    if (w <= 0 || h <= 0) return false;
    outWidthSBS = w;
    outHeight   = h;
    return true;
}

// Fallback for panel-native sizing when the LeiaSR Displays SDK isn't installed:
// the SR panel is the physical display the game window lives on, and its OS
// monitor rect is the pixel grid the lenticular pattern aligns to. Not quite
// as precise as the Displays API (which can report calibration-dependent dims),
// but close enough for the common case of a non-rotated SR display.
bool QueryPanelNativeSize(void* hwnd, int& outPanelW, int& outPanelH)
{
    // Try the LeiaSR Displays API first. getPhysicalResolutionWidth/Height
    // return the panel's actual pixel grid -- what the weaver OUTPUT must
    // match so the lenticular pattern aligns 1:1. Do NOT use
    // getRecommendedViewsTextureWidth/Height here: those return the SBS
    // INPUT views-texture size (often much smaller than the panel on
    // calibration-dependent SKUs; e.g. a 3840x2160 panel can report
    // 1920x1080 recommended views, which if used as panel size produces a
    // 960x1080 half-SBS target and the game ends up rendering a tiny
    // sub-native frame upscaled by DWM).
    ResolveOnce();
    if (s_available && s_getPhysicalWidth && s_getPhysicalHeight)
    {
        SRContext_t ctx = s_newSRContext();
        if (ctx)
        {
            s_initializeSRContext(ctx);
            SRDisplay_t display = s_createDisplay(ctx);
            if (display)
            {
                const int w = s_getPhysicalWidth(display);
                const int h = s_getPhysicalHeight(display);
                s_deleteSRContext(ctx);
                if (w > 0 && h > 0)
                {
                    outPanelW = w;
                    outPanelH = h;
                    return true;
                }
            }
            else
            {
                s_deleteSRContext(ctx);
            }
        }
    }

    // Fall back to the OS monitor. GetMonitorInfo returns the virtual-screen
    // rect, which for a non-cloned, non-rotated display matches the panel's
    // pixel grid. On an SR panel that's exactly what we want to weave into.
    HWND window = reinterpret_cast<HWND>(hwnd);
    if (!window || !::IsWindow(window)) return false;
    HMONITOR hMon = ::MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
    if (!hMon) return false;
    MONITORINFO mi = {};
    mi.cbSize = sizeof(MONITORINFO);
    if (!::GetMonitorInfoW(hMon, &mi)) return false;
    const int mw = mi.rcMonitor.right  - mi.rcMonitor.left;
    const int mh = mi.rcMonitor.bottom - mi.rcMonitor.top;
    if (mw <= 0 || mh <= 0) return false;
    outPanelW = mw;
    outPanelH = mh;
    return true;
}

}  // namespace wiz3d
