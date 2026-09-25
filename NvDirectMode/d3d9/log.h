/* NvDirectMode/d3d11 - logging API exposed to proxy classes
 *
 * dllmain.cpp owns the actual log file + the verbose-logging flag (read
 * from 3DVision_Config.xml at DLL_PROCESS_ATTACH). Other TUs in this DLL
 * call NvDM_Log / NvDM_LogVerbose via the C-linkage bridge declared here.
 */

#pragma once

extern "C" void NvDM_Log(const char* fmt, ...);
extern "C" int  NvDM_VerboseEnabled();
extern "C" int  NvDM_SwapEyes();
extern "C" int  NvDM_OutputMode();
extern "C" int  NvDM_OutputIsTopBottom();
extern "C" int  NvDM_TraceD9Methods();

// LOG_VERBOSE: gated on the config flag — emits at most a small amount
// of info per call site (use NVDM_TRACE_FIRST below for hot paths).
#define LOG_VERBOSE(...)            do { if (NvDM_VerboseEnabled()) NvDM_Log(__VA_ARGS__); } while (0)

// Trace the first N calls only — use for hot-path methods (per-frame Present,
// per-bind OMSetRenderTargets, etc.) so verbose logging doesn't fill the
// disk during normal play.
#define NVDM_TRACE_FIRST_N(n, ...)  do {                                                    \
    static volatile long _nvdm_count = 0;                                                   \
    if (NvDM_VerboseEnabled() && _InterlockedIncrement(&_nvdm_count) <= (long)(n)) {        \
        NvDM_Log(__VA_ARGS__);                                                              \
    }                                                                                       \
} while (0)

// D9_TRACE_R: gated on TraceD9Methods config flag. Logs first 200 calls per
// call site with formatted args + return HRESULT. Purpose-built for the MT
// Framework ERR09 diagnostic — finds the last IDirect3D9 / IDirect3DDevice9
// method the game called before printing its "Error 9: Unsupported function"
// dialog. Zero cost when the flag is off (default).
#define D9_TRACE_R(name, hr, argfmt, ...)  do {                                             \
    if (NvDM_TraceD9Methods()) {                                                            \
        static volatile long _d9t_count = 0;                                                \
        if (_InterlockedIncrement(&_d9t_count) <= 200) {                                    \
            NvDM_Log("  D9::" name "(" argfmt ") -> 0x%08lX\n", __VA_ARGS__, (long)(hr));   \
        }                                                                                   \
    }                                                                                       \
} while (0)

// D9_TRACE_R0: for zero-arg methods so we don't need a dummy arg for the varargs.
#define D9_TRACE_R0(name, hr)  do {                                                         \
    if (NvDM_TraceD9Methods()) {                                                            \
        static volatile long _d9t_count = 0;                                                \
        if (_InterlockedIncrement(&_d9t_count) <= 200) {                                    \
            NvDM_Log("  D9::" name "() -> 0x%08lX\n", (long)(hr));                          \
        }                                                                                   \
    }                                                                                       \
} while (0)
