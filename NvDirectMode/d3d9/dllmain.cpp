/* NvDirectMode - d3d9.dll Direct Mode Proxy
 *
 * Drop this d3d9.dll into a game folder that uses NVIDIA 3D Vision Direct Mode
 * (e.g. Max Payne 3 -stereo 1, or any game that calls NvAPI_Stereo_SetActiveEye
 * and renders each eye explicitly). NvDirectMode forwards Direct3DCreate9 to
 * the real system DLL and (in later stages) intercepts swap-chain creation to
 * double the back-buffer width and route SetRenderTarget per the active eye.
 *
 * Stage 1 (this file): plain passthrough + crash-handling. The DLL loads,
 * the game runs unchanged, and we have a log to confirm the proxy is being
 * picked up. Buffer-doubling and eye routing land in stage 1b.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "../proxy_version.h"
#include "proxy_factory.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <psapi.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "psapi.lib")

// ---------------------------------------------------------------------------
// Diagnostic log + 3DVision_Config.xml reader (mirrors the d3d11 pattern).
// ---------------------------------------------------------------------------
static FILE* g_logFile = NULL;
static int   g_loggingEnabled = 1;
static int   g_verboseEnabled = 1;
static int   g_swapEyes       = 0;
static int   g_wrapDevices    = 1;
static int   g_outputMode     = 1;   // DX9 default = SBS (mode 8/SR also implemented but unverified; opt in via XML)
static int   g_useLayoutStable = 0;   // 0=off  1=IDirect3D9 vtable patch (task #61)  2=+IDirect3DDevice9 vtable patch (task #68)
static int   g_anaglyphColour  = 0;   // 0=RC (default), 1=GM, 2=AB
static int   g_anaglyphMethod  = 0;   // 0=Dubois (default), 1=Compromise, 2=Color, 3=HalfColor, 4=Optimised, 5=Grey, 6=True

static void LogOpen(void)
{
    if (g_logFile || !g_loggingEnabled) return;
    WCHAR dir[MAX_PATH];
    GetModuleFileNameW(NULL, dir, MAX_PATH);
    WCHAR* pSlash = wcsrchr(dir, L'\\');
    if (pSlash) *(pSlash + 1) = L'\0';
    lstrcatW(dir, L"nvdirectmode_proxy.log");
    g_logFile = _wfopen(dir, L"a");
}

static void Log(const char* fmt, ...)
{
    if (!g_loggingEnabled) return;
    if (!g_logFile) LogOpen();
    if (!g_logFile) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_logFile, fmt, ap);
    va_end(ap);
    fflush(g_logFile);
}

extern "C" void NvDM_Log(const char* fmt, ...)
{
    if (!g_loggingEnabled) return;
    if (!g_logFile) LogOpen();
    if (!g_logFile) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_logFile, fmt, ap);
    va_end(ap);
    fflush(g_logFile);
}
extern "C" int NvDM_VerboseEnabled() { return g_verboseEnabled; }
extern "C" int NvDM_SwapEyes()       { return g_swapEyes; }
extern "C" int NvDM_OutputMode()     { return g_outputMode; }
extern "C" int NvDM_OutputIsTopBottom() { return (g_outputMode == 0 || g_outputMode == 3) ? 1 : 0; }
extern "C" int NvDM_UseLayoutStableLevel() { return g_useLayoutStable; }
extern "C" int NvDM_AnaglyphColour() { return g_anaglyphColour; }
extern "C" int NvDM_AnaglyphMethod() { return g_anaglyphMethod; }

static int ReadConfigInt(const char* xml, const char* tag, int defaultValue)
{
    char needle[64];
    _snprintf_s(needle, sizeof(needle), _TRUNCATE, "<%s Value=\"", tag);
    const char* p = strstr(xml, needle);
    if (!p) return defaultValue;
    p += strlen(needle);
    return atoi(p);
}

static void LoadConfig(HMODULE hProxy)
{
    WCHAR cfgPath[MAX_PATH];
    GetModuleFileNameW(hProxy, cfgPath, MAX_PATH);
    WCHAR* pSlash = wcsrchr(cfgPath, L'\\');
    if (pSlash) *(pSlash + 1) = L'\0';
    lstrcatW(cfgPath, L"3DVision_Config.xml");
    FILE* f = _wfopen(cfgPath, L"rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 16 * 1024) { fclose(f); return; }
    char* buf = (char*)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return; }
    size_t n = fread(buf, 1, (size_t)sz, f);
    buf[n] = '\0';
    fclose(f);
    g_loggingEnabled = ReadConfigInt(buf, "LoggingEnabled", g_loggingEnabled);
    g_verboseEnabled = ReadConfigInt(buf, "VerboseLogging", g_verboseEnabled);
    g_swapEyes       = ReadConfigInt(buf, "SwapEyes",       g_swapEyes);
    g_wrapDevices     = ReadConfigInt(buf, "WrapDevices",         g_wrapDevices);
    g_useLayoutStable = ReadConfigInt(buf, "UseLayoutStableProxy", g_useLayoutStable);
    g_outputMode      = ReadConfigInt(buf, "OutputMode",      g_outputMode);
    g_anaglyphColour  = ReadConfigInt(buf, "AnaglyphColour",  g_anaglyphColour);
    g_anaglyphMethod  = ReadConfigInt(buf, "AnaglyphMethod",  g_anaglyphMethod);
    free(buf);
}

// ---------------------------------------------------------------------------
// Vectored exception handler — same filter logic as wiz3D-proxy/d3d9.
// PRIV/ILLEGAL excluded because third-party DLLs (VMware backdoor probes,
// CPUID feature checks) wrap them in __try/__except and a VEH log under
// loader-lock breaks SR runtime init.
// ---------------------------------------------------------------------------
static PVOID g_hVEH = NULL;
static volatile LONG g_crashLogged = 0;

static LONG CALLBACK VectoredCrashHandler(EXCEPTION_POINTERS* pExInfo)
{
    DWORD code = pExInfo->ExceptionRecord->ExceptionCode;

    switch (code)
    {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_STACK_OVERFLOW:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_IN_PAGE_ERROR:
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:
    case EXCEPTION_FLT_INVALID_OPERATION:
        break;
    default:
        return EXCEPTION_CONTINUE_SEARCH;
    }

    if (InterlockedCompareExchange(&g_crashLogged, 1, 0) != 0)
        return EXCEPTION_CONTINUE_SEARCH;

    Log("\n!!! FATAL EXCEPTION (VEH) !!!\n");
    Log("Exception code: 0x%08lX\n", code);
    void* crashAddr = pExInfo->ExceptionRecord->ExceptionAddress;
    Log("Crash address:  %p\n", crashAddr);

    HMODULE hMod = NULL;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)crashAddr, &hMod))
    {
        WCHAR modName[MAX_PATH];
        GetModuleFileNameW(hMod, modName, MAX_PATH);
        BYTE* base = (BYTE*)hMod;
        DWORD_PTR offset = (BYTE*)crashAddr - base;
        Log("Faulting module: %ls + 0x%IX\n", modName, offset);
    }
    else
    {
        Log("Faulting module: UNKNOWN\n");
    }

    // Minidump for post-mortem
    {
        WCHAR dumpPath[MAX_PATH];
        GetModuleFileNameW(NULL, dumpPath, MAX_PATH);
        WCHAR* pSlash = wcsrchr(dumpPath, L'\\');
        if (pSlash) *(pSlash + 1) = L'\0';
        lstrcatW(dumpPath, L"nvdirectmode_crash.dmp");

        HANDLE hFile = CreateFileW(dumpPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
        if (hFile != INVALID_HANDLE_VALUE)
        {
            MINIDUMP_EXCEPTION_INFORMATION mei;
            mei.ThreadId = GetCurrentThreadId();
            mei.ExceptionPointers = pExInfo;
            mei.ClientPointers = FALSE;
            MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(),
                              hFile,
                              (MINIDUMP_TYPE)(MiniDumpWithDataSegs |
                                              MiniDumpWithHandleData |
                                              MiniDumpWithThreadInfo |
                                              MiniDumpWithFullMemoryInfo),
                              &mei, NULL, NULL);
            CloseHandle(hFile);
            Log("Minidump written to: %ls\n", dumpPath);
        }
    }

    Log("=== CRASH END ===\n");
    if (g_logFile) fflush(g_logFile);
    return EXCEPTION_CONTINUE_SEARCH;
}

// ---------------------------------------------------------------------------
// Real d3d9.dll handles + function pointers
// ---------------------------------------------------------------------------
static HMODULE g_hRealD3D9 = NULL;
static HMODULE g_hProxy    = NULL;

typedef void*   (WINAPI *pfnDirect3DCreate9)(UINT SDKVersion);
typedef HRESULT (WINAPI *pfnDirect3DCreate9Ex)(UINT SDKVersion, void** ppD3D);

static pfnDirect3DCreate9   g_pfnRealCreate9   = NULL;
static pfnDirect3DCreate9Ex g_pfnRealCreate9Ex = NULL;

static FARPROC g_pfnD3DPERF_BeginEvent       = NULL;
static FARPROC g_pfnD3DPERF_EndEvent         = NULL;
static FARPROC g_pfnD3DPERF_GetStatus        = NULL;
static FARPROC g_pfnD3DPERF_QueryRepeatFrame = NULL;
static FARPROC g_pfnD3DPERF_SetMarker        = NULL;
static FARPROC g_pfnD3DPERF_SetOptions       = NULL;
static FARPROC g_pfnD3DPERF_SetRegion        = NULL;
static FARPROC g_pfnDebugSetLevel            = NULL;
static FARPROC g_pfnDebugSetMute             = NULL;

static BOOL LoadRealD3D9(void)
{
    if (g_hRealD3D9)
        return TRUE;

    WCHAR sysDir[MAX_PATH];
    GetSystemDirectoryW(sysDir, MAX_PATH);
    lstrcatW(sysDir, L"\\d3d9.dll");

    g_hRealD3D9 = LoadLibraryW(sysDir);
    if (!g_hRealD3D9)
    {
        Log("FAIL: Could not load real d3d9.dll from %ls (error %lu)\n", sysDir, GetLastError());
        return FALSE;
    }
    Log("OK: Real d3d9.dll loaded from %ls\n", sysDir);

    g_pfnRealCreate9   = (pfnDirect3DCreate9)  GetProcAddress(g_hRealD3D9, "Direct3DCreate9");
    g_pfnRealCreate9Ex = (pfnDirect3DCreate9Ex) GetProcAddress(g_hRealD3D9, "Direct3DCreate9Ex");

    g_pfnD3DPERF_BeginEvent       = GetProcAddress(g_hRealD3D9, "D3DPERF_BeginEvent");
    g_pfnD3DPERF_EndEvent         = GetProcAddress(g_hRealD3D9, "D3DPERF_EndEvent");
    g_pfnD3DPERF_GetStatus        = GetProcAddress(g_hRealD3D9, "D3DPERF_GetStatus");
    g_pfnD3DPERF_QueryRepeatFrame = GetProcAddress(g_hRealD3D9, "D3DPERF_QueryRepeatFrame");
    g_pfnD3DPERF_SetMarker        = GetProcAddress(g_hRealD3D9, "D3DPERF_SetMarker");
    g_pfnD3DPERF_SetOptions       = GetProcAddress(g_hRealD3D9, "D3DPERF_SetOptions");
    g_pfnD3DPERF_SetRegion        = GetProcAddress(g_hRealD3D9, "D3DPERF_SetRegion");
    g_pfnDebugSetLevel            = GetProcAddress(g_hRealD3D9, "DebugSetLevel");
    g_pfnDebugSetMute             = GetProcAddress(g_hRealD3D9, "DebugSetMute");

    return TRUE;
}

// ---------------------------------------------------------------------------
// Exported: Direct3DCreate9 - stage 1 passthrough.
// Stage 1b will wrap the returned IDirect3D9 with our own object that
// intercepts CreateDevice() so we can install per-eye RT routing.
// ---------------------------------------------------------------------------
extern "C" __declspec(dllexport) void* WINAPI Direct3DCreate9(UINT SDKVersion)
{
    Log("Direct3DCreate9(SDKVersion=%u) called\n", SDKVersion);
    if (!LoadRealD3D9() || !g_pfnRealCreate9) return NULL;
    void* real = g_pfnRealCreate9(SDKVersion);
    if (!real) { Log("  real Direct3DCreate9 returned NULL\n"); return NULL; }
    if (!g_wrapDevices)
    {
        Log("  passthrough: WrapDevices=0; IDirect3D9=%p left unwrapped\n", real);
        return real;
    }
    if (g_useLayoutStable)
    {
        // Task #61: hot-patch the real vtable's CreateDevice slot and
        // hand the real IDirect3D9 back to the game. Game's anti-tamper
        // sniff sees the real layout (passes); when game later calls
        // CreateDevice via the vtable, our hook fires and wraps the
        // returned device with the existing Device9Proxy.
        if (NvDirectMode::InstallVtablePatchForD3D9(real, /*isEx=*/false))
        {
            Log("  layout-stable: patched IDirect3D9 vtable, returning real %p\n", real);
            return real;
        }
        Log("  layout-stable: vtable patch FAILED — falling back to D3D9Proxy wrap\n");
    }
    void* proxy = NvDirectMode::CreateD3D9Proxy(real);
    Log("  wrapped IDirect3D9 %p -> proxy %p\n", real, proxy);
    return proxy;
}

