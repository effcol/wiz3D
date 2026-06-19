/* NvDirectMode - dxgi.dll Direct Mode Proxy
 *
 * Companion to NvDirectMode/d3d11/d3d11.dll. Drop both into the game folder
 * of a DX11 Direct Mode game (Tutorial07, Tomb Raider 2013, etc) alongside
 * nvapi[64].dll and 3DVision_Config.xml. The dxgi proxy intercepts the
 * DXGI factory creation path (CreateDXGIFactory* + IDXGIFactory::CreateSwapChain)
 * which the d3d11 proxy alone misses — many production DX11 games create
 * their swap chain via CreateDXGIFactory rather than D3D11CreateDeviceAndSwapChain.
 *
 * Stage 1 (this file): passthrough skeleton. Loads real system dxgi.dll,
 * forwards CreateDXGIFactory / 1 / 2 + a couple of minor exports unchanged.
 * Wrapping (IDXGIFactory + IDXGISwapChain on the factory path) lands in
 * stage 2.
 *
 * Re-entrancy: when our d3d11 proxy's D3D11CreateDeviceAndSwapChain calls
 * the real CreateDeviceAndSwapChain, real internally calls CreateDXGIFactory
 * via dxgi.dll's exports. With our dxgi proxy in place that resolves to us.
 * We just forward — stage 2 will need re-entry guarding so we don't double-
 * mutate the swap-chain desc.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "../proxy_version.h"

// Bridge into DXGIFactoryProxy.cpp via plain void*/IID* — DXGIFactoryProxy.h
// can't be included here because it pulls dxgi.h which redeclares
// CreateDXGIFactory* without our __declspec(dllexport) linkage.
extern "C" void* NvDM_DXGI_WrapFactory(void* realFactoryUnknown, const IID* riidPtr);
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <psapi.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "dxguid.lib")

// ---------------------------------------------------------------------------
// Diagnostic log + 3DVision_Config.xml reader (mirrors d3d11)
// ---------------------------------------------------------------------------
static FILE* g_logFile = NULL;
static int   g_loggingEnabled = 1;
static int   g_verboseEnabled = 1;
static int   g_swapEyes       = 0;
static int   g_wrapDevices    = 1;
// Fallback used only when 3DVision_Config.xml is absent (shipped configs set
// OutputMode explicitly). d3d11/dxgi default to 8 (SR weave) by design — these
// are the SR-targeted backends. d3d9/d3d10/opengl32 deliberately default to 1
// (SBS) instead because SR weave is unverified there.
static int   g_outputMode     = 8;

static void LogOpen(void)
{
    if (g_logFile || !g_loggingEnabled) return;
    WCHAR dir[MAX_PATH];
    GetModuleFileNameW(NULL, dir, MAX_PATH);
    WCHAR* pSlash = wcsrchr(dir, L'\\');
    if (pSlash) *(pSlash + 1) = L'\0';
    // Shared with the d3d11 proxy log — append mode so they don't clobber.
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

// DXGIFactoryProxy.cpp lives in this DLL but in a different TU; expose a
// log entry point with C linkage so it can append to the same log file.
extern "C" void NvDM_DxgiLog(const char* fmt, ...)
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
    g_wrapDevices    = ReadConfigInt(buf, "WrapDevices",    g_wrapDevices);
    g_outputMode     = ReadConfigInt(buf, "OutputMode",     g_outputMode);
    free(buf);
}

// ---------------------------------------------------------------------------
// VEH crash handler (same shape as d3d11)
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

    Log("\n!!! FATAL EXCEPTION (VEH, dxgi proxy) !!!\n");
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
            MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hFile,
                (MINIDUMP_TYPE)(MiniDumpWithDataSegs | MiniDumpWithHandleData |
                                MiniDumpWithThreadInfo | MiniDumpWithFullMemoryInfo),
                &mei, NULL, NULL);
            CloseHandle(hFile);
        }
    }
    Log("=== CRASH END (dxgi) ===\n");
    return EXCEPTION_CONTINUE_SEARCH;
}

