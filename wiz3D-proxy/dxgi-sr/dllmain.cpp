/* wiz3D Simulated Reality -- dxgi.dll bootstrap proxy
 *
 * Drag-and-drop bootstrap for the wiz3D SR (Samsung Odyssey 3D native
 * stereo) path. Lives next to the game's exe. On startup Windows loads
 * this via standard DLL search BEFORE System32\dxgi.dll, so our DllMain
 * runs before the game's own init.
 *
 * What we do in DllMain:
 *   1. Load the real system dxgi.dll (System32), cache its handle, so
 *      every export we forward resolves to the real implementation.
 *   2. Make sure <game-exe-dir>\GameLauncherData\core\Odyssey3DSDK.dll
 *      exists -- the Samsung SDK handshake expects to LoadLibrary that
 *      specific path. We ship the subfolder structure in the release
 *      zip, but belt-and-braces: if the subdir is missing but the DLL
 *      is sitting at game-exe-dir\Odyssey3DSDK.dll, copy it into place.
 *   3. Write HKCU\SOFTWARE\Samsung\Odyssey3DHub\InstallPath = game-exe-dir
 *      so the Samsung-SDK-aware game appends \GameLauncherData\core\
 *      Odyssey3DSDK.dll and finds our stand-in.
 *   4. Return TRUE and let the game proceed. Export forwarding handles
 *      every subsequent DXGI call transparently.
 *
 * No installer. No admin rights (HKCU is user-level). No persistent
 * state outside the game dir (apart from the HKCU key, which we leave in
 * place; cheap, future game launches that don't want our stand-in just
 * don't ship with the subfolder/DLL present).
 *
 * Logging: see wiz3D_Logs\wiz3D_proxy.log next to the game exe. Lines
 * tagged "SR-boot" to distinguish from other wiz3D proxies.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dxgi1_4.h>
#include <stdio.h>
#include <stdarg.h>
#include "menu_squeeze.h"

// ---------------------------------------------------------------------------
// Logging -- shared log file wiz3D_proxy.log, same convention as every other
// wiz3D proxy so a multi-wrapper drop shares one log.
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
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(g_logFile, "[%02u:%02u:%02u.%03u] [SR-boot] ",
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_logFile, fmt, ap);
    va_end(ap);
    fflush(g_logFile);
}

// ---------------------------------------------------------------------------
// Real dxgi.dll loader (cached)
// ---------------------------------------------------------------------------
static HMODULE g_hRealDxgi = nullptr;

static BOOL LoadRealDxgi(void)
{
    if (g_hRealDxgi) return TRUE;
    WCHAR sysDir[MAX_PATH];
    GetSystemDirectoryW(sysDir, MAX_PATH);
    WCHAR path[MAX_PATH];
    wcscpy_s(path, sysDir);
    wcscat_s(path, L"\\dxgi.dll");
    g_hRealDxgi = LoadLibraryW(path);
    if (!g_hRealDxgi)
    {
        Log("FAIL: real dxgi.dll load (err=%lu)\n", GetLastError());
        return FALSE;
    }
    Log("OK: real dxgi.dll loaded from %ls\n", path);
    return TRUE;
}

// ---------------------------------------------------------------------------
// Lazy GetProcAddress helper for forwarders. Caches by name so each export
// is resolved once per process lifetime.
// ---------------------------------------------------------------------------
static FARPROC ResolveReal(const char* name)
{
    if (!g_hRealDxgi) LoadRealDxgi();
    if (!g_hRealDxgi) return nullptr;
    return GetProcAddress(g_hRealDxgi, name);
}

// ---------------------------------------------------------------------------
// Samsung SDK handshake setup.
// ---------------------------------------------------------------------------
static void GetGameExeDir(WCHAR out[MAX_PATH])
{
    GetModuleFileNameW(NULL, out, MAX_PATH);
    WCHAR* slash = wcsrchr(out, L'\\');
    if (slash) *(slash + 1) = L'\0';
}

static BOOL EnsureSamsungSubdirAndDLL(const WCHAR* gameExeDir)
{
    // Build <gameExeDir>GameLauncherData\core
    WCHAR subdir[MAX_PATH];
    wcscpy_s(subdir, gameExeDir);
    wcscat_s(subdir, L"GameLauncherData");
    CreateDirectoryW(subdir, NULL);    // ok if exists
    wcscat_s(subdir, L"\\core");
    CreateDirectoryW(subdir, NULL);

    WCHAR targetDll[MAX_PATH];
    wcscpy_s(targetDll, subdir);
    wcscat_s(targetDll, L"\\Odyssey3DSDK.dll");

    DWORD attrs = GetFileAttributesW(targetDll);
    if (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY))
    {
        Log("Samsung subdir+DLL already in place: %ls\n", targetDll);
        return TRUE;
    }

    // Belt-and-braces: if the DLL is sitting next to game.exe but not in the
    // Samsung subpath, copy it into place. This lets a user drop just two
    // files next to game.exe (dxgi.dll + Odyssey3DSDK.dll) and have the
    // bootstrap create the needed subpath on first launch.
    WCHAR sourceDll[MAX_PATH];
    wcscpy_s(sourceDll, gameExeDir);
    wcscat_s(sourceDll, L"Odyssey3DSDK.dll");
    if (GetFileAttributesW(sourceDll) == INVALID_FILE_ATTRIBUTES)
    {
        Log("WARN: no Odyssey3DSDK.dll at %ls (expected drag-and-drop payload)\n", sourceDll);
        return FALSE;
    }

    if (!CopyFileW(sourceDll, targetDll, FALSE))
    {
        Log("FAIL: CopyFileW %ls -> %ls (err=%lu)\n",
            sourceDll, targetDll, GetLastError());
        return FALSE;
    }
    Log("Copied Odyssey3DSDK.dll into Samsung subpath: %ls\n", targetDll);
    return TRUE;
}

// Writes CP2077's REDengine SBSDebug config fragment to
// <CP2077>\engine\config\platform\pc\braindance3d.ini
//
// Without this file, our Odyssey3DSDK stand-in makes the game show its
// hidden "3D Mode" option (the Samsung handshake succeeds), but at
// render-time the game routes final output through the Weaver -- which
// our stand-in no-ops under the "SBSDebug does its own output" assumption
// baked into the Samsung SDK. Result: menus render mono (don't go through
// the weaver) but in-game stereo is black (weaver never writes the
// output framebuffer). Logs from the user's first CP2077 test confirmed
// the game called Weaver::Weave at render time.
//
// SBSDebug=true tells REDengine to bypass the weaver and write half-SBS
// to its own backbuffer, which is the vl3c-tested path.
//
// Detection: CP2077 ships with <game>\engine\config\ already present
// (REDengine's static config tree). We confirm that directory exists
// before writing, so this is a no-op on games with a different layout
// -- future Samsung-SDK titles get the registry handshake without a
// CP2077-shaped ini.
static void WriteRedengineConfigIfCP2077(const WCHAR* gameExeDir)
{
    // gameExeDir is "<CP2077>\bin\x64\"; walk up two to reach <CP2077>\.
    WCHAR root[MAX_PATH];
    wcscpy_s(root, gameExeDir);
    size_t len = wcslen(root);
    if (len > 0 && root[len - 1] == L'\\') root[len - 1] = L'\0';
    WCHAR* slash = wcsrchr(root, L'\\');
    if (!slash) return;
    *slash = L'\0';                        // drops "\x64"
    slash = wcsrchr(root, L'\\');
    if (!slash) return;
    *slash = L'\0';                        // drops "\bin"
    // root is now "<CP2077>" (no trailing backslash)

    WCHAR engineCfgDir[MAX_PATH];
    swprintf_s(engineCfgDir, MAX_PATH, L"%s\\engine\\config", root);
    DWORD a = GetFileAttributesW(engineCfgDir);
    if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY))
    {
        Log("REDengine config dir not found at %ls; skipping ini drop (not CP2077?)\n",
            engineCfgDir);
        return;
    }

    WCHAR pcDir[MAX_PATH];
    swprintf_s(pcDir, MAX_PATH, L"%s\\platform",      engineCfgDir); CreateDirectoryW(pcDir, NULL);
    swprintf_s(pcDir, MAX_PATH, L"%s\\platform\\pc",  engineCfgDir); CreateDirectoryW(pcDir, NULL);

    WCHAR iniPath[MAX_PATH];
    swprintf_s(iniPath, MAX_PATH, L"%s\\braindance3d.ini", pcDir);

    // Verbatim fragment vl3c's package ships at the same path.
    // OverrideEnabled=false means the in-game "3D Mode" toggle (not just
    // the compile-time flag) controls stereo-on/off.
    const char* body =
        "; wiz3D Simulated Reality -- CP2077 REDengine SBSDebug flip.\r\n"
        "; Makes the game's 3D Mode render half-SBS to its own backbuffer\r\n"
        "; instead of handing the frame to the Samsung native weaver (which\r\n"
        "; our stand-in no-ops under this path, causing a black screen in-game).\r\n"
        "; Delete this file to undo.\r\n"
        "[Odyssey3D]\r\n"
        "OverrideEnabled = false\r\n"
        "StereoEnabled = true\r\n"
        "SBSDebug = true\r\n";

    HANDLE h = CreateFileW(iniPath, GENERIC_WRITE, 0, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
    {
        Log("FAIL: CreateFileW %ls (err=%lu)\n", iniPath, GetLastError());
        return;
    }
    DWORD written = 0;
    DWORD n = (DWORD)strlen(body);
    WriteFile(h, body, n, &written, NULL);
    CloseHandle(h);
    Log("Wrote CP2077 REDengine SBSDebug ini: %ls (%lu bytes)\n", iniPath, (unsigned long)written);
}

static void WriteSamsungRegistry(const WCHAR* gameExeDir)
{
    // HKCU\SOFTWARE\Samsung\Odyssey3DHub\InstallPath = <gameExeDir>
    // Trimming the trailing '\' because that's what Samsung's own installer writes.
    WCHAR trimmed[MAX_PATH];
    wcscpy_s(trimmed, gameExeDir);
    size_t len = wcslen(trimmed);
    if (len > 0 && trimmed[len - 1] == L'\\') trimmed[len - 1] = L'\0';

    HKEY hKey = nullptr;
    LSTATUS st = RegCreateKeyExW(HKEY_CURRENT_USER,
                                 L"SOFTWARE\\Samsung\\Odyssey3DHub",
                                 0, nullptr, 0, KEY_WRITE, nullptr, &hKey, nullptr);
    if (st != ERROR_SUCCESS)
    {
        Log("FAIL: RegCreateKeyExW HKCU\\SOFTWARE\\Samsung\\Odyssey3DHub (err=%ld)\n", (long)st);
        return;
    }
    DWORD bytes = (DWORD)((wcslen(trimmed) + 1) * sizeof(WCHAR));
    st = RegSetValueExW(hKey, L"InstallPath", 0, REG_SZ,
                        (const BYTE*)trimmed, bytes);
    RegCloseKey(hKey);
    if (st == ERROR_SUCCESS)
        Log("Wrote HKCU\\SOFTWARE\\Samsung\\Odyssey3DHub\\InstallPath = %ls\n", trimmed);
    else
        Log("FAIL: RegSetValueExW InstallPath (err=%ld)\n", (long)st);
}

// ---------------------------------------------------------------------------
// DXGI export forwarders. Three functions, all forward to real dxgi.dll.
// ---------------------------------------------------------------------------
typedef HRESULT (WINAPI* PFN_CreateDXGIFactory )(REFIID, void**);
typedef HRESULT (WINAPI* PFN_CreateDXGIFactory1)(REFIID, void**);
typedef HRESULT (WINAPI* PFN_CreateDXGIFactory2)(UINT, REFIID, void**);

// After each successful factory creation we hand it to the menu-squeeze
// module so it can vtable-patch CreateSwapChain* and later patch Present
// on the returned swap chain. ppFactory may be a derived IDXGIFactory2
// IID; menu_squeeze.OnFactoryCreated does QueryInterface internally.
static void NotifySqueezeOfFactory(void** ppFactory, REFIID riid)
{
    if (!ppFactory || !*ppFactory) return;
    IDXGIFactory* base = nullptr;
    if (SUCCEEDED(((IUnknown*)*ppFactory)->QueryInterface(IID_PPV_ARGS(&base))) && base) {
        wiz3d_sr::OnFactoryCreated(base);
        base->Release();
    }
    (void)riid;
}

// Export linkage is set by dxgi.def -- no __declspec(dllexport) here
// because the dxgi1_4.h declarations we include have default linkage
// and the two would conflict (C2375).
extern "C" HRESULT WINAPI CreateDXGIFactory(REFIID riid, void** ppFactory)
{
    static PFN_CreateDXGIFactory pfn = nullptr;
    if (!pfn) pfn = (PFN_CreateDXGIFactory)ResolveReal("CreateDXGIFactory");
    if (!pfn) return E_FAIL;
    HRESULT hr = pfn(riid, ppFactory);
    if (SUCCEEDED(hr)) NotifySqueezeOfFactory(ppFactory, riid);
    return hr;
}

extern "C" HRESULT WINAPI CreateDXGIFactory1(REFIID riid, void** ppFactory)
{
    static PFN_CreateDXGIFactory1 pfn = nullptr;
    if (!pfn) pfn = (PFN_CreateDXGIFactory1)ResolveReal("CreateDXGIFactory1");
    if (!pfn) return E_FAIL;
    HRESULT hr = pfn(riid, ppFactory);
    if (SUCCEEDED(hr)) NotifySqueezeOfFactory(ppFactory, riid);
    return hr;
}

extern "C" HRESULT WINAPI CreateDXGIFactory2(UINT flags, REFIID riid, void** ppFactory)
{
    static PFN_CreateDXGIFactory2 pfn = nullptr;
    if (!pfn) pfn = (PFN_CreateDXGIFactory2)ResolveReal("CreateDXGIFactory2");
    if (!pfn) return E_FAIL;
    HRESULT hr = pfn(flags, riid, ppFactory);
    if (SUCCEEDED(hr)) NotifySqueezeOfFactory(ppFactory, riid);
    return hr;
}

// ---------------------------------------------------------------------------
// DllMain -- the whole handshake runs here on DLL_PROCESS_ATTACH.
// ---------------------------------------------------------------------------
BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
{
    (void)hModule; (void)lpReserved;
    switch (ul_reason_for_call)
    {
        case DLL_PROCESS_ATTACH:
        {
            LogOpen();
            WCHAR exePath[MAX_PATH];
            GetModuleFileNameW(NULL, exePath, MAX_PATH);
            Log("================================================================\n");
            Log("wiz3D SR bootstrap (dxgi.dll) loaded by %ls\n", exePath);

            WCHAR gameDir[MAX_PATH];
            GetGameExeDir(gameDir);
            Log("Game dir resolved: %ls\n", gameDir);

            if (EnsureSamsungSubdirAndDLL(gameDir))
                WriteSamsungRegistry(gameDir);
            else
                Log("Skipping registry write -- Odyssey3DSDK.dll not deployable here.\n");

            // Drop the CP2077 REDengine SBSDebug ini if we're installed in a
            // CP2077 layout. No-op on games with a different directory shape.
            WriteRedengineConfigIfCP2077(gameDir);

            // Preload the real dxgi so first forward call doesn't incur the
            // library search cost mid-frame.
            LoadRealDxgi();

            // Initialise the menu-squeeze module -- opens its log, reads
            // WIZ3D_SR_MENU_SQUEEZE env, prepares the state. No D3D12 work
            // happens here; that's lazy on the first factory creation +
            // first flat frame.
            wiz3d_sr::MenuSqueezeInit();

            Log("Bootstrap complete; game startup continues.\n");
            Log("================================================================\n");
            break;
        }
        case DLL_PROCESS_DETACH:
            Log("wiz3D SR bootstrap (dxgi.dll) unloading\n");
            wiz3d_sr::MenuSqueezeShutdown();
            if (g_logFile) { fclose(g_logFile); g_logFile = nullptr; }
            break;
    }
    return TRUE;
}
