// NvApiProxy.cpp
// Fake nvapi.dll (x86) / nvapi64.dll (x64)
//
// Wraps NVIDIA's NVAPI surface so that 3D Vision-aware games (e.g.
// Batman: Arkham Asylum) believe they are running on a 3D Vision-capable
// NVIDIA GPU.  Lets such games enable their built-in stereo render path
// on AMD/Intel hardware, which wiz3D's d3d9 wrapper then converts to the
// configured output (anaglyph, SBS, SR weave, etc.).
//
// Two operating modes, auto-detected at first call:
//   PASSTHROUGH: real NVIDIA NVAPI is present in System32 AND succeeds at
//     NvAPI_Initialize.  Forward every call to it untouched.  This keeps
//     us harmless on real NVIDIA systems and avoids breaking anyone's
//     genuine 3D Vision setup.
//   SPOOF: real NVAPI absent or failing.  Answer the game's calls with
//     fake "NVIDIA RTX 2080 Ti running driver 426.06" responses.  Stereo
//     APIs all succeed but no-op since wiz3D handles the actual stereo
//     downstream of D3D.
//
// NVAPI exposes its entire surface through a single export,
//   void* nvapi_QueryInterface(NvU32 functionId);
// which returns __cdecl function pointers indexed by 32-bit IDs.  Function
// IDs and signatures are taken from the MIT-licensed NVAPI 2026 SDK
// headers in lib/nvapi_2026/.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "..\..\Shared\version.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ============================================================
// Local NVAPI types (subset; signatures must match real NVAPI).
// Pulled from lib/nvapi_2026/{nvapi_lite_common.h, nvapi_lite_stereo.h, nvapi.h}
// rather than including the full headers — keeps this DLL self-contained
// and avoids the SAL annotation / version-macro machinery we don't need.
// ============================================================
typedef int                NvAPI_Status;
typedef unsigned int       NvU32;
typedef unsigned char      NvU8;
typedef unsigned __int64   NvU64;
typedef void*              StereoHandle;
typedef void*              NvPhysicalGpuHandle;
typedef void*              NvLogicalGpuHandle;
typedef void*              NvDisplayHandle;

#define NVAPI_OK                     0
#define NVAPI_ERROR                 -1
#define NVAPI_END_ENUMERATION       -7
#define NVAPI_NOT_SUPPORTED       -104

#define NVAPI_SHORT_STRING_MAX     64
#define NVAPI_MAX_PHYSICAL_GPUS    64
typedef char NvAPI_ShortString[NVAPI_SHORT_STRING_MAX];

#define MAKE_NVAPI_VERSION(t, v) ((NvU32)(sizeof(t) | ((v) << 16)))

typedef struct _NV_DISPLAY_DRIVER_VERSION
{
    NvU32              version;
    NvU32              drvVersion;
    NvU32              bldChangeListNum;
    NvAPI_ShortString  szBuildBranchString;
    NvAPI_ShortString  szAdapterString;
} NV_DISPLAY_DRIVER_VERSION;
#define NV_DISPLAY_DRIVER_VERSION_VER  MAKE_NVAPI_VERSION(NV_DISPLAY_DRIVER_VERSION, 1)

typedef enum {
    NV_SYSTEM_TYPE_UNKNOWN = 0,
    NV_SYSTEM_TYPE_LAPTOP  = 1,
    NV_SYSTEM_TYPE_DESKTOP = 2,
} NV_SYSTEM_TYPE;

typedef enum {
    NV_GPU_TYPE_UNKNOWN = 0,
    NV_GPU_TYPE_IGPU    = 1,
    NV_GPU_TYPE_DGPU    = 2,
} NV_GPU_TYPE;

typedef enum {
    NVAPI_STEREO_EYE_RIGHT = 1,
    NVAPI_STEREO_EYE_LEFT  = 2,
    NVAPI_STEREO_EYE_MONO  = 3,
} NV_STEREO_ACTIVE_EYE;

typedef enum {
    NVAPI_STEREO_DRIVER_MODE_AUTOMATIC = 0,
    NVAPI_STEREO_DRIVER_MODE_DIRECT    = 2,
} NV_STEREO_DRIVER_MODE;

typedef enum {
    NVAPI_STEREO_SURFACECREATEMODE_AUTO        = 0,
    NVAPI_STEREO_SURFACECREATEMODE_FORCESTEREO = 1,
    NVAPI_STEREO_SURFACECREATEMODE_FORCEMONO   = 2,
} NVAPI_STEREO_SURFACECREATEMODE;

#define NVAPI_INTERFACE NvAPI_Status __cdecl

// ============================================================
// Diagnostic log (next to the DLL, i.e. in the game folder)
// ============================================================
static HMODULE g_hSelf = nullptr;