// ---------------------------------------------------------------------------
// Real dxgi.dll handles + function pointers (typed via void* to keep dxgi.h
// out of this TU — same export-linkage clash rationale as d3d11 proxy)
// ---------------------------------------------------------------------------
static HMODULE g_hRealDXGI = NULL;
static HMODULE g_hProxy    = NULL;

typedef HRESULT (WINAPI *pfnCreateDXGIFactory) (REFIID riid, void** ppFactory);
typedef HRESULT (WINAPI *pfnCreateDXGIFactory1)(REFIID riid, void** ppFactory);
typedef HRESULT (WINAPI *pfnCreateDXGIFactory2)(UINT Flags, REFIID riid, void** ppFactory);
typedef HRESULT (WINAPI *pfnDXGIDeclareAdapterRemovalSupport)(void);
typedef HRESULT (WINAPI *pfnDXGIGetDebugInterface1)(UINT Flags, REFIID riid, void** pDebug);

static pfnCreateDXGIFactory                g_pfnRealCreateDXGIFactory                = NULL;
static pfnCreateDXGIFactory1               g_pfnRealCreateDXGIFactory1               = NULL;
static pfnCreateDXGIFactory2               g_pfnRealCreateDXGIFactory2               = NULL;
static pfnDXGIDeclareAdapterRemovalSupport g_pfnRealDXGIDeclareAdapterRemovalSupport = NULL;
static pfnDXGIGetDebugInterface1           g_pfnRealDXGIGetDebugInterface1           = NULL;

static BOOL LoadRealDXGI(void)
{
    if (g_hRealDXGI) return TRUE;
    WCHAR sysDir[MAX_PATH];
    GetSystemDirectoryW(sysDir, MAX_PATH);
    lstrcatW(sysDir, L"\\dxgi.dll");
    g_hRealDXGI = LoadLibraryW(sysDir);
    if (!g_hRealDXGI)
    {
        Log("FAIL: Could not load real dxgi.dll from %ls (error %lu)\n", sysDir, GetLastError());
        return FALSE;
    }
    Log("OK: Real dxgi.dll loaded from %ls\n", sysDir);
    g_pfnRealCreateDXGIFactory                = (pfnCreateDXGIFactory)               GetProcAddress(g_hRealDXGI, "CreateDXGIFactory");
    g_pfnRealCreateDXGIFactory1               = (pfnCreateDXGIFactory1)              GetProcAddress(g_hRealDXGI, "CreateDXGIFactory1");
    g_pfnRealCreateDXGIFactory2               = (pfnCreateDXGIFactory2)              GetProcAddress(g_hRealDXGI, "CreateDXGIFactory2");
    g_pfnRealDXGIDeclareAdapterRemovalSupport = (pfnDXGIDeclareAdapterRemovalSupport)GetProcAddress(g_hRealDXGI, "DXGIDeclareAdapterRemovalSupport");
    g_pfnRealDXGIGetDebugInterface1           = (pfnDXGIGetDebugInterface1)          GetProcAddress(g_hRealDXGI, "DXGIGetDebugInterface1");
    return TRUE;
}

// ---------------------------------------------------------------------------
// Exported: CreateDXGIFactory (stage 1 passthrough; stage 2 wraps the factory)
// ---------------------------------------------------------------------------
// Wrap the just-returned real factory in our DXGIFactoryProxy. Best-effort:
// if anything fails we leave *ppFactory pointing at the unwrapped real
// (passthrough — game still works, but factory CreateSwapChain* paths are
// missed).
static void WrapFactoryReturn(REFIID riid, void** ppFactory, HRESULT hr)
{
    if (FAILED(hr) || !ppFactory || !*ppFactory) return;
    void* realFactory = *ppFactory;
    void* wrapped = NvDM_DXGI_WrapFactory(realFactory, &riid);
    if (wrapped)
    {
        *ppFactory = wrapped;
        Log("  factory wrapped: %p -> %p (riid match)\n", realFactory, wrapped);
    }
    else
    {
        Log("  factory NOT wrapped (passthrough): real=%p\n", realFactory);
    }
}

