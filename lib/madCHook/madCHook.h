/**
 * madCHook.h - MadCHook API Compatibility Shim
 *
 * This header provides the MadCHook API surface used by the iZ3D codebase,
 * implemented on top of MinHook (https://github.com/TsudaKageyu/minhook).
 *
 * It allows all existing #include <madCHook.h> / "MadCHook.h" references
 * to compile without modifications to the consuming code.
 *
 * Part of the WiZ3D project.
 */
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <MinHook.h>
#include <string>
#include <vector>
#include <mutex>

// ---- MadCHook flag constants (used by iZ3D but ignored in MinHook shim) ----
// These flags controlled MadCHook-specific behavior that has no MinHook equivalent.
// We define them so existing code compiles; the values are ignored.
#ifndef NO_SAFE_UNHOOKING
#define NO_SAFE_UNHOOKING           0x01
#endif
#ifndef DONT_RENEW_OVERWRITTEN_HOOK
#define DONT_RENEW_OVERWRITTEN_HOOK 0x02
#endif
#ifndef RENEW_OVERWRITTEN_HOOKS
#define RENEW_OVERWRITTEN_HOOKS     (~DONT_RENEW_OVERWRITTEN_HOOK)
#endif
#ifndef SAFE_HOOKING
#define SAFE_HOOKING                0x00
#endif
#ifndef MIXTURE_MODE
#define MIXTURE_MODE                0x04
#endif

// ---- Internal tracking for HookAPI (module path -> target address) ----
namespace madCHookShim {

    // We need to track which PVOID* originals map to which target addresses,
    // so that UnhookAPI/UnhookCode can call MH_DisableHook + MH_RemoveHook.
    struct HookEntry {
        LPVOID pTarget;
        PVOID* ppOriginal;
    };

    inline std::vector<HookEntry>& GetHookEntries() {
        static std::vector<HookEntry> entries;
        return entries;
    }

    // MinHook fills the caller's "original" only on MH_OK, so remember trampolines ourselves.
    struct TrampolineEntry {
        LPVOID pTarget;
        LPVOID pTrampoline;
    };

    inline std::vector<TrampolineEntry>& GetTrampolines() {
        static std::vector<TrampolineEntry> tramps;
        return tramps;
    }

    inline std::mutex& GetMutex() {
        static std::mutex mtx;
        return mtx;
    }

    inline void RegisterHook(LPVOID pTarget, PVOID* ppOriginal) {
        std::lock_guard<std::mutex> lock(GetMutex());
        GetHookEntries().push_back({ pTarget, ppOriginal });
    }

    inline void RememberTrampoline(LPVOID pTarget, LPVOID pTrampoline) {
        std::lock_guard<std::mutex> lock(GetMutex());
        for (auto& e : GetTrampolines()) {
            if (e.pTarget == pTarget) {
                e.pTrampoline = pTrampoline;
                return;
            }
        }
        GetTrampolines().push_back({ pTarget, pTrampoline });
    }

    inline LPVOID FindTrampoline(LPVOID pTarget) {
        std::lock_guard<std::mutex> lock(GetMutex());
        for (auto& e : GetTrampolines()) {
            if (e.pTarget == pTarget)
                return e.pTrampoline;
        }
        return nullptr;
    }

    inline LPVOID FindTarget(PVOID* ppOriginal) {
        std::lock_guard<std::mutex> lock(GetMutex());
        for (auto& e : GetHookEntries()) {
            if (e.ppOriginal == ppOriginal)
                return e.pTarget;
        }
        return nullptr;
    }

    inline void RemoveEntry(PVOID* ppOriginal) {
        std::lock_guard<std::mutex> lock(GetMutex());
        auto& entries = GetHookEntries();
        for (auto it = entries.begin(); it != entries.end(); ++it) {
            if (it->ppOriginal == ppOriginal) {
                entries.erase(it);
                return;
            }
        }
    }