static void WriteLog(const char* msg)
{
    wchar_t dir[MAX_PATH] = {};
    if (g_hSelf)
    {
        GetModuleFileNameW(g_hSelf, dir, MAX_PATH);
        wchar_t* p = wcsrchr(dir, L'\\');
        if (p) p[1] = L'\0';
    }
    if (!dir[0] && !GetTempPathW(MAX_PATH, dir)) return;

    wchar_t path[MAX_PATH];
    wcscpy_s(path, dir);
    wcscat_s(path, L"NvApiProxy.log");
    HANDLE h = CreateFileW(path, FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD written;
    WriteFile(h, msg, static_cast<DWORD>(strlen(msg)), &written, nullptr);
    CloseHandle(h);
}

// ============================================================
// Spoofed identity
//   RTX 2080 Ti — last NVIDIA card to ship with official 3D Vision support.
//   Driver 426.06 — last 3D Vision-aware NVIDIA driver (beta).  Some games
//   bail on driver versions below ~418, and 426.06 satisfies all known
//   3D Vision title minimums while still being recognisably "of the era".
// ============================================================
static const char* kSpoofGpuName       = "NVIDIA GeForce RTX 2080 Ti";
static const char* kSpoofBranchStr     = "r426_00-100";
static const NvU32 kSpoofDrvVersion    = 42606;       // major*100 + minor (NVAPI convention)
static const NvU32 kSpoofBldChangeNum  = 25739267;
static const NvU32 kSpoofPciVendor     = 0x10DE;       // NVIDIA
static const NvU32 kSpoofPciDevice     = 0x1E07;       // RTX 2080 Ti (TU102)
static const NvU32 kSpoofPciSubVendor  = 0x10DE;
static const NvU32 kSpoofPciSubDevice  = 0x12FF;
static const NvU32 kSpoofPciRevision   = 0xA1;

static const NvPhysicalGpuHandle kFakePhysicalGpu = (NvPhysicalGpuHandle)0xFA1E0001ULL;
static const NvLogicalGpuHandle  kFakeLogicalGpu  = (NvLogicalGpuHandle) 0xFA1E1001ULL;
static const NvDisplayHandle     kFakeDisplay     = (NvDisplayHandle)    0xFA1ED15AULL;
typedef void* NvUnAttachedDisplayHandle;

// ============================================================
// Mode detection: passthrough to real NVIDIA NVAPI, or spoof?
// Lazy-initialised on first nvapi_QueryInterface call (DllMain is too
// early to LoadLibrary safely).
// ============================================================
typedef void* (__cdecl *PFN_nvapi_QueryInterface)(NvU32 id);

static bool                     g_bInitialised   = false;
static bool                     g_bPassthrough   = false;
static HMODULE                  g_hRealNvapi     = nullptr;
static PFN_nvapi_QueryInterface g_pfnRealQI      = nullptr;

static void InitMode()
{
    if (g_bInitialised) return;
    g_bInitialised = true;

    wchar_t sysDir[MAX_PATH] = {};
    GetSystemDirectoryW(sysDir, MAX_PATH);

    wchar_t path[MAX_PATH];
    wcscpy_s(path, sysDir);
#ifdef _WIN64
    wcscat_s(path, L"\\nvapi64.dll");
#else
    // 32-bit processes get File System Redirection: "System32\nvapi.dll"
    // resolves to SysWOW64\nvapi.dll automatically.
    wcscat_s(path, L"\\nvapi.dll");
#endif

    g_hRealNvapi = LoadLibraryW(path);
    if (!g_hRealNvapi)
    {
        WriteLog("[NvApiProxy] no real NVAPI on system -> SPOOF mode\n");
        return;
    }

    g_pfnRealQI = (PFN_nvapi_QueryInterface)
        GetProcAddress(g_hRealNvapi, "nvapi_QueryInterface");
    if (!g_pfnRealQI)
    {
        FreeLibrary(g_hRealNvapi);
        g_hRealNvapi = nullptr;
        WriteLog("[NvApiProxy] real NVAPI loaded but no QueryInterface -> SPOOF mode\n");
        return;
    }

    // Verify the real NVAPI actually works (handles orphaned nvapi.dll left
    // behind by an uninstalled NVIDIA driver, or a non-functional driver).
    typedef NvAPI_Status (__cdecl *PFN_Initialize)(void);
    PFN_Initialize pfnInit = (PFN_Initialize)g_pfnRealQI(0x0150E828);
    if (!pfnInit || pfnInit() != NVAPI_OK)
    {
        FreeLibrary(g_hRealNvapi);
        g_hRealNvapi = nullptr;
        g_pfnRealQI  = nullptr;
        WriteLog("[NvApiProxy] real NvAPI_Initialize failed -> SPOOF mode\n");
        return;
    }

    g_bPassthrough = true;
    WriteLog("[NvApiProxy] real NVIDIA driver detected -> PASSTHROUGH mode (Stereo_* still served by wiz3D bridge)\n");
}

// ============================================================
// Spoofed implementations
// ============================================================
//
// Per-handle state isn't tracked; one global block suffices because wiz3D
// performs the actual stereoization downstream regardless of what the game
// asks for here.  If a real-world game proves to need per-device isolation
// (separate IPD/convergence per swapchain), promote this to a small map.
struct FakeStereoState
{
    NvU8                            isActive;
    float                           separation;     // 0..100 percent
    float                           convergence;
    NV_STEREO_ACTIVE_EYE            activeEye;
    NV_STEREO_DRIVER_MODE           driverMode;
    NVAPI_STEREO_SURFACECREATEMODE  createMode;
    NvU32                           frustumAdjust;
};

static FakeStereoState g_Stereo = {
    /*isActive*/      1,
    /*separation*/    15.0f,
    /*convergence*/   0.5f,
    /*activeEye*/     NVAPI_STEREO_EYE_MONO,    // game hasn't called SetActiveEye — wrapper interprets as mono
    /*driverMode*/    NVAPI_STEREO_DRIVER_MODE_AUTOMATIC,
    /*createMode*/    NVAPI_STEREO_SURFACECREATEMODE_AUTO,
    /*frustumAdjust*/ 1,
};

// ============================================================
// Optional config flag: NvDirectMode's 3DVision_Config.xml lives in the
// game folder alongside this DLL. We read DisableStereoSpoof at
// DLL_PROCESS_ATTACH so the spoof can be toggled per-game without
// recompiling. Default 0 = spoof stereo as enabled (the historical
// behaviour). Setting 1 makes Stereo_IsEnabled / Stereo_IsActivated
// report FALSE so the game treats stereo as unavailable — useful as a
// diagnostic ("does my game work without our wrap claiming stereo?").
// ============================================================
static int g_disableStereoSpoof = 0;

// ============================================================
// Stereo_SetNotificationMessage state. NVAPI lets a game register
// (HWND, messageId); the driver PostMessage()s that message whenever
// stereo activation state changes externally (e.g. user pressed Ctrl+T
// on a real 3D Vision rig, or in our case the wiz3D '*' hotkey). The
// game's WndProc then updates its in-UI 3D toggle.
//
// We piggy-back on the game's own Stereo_IsActivated polling cadence
// rather than spinning up a watcher thread: whenever IsActivated would
// return a different value from the last one we reported, we PostMessage
// after returning. Latency is bounded by the game's poll interval (60
// frames in Alice TMR, fast enough to feel responsive). Trade-off
// vs. a thread/callback: zero extra code in the wrapper, no new
// inter-DLL surface, no thread-safety hazards.
//
// Edge case: when the game itself calls Stereo_Activate/Deactivate, the
// next IsActivated will see a different value vs. lastReported and
// fire a notification with wParam = new state. That's the canonical
// NVAPI behaviour too (drivers always notify, even on self-initiated
// changes — handlers are expected to be idempotent).
// ============================================================
static HWND        g_notifyHwnd        = nullptr;
static UINT        g_notifyMsg         = 0;
static int         g_lastReportedActive = -1; // -1 = haven't reported yet

static int ReadConfigInt(const char* xml, const char* tag, int defaultValue)
{
    char needle[64];
    _snprintf_s(needle, sizeof(needle), _TRUNCATE, "<%s Value=\"", tag);
    const char* p = strstr(xml, needle);
    if (!p) return defaultValue;
    p += strlen(needle);
    return atoi(p);
}

static void LoadConfig(HMODULE hSelf)
{
    WCHAR cfgPath[MAX_PATH];
    if (!GetModuleFileNameW(hSelf, cfgPath, MAX_PATH)) return;
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

    g_disableStereoSpoof = ReadConfigInt(buf, "DisableStereoSpoof", g_disableStereoSpoof);
    free(buf);
}

// ============================================================
// Live bridge to S3DWrapperD3D9.dll's wiz3D state.
//
// When the DX9 wrapper is loaded in-process, route stereo getters/setters
// through it so 3D Vision-aware games (Skyrim, Witcher 2, etc.) and HelixMod
// see the user's current wiz3D separation/convergence and writes propagate
// back to the wrapper. When the wrapper isn't loaded - e.g. the proxy is
// being used standalone, or before the device is created - we transparently
// fall back to the local FakeStereoState above.
//
// Resolution strategy: cheap GetModuleHandle on every call, so if the
// wrapper appears mid-session (lazy load by Direct3DCreate9) we pick it up
// without a restart.
// ============================================================
struct Wiz3DBridge
{
    int   (__cdecl* GetStereoActive)();
    void  (__cdecl* SetStereoActive)(int);
    float (__cdecl* GetSeparationPercent)();
    void  (__cdecl* SetSeparationPercent)(float);
    float (__cdecl* GetConvergence)();
    void  (__cdecl* SetConvergence)(float);
    void  (__cdecl* StepSeparation)(int);
    void  (__cdecl* StepConvergence)(int);
    int   (__cdecl* HasProfileEntry)();
};
static Wiz3DBridge g_Wiz3D = {};
static HMODULE     g_Wiz3DModule = nullptr;

// Profile-aware Set gating. When a Base/Community/User profile entry
// matched the running game, profile values should win over game-side
// startup defaults. We distinguish "game init Set" from "user-adjusted
// Set" by tracking whether the game has called Get yet — in-game UI
// widgets always Get-then-Set (to position their slider), init code
// just blindly Sets hardcoded defaults.
static bool g_gameHasReadSeparation = false;
static bool g_gameHasReadConvergence = false;
static bool g_gameHasReadActive = false;

// Decide whether to apply this Set to wiz3D state. Logic:
//   has_profile && !user_engaged → ignore (profile wins)
//   !has_profile && !user_engaged → apply (no better info, game's default
//                                          becomes our initial value;
//                                          don't persist yet, wait for
//                                          an actual user interaction)
//   user_engaged → apply + persist (user adjustment, always honor)
//
// "Persist" means call the bridge's Set (which writes UserProfile.xml).
// The "no profile + init" branch updates the bridge state in a way that
// affects current-session rendering but the bridge's WriteInputData
// happens anyway. Per design: rather than complicate the bridge with
// a "don't persist this one" path, we accept that the first init Set
// for an unprofiled game DOES get saved, which is fine — it's the
// best baseline we have.
static bool ShouldApplyGameSet(bool& userEngagedFlag)
{
    if (userEngagedFlag) return true;
    return g_Wiz3D.HasProfileEntry ? (g_Wiz3D.HasProfileEntry() == 0) : true;
}

static bool ResolveWiz3DBridge()
{
    HMODULE m = GetModuleHandleA("S3DWrapperD3D9.dll");
    if (!m)
    {
        if (g_Wiz3DModule)
        {
            // Wrapper was unloaded - drop pointers
            memset(&g_Wiz3D, 0, sizeof(g_Wiz3D));
            g_Wiz3DModule = nullptr;
            g_gameHasReadSeparation = g_gameHasReadConvergence = g_gameHasReadActive = false;
        }
        return false;
    }
    if (m != g_Wiz3DModule)
    {
        g_Wiz3D.GetStereoActive      = (int   (__cdecl*)())   GetProcAddress(m, "Wiz3D_GetStereoActive");
        g_Wiz3D.SetStereoActive      = (void  (__cdecl*)(int))GetProcAddress(m, "Wiz3D_SetStereoActive");
        g_Wiz3D.GetSeparationPercent = (float (__cdecl*)())   GetProcAddress(m, "Wiz3D_GetSeparationPercent");
        g_Wiz3D.SetSeparationPercent = (void  (__cdecl*)(float))GetProcAddress(m, "Wiz3D_SetSeparationPercent");
        g_Wiz3D.GetConvergence       = (float (__cdecl*)())   GetProcAddress(m, "Wiz3D_GetConvergence");
        g_Wiz3D.SetConvergence       = (void  (__cdecl*)(float))GetProcAddress(m, "Wiz3D_SetConvergence");
        g_Wiz3D.StepSeparation       = (void  (__cdecl*)(int))GetProcAddress(m, "Wiz3D_StepSeparation");
        g_Wiz3D.StepConvergence      = (void  (__cdecl*)(int))GetProcAddress(m, "Wiz3D_StepConvergence");
        g_Wiz3D.HasProfileEntry      = (int   (__cdecl*)())   GetProcAddress(m, "Wiz3D_HasProfileEntry");
        g_Wiz3DModule = m;
    }
    return g_Wiz3D.GetSeparationPercent != nullptr;
}

// --- general / driver -------------------------------------------------------
// Diagnostic: log the *first* time each spoof function is hit so we can see
// exactly which NvAPI calls a game / mod is making, in order.
//
// For stereo Get* / Set* functions we additionally need to see:
//   - polling cadence (does the game read separation per-frame? once per scene?)
//   - the actual value being returned / written (so we can verify the wiz3D
//     bridge is feeding through correctly when the user moves a slider in
//     game or in our config)
//
// NVAPI_TRACE_PERIODIC: every Nth call, includes a printf-style value snapshot.
// NVAPI_TRACE_EVERY:    every call, includes a value. For rare Set functions
//                       where every event is interesting (user pressed an
//                       in-game stereo hotkey).
//
// Cheap on the hot path: one interlocked increment + a modulo + branch.
//
// Disable by setting WIZ3D_NVAPI_TRACE=0 if logs ever get too noisy.
#define WIZ3D_NVAPI_TRACE 1
#define WIZ3D_NVAPI_PERIODIC_N 60
#if WIZ3D_NVAPI_TRACE
  #define NVAPI_TRACE_FIRST(name)                                          \
      do {                                                                 \
          static volatile LONG _once = 0;                                  \
          if (InterlockedCompareExchange(&_once, 1, 0) == 0)               \
              WriteLog("[NvApiProxy] first call: " name "\n");             \
      } while (0)

  // Logs call #1, then every Nth call. snprintf into a stack buffer; format
  // string must NOT use floats from outside the macro arg list (we wrap them).
  #define NVAPI_TRACE_PERIODIC(name, fmt, ...)                             \
      do {                                                                 \
          static volatile LONG _count = 0;                                  \
          LONG _c = InterlockedIncrement(&_count);                         \
          if (_c == 1 || (_c % WIZ3D_NVAPI_PERIODIC_N) == 0) {             \
              char _buf[160];                                              \
              _snprintf_s(_buf, sizeof(_buf), _TRUNCATE,                   \
                  "[NvApiProxy] " name " #%ld: " fmt "\n", _c, __VA_ARGS__);\
              WriteLog(_buf);                                              \
          }                                                                \
      } while (0)

  // Every call logged with the value. Use sparingly - only for rare events.
  #define NVAPI_TRACE_EVERY(name, fmt, ...)                                \
      do {                                                                 \
          char _buf[160];                                                  \
          _snprintf_s(_buf, sizeof(_buf), _TRUNCATE,                       \
              "[NvApiProxy] " name ": " fmt "\n", __VA_ARGS__);            \
          WriteLog(_buf);                                                  \
      } while (0)
#else
  #define NVAPI_TRACE_FIRST(name)                       ((void)0)
  #define NVAPI_TRACE_PERIODIC(name, fmt, ...)          ((void)0)
  #define NVAPI_TRACE_EVERY(name, fmt, ...)             ((void)0)
#endif

NVAPI_INTERFACE Spoof_Initialize(void) { NVAPI_TRACE_FIRST("Initialize"); return NVAPI_OK; }
NVAPI_INTERFACE Spoof_Unload(void)     { NVAPI_TRACE_FIRST("Unload"); return NVAPI_OK; }

NVAPI_INTERFACE Spoof_GetErrorMessage(NvAPI_Status nr, NvAPI_ShortString szDesc)
{
    NVAPI_TRACE_FIRST("GetErrorMessage");
    if (!szDesc) return NVAPI_ERROR;
    _snprintf_s(szDesc, NVAPI_SHORT_STRING_MAX, _TRUNCATE,
                "NvApiProxy: status %d", (int)nr);
    return NVAPI_OK;
}

NVAPI_INTERFACE Spoof_GetInterfaceVersionString(NvAPI_ShortString szDesc)
{
    NVAPI_TRACE_FIRST("GetInterfaceVersionString");
    if (!szDesc) return NVAPI_ERROR;
    strcpy_s(szDesc, NVAPI_SHORT_STRING_MAX, kSpoofBranchStr);
    return NVAPI_OK;
}

NVAPI_INTERFACE Spoof_SYS_GetDriverAndBranchVersion(NvU32* pDrv, NvAPI_ShortString szBranch)
{
    NVAPI_TRACE_FIRST("SYS_GetDriverAndBranchVersion");
    if (!pDrv || !szBranch) return NVAPI_ERROR;
    *pDrv = kSpoofDrvVersion;
    strcpy_s(szBranch, NVAPI_SHORT_STRING_MAX, kSpoofBranchStr);
    return NVAPI_OK;
}

NVAPI_INTERFACE Spoof_GetDisplayDriverVersion(NvDisplayHandle, NV_DISPLAY_DRIVER_VERSION* pVer)
{
    NVAPI_TRACE_FIRST("GetDisplayDriverVersion");
    if (!pVer) return NVAPI_ERROR;
    pVer->version          = NV_DISPLAY_DRIVER_VERSION_VER;
    pVer->drvVersion       = kSpoofDrvVersion;
    pVer->bldChangeListNum = kSpoofBldChangeNum;
    strcpy_s(pVer->szBuildBranchString, NVAPI_SHORT_STRING_MAX, kSpoofBranchStr);
    strcpy_s(pVer->szAdapterString,     NVAPI_SHORT_STRING_MAX, kSpoofGpuName);
    return NVAPI_OK;
}

// --- GPU enumeration --------------------------------------------------------
NVAPI_INTERFACE Spoof_EnumPhysicalGPUs(NvPhysicalGpuHandle h[NVAPI_MAX_PHYSICAL_GPUS], NvU32* pCount)
{
    NVAPI_TRACE_FIRST("EnumPhysicalGPUs");
    if (!h || !pCount) return NVAPI_ERROR;
    h[0] = kFakePhysicalGpu;
    for (int i = 1; i < NVAPI_MAX_PHYSICAL_GPUS; ++i) h[i] = nullptr;
    *pCount = 1;
    return NVAPI_OK;
}

NVAPI_INTERFACE Spoof_EnumLogicalGPUs(NvLogicalGpuHandle h[NVAPI_MAX_PHYSICAL_GPUS], NvU32* pCount)
{
    NVAPI_TRACE_FIRST("EnumLogicalGPUs");
    if (!h || !pCount) return NVAPI_ERROR;
    h[0] = kFakeLogicalGpu;
    for (int i = 1; i < NVAPI_MAX_PHYSICAL_GPUS; ++i) h[i] = nullptr;
    *pCount = 1;
    return NVAPI_OK;
}

NVAPI_INTERFACE Spoof_GetPhysicalGPUsFromDisplay(NvDisplayHandle, NvPhysicalGpuHandle h[NVAPI_MAX_PHYSICAL_GPUS], NvU32* pCount)
{
    NVAPI_TRACE_FIRST("GetPhysicalGPUsFromDisplay");
    return Spoof_EnumPhysicalGPUs(h, pCount);
}

// --- display enumeration (3D Vision games iterate displays before stereo setup)
NVAPI_INTERFACE Spoof_EnumNvidiaDisplayHandle(NvU32 thisEnum, NvDisplayHandle* pHandle)
{
    NVAPI_TRACE_FIRST("EnumNvidiaDisplayHandle");
    if (!pHandle) return NVAPI_ERROR;
    if (thisEnum == 0)
    {
        *pHandle = kFakeDisplay;
        return NVAPI_OK;
    }
    return NVAPI_END_ENUMERATION;
}

NVAPI_INTERFACE Spoof_EnumNvidiaUnAttachedDisplayHandle(NvU32, NvUnAttachedDisplayHandle* pHandle)
{
    NVAPI_TRACE_FIRST("EnumNvidiaUnAttachedDisplayHandle");
    if (pHandle) *pHandle = nullptr;
    return NVAPI_END_ENUMERATION;
}

NVAPI_INTERFACE Spoof_GetAssociatedNvidiaDisplayHandle(const char* /*szDisplayName*/, NvDisplayHandle* pHandle)
{
    NVAPI_TRACE_FIRST("GetAssociatedNvidiaDisplayHandle");
    if (!pHandle) return NVAPI_ERROR;
    *pHandle = kFakeDisplay;
    return NVAPI_OK;
}

NVAPI_INTERFACE Spoof_GetAssociatedDisplayOutputId(NvDisplayHandle, NvU32* pOutputId)
{
    NVAPI_TRACE_FIRST("GetAssociatedDisplayOutputId");
    if (!pOutputId) return NVAPI_ERROR;
    *pOutputId = 0x00000001;
    return NVAPI_OK;
}

NVAPI_INTERFACE Spoof_GPU_GetFullName(NvPhysicalGpuHandle, NvAPI_ShortString szName)
{
    NVAPI_TRACE_FIRST("GPU_GetFullName");
    if (!szName) return NVAPI_ERROR;
    strcpy_s(szName, NVAPI_SHORT_STRING_MAX, kSpoofGpuName);
    return NVAPI_OK;
}

NVAPI_INTERFACE Spoof_GPU_GetSystemType(NvPhysicalGpuHandle, NV_SYSTEM_TYPE* p)
{
    NVAPI_TRACE_FIRST("GPU_GetSystemType");
    if (!p) return NVAPI_ERROR;
    *p = NV_SYSTEM_TYPE_DESKTOP;
    return NVAPI_OK;
}

NVAPI_INTERFACE Spoof_GPU_GetGPUType(NvPhysicalGpuHandle, NV_GPU_TYPE* p)
{
    NVAPI_TRACE_FIRST("GPU_GetGPUType");
    if (!p) return NVAPI_ERROR;
    *p = NV_GPU_TYPE_DGPU;
    return NVAPI_OK;
}

NVAPI_INTERFACE Spoof_GPU_GetPCIIdentifiers(NvPhysicalGpuHandle,
    NvU32* pDeviceId, NvU32* pSubsystemId, NvU32* pRevisionId, NvU32* pExtDeviceId)
{
    NVAPI_TRACE_FIRST("GPU_GetPCIIdentifiers");
    if (pDeviceId)    *pDeviceId    = (kSpoofPciDevice    << 16) | kSpoofPciVendor;
    if (pSubsystemId) *pSubsystemId = (kSpoofPciSubDevice << 16) | kSpoofPciSubVendor;
    if (pRevisionId)  *pRevisionId  = kSpoofPciRevision;
    if (pExtDeviceId) *pExtDeviceId = kSpoofPciDevice;
    return NVAPI_OK;
}

// CUDA core count for the spoofed RTX 2080 Ti (TU102 = 4352 cores). Some
// engines (Max Payne 3, EGO engine games confirmed 2026-05-10) query this
// to scale shader workloads. On real NVIDIA hardware passthrough handles
// it; on AMD/Intel without this we'd hit the default OkNoOp arm and games
// would see 0 cores → potentially disable shader optimizations or bail.
NVAPI_INTERFACE Spoof_GPU_GetGpuCoreCount(NvPhysicalGpuHandle, NvU32* pCount)
{
    NVAPI_TRACE_FIRST("GPU_GetGpuCoreCount");
    if (!pCount) return NVAPI_ERROR;
    *pCount = 4352;  // TU102 — matches our spoofed RTX 2080 Ti identity
    return NVAPI_OK;
}

// NV depth-bounds-test extension — Tomb Raider 2013 and Hitman Absolution
// both call this for optimization. AMD GPUs have similar functionality
// via a different path; the game doesn't know that and just expects this
// call to "work". Returning OK with no actual hardware action means depth
// bounds aren't actually applied — purely a perf optimization, not a
// correctness issue (more pixels get drawn than strictly needed).
NVAPI_INTERFACE Spoof_D3D11_SetDepthBoundsTest(void* /*pDevice*/, NvU32 /*bEnable*/,
                                                 float /*fMinDepth*/, float /*fMaxDepth*/)
{
    NVAPI_TRACE_FIRST("D3D11_SetDepthBoundsTest");
    return NVAPI_OK;
}

// --- stereo -----------------------------------------------------------------
//
// Stereo_Enable/Disable are the OS-wide 3D Vision enable toggle (the NVIDIA
// Control Panel checkbox). Stereo_Activate/Deactivate are the per-session
// "stereo currently rendering" state. Games like Batman: Arkham Asylum use
// Enable/Disable for their in-game 3D Vision option, while others use
// Activate/Deactivate. Wire both pairs into wiz3D's StereoActive flag so
// either kind of in-game toggle drives wiz3D's mono/stereo state — same
// effect as the user pressing the * hotkey.
//
// Canonical NVAPI stereo init order (per Tal Liron's `opengl-3d-vision-bridge`,
// MIT; the same order NVIDIA's own docs prescribe):
//
//   NvAPI_Initialize();                                       // global init
//   NvAPI_Stereo_CreateConfigurationProfileRegistryKey(
//       NVAPI_STEREO_DX9_REGISTRY_PROFILE);                   // before device
//   NvAPI_Stereo_Enable();                                    // before device
//   /* ... create IDirect3DDevice9 / DX10 / DX11 device ... */
//   NvAPI_Stereo_CreateHandleFromIUnknown(device, &handle);   // after device
//   NvAPI_Stereo_Activate(handle);                            // start rendering
//   NvAPI_Stereo_GetSeparation(handle, &sep);                 // optional, query
//   NvAPI_Stereo_GetConvergence(handle, &conv);
//   NvAPI_Stereo_GetFrustumAdjustMode(handle, &mode);         // NO_ADJUST / STRETCH / CLEAR_EDGES
//   NvAPI_Stereo_GetEyeSeparation(handle, &eyeSep);
//
// We're the spoof side of this contract — every function above has an entry
// in the dispatch table below, plus a Spoof_* implementation that returns
// NVAPI_OK with sensible defaults so the game's init succeeds even on
// non-NVIDIA hardware. The wiz3D bridge picks up the in-game stereo toggle
// regardless of which sequence the game uses.
NVAPI_INTERFACE Spoof_Stereo_Enable(void)
{
    NVAPI_TRACE_FIRST("Stereo_Enable");
    if (ResolveWiz3DBridge()) g_Wiz3D.SetStereoActive(1);
    g_Stereo.isActive = 1;
    return NVAPI_OK;
}
NVAPI_INTERFACE Spoof_Stereo_Disable(void)
{
    NVAPI_TRACE_FIRST("Stereo_Disable");
    if (ResolveWiz3DBridge()) g_Wiz3D.SetStereoActive(0);
    g_Stereo.isActive = 0;
    return NVAPI_OK;
}

NVAPI_INTERFACE Spoof_Stereo_IsEnabled(NvU8* p)
{
    NVAPI_TRACE_FIRST("Stereo_IsEnabled");
    if (!p) return NVAPI_ERROR;
    *p = g_disableStereoSpoof ? 0 : 1;
    return NVAPI_OK;
}

NVAPI_INTERFACE Spoof_Stereo_GetStereoSupport(void* /*opaque pCaps*/) { NVAPI_TRACE_FIRST("Stereo_GetStereoSupport"); return NVAPI_OK; }

NVAPI_INTERFACE Spoof_Stereo_CreateHandleFromIUnknown(void* pDevice, StereoHandle* pH)
{
    NVAPI_TRACE_FIRST("Stereo_CreateHandleFromIUnknown");
    if (!pH) return NVAPI_ERROR;
    *pH = pDevice ? (StereoHandle)pDevice : (StereoHandle)(uintptr_t)0xFA1ED1CEULL;
    return NVAPI_OK;
}

NVAPI_INTERFACE Spoof_Stereo_DestroyHandle(StereoHandle) { NVAPI_TRACE_FIRST("Stereo_DestroyHandle"); return NVAPI_OK; }

NVAPI_INTERFACE Spoof_Stereo_Activate(StereoHandle)
{
    NVAPI_TRACE_FIRST("Stereo_Activate");
    if (ResolveWiz3DBridge() && ShouldApplyGameSet(g_gameHasReadActive))
        g_Wiz3D.SetStereoActive(1);
    g_Stereo.isActive = 1;
    return NVAPI_OK;
}
NVAPI_INTERFACE Spoof_Stereo_Deactivate(StereoHandle)
{
    NVAPI_TRACE_FIRST("Stereo_Deactivate");
    if (ResolveWiz3DBridge() && ShouldApplyGameSet(g_gameHasReadActive))
        g_Wiz3D.SetStereoActive(0);
    g_Stereo.isActive = 0;
    return NVAPI_OK;
}
NVAPI_INTERFACE Spoof_Stereo_IsActivated(StereoHandle, NvU8* p)
{
    if (!p) return NVAPI_ERROR;
    if (g_disableStereoSpoof)
    {
        *p = 0;
    }
    else
    {
        *p = ResolveWiz3DBridge()
            ? (NvU8)(g_Wiz3D.GetStereoActive() ? 1 : 0)
            : g_Stereo.isActive;
    }
    g_gameHasReadActive = true; // user-UI gate: game has now queried our state

    // Push notification: if a game registered (HWND, msg) via
    // Stereo_SetNotificationMessage and our returned state differs from
    // what we last reported, PostMessage so its in-UI checkbox can sync.
    // Skip the very first report (lastReported == -1) so games don't
    // get a spurious notification on startup.
    if (g_notifyHwnd && g_lastReportedActive >= 0 && (int)*p != g_lastReportedActive)
    {
        PostMessage(g_notifyHwnd, g_notifyMsg, (WPARAM)*p, 0);
    }
    g_lastReportedActive = (int)*p;

    NVAPI_TRACE_PERIODIC("Stereo_IsActivated", "%d", (int)*p);
    return NVAPI_OK;
}

NVAPI_INTERFACE Spoof_Stereo_GetSeparation(StereoHandle, float* p)
{
    if (!p) return NVAPI_ERROR;
    *p = ResolveWiz3DBridge()
        ? g_Wiz3D.GetSeparationPercent()
        : g_Stereo.separation;
    g_gameHasReadSeparation = true; // user-UI gate
    NVAPI_TRACE_PERIODIC("Stereo_GetSeparation", "%.2f%%", *p);
    return NVAPI_OK;
}
NVAPI_INTERFACE Spoof_Stereo_SetSeparation(StereoHandle, float v)
{
    if (v < 0.0f || v > 100.0f) return NVAPI_ERROR;
    if (ResolveWiz3DBridge() && ShouldApplyGameSet(g_gameHasReadSeparation))
        g_Wiz3D.SetSeparationPercent(v);
    g_Stereo.separation = v;
    NVAPI_TRACE_EVERY("Stereo_SetSeparation", "%.2f%%", v);
    return NVAPI_OK;
}
NVAPI_INTERFACE Spoof_Stereo_DecreaseSeparation(StereoHandle)
{
    // Hotkey-style entry — always treat as user intent. Route to wiz3D's
    // native STEP_STEREOBASE step (Num+/- equivalent magnitude).
    NVAPI_TRACE_EVERY("Stereo_DecreaseSeparation", "step %d", -1);
    if (ResolveWiz3DBridge() && g_Wiz3D.StepSeparation)
    {
        g_Wiz3D.StepSeparation(-1);
        g_Stereo.separation = g_Wiz3D.GetSeparationPercent();
    }
    else
    {
        g_Stereo.separation = (g_Stereo.separation > 5.0f) ? g_Stereo.separation - 5.0f : 0.0f;
    }
    g_gameHasReadSeparation = true;
    return NVAPI_OK;
}
NVAPI_INTERFACE Spoof_Stereo_IncreaseSeparation(StereoHandle)
{
    NVAPI_TRACE_EVERY("Stereo_IncreaseSeparation", "step %d", +1);
    if (ResolveWiz3DBridge() && g_Wiz3D.StepSeparation)
    {
        g_Wiz3D.StepSeparation(+1);
        g_Stereo.separation = g_Wiz3D.GetSeparationPercent();
    }
    else
    {
        g_Stereo.separation = (g_Stereo.separation < 95.0f) ? g_Stereo.separation + 5.0f : 100.0f;
    }
    g_gameHasReadSeparation = true;
    return NVAPI_OK;
}

NVAPI_INTERFACE Spoof_Stereo_GetConvergence(StereoHandle, float* p)
{
    if (!p) return NVAPI_ERROR;
    *p = ResolveWiz3DBridge()
        ? g_Wiz3D.GetConvergence()
        : g_Stereo.convergence;
    g_gameHasReadConvergence = true; // user-UI gate
    NVAPI_TRACE_PERIODIC("Stereo_GetConvergence", "%.4f", *p);
    return NVAPI_OK;
}
NVAPI_INTERFACE Spoof_Stereo_SetConvergence(StereoHandle, float v)
{
    if (ResolveWiz3DBridge() && ShouldApplyGameSet(g_gameHasReadConvergence))
        g_Wiz3D.SetConvergence(v);
    g_Stereo.convergence = v;
    NVAPI_TRACE_EVERY("Stereo_SetConvergence", "%.4f", v);
    return NVAPI_OK;
}
NVAPI_INTERFACE Spoof_Stereo_DecreaseConvergence(StereoHandle)
{
    NVAPI_TRACE_EVERY("Stereo_DecreaseConvergence", "step %d", -1);
    if (ResolveWiz3DBridge() && g_Wiz3D.StepConvergence)
    {
        g_Wiz3D.StepConvergence(-1);
        g_Stereo.convergence = g_Wiz3D.GetConvergence();
    }
    else
    {
        g_Stereo.convergence -= 0.05f;
    }
    g_gameHasReadConvergence = true;
    return NVAPI_OK;
}
NVAPI_INTERFACE Spoof_Stereo_IncreaseConvergence(StereoHandle)
{
    NVAPI_TRACE_EVERY("Stereo_IncreaseConvergence", "step %d", +1);
    if (ResolveWiz3DBridge() && g_Wiz3D.StepConvergence)
    {
        g_Wiz3D.StepConvergence(+1);
        g_Stereo.convergence = g_Wiz3D.GetConvergence();
    }
    else
    {
        g_Stereo.convergence += 0.05f;
    }
    g_gameHasReadConvergence = true;
    return NVAPI_OK;
}

NVAPI_INTERFACE Spoof_Stereo_GetEyeSeparation(StereoHandle, float* p)
{
    NVAPI_TRACE_FIRST("Stereo_GetEyeSeparation");
    if (!p) return NVAPI_ERROR;
    *p = 0.064f;
    return NVAPI_OK;
}

// Stage 4 per-eye capture bridge: NvDirectMode/d3d11 registers a callback
// here so it can copy the shadow texture into a per-eye frame *before*
// the next render overwrites it. Called on every eye change; receives
// the OLD eye so the listener can capture the eye's frame. Single-slot
// listener (most games have one render thread / one swap chain).
typedef void (__cdecl *Wiz3D_EyeChangeCallback)(int oldEye, int newEye);
static Wiz3D_EyeChangeCallback g_eyeChangeCallback = nullptr;

extern "C" __declspec(dllexport) void __cdecl Wiz3D_SetEyeChangeCallback(Wiz3D_EyeChangeCallback cb)
{
    g_eyeChangeCallback = cb;
}

NVAPI_INTERFACE Spoof_Stereo_SetActiveEye(StereoHandle, NV_STEREO_ACTIVE_EYE eye)
{
    NVAPI_TRACE_FIRST("Stereo_SetActiveEye");
    int oldEye = (int)g_Stereo.activeEye;
    g_Stereo.activeEye = eye;
    if (g_eyeChangeCallback && oldEye != (int)eye)
        g_eyeChangeCallback(oldEye, (int)eye);
    return NVAPI_OK;
}
NVAPI_INTERFACE Spoof_Stereo_SetDriverMode(NV_STEREO_DRIVER_MODE mode)
{
    // Log the actual mode value, not just the call. Critical for diagnosing
    // whether a game is *really* a Direct Mode title (DIRECT=2 — game
    // renders both eyes itself per Stereo_SetActiveEye) or an Auto Mode
    // title that's setting the mode pre-emptively (AUTOMATIC=0 — game
    // expects the driver to project both eyes from a single render). Pure
    // Auto Mode games typically don't call SetDriverMode at all — the
    // canonical 3D-Vision-Automatic Tutorial07 sample omits it entirely.
    {
        char buf[120];
        const char* name = (mode == NVAPI_STEREO_DRIVER_MODE_DIRECT)    ? "DIRECT"
                         : (mode == NVAPI_STEREO_DRIVER_MODE_AUTOMATIC) ? "AUTOMATIC"
                         : "UNKNOWN";
        _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                    "[NvApiProxy] Stereo_SetDriverMode(%d = %s)\n", (int)mode, name);
        WriteLog(buf);
    }
    g_Stereo.driverMode = mode;
    return NVAPI_OK;
}