extern "C" __declspec(dllexport) HRESULT WINAPI CreateDXGIFactory(REFIID riid, void** ppFactory)
{
    Log("CreateDXGIFactory called\n");
    if (!LoadRealDXGI() || !g_pfnRealCreateDXGIFactory) return E_FAIL;
    HRESULT hr = g_pfnRealCreateDXGIFactory(riid, ppFactory);
    Log("  real CreateDXGIFactory returned 0x%08lX  factory=%p\n", hr, ppFactory ? *ppFactory : NULL);
    WrapFactoryReturn(riid, ppFactory, hr);
    return hr;
}

extern "C" __declspec(dllexport) HRESULT WINAPI CreateDXGIFactory1(REFIID riid, void** ppFactory)
{
    Log("CreateDXGIFactory1 called\n");
    if (!LoadRealDXGI() || !g_pfnRealCreateDXGIFactory1) return E_FAIL;
    HRESULT hr = g_pfnRealCreateDXGIFactory1(riid, ppFactory);
    Log("  real CreateDXGIFactory1 returned 0x%08lX  factory=%p\n", hr, ppFactory ? *ppFactory : NULL);
    WrapFactoryReturn(riid, ppFactory, hr);
    return hr;
}

extern "C" __declspec(dllexport) HRESULT WINAPI CreateDXGIFactory2(UINT Flags, REFIID riid, void** ppFactory)
{
    Log("CreateDXGIFactory2(Flags=0x%X) called\n", Flags);
    if (!LoadRealDXGI() || !g_pfnRealCreateDXGIFactory2) return E_FAIL;
    HRESULT hr = g_pfnRealCreateDXGIFactory2(Flags, riid, ppFactory);
    Log("  real CreateDXGIFactory2 returned 0x%08lX  factory=%p\n", hr, ppFactory ? *ppFactory : NULL);
    WrapFactoryReturn(riid, ppFactory, hr);
    return hr;
}

extern "C" __declspec(dllexport) HRESULT WINAPI DXGIDeclareAdapterRemovalSupport(void)
{
    if (!LoadRealDXGI() || !g_pfnRealDXGIDeclareAdapterRemovalSupport) return E_FAIL;
    return g_pfnRealDXGIDeclareAdapterRemovalSupport();
}

extern "C" __declspec(dllexport) HRESULT WINAPI DXGIGetDebugInterface1(UINT Flags, REFIID riid, void** pDebug)
{
    if (!LoadRealDXGI() || !g_pfnRealDXGIGetDebugInterface1) return E_FAIL;
    return g_pfnRealDXGIGetDebugInterface1(Flags, riid, pDebug);
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
            Log("=== NvDirectMode " DISPLAYED_VERSION " - dxgi proxy loaded (stage 2: factory wrap) ===\n");
            Log("Game exe: %ls\n", exePath);
            WCHAR proxyPath[MAX_PATH];
            GetModuleFileNameW(hModule, proxyPath, MAX_PATH);
            Log("Proxy DLL: %ls\n", proxyPath);
            Log("Config:    OutputMode=%d (%s)  WrapDevices=%d  SwapEyes=%d  LoggingEnabled=%d  VerboseLogging=%d\n",
                g_outputMode, NvDM_OutputIsTopBottom() ? "Top-and-Bottom" : "Side-by-Side",
                g_wrapDevices, g_swapEyes, g_loggingEnabled, g_verboseEnabled);
        }
        break;

    case DLL_PROCESS_DETACH:
        Log("=== NvDirectMode dxgi proxy unloading ===\n");
        if (g_hVEH) { RemoveVectoredExceptionHandler(g_hVEH); g_hVEH = NULL; }
        if (g_hRealDXGI) { FreeLibrary(g_hRealDXGI); g_hRealDXGI = NULL; }
        if (g_logFile) { fclose(g_logFile); g_logFile = NULL; }
        break;
    }
    return TRUE;
}
