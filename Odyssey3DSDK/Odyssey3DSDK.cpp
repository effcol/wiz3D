/*
 * wiz3D - Odyssey3DSDK.dll stand-in
 * Simulated Reality native-stereo unlock for Samsung Odyssey 3D titles.
 *
 * Copyright (C) 2026 wiz3D contributors.
 * Licensed under the GNU Lesser General Public License, version 2.1.
 * See LICENSE.txt in the repository root for the full text.
 *
 * Clean-room implementation of Samsung's Odyssey3DSDK.dll interface.
 * The 3-interface / 32-slot ABI shape was reverse-engineered from public
 * CP2077 2.5 call-sites by vl3c (vl3c/cyberpunk2077-3d-braindance-edition,
 * MIT 2026) and reproduced here as a specification only -- no code was
 * taken. See THIRD_PARTY_NOTICES.txt for the attribution block.
 *
 * What this DLL does:
 *   Supplies a stand-in Samsung Odyssey 3D SDK that reports
 *   "SR display present" to any game that probes for one, so games with
 *   a Samsung-gated native stereo mode (Cyberpunk 2077, and the growing
 *   list of Samsung-exclusive native-stereo titles) will enable their
 *   internal half-SBS debug path and render stereo on any display.
 *
 * How games find us:
 *   Game startup reads  HKCU\SOFTWARE\Samsung\Odyssey3DHub\InstallPath,
 *   appends            \GameLauncherData\core\Odyssey3DSDK.dll,
 *   LoadLibrary's that  path, GetProcAddress's our three exports.
 *   The companion dxgi.dll bootstrap proxy writes that registry value
 *   so the user never has to run an installer -- drag-and-drop only.
 *
 * Threading:
 *   Game polls Device methods (GetEyePosition, GetResolution, Hint48)
 *   from multiple threads concurrently -- ~110 Hz, ~55 Hz, 400-900 Hz
 *   respectively per vl3c's instrumentation. All vtable slots are
 *   therefore reentrant-safe; mutable state is guarded by
 *   CRITICAL_SECTIONs.
 *
 * Logging:
 *   All vtable calls are logged on their first invocation and periodically
 *   thereafter.  The envvar WIZ3D_SR_LOG_SLOTS=1 enables a per-call dump
 *   of every unmapped vtable slot, useful for discovering the vtable
 *   shape of a new Samsung-SDK-consuming title.  Output lands in
 *   wiz3D_Logs\wiz3D_SR.log next to the game exe.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>

// LeiaSR DX12 passthrough mode. Only compiled on x64 since the LeiaSR
// SDK is x64-only. 32-bit builds stay no-op/SBSDebug-fallback forever.
#if defined(WIZ3D_SR_HAVE_LEIASR)
#include <d3d12.h>
#include <sr/management/srcontext.h>
#include <sr/weaver/WeaverTypes.h>
#include <sr/weaver/dx12weaver.h>
#include <sr/sense/system/systemsense.h>
#endif

// ----------------------------------------------------------------------------
// Build-time constants
// ----------------------------------------------------------------------------
#define WIZ3D_SR_VERSION_STRING  "wiz3D Simulated Reality 0.5.0"
#define WIZ3D_SR_LOG_FILENAME    L"wiz3D_SR.log"
#define WIZ3D_SR_SHARED_LOG      L"wiz3D_proxy.log"

// Each vtable the game sees is 32 pointer slots wide. Most are unmapped;
// unmapped slots route to a stub that logs and returns the first argument
// (safe for the common C-vtable idiom where arg0 is the self-pointer OR
// an out-param pointer the caller passes in).
static const unsigned kVtableSlots = 32;

// ----------------------------------------------------------------------------
// Logging
// ----------------------------------------------------------------------------
static FILE*             g_logFile = nullptr;
static CRITICAL_SECTION  g_logLock;
static bool              g_logInit = false;
static bool              g_logSlotsEveryCall = false;

static void SrLogInit()
{
    if (g_logInit) return;
    InitializeCriticalSection(&g_logLock);
    g_logInit = true;

    WCHAR dir[MAX_PATH];
    GetModuleFileNameW(NULL, dir, MAX_PATH);
    WCHAR* slash = wcsrchr(dir, L'\\');
    if (slash) *(slash + 1) = L'\0';
    lstrcatW(dir, L"wiz3D_Logs");
    CreateDirectoryW(dir, NULL);
    lstrcatW(dir, L"\\" WIZ3D_SR_LOG_FILENAME);
    g_logFile = _wfopen(dir, L"a");

    // Honor WIZ3D_SR_LOG_SLOTS=1 for exhaustive per-call vtable dumps.
    char v[8] = {};
    DWORD n = GetEnvironmentVariableA("WIZ3D_SR_LOG_SLOTS", v, sizeof(v));
    g_logSlotsEveryCall = (n > 0 && v[0] == '1');
}

static void SrLog(const char* fmt, ...)
{
    SrLogInit();
    if (!g_logFile) return;
    EnterCriticalSection(&g_logLock);
    SYSTEMTIME st;
    GetLocalTime(&st);
    // Thread ID in the stamp -- diagnosis of the intermittent load-screen
    // crash needs to see which thread is calling what, so we tag every
    // line.  Cheap (TlsGetValue-shaped syscall); worth it unconditionally.
    fprintf(g_logFile, "[%02u:%02u:%02u.%03u T%lu] ",
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
            (unsigned long)GetCurrentThreadId());
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_logFile, fmt, ap);
    va_end(ap);
    fflush(g_logFile);
    LeaveCriticalSection(&g_logLock);
}

// ----------------------------------------------------------------------------
// Config -- read from wiz3D_Config.xml alongside the DLL, if present. For
// phase 1 we read only the stereo basics (IPD, nominal eye-Z, forced
// monitor rect override). The DX12 compositor + output-mode knobs land in
// phase 2 when the output stage ports.
// ----------------------------------------------------------------------------
struct SrConfig
{
    float ipd_mm         = 64.0f;    // interpupillary distance; eye_x = ±ipd/2
    float eye_z_mm       = 600.0f;   // nominal view distance (vl3c default)
    bool  log_slots      = false;    // mirror of WIZ3D_SR_LOG_SLOTS envvar
    bool  override_rect  = false;    // if true, use rect_* instead of DXGI probe
    int64_t rect_top     = 0;
    int64_t rect_left    = 0;
    int64_t rect_right   = 1920;
    int64_t rect_bottom  = 1080;
};
static SrConfig g_cfg;

// ----------------------------------------------------------------------------
// LeiaSR passthrough mode
//
// When active, our Weaver forwards all four game-side vtable calls to a
// real SR::IDX12Weaver1 so the user gets native lenticular output on any
// LeiaSR-compatible display (Samsung Odyssey, Acer SpatialLabs, Lume Pad,
// Lenovo ThinkVision 3D, Red Magic). When inactive, Weaver no-ops and the
// user is expected to be running SBSDebug + an external weaver like
// SR-Loom.
//
// Mode selection:
//   WIZ3D_SR_MODE=leiasr   -- force passthrough; fails loudly if runtime
//                             missing
//   WIZ3D_SR_MODE=sbsdebug -- force no-op; never try passthrough
//   WIZ3D_SR_MODE=auto     -- default; probe LeiaSR runtime at CreateWeaver
//                             time, use passthrough if available, else fall
//                             back silently to no-op
//
// Lazy init at Factory::CreateWeaver time -- that's where the game supplies
// us the ID3D12Device + HWND we need. Called under g_sr_lock.
// ----------------------------------------------------------------------------
enum class SrMode { Auto, Leiasr, Sbsdebug };
static SrMode g_srMode = SrMode::Auto;

#if defined(WIZ3D_SR_HAVE_LEIASR)
static CRITICAL_SECTION  g_srLock;
static bool              g_srLockInit    = false;
static SR::SRContext*    g_srContext     = nullptr;
static SR::IDX12Weaver1* g_srWeaver      = nullptr;
static bool              g_srInitTried   = false;
static bool              g_srActive      = false;

// RT binding state for the output framebuffer the game hands us.
// LeiaSR's weave() reads currently-bound RT via the command list we set,
// so we create a tiny CPU-visible RTV heap on the game's device and
// bind the game's output resource as RT right before calling weave().
static ID3D12Device*            g_srDevice         = nullptr;    // game's device, not AddRef'd (lifetime longer than us)
static ID3D12DescriptorHeap*    g_srRtvHeap        = nullptr;
static ID3D12Resource*          g_srBoundOutputFB  = nullptr;    // last-seen output FB (not AddRef'd, we just track identity)
static DXGI_FORMAT              g_srBoundOutputFmt = DXGI_FORMAT_UNKNOWN;
static ID3D12GraphicsCommandList* g_srBoundCmdList = nullptr;    // last-set cmd list, for RT rebind at Weave time
static ID3D12Resource*          g_srBoundInputTex  = nullptr;    // last-seen input view texture (identity only)
static HWND                     g_srHwnd           = nullptr;    // game's hwnd, cached for Weaver re-create

static void SrInitEnv()
{
    if (g_srLockInit) return;
    InitializeCriticalSection(&g_srLock);
    g_srLockInit = true;

    char v[16] = {};
    DWORD n = GetEnvironmentVariableA("WIZ3D_SR_MODE", v, sizeof(v));
    if (n > 0) {
        if      (!_stricmp(v, "leiasr"))   g_srMode = SrMode::Leiasr;
        else if (!_stricmp(v, "sbsdebug")) g_srMode = SrMode::Sbsdebug;
    }
}

// Lazy init on first Factory::CreateWeaver. Returns true if passthrough
// is now active; false if we're falling back to no-op.
static bool SrLazyInit(ID3D12Device* device, HWND hwnd)
{
    EnterCriticalSection(&g_srLock);
    if (g_srInitTried) { bool v = g_srActive; LeaveCriticalSection(&g_srLock); return v; }
    g_srInitTried = true;

    if (g_srMode == SrMode::Sbsdebug) {
        SrLog("SR mode=sbsdebug -- not initializing LeiaSR passthrough\n");
        LeaveCriticalSection(&g_srLock);
        return false;
    }

    // Delay-loaded SR DLLs may fail to resolve. Wrap SRContext::create in SEH
    // so a missing runtime doesn't take down the game.
    __try {
        g_srContext = SR::SRContext::create();
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        g_srContext = nullptr;
    }
    if (!g_srContext) {
        SrLog("SR: SRContext::create() failed (runtime missing?); falling back to no-op\n");
        LeaveCriticalSection(&g_srLock);
        return false;
    }

    // Register SystemSense BEFORE CreateDX12Weaver.  The LeiaSR DX11 and
    // DX12 example main.cpp both register SystemSense first, THEN call
    // CreateXxxWeaver, THEN initialize().  SR-Lib's SR.cpp uses a
    // different order (create weaver first), but SR-Lib clients already
    // have their SystemSense registered upstream; we don't.  Doing it
    // before weaver creation lets the weaver see the sense when it
    // interrogates the context.
    __try {
        SR::SystemSense::create(*g_srContext);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        SrLog("SR: SystemSense::create threw (continuing without system events)\n");
    }

    WeaverErrorCode err = SR::CreateDX12Weaver(g_srContext, device, hwnd, &g_srWeaver);
    if (err != WeaverSuccess || !g_srWeaver) {
        SrLog("SR: CreateDX12Weaver err=%d; falling back to no-op\n", (int)err);
        if (g_srContext) { delete g_srContext; g_srContext = nullptr; }
        LeaveCriticalSection(&g_srLock);
        return false;
    }

    g_srWeaver->setLatencyInFrames(1);
    g_srWeaver->enableLateLatching(true);
    g_srContext->initialize();
    SrLog("SR: SystemSense registered, Weaver created, context initialized, late-latching enabled\n");

    // Tiny CPU-visible RTV heap, 1 slot, for binding the game's output FB.
    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = 1;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_srRtvHeap)))) {
        SrLog("SR: CreateDescriptorHeap(rtv) failed; falling back to no-op\n");
        if (g_srWeaver)  { g_srWeaver->destroy(); g_srWeaver  = nullptr; }
        if (g_srContext) { delete g_srContext;    g_srContext = nullptr; }
        LeaveCriticalSection(&g_srLock);
        return false;
    }

    g_srDevice = device;    // raw pointer; CP2077's device lives longer than our DLL
    g_srHwnd   = hwnd;      // cached so SrRecreateWeaver can rebuild against the same window
    g_srActive = true;
    SrLog("================================================================\n");
    SrLog("SR LeiaSR passthrough INITIALIZED (device=%p hwnd=%p)\n", (void*)device, (void*)hwnd);
    SrLog("Weaver vtable will forward to real SR::IDX12Weaver1 -- lenticular output\n");
    SrLog("will come from LeiaSR runtime on the user's SR display.\n");
    SrLog("================================================================\n");
    LeaveCriticalSection(&g_srLock);
    return true;
}

// Called when the game provides a new input texture whose pointer identity
// differs from what we've seen before.  Typical trigger: user changed
// Odyssey3D settings (Strength / WeaponStrength / Preset) and the game
// re-provisioned its stereo render target.  Without this, LeiaSR's
// internal SRV handles cache the OLD resource and crash on the next
// weave sample.
//
// Game's per-frame order is SetCommandList -> SetInputViewTexture ->
// SetOutputFrameBuffer -> Weave, so by the time we fire here the cmd
// list was already set on the OLD weaver.  Snapshot and re-push it on
// the fresh weaver -- IDX12Weaver1 doc: "It is required to set the
// command list, the viewport and the scissor rect before calling
// weave() or it will throw an exception" (otherwise -> abort).
//
// SRContext + SystemSense are sticky on the context, so they don't
// need to be recreated.  Output FB + format caches are reset so their
// forwarder re-emits setOutputFormat and CreateRTV on the new weaver.
static bool SrRecreateWeaver(const char* reason)
{
    EnterCriticalSection(&g_srLock);
    if (!g_srActive || !g_srDevice) {
        LeaveCriticalSection(&g_srLock);
        return false;
    }
    SrLog("SR: recreating Weaver (reason: %s)\n", reason ? reason : "unspecified");

    ID3D12GraphicsCommandList* savedCmdList = g_srBoundCmdList;

    if (g_srWeaver) {
        __try { g_srWeaver->destroy(); } __except(EXCEPTION_EXECUTE_HANDLER) {}
        g_srWeaver = nullptr;
    }
    g_srBoundInputTex  = nullptr;
    g_srBoundOutputFB  = nullptr;
    g_srBoundOutputFmt = DXGI_FORMAT_UNKNOWN;
    g_srBoundCmdList   = nullptr;

    WeaverErrorCode err = SR::CreateDX12Weaver(g_srContext, g_srDevice, g_srHwnd, &g_srWeaver);
    if (err != WeaverSuccess || !g_srWeaver) {
        SrLog("SR: CreateDX12Weaver (recreate) err=%d; disabling passthrough\n", (int)err);
        g_srActive = false;
        LeaveCriticalSection(&g_srLock);
        return false;
    }
    g_srWeaver->setLatencyInFrames(1);
    g_srWeaver->enableLateLatching(true);

    if (savedCmdList) {
        g_srWeaver->setCommandList(savedCmdList);
        g_srBoundCmdList = savedCmdList;
        SrLog("SR: Weaver recreated OK (cmd list re-pushed: cl=%p)\n", (void*)savedCmdList);
    } else {
        SrLog("SR: Weaver recreated OK (no cached cmd list -- game must call "
              "SetCommandList before Weave or weave() will throw)\n");
    }
    LeaveCriticalSection(&g_srLock);
    return true;
}

// Tear down LeiaSR cleanly.  Called from Device::Destroy (game's own
// shutdown path) and from DLL_PROCESS_DETACH (process-exit backstop).
// Must be idempotent so both can fire without double-free.
//
// Why bother: a leaked IDX12Weaver1 holds GPU resource references and
// an active connection to SRService.exe; if the game is cycling through
// crashes, those zombie references accumulate until the next cold boot.
// Clean teardown also lets SRService know this client is gone, so the
// next launch starts from a known state.
static void SrShutdown(const char* reason)
{
    if (!g_srLockInit) return;
    EnterCriticalSection(&g_srLock);
    if (!g_srActive && !g_srWeaver && !g_srContext) {
        LeaveCriticalSection(&g_srLock);
        return;
    }
    SrLog("SR: shutting down (reason: %s)\n", reason ? reason : "unspecified");

    if (g_srWeaver) {
        __try { g_srWeaver->destroy(); } __except(EXCEPTION_EXECUTE_HANDLER) {}
        g_srWeaver = nullptr;
    }
    if (g_srRtvHeap) {
        g_srRtvHeap->Release();
        g_srRtvHeap = nullptr;
    }
    if (g_srContext) {
        __try { delete g_srContext; } __except(EXCEPTION_EXECUTE_HANDLER) {}
        g_srContext = nullptr;
    }
    g_srBoundOutputFB  = nullptr;
    g_srBoundOutputFmt = DXGI_FORMAT_UNKNOWN;
    g_srBoundCmdList   = nullptr;
    g_srBoundInputTex  = nullptr;
    g_srDevice         = nullptr;
    g_srHwnd           = nullptr;
    g_srActive         = false;
    g_srInitTried      = false;

    LeaveCriticalSection(&g_srLock);
}
#else
static void SrInitEnv() {}
static bool SrLazyInit(void*, HWND) { return false; }
static bool SrRecreateWeaver(const char*) { return false; }
static void SrShutdown(const char*) {}
#endif  // WIZ3D_SR_HAVE_LEIASR

// ----------------------------------------------------------------------------
// Monitor discovery
//
// vl3c's audit documented the GetMonitorRect quirk: Samsung's SDK returns
// {top, left, right, bottom} as int64 -- NOT the standard Win32
// {left, top, right, bottom} order. The game compares the result against
// its own DXGI output enumeration, so returning the actual output rect
// is correct. The int64 widening is just header-level ABI.
// ----------------------------------------------------------------------------
static CRITICAL_SECTION g_monLock;
static bool             g_monInit = false;
static int64_t          g_monRect[4] = { 0, 0, 1920, 1080 };  // {top,left,right,bottom}
static int              g_monW = 1920;
static int              g_monH = 1080;

static void SrMonitorInit()
{
    if (g_monInit) return;
    InitializeCriticalSection(&g_monLock);
    g_monInit = true;

    // Primary monitor as a safe default. The actual "game window's monitor"
    // lookup needs the game's HWND which the Factory::CreateWeaver call
    // supplies -- we'll refresh at that point.
    HMONITOR hm = MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    if (hm && GetMonitorInfoW(hm, &mi))
    {
        EnterCriticalSection(&g_monLock);
        g_monRect[0] = mi.rcMonitor.top;
        g_monRect[1] = mi.rcMonitor.left;
        g_monRect[2] = mi.rcMonitor.right;
        g_monRect[3] = mi.rcMonitor.bottom;
        g_monW = mi.rcMonitor.right  - mi.rcMonitor.left;
        g_monH = mi.rcMonitor.bottom - mi.rcMonitor.top;
        LeaveCriticalSection(&g_monLock);
    }
    SrLog("SR mon init: {top=%lld left=%lld right=%lld bottom=%lld} (%dx%d)\n",
          (long long)g_monRect[0], (long long)g_monRect[1],
          (long long)g_monRect[2], (long long)g_monRect[3],
          g_monW, g_monH);
}

static void SrMonitorRefreshFromHwnd(HWND hwnd)
{
    if (!hwnd) return;
    HMONITOR hm = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    if (!hm) return;
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(hm, &mi)) return;
    EnterCriticalSection(&g_monLock);
    g_monRect[0] = mi.rcMonitor.top;
    g_monRect[1] = mi.rcMonitor.left;
    g_monRect[2] = mi.rcMonitor.right;
    g_monRect[3] = mi.rcMonitor.bottom;
    g_monW = mi.rcMonitor.right  - mi.rcMonitor.left;
    g_monH = mi.rcMonitor.bottom - mi.rcMonitor.top;
    LeaveCriticalSection(&g_monLock);
    SrLog("SR mon from hwnd=%p: {top=%lld left=%lld right=%lld bottom=%lld} (%dx%d)\n",
          (void*)hwnd,
          (long long)g_monRect[0], (long long)g_monRect[1],
          (long long)g_monRect[2], (long long)g_monRect[3],
          g_monW, g_monH);
}

// ----------------------------------------------------------------------------
// Call counters -- first-call logging + periodic rate dump. The game calls
// GetEyePosition / GetResolution / Hint48 very frequently; we don't want a
// log entry per call unless WIZ3D_SR_LOG_SLOTS is set.
// ----------------------------------------------------------------------------
// Per-slot bitset of up to 32 unique thread IDs we've seen call this slot.
// Keyed by TID low-bits (not perfect but cheap and sufficient for CP2077's
// thread count).  Lets us log "slot X now also hit by thread Y" the first
// time a new thread calls each slot, revealing the thread distribution
// used to drive our Odyssey3DSDK -- important for diagnosing the
// intermittent load-screen crash which has race-signature shape.
struct CallCounter {
    volatile LONG count            = 0;
    const char*   name             = nullptr;
    volatile LONG seenTids[8]      = {};   // slot[i] = TID or 0; 8 unique TIDs tracked
};

static void NoteCall(CallCounter& c)
{
    LONG n = InterlockedIncrement(&c.count);
    if (n == 1) SrLog("SR first call: %s\n", c.name ? c.name : "<unnamed>");
    // Periodic rate dump every ~1024 calls -- cheap, keeps the log readable.
    if ((n & 0x3FF) == 0) SrLog("SR %s: %ld calls\n", c.name ? c.name : "<unnamed>", (long)n);

    // New-TID detection: scan the per-slot TID set.  If this TID isn't
    // in the set yet, atomically claim a free slot and log it.  O(8)
    // per call, no allocation, no mutex -- cheap enough even for Hint48's
    // ~340Hz rate.
    LONG tid = (LONG)GetCurrentThreadId();
    for (int i = 0; i < 8; ++i) {
        LONG v = c.seenTids[i];
        if (v == tid) break;                               // already seen
        if (v == 0) {
            if (InterlockedCompareExchange(&c.seenTids[i], tid, 0) == 0) {
                SrLog("SR thread-distribution: %s now called by TID %lu (slot %d of 8)\n",
                      c.name ? c.name : "<unnamed>", (unsigned long)tid, i);
            }
            break;    // we claimed it (or lost the race to another new TID, re-scan next time)
        }
    }

    if (g_logSlotsEveryCall)
        SrLog("SR call: %s (#%ld)\n", c.name ? c.name : "<unnamed>", (long)n);
}

// ----------------------------------------------------------------------------
// Interfaces -- game sees `{void** vtable, state...}` structs. We allocate
// the vtables as static arrays of void* so they're ABI-stable per build.
//
// Factory  (slots 0..31)   -- 2 mapped: CreateDevice@0, CreateWeaver@2
// Device   (slots 0..31)   -- 7 mapped: GetEyePosition@1, GetResolution@2,
//                             GetFloat2_18@3, GetMonitorRect@6, Hint48@9,
//                             IsAvailable@10, Destroy@12
// Weaver   (slots 0..31)   -- 4 mapped: Weave@2, SetInputViewTexture@4,
//                             SetOutputFrameBuffer@6, SetCommandList@7
//
// MSVC _Ref_count_base-shaped vtable (4 slots) is also required for the
// SharedPtr return of Factory::CreateWeaver -- see RefCount section.
// ----------------------------------------------------------------------------

struct Factory;
struct Device;

// Weaver struct -- layout matched to vl3c's reverse-engineered map so the
// game sees the fields it expects at the offsets it expects.  Populated by
// the Weaver forwarders (SetInputViewTexture, SetOutputFrameBuffer,
// SetCommandList) so a thread reading these back during the loading screen
// finds non-zero state matching what the real Samsung SDK would store.
//
// Load-screen crash hypothesis (as of 5f1a4985): the game -- or more
// likely LeiaSR's background threads interrogating a game-owned structure
// that was initialised from our Weaver -- derives a handle/pointer whose
// zero-init value becomes a -1 sentinel downstream, and later dereferences
// it (crash at 0xFFFFFFFFFFFFFFFF).  Populating these fields with the
// pointers the game hands us gives the game whatever state it stored to
// read back later.
struct Weaver
{
    void**      vtable;        // 0x00
    void*       input;         // 0x08  (really ID3D12Resource* when LeiaSR; void* here keeps x86 happy)
    int         inputFormat;   // 0x10 (+ 4 padding)
    int         _pad0;
    void*       output;        // 0x18  (really ID3D12Resource*)
    void*       cmdList;       // 0x20  (really ID3D12GraphicsCommandList*)
    uint64_t    lastInputW;    // 0x28
    uint64_t    lastOutputW;   // 0x30
    unsigned    lastInputH;    // 0x38
    unsigned    lastOutputH;   // 0x3C
};

typedef Device**         (__stdcall *FactoryCreateDeviceFn )(Factory* self, Device** out);
typedef struct SharedPtr {
    Weaver*          ptr;
    struct RefCount* rc;
} SharedPtr;
// d3dDevice / allocator are ID3D12Device / ID3D12CommandAllocator in practice,
// but we never dereference them (SBSDebug path), so void* keeps us free of
// the d3d12 header pull.
typedef SharedPtr*       (__stdcall *FactoryCreateWeaverFn )(Factory* self, SharedPtr* retSlot,
                                                             Device* dev, void* d3dDevice,
                                                             void* allocator, void* arg5,
                                                             uintptr_t arg6, uintptr_t arg7,
                                                             HWND hwnd);

typedef float*           (__stdcall *DeviceGetEyePositionFn)(Device* self, float out[3], int eye);
typedef float*           (__stdcall *DeviceGetResolutionFn )(Device* self, float out[2]);
typedef float*           (__stdcall *DeviceGetFloat2_18Fn  )(Device* self, float out[2]);
typedef int64_t*         (__stdcall *DeviceGetMonitorRectFn)(Device* self, int64_t out[4]);
typedef uintptr_t        (__stdcall *DeviceHint48Fn        )(Device* self, uintptr_t flag);
typedef uint8_t          (__stdcall *DeviceIsAvailableFn   )(Device* self);
typedef void*            (__stdcall *DeviceDestroyFn       )(Device* self, unsigned flags);

typedef void             (__stdcall *WeaverWeaveFn               )(Weaver* self, unsigned w, unsigned h, int x, int y);
typedef void             (__stdcall *WeaverSetInputViewTextureFn )(Weaver* self, void* tex, int dxgiFormat);
typedef void             (__stdcall *WeaverSetOutputFrameBufferFn)(Weaver* self, void* tex);
typedef void             (__stdcall *WeaverSetCommandListFn      )(Weaver* self, void* commandList);

// ----------------------------------------------------------------------------
// Unmapped-slot stub.
//
// Every unmapped slot of every vtable routes here. We don't know the real
// signature; the safe behavior is to log the slot invocation and return
// the first argument unchanged. That matches the game's out-param-getter
// idiom (caller provides a pointer, callee writes to it, returns it) and
// is a no-op pass-through for self-receivers.
//
// Because __stdcall callees pop the args, calling this with a mismatched
// argument count on x86 would corrupt the stack. We therefore declare the
// stub varargs on x86 (caller cleans up) and __fastcall-shaped on x64
// (register-passed, callee doesn't pop). On x64 the first arg (self /
// out-pointer) arrives in RCX and the return in RAX -- we just return
// RCX. On x86 we return the first arg which is at the top of the stack.
//
// Each vtable slot thus needs its own trampoline that captures the slot
// index, logs it, then returns the first arg. We generate those below.
// ----------------------------------------------------------------------------

#if defined(_M_X64) || defined(__x86_64__)
    #define SR_STUB_CALLCONV
#else
    #define SR_STUB_CALLCONV __stdcall
#endif

static void SrLogUnmapped(const char* iface, unsigned slot, void* arg0)
{
    SrLog("SR UNMAPPED %s[%u] called -- returning arg0=%p (first-arg passthrough stub)\n",
          iface, slot, arg0);
}

// One stub per slot per interface so logging reports the actual slot that
// was hit. 32 slots * 3 interfaces = 96 stubs. We macro them out for compact
// source; the generated code is a one-line log + return-arg0.
//
// On x86 __stdcall unknown-arity: unsafe to pop. We declare these naked-ish
// and only touch arg0 (ECX in __fastcall or [ESP+4] in __stdcall). Standard
// stdcall callees pop a size encoded in the function; since we don't know
// the arity, we use the __cdecl variant where the CALLER pops -- then our
// return doesn't need to adjust ESP by the arg count.
//
// NOTE: in practice the game never calls unmapped slots in CP2077 2.5. The
// slots exist for future-game-compatibility and for the WIZ3D_SR_LOG_SLOTS
// diagnostic path. If a new game DOES call an unmapped slot, that becomes a
// bug report + a decision whether to implement it.
//
// For the vast majority of unmapped slots the game simply never calls them,
// so the arity issue is moot. For the few slots where this stub DOES fire,
// log + return-arg0 is the safest minimal response.

#define SR_UNMAPPED_STUB(IFACE, SLOT)                                           \
    static void* __cdecl SrUnmapped_##IFACE##_##SLOT(void* arg0, ...)           \
    {                                                                           \
        SrLogUnmapped(#IFACE, SLOT, arg0);                                      \
        return arg0;                                                            \
    }

// Factory, Device, Weaver -- all 32 slots per interface.
// (Compilers will DCE the unused stubs since they're referenced by vtable
// init only -- generating all 96 keeps the macro expansion mechanical.)
#define SR_UNMAPPED_ALL_32(IFACE)                                               \
    SR_UNMAPPED_STUB(IFACE, 0)  SR_UNMAPPED_STUB(IFACE, 1)                      \
    SR_UNMAPPED_STUB(IFACE, 2)  SR_UNMAPPED_STUB(IFACE, 3)                      \
    SR_UNMAPPED_STUB(IFACE, 4)  SR_UNMAPPED_STUB(IFACE, 5)                      \
    SR_UNMAPPED_STUB(IFACE, 6)  SR_UNMAPPED_STUB(IFACE, 7)                      \
    SR_UNMAPPED_STUB(IFACE, 8)  SR_UNMAPPED_STUB(IFACE, 9)                      \
    SR_UNMAPPED_STUB(IFACE, 10) SR_UNMAPPED_STUB(IFACE, 11)                     \
    SR_UNMAPPED_STUB(IFACE, 12) SR_UNMAPPED_STUB(IFACE, 13)                     \
    SR_UNMAPPED_STUB(IFACE, 14) SR_UNMAPPED_STUB(IFACE, 15)                     \
    SR_UNMAPPED_STUB(IFACE, 16) SR_UNMAPPED_STUB(IFACE, 17)                     \
    SR_UNMAPPED_STUB(IFACE, 18) SR_UNMAPPED_STUB(IFACE, 19)                     \
    SR_UNMAPPED_STUB(IFACE, 20) SR_UNMAPPED_STUB(IFACE, 21)                     \
    SR_UNMAPPED_STUB(IFACE, 22) SR_UNMAPPED_STUB(IFACE, 23)                     \
    SR_UNMAPPED_STUB(IFACE, 24) SR_UNMAPPED_STUB(IFACE, 25)                     \
    SR_UNMAPPED_STUB(IFACE, 26) SR_UNMAPPED_STUB(IFACE, 27)                     \
    SR_UNMAPPED_STUB(IFACE, 28) SR_UNMAPPED_STUB(IFACE, 29)                     \
    SR_UNMAPPED_STUB(IFACE, 30) SR_UNMAPPED_STUB(IFACE, 31)

SR_UNMAPPED_ALL_32(Factory)
SR_UNMAPPED_ALL_32(Device)
SR_UNMAPPED_ALL_32(Weaver)

#define SR_UNMAPPED_SLOT(IFACE, SLOT) (void*)&SrUnmapped_##IFACE##_##SLOT

// ----------------------------------------------------------------------------
// Device mapped slots
// ----------------------------------------------------------------------------

static CallCounter g_cc_GetEyePosition = { 0, "Device::GetEyePosition" };
static CallCounter g_cc_GetResolution  = { 0, "Device::GetResolution"  };
static CallCounter g_cc_GetFloat2_18   = { 0, "Device::GetFloat2_18"   };
static CallCounter g_cc_GetMonitorRect = { 0, "Device::GetMonitorRect" };
static CallCounter g_cc_Hint48         = { 0, "Device::Hint48"         };
static CallCounter g_cc_IsAvailable    = { 0, "Device::IsAvailable"    };
static CallCounter g_cc_Destroy        = { 0, "Device::Destroy"        };

static float* __stdcall DevGetEyePosition(Device* self, float out[3], int eye)
{
    NoteCall(g_cc_GetEyePosition);

#if defined(WIZ3D_SR_HAVE_LEIASR)
    // Real-time eye tracking: hand the game LeiaSR's current tracker
    // positions on every call.  The lazy-cache experiment (5296c85b)
    // proved that the 128Hz x 8-thread SDK polling isn't what causes
    // the intermittent load crash -- the cache stopped the spam and
    // the crash still fired.  Real-time tracking is what the user
    // asked for and what gives the best stereo (per-frame updated
    // camera offsets matching the viewer's actual head position).
    if (g_srActive && g_srWeaver && out)
    {
        float lEye[3] = {0}, rEye[3] = {0};
        bool gotEyes = false;
        __try {
            g_srWeaver->getPredictedEyePositions(lEye, rEye);
            gotEyes = true;
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            // Fall through to fake-IPD path.
        }
        if (gotEyes) {
            const float* eyeV = (eye == 0) ? lEye : rEye;
            out[0] = eyeV[0];
            out[1] = eyeV[1];
            out[2] = eyeV[2];
            return out;
        }
    }
#endif

    // Fallback (SR not active, SBSDebug mode, or getPredictedEyePositions
    // threw): hardcoded IPD/2 at nominal view distance.  mm units;
    // right-handed; x = +-IPD/2, y = 0, z = eye_z_mm.
    const float x = (eye == 0 ? -1.0f : +1.0f) * (g_cfg.ipd_mm * 0.5f);
    if (out)
    {
        out[0] = x;
        out[1] = 0.0f;
        out[2] = g_cfg.eye_z_mm;
    }
    return out;
}

static float* __stdcall DevGetResolution(Device* self, float out[2])
{
    NoteCall(g_cc_GetResolution);
    EnterCriticalSection(&g_monLock);
    const int w = g_monW, h = g_monH;
    LeaveCriticalSection(&g_monLock);
    if (out) { out[0] = (float)w; out[1] = (float)h; }
    return out;
}

static float* __stdcall DevGetFloat2_18(Device* self, float out[2])
{
    NoteCall(g_cc_GetFloat2_18);
    // vl3c's 0.2.0 (2026-10-29) switched this from {10000, 10000} (which
    // reproduced the game's 100.0-defaulting field after the x0.01 scaling)
    // to the real Samsung Odyssey 27" panel dims in cm (59.77 x 33.62).
    // Their CHANGELOG notes: "world depth showed no visible change between
    // 10000 and 120 x 67.5, so it isn't a setting" -- so this isn't a
    // depth/scale knob, but matching the real panel is defensible defaulting
    // in case a future game version uses it differently.
    if (out) { out[0] = 59.77f; out[1] = 33.62f; }
    return out;
}

static int64_t* __stdcall DevGetMonitorRect(Device* self, int64_t out[4])
{
    NoteCall(g_cc_GetMonitorRect);
    EnterCriticalSection(&g_monLock);
    if (out)
    {
        // {top, left, right, bottom} -- NOT standard Win32 order (see header note).
        out[0] = g_monRect[0];
        out[1] = g_monRect[1];
        out[2] = g_monRect[2];
        out[3] = g_monRect[3];
    }
    LeaveCriticalSection(&g_monLock);
    return out;
}

static uintptr_t __stdcall DevHint48(Device* self, uintptr_t flag)
{
    NoteCall(g_cc_Hint48);
    // 400-900 Hz hint; game ignores return. Treat as a tracking/activity poll.
    (void)flag;
    return 0;
}

static uint8_t __stdcall DevIsAvailable(Device* self)
{
    NoteCall(g_cc_IsAvailable);
    // "Yes, an SR display is attached." No EDID check, no monitor-model probe.
    return 1;
}

static void* __stdcall DevDestroy(Device* self, unsigned flags)
{
    NoteCall(g_cc_Destroy);
    SrLog("SR Device::Destroy(self=%p, flags=0x%X)\n", (void*)self, flags);
    // Device::Destroy is the game's explicit "I'm done with the SDK" signal.
    // Tear down LeiaSR here so SRService.exe sees us disconnect cleanly and
    // the next launch isn't inheriting state from a zombie connection.
    SrShutdown("Device::Destroy");
    if (flags & 1u) HeapFree(GetProcessHeap(), 0, self);
    return nullptr;
}

// ----------------------------------------------------------------------------
// Weaver mapped slots.
//
// Thread-safety note (critical, learned from the intermittent load-screen
// crash on CP2077): the game calls these forwards from EIGHT concurrent
// threads (async-compute cmd-list recording -- diagnostic log revealed
// SetCommandList/SetInputViewTexture/SetOutputFrameBuffer/Weave each hit
// by 8 unique TIDs within ~140ms).  LeiaSR's IDX12Weaver1 is NOT
// documented thread-safe, and the SDK example main.cpp uses only a
// single rendering thread.  Letting 8 threads concurrently poke LeiaSR's
// internal state corrupts it -- the symptom is a heap-corruption-shape
// crash (varying 0x28 / 0x550 / 0xFFFFFFFFFFFFFFFF addresses) that fires
// later from LeiaSR's background threads.
//
// Fix: serialize every forward with g_srLock.  Each call is atomic from
// LeiaSR's perspective; the game's threads will stall on each other,
// but that's the right trade for correctness.  Performance hit will
// show if CP2077's weave work is a bottleneck -- so far it isn't,
// 1024 weave frames in 15 wall-clock seconds is 68Hz, below the 8-way
// theoretical cap.
// ----------------------------------------------------------------------------

static CallCounter g_cc_Weave        = { 0, "Weaver::Weave" };
static CallCounter g_cc_SetInputView = { 0, "Weaver::SetInputViewTexture" };
static CallCounter g_cc_SetOutputFB  = { 0, "Weaver::SetOutputFrameBuffer" };
static CallCounter g_cc_SetCmdList   = { 0, "Weaver::SetCommandList" };

static void __stdcall WeaverWeave(Weaver* self, unsigned w, unsigned h, int x, int y)
{
    NoteCall(g_cc_Weave);
    (void)self;

#if defined(WIZ3D_SR_HAVE_LEIASR)
    EnterCriticalSection(&g_srLock);
    if (g_srActive && g_srWeaver) {
        // Gap detection: log when weave resumes after >1s pause.  This
        // reveals menu->gameplay transitions clearly in the diagnostic
        // log -- useful for isolating the intermittent load-screen crash,
        // which happens somewhere between "last weave of menu" and "first
        // weave of gameplay".
        static ULONGLONG lastWeaveTick = 0;
        ULONGLONG now = GetTickCount64();
        if (lastWeaveTick && now - lastWeaveTick > 1000) {
            SrLog("SR Weaver::Weave: resuming after %llums gap "
                  "(menu->gameplay or similar transition)\n",
                  (unsigned long long)(now - lastWeaveTick));
        }
        lastWeaveTick = now;

        // RT binding is done in SetCommandList (test 1 pattern). Here we
        // only set weave params + record LeiaSR's weave commands onto
        // whichever cmd list we last cached.
        D3D12_VIEWPORT vp = { (float)x, (float)y, (float)w, (float)h, 0.f, 1.f };
        D3D12_RECT     sr = { x, y, x + (int)w, y + (int)h };
        g_srWeaver->setViewport(vp);
        g_srWeaver->setScissorRect(sr);

        // One-time diagnostic: on the first few weaves, dump what LeiaSR
        // thinks the user's eye positions are. If camera+initialize() hooked
        // up correctly these should be moving, non-zero values. If they are
        // (0,0,0) or NaN, LeiaSR's tracker isn't producing eye data and
        // weave() may be falling back to mono.
        static int logged = 0;
        if (logged < 5) {
            float lEye[3] = {0}, rEye[3] = {0};
            __try {
                g_srWeaver->getPredictedEyePositions(lEye, rEye);
                SrLog("SR Weaver::Weave #%d: w=%u h=%u pos=(%d,%d)  "
                      "eyeL=(%.1f,%.1f,%.1f)  eyeR=(%.1f,%.1f,%.1f)\n",
                      logged, w, h, x, y,
                      lEye[0], lEye[1], lEye[2], rEye[0], rEye[1], rEye[2]);
            } __except(EXCEPTION_EXECUTE_HANDLER) {
                SrLog("SR Weaver::Weave #%d: getPredictedEyePositions threw\n", logged);
            }
            ++logged;
        }

        g_srWeaver->weave();
        LeaveCriticalSection(&g_srLock);
        return;
    }
    LeaveCriticalSection(&g_srLock);
#endif
    // SBSDebug fallback: no-op (game writes half-SBS to its own BB).
    (void)w; (void)h; (void)x; (void)y;
}

static void __stdcall WeaverSetInputViewTexture(Weaver* self, void* tex, int dxgiFormat)
{
    NoteCall(g_cc_SetInputView);
    (void)self;

#if defined(WIZ3D_SR_HAVE_LEIASR)
    EnterCriticalSection(&g_srLock);
    if (g_srActive && g_srWeaver && tex) {
        ID3D12Resource* r = (ID3D12Resource*)tex;

        // Resource-identity change -> stale LeiaSR SRV handles -> crash.
        // Rebuild the Weaver so LeiaSR builds fresh handles against the
        // new texture.  See SrRecreateWeaver header for the full rationale.
        if (g_srBoundInputTex && r != g_srBoundInputTex) {
            SrLog("SR Weaver::SetInputViewTexture: input texture changed "
                  "(old=%p -> new=%p); recreating Weaver\n",
                  (void*)g_srBoundInputTex, (void*)r);
            SrRecreateWeaver("input texture resource identity changed");
            if (!g_srWeaver) { LeaveCriticalSection(&g_srLock); return; }
        }

        D3D12_RESOURCE_DESC d = r->GetDesc();
        g_srWeaver->setInputViewTexture(r, (int)d.Width, (int)d.Height, (DXGI_FORMAT)dxgiFormat);
        g_srBoundInputTex = r;

        // Populate the Weaver struct fields the game may read back later.
        // vl3c's Weaver stores these for its own purposes but the layout
        // match means the game sees non-zero state at the expected
        // offsets if it ever reads directly from the Weaver object.
        if (self) {
            self->input       = r;
            self->inputFormat = dxgiFormat;
            self->lastInputW  = d.Width;
            self->lastInputH  = d.Height;
        }

        static int logged = 0;
        if (logged++ < 3)
            SrLog("SR Weaver::SetInputViewTexture: tex=%p dxgiFormat=%d (fwd to LeiaSR %llux%u)\n",
                  tex, dxgiFormat, (unsigned long long)d.Width, d.Height);
        LeaveCriticalSection(&g_srLock);
        return;
    }
    LeaveCriticalSection(&g_srLock);
#endif
    // SBSDebug fallback: populate struct state for game-side reads
    // without requiring D3D12 headers (x86 build has no d3d12.h).  Width
    // and height can't be queried without GetDesc, so leave those at zero
    // in the SBSDebug path -- the pointer-slot populate is the important
    // part (that's what the game would read as a non-null handle).
    if (self && tex) {
        self->input       = tex;
        self->inputFormat = dxgiFormat;
    }
    SrLog("SR Weaver::SetInputViewTexture: tex=%p dxgiFormat=%d (ignored under SBSDebug; struct populated)\n",
          tex, dxgiFormat);
}

static void __stdcall WeaverSetOutputFrameBuffer(Weaver* self, void* tex)
{
    NoteCall(g_cc_SetOutputFB);
    (void)self;

    // Populate the struct state the game may read regardless of mode.
    if (self && tex) {
        self->output = tex;
    }
#if defined(WIZ3D_SR_HAVE_LEIASR)
    EnterCriticalSection(&g_srLock);
    if (g_srActive && g_srWeaver && tex && g_srDevice && g_srRtvHeap) {
        ID3D12Resource* r = (ID3D12Resource*)tex;
        D3D12_RESOURCE_DESC d = r->GetDesc();
        if (self) {
            self->lastOutputW = d.Width;
            self->lastOutputH = d.Height;
        }
        if (d.Format != g_srBoundOutputFmt) {
            g_srWeaver->setOutputFormat(d.Format);
            g_srBoundOutputFmt = d.Format;

            // Tell LeiaSR whether to do shader-side sRGB conversion based
            // on the output format.  Per IWeaverBase1 doc comments:
            //   "When input is already linear or set for hardware
            //    conversion, set read to false."
            //   "When output is treated as linear (set for hardware
            //    conversion), set write to false."
            // _SRGB-suffixed DXGI formats are "set for hardware
            // conversion" -- the GPU does the gamma on sample/store.  If
            // we don't tell LeiaSR, its shader also does gamma and we
            // double-convert.  CP2077 uses format 29 (R8G8B8A8_UNORM_SRGB)
            // so this applies.
            bool isSrgb =
                d.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB    ||
                d.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB    ||
                d.Format == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
            g_srWeaver->setShaderSRGBConversion(!isSrgb, !isSrgb);

            static int logged = 0;
            if (logged++ < 3)
                SrLog("SR Weaver::SetOutputFrameBuffer: tex=%p fmt=%d (fwd setOutputFormat; "
                      "ShaderSRGBConversion=%s)\n",
                      tex, (int)d.Format, isSrgb ? "false,false (HW gamma)" : "true,true (shader gamma)");
        }
        // Create / refresh RTV so later SetCommandList's cmd-list can bind the
        // right RT. We re-create unconditionally -- cheap, 1 slot, every bind
        // overwrites the previous.
        if (r != g_srBoundOutputFB) {
            D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
            rtvDesc.Format        = d.Format;
            rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
            g_srDevice->CreateRenderTargetView(r, &rtvDesc,
                g_srRtvHeap->GetCPUDescriptorHandleForHeapStart());
            g_srBoundOutputFB = r;
        }
        LeaveCriticalSection(&g_srLock);
        return;
    }
    LeaveCriticalSection(&g_srLock);
#endif
    SrLog("SR Weaver::SetOutputFrameBuffer: tex=%p (ignored under SBSDebug)\n", tex);
}

static void __stdcall WeaverSetCommandList(Weaver* self, void* commandList)
{
    NoteCall(g_cc_SetCmdList);
    (void)self;

    // Populate the struct state the game may read regardless of mode.
    if (self && commandList) {
        self->cmdList = commandList;
    }
#if defined(WIZ3D_SR_HAVE_LEIASR)
    EnterCriticalSection(&g_srLock);
    if (g_srActive && g_srWeaver && commandList) {
        ID3D12GraphicsCommandList* cl = (ID3D12GraphicsCommandList*)commandList;
        g_srWeaver->setCommandList(cl);
        g_srBoundCmdList = cl;
        // Bind our RTV right here (test 1 pattern). On frame 1 the RTV
        // descriptor is still uninitialized at this point -- CreateRTV
        // happens later in SetOutputFrameBuffer -- but DX12 reads the
        // descriptor's resource reference at command-list execute time,
        // not record time, so by the time the GPU runs this binding it
        // sees the live RTV. Moving this to Weave() broke stereo output
        // (observed in b9012739), reverted here.
        if (g_srRtvHeap) {
            D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_srRtvHeap->GetCPUDescriptorHandleForHeapStart();
            cl->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        }
        static int logged = 0;
        if (logged++ < 3) SrLog("SR Weaver::SetCommandList: cl=%p (fwd + RT bound)\n", commandList);
        LeaveCriticalSection(&g_srLock);
        return;
    }
    LeaveCriticalSection(&g_srLock);
#endif
    SrLog("SR Weaver::SetCommandList: cl=%p (ignored under SBSDebug)\n", commandList);
}

// ----------------------------------------------------------------------------
// Weaver + its RefCount control block (MSVC _Ref_count_base shape)
//
// Factory::CreateWeaver returns a std::shared_ptr<Weaver> by hidden pointer.
// MSVC's shared_ptr is {ptr, _Ref_count_base*}; the control block has a
// 4-slot vtable {_Destroy, _Delete_this, ~dtor, _Get_deleter} followed by
// two volatile LONG refcounts and the managed pointer.
// ----------------------------------------------------------------------------

struct RefCount
{
    void** vtable;             // 4-slot _Ref_count_base
    volatile LONG uses;        // strong refcount
    volatile LONG weaks;       // weak refcount
    Weaver*       obj;         // managed pointer
};

static void  __stdcall Rc_Destroy    (RefCount* self);
static void  __stdcall Rc_DeleteThis (RefCount* self);
static void* __stdcall Rc_Dtor       (RefCount* self, unsigned flags);
static void* __stdcall Rc_GetDeleter (RefCount* self, const void* type);

static void* g_RcVtable[4] = {
    (void*)&Rc_Destroy,
    (void*)&Rc_DeleteThis,
    (void*)&Rc_Dtor,
    (void*)&Rc_GetDeleter,
};

static void __stdcall Rc_Destroy(RefCount* self)
{
    if (!self) return;
    SrLog("SR RefCount::_Destroy(self=%p obj=%p)\n", (void*)self, (void*)self->obj);
    if (self->obj) {
        HeapFree(GetProcessHeap(), 0, self->obj);
        self->obj = nullptr;
    }
}

static void __stdcall Rc_DeleteThis(RefCount* self)
{
    if (!self) return;
    SrLog("SR RefCount::_Delete_this(self=%p)\n", (void*)self);
    HeapFree(GetProcessHeap(), 0, self);
}

static void* __stdcall Rc_Dtor(RefCount* self, unsigned flags)
{
    SrLog("SR RefCount::~dtor(self=%p flags=0x%X)\n", (void*)self, flags);
    if (self && self->obj) {
        HeapFree(GetProcessHeap(), 0, self->obj);
        self->obj = nullptr;
    }
    if ((flags & 1u) && self) HeapFree(GetProcessHeap(), 0, self);
    return nullptr;
}

static void* __stdcall Rc_GetDeleter(RefCount* self, const void* type)
{
    (void)self; (void)type;
    return nullptr;
}

static void* g_WeaverVtable[kVtableSlots];

static Weaver* AllocateWeaver()
{
    Weaver* w = (Weaver*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(Weaver));
    if (!w) return nullptr;
    w->vtable = g_WeaverVtable;
    return w;
}

static SharedPtr MakeWeaverSharedPtr()
{
    Weaver*   w  = AllocateWeaver();
    RefCount* rc = (RefCount*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(RefCount));
    if (rc) {
        rc->vtable = g_RcVtable;
        rc->uses   = 1;
        rc->weaks  = 1;
        rc->obj    = w;
    }
    return SharedPtr{ w, rc };
}

// ----------------------------------------------------------------------------
// Factory mapped slots
// ----------------------------------------------------------------------------

struct Device
{
    void**   vtable;    // 32-slot
    uint8_t  reserved[64];
};

struct Factory
{
    void**   vtable;    // 32-slot
    uint8_t  reserved[64];
};

static void* g_DeviceVtable[kVtableSlots];
static void* g_FactoryVtable[kVtableSlots];

static Device* AllocateDevice()
{
    Device* d = (Device*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(Device));
    if (!d) return nullptr;
    d->vtable = g_DeviceVtable;
    SrLog("SR Factory::CreateDevice: allocated Device=%p vtable=%p\n",
          (void*)d, (void*)g_DeviceVtable);
    return d;
}

static Device** __stdcall FacCreateDevice(Factory* self, Device** out)
{
    SrLog("SR Factory::CreateDevice(self=%p, out=%p)\n", (void*)self, (void*)out);
    Device* dev = AllocateDevice();
    if (out) *out = dev;
    return out;
}

static SharedPtr* __stdcall FacCreateWeaver(Factory* self, SharedPtr* retSlot,
                                            Device* dev, void* d3dDevice,
                                            void* allocator, void* arg5,
                                            uintptr_t arg6, uintptr_t arg7,
                                            HWND hwnd)
{
    SrLog("SR Factory::CreateWeaver: dev=%p d3dDevice=%p allocator=%p arg5=%p arg6=%p arg7=%p hwnd=%p\n",
          (void*)dev, (void*)d3dDevice, (void*)allocator,
          arg5, (void*)arg6, (void*)arg7, (void*)hwnd);

    // Game's hwnd is the Samsung-SDK path's definitive "where should stereo
    // output land" cue. Refresh the monitor rect from it so GetMonitorRect
    // reports the game's actual target display rather than the primary.
    SrMonitorRefreshFromHwnd(hwnd);

#if defined(WIZ3D_SR_HAVE_LEIASR)
    // Lazy LeiaSR init here -- game just supplied us the ID3D12Device + hwnd
    // we need. If the runtime isn't present or mode is forced sbsdebug, this
    // returns false and our Weaver vtable stays in no-op mode.
    SrLazyInit((ID3D12Device*)d3dDevice, hwnd);
#endif

    SharedPtr sp = MakeWeaverSharedPtr();
    SrLog("SR Factory::CreateWeaver: Weaver=%p RefCount=%p\n",
          (void*)sp.ptr, (void*)sp.rc);
    if (retSlot) *retSlot = sp;
    return retSlot;
}

// ----------------------------------------------------------------------------
// Vtable initialization -- runs once in DllMain(DLL_PROCESS_ATTACH)
// ----------------------------------------------------------------------------

static void InitFactoryVtable()
{
    for (unsigned i = 0; i < kVtableSlots; ++i) g_FactoryVtable[i] = nullptr;
    g_FactoryVtable[0]  = (void*)&FacCreateDevice;
    g_FactoryVtable[1]  = SR_UNMAPPED_SLOT(Factory, 1);
    g_FactoryVtable[2]  = (void*)&FacCreateWeaver;
    for (unsigned i = 3; i < kVtableSlots; ++i)
    {
        g_FactoryVtable[i] = (void*)&SrUnmapped_Factory_31;    // single fallback is fine
    }
    // Overwrite with per-slot stubs where we care about slot-identified logging.
    g_FactoryVtable[3]  = SR_UNMAPPED_SLOT(Factory, 3);
    g_FactoryVtable[4]  = SR_UNMAPPED_SLOT(Factory, 4);
    g_FactoryVtable[5]  = SR_UNMAPPED_SLOT(Factory, 5);
    g_FactoryVtable[6]  = SR_UNMAPPED_SLOT(Factory, 6);
    g_FactoryVtable[7]  = SR_UNMAPPED_SLOT(Factory, 7);
    g_FactoryVtable[8]  = SR_UNMAPPED_SLOT(Factory, 8);
    g_FactoryVtable[9]  = SR_UNMAPPED_SLOT(Factory, 9);
    g_FactoryVtable[10] = SR_UNMAPPED_SLOT(Factory, 10);
    g_FactoryVtable[11] = SR_UNMAPPED_SLOT(Factory, 11);
    g_FactoryVtable[12] = SR_UNMAPPED_SLOT(Factory, 12);
    g_FactoryVtable[13] = SR_UNMAPPED_SLOT(Factory, 13);
    g_FactoryVtable[14] = SR_UNMAPPED_SLOT(Factory, 14);
    g_FactoryVtable[15] = SR_UNMAPPED_SLOT(Factory, 15);
    g_FactoryVtable[16] = SR_UNMAPPED_SLOT(Factory, 16);
    g_FactoryVtable[17] = SR_UNMAPPED_SLOT(Factory, 17);
    g_FactoryVtable[18] = SR_UNMAPPED_SLOT(Factory, 18);
    g_FactoryVtable[19] = SR_UNMAPPED_SLOT(Factory, 19);
    g_FactoryVtable[20] = SR_UNMAPPED_SLOT(Factory, 20);
    g_FactoryVtable[21] = SR_UNMAPPED_SLOT(Factory, 21);
    g_FactoryVtable[22] = SR_UNMAPPED_SLOT(Factory, 22);
    g_FactoryVtable[23] = SR_UNMAPPED_SLOT(Factory, 23);
    g_FactoryVtable[24] = SR_UNMAPPED_SLOT(Factory, 24);
    g_FactoryVtable[25] = SR_UNMAPPED_SLOT(Factory, 25);
    g_FactoryVtable[26] = SR_UNMAPPED_SLOT(Factory, 26);
    g_FactoryVtable[27] = SR_UNMAPPED_SLOT(Factory, 27);
    g_FactoryVtable[28] = SR_UNMAPPED_SLOT(Factory, 28);
    g_FactoryVtable[29] = SR_UNMAPPED_SLOT(Factory, 29);
    g_FactoryVtable[30] = SR_UNMAPPED_SLOT(Factory, 30);
    g_FactoryVtable[31] = SR_UNMAPPED_SLOT(Factory, 31);
}

static void InitDeviceVtable()
{
    for (unsigned i = 0; i < kVtableSlots; ++i)
    {
        // Default to the per-slot stub so logs identify which slot got hit.
        switch (i) {
            case 0:  g_DeviceVtable[i] = SR_UNMAPPED_SLOT(Device, 0);  break;
            case 1:  g_DeviceVtable[i] = (void*)&DevGetEyePosition;    break;
            case 2:  g_DeviceVtable[i] = (void*)&DevGetResolution;     break;
            case 3:  g_DeviceVtable[i] = (void*)&DevGetFloat2_18;      break;
            case 4:  g_DeviceVtable[i] = SR_UNMAPPED_SLOT(Device, 4);  break;
            case 5:  g_DeviceVtable[i] = SR_UNMAPPED_SLOT(Device, 5);  break;
            case 6:  g_DeviceVtable[i] = (void*)&DevGetMonitorRect;    break;
            case 7:  g_DeviceVtable[i] = SR_UNMAPPED_SLOT(Device, 7);  break;
            case 8:  g_DeviceVtable[i] = SR_UNMAPPED_SLOT(Device, 8);  break;
            case 9:  g_DeviceVtable[i] = (void*)&DevHint48;            break;
            case 10: g_DeviceVtable[i] = (void*)&DevIsAvailable;       break;
            case 11: g_DeviceVtable[i] = SR_UNMAPPED_SLOT(Device, 11); break;
            case 12: g_DeviceVtable[i] = (void*)&DevDestroy;           break;
            case 13: g_DeviceVtable[i] = SR_UNMAPPED_SLOT(Device, 13); break;
            case 14: g_DeviceVtable[i] = SR_UNMAPPED_SLOT(Device, 14); break;
            case 15: g_DeviceVtable[i] = SR_UNMAPPED_SLOT(Device, 15); break;
            case 16: g_DeviceVtable[i] = SR_UNMAPPED_SLOT(Device, 16); break;
            case 17: g_DeviceVtable[i] = SR_UNMAPPED_SLOT(Device, 17); break;
            case 18: g_DeviceVtable[i] = SR_UNMAPPED_SLOT(Device, 18); break;
            case 19: g_DeviceVtable[i] = SR_UNMAPPED_SLOT(Device, 19); break;
            case 20: g_DeviceVtable[i] = SR_UNMAPPED_SLOT(Device, 20); break;
            case 21: g_DeviceVtable[i] = SR_UNMAPPED_SLOT(Device, 21); break;
            case 22: g_DeviceVtable[i] = SR_UNMAPPED_SLOT(Device, 22); break;
            case 23: g_DeviceVtable[i] = SR_UNMAPPED_SLOT(Device, 23); break;
            case 24: g_DeviceVtable[i] = SR_UNMAPPED_SLOT(Device, 24); break;
            case 25: g_DeviceVtable[i] = SR_UNMAPPED_SLOT(Device, 25); break;
            case 26: g_DeviceVtable[i] = SR_UNMAPPED_SLOT(Device, 26); break;
            case 27: g_DeviceVtable[i] = SR_UNMAPPED_SLOT(Device, 27); break;
            case 28: g_DeviceVtable[i] = SR_UNMAPPED_SLOT(Device, 28); break;
            case 29: g_DeviceVtable[i] = SR_UNMAPPED_SLOT(Device, 29); break;
            case 30: g_DeviceVtable[i] = SR_UNMAPPED_SLOT(Device, 30); break;
            case 31: g_DeviceVtable[i] = SR_UNMAPPED_SLOT(Device, 31); break;
        }
    }
}

static void InitWeaverVtable()
{
    for (unsigned i = 0; i < kVtableSlots; ++i)
    {
        switch (i) {
            case 0:  g_WeaverVtable[i] = SR_UNMAPPED_SLOT(Weaver, 0);  break;
            case 1:  g_WeaverVtable[i] = SR_UNMAPPED_SLOT(Weaver, 1);  break;
            case 2:  g_WeaverVtable[i] = (void*)&WeaverWeave;          break;
            case 3:  g_WeaverVtable[i] = SR_UNMAPPED_SLOT(Weaver, 3);  break;
            case 4:  g_WeaverVtable[i] = (void*)&WeaverSetInputViewTexture;    break;
            case 5:  g_WeaverVtable[i] = SR_UNMAPPED_SLOT(Weaver, 5);  break;
            case 6:  g_WeaverVtable[i] = (void*)&WeaverSetOutputFrameBuffer;   break;
            case 7:  g_WeaverVtable[i] = (void*)&WeaverSetCommandList;         break;
            case 8:  g_WeaverVtable[i] = SR_UNMAPPED_SLOT(Weaver, 8);  break;
            case 9:  g_WeaverVtable[i] = SR_UNMAPPED_SLOT(Weaver, 9);  break;
            case 10: g_WeaverVtable[i] = SR_UNMAPPED_SLOT(Weaver, 10); break;
            case 11: g_WeaverVtable[i] = SR_UNMAPPED_SLOT(Weaver, 11); break;
            case 12: g_WeaverVtable[i] = SR_UNMAPPED_SLOT(Weaver, 12); break;
            case 13: g_WeaverVtable[i] = SR_UNMAPPED_SLOT(Weaver, 13); break;
            case 14: g_WeaverVtable[i] = SR_UNMAPPED_SLOT(Weaver, 14); break;
            case 15: g_WeaverVtable[i] = SR_UNMAPPED_SLOT(Weaver, 15); break;
            case 16: g_WeaverVtable[i] = SR_UNMAPPED_SLOT(Weaver, 16); break;
            case 17: g_WeaverVtable[i] = SR_UNMAPPED_SLOT(Weaver, 17); break;
            case 18: g_WeaverVtable[i] = SR_UNMAPPED_SLOT(Weaver, 18); break;
            case 19: g_WeaverVtable[i] = SR_UNMAPPED_SLOT(Weaver, 19); break;
            case 20: g_WeaverVtable[i] = SR_UNMAPPED_SLOT(Weaver, 20); break;
            case 21: g_WeaverVtable[i] = SR_UNMAPPED_SLOT(Weaver, 21); break;
            case 22: g_WeaverVtable[i] = SR_UNMAPPED_SLOT(Weaver, 22); break;
            case 23: g_WeaverVtable[i] = SR_UNMAPPED_SLOT(Weaver, 23); break;
            case 24: g_WeaverVtable[i] = SR_UNMAPPED_SLOT(Weaver, 24); break;
            case 25: g_WeaverVtable[i] = SR_UNMAPPED_SLOT(Weaver, 25); break;
            case 26: g_WeaverVtable[i] = SR_UNMAPPED_SLOT(Weaver, 26); break;
            case 27: g_WeaverVtable[i] = SR_UNMAPPED_SLOT(Weaver, 27); break;
            case 28: g_WeaverVtable[i] = SR_UNMAPPED_SLOT(Weaver, 28); break;
            case 29: g_WeaverVtable[i] = SR_UNMAPPED_SLOT(Weaver, 29); break;
            case 30: g_WeaverVtable[i] = SR_UNMAPPED_SLOT(Weaver, 30); break;
            case 31: g_WeaverVtable[i] = SR_UNMAPPED_SLOT(Weaver, 31); break;
        }
    }
}

static Factory* AllocateFactory()
{
    Factory* f = (Factory*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(Factory));
    if (!f) return nullptr;
    f->vtable = g_FactoryVtable;
    return f;
}

// ----------------------------------------------------------------------------
// Exports
// ----------------------------------------------------------------------------

extern "C" __declspec(dllexport)
Factory* create_odyssey3d_factory_instance(void)
{
    SrLog("SR create_odyssey3d_factory_instance() called\n");
    // No whitelist. Universal use is the point -- we want Khazan/Stellar Blade/
    // Lies of P/Hell is Us/Mongil to work with the same drop-in. If a title
    // misbehaves specifically because the stand-in doesn't gate on exe name,
    // we add a wiz3D_Config.xml opt-in later.
    Factory* f = AllocateFactory();
    SrLog("SR Factory allocated at %p\n", (void*)f);
    return f;
}

extern "C" __declspec(dllexport)
void destroy_odyssey3d_factory_instance(Factory* f)
{
    SrLog("SR destroy_odyssey3d_factory_instance(f=%p)\n", (void*)f);
    if (f) HeapFree(GetProcessHeap(), 0, f);
}

extern "C" __declspec(dllexport)
const char* braindance3d_version(void)
{
    // Compatible version string for games that check it; identify ourselves
    // as wiz3D so a user grepping the DLL knows which build they have.
    return WIZ3D_SR_VERSION_STRING;
}

// wiz3D private export: lets the menu-squeeze module (loaded in the game
// via the dxgi.dll bootstrap) read our Device vtable call counters each
// frame. The menu-squeeze detection heuristic diffs these counts between
// Present boundaries -- CP2077 polls GetEyePosition only on stereo frames,
// IsAvailable once/frame on 2D (twice on stereo), bursts GetMonitorRect on
// user-toggled 3D changes. Returns -1 for an unknown slot.
//
// Slot indices match the game's Device vtable layout -- see InitDeviceVtable
// above. Keeping this one entry point so the menu-squeeze module doesn't
// have to export+resolve seven separate getters.
extern "C" __declspec(dllexport)
LONG64 wiz3d_sr_get_device_call_count(int slot)
{
    switch (slot) {
        case 1:  return (LONG64)g_cc_GetEyePosition.count;  // stereo-frame beat
        case 2:  return (LONG64)g_cc_GetResolution.count;
        case 3:  return (LONG64)g_cc_GetFloat2_18.count;
        case 6:  return (LONG64)g_cc_GetMonitorRect.count;  // user-toggle burst
        case 9:  return (LONG64)g_cc_Hint48.count;
        case 10: return (LONG64)g_cc_IsAvailable.count;     // 2D-frame beat
        case 12: return (LONG64)g_cc_Destroy.count;
        default: return -1;
    }
}

// ----------------------------------------------------------------------------
// DllMain
// ----------------------------------------------------------------------------

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
{
    (void)hModule; (void)lpReserved;
    switch (ul_reason_for_call)
    {
        case DLL_PROCESS_ATTACH:
            SrLogInit();
            SrInitEnv();
            SrMonitorInit();
            InitFactoryVtable();
            InitDeviceVtable();
            InitWeaverVtable();
            SrLog("================================================================\n");
            SrLog("wiz3D Odyssey3DSDK stand-in loaded (" WIZ3D_SR_VERSION_STRING ")\n");
            SrLog("Clean-room implementation of Samsung Odyssey 3D SDK interface.\n");
            SrLog("Spec reverse-engineered by vl3c (MIT) from public game call-sites.\n");
            SrLog("Vtable slots: Factory=%u Device=%u Weaver=%u (kVtableSlots=%u each)\n",
                  3u, 7u, 4u, kVtableSlots);
            SrLog("Config: IPD=%.1fmm eye_z=%.1fmm log_slots_every_call=%d\n",
                  g_cfg.ipd_mm, g_cfg.eye_z_mm, (int)g_logSlotsEveryCall);
            SrLog("================================================================\n");
            break;
        case DLL_PROCESS_DETACH:
            SrLog("wiz3D Odyssey3DSDK stand-in unloading\n");
            // Backstop: if the game didn't call Device::Destroy (abnormal
            // exit, crash) we still try to drop the LeiaSR connection so
            // SRService.exe sees a cleaner teardown than our process just
            // vanishing with the Weaver still live.
            SrShutdown("DLL_PROCESS_DETACH");
            if (g_logFile) { fclose(g_logFile); g_logFile = nullptr; }
            break;
    }
    return TRUE;
}