extern "C" __declspec(dllexport) HRESULT WINAPI Direct3DCreate9Ex(UINT SDKVersion, void** ppD3D)
{
    Log("Direct3DCreate9Ex(SDKVersion=%u) called\n", SDKVersion);
    if (!ppD3D) return E_FAIL;
    if (!LoadRealD3D9() || !g_pfnRealCreate9Ex) return E_FAIL;
    void* real = nullptr;
    HRESULT hr = g_pfnRealCreate9Ex(SDKVersion, &real);
    if (FAILED(hr) || !real) { *ppD3D = nullptr; return hr; }
    if (!g_wrapDevices)
    {
        *ppD3D = real;
        Log("  passthrough: WrapDevices=0; IDirect3D9Ex=%p left unwrapped\n", real);
        return hr;
    }
    if (g_useLayoutStable)
    {
        if (NvDirectMode::InstallVtablePatchForD3D9(real, /*isEx=*/true))
        {
            *ppD3D = real;
            Log("  layout-stable: patched IDirect3D9Ex vtable, returning real %p\n", real);
            return hr;
        }
        Log("  layout-stable: vtable patch FAILED — falling back to D3D9ExProxy wrap\n");
    }
    *ppD3D = NvDirectMode::CreateD3D9ExProxy(real);
    Log("  wrapped IDirect3D9Ex %p -> proxy %p\n", real, *ppD3D);
    return hr;
}

