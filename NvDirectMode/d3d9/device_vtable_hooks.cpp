/* NvDirectMode/d3d9 - IDirect3DDevice9 function-body hooks via MinHook
 *
 * Migrated from vtable-slot swapping (task #68) to MinHook function-body
 * inline hooks. Rationale: Steam Overlay (gameoverlayrenderer.dll) also
 * patches IDirect3DDevice9's Present/GetBackBuffer vtable slots. With BOTH
 * of us doing vtable-slot swap, the call chain that each side saved as
 * "original" ends up pointing back into the other's hook, and a repeat
 * invocation from either side recurses forever → crash (observed in Oil
 * Rush / Hard Reset on Steam). See git log around this file for the
 * observed callstack.
 *
 * MinHook is invisible to vtable inspection — it patches the target
 * function's first bytes with a jmp to our hook, and gives us a
 * trampoline (a copy of the original prologue + a jmp back to
 * target+prologue) to call the original. Steam Overlay's vtable-slot
 * swap continues to work: its saved "original" points to the real
 * Present, which now has our jmp at its prologue, so our hook fires
 * cleanly from within Steam's hook, and we forward via the trampoline.
 * No shared vtable slot to fight over → no recursion.
 *
 * The old "cached PFN_* to avoid recursion" reasoning still applies, but
 * now the PFN_* pointers are trampolines instead of raw function pointers.
 * Semantically identical for callers.
 */

#include "device_vtable_hooks.h"
#include "swapchain_helpers.h"
#include "eye_state.h"
#include "log.h"
#include "SR.hpp"          // Simulated Reality — same path as Device9Proxy.cpp
#include <MinHook.h>

extern "C" void NvDM_Log(const char* fmt, ...);
extern "C" int  NvDM_VerboseEnabled();
extern "C" int  NvDM_OutputIsTopBottom();
extern "C" int  NvDM_SwapEyes();
extern "C" int  NvDM_OutputMode();
extern "C" int  NvDM_SRSRGB();

namespace NvDirectMode
{

namespace
{
    // ------------------------------------------------------------------
    // IDirect3DDevice9 vtable indices (stable across Windows versions
    // since the IDL never changes for shipped d3d9.h interfaces).
    // ------------------------------------------------------------------
    constexpr int kSlotReset         = 16;
    constexpr int kSlotPresent       = 17;
    constexpr int kSlotGetBackBuffer = 18;
    // IDirect3DDevice9Ex extends IDirect3DDevice9 (119 slots: 0-118),
    // then appends its own. PresentEx = 121, ResetEx = 132.
    constexpr int kSlotPresentEx     = 121;
    constexpr int kSlotResetEx       = 132;
    // IDirect3DSwapChain9 slots. Oil Rush et al. bypass the device's
    // Present/GetBackBuffer entirely by holding a swap chain pointer and
    // calling its methods directly. We patch the SC vtable too and route
    // through the same shadow/composite logic; filter on primarySC so
    // additional swap chains (menu overlays, etc.) pass through untouched.
    constexpr int kSlotSC_Present       = 3;
    constexpr int kSlotSC_GetBackBuffer = 5;
    // Diagnostic hooks for the Hard-Reset investigation (task #178).
    // If the game switches render targets mid-frame, we need to see
    // which surfaces it binds (would explain "left eye rendered
    // off-screen, right eye rendered into BB"). Clear-count per frame
    // is the other tell (a game running scene twice usually clears
    // twice). Slots come from d3d9.h: SetRenderTarget=37,
    // GetRenderTarget=38 (unhooked here — read-only), Clear=43.
    constexpr int kSlotSetRenderTarget  = 37;
    constexpr int kSlotClear            = 43;

