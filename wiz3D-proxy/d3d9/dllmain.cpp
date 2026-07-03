/* wiz3D - d3d9.dll Proxy Loader
 *
 * Drop this d3d9.dll into a game's folder alongside S3DWrapperD3D9.dll
 * and the output plugins. The proxy forwards all D3D9 calls to the real
 * system DLL and routes Direct3DCreate9(Ex) through the iZ3D stereo
 * wrapper using the same InitializeExchangeServer protocol as S3DInjector.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "../proxy_version.h"
#include <stdio.h>
#include <psapi.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "psapi.lib")

// ---------------------------------------------------------------------------
// Simple diagnostic log — writes to wiz3D_proxy.log in the proxy's directory
// ---------------------------------------------------------------------------
static FILE* g_logFile = NULL;

static void LogOpen(void)
{
    if (g_logFile) return;
    WCHAR dir[MAX_PATH];
    GetModuleFileNameW(NULL, dir, MAX_PATH);  // game exe path
    WCHAR* pSlash = wcsrchr(dir, L'\\');
    if (pSlash) *(pSlash + 1) = L'\0';
    lstrcatW(dir, L"wiz3D_proxy.log");
    g_logFile = _wfopen(dir, L"a");  // append: shared with d3d10/d3d11/dxgi proxies
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
// Vectored exception handler — can't be overridden by the game engine.
// Only logs truly fatal exceptions (access violation, illegal instruction, etc.)
// ---------------------------------------------------------------------------
static PVOID g_hVEH = NULL;
static volatile LONG g_crashLogged = 0;  // prevent re-entrant logging

// Known false-alarm 3rd-party modules: AVs originating inside these DLLs are
// caught by the OWNING code (via __try/__except) and don't fatally affect the
// game, but our VEH was logging them as if they were fatal — confusing users
// reporting "crashes" that the game itself recovered from cleanly.
//
// Pattern across user reports (`project_known_false_alarms.md`):
//   - dxdiagn.dll / SETUPAPI.dll / SPFILEQ.dll: DirectX-diagnostics + setup-api
//     null-derefs during game init; game's launcher wraps in SEH. Bionic
//     Commando-AMD is the canonical example.
//   - EOSOVH-Win32-Shipping.dll: Epic Online Services overlay strlen-loop
//     trap when SR weave runs in TR2013-class titles (project_tr2013_sr_dead_end).
// Compared by leaf filename (case-insensitive). If a fault originates inside
// one of these, we return CONTINUE_SEARCH WITHOUT writing a dump — the
// existing SEH chain still gets the exception, the game still recovers.
static const wchar_t* const kFalseAlarmModules[] = {
    L"dxdiagn.dll",
    L"SETUPAPI.dll",
    L"SPFILEQ.dll",
    L"drvstore.dll",
    L"EOSOVH-Win32-Shipping.dll",
    // DimencoWeaving runs a VMware-backdoor hypervisor probe during SR
    // runtime init (`IN EAX, DX` to port 0x5658 with EAX="VMXh"). That
    // raises PRIV_INSTRUCTION which the SR SDK's own __try/__except
    // catches and uses to decide "we're on bare metal." Sometimes a
    // wrapping translator promotes it to an ACCESS_VIOLATION going
    // through this VEH first — whitelist by module so we don't log a
    // misleading fatal-dump for a working game. (PRIV_INSTRUCTION
    // itself is already excluded explicitly in the case-statement
    // above, but covers the AV-translation edge case here.)
    L"DimencoWeaving32.dll",     // x86
    L"DimencoWeaving.dll",       // x64
};

static bool IsKnownFalseAlarmModule(LPCWSTR fullPath)
{
    if (!fullPath) return false;
    LPCWSTR leaf = wcsrchr(fullPath, L'\\');
    leaf = leaf ? leaf + 1 : fullPath;
    for (const wchar_t* known : kFalseAlarmModules)
        if (_wcsicmp(leaf, known) == 0) return true;
    return false;
}

static LONG CALLBACK VectoredCrashHandler(EXCEPTION_POINTERS* pExInfo)
{
    DWORD code = pExInfo->ExceptionRecord->ExceptionCode;

    // Only intercept fatal exception codes — skip benign first-chance ones.
    // PRIV_INSTRUCTION and ILLEGAL_INSTRUCTION are deliberately excluded:
    //   - PRIV_INSTRUCTION fires from VMware/hypervisor backdoor probes (e.g. the
    //     `IN EAX, DX` to port 0x5658 with EAX="VMXh") that third-party DLLs
    //     wrap in __try/__except to detect virtualisation. Logging here breaks
    //     SR runtime initialisation in S3DAPI's DllMain — MiniDumpWriteDump
    //     under loader lock disrupts the SEH walk and DLL init aborts with 1114.
    //   - ILLEGAL_INSTRUCTION fires from CPU-feature probes (cpuid in unsupported
    //     modes, xgetbv on older CPUs) for the same SEH-wrapped detection pattern.
    switch (code)
    {
    // Explicit skip for benign probes — same as falling through to default,
    // but spelled out so a future edit that adds a case for one of these
    // codes (e.g. someone diagnosing a real PRIV crash) doesn't accidentally
    // re-enable logging of the SR-runtime VMware-probe and lock up DllMain.
    case EXCEPTION_PRIV_INSTRUCTION:        // DimencoWeaving hypervisor probe
    case EXCEPTION_ILLEGAL_INSTRUCTION:     // CPU-feature probes (cpuid/xgetbv)
        return EXCEPTION_CONTINUE_SEARCH;
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_STACK_OVERFLOW:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_IN_PAGE_ERROR:
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:
    case EXCEPTION_FLT_INVALID_OPERATION:
        break; // log these
    default:
        return EXCEPTION_CONTINUE_SEARCH; // skip C++/breakpoints/other benign
    }

    // Identify the faulting module FIRST — needed both for the false-alarm
    // filter below and the full crash log further down.
    void* crashAddr = pExInfo->ExceptionRecord->ExceptionAddress;
    HMODULE hMod = NULL;
    WCHAR faultingModName[MAX_PATH] = {};
    bool haveMod = false;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)crashAddr, &hMod))
    {
        GetModuleFileNameW(hMod, faultingModName, MAX_PATH);
        haveMod = true;
    }

    // P2 false-alarm filter: AVs inside known-noisy 3rd-party DLLs get
    // swallowed by the OWNING code's SEH and the game keeps running fine.
    // Skip the full dump path entirely and let the exception propagate.
    // We intentionally do NOT set g_crashLogged here, so a real fatal crash
    // later in the session still produces a dump.
    if (haveMod && IsKnownFalseAlarmModule(faultingModName))
    {
        // One-line trace per session is enough for triage; SAFE: no
        // MiniDumpWriteDump, no full stack walk, no loader-lock-sensitive
        // operations. Single fprintf-equivalent under Log()'s existing lock.
        static volatile LONG s_falseAlarmLogged = 0;
        if (InterlockedCompareExchange(&s_falseAlarmLogged, 1, 0) == 0)
        {
            BYTE* base = (BYTE*)hMod;
            DWORD_PTR offset = (BYTE*)crashAddr - base;
            LPCWSTR leaf = wcsrchr(faultingModName, L'\\');
            leaf = leaf ? leaf + 1 : faultingModName;
            Log("[VEH] Suppressed known-false-alarm exception in %ls + 0x%IX "
                "(code 0x%08lX) — game's own SEH handles this cleanly.\n",
                leaf, offset, code);
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }

    // Only log the first fatal exception (avoid flooding from re-entrant crashes)
    if (InterlockedCompareExchange(&g_crashLogged, 1, 0) != 0)
        return EXCEPTION_CONTINUE_SEARCH;

    Log("\n!!! FATAL EXCEPTION (VEH) !!!\n");
    Log("Exception code: 0x%08lX\n", code);
    Log("Crash address:  %p\n", crashAddr);

    if (haveMod)
    {
        BYTE* base = (BYTE*)hMod;
        DWORD_PTR offset = (BYTE*)crashAddr - base;
        Log("Faulting module: %ls + 0x%IX\n", faultingModName, offset);
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

    // Log register context
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

    // Stack trace via CaptureStackBackTrace
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

    // Also walk the stack from RSP to find return addresses
    Log("--- Stack memory (potential return addresses) ---\n");
    {
#ifdef _WIN64
        ULONG_PTR* pStack = (ULONG_PTR*)ctx->Rsp;
#else
        ULONG_PTR* pStack = (ULONG_PTR*)ctx->Esp;
#endif
        for (int i = 0; i < 32; i++)
        {
            __try {
                ULONG_PTR val = pStack[i];
                HMODULE hFrameMod = NULL;
                if (val > 0x10000 &&
                    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                       (LPCSTR)val, &hFrameMod))
                {
                    WCHAR modName[MAX_PATH];
                    GetModuleFileNameW(hFrameMod, modName, MAX_PATH);
                    WCHAR* pSlash = wcsrchr(modName, L'\\');
                    DWORD_PTR offset = (BYTE*)val - (BYTE*)hFrameMod;
                    Log("  RSP+0x%03X: %p  %ls+0x%IX\n",
                        (int)(i * sizeof(ULONG_PTR)), (void*)val,
                        pSlash ? pSlash + 1 : modName, offset);
                }
            } __except(EXCEPTION_EXECUTE_HANDLER) {
                break; // stop if stack is unreadable
            }
        }
    }
    Log("--- End stack memory ---\n");

    // List loaded modules for crash analysis
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

    // Write a minidump for later analysis (with full memory for better debugging)
    {
        WCHAR dumpPath[MAX_PATH];
        GetModuleFileNameW(NULL, dumpPath, MAX_PATH);
        WCHAR* pSlash = wcsrchr(dumpPath, L'\\');
        if (pSlash) *(pSlash + 1) = L'\0';
        lstrcatW(dumpPath, L"wiz3D_crash.dmp");

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
static HMODULE g_hRealD3D9 = NULL;   // real system d3d9.dll
static HMODULE g_hWrapper  = NULL;   // S3DWrapperD3D9.dll
static HMODULE g_hProxy    = NULL;   // our own HMODULE
static BOOL    g_bWrapperActive = FALSE;

// ---------------------------------------------------------------------------
// Typedefs matching the iZ3D wrapper protocol
// ---------------------------------------------------------------------------
typedef void*   (WINAPI *pfnDirect3DCreate9)(UINT SDKVersion);
typedef HRESULT (WINAPI *pfnDirect3DCreate9Ex)(UINT SDKVersion, void** ppD3D);
typedef DWORD   (WINAPI *pfnInitializeExchangeServer)(void);

// Real d3d9.dll function pointers
static pfnDirect3DCreate9   g_pfnRealCreate9   = NULL;
static pfnDirect3DCreate9Ex g_pfnRealCreate9Ex = NULL;

// Wrapper function pointers (resolved once)
static pfnDirect3DCreate9   g_pfnWrapCreate9   = NULL;
static pfnDirect3DCreate9Ex g_pfnWrapCreate9Ex = NULL;

// Forwarded helper exports
static FARPROC g_pfnD3DPERF_BeginEvent       = NULL;
static FARPROC g_pfnD3DPERF_EndEvent         = NULL;
static FARPROC g_pfnD3DPERF_GetStatus        = NULL;
static FARPROC g_pfnD3DPERF_QueryRepeatFrame = NULL;
static FARPROC g_pfnD3DPERF_SetMarker        = NULL;
static FARPROC g_pfnD3DPERF_SetOptions       = NULL;
static FARPROC g_pfnD3DPERF_SetRegion        = NULL;
static FARPROC g_pfnDebugSetLevel            = NULL;
static FARPROC g_pfnDebugSetMute             = NULL;

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
// Load real d3d9.dll from System32
// ---------------------------------------------------------------------------
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
// Load S3DWrapperD3D9.dll and run the InitializeExchangeServer handshake
// (mirrors S3DInjector's Direct3DCreate9Callback protocol)
// ---------------------------------------------------------------------------
static void LoadWrapper(void)
{
    if (g_bWrapperActive)
        return;
    g_bWrapperActive = TRUE;   // only try once

    WCHAR wrapPath[MAX_PATH];
    GetProxyDirectory(wrapPath, MAX_PATH);
    lstrcatW(wrapPath, L"S3DWrapperD3D9.dll");

    Log("Loading wrapper: %ls\n", wrapPath);

    // --- Dependency diagnostics ---
    // Try loading the wrapper without resolving imports first
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

    // Pre-check each known dependency of S3DWrapperD3D9.dll
    {
        WCHAR depPath[MAX_PATH];
        static const WCHAR* localDeps[] = {
            L"S3DAPI.dll", L"S3DUtils.dll", L"ZLOg.dll", NULL
        };
        static const WCHAR* systemDeps[] = {
            L"PSAPI.DLL", L"d3dx9_43.dll", L"WINMM.dll", NULL
        };

        Log("--- Checking local dependencies ---\n");
        for (int i = 0; localDeps[i]; i++)
        {
            // Per-dep "probing" log lets us tell the difference between a hang inside
            // LoadLibrary's DllMain and a process termination from elsewhere — without
            // this line a deadlocking dep produces an indistinguishable truncated log.
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

    // Add the proxy's directory to the DLL search path so that
    // S3DAPI.dll, ZLOg.dll etc. (which live next to the proxy) are found
    // even when the game exe is in a different directory (e.g. Source engine
    // games where exe is in root\ but our DLLs are in bin\).
    {
        WCHAR proxyDir[MAX_PATH];
        GetProxyDirectory(proxyDir, MAX_PATH);
        // Remove trailing backslash for SetDllDirectory
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
        DWORD loadErr = GetLastError();
        Log("FAIL: Could not load wrapper (error %lu)\n", loadErr);

        // Diagnostic: load wrapper PE without resolving imports, walk its import
        // table, then GetProcAddress each named import against the corresponding
        // DLL to identify exactly which symbol can't be resolved (error 127 case).
        HMODULE hPE = LoadLibraryExW(wrapPath, NULL, DONT_RESOLVE_DLL_REFERENCES);
        if (hPE)
        {
            BYTE* base = (BYTE*)hPE;
            IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
            IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
            IMAGE_DATA_DIRECTORY* impDir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
            if (impDir->Size > 0)
            {
                IMAGE_IMPORT_DESCRIPTOR* iid = (IMAGE_IMPORT_DESCRIPTOR*)(base + impDir->VirtualAddress);
                Log("--- Walking import table to find unresolved symbol ---\n");
                for (; iid->Name; ++iid)
                {
                    const char* dllName = (const char*)(base + iid->Name);
                    HMODULE hDep = GetModuleHandleA(dllName);
                    if (!hDep) hDep = LoadLibraryA(dllName);
                    if (!hDep) { Log("  [%s] DLL load failed (err %lu)\n", dllName, GetLastError()); continue; }

                    // OriginalFirstThunk (INT) preserves names; FirstThunk (IAT) is patched at load
                    DWORD intRva = iid->OriginalFirstThunk ? iid->OriginalFirstThunk : iid->FirstThunk;
                    IMAGE_THUNK_DATA* thunk = (IMAGE_THUNK_DATA*)(base + intRva);
                    int total = 0, missing = 0;
                    for (; thunk->u1.AddressOfData; ++thunk)
                    {
                        ++total;
                        if (IMAGE_SNAP_BY_ORDINAL(thunk->u1.Ordinal))
                        {
                            WORD ord = (WORD)IMAGE_ORDINAL(thunk->u1.Ordinal);
                            FARPROC p = GetProcAddress(hDep, MAKEINTRESOURCEA(ord));
                            if (!p) { Log("  [%s] MISSING ordinal %u\n", dllName, ord); ++missing; }
                        }
                        else
                        {
                            IMAGE_IMPORT_BY_NAME* iibn = (IMAGE_IMPORT_BY_NAME*)(base + thunk->u1.AddressOfData);
                            FARPROC p = GetProcAddress(hDep, iibn->Name);
                            if (!p) { Log("  [%s] MISSING %s\n", dllName, iibn->Name); ++missing; }
                        }
                    }
                    Log("  [%s] %d/%d resolved\n", dllName, total - missing, total);
                }
                Log("--- End import walk ---\n");
            }
            FreeLibrary(hPE);
        }
        else
        {
            Log("FAIL: Could not even load wrapper PE without imports (err %lu)\n", GetLastError());
        }
        return;
    }
    Log("OK: Wrapper loaded\n");

    // Run the exchange-server handshake (same as S3DInjector)
    pfnInitializeExchangeServer pGetRouter =
        (pfnInitializeExchangeServer)GetProcAddress(g_hWrapper, "InitializeExchangeServer");
    if (pGetRouter)
    {
        Log("Calling InitializeExchangeServer...\n");
        DWORD routerType = pGetRouter();
        Log("InitializeExchangeServer returned routerType=%lu\n", routerType);
        if (routerType <= 1)
        {
            // Router type 0 or 1: use wrapper's own Direct3DCreate9/Ex
            g_pfnWrapCreate9   = (pfnDirect3DCreate9)  GetProcAddress(g_hWrapper, "Direct3DCreate9");
            g_pfnWrapCreate9Ex = (pfnDirect3DCreate9Ex) GetProcAddress(g_hWrapper, "Direct3DCreate9Ex");
            Log("Wrapper exports: Direct3DCreate9=%p, Direct3DCreate9Ex=%p\n",
                g_pfnWrapCreate9, g_pfnWrapCreate9Ex);
        }
        else
        {
            Log("WARN: routerType=%lu > 1, not using wrapper Create functions\n", routerType);
        }
    }
    else
    {
        Log("FAIL: InitializeExchangeServer not found in wrapper\n");
    }

    // Diagnostic: log what profile XML(s) matched (or "" if none).
    // Per-source flags: base = BaseProfile.xml (iZ3D-shipped); community =
    // CommunityProfile.xml (user-contributed, only checked if base didn't
    // match); user = UserProfile.xml (current-user appdata, always checked,
    // can override base/community). All-zero = no real match, ProfileName
    // came from the exe-name fallback so it's logged empty to make the
    // miss obvious (common cause: BaseProfile.xml not next to the wrapper
    // DLL, or exe-name mismatch in the <File Name=".."/> tag).
    typedef void (__stdcall *pfnGetProfile)(char*, size_t, int*, int*, int*);
    pfnGetProfile pGetProfile =
        (pfnGetProfile)GetProcAddress(g_hWrapper, "wiz3D_GetActiveProfileInfo");
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

    if (!g_pfnWrapCreate9)
    {
        Log("WARN: No wrapper Direct3DCreate9 — falling through to real DLL\n");
        FreeLibrary(g_hWrapper);
        g_hWrapper = NULL;
    }

    // --- Post-load file checks ---
    // The wrapper expects wiz3D_Config.xml + OutputMethods/<name>.dll in the
    // game folder. If either is missing the wrapper's init paths run with
    // uninitialised state and dereference NULL deep inside the stereo
    // pipeline (Ninja Blade signature in user logs).  Treat missing-files as
    // wrapper-unavailable: free it, NULL the forward pointers, and let
    // Direct3DCreate9 fall through to the real d3d9.dll. The game then runs
    // mono — a worse experience than stereo, but better than a crash.
    {
        WCHAR chkPath[MAX_PATH];
        GetProxyDirectory(chkPath, MAX_PATH);
        lstrcatW(chkPath, L"wiz3D_Config.xml");
        DWORD attrNew = GetFileAttributesW(chkPath);
        GetProxyDirectory(chkPath, MAX_PATH);
        lstrcatW(chkPath, L"Config.xml");
        DWORD attrLegacy = GetFileAttributesW(chkPath);
        bool configFound = (attrNew != INVALID_FILE_ATTRIBUTES) ||
                           (attrLegacy != INVALID_FILE_ATTRIBUTES);

        Log("--- Post-load file check ---\n");
        Log("  wiz3D_Config.xml: %s\n", (attrNew != INVALID_FILE_ATTRIBUTES) ? "FOUND" : "missing");
        Log("  Config.xml (legacy): %s\n", (attrLegacy != INVALID_FILE_ATTRIBUTES) ? "FOUND" : "missing");

        GetProxyDirectory(chkPath, MAX_PATH);
        lstrcatW(chkPath, L"OutputMethods");
        DWORD omAttr = GetFileAttributesW(chkPath);
        bool omFound = (omAttr != INVALID_FILE_ATTRIBUTES) &&
                       (omAttr & FILE_ATTRIBUTE_DIRECTORY);
        Log("  OutputMethods/: %s\n", omFound ? "FOUND" : "MISSING!");

        GetProxyDirectory(chkPath, MAX_PATH);
        lstrcatW(chkPath, L"OutputMethods\\SideBySideOutput.dll");
        DWORD sbsAttr = GetFileAttributesW(chkPath);
        Log("  OutputMethods/SideBySideOutput.dll: %s\n",
            (sbsAttr != INVALID_FILE_ATTRIBUTES) ? "FOUND" : "MISSING!");
        Log("--- End post-load check ---\n");

        // Fail-fast: if essential files are absent, disable wrapper routing
        // and let the proxy fall through to the real d3d9.dll. Game then
        // runs mono instead of crashing deep inside the wrapper.
        if (!configFound || !omFound)
        {
            Log("FAIL: essential wiz3D files missing (config=%d OutputMethods=%d) — "
                "disabling wrapper, falling back to mono passthrough.\n",
                (int)configFound, (int)omFound);
            g_pfnWrapCreate9   = NULL;
            g_pfnWrapCreate9Ex = NULL;
            if (g_hWrapper)
            {
                FreeLibrary(g_hWrapper);
                g_hWrapper = NULL;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Exported: Direct3DCreate9
// ---------------------------------------------------------------------------
extern "C" __declspec(dllexport) void* WINAPI Direct3DCreate9(UINT SDKVersion)
{
    Log("Direct3DCreate9(SDKVersion=%u) called\n", SDKVersion);

    if (!LoadRealD3D9())
    {
        Log("FAIL: LoadRealD3D9 failed, returning NULL\n");
        return NULL;
    }

    LoadWrapper();

    // If wrapper is active, route through it (it internally calls the real DLL)
    if (g_pfnWrapCreate9)
    {
        Log("Routing through WRAPPER Direct3DCreate9...\n");
        void* result = g_pfnWrapCreate9(SDKVersion);
        Log("Wrapper Direct3DCreate9 returned %p\n", result);
        return result;
    }

    // Fallback: pass through to real d3d9.dll
    if (g_pfnRealCreate9)
    {
        Log("Routing through REAL Direct3DCreate9 (no wrapper)\n");
        return g_pfnRealCreate9(SDKVersion);
    }

    Log("FAIL: No Direct3DCreate9 available, returning NULL\n");
    return NULL;
}

// ---------------------------------------------------------------------------
// Exported: Direct3DCreate9Ex
// ---------------------------------------------------------------------------
extern "C" __declspec(dllexport) HRESULT WINAPI Direct3DCreate9Ex(UINT SDKVersion, void** ppD3D)
{
    Log("Direct3DCreate9Ex(SDKVersion=%u) called\n", SDKVersion);

    if (!LoadRealD3D9())
    {
        Log("FAIL: LoadRealD3D9 failed\n");
        return E_FAIL;
    }

    LoadWrapper();

    if (g_pfnWrapCreate9Ex)
    {
        Log("Routing through WRAPPER Direct3DCreate9Ex...\n");
        HRESULT hr = g_pfnWrapCreate9Ex(SDKVersion, ppD3D);
        Log("Wrapper Direct3DCreate9Ex returned 0x%08lX, ppD3D=%p\n", hr, ppD3D ? *ppD3D : NULL);
        return hr;
    }

    if (g_pfnRealCreate9Ex)
    {
        Log("Routing through REAL Direct3DCreate9Ex (no wrapper)\n");
        return g_pfnRealCreate9Ex(SDKVersion, ppD3D);
    }

    Log("FAIL: No Direct3DCreate9Ex available\n");
    return E_FAIL;
}

// ---------------------------------------------------------------------------
// Forwarded D3DPERF exports (used by PIX, profilers, some games)
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
        LogOpen();
        g_hVEH = AddVectoredExceptionHandler(1, VectoredCrashHandler);
        {
            WCHAR exePath[MAX_PATH];
            GetModuleFileNameW(NULL, exePath, MAX_PATH);
            Log("=== wiz3D " DISPLAYED_VERSION " - d3d9 proxy loaded ===\n");
            Log("Game exe: %ls\n", exePath);
            WCHAR proxyPath[MAX_PATH];
            GetModuleFileNameW(hModule, proxyPath, MAX_PATH);
            Log("Proxy DLL: %ls\n", proxyPath);
        }
        break;

    case DLL_PROCESS_DETACH:
        Log("=== wiz3D proxy unloading ===\n");
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