// ---------------------------------------------------------------------------
// Forwarded D3DPERF / Debug exports
// ---------------------------------------------------------------------------
extern "C" __declspec(dllexport) int WINAPI D3DPERF_BeginEvent(DWORD col, LPCWSTR name)
{
    if (!LoadRealD3D9() || !g_pfnD3DPERF_BeginEvent) return 0;
    typedef int (WINAPI *PFN)(DWORD, LPCWSTR);
    return ((PFN)g_pfnD3DPERF_BeginEvent)(col, name);
}

extern "C" __declspec(dllexport) int WINAPI D3DPERF_EndEvent(void)
{
    if (!LoadRealD3D9() || !g_pfnD3DPERF_EndEvent) return 0;
    typedef int (WINAPI *PFN)(void);
    return ((PFN)g_pfnD3DPERF_EndEvent)();
}

extern "C" __declspec(dllexport) DWORD WINAPI D3DPERF_GetStatus(void)
{
    if (!LoadRealD3D9() || !g_pfnD3DPERF_GetStatus) return 0;
    typedef DWORD (WINAPI *PFN)(void);
    return ((PFN)g_pfnD3DPERF_GetStatus)();
}

extern "C" __declspec(dllexport) BOOL WINAPI D3DPERF_QueryRepeatFrame(void)
{
    if (!LoadRealD3D9() || !g_pfnD3DPERF_QueryRepeatFrame) return FALSE;
    typedef BOOL (WINAPI *PFN)(void);
    return ((PFN)g_pfnD3DPERF_QueryRepeatFrame)();
}