    typedef HRESULT (STDMETHODCALLTYPE *PFN_Reset)           (IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
    typedef HRESULT (STDMETHODCALLTYPE *PFN_Present)         (IDirect3DDevice9*, CONST RECT*, CONST RECT*, HWND, CONST RGNDATA*);
    typedef HRESULT (STDMETHODCALLTYPE *PFN_GetBackBuffer)   (IDirect3DDevice9*, UINT, UINT, D3DBACKBUFFER_TYPE, IDirect3DSurface9**);
    typedef HRESULT (STDMETHODCALLTYPE *PFN_PresentEx)       (IDirect3DDevice9Ex*, CONST RECT*, CONST RECT*, HWND, CONST RGNDATA*, DWORD);
    typedef HRESULT (STDMETHODCALLTYPE *PFN_ResetEx)         (IDirect3DDevice9Ex*, D3DPRESENT_PARAMETERS*, D3DDISPLAYMODEEX*);
    typedef HRESULT (STDMETHODCALLTYPE *PFN_SC_Present)      (IDirect3DSwapChain9*, CONST RECT*, CONST RECT*, HWND, CONST RGNDATA*, DWORD);
    typedef HRESULT (STDMETHODCALLTYPE *PFN_SC_GetBackBuffer)(IDirect3DSwapChain9*, UINT, D3DBACKBUFFER_TYPE, IDirect3DSurface9**);
    typedef HRESULT (STDMETHODCALLTYPE *PFN_SetRenderTarget) (IDirect3DDevice9*, DWORD, IDirect3DSurface9*);
    typedef HRESULT (STDMETHODCALLTYPE *PFN_Clear)           (IDirect3DDevice9*, DWORD, const D3DRECT*, DWORD, D3DCOLOR, float, DWORD);

    static PFN_Reset            g_origReset            = nullptr;
    static PFN_Present          g_origPresent          = nullptr;
    static PFN_GetBackBuffer    g_origGetBackBuffer    = nullptr;
    static PFN_PresentEx        g_origPresentEx        = nullptr;
    static PFN_ResetEx          g_origResetEx          = nullptr;
    static PFN_SC_Present       g_origSC_Present       = nullptr;
    static PFN_SC_GetBackBuffer g_origSC_GetBackBuffer = nullptr;
    static PFN_SetRenderTarget  g_origSetRenderTarget  = nullptr;
    static PFN_Clear            g_origClear            = nullptr;

    static bool g_patchedReset            = false;
    static bool g_patchedPresent          = false;
    static bool g_patchedGetBackBuffer    = false;
    static bool g_patchedPresentEx        = false;
    static bool g_patchedResetEx          = false;
    static bool g_patchedSC_Present       = false;
    static bool g_patchedSC_GetBackBuffer = false;
    static bool g_patchedSetRenderTarget  = false;
    static bool g_patchedClear            = false;

    // Per-frame counters for the RT/Clear diagnostic. Reset at each
    // Composite entry (i.e. at Present time), reflect what the game did
    // in the last frame. Also collect a small ring of unique
    // render-target pointers bound within a frame — that's the "did we
    // see an off-screen RT bind?" signal for Hard Reset's stereo probe.
    static volatile LONG g_frameSetRTCount = 0;
    static volatile LONG g_frameClearCount = 0;
    static IDirect3DSurface9* g_frameRTs[8] = {};
    static volatile LONG g_frameRTCount = 0;

    // ------------------------------------------------------------------
    // Primary device shadow state. One game = one device, same as the
    // Device9Proxy::g_primaryDevice slot — keeps the mapping trivial
    // (no map lookup per Present) and matches what the eye-change
    // callback dispatcher already assumes.
    // ------------------------------------------------------------------
    struct ShadowState
    {
        IDirect3DDevice9*    realDevice    = nullptr;     // not refed (game owns it)
        IDirect3DDevice9Ex*  realDeviceEx  = nullptr;     // == realDevice when isEx
        bool                 isEx          = false;
        UINT                 logicalW      = 0;
        UINT                 logicalH      = 0;
        IDirect3DSurface9*   trackedBB     = nullptr;     // refed
        IDirect3DSurface9*   shadowBB      = nullptr;     // refed
        IDirect3DSurface9*   leftEye       = nullptr;     // refed
        IDirect3DSurface9*   rightEye      = nullptr;     // refed
        // Identity token for the game's primary swap chain — filter for
        // the SC hooks. Held as a ref for pointer stability; released on
        // Reset / teardown.
        IDirect3DSwapChain9* primarySC     = nullptr;     // refed
        // SR (Simulated Reality — Leia / Samsung Odyssey lightfield weave)
        // pipeline for OutputMode 8. Mirrors Device9Proxy::m_sr* fields.
        // Interface init is lazy on first Composite; SBS texture allocs
        // on demand when logical dims are known. srFailed sticks once
        // SR runtime init has irrecoverably failed so we don't retry
        // every frame — natural fallthrough to the SBS composite path.
        bool                              srFailed        = false;
        SimulatedReality::SRInterfaceDX9* srInterfaceDX9  = nullptr;
        IDirect3DTexture9*                srSBSTex        = nullptr;
        IDirect3DSurface9*                srSBSSurf       = nullptr; // surface level 0 of srSBSTex
        UINT                              srSBSW          = 0;       // == logicalW * 2
        UINT                              srSBSH          = 0;       // == logicalH
        D3DFORMAT                         srSBSFmt        = D3DFMT_UNKNOWN;
    };

    static ShadowState      g_state;
    static CRITICAL_SECTION g_stateLock;
    static volatile LONG    g_stateLockInit = 0;

    void EnsureStateLock()
    {
        if (InterlockedCompareExchange(&g_stateLockInit, 1, 0) == 0)
            InitializeCriticalSection(&g_stateLock);
    }

    void ReleaseShadowSurfaces(ShadowState& s)
    {
        if (s.shadowBB) { s.shadowBB->Release(); s.shadowBB = nullptr; }
        if (s.leftEye)  { s.leftEye->Release();  s.leftEye  = nullptr; }
        if (s.rightEye) { s.rightEye->Release(); s.rightEye = nullptr; }
    }

    // SR pipeline teardown — mirror of Device9Proxy::ReleaseSRPipeline.
    // Called on Reset and on device teardown; SR resources are tied to
    // the same lifetime as trackedBB / shadowBB.
    void ReleaseSRPipeline(ShadowState& s)
    {
        if (s.srSBSSurf) { s.srSBSSurf->Release(); s.srSBSSurf = nullptr; }
        if (s.srSBSTex)  { s.srSBSTex->Release();  s.srSBSTex  = nullptr; }
        s.srSBSW = 0; s.srSBSH = 0; s.srSBSFmt = D3DFMT_UNKNOWN;
        if (s.srInterfaceDX9)
        {
            s.srInterfaceDX9->Delete();
            s.srInterfaceDX9 = nullptr;
        }
        // NB: srFailed is a permanent one-shot; a Reset does NOT re-arm
        // it. If SR runtime is absent at first init we won't keep
        // retrying every device Reset.
    }

    void ReleaseTrackedBB(ShadowState& s)
    {
        if (s.trackedBB) { s.trackedBB->Release(); s.trackedBB = nullptr; }
        if (s.primarySC) { s.primarySC->Release(); s.primarySC = nullptr; }
        ReleaseSRPipeline(s);
    }

    void EnsureShadow(ShadowState& s)
    {
        if (s.shadowBB || !s.realDevice || !s.trackedBB) return;
        D3DSURFACE_DESC desc = {};
        if (FAILED(s.trackedBB->GetDesc(&desc))) return;
        s.logicalW = desc.Width;
        s.logicalH = desc.Height;
        // Game-side render target at logical size, same format/MS so
        // StretchRect can copy without conversion.
        HRESULT hr = s.realDevice->CreateRenderTarget(desc.Width, desc.Height, desc.Format,
                                                       desc.MultiSampleType, desc.MultiSampleQuality,
                                                       FALSE, &s.shadowBB, NULL);
        if (FAILED(hr) || !s.shadowBB)
        {
            NvDM_Log("  d3d9 [stable-dev] EnsureShadow: CreateRenderTarget(%ux%u) FAILED hr=0x%08lX\n",
                     desc.Width, desc.Height, hr);
            s.shadowBB = nullptr;
            return;
        }
        NvDM_Log("  d3d9 [stable-dev] EnsureShadow: shadow=%p (%ux%u, fmt=%d) for realBB=%p\n",
                 s.shadowBB, desc.Width, desc.Height, (int)desc.Format, (void*)s.trackedBB);
    }

    void StashTrackedBB(ShadowState& s)
    {
        ReleaseTrackedBB(s);
        ReleaseShadowSurfaces(s);
        if (!s.realDevice) return;
        // GetBackBuffer(0,0,MONO) — go via cached orig to avoid recursion
        // into our own Hook_GetBackBuffer (which would hand back the
        // shadow we haven't created yet).
        if (g_origGetBackBuffer)
            g_origGetBackBuffer(s.realDevice, 0, 0, D3DBACKBUFFER_TYPE_MONO, &s.trackedBB);
        // Capture the primary swap chain identity for the SC hook filter.
        s.realDevice->GetSwapChain(0, &s.primarySC);
        EnsureShadow(s);
        // NB: eye surfaces stay lazy-alloc — pre-allocating would leave
        // the "not-yet-captured" eye showing driver-default black in the
        // weave, which looks worse than the mono-in-stereo fallback in
        // Composite/RunSRWeave (mirrors the captured eye across both
        // halves until the other eye is also captured).
    }

    // ------------------------------------------------------------------
    // SR pipeline for OutputMode 8 (Simulated Reality weave).
    // Direct port of Device9Proxy::{EnsureSRWeaver, EnsureSRSBSTexture,
    // RunSRWeave} onto ShadowState. Semantics unchanged from that path;
    // this just lets the layout-stable / MinHook code route get to SR
    // instead of falling back to SBS when the game uses
    // UseLayoutStableProxy=2 (both Hard Reset and Oil Rush do).
    // ------------------------------------------------------------------
    bool EnsureSRWeaver(ShadowState& s)
    {
        if (s.srFailed)         return false;
        if (s.srInterfaceDX9)   return true;
        if (!s.realDevice)      return false;

        // Output-window HWND: prefer the device's focus window; fall back
        // to the primary swap chain's present params.
        D3DDEVICE_CREATION_PARAMETERS cp = {};
        HWND hWnd = nullptr;
        if (SUCCEEDED(s.realDevice->GetCreationParameters(&cp)))
            hWnd = cp.hFocusWindow;
        if (!hWnd && s.primarySC)
        {
            D3DPRESENT_PARAMETERS pp = {};
            s.primarySC->GetPresentParameters(&pp);
            hWnd = pp.hDeviceWindow;
        }

        HRESULT hr = SimulatedReality::CreateSRInterfaceDX9(s.realDevice, hWnd, &s.srInterfaceDX9);
        if (FAILED(hr) || !s.srInterfaceDX9)
        {
            NvDM_Log("  d3d9 [stable-dev] EnsureSRWeaver: CreateSRInterfaceDX9 failed hr=0x%08lX hWnd=%p dev=%p\n",
                     hr, (void*)hWnd, (void*)s.realDevice);
            s.srFailed = true;
            return false;
        }
        NvDM_Log("  d3d9 [stable-dev] EnsureSRWeaver: ready (hWnd=%p srInterface=%p)\n",
                 (void*)hWnd, (void*)s.srInterfaceDX9);
        return true;
    }

    bool EnsureSRSBSTexture(ShadowState& s)
    {
        if (!s.realDevice) return false;
        if (s.logicalW == 0 || s.logicalH == 0) return false;

        D3DFORMAT wantFmt = D3DFMT_A8R8G8B8;
        if (s.trackedBB)
        {
            D3DSURFACE_DESC desc = {};
            if (SUCCEEDED(s.trackedBB->GetDesc(&desc)))
                wantFmt = desc.Format;
        }

        UINT wantW = s.logicalW * 2;
        UINT wantH = s.logicalH;
        if (s.srSBSTex && s.srSBSW == wantW && s.srSBSH == wantH && s.srSBSFmt == wantFmt)
            return true;

        if (s.srSBSSurf) { s.srSBSSurf->Release(); s.srSBSSurf = nullptr; }
        if (s.srSBSTex)  { s.srSBSTex->Release();  s.srSBSTex  = nullptr; }

        HRESULT hr = s.realDevice->CreateTexture(wantW, wantH, 1, D3DUSAGE_RENDERTARGET,
                                                  wantFmt, D3DPOOL_DEFAULT,
                                                  &s.srSBSTex, nullptr);
        if (FAILED(hr) || !s.srSBSTex)
        {
            NvDM_Log("  d3d9 [stable-dev] EnsureSRSBSTexture: CreateTexture(%ux%u fmt=%d) FAILED hr=0x%08lX\n",
                     wantW, wantH, (int)wantFmt, hr);
            s.srSBSTex = nullptr;
            return false;
        }
        if (FAILED(s.srSBSTex->GetSurfaceLevel(0, &s.srSBSSurf)) || !s.srSBSSurf)
        {
            s.srSBSTex->Release(); s.srSBSTex = nullptr;
            return false;
        }

        s.srSBSW = wantW; s.srSBSH = wantH; s.srSBSFmt = wantFmt;

        // Bind SBS texture to the SR interface once per (re)allocation.
        // The 2nd arg tells SR-Lib whether to treat the input as sRGB.
        // Wrong choice = gamma-shifted / washed colours through the
        // weave. Default 0 (linear) matches Device9Proxy; flip via
        // SRSRGB config to test.
        bool srgbFlag = (NvDM_SRSRGB() != 0);
        if (s.srInterfaceDX9)
            s.srInterfaceDX9->SetInputTexture(s.srSBSTex, srgbFlag);

        NvDM_Log("  d3d9 [stable-dev] EnsureSRSBSTexture: sbs=%p (%ux%u fmt=%d) srgb=%d\n",
                 (void*)s.srSBSTex, wantW, wantH, (int)wantFmt, (int)srgbFlag);
        return true;
    }

    // Per-eye capture counters. Their definitions were originally near
    // CaptureEye (further down) but the staleness helpers below need to
    // read them, so they've been hoisted above. Reset never zeroes
    // them — session-cumulative is what makes the staleness heuristic
    // work (we compare successive snapshots, not absolute values).
    static volatile LONG g_leftCaptureCount  = 0;
    static volatile LONG g_rightCaptureCount = 0;

    // Staleness threshold: this many consecutive composites without an
    // eye's capture counter advancing → treat that eye as stale, mirror
    // the other across both halves. Empirically Hard Reset shows LEFT
    // going stale for thousands of frames at a time (game does
    // SetActiveEye(RIGHT) per frame but not LEFT — its "left frozen,
    // right moving" symptom). 5 is enough to filter out normal
    // per-eye alternation jitter while catching multi-frame freezes.
    static constexpr LONG kStaleFrameThreshold = 5;
    static LONG s_lastSeenLeftCap    = 0;
    static LONG s_lastSeenRightCap   = 0;
    static LONG s_leftStaleFrames    = 0;
    static LONG s_rightStaleFrames   = 0;

    bool LeftIsStale()  { return s_leftStaleFrames  > kStaleFrameThreshold; }
    bool RightIsStale() { return s_rightStaleFrames > kStaleFrameThreshold; }

    // Called once per Composite to update staleness state.
    void RefreshEyeStaleness()
    {
        LONG curLeft  = g_leftCaptureCount;
        LONG curRight = g_rightCaptureCount;
        if (curLeft  > s_lastSeenLeftCap)  { s_leftStaleFrames  = 0; s_lastSeenLeftCap  = curLeft;  }
        else                                { s_leftStaleFrames  += 1;                              }
        if (curRight > s_lastSeenRightCap) { s_rightStaleFrames = 0; s_lastSeenRightCap = curRight; }
        else                                { s_rightStaleFrames += 1;                              }
    }

    bool RunSRWeave(ShadowState& s)
    {
        if (!EnsureSRWeaver(s))     return false;
        if (!EnsureSRSBSTexture(s)) return false;
        if (!s.trackedBB)           return false;

        // Tolerate one eye still being NULL (asymmetric per-eye call
        // pattern from the game — e.g. after a stereo-toggle Reset, the
        // game may go many frames issuing SetActiveEye(RIGHT) only
        // before the first LEFT swings back). Mirror the captured eye
        // across both halves so the display keeps updating instead of
        // silently dropping SR and going to single-eye SBS.
        if (!s.leftEye && !s.rightEye) return false;

        bool swap = (NvDM_SwapEyes() != 0);
        IDirect3DSurface9* leftSrc  = swap ? s.rightEye : s.leftEye;
        IDirect3DSurface9* rightSrc = swap ? s.leftEye  : s.rightEye;
        if (!leftSrc)  leftSrc  = rightSrc;
        if (!rightSrc) rightSrc = leftSrc;
        // NB: previous versions here also mono-mirrored the fresh eye
        // across both halves when the OTHER was "stale" per the
        // staleness counters. Removed — reverted after Hard Reset
        // crashed with it enabled and the underlying capture pattern
        // turned out to actually be much more balanced than the
        // earlier degenerate run implied. The staleness counters
        // themselves are still tracked and printed in the Composite
        // heartbeat so we retain visibility into any real asymmetry.

        // Step A: blit each eye into the corresponding half of the SBS texture.
        RECT leftHalf  = { 0,                       0,
                           (LONG)s.logicalW,        (LONG)s.logicalH };
        RECT rightHalf = { (LONG)s.logicalW,        0,
                           (LONG)(s.logicalW * 2),  (LONG)s.logicalH };
        HRESULT hr;
        hr = s.realDevice->StretchRect(leftSrc,  nullptr, s.srSBSSurf, &leftHalf,  D3DTEXF_LINEAR);
        if (FAILED(hr))
        {
            NVDM_TRACE_FIRST_N(5, "  d3d9 [stable-dev] RunSRWeave: left StretchRect FAILED hr=0x%08lX\n", hr);
            return false;
        }
        hr = s.realDevice->StretchRect(rightSrc, nullptr, s.srSBSSurf, &rightHalf, D3DTEXF_LINEAR);
        if (FAILED(hr))
        {
            NVDM_TRACE_FIRST_N(5, "  d3d9 [stable-dev] RunSRWeave: right StretchRect FAILED hr=0x%08lX\n", hr);
            return false;
        }

        // Step B: bind the real BB as RT and call Weave().
        IDirect3DSurface9* prevRT = nullptr;
        s.realDevice->GetRenderTarget(0, &prevRT);
        s.realDevice->SetRenderTarget(0, s.trackedBB);

        D3DVIEWPORT9 vp = { 0, 0, s.logicalW, s.logicalH, 0.0f, 1.0f };
        s.realDevice->SetViewport(&vp);

        bool sceneBegun = SUCCEEDED(s.realDevice->BeginScene());
        NVDM_TRACE_FIRST_N(5, "  d3d9 [stable-dev] RunSRWeave: weave call srInterface=%p SBS=%ux%u\n",
                           (void*)s.srInterfaceDX9, s.srSBSW, s.srSBSH);
        s.srInterfaceDX9->Weave();
        if (sceneBegun) s.realDevice->EndScene();

        if (prevRT) { s.realDevice->SetRenderTarget(0, prevRT); prevRT->Release(); }

        static volatile LONG s_weaveCount = 0;
        LONG n = _InterlockedIncrement(&s_weaveCount);
        if (n == 60 || n == 180 || n == 600 || n == 1800 || (n > 0 && (n % 3600) == 0))
            NvDM_Log("  d3d9 [stable-dev] RunSRWeave: heartbeat #%ld (SR still alive)\n", (long)n);

        return true;
    }

    // (g_leftCaptureCount and g_rightCaptureCount defined earlier in
    // this namespace — hoisted so the staleness helpers can read them.)
    void CaptureEye(ShadowState& s, int eyeBeingLeft)
    {
        if (!s.shadowBB || !s.realDevice) return;
        IDirect3DSurface9** slot = nullptr;
        const char* eyeTag = nullptr;
        volatile LONG* counter = nullptr;
        if      (eyeBeingLeft == NvDirectMode::kEyeLeft)  { slot = &s.leftEye;  eyeTag = "LEFT";  counter = &g_leftCaptureCount;  }
        else if (eyeBeingLeft == NvDirectMode::kEyeRight) { slot = &s.rightEye; eyeTag = "RIGHT"; counter = &g_rightCaptureCount; }
        else return;

        if (!*slot)
        {
            D3DSURFACE_DESC desc = {};
            s.shadowBB->GetDesc(&desc);
            HRESULT hr = s.realDevice->CreateRenderTarget(desc.Width, desc.Height, desc.Format,
                                                           desc.MultiSampleType, desc.MultiSampleQuality,
                                                           FALSE, slot, NULL);
            if (FAILED(hr) || !*slot) return;
            NvDM_Log("  d3d9 [stable-dev] CaptureEye(%s): allocated eye surface=%p\n",
                     eyeTag, *slot);
        }
        HRESULT sr = s.realDevice->StretchRect(s.shadowBB, NULL, *slot, NULL, D3DTEXF_NONE);
        // Per-eye counter increment + log the first-N calls PER EYE so the
        // shared budget can't hide the asymmetry any more.
        LONG n = _InterlockedIncrement(counter);
        if (n <= 8 || (n % 300) == 0)
            NvDM_Log("  d3d9 [stable-dev] CaptureEye(%s) #%ld: StretchRect shadow=%p -> eye=%p hr=0x%08lX\n",
                     eyeTag, (long)n, s.shadowBB, *slot, sr);
    }

    void CompositeIntoRealBB(ShadowState& s)
    {
        if (!s.realDevice || !s.trackedBB)
        {
            NVDM_TRACE_FIRST_N(8, "  d3d9 [stable-dev] Composite: SKIP realDevice=%p trackedBB=%p\n",
                               s.realDevice, s.trackedBB);
            return;
        }
        int currentEye = NvDirectMode::GetActiveEye();
        // Update per-eye staleness before the SR branch so RunSRWeave
        // can consult LeftIsStale() / RightIsStale() and mirror the
        // fresh eye across both halves if the game has stopped
        // capturing one side (Hard Reset's SetActiveEye(RIGHT)-only
        // period pattern).
        RefreshEyeStaleness();
        NVDM_TRACE_FIRST_N(16, "  d3d9 [stable-dev] Composite: enter currentEye=%d leftEye=%p rightEye=%p shadow=%p tracked=%p leftStale=%ld rightStale=%ld\n",
                           currentEye, s.leftEye, s.rightEye, s.shadowBB, s.trackedBB,
                           (long)s_leftStaleFrames, (long)s_rightStaleFrames);
        // Frame-count heartbeat every 60 frames (≈1s at 60fps) with full
        // state, PLUS a running total of how many CaptureEye StretchRects
        // each eye has received this session, PLUS per-Present deltas
        // for the NvApi Activate/Deactivate/SetActiveEye counters.
        // Delta view is the whole point — if Hard Reset's per-frame
        // pattern is really A→SA(L)→SA(R)→D, we should see roughly
        // dA=60, dD=60, dSA_L=60, dSA_R=60 per 60-frame tick. Deviation
        // from that shape tells us what the game is actually doing.
        {
            static volatile LONG s_frames = 0;
            LONG n = _InterlockedIncrement(&s_frames);
            if ((n % 60) == 0)
            {
                typedef long (__cdecl *PFN_LGetter)();
                static HMODULE hNv = nullptr;
                static PFN_LGetter fnAct = nullptr, fnDeact = nullptr,
                                    fnSAL = nullptr, fnSAR = nullptr, fnSAM = nullptr;
                if (!hNv)
                {
                    hNv = GetModuleHandleW(L"nvapi.dll");
                    if (!hNv) hNv = GetModuleHandleW(L"nvapi64.dll");
                    if (hNv)
                    {
                        fnAct   = (PFN_LGetter)GetProcAddress(hNv, "Wiz3D_GetActivateCount");
                        fnDeact = (PFN_LGetter)GetProcAddress(hNv, "Wiz3D_GetDeactivateCount");
                        fnSAL   = (PFN_LGetter)GetProcAddress(hNv, "Wiz3D_GetSALeftCount");
                        fnSAR   = (PFN_LGetter)GetProcAddress(hNv, "Wiz3D_GetSARightCount");
                        fnSAM   = (PFN_LGetter)GetProcAddress(hNv, "Wiz3D_GetSAMonoCount");
                    }
                }
                long a  = fnAct   ? fnAct()   : -1;
                long d  = fnDeact ? fnDeact() : -1;
                long sl = fnSAL   ? fnSAL()   : -1;
                long sr = fnSAR   ? fnSAR()   : -1;
                long sm = fnSAM   ? fnSAM()   : -1;

                static long s_prevA = 0, s_prevD = 0, s_prevSL = 0, s_prevSR = 0, s_prevSM = 0;
                long dA  = (a  >= 0) ? a  - s_prevA  : 0;
                long dD  = (d  >= 0) ? d  - s_prevD  : 0;
                long dSL = (sl >= 0) ? sl - s_prevSL : 0;
                long dSR = (sr >= 0) ? sr - s_prevSR : 0;
                long dSM = (sm >= 0) ? sm - s_prevSM : 0;

                // Snapshot the current-frame SetRT / Clear tallies too.
                // These reflect ACTIVITY IN THE FRAME WE'RE ABOUT TO
                // PRESENT — the whole point of this diagnostic. The
                // Composite is called at Present, so these numbers are
                // "how many RT-binds / Clears in the frame just drawn".
                // g_frameRTs[] holds distinct RT surface pointers bound
                // during the frame — non-shadow ones are the smoking
                // gun for off-screen per-eye rendering.
                LONG rtc    = g_frameSetRTCount;
                LONG clc    = g_frameClearCount;
                LONG rtd    = g_frameRTCount;
                void* rt0   = (rtd >= 1) ? g_frameRTs[0] : nullptr;
                void* rt1   = (rtd >= 2) ? g_frameRTs[1] : nullptr;
                void* rt2   = (rtd >= 3) ? g_frameRTs[2] : nullptr;
                void* rt3   = (rtd >= 4) ? g_frameRTs[3] : nullptr;

                NvDM_Log("  d3d9 [stable-dev] Composite: heartbeat frame #%ld currentEye=%d leftCap=%ld rightCap=%ld  nvapi totals: A=%ld D=%ld SA(L)=%ld SA(R)=%ld SA(M)=%ld  last60fr: dA=%ld dD=%ld dL=%ld dR=%ld dM=%ld  SetRT=%ld Clear=%ld distinctRTs=%ld [%p,%p,%p,%p]\n",
                         (long)n, currentEye,
                         (long)g_leftCaptureCount, (long)g_rightCaptureCount,
                         a, d, sl, sr, sm,
                         dA, dD, dSL, dSR, dSM,
                         (long)rtc, (long)clc, (long)rtd, rt0, rt1, rt2, rt3);
                s_prevA = a; s_prevD = d; s_prevSL = sl; s_prevSR = sr; s_prevSM = sm;
            }
            // Reset per-frame counters AFTER the heartbeat prints, so
            // next frame accumulates cleanly. Non-heartbeat frames still
            // reset so the counters reflect one frame's worth.
            g_frameSetRTCount = 0;
            g_frameClearCount = 0;
            g_frameRTCount    = 0;
            for (int i = 0; i < 8; ++i) g_frameRTs[i] = nullptr;
        }
        // OutputMode 8 = SR weave. Placed AFTER the heartbeat block on
        // purpose — the previous ordering short-circuited before the
        // heartbeat printed, so once SR started firing we lost all
        // diagnostic visibility into per-Present state.
        if (NvDM_OutputMode() == 8 && s.leftEye && s.rightEye)
        {
            if (currentEye == NvDirectMode::kEyeLeft || currentEye == NvDirectMode::kEyeRight)
                CaptureEye(s, currentEye);
            if (RunSRWeave(s)) return;
            // If we got here, SR failed — natural fallthrough to the
            // SBS path below.
        }
        if (currentEye == NvDirectMode::kEyeLeft || currentEye == NvDirectMode::kEyeRight)
            CaptureEye(s, currentEye);

        bool topBottom = NvDM_OutputIsTopBottom() != 0;
        bool swap      = NvDM_SwapEyes() != 0;
        IDirect3DSurface9* leftSrc  = swap ? s.rightEye : s.leftEye;
        IDirect3DSurface9* rightSrc = swap ? s.leftEye  : s.rightEye;

        D3DSURFACE_DESC bbDesc = {};
        s.trackedBB->GetDesc(&bbDesc);

        if (leftSrc && rightSrc)
        {
            RECT leftRect, rightRect;
            if (topBottom)
            {
                leftRect  = { 0, 0, (LONG)bbDesc.Width, (LONG)(bbDesc.Height / 2) };
                rightRect = { 0, (LONG)(bbDesc.Height / 2), (LONG)bbDesc.Width, (LONG)bbDesc.Height };
            }
            else
            {
                leftRect  = { 0, 0, (LONG)(bbDesc.Width / 2), (LONG)bbDesc.Height };
                rightRect = { (LONG)(bbDesc.Width / 2), 0, (LONG)bbDesc.Width, (LONG)bbDesc.Height };
            }
            s.realDevice->StretchRect(leftSrc,  NULL, s.trackedBB, &leftRect,  D3DTEXF_LINEAR);
            s.realDevice->StretchRect(rightSrc, NULL, s.trackedBB, &rightRect, D3DTEXF_LINEAR);
        }
        else
        {
            // Single eye / mono fallback.
            IDirect3DSurface9* src = leftSrc ? leftSrc : (rightSrc ? rightSrc : s.shadowBB);
            if (src)
                s.realDevice->StretchRect(src, NULL, s.trackedBB, NULL, D3DTEXF_NONE);
        }
    }

    // ------------------------------------------------------------------
    // Eye-change callback. Mirrors Device9Proxy::OnEyeChange — captures
    // the OLD eye's frame from the shadow into a per-eye surface.
    // ------------------------------------------------------------------
    void OnEyeChange(int oldEye, int /*newEye*/)
    {
        EnsureStateLock();
        EnterCriticalSection(&g_stateLock);
        if (g_state.realDevice) CaptureEye(g_state, oldEye);
        LeaveCriticalSection(&g_stateLock);
    }

    // ------------------------------------------------------------------
    // Hooks. pThis is the real device (game's pointer). Look up shadow
    // state, do our work, forward via cached orig fn ptr.
    // ------------------------------------------------------------------
    HRESULT STDMETHODCALLTYPE Hook_Reset(IDirect3DDevice9* pThis, D3DPRESENT_PARAMETERS* pPP)
    {
        if (!g_origReset) return D3DERR_INVALIDCALL;
        // Dump the presentation parameters — a stereo mode toggle that
        // triggers a Reset almost always changes refresh rate, fullscreen
        // state, or backbuffer dimensions. We want to see EXACTLY what
        // the game asked for so we can correlate against the freeze.
        if (pPP)
        {
            NvDM_Log("  d3d9 [stable-dev] Hook_Reset: pThis=%p (realDevice=%p match=%d) "
                     "BB=%ux%u fmt=%d fs=%d refresh=%uHz swapEffect=%d hDeviceWindow=%p\n",
                     pThis, g_state.realDevice, (int)(pThis == g_state.realDevice),
                     pPP->BackBufferWidth, pPP->BackBufferHeight, (int)pPP->BackBufferFormat,
                     (int)(!pPP->Windowed), pPP->FullScreen_RefreshRateInHz,
                     (int)pPP->SwapEffect, (void*)pPP->hDeviceWindow);
        }
        else
        {
            NvDM_Log("  d3d9 [stable-dev] Hook_Reset: pThis=%p (realDevice=%p match=%d) pPP=NULL\n",
                     pThis, g_state.realDevice, (int)(pThis == g_state.realDevice));
        }

        EnsureStateLock();
        EnterCriticalSection(&g_stateLock);

        // Tracked BB & shadow are invalidated by Reset — release before
        // forwarding so refs don't outlive the swapchain rebuild.
        if (pThis == g_state.realDevice)
        {
            ReleaseTrackedBB(g_state);
            ReleaseShadowSurfaces(g_state);
        }

        D3DPRESENT_PARAMETERS modified;
        UINT logicalW = 0, logicalH = 0;
        if (pPP)
        {
            modified = *pPP;
            ResolveAndDoubleSwapchainParams(&modified, modified.hDeviceWindow, &logicalW, &logicalH);
        }

        LeaveCriticalSection(&g_stateLock);
        HRESULT hr = g_origReset(pThis, pPP ? &modified : nullptr);
        EnterCriticalSection(&g_stateLock);

        if (SUCCEEDED(hr) && pThis == g_state.realDevice)
        {
            if (logicalW > 0) { g_state.logicalW = logicalW; g_state.logicalH = logicalH; }
            StashTrackedBB(g_state);
            NvDM_Log("  d3d9 [stable-dev] Hook_Reset: SUCCESS logical=%ux%u tracked=%p shadow=%p\n",
                     g_state.logicalW, g_state.logicalH, g_state.trackedBB, g_state.shadowBB);
        }
        else if (FAILED(hr))
        {
            NvDM_Log("  d3d9 [stable-dev] Hook_Reset: orig Reset FAILED hr=0x%08lX (D3DERR_INVALIDCALL"
                     " = something we haven't released holds a D3DPOOL_DEFAULT resource)\n", hr);
        }
        LeaveCriticalSection(&g_stateLock);
        return hr;
    }

    HRESULT STDMETHODCALLTYPE Hook_Present(IDirect3DDevice9* pThis,
                                            CONST RECT* sr, CONST RECT* dr,
                                            HWND h, CONST RGNDATA* d)
    {
        if (!g_origPresent) return D3DERR_INVALIDCALL;
        NVDM_TRACE_FIRST_N(8, "  d3d9 [stable-dev] Hook_Present: pThis=%p (realDevice=%p match=%d)\n",
                           pThis, g_state.realDevice, (int)(pThis == g_state.realDevice));
        EnsureStateLock();
        EnterCriticalSection(&g_stateLock);
        if (pThis == g_state.realDevice) CompositeIntoRealBB(g_state);
        LeaveCriticalSection(&g_stateLock);
        return g_origPresent(pThis, sr, dr, h, d);
    }

    HRESULT STDMETHODCALLTYPE Hook_GetBackBuffer(IDirect3DDevice9* pThis,
                                                  UINT iSC, UINT iBB,
                                                  D3DBACKBUFFER_TYPE T,
                                                  IDirect3DSurface9** ppBB)
    {
        if (!g_origGetBackBuffer) return D3DERR_INVALIDCALL;
        // Hand the shadow surface for the primary BB; everything else
        // forwards untouched.
        if (iSC == 0 && iBB == 0 && T == D3DBACKBUFFER_TYPE_MONO && ppBB)
        {
            EnsureStateLock();
            EnterCriticalSection(&g_stateLock);
            if (pThis == g_state.realDevice)
            {
                EnsureShadow(g_state);
                if (g_state.shadowBB)
                {
                    g_state.shadowBB->AddRef();
                    *ppBB = g_state.shadowBB;
                    LeaveCriticalSection(&g_stateLock);
                    return S_OK;
                }
            }
            LeaveCriticalSection(&g_stateLock);
        }
        return g_origGetBackBuffer(pThis, iSC, iBB, T, ppBB);
    }

    HRESULT STDMETHODCALLTYPE Hook_PresentEx(IDirect3DDevice9Ex* pThis,
                                              CONST RECT* sr, CONST RECT* dr,
                                              HWND h, CONST RGNDATA* d, DWORD F)
    {
        if (!g_origPresentEx) return D3DERR_INVALIDCALL;
        EnsureStateLock();
        EnterCriticalSection(&g_stateLock);
        if (static_cast<IDirect3DDevice9*>(pThis) == g_state.realDevice)
            CompositeIntoRealBB(g_state);
        LeaveCriticalSection(&g_stateLock);
        return g_origPresentEx(pThis, sr, dr, h, d, F);
    }

    // IDirect3DSwapChain9 hooks (Oil Rush et al. bypass the device path).
    HRESULT STDMETHODCALLTYPE Hook_SC_Present(IDirect3DSwapChain9* pThis,
                                                CONST RECT* sr, CONST RECT* dr,
                                                HWND h, CONST RGNDATA* d, DWORD F)
    {
        if (!g_origSC_Present) return D3DERR_INVALIDCALL;
        NVDM_TRACE_FIRST_N(8, "  d3d9 [stable-dev] Hook_SC_Present: pThis=%p (primarySC=%p match=%d)\n",
                           pThis, g_state.primarySC, (int)(pThis == g_state.primarySC));
        EnsureStateLock();
        EnterCriticalSection(&g_stateLock);
        if (pThis == g_state.primarySC) CompositeIntoRealBB(g_state);
        LeaveCriticalSection(&g_stateLock);
        return g_origSC_Present(pThis, sr, dr, h, d, F);
    }

    HRESULT STDMETHODCALLTYPE Hook_SC_GetBackBuffer(IDirect3DSwapChain9* pThis,
                                                     UINT iBB,
                                                     D3DBACKBUFFER_TYPE T,
                                                     IDirect3DSurface9** ppBB)
    {
        if (!g_origSC_GetBackBuffer) return D3DERR_INVALIDCALL;
        if (iBB == 0 && T == D3DBACKBUFFER_TYPE_MONO && ppBB)
        {
            EnsureStateLock();
            EnterCriticalSection(&g_stateLock);
            if (pThis == g_state.primarySC)
            {
                EnsureShadow(g_state);
                if (g_state.shadowBB)
                {
                    g_state.shadowBB->AddRef();
                    *ppBB = g_state.shadowBB;
                    LeaveCriticalSection(&g_stateLock);
                    return S_OK;
                }
            }
            LeaveCriticalSection(&g_stateLock);
        }
        return g_origSC_GetBackBuffer(pThis, iBB, T, ppBB);
    }

    // ------------------------------------------------------------------
    // Diagnostic hooks (task #178 Hard Reset investigation).
    // Counts SetRenderTarget + Clear calls per frame and remembers up
    // to 8 distinct RT surface pointers bound within the frame. Zeroed
    // at each Composite entry (i.e. at Present) so the numbers reported
    // in the heartbeat reflect one frame's activity.
    // ------------------------------------------------------------------
    HRESULT STDMETHODCALLTYPE Hook_SetRenderTarget(IDirect3DDevice9* pThis, DWORD idx, IDirect3DSurface9* pRT)
    {
        if (!g_origSetRenderTarget) return D3DERR_INVALIDCALL;
        LONG n = _InterlockedIncrement(&g_frameSetRTCount);
        if (pRT && idx == 0)
        {
            // Track distinct surface pointers this frame (slot 0 only —
            // that's the RT the game's actual scene renders into).
            LONG existing = g_frameRTCount;
            bool seen = false;
            for (LONG i = 0; i < existing && i < 8; ++i)
                if (g_frameRTs[i] == pRT) { seen = true; break; }
            if (!seen && existing < 8)
            {
                LONG at = _InterlockedIncrement(&g_frameRTCount) - 1;
                if (at < 8) g_frameRTs[at] = pRT;
            }
            NVDM_TRACE_FIRST_N(24, "  d3d9 [stable-dev] Hook_SetRenderTarget #%ld: idx=0 pRT=%p (shadow=%p tracked=%p) match=%d\n",
                               (long)n, pRT, g_state.shadowBB, g_state.trackedBB,
                               (int)(pRT == g_state.shadowBB || pRT == g_state.trackedBB));
        }
        return g_origSetRenderTarget(pThis, idx, pRT);
    }

    HRESULT STDMETHODCALLTYPE Hook_Clear(IDirect3DDevice9* pThis, DWORD Count, const D3DRECT* pRects,
                                          DWORD Flags, D3DCOLOR Color, float Z, DWORD Stencil)
    {
        if (!g_origClear) return D3DERR_INVALIDCALL;
        LONG n = _InterlockedIncrement(&g_frameClearCount);
        NVDM_TRACE_FIRST_N(24, "  d3d9 [stable-dev] Hook_Clear #%ld: flags=0x%lX color=0x%08lX Z=%.2f\n",
                           (long)n, Flags, Color, Z);
        return g_origClear(pThis, Count, pRects, Flags, Color, Z, Stencil);
    }

    HRESULT STDMETHODCALLTYPE Hook_ResetEx(IDirect3DDevice9Ex* pThis,
                                            D3DPRESENT_PARAMETERS* pPP,
                                            D3DDISPLAYMODEEX* pFM)
    {
        if (!g_origResetEx) return D3DERR_INVALIDCALL;
        NvDM_Log("  d3d9 [stable-dev] Hook_ResetEx: pThis=%p (realDevice=%p match=%d)\n",
                 pThis, g_state.realDevice,
                 (int)(static_cast<IDirect3DDevice9*>(pThis) == g_state.realDevice));

        EnsureStateLock();
        EnterCriticalSection(&g_stateLock);
        if (static_cast<IDirect3DDevice9*>(pThis) == g_state.realDevice)
        {
            ReleaseTrackedBB(g_state);
            ReleaseShadowSurfaces(g_state);
        }

        D3DPRESENT_PARAMETERS modified;
        UINT logicalW = 0, logicalH = 0;
        if (pPP)
        {
            modified = *pPP;
            ResolveAndDoubleSwapchainParams(&modified, modified.hDeviceWindow, &logicalW, &logicalH);
        }

        LeaveCriticalSection(&g_stateLock);
        HRESULT hr = g_origResetEx(pThis, pPP ? &modified : nullptr, pFM);
        EnterCriticalSection(&g_stateLock);

        if (SUCCEEDED(hr) && static_cast<IDirect3DDevice9*>(pThis) == g_state.realDevice)
        {
            if (logicalW > 0) { g_state.logicalW = logicalW; g_state.logicalH = logicalH; }
            StashTrackedBB(g_state);
        }
        else if (FAILED(hr))
        {
            NvDM_Log("  d3d9 [stable-dev] Hook_ResetEx: orig ResetEx failed hr=0x%08lX\n", hr);
        }
        LeaveCriticalSection(&g_stateLock);
        return hr;
    }

    // Idempotent MinHook init — MH_Initialize is fine to call once per
    // process; subsequent calls return MH_ERROR_ALREADY_INITIALIZED which
    // we treat as success.
    bool EnsureMinHookInit()
    {
        static volatile LONG s_init = 0;
        if (InterlockedCompareExchange(&s_init, 1, 0) == 0)
        {
            MH_STATUS s = MH_Initialize();
            if (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED)
            {
                NvDM_Log("  d3d9 [minhook] MH_Initialize FAILED status=%d\n", (int)s);
                return false;
            }
            NvDM_Log("  d3d9 [minhook] MH_Initialize OK\n");
        }
        return true;
    }

    // Same signature as the old vtable-slot swap function. Now:
    //   1. Reads the target function pointer from the vtable (invisible read,
    //      does NOT modify the vtable).
    //   2. Registers an inline hook with MinHook — patches the target
    //      function's prologue with a jmp to our hook. Vtable stays intact.
    //   3. Receives back a trampoline pointer that runs the original
    //      prologue + jumps into target+prologueSize. We store this as the
    //      "original" so callers keep working unchanged.
    //
    // Coexistence with Steam Overlay (& other vtable-slot hookers): they
    // do vtable[slot] = their_hook; their_original = old vtable[slot].
    // Their saved original still points at the real function (which now
    // has our inline jmp at its start). When their hook forwards, our
    // hook fires, we forward via the trampoline, and control returns to
    // them. Their saved original never gets rewritten to point at us —
    // vtable and function body are separate memory. Recursion impossible.
    bool PatchSlot(void** vtable, int slot, void* hook, void** outOriginal, const char* tag)
    {
        if (!EnsureMinHookInit()) return false;

        void* target = vtable[slot];
        if (!target)
        {
            NvDM_Log("  d3d9 [minhook] Hook(%s, slot=%d): vtable slot is NULL\n", tag, slot);
            return false;
        }

        MH_STATUS cs = MH_CreateHook(target, hook, outOriginal);
        // ALREADY_CREATED is fine — the hook is process-global and covers
        // any device sharing this vtable, so re-registering is a no-op
        // for us. Just make sure we have the trampoline.
        if (cs != MH_OK && cs != MH_ERROR_ALREADY_CREATED)
        {
            NvDM_Log("  d3d9 [minhook] MH_CreateHook(%s, slot=%d, target=%p) FAILED status=%d\n",
                     tag, slot, target, (int)cs);
            return false;
        }

        MH_STATUS es = MH_EnableHook(target);
        if (es != MH_OK && es != MH_ERROR_ENABLED)
        {
            NvDM_Log("  d3d9 [minhook] MH_EnableHook(%s, target=%p) FAILED status=%d\n",
                     tag, target, (int)es);
            return false;
        }

        NvDM_Log("  d3d9 [minhook] Hooked %s (slot=%d, target=%p, trampoline=%p)\n",
                 tag, slot, target, *outOriginal);
        return true;
    }
} // anonymous namespace

bool InstallDeviceVtablePatch(IDirect3DDevice9* realDevice, bool isEx,
                                UINT logicalW, UINT logicalH)
{
    if (!realDevice) return false;

    EnsureStateLock();

    // Patch vtable slots once per process. The vtable is shared across
    // every IDirect3DDevice9 in this process (system-runtime singleton),
    // so a single patch covers any future devices the game creates.
    void** vtable = *reinterpret_cast<void***>(realDevice);

    if (!g_patchedReset)
    {
        if (!PatchSlot(vtable, kSlotReset,
                       reinterpret_cast<void*>(&Hook_Reset),
                       reinterpret_cast<void**>(&g_origReset),
                       "Reset"))
            return false;
        g_patchedReset = true;
    }
    if (!g_patchedPresent)
    {
        if (!PatchSlot(vtable, kSlotPresent,
                       reinterpret_cast<void*>(&Hook_Present),
                       reinterpret_cast<void**>(&g_origPresent),
                       "Present"))
            return false;
        g_patchedPresent = true;
    }
    if (!g_patchedGetBackBuffer)
    {
        if (!PatchSlot(vtable, kSlotGetBackBuffer,
                       reinterpret_cast<void*>(&Hook_GetBackBuffer),
                       reinterpret_cast<void**>(&g_origGetBackBuffer),
                       "GetBackBuffer"))
            return false;
        g_patchedGetBackBuffer = true;
    }
    // Diagnostic hooks for task #178. Failure to install is non-fatal —
    // we still get correct rendering, just no RT/Clear visibility.
    if (!g_patchedSetRenderTarget)
    {
        if (PatchSlot(vtable, kSlotSetRenderTarget,
                       reinterpret_cast<void*>(&Hook_SetRenderTarget),
                       reinterpret_cast<void**>(&g_origSetRenderTarget),
                       "SetRenderTarget"))
            g_patchedSetRenderTarget = true;
    }
    if (!g_patchedClear)
    {
        if (PatchSlot(vtable, kSlotClear,
                       reinterpret_cast<void*>(&Hook_Clear),
                       reinterpret_cast<void**>(&g_origClear),
                       "Clear"))
            g_patchedClear = true;
    }

    if (isEx)
    {
        // The Ex slots live on the same vtable when the device's true type
        // is IDirect3DDevice9Ex (single inheritance + extension).
        if (!g_patchedPresentEx)
        {
            if (PatchSlot(vtable, kSlotPresentEx,
                          reinterpret_cast<void*>(&Hook_PresentEx),
                          reinterpret_cast<void**>(&g_origPresentEx),
                          "PresentEx"))
                g_patchedPresentEx = true;
        }
        if (!g_patchedResetEx)
        {
            if (PatchSlot(vtable, kSlotResetEx,
                          reinterpret_cast<void*>(&Hook_ResetEx),
                          reinterpret_cast<void**>(&g_origResetEx),
                          "ResetEx"))
                g_patchedResetEx = true;
        }
    }

    // Register state for this device. Eye-change callback is registered
    // once; the dispatcher only fires for our primary device.
    EnterCriticalSection(&g_stateLock);
    if (g_state.realDevice && g_state.realDevice != realDevice)
    {
        // Replacing primary — release old refs (game presumably destroyed
        // the old device and is making a new one).
        ReleaseTrackedBB(g_state);
        ReleaseShadowSurfaces(g_state);
    }
    g_state.realDevice   = realDevice;
    g_state.realDeviceEx = isEx ? static_cast<IDirect3DDevice9Ex*>(realDevice) : nullptr;
    g_state.isEx         = isEx;
    g_state.logicalW     = logicalW;
    g_state.logicalH     = logicalH;
    StashTrackedBB(g_state);
    LeaveCriticalSection(&g_stateLock);

    // Patch the swap chain vtable once. Shared across the runtime so a
    // single patch covers additional swap chains too; the hook filters
    // on primarySC identity.
    if ((!g_patchedSC_Present || !g_patchedSC_GetBackBuffer) && g_state.primarySC)
    {
        void** scVtable = *reinterpret_cast<void***>(g_state.primarySC);
        if (!g_patchedSC_Present)
        {
            if (PatchSlot(scVtable, kSlotSC_Present,
                          reinterpret_cast<void*>(&Hook_SC_Present),
                          reinterpret_cast<void**>(&g_origSC_Present),
                          "SC_Present"))
                g_patchedSC_Present = true;
        }
        if (!g_patchedSC_GetBackBuffer)
        {
            if (PatchSlot(scVtable, kSlotSC_GetBackBuffer,
                          reinterpret_cast<void*>(&Hook_SC_GetBackBuffer),
                          reinterpret_cast<void**>(&g_origSC_GetBackBuffer),
                          "SC_GetBackBuffer"))
                g_patchedSC_GetBackBuffer = true;
        }
    }

    static volatile LONG s_callbackRegistered = 0;
    if (InterlockedCompareExchange(&s_callbackRegistered, 1, 0) == 0)
        NvDirectMode::RegisterEyeChangeHandler(&OnEyeChange);

    return true;
}

void UninstallDeviceVtablePatches()
{
    // MH_Uninitialize disables every hook (restores each target
    // function's original prologue bytes) and frees the trampoline
    // memory. Safe to call even if MH_Initialize was never called —
    // it returns MH_ERROR_NOT_INITIALIZED which we ignore.
    MH_STATUS s = MH_Uninitialize();
    if (s == MH_OK)
        NvDM_Log("  d3d9 [minhook] MH_Uninitialize OK — all hooks disabled\n");
    else if (s != MH_ERROR_NOT_INITIALIZED)
        NvDM_Log("  d3d9 [minhook] MH_Uninitialize returned status=%d\n", (int)s);
}

} // namespace NvDirectMode
