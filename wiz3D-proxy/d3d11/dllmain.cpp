/* wiz3D - d3d11.dll Proxy Loader
 *
 * Drop this d3d11.dll into a game's folder alongside S3DWrapperD3D10.dll
 * and the output plugins. Forwards every d3d11.dll export to the real
 * system d3d11.dll. Intercepts D3D11CreateDevice and
 * D3D11CreateDeviceAndSwapChain — Stage 1 just logs and passes through;
 * Stage 2 will route the swap-chain creation through the wrapper.
 *
 * The DX10 wrapper (S3DWrapperD3D10) handles both DX10 and DX11 in this
 * codebase — there's no separate S3DWrapperD3D11.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "../proxy_version.h"
#include <stdio.h>
#include <stdarg.h>
#include <intrin.h>   // _ReturnAddress

// ---------------------------------------------------------------------------
// Log to wiz3D_proxy.log (append) shared with the d3d10 / d3d9 / dxgi proxies.
// ---------------------------------------------------------------------------
static FILE* g_logFile = nullptr;

static void LogOpen(void)
{
    if (g_logFile) return;
    WCHAR dir[MAX_PATH];
    GetModuleFileNameW(NULL, dir, MAX_PATH);
    WCHAR* pSlash = wcsrchr(dir, L'\\');
    if (pSlash) *(pSlash + 1) = L'\0';
    lstrcatW(dir, L"wiz3D_Logs");
    CreateDirectoryW(dir, NULL);
    lstrcatW(dir, L"\\wiz3D_proxy.log");
    g_logFile = _wfopen(dir, L"a");
}

static void Log(const char* fmt, ...)
{
    if (!g_logFile) LogOpen();
    if (!g_logFile) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_logFile, fmt, ap);
    va_end(ap);
    fflush(g_logFile);
}

// ---------------------------------------------------------------------------
static HMODULE g_hRealD3D11 = nullptr;
static HMODULE g_hWrapper   = nullptr;
static HMODULE g_hProxy     = nullptr;

static BOOL LoadRealD3D11(void)
{
    if (g_hRealD3D11) return TRUE;
    WCHAR sysDir[MAX_PATH];
    GetSystemDirectoryW(sysDir, MAX_PATH);
    WCHAR path[MAX_PATH];
    wcscpy_s(path, sysDir);
    wcscat_s(path, L"\\d3d11.dll");
    g_hRealD3D11 = LoadLibraryW(path);
    if (!g_hRealD3D11)
    {
        Log("FAIL: real d3d11.dll load (err=%lu)\n", GetLastError());
        return FALSE;
    }
    Log("OK: Real d3d11.dll loaded from %ls\n", path);
    return TRUE;
}

static FARPROC GetReal(FARPROC* pCache, const char* name)
{
    if (*pCache) return *pCache;
    if (!LoadRealD3D11()) return nullptr;
    *pCache = GetProcAddress(g_hRealD3D11, name);
    return *pCache;
}

#define DECLARE_FWD(name) static FARPROC g_pfn_##name = nullptr

DECLARE_FWD(CreateDirect3D11DeviceFromDXGIDevice);
DECLARE_FWD(CreateDirect3D11SurfaceFromDXGISurface);
DECLARE_FWD(D3D11CoreCreateDevice);
DECLARE_FWD(D3D11CoreCreateLayeredDevice);
DECLARE_FWD(D3D11CoreGetLayeredDeviceSize);
DECLARE_FWD(D3D11CoreRegisterLayers);
DECLARE_FWD(D3D11CreateDevice);
DECLARE_FWD(D3D11CreateDeviceAndSwapChain);
DECLARE_FWD(D3D11CreateDeviceForD3D12);
DECLARE_FWD(D3D11On12CreateDevice);
DECLARE_FWD(D3DKMTCloseAdapter);
DECLARE_FWD(D3DKMTCreateAllocation);
DECLARE_FWD(D3DKMTCreateContext);
DECLARE_FWD(D3DKMTCreateDevice);
DECLARE_FWD(D3DKMTCreateSynchronizationObject);
DECLARE_FWD(D3DKMTDestroyAllocation);
DECLARE_FWD(D3DKMTDestroyContext);
DECLARE_FWD(D3DKMTDestroyDevice);
DECLARE_FWD(D3DKMTDestroySynchronizationObject);
DECLARE_FWD(D3DKMTEscape);
DECLARE_FWD(D3DKMTGetContextSchedulingPriority);
DECLARE_FWD(D3DKMTGetDeviceState);
DECLARE_FWD(D3DKMTGetDisplayModeList);
DECLARE_FWD(D3DKMTGetMultisampleMethodList);
DECLARE_FWD(D3DKMTGetRuntimeData);
DECLARE_FWD(D3DKMTGetSharedPrimaryHandle);
DECLARE_FWD(D3DKMTLock);
DECLARE_FWD(D3DKMTOpenAdapterFromHdc);
DECLARE_FWD(D3DKMTOpenResource);
DECLARE_FWD(D3DKMTPresent);
DECLARE_FWD(D3DKMTQueryAdapterInfo);
DECLARE_FWD(D3DKMTQueryAllocationResidency);
DECLARE_FWD(D3DKMTQueryResourceInfo);
DECLARE_FWD(D3DKMTRender);
DECLARE_FWD(D3DKMTSetAllocationPriority);
DECLARE_FWD(D3DKMTSetContextSchedulingPriority);
DECLARE_FWD(D3DKMTSetDisplayMode);
DECLARE_FWD(D3DKMTSetDisplayPrivateDriverFormat);
DECLARE_FWD(D3DKMTSetGammaRamp);
DECLARE_FWD(D3DKMTSetVidPnSourceOwner);
DECLARE_FWD(D3DKMTSignalSynchronizationObject);
DECLARE_FWD(D3DKMTUnlock);
DECLARE_FWD(D3DKMTWaitForSynchronizationObject);
DECLARE_FWD(D3DKMTWaitForVerticalBlankEvent);
DECLARE_FWD(D3DPerformance_BeginEvent);
DECLARE_FWD(D3DPerformance_EndEvent);
DECLARE_FWD(D3DPerformance_GetStatus);
DECLARE_FWD(D3DPerformance_SetMarker);
DECLARE_FWD(EnableFeatureLevelUpgrade);
DECLARE_FWD(OpenAdapter10);
DECLARE_FWD(OpenAdapter10_2);

// ---------------------------------------------------------------------------
// Special: D3D11CreateDevice / D3D11CreateDeviceAndSwapChain — wraps the
// returned device + immediate context in wiz3D's Option B Device11Proxy /
// Context11Proxy via the S3DWrapperD3D10-exported entry point. The proxy
// presents the same COM identity to the game but lives above the runtime,
// avoiding the COM-lifecycle invariants that the DDI-hook approach can't
// satisfy on Win11 D3D11.10. If the wrapper DLL isn't loadable or the
// export is missing, we pass through unwrapped (mono fallback).
// ---------------------------------------------------------------------------
typedef void (*PFN_wiz3D_WrapD3D11DeviceAndContext)(void**, void**);
typedef void (*PFN_wiz3D_WrapSwapChain)(void**, void*);
static PFN_wiz3D_WrapD3D11DeviceAndContext g_pfn_wiz3D_WrapD3D11    = nullptr;
static PFN_wiz3D_WrapSwapChain             g_pfn_wiz3D_WrapSwapChain = nullptr;
static bool                                g_pfn_wiz3D_WrapD3D11_resolved = false;

static void MaybeWrapDeviceAndContext(void** ppDevice, void** ppContext)
{
    if (!ppDevice || !*ppDevice) return;
    if (!g_pfn_wiz3D_WrapD3D11_resolved)
    {
        g_pfn_wiz3D_WrapD3D11_resolved = true;
        // Resolve from S3DWrapperD3D10.dll loaded in this process. It's loaded
        // earlier in the proxy startup path (LoadLibrary in DllMain). If the
        // wrapper isn't present, GetModuleHandle returns NULL — game proceeds
        // unwrapped (mono).
        HMODULE hWrap = GetModuleHandleW(L"S3DWrapperD3D10.dll");
        if (hWrap)
        {
            g_pfn_wiz3D_WrapD3D11 = (PFN_wiz3D_WrapD3D11DeviceAndContext)
                GetProcAddress(hWrap, "wiz3D_WrapD3D11DeviceAndContext");
            g_pfn_wiz3D_WrapSwapChain = (PFN_wiz3D_WrapSwapChain)
                GetProcAddress(hWrap, "wiz3D_WrapSwapChain");
            Log("  wiz3D Option B: wiz3D_WrapD3D11DeviceAndContext=%p wiz3D_WrapSwapChain=%p (hWrap=%p)\n",
                (void*)g_pfn_wiz3D_WrapD3D11, (void*)g_pfn_wiz3D_WrapSwapChain, (void*)hWrap);
        }
        else
        {
            Log("  wiz3D Option B: S3DWrapperD3D10.dll not loaded — passing through unwrapped\n");
        }
    }
    if (g_pfn_wiz3D_WrapD3D11)
        g_pfn_wiz3D_WrapD3D11(ppDevice, ppContext);
}

// Stage 4b.3: companion wrap for the swap chain returned by
// D3D11CreateDeviceAndSwapChain. Must be called AFTER MaybeWrapDeviceAndContext
// so *ppDevice is already the Device11Proxy we hand to wiz3D_WrapSwapChain as
// the parent device. No-op when the wrapper DLL isn't loaded.
static void MaybeWrapSwapChain(void** ppSwapChain, void* pWrappedDevice)
{
    if (!ppSwapChain || !*ppSwapChain || !pWrappedDevice) return;
    if (g_pfn_wiz3D_WrapSwapChain)
        g_pfn_wiz3D_WrapSwapChain(ppSwapChain, pWrappedDevice);
}

typedef HRESULT(WINAPI* PFN_D3D11CreateDevice)(
    void*, int, HMODULE, UINT, const void*, UINT, UINT,
    void**, void*, void**);
typedef HRESULT(WINAPI* PFN_D3D11CreateDeviceAndSwapChain)(
    void*, int, HMODULE, UINT, const void*, UINT, UINT,
    const void*, void**, void**, void*, void**);

// An overlay hooking the real D3D11CreateDevice may call this export again as its original; re-entered, the device comes
// from the real D3D11CreateDeviceAndSwapChain without a swap chain, unwrapped (the outer call wraps it).
static thread_local int t_createDeviceDepth = 0;

static void LogCaller(const char* what, void* ret, int depth)
{
    HMODULE m = nullptr;
    WCHAR path[MAX_PATH] = L"?";
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)ret, &m) && m)
        GetModuleFileNameW(m, path, MAX_PATH);
    Log("  %s re-entered (depth %d) from %ls+0x%llX\n", what, depth, path,
        (unsigned long long)((char*)ret - (char*)m));
}

extern "C" __declspec(dllexport) HRESULT WINAPI D3D11CreateDevice(
    void* pAdapter, int DriverType, HMODULE Software, UINT Flags,
    const void* pFeatureLevels, UINT FeatureLevels, UINT SDKVersion,
    void** ppDevice, void* pFeatureLevel, void** ppImmediateContext)
{
    Log("D3D11CreateDevice called (DriverType=%d, Flags=0x%X, SDKVersion=%u)\n",
        DriverType, Flags, SDKVersion);
    if (t_createDeviceDepth > 0)
    {
        LogCaller("D3D11CreateDevice", _ReturnAddress(), t_createDeviceDepth + 1);
        auto pc = (PFN_D3D11CreateDeviceAndSwapChain)GetReal(
            &g_pfn_D3D11CreateDeviceAndSwapChain, "D3D11CreateDeviceAndSwapChain");
        if (!pc) return E_FAIL;
        HRESULT inner = pc(pAdapter, DriverType, Software, Flags, pFeatureLevels, FeatureLevels, SDKVersion,
                           nullptr, nullptr, ppDevice, pFeatureLevel, ppImmediateContext);
        Log("  re-entered D3D11CreateDevice -> real D3D11CreateDeviceAndSwapChain (no swap chain) 0x%08lX\n", inner);
        return inner;
    }
    auto p = (PFN_D3D11CreateDevice)GetReal(&g_pfn_D3D11CreateDevice, "D3D11CreateDevice");
    if (!p) return E_FAIL;
    ++t_createDeviceDepth;
    HRESULT hr = p(pAdapter, DriverType, Software, Flags, pFeatureLevels,
                   FeatureLevels, SDKVersion, ppDevice, pFeatureLevel, ppImmediateContext);
    --t_createDeviceDepth;
    Log("  D3D11CreateDevice returned 0x%08lX, *ppDevice=%p\n",
        hr, ppDevice ? *ppDevice : nullptr);
    if (SUCCEEDED(hr)) MaybeWrapDeviceAndContext(ppDevice, ppImmediateContext);
    return hr;
}

extern "C" __declspec(dllexport) HRESULT WINAPI D3D11CreateDeviceAndSwapChain(
    void* pAdapter, int DriverType, HMODULE Software, UINT Flags,
    const void* pFeatureLevels, UINT FeatureLevels, UINT SDKVersion,
    const void* pSwapChainDesc, void** ppSwapChain, void** ppDevice,
    void* pFeatureLevel, void** ppImmediateContext)
{
    Log("D3D11CreateDeviceAndSwapChain called (DriverType=%d, Flags=0x%X, SDKVersion=%u)\n",
        DriverType, Flags, SDKVersion);
    auto p = (PFN_D3D11CreateDeviceAndSwapChain)GetReal(
        &g_pfn_D3D11CreateDeviceAndSwapChain, "D3D11CreateDeviceAndSwapChain");
    if (!p) return E_FAIL;
    HRESULT hr = p(pAdapter, DriverType, Software, Flags, pFeatureLevels,
                   FeatureLevels, SDKVersion, pSwapChainDesc, ppSwapChain,
                   ppDevice, pFeatureLevel, ppImmediateContext);
    Log("  D3D11CreateDeviceAndSwapChain returned 0x%08lX, *ppSwapChain=%p, *ppDevice=%p\n",
        hr, ppSwapChain ? *ppSwapChain : nullptr, ppDevice ? *ppDevice : nullptr);
    if (SUCCEEDED(hr))
    {
        MaybeWrapDeviceAndContext(ppDevice, ppImmediateContext);
        // Wrap the swap chain only for this entry path. Games using the
        // two-call sequence (D3D11CreateDevice + factory->CreateSwapChain)
        // bypass us for the SC creation — Stage 4b.3-follow-up (factory
        // hook) is needed for that and will land alongside the recording
        // infrastructure that depends on it.
        if (ppDevice && *ppDevice) MaybeWrapSwapChain(ppSwapChain, *ppDevice);
    }
    return hr;
}

// ---------------------------------------------------------------------------
// Plain pass-through forwarders. Macro generates a typedef + extern-C export
// that lazy-resolves and calls through with the same signature.
// ---------------------------------------------------------------------------
#define FWD_RET(rt, name, params, args)                                       \
    typedef rt(WINAPI* PFN_##name) params;                                    \
    extern "C" __declspec(dllexport) rt WINAPI name params {                  \
        auto p = (PFN_##name)GetReal(&g_pfn_##name, #name);                   \
        if (!p) return (rt)0;                                                 \
        return p args;                                                        \
    }

// WinRT/UWP factory exports (DX11.1+)
FWD_RET(HRESULT, CreateDirect3D11DeviceFromDXGIDevice,
    (void* dxgiDevice, void** graphicsDevice),
    (dxgiDevice, graphicsDevice))

FWD_RET(HRESULT, CreateDirect3D11SurfaceFromDXGISurface,
    (void* dgxiSurface, void** graphicsSurface),
    (dgxiSurface, graphicsSurface))

// Core/layered device APIs (used by the runtime)
FWD_RET(HRESULT, D3D11CoreCreateDevice,
    (void* pFactory, void* pAdapter, UINT Flags, const void* pFeatureLevels,
     UINT FeatureLevels, void** ppDevice),
    (pFactory, pAdapter, Flags, pFeatureLevels, FeatureLevels, ppDevice))

FWD_RET(HRESULT, D3D11CoreCreateLayeredDevice,
    (const void* a, DWORD b, const void* c, const IID* d, void** e),
    (a, b, c, d, e))

FWD_RET(SIZE_T, D3D11CoreGetLayeredDeviceSize,
    (const void* a, DWORD b), (a, b))

FWD_RET(HRESULT, D3D11CoreRegisterLayers,
    (const void* a, DWORD b), (a, b))

FWD_RET(HRESULT, D3D11CreateDeviceForD3D12,
    (void* a, void* b, UINT c, const void* d, UINT e, UINT f,
     const void* g, void** h, void** i),
    (a, b, c, d, e, f, g, h, i))

FWD_RET(HRESULT, D3D11On12CreateDevice,
    (void* pDevice, UINT Flags, const void* pFeatureLevels, UINT FeatureLevels,
     void* const* ppCommandQueues, UINT NumQueues, UINT NodeMask,
     void** ppDevice, void** ppImmediateContext, void* pChosenFeatureLevel),
    (pDevice, Flags, pFeatureLevels, FeatureLevels, ppCommandQueues,
     NumQueues, NodeMask, ppDevice, ppImmediateContext, pChosenFeatureLevel))

// D3DKMT* — kernel-mode thunks. Each takes a pointer to a D3DKMT_* struct
// and returns an NTSTATUS. Treating as opaque single-arg.
typedef LONG NTSTATUS_;
#define KMT_FWD(name)                                                         \
    typedef NTSTATUS_(WINAPI* PFN_##name)(void*);                             \
    extern "C" __declspec(dllexport) NTSTATUS_ WINAPI name(void* pData) {     \
        auto p = (PFN_##name)GetReal(&g_pfn_##name, #name);                   \
        return p ? p(pData) : (NTSTATUS_)0xC0000001L;                         \
    }

KMT_FWD(D3DKMTCloseAdapter)
KMT_FWD(D3DKMTCreateAllocation)
KMT_FWD(D3DKMTCreateContext)
KMT_FWD(D3DKMTCreateDevice)
KMT_FWD(D3DKMTCreateSynchronizationObject)
KMT_FWD(D3DKMTDestroyAllocation)
KMT_FWD(D3DKMTDestroyContext)
KMT_FWD(D3DKMTDestroyDevice)
KMT_FWD(D3DKMTDestroySynchronizationObject)
KMT_FWD(D3DKMTEscape)
KMT_FWD(D3DKMTGetContextSchedulingPriority)
KMT_FWD(D3DKMTGetDeviceState)
KMT_FWD(D3DKMTGetDisplayModeList)
KMT_FWD(D3DKMTGetMultisampleMethodList)
KMT_FWD(D3DKMTGetRuntimeData)
KMT_FWD(D3DKMTGetSharedPrimaryHandle)
KMT_FWD(D3DKMTLock)
KMT_FWD(D3DKMTOpenAdapterFromHdc)
KMT_FWD(D3DKMTOpenResource)
KMT_FWD(D3DKMTPresent)
KMT_FWD(D3DKMTQueryAdapterInfo)
KMT_FWD(D3DKMTQueryAllocationResidency)
KMT_FWD(D3DKMTQueryResourceInfo)
KMT_FWD(D3DKMTRender)
KMT_FWD(D3DKMTSetAllocationPriority)
KMT_FWD(D3DKMTSetContextSchedulingPriority)
KMT_FWD(D3DKMTSetDisplayMode)
KMT_FWD(D3DKMTSetDisplayPrivateDriverFormat)
KMT_FWD(D3DKMTSetGammaRamp)
KMT_FWD(D3DKMTSetVidPnSourceOwner)
KMT_FWD(D3DKMTSignalSynchronizationObject)
KMT_FWD(D3DKMTUnlock)
KMT_FWD(D3DKMTWaitForSynchronizationObject)
KMT_FWD(D3DKMTWaitForVerticalBlankEvent)

// D3DPerformance_* — debug markers for PIX/profilers
FWD_RET(int, D3DPerformance_BeginEvent,
    (DWORD Color, LPCWSTR Name), (Color, Name))

FWD_RET(int, D3DPerformance_EndEvent, (void), ())

FWD_RET(DWORD, D3DPerformance_GetStatus, (void), ())

// D3DPerformance_SetMarker returns void — written manually since the FWD_RET
// macro casts the missing function pointer to (rt)0 which doesn't work for void.
typedef void(WINAPI* PFN_D3DPerformance_SetMarker)(DWORD, LPCWSTR);
extern "C" __declspec(dllexport) void WINAPI D3DPerformance_SetMarker(
    DWORD Color, LPCWSTR Name)
{
    auto p = (PFN_D3DPerformance_SetMarker)GetReal(
        &g_pfn_D3DPerformance_SetMarker, "D3DPerformance_SetMarker");
    if (p) p(Color, Name);
}

FWD_RET(HRESULT, EnableFeatureLevelUpgrade,
    (void* a, UINT b, void* c), (a, b, c))

FWD_RET(HRESULT, OpenAdapter10,
    (void* pData), (pData))

FWD_RET(HRESULT, OpenAdapter10_2,
    (void* pData), (pData))

// ---------------------------------------------------------------------------
// DllMain
// ---------------------------------------------------------------------------
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        g_hProxy = hModule;
        DisableThreadLibraryCalls(hModule);
        LogOpen();
        {
            WCHAR exePath[MAX_PATH];
            GetModuleFileNameW(NULL, exePath, MAX_PATH);
            Log("\n=== wiz3D " DISPLAYED_VERSION " - d3d11 proxy loaded ===\n");
            Log("Game exe: %ls\n", exePath);
            WCHAR proxyPath[MAX_PATH];
            GetModuleFileNameW(hModule, proxyPath, MAX_PATH);
            Log("Proxy DLL: %ls\n", proxyPath);
        }
        break;

    case DLL_PROCESS_DETACH:
        Log("=== wiz3D d3d11 proxy unloading ===\n");
        if (g_hWrapper)   { FreeLibrary(g_hWrapper);   g_hWrapper   = nullptr; }
        if (g_hRealD3D11) { FreeLibrary(g_hRealD3D11); g_hRealD3D11 = nullptr; }
        if (g_logFile)    { fclose(g_logFile);         g_logFile    = nullptr; }
        break;
    }
    return TRUE;
}