NVAPI_INTERFACE Spoof_Stereo_IsWindowedModeSupported(NvU8* p)
{
    NVAPI_TRACE_FIRST("Stereo_IsWindowedModeSupported");
    if (!p) return NVAPI_ERROR;
    *p = 1;
    return NVAPI_OK;
}

NVAPI_INTERFACE Spoof_Stereo_SetSurfaceCreationMode(StereoHandle, NVAPI_STEREO_SURFACECREATEMODE m)
{
    NVAPI_TRACE_FIRST("Stereo_SetSurfaceCreationMode");
    g_Stereo.createMode = m;
    return NVAPI_OK;
}
NVAPI_INTERFACE Spoof_Stereo_GetSurfaceCreationMode(StereoHandle, NVAPI_STEREO_SURFACECREATEMODE* p)
{
    NVAPI_TRACE_FIRST("Stereo_GetSurfaceCreationMode");
    if (!p) return NVAPI_ERROR;
    *p = g_Stereo.createMode;
    return NVAPI_OK;
}

NVAPI_INTERFACE Spoof_Stereo_GetFrustumAdjustMode(StereoHandle, NvU32* p)
{
    NVAPI_TRACE_FIRST("Stereo_GetFrustumAdjustMode");
    if (!p) return NVAPI_ERROR;
    *p = g_Stereo.frustumAdjust;
    return NVAPI_OK;
}
NVAPI_INTERFACE Spoof_Stereo_SetFrustumAdjustMode(StereoHandle, NvU32 mode)
{
    NVAPI_TRACE_FIRST("Stereo_SetFrustumAdjustMode");
    g_Stereo.frustumAdjust = mode;
    return NVAPI_OK;
}