    // Convert ANSI module path to just the filename (wide), since
    // MH_CreateHookApi expects a loaded module name, not a full path.
    // MadCHook's HookAPI accepted full paths; MinHook wants the module name.
    inline std::wstring ExtractModuleName(const char* modulePath) {
        std::string path(modulePath);
        // Find the last backslash or forward slash
        auto pos = path.find_last_of("\\/");
        std::string name = (pos != std::string::npos) ? path.substr(pos + 1) : path;
        // Convert to wide string
        std::wstring wname(name.begin(), name.end());
        return wname;
    }

} // namespace madCHookShim

// ---- MadCHook API shims ----

/**
 * InitializeMadCHook - Initialize the hooking library.
 * Maps to MH_Initialize(). Safe to call multiple times (MinHook returns
 * MH_ERROR_ALREADY_INITIALIZED on subsequent calls, which we ignore).
 */
inline void InitializeMadCHook() {
    MH_Initialize();
}

/**
 * FinalizeMadCHook - Shut down the hooking library.
 * Disables all hooks and calls MH_Uninitialize().
 */
inline void FinalizeMadCHook() {
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
}

/**
 * HookAPI - Hook a named export from a DLL module.
 *
 * MadCHook signature:
 *   BOOL HookAPI(const char* modulePath, const char* funcName,
 *                void* callback, PVOID* original, DWORD flags);
 *
 * MinHook equivalent: MH_CreateHookApi + MH_EnableHook.
 *
 * NOTE: MadCHook accepted full file paths (e.g. "C:\Windows\System32\d3d9.dll").
 * MinHook's MH_CreateHookApi wants the module name as loaded (e.g. L"d3d9.dll").
 * The module must already be loaded for MinHook to find it.
 * If not loaded, we attempt LoadLibraryA first.
 */
inline BOOL HookAPI(const char* pszModule, const char* pszFuncName,
                    void* pDetour, PVOID* ppOriginal, DWORD /*flags*/ = 0)
{
    if (!pszModule || !pszFuncName || !pDetour || !ppOriginal)
        return FALSE;

    // Ensure the module is loaded (MadCHook loaded it if needed; MinHook requires it)
    HMODULE hMod = GetModuleHandleA(pszModule);
    if (!hMod) {
        // Try with just the filename
        std::wstring modName = madCHookShim::ExtractModuleName(pszModule);
        hMod = GetModuleHandleW(modName.c_str());
        if (!hMod) {
            // Load it
            hMod = LoadLibraryA(pszModule);
            if (!hMod)
                return FALSE;
        }
    }

    // Get the target function address for tracking
    FARPROC pTarget = GetProcAddress(hMod, pszFuncName);
    if (!pTarget)
        return FALSE;

    // Use MH_CreateHook with the resolved address (more reliable than MH_CreateHookApi
    // when the module was loaded via full path)
    MH_STATUS status = MH_CreateHook((LPVOID)pTarget, pDetour, ppOriginal);
    if (status == MH_OK) {
        madCHookShim::RememberTrampoline((LPVOID)pTarget, *ppOriginal);
    } else if (status == MH_ERROR_ALREADY_CREATED) {
        // See HookCode: MinHook does not fill *ppOriginal in this case.
        LPVOID pTrampoline = madCHookShim::FindTrampoline((LPVOID)pTarget);
        if (!pTrampoline)
            return FALSE;
        *ppOriginal = pTrampoline;
    } else {
        return FALSE;
    }

    // Track it so UnhookAPI can find the target
    madCHookShim::RegisterHook((LPVOID)pTarget, ppOriginal);

    // MadCHook hooks were active immediately; MinHook requires explicit enable
    status = MH_EnableHook((LPVOID)pTarget);
    return (status == MH_OK || status == MH_ERROR_ENABLED) ? TRUE : FALSE;
}

/**
 * UnhookAPI - Remove a previously installed API hook.
 *
 * MadCHook signature: BOOL UnhookAPI(PVOID* ppOriginal);
 *
 * IMPORTANT: We only MH_DisableHook (restore original bytes) but do NOT
 * call MH_RemoveHook. MH_RemoveHook frees the trampoline memory, which
 * would invalidate saved "original" function pointers (ppNextHook) that
 * callers may still use after unhooking. The iZ3D wrapper destructor
 * (CBaseStereoRenderer::~CBaseStereoRenderer) calls original device
 * methods AFTER UnhookDevice(), so trampolines must remain valid.
 * Trampolines are cleaned up at process exit by MH_Uninitialize.
 */