extern "C" __declspec(dllexport) void WINAPI D3DPERF_SetMarker(DWORD col, LPCWSTR name)
{
    if (!LoadRealD3D9() || !g_pfnD3DPERF_SetMarker) return;
    typedef void (WINAPI *PFN)(DWORD, LPCWSTR);
    ((PFN)g_pfnD3DPERF_SetMarker)(col, name);
}

extern "C" __declspec(dllexport) void WINAPI D3DPERF_SetOptions(DWORD dwOptions)
{
    if (!LoadRealD3D9() || !g_pfnD3DPERF_SetOptions) return;
    typedef void (WINAPI *PFN)(DWORD);
    ((PFN)g_pfnD3DPERF_SetOptions)(dwOptions);
}

extern "C" __declspec(dllexport) void WINAPI D3DPERF_SetRegion(DWORD col, LPCWSTR name)
{
    if (!LoadRealD3D9() || !g_pfnD3DPERF_SetRegion) return;
    typedef void (WINAPI *PFN)(DWORD, LPCWSTR);
    ((PFN)g_pfnD3DPERF_SetRegion)(col, name);
}

extern "C" __declspec(dllexport) void WINAPI DebugSetLevel(DWORD dw)
{
    if (!LoadRealD3D9() || !g_pfnDebugSetLevel) return;
    typedef void (WINAPI *PFN)(DWORD);
    ((PFN)g_pfnDebugSetLevel)(dw);
}