NVAPI_INTERFACE Spoof_Stereo_InitActivation(StereoHandle, NvU8 /*enable*/)        { NVAPI_TRACE_FIRST("Stereo_InitActivation");        return NVAPI_OK; }
NVAPI_INTERFACE Spoof_Stereo_Trigger_Activation(StereoHandle)                     { NVAPI_TRACE_FIRST("Stereo_Trigger_Activation");    return NVAPI_OK; }
NVAPI_INTERFACE Spoof_Stereo_ReverseStereoBlitControl(StereoHandle, NvU8 /*on*/)  { NVAPI_TRACE_FIRST("Stereo_ReverseStereoBlitControl"); return NVAPI_OK; }
NVAPI_INTERFACE Spoof_Stereo_SetNotificationMessage(StereoHandle, NvU64 hWnd, NvU64 msgId)
{
    NVAPI_TRACE_FIRST("Stereo_SetNotificationMessage");
    // NVAPI passes HWND + UINT widened to NvU64 for x64 ABI compat.
    g_notifyHwnd = (HWND)(uintptr_t)hWnd;
    g_notifyMsg  = (UINT)msgId;
    return NVAPI_OK;
}

NVAPI_INTERFACE Spoof_Stereo_SetDefaultProfile(const char* /*szName*/)            { NVAPI_TRACE_FIRST("Stereo_SetDefaultProfile");      return NVAPI_OK; }
NVAPI_INTERFACE Spoof_Stereo_GetDefaultProfile(NvU32 /*cbIn*/, char* /*szName*/, NvU32* pcbOut)
{
    NVAPI_TRACE_FIRST("Stereo_GetDefaultProfile");
    if (!pcbOut) return NVAPI_ERROR;
    *pcbOut = 0;  // no default profile set
    return NVAPI_OK;
}

