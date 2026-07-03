/* wiz3D - dxgi.dll Proxy Loader
 *
 * Drop this dxgi.dll into a game's folder alongside S3DWrapperD3D10.dll
 * and the output plugins. The proxy forwards DXGI factory creation through
 * the iZ3D DX10 stereo wrapper using the same InitializeExchangeServer
 * protocol as S3DInjector.
 *
 * Two hook layers (same as the original S3DInjector):
 *   1. DXGI: CreateDXGIFactory/1 -> wrapper hooks CreateSwapChain vtable
 *   2. DDI:  GPU UMD's OpenAdapter10/10_2 -> wrapper wraps D3D10 device
 *
 * Re-entrancy handling: the wrapper internally calls CreateDXGIFactory
 * (which resolves back to us since we ARE dxgi.dll). We detect re-entry
 * and forward directly to the real system dxgi.dll.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "../proxy_version.h"
#include <stdio.h>
#include <stdlib.h>
#include <psapi.h>
#include <dbghelp.h>
#include <MinHook.h>
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
    // Append rather than truncate — when multiple proxies (dxgi + d3d10 + d3d11)
    // are deployed in the same game folder they all share this log file. Anyone
    // using "w" would clobber lines from the others mid-write.
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
// Vectored exception handler — can't be overridden by the game engine.
// Only logs truly fatal exceptions (access violation, illegal instruction, etc.)
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

    // Dump code bytes at crash address for opcode analysis
    Log("--- Code bytes at crash address ---\n");
    {
        BYTE* pCode = (BYTE*)crashAddr;
        __try
        {
            Log("  -16: ");
            for (int i = -16; i < 0; i++)
                Log("%02X ", pCode[i]);
            Log("\n");
            Log("  EIP: ");
            for (int i = 0; i < 32; i++)
                Log("%02X ", pCode[i]);
            Log("\n");
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            Log("  (could not read memory at %p)\n", crashAddr);
        }
    }
    Log("--- End code bytes ---\n");

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

    // Stack trace from the FAULTING context (not VEH handler stack)
    Log("--- Faulting thread stack trace (StackWalk64) ---\n");
    {
        SymInitialize(GetCurrentProcess(), NULL, TRUE);
        CONTEXT ctxCopy = *ctx;
        STACKFRAME64 sf = {};
#ifndef _WIN64
        sf.AddrPC.Offset    = ctxCopy.Eip;
        sf.AddrPC.Mode      = AddrModeFlat;
        sf.AddrFrame.Offset = ctxCopy.Ebp;
        sf.AddrFrame.Mode   = AddrModeFlat;
        sf.AddrStack.Offset = ctxCopy.Esp;
        sf.AddrStack.Mode   = AddrModeFlat;
        DWORD machType = IMAGE_FILE_MACHINE_I386;
#else
        sf.AddrPC.Offset    = ctxCopy.Rip;
        sf.AddrPC.Mode      = AddrModeFlat;
        sf.AddrFrame.Offset = ctxCopy.Rbp;
        sf.AddrFrame.Mode   = AddrModeFlat;
        sf.AddrStack.Offset = ctxCopy.Rsp;
        sf.AddrStack.Mode   = AddrModeFlat;
        DWORD machType = IMAGE_FILE_MACHINE_AMD64;
#endif
        for (int i = 0; i < 48; i++)
        {
            if (!StackWalk64(machType, GetCurrentProcess(), GetCurrentThread(),
                    &sf, &ctxCopy, NULL, SymFunctionTableAccess64, SymGetModuleBase64, NULL))
                break;
            if (sf.AddrPC.Offset == 0) break;

            HMODULE hFrameMod = NULL;
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   (LPCSTR)(ULONG_PTR)sf.AddrPC.Offset, &hFrameMod))
            {
                WCHAR modName[MAX_PATH];
                GetModuleFileNameW(hFrameMod, modName, MAX_PATH);
                WCHAR* pSlash = wcsrchr(modName, L'\\');
                DWORD_PTR offset = (DWORD_PTR)sf.AddrPC.Offset - (DWORD_PTR)hFrameMod;
                Log("  [%2d] %p  %ls+0x%IX\n", i, (void*)(ULONG_PTR)sf.AddrPC.Offset,
                    pSlash ? pSlash + 1 : modName, offset);
            }
            else
            {
                Log("  [%2d] %p  (unknown)\n", i, (void*)(ULONG_PTR)sf.AddrPC.Offset);
            }
        }
        SymCleanup(GetCurrentProcess());
    }
    Log("--- End faulting stack trace ---\n");

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
#ifdef _WIN64
                    Log("  RSP+0x%03X: %p  %ls+0x%IX\n",
#else
                    Log("  ESP+0x%03X: %p  %ls+0x%IX\n",
#endif
                        (int)(i * sizeof(ULONG_PTR)), (void*)val,
                        pSlash ? pSlash + 1 : modName, offset);
                }
            } __except(EXCEPTION_EXECUTE_HANDLER) {
                break;
            }
        }
    }
    Log("--- End stack memory ---\n");

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
static HMODULE g_hRealDXGI  = NULL;   // real system dxgi.dll
static HMODULE g_hWrapper   = NULL;   // S3DWrapperD3D10.dll
static HMODULE g_hProxy     = NULL;   // our own HMODULE
static BOOL    g_bWrapperActive = FALSE;
static HMODULE g_hUmd = NULL;         // GPU UMD (if we load it)

// ---------------------------------------------------------------------------
// Typedefs
// ---------------------------------------------------------------------------
// Real DXGI signatures (2 params)
typedef HRESULT (WINAPI *pfnCreateDXGIFactory)(REFIID riid, void** ppFactory);
typedef HRESULT (WINAPI *pfnCreateDXGIFactory2)(UINT Flags, REFIID riid, void** ppFactory);
typedef DWORD   (WINAPI *pfnInitializeExchangeServer)(void);

// Wrapper's Hooked_CreateDXGIFactory has 3 params (extra HMODULE)
typedef HRESULT (WINAPI *pfnHookedCreateDXGIFactory)(REFIID riid, void** ppFactory, HMODULE hCallingModule);
// Wrapper's Hooked_CreateDXGIFactory2 has 4 params (extra HMODULE)
typedef HRESULT (WINAPI *pfnHookedCreateDXGIFactory2)(UINT Flags, REFIID riid, void** ppFactory, HMODULE hCallingModule);

// Real DXGI function pointers
static pfnCreateDXGIFactory  g_pfnRealCreateFactory  = NULL;
static pfnCreateDXGIFactory  g_pfnRealCreateFactory1 = NULL;
static pfnCreateDXGIFactory2 g_pfnRealCreateFactory2 = NULL;

// Wrapper function pointers (3-param versions)
static pfnHookedCreateDXGIFactory  g_pfnWrapCreateFactory  = NULL;
static pfnHookedCreateDXGIFactory  g_pfnWrapCreateFactory1 = NULL;
static pfnHookedCreateDXGIFactory2 g_pfnWrapCreateFactory2 = NULL;

// Re-entrancy guard: wrapper calls CreateDXGIFactory internally,
// which resolves back to our export. We detect this and forward
// directly to the real dxgi.dll.
static __declspec(thread) int g_reentrant = 0;

// ---------------------------------------------------------------------------
// OpenAdapter10 DDI hooking — IAT-based approach
//
// The DX10 wrapper needs BOTH the DXGI factory hook (for swap chain wrapping)
// AND the DDI device hook (via OpenAdapter10, for stereo rendering).
// Without this, g_pLastD3DDevice is NULL and stereo is skipped.
//
// APPROACH: Instead of inline-hooking the GPU UMD with MinHook (which
// overwrites the first bytes of the driver function and can corrupt driver
// internals via trampolines), we IAT-hook GetProcAddress in d3d11.dll/d3d10.dll.
// When the D3D runtime resolves "OpenAdapter10" / "OpenAdapter10_2" from the
// UMD, we intercept the return value and give it our hook function.
// The UMD code remains COMPLETELY UNMODIFIED.
// ---------------------------------------------------------------------------
typedef HRESULT (APIENTRY *pfnOpenAdapter10)(void* pOpenData);

static pfnOpenAdapter10 g_pfnOrigOpenAdapter10   = NULL;
static pfnOpenAdapter10 g_pfnOrigOpenAdapter10_2 = NULL;
static pfnOpenAdapter10 g_pfnWrapOpenAdapter10   = NULL;
static pfnOpenAdapter10 g_pfnWrapOpenAdapter10_2 = NULL;

static BOOL g_bDDIHooksDisabled = FALSE;

// Re-entrancy guard for OpenAdapter10/_2 inline hooks. Once we inline-hook the
// UMD's OpenAdapter10, the wrapper's own OpenAdapter10 (in S3DWrapperD3D10) will
// re-enter the hook because it does `GetProcAddress(hUmd, "OpenAdapter10")` to
// find the real function — that returns the same patched address (only the
// function's prologue is patched, not the export table) and calling it goes
// through our hook again, causing infinite recursion → 0xC00000FD stack overflow.
//
// Pattern lifted from the original iZ3D S3DInjector::OpenAdapter10Callback —
// on the second (recursive) entry, bypass the wrapper and call the trampoline
// (g_pfnOrigOpenAdapter10) directly so the wrapper's caller sees the real
// UMD's behaviour.
static thread_local int g_callGuardOA10   = 0;
static thread_local int g_callGuardOA10_2 = 0;

// Lazy loading guard: ensure IAT hooks are installed exactly once when d3d10/d3d11 are present
static BOOL g_bIATHooksInstalled = FALSE;

static HRESULT APIENTRY OpenAdapter10Hook(void* pOpenData)
{
    g_callGuardOA10++;
    // Recursive entry from the wrapper's GetProcAddress→call sequence.
    // Skip the wrapper layer and run the trampoline (real UMD) directly.
    if (g_callGuardOA10 > 1)
    {
        Log("OpenAdapter10Hook: recursive entry (depth=%d) → calling trampoline\n", g_callGuardOA10);
        HRESULT hr = g_pfnOrigOpenAdapter10 ? g_pfnOrigOpenAdapter10(pOpenData) : E_FAIL;
        g_callGuardOA10--;
        return hr;
    }
    Log("OpenAdapter10Hook: pOpenData=%p, g_pfnWrapOpenAdapter10=%p, g_pfnOrigOpenAdapter10=%p\n",
        pOpenData, g_pfnWrapOpenAdapter10, g_pfnOrigOpenAdapter10);

    // Dump first 64 bytes of pOpenData BEFORE wrapper call
    if (pOpenData)
    {
        BYTE* pData = (BYTE*)pOpenData;
        __try
        {
            Log("  pOpenData BEFORE: ");
            for (int i = 0; i < 16; i++)
                Log("%02X ", pData[i]);
            Log("\n");
        }
        __except(EXCEPTION_EXECUTE_HANDLER)
        {
            Log("  (could not read pOpenData BEFORE)\n");
        }
    }

    HRESULT hr;
    if (g_pfnWrapOpenAdapter10)
    {
        hr = g_pfnWrapOpenAdapter10(pOpenData);
        Log("OpenAdapter10Hook: wrapper returned 0x%08lX\n", hr);

        // Dump first 64 bytes of pOpenData AFTER wrapper call
        if (pOpenData)
        {
            BYTE* pData = (BYTE*)pOpenData;
            __try
            {
                Log("  pOpenData AFTER: ");
                for (int i = 0; i < 16; i++)
                    Log("%02X ", pData[i]);
                Log("\n");
            }
            __except(EXCEPTION_EXECUTE_HANDLER)
            {
                Log("  (could not read pOpenData AFTER)\n");
            }
        }
    }
    else
    {
        Log("WARN: No wrapper OpenAdapter10, calling original\n");
        hr = g_pfnOrigOpenAdapter10 ? g_pfnOrigOpenAdapter10(pOpenData) : E_FAIL;
    }
    g_callGuardOA10--;
    return hr;
}

static HRESULT APIENTRY OpenAdapter10_2Hook(void* pOpenData)
{
    g_callGuardOA10_2++;
    if (g_callGuardOA10_2 > 1)
    {
        Log("OpenAdapter10_2Hook: recursive entry (depth=%d) → calling trampoline\n", g_callGuardOA10_2);
        HRESULT hr = g_pfnOrigOpenAdapter10_2 ? g_pfnOrigOpenAdapter10_2(pOpenData) : E_FAIL;
        g_callGuardOA10_2--;
        return hr;
    }
    Log("OpenAdapter10_2Hook: pOpenData=%p, g_pfnWrapOpenAdapter10_2=%p, g_pfnOrigOpenAdapter10_2=%p\n",
        pOpenData, g_pfnWrapOpenAdapter10_2, g_pfnOrigOpenAdapter10_2);

    // Dump first 64 bytes of pOpenData BEFORE wrapper call
    if (pOpenData)
    {
        BYTE* pData = (BYTE*)pOpenData;
        __try
        {
            Log("  pOpenData BEFORE: ");
            for (int i = 0; i < 16; i++)
                Log("%02X ", pData[i]);
            Log("\n");
        }
        __except(EXCEPTION_EXECUTE_HANDLER)
        {
            Log("  (could not read pOpenData BEFORE)\n");
        }
    }

    HRESULT hr;
    if (g_pfnWrapOpenAdapter10_2)
    {
        hr = g_pfnWrapOpenAdapter10_2(pOpenData);
        Log("OpenAdapter10_2Hook: wrapper returned 0x%08lX\n", hr);

        // Dump first 64 bytes of pOpenData AFTER wrapper call
        if (pOpenData)
        {
            BYTE* pData = (BYTE*)pOpenData;
            __try
            {
                Log("  pOpenData AFTER: ");
                for (int i = 0; i < 16; i++)
                    Log("%02X ", pData[i]);
                Log("\n");
            }
            __except(EXCEPTION_EXECUTE_HANDLER)
            {
                Log("  (could not read pOpenData AFTER)\n");
            }
        }
    }
    else
    {
        Log("WARN: No wrapper OpenAdapter10_2, calling original\n");
        hr = g_pfnOrigOpenAdapter10_2 ? g_pfnOrigOpenAdapter10_2(pOpenData) : E_FAIL;
    }
    g_callGuardOA10_2--;
    return hr;
}

// ---------------------------------------------------------------------------
// IAT hook for GetProcAddress — intercepts OpenAdapter10 resolution
// ---------------------------------------------------------------------------
typedef FARPROC (WINAPI *pfnGetProcAddress_t)(HMODULE, LPCSTR);
static pfnGetProcAddress_t g_pfnRealGPA_d3d11  = NULL;
static pfnGetProcAddress_t g_pfnRealGPA_d3d10  = NULL;

// Saved IAT slots so we can restore them on unload
static ULONG_PTR* g_pIATSlot_d3d11 = NULL;
static ULONG_PTR* g_pIATSlot_d3d10 = NULL;

static FARPROC WINAPI HookedGPA_d3d11(HMODULE hModule, LPCSTR lpProcName)
{
    FARPROC result = g_pfnRealGPA_d3d11(hModule, lpProcName);
    if (!result || !lpProcName || IS_INTRESOURCE(lpProcName))
        return result;

    if (strcmp(lpProcName, "OpenAdapter10_2") == 0)
    {
        g_pfnOrigOpenAdapter10_2 = (pfnOpenAdapter10)result;
        if (g_pfnWrapOpenAdapter10_2)
        {
            Log("IAT hook: Intercepted OpenAdapter10_2 (orig=%p) via d3d11.dll\n", result);
            return (FARPROC)OpenAdapter10_2Hook;
        }
    }
    else if (strcmp(lpProcName, "OpenAdapter10") == 0)
    {
        g_pfnOrigOpenAdapter10 = (pfnOpenAdapter10)result;
        if (g_pfnWrapOpenAdapter10)
        {
            Log("IAT hook: Intercepted OpenAdapter10 (orig=%p) via d3d11.dll\n", result);
            return (FARPROC)OpenAdapter10Hook;
        }
    }
    return result;
}

static FARPROC WINAPI HookedGPA_d3d10(HMODULE hModule, LPCSTR lpProcName)
{
    FARPROC result = g_pfnRealGPA_d3d10(hModule, lpProcName);
    if (!result || !lpProcName || IS_INTRESOURCE(lpProcName))
        return result;

    if (strcmp(lpProcName, "OpenAdapter10") == 0)
    {
        g_pfnOrigOpenAdapter10 = (pfnOpenAdapter10)result;
        if (g_pfnWrapOpenAdapter10)
        {
            Log("IAT hook: Intercepted OpenAdapter10 (orig=%p) via d3d10.dll\n", result);
            return (FARPROC)OpenAdapter10Hook;
        }
    }
    else if (strcmp(lpProcName, "OpenAdapter10_2") == 0)
    {
        g_pfnOrigOpenAdapter10_2 = (pfnOpenAdapter10)result;
        if (g_pfnWrapOpenAdapter10_2)
        {
            Log("IAT hook: Intercepted OpenAdapter10_2 (orig=%p) via d3d10.dll\n", result);
            return (FARPROC)OpenAdapter10_2Hook;
        }
    }
    return result;
}

// Patch a module's IAT entry for GetProcAddress.
// Returns TRUE if successful, and fills *ppIATSlot for later restore.
static BOOL PatchModuleIAT(HMODULE hModule, const char* moduleName,
    FARPROC (WINAPI *hookFunc)(HMODULE, LPCSTR),
    pfnGetProcAddress_t* pSavedOriginal, ULONG_PTR** ppIATSlot)
{
    if (!hModule) return FALSE;
    *ppIATSlot = NULL;

    __try
    {
        BYTE* pBase = (BYTE*)hModule;
        IMAGE_DOS_HEADER* pDOS = (IMAGE_DOS_HEADER*)pBase;
        if (pDOS->e_magic != IMAGE_DOS_SIGNATURE) return FALSE;

        IMAGE_NT_HEADERS* pNT = (IMAGE_NT_HEADERS*)(pBase + pDOS->e_lfanew);
        if (pNT->Signature != IMAGE_NT_SIGNATURE) return FALSE;

        DWORD importRVA = pNT->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
        if (!importRVA) return FALSE;

        IMAGE_IMPORT_DESCRIPTOR* pImport = (IMAGE_IMPORT_DESCRIPTOR*)(pBase + importRVA);

        for (; pImport->Name; pImport++)
        {
            const char* dllName = (const char*)(pBase + pImport->Name);
            IMAGE_THUNK_DATA* pOrigThunk = (IMAGE_THUNK_DATA*)(pBase + pImport->OriginalFirstThunk);
            IMAGE_THUNK_DATA* pIAT       = (IMAGE_THUNK_DATA*)(pBase + pImport->FirstThunk);

            for (; pOrigThunk->u1.AddressOfData; pOrigThunk++, pIAT++)
            {
                if (pOrigThunk->u1.Ordinal & IMAGE_ORDINAL_FLAG)
                    continue;

                IMAGE_IMPORT_BY_NAME* pName =
                    (IMAGE_IMPORT_BY_NAME*)(pBase + pOrigThunk->u1.AddressOfData);

                if (strcmp(pName->Name, "GetProcAddress") == 0)
                {
                    *pSavedOriginal = (pfnGetProcAddress_t)pIAT->u1.Function;
                    *ppIATSlot = &pIAT->u1.Function;

                    DWORD oldProtect;
                    if (VirtualProtect(&pIAT->u1.Function, sizeof(ULONG_PTR),
                                       PAGE_READWRITE, &oldProtect))
                    {
                        pIAT->u1.Function = (ULONG_PTR)hookFunc;
                        VirtualProtect(&pIAT->u1.Function, sizeof(ULONG_PTR),
                                       oldProtect, &oldProtect);
                        Log("  OK: IAT-hooked GetProcAddress in %s (import from %s)\n",
                            moduleName, dllName);
                        return TRUE;
                    }
                    else
                    {
                        Log("  FAIL: VirtualProtect on %s IAT (error %lu)\n",
                            moduleName, GetLastError());
                        return FALSE;
                    }
                }
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        Log("  FAIL: Exception parsing %s PE headers\n", moduleName);
        return FALSE;
    }

    Log("  WARN: GetProcAddress not found in %s IAT\n", moduleName);
    return FALSE;
}

// Restore original GetProcAddress in patched IAT slots
static void RestoreD3D_IAT(void)
{
    if (g_pIATSlot_d3d11 && g_pfnRealGPA_d3d11)
    {
        DWORD oldProtect;
        if (VirtualProtect(g_pIATSlot_d3d11, sizeof(ULONG_PTR), PAGE_READWRITE, &oldProtect))
        {
            *g_pIATSlot_d3d11 = (ULONG_PTR)g_pfnRealGPA_d3d11;
            VirtualProtect(g_pIATSlot_d3d11, sizeof(ULONG_PTR), oldProtect, &oldProtect);
        }
        g_pIATSlot_d3d11 = NULL;
    }
    if (g_pIATSlot_d3d10 && g_pfnRealGPA_d3d10)
    {
        DWORD oldProtect;
        if (VirtualProtect(g_pIATSlot_d3d10, sizeof(ULONG_PTR), PAGE_READWRITE, &oldProtect))
        {
            *g_pIATSlot_d3d10 = (ULONG_PTR)g_pfnRealGPA_d3d10;
            VirtualProtect(g_pIATSlot_d3d10, sizeof(ULONG_PTR), oldProtect, &oldProtect);
        }
        g_pIATSlot_d3d10 = NULL;
    }
}

// Registry path for display adapters
#define GPU_CLASS_KEY L"SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e968-e325-11ce-bfc1-08002be10318}"

// Write UMD filename to iZ3D registry and ensure UMD is loaded.
// (No inline hooking — just registry setup for the wrapper.)
static void SetupUmdRegistry(void)
{
    HKEY hClassKey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, GPU_CLASS_KEY, 0, KEY_READ, &hClassKey) != ERROR_SUCCESS)
    {
        Log("WARN: Cannot open GPU class registry key\n");
        return;
    }

    WCHAR subkeyName[16];
    BOOL bFound = FALSE;
    for (DWORD i = 0; !bFound &&
         RegEnumKeyW(hClassKey, i, subkeyName, _countof(subkeyName)) == ERROR_SUCCESS; i++)
    {
        HKEY hAdapterKey;
        if (RegOpenKeyExW(hClassKey, subkeyName, 0, KEY_READ, &hAdapterKey) != ERROR_SUCCESS)
            continue;

        BOOL bIsWow64 = FALSE;
        IsWow64Process(GetCurrentProcess(), &bIsWow64);
        const WCHAR* valueName = bIsWow64 ? L"UserModeDriverNameWoW" : L"UserModeDriverName";
        WCHAR umdNames[4096];
        DWORD umdSize = sizeof(umdNames);
        DWORD umdType = 0;

        if (RegQueryValueExW(hAdapterKey, valueName, NULL, &umdType,
                             (BYTE*)umdNames, &umdSize) != ERROR_SUCCESS)
        {
            RegCloseKey(hAdapterKey);
            continue;
        }
        RegCloseKey(hAdapterKey);

        if (umdNames[0] == L'\0')
            continue;

        WCHAR umdDll[MAX_PATH];
        lstrcpynW(umdDll, umdNames, MAX_PATH);

        WCHAR umdPath[MAX_PATH];
        if (umdDll[1] == L':' || (umdDll[0] == L'\\' && umdDll[1] == L'\\'))
            lstrcpynW(umdPath, umdDll, MAX_PATH);
        else
        {
            GetSystemDirectoryW(umdPath, MAX_PATH);
            lstrcatW(umdPath, L"\\");
            lstrcatW(umdPath, umdDll);
        }

        Log("GPU UMD [%ls\\%ls]: %ls\n", GPU_CLASS_KEY, subkeyName, umdPath);

        // Ensure UMD is loaded (the wrapper uses GetModuleHandle on it)
        HMODULE hUmd = LoadLibraryW(umdPath);
        if (!hUmd)
        {
            Log("  Could not load UMD (error %lu) — skipping\n", GetLastError());
            continue;
        }

        FARPROC pOA10   = GetProcAddress(hUmd, "OpenAdapter10");
        FARPROC pOA10_2 = GetProcAddress(hUmd, "OpenAdapter10_2");
        Log("  UMD exports: OpenAdapter10=%p, OpenAdapter10_2=%p\n", pOA10, pOA10_2);
        // Keep a handle to the UMD so we can install inline hooks with MinHook if needed
        g_hUmd = hUmd;

        // MinHook inline hook on UMD's OpenAdapter10/_2. The original disable note
        // here said this caused stack overflow 0xC00000FD via infinite recursion
        // — that was real, but the cause was the missing call guard in
        // OpenAdapter10Hook (the wrapper's GetProcAddress→call of OpenAdapter10
        // re-enters the hook). Now fixed by g_callGuardOA10/_2 above. Pattern
        // copied from S3DInjector::OpenAdapter10Callback (the original iZ3D
        // injector's working implementation).
        if (!g_bDDIHooksDisabled)
        {
            if (pOA10 && g_pfnWrapOpenAdapter10)
            {
                if (MH_CreateHook(pOA10, (LPVOID)OpenAdapter10Hook, (LPVOID*)&g_pfnOrigOpenAdapter10) == MH_OK)
                {
                    if (MH_EnableHook(pOA10) == MH_OK)
                        Log("  OK: MinHook inline-hooked UMD OpenAdapter10 (%p)\n", pOA10);
                    else
                        Log("  FAIL: MH_EnableHook(OpenAdapter10) failed\n");
                }
                else
                {
                    Log("  FAIL: MH_CreateHook(OpenAdapter10) failed\n");
                }
            }
            if (pOA10_2 && g_pfnWrapOpenAdapter10_2)
            {
                if (MH_CreateHook(pOA10_2, (LPVOID)OpenAdapter10_2Hook, (LPVOID*)&g_pfnOrigOpenAdapter10_2) == MH_OK)
                {
                    if (MH_EnableHook(pOA10_2) == MH_OK)
                        Log("  OK: MinHook inline-hooked UMD OpenAdapter10_2 (%p)\n", pOA10_2);
                    else
                        Log("  FAIL: MH_EnableHook(OpenAdapter10_2) failed\n");
                }
                else
                {
                    Log("  FAIL: MH_CreateHook(OpenAdapter10_2) failed\n");
                }
            }
        }

        // Write UMD filename to iZ3D registry so the wrapper can locate it
        {
            const WCHAR* pFileName = wcsrchr(umdPath, L'\\');
            pFileName = pFileName ? pFileName + 1 : umdPath;

            HKEY hIZ3DKey;
            if (RegCreateKeyExW(HKEY_CURRENT_USER,
                    L"SOFTWARE\\iZ3D\\iZ3D Driver\\Win32",
                    0, NULL, 0, KEY_WRITE, NULL, &hIZ3DKey, NULL) == ERROR_SUCCESS)
            {
                DWORD cbData = (DWORD)((wcslen(pFileName) + 1) * sizeof(WCHAR));
                RegSetValueExW(hIZ3DKey, L"DriverD3D10VistaModule", 0, REG_SZ,
                    (const BYTE*)pFileName, cbData);
                RegSetValueExW(hIZ3DKey, L"DriverD3D10Win7Module", 0, REG_SZ,
                    (const BYTE*)pFileName, cbData);
                RegCloseKey(hIZ3DKey);
                Log("  Wrote UMD filename '%ls' to iZ3D registry\n", pFileName);
            }
            else
            {
                Log("  WARN: Could not write UMD filename to iZ3D registry\n");
            }
        }

        bFound = TRUE;
    }

    RegCloseKey(hClassKey);
}

// Set up IAT hooks on D3D runtime DLLs for GetProcAddress interception
static void HookD3D_IAT(void)
{
    if (g_bDDIHooksDisabled)
    {
        Log("DDI hooks DISABLED by WIZ3D_NO_DDI_HOOK\n");
        return;
    }

    Log("--- IAT-hooking D3D runtime GetProcAddress ---\n");

    HMODULE hD3D11 = GetModuleHandleW(L"d3d11.dll");
    HMODULE hD3D10 = GetModuleHandleW(L"d3d10.dll");

    if (hD3D11)
        PatchModuleIAT(hD3D11, "d3d11.dll", HookedGPA_d3d11,
                        &g_pfnRealGPA_d3d11, &g_pIATSlot_d3d11);
    else
        Log("  d3d11.dll not loaded — skipping\n");

    if (hD3D10)
        PatchModuleIAT(hD3D10, "d3d10.dll", HookedGPA_d3d10,
                        &g_pfnRealGPA_d3d10, &g_pIATSlot_d3d10);
    else
        Log("  d3d10.dll not loaded — skipping\n");

    if (!hD3D11 && !hD3D10)
        Log("WARN: Neither d3d11.dll nor d3d10.dll loaded — DDI hooks not possible\n");

    Log("--- End IAT hook ---\n");
}

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
// Load real dxgi.dll from System32
// ---------------------------------------------------------------------------
static BOOL LoadRealDXGI(void)
{
    if (g_hRealDXGI)
        return TRUE;

    WCHAR sysDir[MAX_PATH];
    GetSystemDirectoryW(sysDir, MAX_PATH);
    lstrcatW(sysDir, L"\\dxgi.dll");

    g_hRealDXGI = LoadLibraryW(sysDir);
    if (!g_hRealDXGI)
    {
        Log("FAIL: Could not load real dxgi.dll from %ls (error %lu)\n", sysDir, GetLastError());
        return FALSE;
    }
    Log("OK: Real dxgi.dll loaded from %ls (handle=%p)\n", sysDir, g_hRealDXGI);

    g_pfnRealCreateFactory  = (pfnCreateDXGIFactory) GetProcAddress(g_hRealDXGI, "CreateDXGIFactory");
    g_pfnRealCreateFactory1 = (pfnCreateDXGIFactory) GetProcAddress(g_hRealDXGI, "CreateDXGIFactory1");
    g_pfnRealCreateFactory2 = (pfnCreateDXGIFactory2)GetProcAddress(g_hRealDXGI, "CreateDXGIFactory2");

    Log("Real exports: CreateDXGIFactory=%p, CreateDXGIFactory1=%p, CreateDXGIFactory2=%p\n",
        g_pfnRealCreateFactory, g_pfnRealCreateFactory1, g_pfnRealCreateFactory2);

    return TRUE;
}

// ---------------------------------------------------------------------------
// Load S3DWrapperD3D10.dll and run the InitializeExchangeServer handshake
// ---------------------------------------------------------------------------
static void LoadWrapper(void)
{
    if (g_bWrapperActive)
        return;
    g_bWrapperActive = TRUE;

    WCHAR wrapPath[MAX_PATH];
    GetProxyDirectory(wrapPath, MAX_PATH);
    lstrcatW(wrapPath, L"S3DWrapperD3D10.dll");

    Log("Loading wrapper: %ls\n", wrapPath);

    // Dependency diagnostics
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

    // Pre-check dependencies
    {
        WCHAR depPath[MAX_PATH];
        static const WCHAR* localDeps[] = {
            L"S3DAPI.dll", L"S3DUtils.dll", L"ZLOg.dll", NULL
        };
        static const WCHAR* systemDeps[] = {
            L"PSAPI.DLL", L"d3dx9_43.dll", L"WINMM.dll", L"D3DCOMPILER_47.dll", NULL
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

    // Set DLL search path to proxy directory
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

    SetDllDirectoryW(NULL);

    if (!g_hWrapper)
    {
        Log("FAIL: Could not load wrapper (error %lu)\n", GetLastError());
        return;
    }
    Log("OK: Wrapper loaded (handle=%p)\n", g_hWrapper);

    // Run the exchange-server handshake
    pfnInitializeExchangeServer pGetRouter =
        (pfnInitializeExchangeServer)GetProcAddress(g_hWrapper, "InitializeExchangeServer");
    if (pGetRouter)
    {
        Log("Calling InitializeExchangeServer...\n");
        DWORD routerType = pGetRouter();
        Log("InitializeExchangeServer returned routerType=%lu\n", routerType);
        if (routerType <= 1)
        {
            // Wrapper's CreateDXGIFactory export = Hooked_CreateDXGIFactory (3-param version)
            g_pfnWrapCreateFactory  = (pfnHookedCreateDXGIFactory)GetProcAddress(g_hWrapper, "CreateDXGIFactory");
            g_pfnWrapCreateFactory1 = (pfnHookedCreateDXGIFactory)GetProcAddress(g_hWrapper, "CreateDXGIFactory1");
            g_pfnWrapCreateFactory2 = (pfnHookedCreateDXGIFactory2)GetProcAddress(g_hWrapper, "CreateDXGIFactory2");
            Log("Wrapper exports: CreateDXGIFactory=%p, CreateDXGIFactory1=%p, CreateDXGIFactory2=%p\n",
                g_pfnWrapCreateFactory, g_pfnWrapCreateFactory1, g_pfnWrapCreateFactory2);

            // DDI device wrapping (OpenAdapter10/OpenAdapter10_2). The wrapper's
            // implementation reads the UMD path from the iZ3D installer's registry
            // keys (HKEY_CURRENT_USER\SOFTWARE\iZ3D\iZ3D Driver\Win32) — those keys
            // are absent in our portable proxy deployment, so we populate them
            // ourselves via SetupUmdRegistry below before the runtime calls
            // OpenAdapter10. With that fixed, the wrapper's OpenAdapter10 can
            // resolve and call the real UMD's OpenAdapter10 successfully.
            // (If this re-enable causes GPU driver crashes set the
            // WIZ3D_NO_DDI_HOOK=1 environment variable to bypass the IAT hooks.)
            g_pfnWrapOpenAdapter10   = (pfnOpenAdapter10)GetProcAddress(g_hWrapper, "OpenAdapter10");
            g_pfnWrapOpenAdapter10_2 = (pfnOpenAdapter10)GetProcAddress(g_hWrapper, "OpenAdapter10_2");
            Log("Wrapper exports: OpenAdapter10=%p, OpenAdapter10_2=%p\n",
                g_pfnWrapOpenAdapter10, g_pfnWrapOpenAdapter10_2);

            // Populate the iZ3D registry keys the wrapper's OpenAdapter10 reads
            // (HKCU\SOFTWARE\iZ3D\iZ3D Driver\Win32 → DriverD3D10VistaModule /
            // DriverD3D10Win7Module values, set to the UMD's filename).
            Log("--- GPU UMD registry setup ---\n");
            SetupUmdRegistry();
            Log("--- End UMD registry setup ---\n");
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

    if (!g_pfnWrapCreateFactory)
    {
        Log("WARN: No wrapper CreateDXGIFactory — falling through to real DLL\n");
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

        GetProxyDirectory(chkPath, MAX_PATH);
        lstrcatW(chkPath, L"OutputMethods");
        attr = GetFileAttributesW(chkPath);
        Log("  OutputMethods/: %s\n",
            (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) ? "FOUND" : "MISSING!");

        GetProxyDirectory(chkPath, MAX_PATH);
        lstrcatW(chkPath, L"OutputMethods\\SideBySideOutput.dll");
        attr = GetFileAttributesW(chkPath);
        Log("  OutputMethods/SideBySideOutput.dll: %s\n",
            (attr != INVALID_FILE_ATTRIBUTES) ? "FOUND" : "MISSING!");
        Log("--- End post-load check ---\n");
    }
}

// ---------------------------------------------------------------------------
// Diagnostic: identify which IDXGIFactory* interface was requested, what's at
// vtable[10] (the wrapper hooks IDXGIFactory::CreateSwapChain there), which
// module owns that function, and the first 5 bytes (0xE9 = JMP indicates a
// minhook-style prologue patch is in place). Lets us tell from a log alone
// whether the wrapper's hook actually got installed for this game.
// ---------------------------------------------------------------------------
static void LogFactoryDiagnostics(REFIID riid, void* pFactory)
{
    const char* iidName = "(unknown — wrapper does not hook this interface)";
    switch (riid.Data1)
    {
    case 0x7b7166ec: iidName = "IDXGIFactory"; break;
    case 0x770aae78: iidName = "IDXGIFactory1"; break;
    case 0x50c83a1c: iidName = "IDXGIFactory2"; break;
    case 0x25483823: iidName = "IDXGIFactory3"; break;
    case 0x1bc6ea02: iidName = "IDXGIFactory4"; break;
    case 0x7632e1f5: iidName = "IDXGIFactory5"; break;
    case 0xc1b6694f: iidName = "IDXGIFactory6"; break;
    }
    Log("  Requested IID: %s {%08lX-%04hX-%04hX-...}\n",
        iidName, riid.Data1, riid.Data2, riid.Data3);

    if (!pFactory)
        return;

    void** vtable = *(void***)pFactory;
    void* createSwapChainFn = vtable[10];
    Log("  vtable[10] (CreateSwapChain) = %p\n", createSwapChainFn);

    HMODULE hMod = NULL;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)createSwapChainFn, &hMod))
    {
        CHAR modName[MAX_PATH] = {0};
        GetModuleFileNameA(hMod, modName, MAX_PATH);
        Log("    function lives in: %s\n", modName);
    }

    unsigned char b[5] = {0};
    __try { memcpy(b, createSwapChainFn, 5); } __except(EXCEPTION_EXECUTE_HANDLER) {}
    const char* hookState = (b[0] == 0xE9 || b[0] == 0xEB) ? "JMP — minhook prologue patch active"
                          : (b[0] == 0xFF && b[1] == 0x25) ? "indirect JMP — likely hooked"
                          : "no patch detected";
    Log("    first 5 bytes: %02X %02X %02X %02X %02X (%s)\n",
        b[0], b[1], b[2], b[3], b[4], hookState);
}

// ---------------------------------------------------------------------------
// Exported: CreateDXGIFactory
// ---------------------------------------------------------------------------
extern "C" __declspec(dllexport) HRESULT WINAPI CreateDXGIFactory(REFIID riid, void** ppFactory)
{
    Log("CreateDXGIFactory called (reentrant=%d)\n", g_reentrant);

    if (!LoadRealDXGI())
    {
        Log("FAIL: LoadRealDXGI failed\n");
        return E_FAIL;
    }

    // Re-entrancy: wrapper internally calls CreateDXGIFactory which
    // resolves back here. Forward directly to real dxgi.dll.
    if (g_reentrant > 0)
    {
        Log("  Re-entrant call — forwarding to real dxgi.dll\n");
        return g_pfnRealCreateFactory(riid, ppFactory);
    }

    LoadWrapper();

    // Lazy IAT-hook GetProcAddress in d3d10/d3d11.dll so the runtime gets our
    // OpenAdapter10/_2 hooks when it resolves them from the UMD. Done lazily on
    // the first factory creation because d3d10/d3d11.dll usually aren't loaded
    // until the runtime is about to use them.
    if (!g_bIATHooksInstalled && g_pfnWrapOpenAdapter10)
    {
        HookD3D_IAT();
        g_bIATHooksInstalled = TRUE;
    }

    if (g_pfnWrapCreateFactory)
    {
        Log("Routing through WRAPPER CreateDXGIFactory...\n");
        g_reentrant++;
        HRESULT hr = g_pfnWrapCreateFactory(riid, ppFactory, g_hProxy);
        g_reentrant--;
        Log("Wrapper CreateDXGIFactory returned 0x%08lX, ppFactory=%p\n", hr,
            ppFactory ? *ppFactory : NULL);
        LogFactoryDiagnostics(riid, ppFactory ? *ppFactory : NULL);
        return hr;
    }

    if (g_pfnRealCreateFactory)
    {
        Log("Routing through REAL CreateDXGIFactory (no wrapper)\n");
        return g_pfnRealCreateFactory(riid, ppFactory);
    }

    Log("FAIL: No CreateDXGIFactory available\n");
    return E_FAIL;
}

// ---------------------------------------------------------------------------
// Exported: CreateDXGIFactory1
// ---------------------------------------------------------------------------
extern "C" __declspec(dllexport) HRESULT WINAPI CreateDXGIFactory1(REFIID riid, void** ppFactory)
{
    Log("CreateDXGIFactory1 called (reentrant=%d)\n", g_reentrant);

    if (!LoadRealDXGI())
    {
        Log("FAIL: LoadRealDXGI failed\n");
        return E_FAIL;
    }

    if (g_reentrant > 0)
    {
        Log("  Re-entrant call — forwarding to real dxgi.dll\n");
        return g_pfnRealCreateFactory1(riid, ppFactory);
    }

    LoadWrapper();

    // Lazy IAT-hook GetProcAddress in d3d10/d3d11.dll so the runtime gets our
    // OpenAdapter10/_2 hooks when it resolves them from the UMD. Done lazily on
    // the first factory creation because d3d10/d3d11.dll usually aren't loaded
    // until the runtime is about to use them.
    if (!g_bIATHooksInstalled && g_pfnWrapOpenAdapter10)
    {
        HookD3D_IAT();
        g_bIATHooksInstalled = TRUE;
    }

    if (g_pfnWrapCreateFactory1)
    {
        Log("Routing through WRAPPER CreateDXGIFactory1...\n");
        g_reentrant++;
        HRESULT hr = g_pfnWrapCreateFactory1(riid, ppFactory, g_hProxy);
        g_reentrant--;
        Log("Wrapper CreateDXGIFactory1 returned 0x%08lX, ppFactory=%p\n", hr,
            ppFactory ? *ppFactory : NULL);
        LogFactoryDiagnostics(riid, ppFactory ? *ppFactory : NULL);
        return hr;
    }

    // Fallback: if wrapper doesn't have CreateDXGIFactory1, use real
    if (g_pfnRealCreateFactory1)
    {
        Log("Routing through REAL CreateDXGIFactory1 (no wrapper)\n");
        return g_pfnRealCreateFactory1(riid, ppFactory);
    }

    Log("FAIL: No CreateDXGIFactory1 available\n");
    return E_FAIL;
}

// ---------------------------------------------------------------------------
// Exported: CreateDXGIFactory2
// Routed through the wrapper so its Hooked_CreateDXGIFactory2 can vtable-
// hook slot 10 plus the Factory2-specific swap-chain slots (15/16/24).
// Required for DX10/11 games that take the CreateDXGIFactory2 path —
// without this they bypass our slot-10 hook entirely.
// ---------------------------------------------------------------------------
extern "C" __declspec(dllexport) HRESULT WINAPI CreateDXGIFactory2(UINT Flags, REFIID riid, void** ppFactory)
{
    Log("CreateDXGIFactory2(Flags=%u) called (reentrant=%d)\n", Flags, g_reentrant);

    if (!LoadRealDXGI() || !g_pfnRealCreateFactory2)
    {
        Log("FAIL: No real CreateDXGIFactory2 available\n");
        return E_FAIL;
    }

    if (g_reentrant > 0)
    {
        Log("  Re-entrant call — forwarding to real dxgi.dll\n");
        return g_pfnRealCreateFactory2(Flags, riid, ppFactory);
    }

    LoadWrapper();

    if (!g_bIATHooksInstalled && g_pfnWrapOpenAdapter10)
    {
        HookD3D_IAT();
        g_bIATHooksInstalled = TRUE;
    }

    if (g_pfnWrapCreateFactory2)
    {
        Log("Routing through WRAPPER CreateDXGIFactory2...\n");
        g_reentrant++;
        HRESULT hr = g_pfnWrapCreateFactory2(Flags, riid, ppFactory, g_hProxy);
        g_reentrant--;
        Log("Wrapper CreateDXGIFactory2 returned 0x%08lX, ppFactory=%p\n", hr,
            ppFactory ? *ppFactory : NULL);
        LogFactoryDiagnostics(riid, ppFactory ? *ppFactory : NULL);
        return hr;
    }

    Log("Routing through REAL CreateDXGIFactory2 (no wrapper)\n");
    return g_pfnRealCreateFactory2(Flags, riid, ppFactory);
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
        MH_Initialize();  // kept for potential future use; no hooks created
        g_hVEH = AddVectoredExceptionHandler(1, VectoredCrashHandler);
        {
            WCHAR exePath[MAX_PATH];
            GetModuleFileNameW(NULL, exePath, MAX_PATH);
            Log("=== wiz3D " DISPLAYED_VERSION " - dxgi proxy loaded ===\n");
            Log("Game exe: %ls\n", exePath);
            WCHAR proxyPath[MAX_PATH];
            GetModuleFileNameW(hModule, proxyPath, MAX_PATH);
            Log("Proxy DLL: %ls\n", proxyPath);
        }
        // Check env var to disable DDI hooks (for crash diagnosis)
        {
            char envBuf[8] = {};
            if (GetEnvironmentVariableA("WIZ3D_NO_DDI_HOOK", envBuf, sizeof(envBuf)) > 0
                && envBuf[0] == '1')
            {
                g_bDDIHooksDisabled = TRUE;
                Log("*** WIZ3D_NO_DDI_HOOK=1 — DDI device wrapping DISABLED ***\n");
            }
        }
        // Pre-load real dxgi.dll so the wrapper's IAT resolution
        // for its own "dxgi.dll" imports finds the real one already loaded.
        LoadRealDXGI();
        break;

    case DLL_PROCESS_DETACH:
        Log("=== wiz3D dxgi proxy unloading ===\n");
        RestoreD3D_IAT();
        MH_Uninitialize();
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
        if (g_hRealDXGI)
        {
            FreeLibrary(g_hRealDXGI);
            g_hRealDXGI = NULL;
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
