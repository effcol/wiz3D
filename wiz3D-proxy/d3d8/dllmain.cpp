/* wiz3D - d3d8.dll Proxy Loader
 *
 * Drop this d3d8.dll into a game's folder alongside S3DWrapperD3D8.dll
 * and the support DLLs. The proxy loads the iZ3D DX8-to-DX9 translation
 * wrapper (S3DWrapperD3D8.dll) and routes Direct3DCreate8 through it.
 *
 * The wrapper internally calls Direct3DCreate9 to create a real D3D9
 * device and presents D3D8-compatible interfaces to the game.
 *
 * Pass-through exports (ValidateVertexShader, ValidatePixelShader,
 * DebugSetMute) are forwarded to the real system d3d8.dll.
 */

#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include "../proxy_version.h"
#include <stdio.h>
#include <psapi.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "psapi.lib")

// ---------------------------------------------------------------------------
// Simple diagnostic log — writes to wiz3D_d3d8_proxy.log in the proxy dir
// ---------------------------------------------------------------------------
static FILE* g_logFile = NULL;

static void LogOpen(void)
{
    if (g_logFile) return;
    WCHAR dir[MAX_PATH];
    GetModuleFileNameW(NULL, dir, MAX_PATH);  // game exe path
    WCHAR* pSlash = wcsrchr(dir, L'\\');
    if (pSlash) *(pSlash + 1) = L'\0';
    lstrcatW(dir, L"wiz3D_d3d8_proxy.log");
    g_logFile = _wfopen(dir, L"a");  // append: shared with other proxies
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
// Vectored exception handler — logs fatal exceptions for crash diagnostics
// ---------------------------------------------------------------------------
static PVOID g_hVEH = NULL;
static volatile LONG g_crashLogged = 0;

static LONG CALLBACK VectoredCrashHandler(EXCEPTION_POINTERS* pExInfo)
{
    DWORD code = pExInfo->ExceptionRecord->ExceptionCode;

    switch (code)
    {
    // PRIV_INSTRUCTION / ILLEGAL_INSTRUCTION excluded — see d3d9 dllmain for rationale
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

    if (code == EXCEPTION_ACCESS_VIOLATION &&
        pExInfo->ExceptionRecord->NumberParameters >= 2)
    {
        ULONG_PTR accessType = pExInfo->ExceptionRecord->ExceptionInformation[0];
        const char* op = "UNKNOWN";
        if (accessType == 0) op = "READ";
        else if (accessType == 1) op = "WRITE";
        else if (accessType == 8) op = "DEP (execute non-executable memory)";
        Log("Access violation: %s of address %p\n", op,
            (void*)pExInfo->ExceptionRecord->ExceptionInformation[1]);
    }

    CONTEXT* ctx = pExInfo->ContextRecord;
#ifndef _WIN64
    Log("EAX=%08lX EBX=%08lX ECX=%08lX EDX=%08lX\n", ctx->Eax, ctx->Ebx, ctx->Ecx, ctx->Edx);
    Log("ESI=%08lX EDI=%08lX EBP=%08lX ESP=%08lX\n", ctx->Esi, ctx->Edi, ctx->Ebp, ctx->Esp);
    Log("EIP=%08lX\n", ctx->Eip);
#else
    Log("RAX=%016llX RBX=%016llX RCX=%016llX RDX=%016llX\n", ctx->Rax, ctx->Rbx, ctx->Rcx, ctx->Rdx);
    Log("RSI=%016llX RDI=%016llX RBP=%016llX RSP=%016llX\n", ctx->Rsi, ctx->Rdi, ctx->Rbp, ctx->Rsp);
    Log("R8 =%016llX R9 =%016llX R10=%016llX R11=%016llX\n", ctx->R8, ctx->R9, ctx->R10, ctx->R11);
    Log("R12=%016llX R13=%016llX R14=%016llX R15=%016llX\n", ctx->R12, ctx->R13, ctx->R14, ctx->R15);
    Log("RIP=%016llX\n", ctx->Rip);
#endif

    Log("--- Stack trace ---\n");
    {
        void* stack[64];
        USHORT frames = CaptureStackBackTrace(0, 64, stack, NULL);
        for (USHORT i = 0; i < frames; i++)
        {
            HMODULE hFrameMod = NULL;
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   (LPCSTR)stack[i], &hFrameMod))
            {
                WCHAR modName[MAX_PATH];
                GetModuleFileNameW(hFrameMod, modName, MAX_PATH);
                WCHAR* pSlash = wcsrchr(modName, L'\\');
                DWORD_PTR offset = (BYTE*)stack[i] - (BYTE*)hFrameMod;
                Log("  [%2u] %p  %ls+0x%IX\n", i, stack[i],
                    pSlash ? pSlash + 1 : modName, offset);
            }
            else
            {
                Log("  [%2u] %p  (unknown)\n", i, stack[i]);
            }
        }
    }
    Log("--- End stack trace ---\n");

    Log("--- Loaded modules ---\n");
    HMODULE hMods[256];
    DWORD cbNeeded;
    if (EnumProcessModules(GetCurrentProcess(), hMods, sizeof(hMods), &cbNeeded))
    {
        DWORD count = cbNeeded / sizeof(HMODULE);
        for (DWORD i = 0; i < count && i < 256; i++)
        {
            WCHAR name[MAX_PATH];
            MODULEINFO mi;
            if (GetModuleFileNameW(hMods[i], name, MAX_PATH) &&
                GetModuleInformation(GetCurrentProcess(), hMods[i], &mi, sizeof(mi)))
            {
                Log("  %p-%p  %ls\n", mi.lpBaseOfDll,
                    (BYTE*)mi.lpBaseOfDll + mi.SizeOfImage, name);
            }
        }
    }
    Log("--- End modules ---\n");

    {
        WCHAR dumpPath[MAX_PATH];
        GetModuleFileNameW(NULL, dumpPath, MAX_PATH);
        WCHAR* pSlash = wcsrchr(dumpPath, L'\\');
        if (pSlash) *(pSlash + 1) = L'\0';
        lstrcatW(dumpPath, L"wiz3D_d3d8_crash.dmp");

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
// Handles
// ---------------------------------------------------------------------------
static HMODULE g_hRealD3D8 = NULL;   // real system d3d8.dll
static HMODULE g_hWrapper  = NULL;   // S3DWrapperD3D8.dll
static HMODULE g_hProxy    = NULL;   // our own HMODULE
static BOOL    g_bWrapperActive = FALSE;

// ---------------------------------------------------------------------------
// Typedefs
// ---------------------------------------------------------------------------
typedef void*   (WINAPI *pfnDirect3DCreate8)(UINT SDKVersion);
typedef DWORD   (WINAPI *pfnInitializeExchangeServer)(void);
typedef HRESULT (WINAPI *pfnValidateVertexShader)(const DWORD*, const DWORD*, const void*, DWORD, char**);
typedef HRESULT (WINAPI *pfnValidatePixelShader)(const DWORD*, const void*, DWORD, char**);
typedef void    (WINAPI *pfnDebugSetMute)(void);

// Real d3d8.dll function pointers (for pass-through)
static pfnDirect3DCreate8      g_pfnRealCreate8      = NULL;
static pfnValidateVertexShader g_pfnValidateVS       = NULL;
static pfnValidatePixelShader  g_pfnValidatePS       = NULL;
static pfnDebugSetMute         g_pfnDebugSetMute     = NULL;

// Wrapper function pointer
static pfnDirect3DCreate8      g_pfnWrapCreate8      = NULL;

// ---------------------------------------------------------------------------
// Get the directory containing this proxy DLL
// ---------------------------------------------------------------------------
static void GetProxyDirectory(WCHAR* dir, DWORD maxLen)
{
    GetModuleFileNameW(g_hProxy, dir, maxLen);
    WCHAR* pSlash = wcsrchr(dir, L'\\');
    if (pSlash)
        *(pSlash + 1) = L'\0';
}

// ---------------------------------------------------------------------------
// Load real d3d8.dll from System32 (for pass-through functions)
// ---------------------------------------------------------------------------
static BOOL LoadRealD3D8(void)
{
    if (g_hRealD3D8)
        return TRUE;

    WCHAR sysDir[MAX_PATH];
    GetSystemDirectoryW(sysDir, MAX_PATH);
    lstrcatW(sysDir, L"\\d3d8.dll");

    g_hRealD3D8 = LoadLibraryW(sysDir);
    if (!g_hRealD3D8)
    {
        Log("FAIL: Could not load real d3d8.dll from %ls (error %lu)\n", sysDir, GetLastError());
        return FALSE;
    }
    Log("OK: Real d3d8.dll loaded from %ls\n", sysDir);

    g_pfnRealCreate8  = (pfnDirect3DCreate8)     GetProcAddress(g_hRealD3D8, "Direct3DCreate8");
    g_pfnValidateVS   = (pfnValidateVertexShader) GetProcAddress(g_hRealD3D8, "ValidateVertexShader");
    g_pfnValidatePS    = (pfnValidatePixelShader)  GetProcAddress(g_hRealD3D8, "ValidatePixelShader");
    g_pfnDebugSetMute  = (pfnDebugSetMute)         GetProcAddress(g_hRealD3D8, "DebugSetMute");

    return TRUE;
}

// ---------------------------------------------------------------------------
// Load S3DWrapperD3D8.dll and run the InitializeExchangeServer handshake
// ---------------------------------------------------------------------------
static void LoadWrapper(void)
{
    if (g_bWrapperActive)
        return;
    g_bWrapperActive = TRUE;   // only try once

    WCHAR wrapPath[MAX_PATH];
    GetProxyDirectory(wrapPath, MAX_PATH);
    lstrcatW(wrapPath, L"S3DWrapperD3D8.dll");

    Log("Loading wrapper: %ls\n", wrapPath);

    // Diagnostic: try loading without resolving imports first
    HMODULE hTest = LoadLibraryExW(wrapPath, NULL, DONT_RESOLVE_DLL_REFERENCES);
    if (hTest)
    {
        Log("OK: Wrapper PE is loadable (DONT_RESOLVE_DLL_REFERENCES)\n");
        FreeLibrary(hTest);
    }
    else
    {
        Log("FAIL: Wrapper PE not loadable even without imports (error %lu)\n", GetLastError());
    }

    // Pre-check known local dependencies
    {
        WCHAR depPath[MAX_PATH];
        static const WCHAR* localDeps[] = {
            L"S3DAPI.dll", L"S3DUtils.dll", L"ZLOg.dll", NULL
        };
        static const WCHAR* systemDeps[] = {
            L"PSAPI.DLL", L"d3dx9_43.dll", L"WINMM.dll", L"d3d9.dll", NULL
        };

        Log("--- Checking local dependencies ---\n");
        for (int i = 0; localDeps[i]; i++)
        {
            // Probing log helps distinguish a hang inside LoadLibrary's DllMain
            // from external process termination — without it, a deadlocked dep
            // produces an indistinguishable truncated log.
            Log("  Probing %ls...\n", localDeps[i]);
            GetProxyDirectory(depPath, MAX_PATH);
            lstrcatW(depPath, localDeps[i]);

            DWORD attrs = GetFileAttributesW(depPath);
            if (attrs == INVALID_FILE_ATTRIBUTES)
            {
                Log("    %ls: FILE NOT FOUND\n", localDeps[i]);
                continue;
            }

            HMODULE hDep = LoadLibraryW(depPath);
            if (hDep)
            {
                Log("    %ls: OK (loaded)\n", localDeps[i]);
                FreeLibrary(hDep);
            }
            else
            {
                Log("    %ls: FAIL (error %lu)\n", localDeps[i], GetLastError());
            }
        }

        Log("--- Checking system dependencies ---\n");
        for (int i = 0; systemDeps[i]; i++)
        {
            HMODULE hDep = LoadLibraryW(systemDeps[i]);
            if (hDep)
            {
                Log("  %ls: OK\n", systemDeps[i]);
                FreeLibrary(hDep);
            }
            else
            {
                Log("  %ls: FAIL (error %lu)\n", systemDeps[i], GetLastError());
            }
        }
        Log("--- End dependency check ---\n");
    }

    // Add the proxy's directory to the DLL search path
    {
        WCHAR proxyDir[MAX_PATH];
        GetProxyDirectory(proxyDir, MAX_PATH);
        size_t len = wcslen(proxyDir);
        if (len > 0 && proxyDir[len - 1] == L'\\')
            proxyDir[len - 1] = L'\0';
        SetDllDirectoryW(proxyDir);
        Log("SetDllDirectory: %ls\n", proxyDir);
    }

    g_hWrapper = LoadLibraryW(wrapPath);

    // Restore default DLL search order
    SetDllDirectoryW(NULL);

    if (!g_hWrapper)
    {
        Log("FAIL: Could not load wrapper (error %lu)\n", GetLastError());
        return;
    }
    Log("OK: Wrapper loaded\n");

    // Run the exchange-server handshake (same protocol as S3DInjector)
    pfnInitializeExchangeServer pGetRouter =
        (pfnInitializeExchangeServer)GetProcAddress(g_hWrapper, "InitializeExchangeServer");
    if (pGetRouter)
    {
        Log("Calling InitializeExchangeServer...\n");
        DWORD routerType = pGetRouter();
        Log("InitializeExchangeServer returned routerType=%lu\n", routerType);
        if (routerType <= 1)
        {
            g_pfnWrapCreate8 = (pfnDirect3DCreate8)GetProcAddress(g_hWrapper, "Direct3DCreate8");
            Log("Wrapper exports: Direct3DCreate8=%p\n", g_pfnWrapCreate8);
        }
        else
        {
            Log("WARN: routerType=%lu > 1, not using wrapper Create function\n", routerType);
        }
    }
    else
    {
        Log("FAIL: InitializeExchangeServer not found in wrapper\n");
    }

    // Diagnostic: log per-source profile match flags (base/community/user).
    // DX8 wrapper has its own minimal local gInfo (no ProfileName), so we
    // query S3DWrapperD3D9 which has S3DAPI/ProfileName. DX8 wrappers later
    // call Direct3DCreate9 anyway so loading S3DWrapperD3D9.dll explicitly
    // here only changes the timing — S3DAPI's profile read fires sooner.
    // Uses the DLL-search path the system already has (game folder first).
    HMODULE hWrap9 = LoadLibraryW(L"S3DWrapperD3D9.dll");
    if (hWrap9)
    {
        typedef void (__stdcall *pfnGetProfile)(char*, size_t, int*, int*, int*);
        pfnGetProfile pGetProfile =
            (pfnGetProfile)GetProcAddress(hWrap9, "wiz3D_GetActiveProfileInfo");
        if (pGetProfile)
        {
            char profileName[260] = {};
            int matchedBase = 0, matchedCommunity = 0, matchedUser = 0;
            pGetProfile(profileName, sizeof(profileName), &matchedBase, &matchedCommunity, &matchedUser);
            int anyMatched = matchedBase || matchedCommunity || matchedUser;
            Log("ProfileLoad: ProfileName='%s' base=%d community=%d user=%d\n",
                anyMatched ? profileName : "",
                matchedBase, matchedCommunity, matchedUser);
        }
        // intentionally do NOT FreeLibrary — the DX8 wrapper will pull it in
        // again via Direct3DCreate9 shortly, and dropping it here would just
        // waste a load/unload cycle.
    }
    else
    {
        Log("ProfileLoad: S3DWrapperD3D9.dll not loadable — DX8 profile state unknown\n");
    }

    if (!g_pfnWrapCreate8)
    {
        Log("WARN: No wrapper Direct3DCreate8 — falling through to real DLL\n");
        FreeLibrary(g_hWrapper);
        g_hWrapper = NULL;
    }

    // Post-load file checks
    {
        WCHAR chkPath[MAX_PATH];
        // Config: prefer wiz3D_Config.xml, accept legacy Config.xml.
        GetProxyDirectory(chkPath, MAX_PATH);
        lstrcatW(chkPath, L"wiz3D_Config.xml");
        DWORD attrNew = GetFileAttributesW(chkPath);
        GetProxyDirectory(chkPath, MAX_PATH);
        lstrcatW(chkPath, L"Config.xml");
        DWORD attr = GetFileAttributesW(chkPath);
        Log("--- Post-load file check ---\n");
        Log("  wiz3D_Config.xml: %s\n", (attrNew != INVALID_FILE_ATTRIBUTES) ? "FOUND" : "missing");
        Log("  Config.xml (legacy): %s\n", (attr != INVALID_FILE_ATTRIBUTES) ? "FOUND" : "missing");
        if (attrNew == INVALID_FILE_ATTRIBUTES && attr == INVALID_FILE_ATTRIBUTES)
            Log("  WARNING: no config file found\n");
        Log("--- End post-load check ---\n");
    }
}

// ---------------------------------------------------------------------------
// Exported: Direct3DCreate8
// ---------------------------------------------------------------------------
extern "C" __declspec(dllexport) void* WINAPI Direct3DCreate8(UINT SDKVersion)
{
    Log("Direct3DCreate8(SDKVersion=%u) called\n", SDKVersion);

    LoadRealD3D8();
    LoadWrapper();

    // If wrapper is active, route through it
    if (g_pfnWrapCreate8)
    {
        Log("Routing through WRAPPER Direct3DCreate8...\n");
        void* result = g_pfnWrapCreate8(SDKVersion);
        Log("Wrapper Direct3DCreate8 returned %p\n", result);
        return result;
    }

    // Fallback: pass through to real d3d8.dll
    if (g_pfnRealCreate8)
    {
        Log("Routing through REAL Direct3DCreate8 (no wrapper)\n");
        return g_pfnRealCreate8(SDKVersion);
    }

    Log("FAIL: No Direct3DCreate8 available, returning NULL\n");
    return NULL;
}

// ---------------------------------------------------------------------------
// Exported: ValidateVertexShader (pass-through to real d3d8.dll)
// ---------------------------------------------------------------------------
extern "C" __declspec(dllexport) HRESULT WINAPI ValidateVertexShader(
    const DWORD* pVertexShader,
    const DWORD* pVertexDecl,
    const void*  pCaps,
    DWORD        Flags,
    char**       ppErrorString)
{
    if (!LoadRealD3D8() || !g_pfnValidateVS) return E_FAIL;
    return g_pfnValidateVS(pVertexShader, pVertexDecl, pCaps, Flags, ppErrorString);
}

// ---------------------------------------------------------------------------
// Exported: ValidatePixelShader (pass-through to real d3d8.dll)
// ---------------------------------------------------------------------------
extern "C" __declspec(dllexport) HRESULT WINAPI ValidatePixelShader(
    const DWORD* pPixelShader,
    const void*  pCaps,
    DWORD        Flags,
    char**       ppErrorString)
{
    if (!LoadRealD3D8() || !g_pfnValidatePS) return E_FAIL;
    return g_pfnValidatePS(pPixelShader, pCaps, Flags, ppErrorString);
}

// ---------------------------------------------------------------------------
// Exported: DebugSetMute (pass-through to real d3d8.dll)
// ---------------------------------------------------------------------------
extern "C" __declspec(dllexport) void WINAPI DebugSetMute(void)
{
    if (!LoadRealD3D8() || !g_pfnDebugSetMute) return;
    g_pfnDebugSetMute();
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
        LogOpen();
        g_hVEH = AddVectoredExceptionHandler(1, VectoredCrashHandler);
        {
            WCHAR exePath[MAX_PATH];
            GetModuleFileNameW(NULL, exePath, MAX_PATH);
            Log("=== wiz3D " DISPLAYED_VERSION " - d3d8 proxy loaded ===\n");
            Log("Game exe: %ls\n", exePath);
            WCHAR proxyPath[MAX_PATH];
            GetModuleFileNameW(hModule, proxyPath, MAX_PATH);
            Log("Proxy DLL: %ls\n", proxyPath);
        }
        break;

    case DLL_PROCESS_DETACH:
        Log("=== wiz3D d3d8 proxy unloading ===\n");
        if (g_hVEH)
        {
            RemoveVectoredExceptionHandler(g_hVEH);
            g_hVEH = NULL;
        }
        if (g_hWrapper)
        {
            FreeLibrary(g_hWrapper);
            g_hWrapper = NULL;
        }
        if (g_hRealD3D8)
        {
            FreeLibrary(g_hRealD3D8);
            g_hRealD3D8 = NULL;
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