NVAPI_INTERFACE Spoof_Stereo_Debug_WasLastDrawStereoized(StereoHandle, NvU8* p)
{
    NVAPI_TRACE_FIRST("Stereo_Debug_WasLastDrawStereoized");
    if (!p) return NVAPI_ERROR;
    *p = 1;
    return NVAPI_OK;
}

// Game-profile registry calls. Pre-3D-Vision-discontinuation games (Hard
// Reset confirmed 2026-05-10) call these to persist their stereo config to
// the per-game profile under HKLM/HKCU. Modern NVIDIA drivers (post-418.91)
// removed the entire 3D Vision profile system and answer these with
// NOT_SUPPORTED — the game then concludes "stereo config can't be saved"
// and greys out the in-game stereo option even though Stereo_IsActivated
// reported success. Spoof OK so games proceed; we discard the writes since
// wiz3D handles stereo settings itself.
NVAPI_INTERFACE Spoof_Stereo_CreateConfigurationProfileRegistryKey(NvU32 /*regKeyType*/)
{
    NVAPI_TRACE_FIRST("Stereo_CreateConfigurationProfileRegistryKey");
    return NVAPI_OK;
}
NVAPI_INTERFACE Spoof_Stereo_DeleteConfigurationProfileRegistryKey(NvU32 /*regKeyType*/)
{
    NVAPI_TRACE_FIRST("Stereo_DeleteConfigurationProfileRegistryKey");
    return NVAPI_OK;
}
NVAPI_INTERFACE Spoof_Stereo_SetConfigurationProfileValue(NvU32 /*regKeyType*/, NvU32 /*valIdx*/, void* /*pValue*/)
{
    NVAPI_TRACE_FIRST("Stereo_SetConfigurationProfileValue");
    return NVAPI_OK;
}
NVAPI_INTERFACE Spoof_Stereo_DeleteConfigurationProfileValue(NvU32 /*regKeyType*/, NvU32 /*valIdx*/)
{
    NVAPI_TRACE_FIRST("Stereo_DeleteConfigurationProfileValue");
    return NVAPI_OK;
}
NVAPI_INTERFACE Spoof_Stereo_CaptureJpegImage(StereoHandle, NvU32 /*quality*/)
{
    NVAPI_TRACE_FIRST("Stereo_CaptureJpegImage");
    return NVAPI_OK;
}
NVAPI_INTERFACE Spoof_Stereo_CapturePngImage(StereoHandle)
{
    NVAPI_TRACE_FIRST("Stereo_CapturePngImage");
    return NVAPI_OK;
}

