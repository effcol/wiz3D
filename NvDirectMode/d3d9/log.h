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