inline BOOL UnhookAPI(PVOID* ppOriginal)
{
    if (!ppOriginal)
        return FALSE;

    LPVOID pTarget = madCHookShim::FindTarget(ppOriginal);
    if (!pTarget)
        return FALSE;

    MH_STATUS status = MH_DisableHook(pTarget);
    // Do NOT call MH_RemoveHook — trampoline must remain valid.
    madCHookShim::RemoveEntry(ppOriginal);

    return (status == MH_OK) ? TRUE : FALSE;
}

/**
 * HookCode - Hook an arbitrary code address.
 *
 * MadCHook signature:
 *   BOOL HookCode(PVOID pCode, void* pCallback, PVOID* ppNextHook, DWORD flags);
 */
inline BOOL HookCode(PVOID pCode, void* pCallback, PVOID* ppNextHook, DWORD /*flags*/ = 0)
{
    if (!pCode || !pCallback || !ppNextHook)
        return FALSE;

    MH_STATUS status = MH_CreateHook(pCode, pCallback, ppNextHook);
    if (status == MH_OK) {
        madCHookShim::RememberTrampoline(pCode, *ppNextHook);
    } else if (status == MH_ERROR_ALREADY_CREATED) {
        // Several proxies hook one shared d3d9 function, so reuse the first trampoline.
        LPVOID pTrampoline = madCHookShim::FindTrampoline(pCode);
        if (!pTrampoline)
            return FALSE;
        *ppNextHook = pTrampoline;
    } else {
        return FALSE;
    }

    madCHookShim::RegisterHook(pCode, ppNextHook);

    status = MH_EnableHook(pCode);
    return (status == MH_OK || status == MH_ERROR_ENABLED) ? TRUE : FALSE;
}

/**
 * UnhookCode - Remove a code hook.
 *
 * MadCHook signature: BOOL UnhookCode(PVOID* ppNextHook);
 *
 * IMPORTANT: Same as UnhookAPI — only disable, do NOT remove/free
 * the trampoline. See UnhookAPI comment for full explanation.
 */
inline BOOL UnhookCode(PVOID* ppNextHook)
{
    if (!ppNextHook)
        return FALSE;

    LPVOID pTarget = madCHookShim::FindTarget(ppNextHook);
    if (!pTarget)
        return FALSE;

    MH_STATUS status = MH_DisableHook(pTarget);
    // Do NOT call MH_RemoveHook — trampoline must remain valid.
    madCHookShim::RemoveEntry(ppNextHook);

    return (status == MH_OK) ? TRUE : FALSE;
}

/**
 * CollectHooks - Begin batching hooks for a subsequent FlushHooks().
 *
 * MadCHook collected hooks between CollectHooks/FlushHooks and applied them
 * atomically. With MinHook, hooks are enabled immediately in HookAPI/HookCode,
 * so this is a no-op.
 */
inline void CollectHooks() {
    // No-op: MinHook enables hooks immediately
}

/**
 * FlushHooks - Apply any queued hooks.
 *
 * MadCHook had this to batch-apply hooks. With our shim, hooks are
 * enabled immediately, so this is effectively a no-op.
 * We call MH_ApplyQueued() just in case any were queued.
 */
inline void FlushHooks() {
    MH_ApplyQueued();
}

/**
 * SetMadCHookOption - Set a MadCHook-specific option.
 *
 * No MinHook equivalent; this is a no-op.
 */
inline void SetMadCHookOption(DWORD /*option*/, LPCVOID /*value*/) {
    // No-op: MadCHook-specific options have no MinHook equivalent
}

/**
 * OpenGlobalFileMapping - Open a named file mapping in the Global namespace.
 *
 * MadCHook utility: wraps OpenFileMappingA with "Global\" prefix for
 * cross-session shared memory access.
 */
inline HANDLE OpenGlobalFileMapping(const char* pszName, BOOL bInheritHandle)
{
    if (!pszName)
        return NULL;
    std::string globalName = std::string("Global\\") + pszName;
    return OpenFileMappingA(FILE_MAP_ALL_ACCESS, bInheritHandle, globalName.c_str());
}