// Catch-all for IDs we haven't explicitly mapped.  Returning NVAPI_NOT_SUPPORTED
// causes games to gracefully skip optional features (DLSS, Reflex, telemetry,
// etc.) without aborting.
NVAPI_INTERFACE Spoof_NotSupported(void) { NVAPI_TRACE_FIRST("NotSupported (catch-all)"); return NVAPI_NOT_SUPPORTED; }

// Placeholder for known-called-but-not-yet-identified function IDs.  Returning
// NVAPI_OK (with no output written) is more permissive than NOT_SUPPORTED:
// 3D Vision-era games tend to interpret NOT_SUPPORTED as "this entire NVAPI is
// missing capability X, give up", while OK is treated as "optional check
// passed".  Used for IDs we've observed in the wild but can't yet name.
NVAPI_INTERFACE Spoof_OkNoOp(void) { NVAPI_TRACE_FIRST("OkNoOp (unidentified ID)"); return NVAPI_OK; }

// ============================================================
// Dispatcher — the single export every NVAPI consumer goes through.
// IDs from lib/nvapi_2026/nvapi_interface.h.
// ============================================================
// In passthrough mode we still want the Stereo_* surface served by our own
// spoof + wiz3D bridge: NVIDIA dropped 3D Vision support after driver 425, so
// the real nvapi.dll either NOT_SUPPORTEDs every Stereo_ call (best case) or
// outright crashes when called by HelixMod (Valkyria Chronicles 2026-05-08
// repro). Hybrid mode keeps us out of NVIDIA's stereo code on a system where
// 3D Vision is dead, while still letting the real driver answer everything
// else (driver version, GPU info, display enumeration, ...).
static bool IsStereoFunctionId(NvU32 id)
{
    switch (id)
    {
    case 0x239C4545: // Stereo_Enable
    case 0x2EC50C2B: // Stereo_Disable
    case 0x348FF8E1: // Stereo_IsEnabled
    case 0x296C434D: // Stereo_GetStereoSupport
    case 0xAC7E37F4: // Stereo_CreateHandleFromIUnknown
    case 0x3A153134: // Stereo_DestroyHandle
    case 0xF6A1AD68: // Stereo_Activate
    case 0x2D68DE96: // Stereo_Deactivate
    case 0x1FB0BC30: // Stereo_IsActivated
    case 0x451F2134: // Stereo_GetSeparation
    case 0x5C069FA3: // Stereo_SetSeparation
    case 0xDA044458: // Stereo_DecreaseSeparation
    case 0xC9A8ECEC: // Stereo_IncreaseSeparation
    case 0x4AB00934: // Stereo_GetConvergence
    case 0x3DD6B54B: // Stereo_SetConvergence
    case 0x4C87E317: // Stereo_DecreaseConvergence
    case 0xA17DAABE: // Stereo_IncreaseConvergence
    case 0xE6839B43: // Stereo_GetFrustumAdjustMode
    case 0x7BE27FA2: // Stereo_SetFrustumAdjustMode
    case 0xC7177702: // Stereo_InitActivation
    case 0x0D6C6CD2: // Stereo_Trigger_Activation
    case 0x3CD58F89: // Stereo_ReverseStereoBlitControl
    case 0x6B9B409E: // Stereo_SetNotificationMessage
    case 0x96EEA9F8: // Stereo_SetActiveEye
    case 0x5E8F0BEC: // Stereo_SetDriverMode
    case 0xCE653127: // Stereo_GetEyeSeparation
    case 0x40C8ED5E: // Stereo_IsWindowedModeSupported
    case 0xF5DCFCBA: // Stereo_SetSurfaceCreationMode
    case 0x36F1C736: // Stereo_GetSurfaceCreationMode
    case 0xED4416C5: // Stereo_Debug_WasLastDrawStereoized
    case 0x44F0ECD1: // Stereo_SetDefaultProfile
    case 0x624E21C2: // Stereo_GetDefaultProfile
    case 0xBE7692EC: // Stereo_CreateConfigurationProfileRegistryKey
    case 0xF117B834: // Stereo_DeleteConfigurationProfileRegistryKey
    case 0x24409F48: // Stereo_SetConfigurationProfileValue
    case 0x49BCEECF: // Stereo_DeleteConfigurationProfileValue
    case 0x932CB140: // Stereo_CaptureJpegImage
    case 0x8B7E99B5: // Stereo_CapturePngImage
    // Private NVIDIA function IDs (not in the public 2026 SDK header). 3D
    // Vision-era games (Hard Reset confirmed) call these BEFORE the named
    // Stereo_* sequence, then gate their in-game stereo option on the
    // result. Modern NVIDIA drivers (post-3D-Vision-discontinuation)
    // answer NOT_SUPPORTED → game greys out the option even though our
    // named Stereo_* spoofs all succeed afterwards. Forcing these through
    // our spoof+OkNoOp returns NVAPI_OK so the gate flips open.
    case 0x33C7358C:
    case 0x36E39E6B:
    case 0x593E8644:
    // Driver version queries — force through our spoof even in passthrough
    // mode so games see our spoofed 3D-Vision-era driver (426.06, the last
    // 3D-Vision-aware NVIDIA driver) instead of the real installed driver
    // (which is always post-425.31 on a modern setup and signals "3D Vision
    // dropped" to games). Without this forcing, EGO engine games (Dirt
    // Rally, Grid Autosport, Dirt 3, Dirt Showdown) and Hard Reset complete
    // the Stereo_* init sequence successfully but skip per-eye view-shift
    // rendering and hide the in-game stereoscopic 3D menu option, having
    // detected the modern driver. Our regular spoof returns kSpoofDrvVersion
    // = 42606 + branch "r426_00-100".
    case 0x2926AAAD: // SYS_GetDriverAndBranchVersion
    case 0xF951A4D1: // GetDisplayDriverVersion
        return true;
    default:
        return false;
    }
}