extern "C" __declspec(dllexport) void WINAPI DebugSetMute(void)
{
    if (!LoadRealD3D9() || !g_pfnDebugSetMute) return;
    typedef void (WINAPI *PFN)(void);
    ((PFN)g_pfnDebugSetMute)();
}

// ---------------------------------------------------------------------------
// DllMain
// ---------------------------------------------------------------------------
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        g_hProxy = hModule;
        DisableThreadLibraryCalls(hModule);
        LoadConfig(hModule);
        LogOpen();
        g_hVEH = AddVectoredExceptionHandler(1, VectoredCrashHandler);
        {
            WCHAR exePath[MAX_PATH];
            GetModuleFileNameW(NULL, exePath, MAX_PATH);
            Log("=== NvDirectMode " DISPLAYED_VERSION " - d3d9 proxy loaded ===\n");
            Log("Game exe: %ls\n", exePath);
            WCHAR proxyPath[MAX_PATH];
            GetModuleFileNameW(hModule, proxyPath, MAX_PATH);
            Log("Proxy DLL: %ls\n", proxyPath);
            Log("Config:    OutputMode=%d (%s)  WrapDevices=%d  SwapEyes=%d  UseLayoutStableProxy=%d  LoggingEnabled=%d  VerboseLogging=%d\n",
                g_outputMode, NvDM_OutputIsTopBottom() ? "Top-and-Bottom" : "Side-by-Side",
                g_wrapDevices, g_swapEyes, g_useLayoutStable, g_loggingEnabled, g_verboseEnabled);
        }
        break;

    case DLL_PROCESS_DETACH:
        Log("=== NvDirectMode d3d9 proxy unloading ===\n");
        if (g_hVEH)
        {
            RemoveVectoredExceptionHandler(g_hVEH);
            g_hVEH = NULL;
        }
        if (g_hRealD3D9)
        {
            FreeLibrary(g_hRealD3D9);
            g_hRealD3D9 = NULL;
        }
        if (g_logFile)
        {
            fclose(g_logFile);
            g_logFile = NULL;
        }
        break;
    }
    return TRUE;
}