/**
 * CreateGlobalFileMapping - Create a named file mapping in the Global namespace.
 *
 * MadCHook utility: wraps CreateFileMappingA with "Global\" prefix for
 * cross-session shared memory access.
 */
inline HANDLE CreateGlobalFileMapping(const char* pszName, DWORD dwSize)
{
    if (!pszName)
        return NULL;
    std::string globalName = std::string("Global\\") + pszName;
    return CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, dwSize, globalName.c_str());
}

/**
 * RenewHook - Re-patch a hook if the target was overwritten by another hooking library.
 *
 * MadCHook could detect and re-apply hooks. MinHook does not support this natively.
 * We attempt MH_EnableHook on the target, which is the closest equivalent.
 */
inline BOOL RenewHook(PVOID* ppOriginal)
{
    if (!ppOriginal)
        return FALSE;
    LPVOID pTarget = madCHookShim::FindTarget(ppOriginal);
    if (!pTarget)
        return FALSE;
    MH_STATUS status = MH_EnableHook(pTarget);
    return (status == MH_OK || status == MH_ERROR_ENABLED) ? TRUE : FALSE;
}

// ---- MadCHook driver injection constants ----
// These constants controlled MadCHook's kernel-mode DLL injection driver.
// With MinHook (user-mode only), the driver APIs are stubs.
#ifndef ALL_SESSIONS
#define ALL_SESSIONS            ((DWORD)0xFFFFFFFF)
#endif
#ifndef SYSTEM_PROCESSES
#define SYSTEM_PROCESSES        ((DWORD)0x10)
#endif
#ifndef DONT_TOUCH_RUNNING_PROCESSES
#define DONT_TOUCH_RUNNING_PROCESSES 0x20
#endif

/**
 * LoadInjectionDriver - Load the MadCHook kernel injection driver.
 *
 * No MinHook equivalent. Stub returns FALSE (driver not available).
 */
inline BOOL LoadInjectionDriver(const wchar_t* /*driverName*/,
                                const wchar_t* /*driverPath*/,
                                const wchar_t* /*description*/)
{
    return FALSE;
}

/**
 * StartInjectionDriver - Start the already-installed MadCHook injection driver.
 *
 * No MinHook equivalent. Stub returns FALSE.
 */
inline BOOL StartInjectionDriver(const wchar_t* /*driverName*/)
{
    return FALSE;
}

/**
 * StopInjectionDriver - Stop the MadCHook injection driver.
 *
 * No MinHook equivalent. Stub returns FALSE.
 */
inline BOOL StopInjectionDriver(const wchar_t* /*driverName*/)
{
    return FALSE;
}

/**
 * InstallInjectionDriver - Install and load the MadCHook kernel injection driver.
 *
 * No MinHook equivalent. Stub returns FALSE.
 */
inline BOOL InstallInjectionDriver(const wchar_t* /*driverName*/,
                                   const wchar_t* /*driverPath*/,
                                   const wchar_t* /*description*/)
{
    return FALSE;
}

/**
 * InjectLibrary - Inject a DLL into processes via the kernel driver.
 *
 * MadCHook signature:
 *   BOOL InjectLibrary(LPCWSTR driverName, LPCWSTR dllPath,
 *                      DWORD session, BOOL systemProcesses,
 *                      LPCWSTR includeList, LPCWSTR excludeList);
 *
 * No MinHook equivalent. Stub returns FALSE.
 */
inline BOOL InjectLibrary(const wchar_t* /*driverName*/,
                          const wchar_t* /*dllPath*/,
                          DWORD /*session*/,
                          BOOL /*systemProcesses*/,
                          const wchar_t* /*includeList*/,
                          const wchar_t* /*excludeList*/)
{
    return FALSE;
}

/**
 * UninjectLibrary - Remove a previously injected DLL via the kernel driver.
 *
 * No MinHook equivalent. Stub returns FALSE.
 */
inline BOOL UninjectLibrary(const wchar_t* /*driverName*/,
                            const wchar_t* /*dllPath*/,
                            DWORD /*session*/,
                            BOOL /*systemProcesses*/,
                            const wchar_t* /*includeList*/,
                            const wchar_t* /*excludeList*/)
{
    return FALSE;
}