// Catch-all per-ID logger. In passthrough mode every non-stereo call goes
// straight to the real driver without ever hitting our named Spoof_* TRACE
// macros, so without this we have no visibility into *what* the game is
// asking the driver. Diagnostic for cases like Hard Reset where Stereo_*
// answers all succeed but the in-game stereo option is still greyed out —
// the gating must come from a different NvAPI call we never see.
//
// Dedup via a small fixed-size set: 3D Vision-era games query 200+ unique
// IDs at startup, never reaches the cap. Linear scan is fine; this is called
// once per unique ID per process, not per frame.
static CRITICAL_SECTION g_csSeenIds;
static volatile LONG    g_seenIdsInit = 0;
static NvU32            g_seenIds[256] = {};
static int              g_numSeenIds   = 0;

static void LogQueryInterfaceId(NvU32 id, const char* tag)
{
    if (InterlockedCompareExchange(&g_seenIdsInit, 1, 0) == 0)
        InitializeCriticalSection(&g_csSeenIds);

    bool isNew = false;
    EnterCriticalSection(&g_csSeenIds);
    bool found = false;
    for (int i = 0; i < g_numSeenIds; ++i)
        if (g_seenIds[i] == id) { found = true; break; }
    if (!found && g_numSeenIds < (int)(sizeof(g_seenIds) / sizeof(g_seenIds[0])))
    {
        g_seenIds[g_numSeenIds++] = id;
        isNew = true;
    }
    LeaveCriticalSection(&g_csSeenIds);

    if (isNew)
    {
        char buf[96];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                    "[NvApiProxy] QI %s id=0x%08X\n", tag, id);
        WriteLog(buf);
    }
}

