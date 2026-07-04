/* NvDirectMode - d3d10.dll Direct Mode Proxy
 *
 * Wraps D3D10CreateDevice / D3D10CreateDeviceAndSwapChain. The "1" variants
 * (D3D10CreateDevice1 / D3D10CreateDeviceAndSwapChain1) are exported as
 * passthrough only — wrapping ID3D10Device1 is deferred (similar to how
 * d3d11 currently doesn't wrap Device1+).
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "../proxy_version.h"
#include "proxy_factory.h"
#include "swapchain_helpers.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <psapi.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "psapi.lib")

// ---------------------------------------------------------------------------
// Diagnostic log + 3DVision_Config.xml reader (mirrors the d3d11 pattern)
// ---------------------------------------------------------------------------
static FILE* g_logFile = NULL;
static int   g_loggingEnabled = 1;
static int   g_verboseEnabled = 1;   // default ON during pre-release
static int   g_swapEyes       = 0;
static int   g_wrapDevices    = 1;
static int   g_outputMode     = 1;   // DX10 default = SBS (mode 8/SR also implemented but unverified; opt in via XML)
static int   g_anaglyphColour  = 0;
static int   g_anaglyphMethod  = 0;

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
    g_loggingEnabled  = ReadConfigInt(buf, "LoggingEnabled",  g_loggingEnabled);
    g_verboseEnabled  = ReadConfigInt(buf, "VerboseLogging",  g_verboseEnabled);
    g_swapEyes        = ReadConfigInt(buf, "SwapEyes",        g_swapEyes);
    g_wrapDevices     = ReadConfigInt(buf, "WrapDevices",     g_wrapDevices);
    g_outputMode      = ReadConfigInt(buf, "OutputMode",      g_outputMode);
    g_anaglyphColour  = ReadConfigInt(buf, "AnaglyphColour",  g_anaglyphColour);
    g_anaglyphMethod  = ReadConfigInt(buf, "AnaglyphMethod",  g_anaglyphMethod);
    free(buf);
}

// ---------------------------------------------------------------------------
// VEH crash handler — same shape as the d3d9 / d3d11 proxies
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
                              (MINIDUMP_TYPE)(MiniDumpWithDataSegs | MiniDumpWithHandleData |
                                              MiniDumpWithThreadInfo | MiniDumpWithFullMemoryInfo),
                              &mei, NULL, NULL);
            CloseHandle(hFile);
        }
    }
    Log("=== CRASH END ===\n");
    if (g_logFile) fflush(g_logFile);
    return EXCEPTION_CONTINUE_SEARCH;
}

// ---------------------------------------------------------------------------
// Real d3d10.dll handles + function pointers (typed via void* to keep d3d10.h
// out of this TU)
// ---------------------------------------------------------------------------
static HMODULE g_hRealD3D10 = NULL;
static HMODULE g_hProxy     = NULL;

typedef HRESULT (WINAPI *pfnD3D10CreateDevice)(
    void* pAdapter, INT DriverType, HMODULE Software, UINT Flags, UINT SDKVersion, void** ppDevice);
typedef HRESULT (WINAPI *pfnD3D10CreateDevice1)(
    void* pAdapter, INT DriverType, HMODULE Software, UINT Flags, INT HardwareLevel, UINT SDKVersion, void** ppDevice);
typedef HRESULT (WINAPI *pfnD3D10CreateDeviceAndSwapChain)(
    void* pAdapter, INT DriverType, HMODULE Software, UINT Flags, UINT SDKVersion,
    void* pSwapChainDesc, void** ppSwapChain, void** ppDevice);
typedef HRESULT (WINAPI *pfnD3D10CreateDeviceAndSwapChain1)(
    void* pAdapter, INT DriverType, HMODULE Software, UINT Flags, INT HardwareLevel, UINT SDKVersion,
    void* pSwapChainDesc, void** ppSwapChain, void** ppDevice);

static pfnD3D10CreateDevice              g_pfnRealCreateDevice              = NULL;
static pfnD3D10CreateDevice1             g_pfnRealCreateDevice1             = NULL;
static pfnD3D10CreateDeviceAndSwapChain  g_pfnRealCreateDeviceAndSwapChain  = NULL;
static pfnD3D10CreateDeviceAndSwapChain1 g_pfnRealCreateDeviceAndSwapChain1 = NULL;

static BOOL LoadRealD3D10(void)
{
    if (g_hRealD3D10) return TRUE;
    WCHAR sysDir[MAX_PATH];
    GetSystemDirectoryW(sysDir, MAX_PATH);
    lstrcatW(sysDir, L"\\d3d10.dll");
    g_hRealD3D10 = LoadLibraryW(sysDir);
    if (!g_hRealD3D10)
    {
        Log("FAIL: Could not load real d3d10.dll from %ls (error %lu)\n", sysDir, GetLastError());
        return FALSE;
    }
    Log("OK: Real d3d10.dll loaded from %ls\n", sysDir);
    g_pfnRealCreateDevice              = (pfnD3D10CreateDevice)             GetProcAddress(g_hRealD3D10, "D3D10CreateDevice");
    g_pfnRealCreateDevice1             = (pfnD3D10CreateDevice1)            GetProcAddress(g_hRealD3D10, "D3D10CreateDevice1");
    g_pfnRealCreateDeviceAndSwapChain  = (pfnD3D10CreateDeviceAndSwapChain) GetProcAddress(g_hRealD3D10, "D3D10CreateDeviceAndSwapChain");
    g_pfnRealCreateDeviceAndSwapChain1 = (pfnD3D10CreateDeviceAndSwapChain1)GetProcAddress(g_hRealD3D10, "D3D10CreateDeviceAndSwapChain1");
    return TRUE;
}

// ---------------------------------------------------------------------------
// Exported: D3D10CreateDevice (wrapped)
// ---------------------------------------------------------------------------
extern "C" __declspec(dllexport) HRESULT WINAPI D3D10CreateDevice(
    void* pAdapter, INT DriverType, HMODULE Software, UINT Flags, UINT SDKVersion, void** ppDevice)
{
    Log("D3D10CreateDevice(DriverType=%d, Flags=0x%X) called\n", DriverType, Flags);
    if (!LoadRealD3D10() || !g_pfnRealCreateDevice) return E_FAIL;
    HRESULT hr = g_pfnRealCreateDevice(pAdapter, DriverType, Software, Flags, SDKVersion, ppDevice);
    Log("  real D3D10CreateDevice returned 0x%08lX\n", hr);
    if (SUCCEEDED(hr) && ppDevice && *ppDevice && g_wrapDevices)
    {
        NvDirectMode::WrapD3D10Device(ppDevice);
        Log("  wrapped device=%p\n", *ppDevice);
    }
    else if (!g_wrapDevices)
    {
        Log("  passthrough: WrapDevices=0; device=%p left unwrapped\n", (ppDevice ? *ppDevice : NULL));
    }
    return hr;
}

// ---------------------------------------------------------------------------
// Exported: D3D10CreateDevice1 (passthrough — Device1 wrapping deferred)
// ---------------------------------------------------------------------------
extern "C" __declspec(dllexport) HRESULT WINAPI D3D10CreateDevice1(
    void* pAdapter, INT DriverType, HMODULE Software, UINT Flags,
    INT HardwareLevel, UINT SDKVersion, void** ppDevice)
{
    Log("D3D10CreateDevice1(DriverType=%d, Flags=0x%X, HardwareLevel=%d) called -- passthrough\n",
        DriverType, Flags, HardwareLevel);
    if (!LoadRealD3D10() || !g_pfnRealCreateDevice1) return E_FAIL;
    return g_pfnRealCreateDevice1(pAdapter, DriverType, Software, Flags, HardwareLevel, SDKVersion, ppDevice);
}

// ---------------------------------------------------------------------------
// Exported: D3D10CreateDeviceAndSwapChain (wrapped + buffer-doubled)
// ---------------------------------------------------------------------------
extern "C" __declspec(dllexport) HRESULT WINAPI D3D10CreateDeviceAndSwapChain(
    void* pAdapter, INT DriverType, HMODULE Software, UINT Flags, UINT SDKVersion,
    void* pSwapChainDesc, void** ppSwapChain, void** ppDevice)
{
    Log("D3D10CreateDeviceAndSwapChain(DriverType=%d, Flags=0x%X) called\n", DriverType, Flags);
    if (!LoadRealD3D10() || !g_pfnRealCreateDeviceAndSwapChain) return E_FAIL;

    unsigned int logicalW = 0, logicalH = 0;
    void* pDescForReal = pSwapChainDesc;
    if (pSwapChainDesc)
        pDescForReal = const_cast<void*>(NvDirectMode::MakeDoubledSwapChainDesc(pSwapChainDesc, &logicalW, &logicalH));

    HRESULT hr = g_pfnRealCreateDeviceAndSwapChain(pAdapter, DriverType, Software, Flags, SDKVersion,
                                                   pDescForReal, ppSwapChain, ppDevice);
    const char* kind = (logicalW <= 16 || logicalH <= 16) ? " (likely probe)" : "";
    Log("  real D3D10CreateDeviceAndSwapChain returned 0x%08lX (logical=%ux%u)%s\n", hr, logicalW, logicalH, kind);
    if (SUCCEEDED(hr) && ppDevice && *ppDevice && g_wrapDevices)
    {
        NvDirectMode::WrapD3D10Device(ppDevice);
        if (logicalW > 0)
            NvDirectMode::SetWrappedDeviceLogicalSize(*ppDevice, logicalW, logicalH);
        if (ppSwapChain && *ppSwapChain)
            *ppSwapChain = NvDirectMode::WrapDXGISwapChain(*ppSwapChain, *ppDevice);
        Log("  wrapped (and-swap): device=%p sc=%p\n",
            *ppDevice, ppSwapChain ? *ppSwapChain : NULL);
    }
    else if (!g_wrapDevices)
    {
        Log("  passthrough (and-swap): WrapDevices=0; device=%p sc=%p left unwrapped\n",
            (ppDevice ? *ppDevice : NULL), (ppSwapChain ? *ppSwapChain : NULL));
    }
    return hr;
}

// ---------------------------------------------------------------------------
// Exported: D3D10CreateDeviceAndSwapChain1 (passthrough — Device1 deferred)
// ---------------------------------------------------------------------------
extern "C" __declspec(dllexport) HRESULT WINAPI D3D10CreateDeviceAndSwapChain1(
    void* pAdapter, INT DriverType, HMODULE Software, UINT Flags,
    INT HardwareLevel, UINT SDKVersion,
    void* pSwapChainDesc, void** ppSwapChain, void** ppDevice)
{
    Log("D3D10CreateDeviceAndSwapChain1(DriverType=%d, Flags=0x%X, HardwareLevel=%d) called -- passthrough\n",
        DriverType, Flags, HardwareLevel);
    if (!LoadRealD3D10() || !g_pfnRealCreateDeviceAndSwapChain1) return E_FAIL;
    return g_pfnRealCreateDeviceAndSwapChain1(pAdapter, DriverType, Software, Flags,
                                              HardwareLevel, SDKVersion,
                                              pSwapChainDesc, ppSwapChain, ppDevice);
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
            Log("=== NvDirectMode " DISPLAYED_VERSION " - d3d10 proxy loaded ===\n");
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
        Log("=== NvDirectMode d3d10 proxy unloading ===\n");
        if (g_hVEH) { RemoveVectoredExceptionHandler(g_hVEH); g_hVEH = NULL; }
        if (g_hRealD3D10) { FreeLibrary(g_hRealD3D10); g_hRealD3D10 = NULL; }
        if (g_logFile) { fclose(g_logFile); g_logFile = NULL; }
        break;
    }
    return TRUE;
}