extern "C" __declspec(dllexport) void* __cdecl nvapi_QueryInterface(NvU32 id)
{
    InitMode();

    // Hybrid: stereo always goes through our spoof+bridge even in passthrough
    // mode. Falls through to the switch below.
    const bool forceSpoof = IsStereoFunctionId(id);

    if (g_bPassthrough && !forceSpoof)
    {
        LogQueryInterfaceId(id, "passthrough");
        return g_pfnRealQI(id);
    }

    switch (id)
    {
        // sys / general
        case 0x0150E828: return (void*)&Spoof_Initialize;
        case 0xD22BDD7E: return (void*)&Spoof_Unload;
        case 0x6C2D048C: return (void*)&Spoof_GetErrorMessage;
        case 0x01053FA5: return (void*)&Spoof_GetInterfaceVersionString;

        // driver / GPU
        case 0x2926AAAD: return (void*)&Spoof_SYS_GetDriverAndBranchVersion;
        case 0xF951A4D1: return (void*)&Spoof_GetDisplayDriverVersion;
        case 0xE5AC921F: return (void*)&Spoof_EnumPhysicalGPUs;
        case 0x48B3EA59: return (void*)&Spoof_EnumLogicalGPUs;
        case 0x34EF9506: return (void*)&Spoof_GetPhysicalGPUsFromDisplay;
        case 0x9ABDD40D: return (void*)&Spoof_EnumNvidiaDisplayHandle;
        case 0x20DE9260: return (void*)&Spoof_EnumNvidiaUnAttachedDisplayHandle;
        case 0x35C29134: return (void*)&Spoof_GetAssociatedNvidiaDisplayHandle;
        case 0xD995937E: return (void*)&Spoof_GetAssociatedDisplayOutputId;

        // Observed-in-the-wild IDs not in the public 2026 SDK header (private
        // NVAPI functions that 3D Vision-era games still call). We return
        // NVAPI_OK so the game proceeds rather than bailing on NOT_SUPPORTED.
        // If a game ends up needing real output values from these, we'll need
        // to identify the actual function and implement it.
        //
        // Logged callers (2026-05-10):
        //   0x33C7358C  — Hard Reset (post-Initialize, pre-Stereo)
        //   0x36E39E6B  — historical (preserved)
        //   0x593E8644  — Hard Reset (post-Initialize, pre-Stereo)
        //   0x1EA54A3B  — Max Payne 3 (Auto Mode title; pre-stereo enum)
        //   0x774AA982  — Max Payne 3, GTA IV (both gate stereo somewhere
        //                 we can't see; OkNoOp doesn't unblock either)
        //   0x6A16D3A0  — Civilization V, Lost Planet 2 (both Auto Mode).
        //                 Speculation: UI/HUD-flatness hint — Auto Mode
        //                 games keep their UI screen-locked outside the
        //                 stereo projection by tagging surfaces. Both
        //                 callers are heavy-UI Auto Mode titles. Spoofing
        //                 OK on AMD/Intel where real driver isn't there.
        //
        // The 0x33C7358C / 0x593E8644 pair fire BEFORE Stereo_SetDriverMode
        // in Hard Reset, suggesting "is 3D Vision available on this driver?"
        // probes. Forcing them through OkNoOp didn't ungray Hard Reset's
        // option (gating turned out to be non-NvAPI), but they still need
        // explicit OK on AMD/Intel where the default NOT_SUPPORTED would
        // make the games bail entirely.
        case 0x33C7358C: return (void*)&Spoof_OkNoOp;
        case 0x36E39E6B: return (void*)&Spoof_OkNoOp;
        case 0x593E8644: return (void*)&Spoof_OkNoOp;
        case 0x1EA54A3B: return (void*)&Spoof_OkNoOp;
        case 0x774AA982: return (void*)&Spoof_OkNoOp;
        case 0x6A16D3A0: return (void*)&Spoof_OkNoOp;

        case 0xCEEE8E9F: return (void*)&Spoof_GPU_GetFullName;
        case 0xBAAABFCC: return (void*)&Spoof_GPU_GetSystemType;
        case 0xC33BAEB1: return (void*)&Spoof_GPU_GetGPUType;
        case 0x2DDFB66E: return (void*)&Spoof_GPU_GetPCIIdentifiers;
        case 0xC7026A87: return (void*)&Spoof_GPU_GetGpuCoreCount;       // MP3, EGO games, Hitman
        case 0x7AAF7A04: return (void*)&Spoof_D3D11_SetDepthBoundsTest;  // TR 2013, Hitman

        // stereo
        case 0x239C4545: return (void*)&Spoof_Stereo_Enable;
        case 0x2EC50C2B: return (void*)&Spoof_Stereo_Disable;
        case 0x348FF8E1: return (void*)&Spoof_Stereo_IsEnabled;
        case 0x296C434D: return (void*)&Spoof_Stereo_GetStereoSupport;
        case 0xAC7E37F4: return (void*)&Spoof_Stereo_CreateHandleFromIUnknown;
        case 0x3A153134: return (void*)&Spoof_Stereo_DestroyHandle;
        case 0xF6A1AD68: return (void*)&Spoof_Stereo_Activate;
        case 0x2D68DE96: return (void*)&Spoof_Stereo_Deactivate;
        case 0x1FB0BC30: return (void*)&Spoof_Stereo_IsActivated;
        case 0x451F2134: return (void*)&Spoof_Stereo_GetSeparation;
        case 0x5C069FA3: return (void*)&Spoof_Stereo_SetSeparation;
        case 0xDA044458: return (void*)&Spoof_Stereo_DecreaseSeparation;
        case 0xC9A8ECEC: return (void*)&Spoof_Stereo_IncreaseSeparation;
        case 0x4AB00934: return (void*)&Spoof_Stereo_GetConvergence;
        case 0x3DD6B54B: return (void*)&Spoof_Stereo_SetConvergence;
        case 0x4C87E317: return (void*)&Spoof_Stereo_DecreaseConvergence;
        case 0xA17DAABE: return (void*)&Spoof_Stereo_IncreaseConvergence;
        case 0xE6839B43: return (void*)&Spoof_Stereo_GetFrustumAdjustMode;
        case 0x7BE27FA2: return (void*)&Spoof_Stereo_SetFrustumAdjustMode;
        case 0xC7177702: return (void*)&Spoof_Stereo_InitActivation;
        case 0x0D6C6CD2: return (void*)&Spoof_Stereo_Trigger_Activation;
        case 0x3CD58F89: return (void*)&Spoof_Stereo_ReverseStereoBlitControl;
        case 0x6B9B409E: return (void*)&Spoof_Stereo_SetNotificationMessage;
        case 0x96EEA9F8: return (void*)&Spoof_Stereo_SetActiveEye;
        case 0x5E8F0BEC: return (void*)&Spoof_Stereo_SetDriverMode;
        case 0xCE653127: return (void*)&Spoof_Stereo_GetEyeSeparation;
        case 0x40C8ED5E: return (void*)&Spoof_Stereo_IsWindowedModeSupported;
        case 0xF5DCFCBA: return (void*)&Spoof_Stereo_SetSurfaceCreationMode;
        case 0x36F1C736: return (void*)&Spoof_Stereo_GetSurfaceCreationMode;
        case 0xED4416C5: return (void*)&Spoof_Stereo_Debug_WasLastDrawStereoized;
        case 0x44F0ECD1: return (void*)&Spoof_Stereo_SetDefaultProfile;
        case 0x624E21C2: return (void*)&Spoof_Stereo_GetDefaultProfile;
        case 0xBE7692EC: return (void*)&Spoof_Stereo_CreateConfigurationProfileRegistryKey;
        case 0xF117B834: return (void*)&Spoof_Stereo_DeleteConfigurationProfileRegistryKey;
        case 0x24409F48: return (void*)&Spoof_Stereo_SetConfigurationProfileValue;
        case 0x49BCEECF: return (void*)&Spoof_Stereo_DeleteConfigurationProfileValue;
        case 0x932CB140: return (void*)&Spoof_Stereo_CaptureJpegImage;
        case 0x8B7E99B5: return (void*)&Spoof_Stereo_CapturePngImage;

        default:
        {
            // Empirically (Batman: Arkham Asylum on AMD, full log), 3D Vision-era
            // games query 200+ NVAPI functions during startup — most undocumented
            // and not in the public 2026 SDK. Returning NOT_SUPPORTED for all
            // unknowns made games bail on the first introspection call.
            //
            // OK-no-op is more permissive: game treats the call as "succeeded,
            // returned no info" rather than "this NVAPI is broken". The risk is
            // OUT params remain whatever the caller initialised them to (often
            // zero), which can cause subtle issues if the game depends on
            // populated outputs. Tradeoff worth taking — better to ship and
            // identify specific blockers than ship-block on every unknown.
            char buf[80];
            _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                        "[NvApiProxy] unknown id=0x%08X -> OK (default)\n", id);
            WriteLog(buf);
            return (void*)&Spoof_OkNoOp;
        }
    }
}

// Some games look up the uppercase symbol; route to the same dispatcher.
extern "C" __declspec(dllexport) void* __cdecl NvAPI_QueryInterface(NvU32 id)
{
    return nvapi_QueryInterface(id);
}

// ============================================================
// NvDirectMode bridge — exposes the game's last NvAPI_Stereo_SetActiveEye
// value so the Direct Mode d3d9/10/11/opengl proxies can route per-eye RT
// binds without each replicating their own NvAPI hook. The value follows
// NVIDIA's NV_STEREO_ACTIVE_EYE encoding:
//   1 = NVAPI_STEREO_EYE_RIGHT
//   2 = NVAPI_STEREO_EYE_LEFT
//   3 = NVAPI_STEREO_EYE_MONO  (game hasn't selected an eye yet)
//
// Resolved at runtime from the NvDirectMode side via GetModuleHandle("nvapi[64].dll")
// + GetProcAddress("Wiz3D_GetActiveEye"). Returns MONO when the proxy is
// loaded but the game hasn't called SetActiveEye yet — caller can treat
// MONO as "no eye routing, render full frame".
// ============================================================
extern "C" __declspec(dllexport) int __cdecl Wiz3D_GetActiveEye()
{
    return (int)g_Stereo.activeEye;
}

// ============================================================
// DllMain
// ============================================================
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_hSelf = hModule;
        DisableThreadLibraryCalls(hModule);
        LoadConfig(hModule);
        char attachLine[160];
        _snprintf_s(attachLine, sizeof(attachLine), _TRUNCATE,
                    "[NvApiProxy] DLL_PROCESS_ATTACH (wiz3D " DISPLAYED_VERSION ") DisableStereoSpoof=%d\n",
                    g_disableStereoSpoof);
        WriteLog(attachLine);
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        if (g_hRealNvapi)
        {
            FreeLibrary(g_hRealNvapi);
            g_hRealNvapi = nullptr;
            g_pfnRealQI  = nullptr;
        }
    }
    return TRUE;
}
