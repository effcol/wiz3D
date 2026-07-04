/* NvDirectMode - opengl32.dll Direct Mode Proxy
 *
 * Drop this opengl32.dll into the game folder of an OpenGL game that uses
 * NVIDIA 3D Vision Direct Mode (NvAPI_Stereo_SetActiveEye + per-eye render).
 *
 * Stage 1 only: passthrough skeleton. The proxy loads real opengl32.dll,
 * resolves all 368 exports, and forwards every call via naked-jmp thunks.
 *
 * Per-eye routing for OpenGL is qualitatively different from the DX path
 * (no swap-chain abstraction, OS-controlled window backbuffer) and is
 * deferred — the design likely requires an internal FBO at 2*W width
 * with a swap-time blit of the active-eye half to the real backbuffer.
 *
 * The 368-thunk pattern + .def file are inherited from wiz3D-proxy/opengl32
 * which solves the same passthrough surface problem.
 */

#define WIN32_LEAN_AND_MEAN
#define NOGDI
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include "../proxy_version.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <psapi.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "psapi.lib")

// ---------------------------------------------------------------------------
// Diagnostic log + 3DVision_Config.xml reader (mirrors the d3d11 pattern).
// On the OpenGL side g_wrapDevices controls whether sys_glBindFramebuffer
// redirects FB 0 binds to our per-eye internal FBO. With WrapDevices=0 the
// proxy is pure-passthrough except for diagnostic logging.
// ---------------------------------------------------------------------------
static FILE* g_logFile = NULL;
static int   g_loggingEnabled = 1;
static int   g_verboseEnabled = 1;
static int   g_swapEyes       = 0;
static int   g_wrapDevices    = 1;
static int   g_outputMode      = 1;   // OpenGL default = SBS (mode 8/SR also implemented but unverified; opt in via XML)
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
extern "C" int NvDM_WrapDevices()    { return g_wrapDevices; }
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
// Vectored exception handler
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
        Log("Faulting module: UNKNOWN\n");

    if (code == EXCEPTION_ACCESS_VIOLATION &&
        pExInfo->ExceptionRecord->NumberParameters >= 2)
    {
        ULONG_PTR accessType = pExInfo->ExceptionRecord->ExceptionInformation[0];
        const char* op = (accessType == 0) ? "READ" : (accessType == 1) ? "WRITE" : "DEP";
        Log("AV: %s of %p\n", op, (void*)pExInfo->ExceptionRecord->ExceptionInformation[1]);
    }

    CONTEXT* ctx = pExInfo->ContextRecord;
#ifdef _M_IX86
    Log("EAX=%08lX EBX=%08lX ECX=%08lX EDX=%08lX\n", ctx->Eax, ctx->Ebx, ctx->Ecx, ctx->Edx);
    Log("ESI=%08lX EDI=%08lX EBP=%08lX ESP=%08lX EIP=%08lX\n", ctx->Esi, ctx->Edi, ctx->Ebp, ctx->Esp, ctx->Eip);
#elif defined(_M_X64)
    Log("RAX=%016llX RBX=%016llX RCX=%016llX RDX=%016llX\n", ctx->Rax, ctx->Rbx, ctx->Rcx, ctx->Rdx);
    Log("RSI=%016llX RDI=%016llX RBP=%016llX RSP=%016llX RIP=%016llX\n", ctx->Rsi, ctx->Rdi, ctx->Rbp, ctx->Rsp, ctx->Rip);
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
                WCHAR mn[MAX_PATH];
                GetModuleFileNameW(hFrameMod, mn, MAX_PATH);
                WCHAR* p = wcsrchr(mn, L'\\');
                Log("  [%2u] %p  %ls+0x%IX\n", i, stack[i], p ? p+1 : mn, (BYTE*)stack[i]-(BYTE*)hFrameMod);
            }
            else
                Log("  [%2u] %p  (unknown)\n", i, stack[i]);
        }
    }
    Log("--- End stack trace ---\n");

    {
        WCHAR dumpPath[MAX_PATH];
        GetModuleFileNameW(NULL, dumpPath, MAX_PATH);
        WCHAR* p = wcsrchr(dumpPath, L'\\');
        if (p) *(p + 1) = L'\0';
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
            Log("Minidump: %ls\n", dumpPath);
        }
    }
    Log("=== CRASH END ===\n");
    if (g_logFile) fflush(g_logFile);
    return EXCEPTION_CONTINUE_SEARCH;
}

// ---------------------------------------------------------------------------
// Module handles
// ---------------------------------------------------------------------------
// g_hRealOpenGL32 is referenced by gl_hooks.cpp + fbo_state.cpp via extern "C";
// keeping it at file scope but not static so the linker can resolve those refs.
extern "C" HMODULE g_hRealOpenGL32 = NULL;
static HMODULE g_hProxy = NULL;

// ---------------------------------------------------------------------------
// All 368 original function pointers from real opengl32.dll
// ---------------------------------------------------------------------------
extern "C" FARPROC g_pOrigGL[368] = {0};

static const char* g_glExportNames[368] = {
    "GlmfBeginGlsBlock",
    "GlmfCloseMetaFile",
    "GlmfEndGlsBlock",
    "GlmfEndPlayback",
    "GlmfInitPlayback",
    "GlmfPlayGlsRecord",
    "glAccum",
    "glAlphaFunc",
    "glAreTexturesResident",
    "glArrayElement",
    "glBegin",
    "glBindTexture",
    "glBitmap",
    "glBlendFunc",
    "glCallList",
    "glCallLists",
    "glClear",
    "glClearAccum",
    "glClearColor",
    "glClearDepth",
    "glClearIndex",
    "glClearStencil",
    "glClipPlane",
    "glColor3b",
    "glColor3bv",
    "glColor3d",
    "glColor3dv",
    "glColor3f",
    "glColor3fv",
    "glColor3i",
    "glColor3iv",
    "glColor3s",
    "glColor3sv",
    "glColor3ub",
    "glColor3ubv",
    "glColor3ui",
    "glColor3uiv",
    "glColor3us",
    "glColor3usv",
    "glColor4b",
    "glColor4bv",
    "glColor4d",
    "glColor4dv",
    "glColor4f",
    "glColor4fv",
    "glColor4i",
    "glColor4iv",
    "glColor4s",
    "glColor4sv",
    "glColor4ub",
    "glColor4ubv",
    "glColor4ui",
    "glColor4uiv",
    "glColor4us",
    "glColor4usv",
    "glColorMask",
    "glColorMaterial",
    "glColorPointer",
    "glCopyPixels",
    "glCopyTexImage1D",
    "glCopyTexImage2D",
    "glCopyTexSubImage1D",
    "glCopyTexSubImage2D",
    "glCullFace",
    "glDebugEntry",
    "glDeleteLists",
    "glDeleteTextures",
    "glDepthFunc",
    "glDepthMask",
    "glDepthRange",
    "glDisable",
    "glDisableClientState",
    "glDrawArrays",
    "glDrawBuffer",
    "glDrawElements",
    "glDrawPixels",
    "glEdgeFlag",
    "glEdgeFlagPointer",
    "glEdgeFlagv",
    "glEnable",
    "glEnableClientState",
    "glEnd",
    "glEndList",
    "glEvalCoord1d",
    "glEvalCoord1dv",
    "glEvalCoord1f",
    "glEvalCoord1fv",
    "glEvalCoord2d",
    "glEvalCoord2dv",
    "glEvalCoord2f",
    "glEvalCoord2fv",
    "glEvalMesh1",
    "glEvalMesh2",
    "glEvalPoint1",
    "glEvalPoint2",
    "glFeedbackBuffer",
    "glFinish",
    "glFlush",
    "glFogf",
    "glFogfv",
    "glFogi",
    "glFogiv",
    "glFrontFace",
    "glFrustum",
    "glGenLists",
    "glGenTextures",
    "glGetBooleanv",
    "glGetClipPlane",
    "glGetDoublev",
    "glGetError",
    "glGetFloatv",
    "glGetIntegerv",
    "glGetLightfv",
    "glGetLightiv",
    "glGetMapdv",
    "glGetMapfv",
    "glGetMapiv",
    "glGetMaterialfv",
    "glGetMaterialiv",
    "glGetPixelMapfv",
    "glGetPixelMapuiv",
    "glGetPixelMapusv",
    "glGetPointerv",
    "glGetPolygonStipple",
    "glGetString",
    "glGetTexEnvfv",
    "glGetTexEnviv",
    "glGetTexGendv",
    "glGetTexGenfv",
    "glGetTexGeniv",
    "glGetTexImage",
    "glGetTexLevelParameterfv",
    "glGetTexLevelParameteriv",
    "glGetTexParameterfv",
    "glGetTexParameteriv",
    "glHint",
    "glIndexMask",
    "glIndexPointer",
    "glIndexd",
    "glIndexdv",
    "glIndexf",
    "glIndexfv",
    "glIndexi",
    "glIndexiv",
    "glIndexs",
    "glIndexsv",
    "glIndexub",
    "glIndexubv",
    "glInitNames",
    "glInterleavedArrays",
    "glIsEnabled",
    "glIsList",
    "glIsTexture",
    "glLightModelf",
    "glLightModelfv",
    "glLightModeli",
    "glLightModeliv",
    "glLightf",
    "glLightfv",
    "glLighti",
    "glLightiv",
    "glLineStipple",
    "glLineWidth",
    "glListBase",
    "glLoadIdentity",
    "glLoadMatrixd",
    "glLoadMatrixf",
    "glLoadName",
    "glLogicOp",
    "glMap1d",
    "glMap1f",
    "glMap2d",
    "glMap2f",
    "glMapGrid1d",
    "glMapGrid1f",
    "glMapGrid2d",
    "glMapGrid2f",
    "glMaterialf",
    "glMaterialfv",
    "glMateriali",
    "glMaterialiv",
    "glMatrixMode",
    "glMultMatrixd",
    "glMultMatrixf",
    "glNewList",
    "glNormal3b",
    "glNormal3bv",
    "glNormal3d",
    "glNormal3dv",
    "glNormal3f",
    "glNormal3fv",
    "glNormal3i",
    "glNormal3iv",
    "glNormal3s",
    "glNormal3sv",
    "glNormalPointer",
    "glOrtho",
    "glPassThrough",
    "glPixelMapfv",
    "glPixelMapuiv",
    "glPixelMapusv",
    "glPixelStoref",
    "glPixelStorei",
    "glPixelTransferf",
    "glPixelTransferi",
    "glPixelZoom",
    "glPointSize",
    "glPolygonMode",
    "glPolygonOffset",
    "glPolygonStipple",
    "glPopAttrib",
    "glPopClientAttrib",
    "glPopMatrix",
    "glPopName",
    "glPrioritizeTextures",
    "glPushAttrib",
    "glPushClientAttrib",
    "glPushMatrix",
    "glPushName",
    "glRasterPos2d",
    "glRasterPos2dv",
    "glRasterPos2f",
    "glRasterPos2fv",
    "glRasterPos2i",
    "glRasterPos2iv",
    "glRasterPos2s",
    "glRasterPos2sv",
    "glRasterPos3d",
    "glRasterPos3dv",
    "glRasterPos3f",
    "glRasterPos3fv",
    "glRasterPos3i",
    "glRasterPos3iv",
    "glRasterPos3s",
    "glRasterPos3sv",
    "glRasterPos4d",
    "glRasterPos4dv",
    "glRasterPos4f",
    "glRasterPos4fv",
    "glRasterPos4i",
    "glRasterPos4iv",
    "glRasterPos4s",
    "glRasterPos4sv",
    "glReadBuffer",
    "glReadPixels",
    "glRectd",
    "glRectdv",
    "glRectf",
    "glRectfv",
    "glRecti",
    "glRectiv",
    "glRects",
    "glRectsv",
    "glRenderMode",
    "glRotated",
    "glRotatef",
    "glScaled",
    "glScalef",
    "glScissor",
    "glSelectBuffer",
    "glShadeModel",
    "glStencilFunc",
    "glStencilMask",
    "glStencilOp",
    "glTexCoord1d",
    "glTexCoord1dv",
    "glTexCoord1f",
    "glTexCoord1fv",
    "glTexCoord1i",
    "glTexCoord1iv",
    "glTexCoord1s",
    "glTexCoord1sv",
    "glTexCoord2d",
    "glTexCoord2dv",
    "glTexCoord2f",
    "glTexCoord2fv",
    "glTexCoord2i",
    "glTexCoord2iv",
    "glTexCoord2s",
    "glTexCoord2sv",
    "glTexCoord3d",
    "glTexCoord3dv",
    "glTexCoord3f",
    "glTexCoord3fv",
    "glTexCoord3i",
    "glTexCoord3iv",
    "glTexCoord3s",
    "glTexCoord3sv",
    "glTexCoord4d",
    "glTexCoord4dv",
    "glTexCoord4f",
    "glTexCoord4fv",
    "glTexCoord4i",
    "glTexCoord4iv",
    "glTexCoord4s",
    "glTexCoord4sv",
    "glTexCoordPointer",
    "glTexEnvf",
    "glTexEnvfv",
    "glTexEnvi",
    "glTexEnviv",
    "glTexGend",
    "glTexGendv",
    "glTexGenf",
    "glTexGenfv",
    "glTexGeni",
    "glTexGeniv",
    "glTexImage1D",
    "glTexImage2D",
    "glTexParameterf",
    "glTexParameterfv",
    "glTexParameteri",
    "glTexParameteriv",
    "glTexSubImage1D",
    "glTexSubImage2D",
    "glTranslated",
    "glTranslatef",
    "glVertex2d",
    "glVertex2dv",
    "glVertex2f",
    "glVertex2fv",
    "glVertex2i",
    "glVertex2iv",
    "glVertex2s",
    "glVertex2sv",
    "glVertex3d",
    "glVertex3dv",
    "glVertex3f",
    "glVertex3fv",
    "glVertex3i",
    "glVertex3iv",
    "glVertex3s",
    "glVertex3sv",
    "glVertex4d",
    "glVertex4dv",
    "glVertex4f",
    "glVertex4fv",
    "glVertex4i",
    "glVertex4iv",
    "glVertex4s",
    "glVertex4sv",
    "glVertexPointer",
    "glViewport",
    "wglChoosePixelFormat",
    "wglCopyContext",
    "wglCreateContext",
    "wglCreateLayerContext",
    "wglDeleteContext",
    "wglDescribeLayerPlane",
    "wglDescribePixelFormat",
    "wglGetCurrentContext",
    "wglGetCurrentDC",
    "wglGetDefaultProcAddress",
    "wglGetLayerPaletteEntries",
    "wglGetPixelFormat",
    "wglGetProcAddress",
    "wglMakeCurrent",
    "wglRealizeLayerPalette",
    "wglSetLayerPaletteEntries",
    "wglSetPixelFormat",
    "wglShareLists",
    "wglSwapBuffers",
    "wglSwapLayerBuffers",
    "wglSwapMultipleBuffers",
    "wglUseFontBitmapsA",
    "wglUseFontBitmapsW",
    "wglUseFontOutlinesA",
    "wglUseFontOutlinesW"
};

// ---------------------------------------------------------------------------
// Get proxy directory
// ---------------------------------------------------------------------------
static void GetProxyDirectory(WCHAR* dir, DWORD maxLen)
{
    GetModuleFileNameW(g_hProxy, dir, maxLen);
    WCHAR* p = wcsrchr(dir, L'\\');
    if (p) *(p + 1) = L'\0';
}

// ---------------------------------------------------------------------------
// Load real opengl32.dll from System32 and resolve all 368 exports
// ---------------------------------------------------------------------------
static BOOL LoadRealOpenGL32(void)
{
    if (g_hRealOpenGL32) return TRUE;

    WCHAR sysDir[MAX_PATH];
    GetSystemDirectoryW(sysDir, MAX_PATH);
    lstrcatW(sysDir, L"\\opengl32.dll");

    g_hRealOpenGL32 = LoadLibraryW(sysDir);
    if (!g_hRealOpenGL32)
    {
        Log("FAIL: Could not load real opengl32.dll from %ls (error %lu)\n", sysDir, GetLastError());
        return FALSE;
    }
    Log("OK: Real opengl32.dll loaded from %ls\n", sysDir);

    int resolved = 0;
    for (int i = 0; i < 368; i++)
    {
        g_pOrigGL[i] = GetProcAddress(g_hRealOpenGL32, g_glExportNames[i]);
        if (g_pOrigGL[i]) resolved++;
    }
    Log("Resolved %d of 368 opengl32 exports\n", resolved);
    return TRUE;
}

// NvDirectMode does NOT load a separate stereoization wrapper (unlike
// wiz3D-proxy/opengl32 which loads S3DWrapperOGL.dll for active stereoization).
// Direct Mode receives already-stereo content from the game; we just need to
// route per-eye output. That logic lands in a future stage; for now this is
// pure passthrough.

// ===========================================================================
// Pass-through exports
// x86: naked jmp thunks with inline asm
// x64: thunks are in thunks_x64.asm (MASM64) � see that file
// ===========================================================================

#ifdef _M_IX86

#define GL_THUNK(exportName, idx) \
    extern "C" __declspec(naked) void exportName() \
    { \
        __asm mov eax, dword ptr [g_pOrigGL + 4*(idx)] \
        __asm test eax, eax \
        __asm jz _skip_##exportName \
        __asm jmp eax \
        __asm _skip_##exportName: \
        __asm ret \
    }

GL_THUNK(GlmfBeginGlsBlock, 0)
GL_THUNK(GlmfCloseMetaFile, 1)
GL_THUNK(GlmfEndGlsBlock, 2)
GL_THUNK(GlmfEndPlayback, 3)
GL_THUNK(GlmfInitPlayback, 4)
GL_THUNK(GlmfPlayGlsRecord, 5)
GL_THUNK(glAccum, 6)
GL_THUNK(glAlphaFunc, 7)
GL_THUNK(glAreTexturesResident, 8)
GL_THUNK(glArrayElement, 9)
GL_THUNK(glBegin, 10)
GL_THUNK(glBindTexture, 11)
GL_THUNK(glBitmap, 12)
GL_THUNK(glBlendFunc, 13)
GL_THUNK(glCallList, 14)
GL_THUNK(glCallLists, 15)
GL_THUNK(glClear, 16)
GL_THUNK(glClearAccum, 17)
GL_THUNK(glClearColor, 18)
GL_THUNK(glClearDepth, 19)
GL_THUNK(glClearIndex, 20)
GL_THUNK(glClearStencil, 21)
GL_THUNK(glClipPlane, 22)
GL_THUNK(glColor3b, 23)
GL_THUNK(glColor3bv, 24)
GL_THUNK(glColor3d, 25)
GL_THUNK(glColor3dv, 26)
GL_THUNK(glColor3f, 27)
GL_THUNK(glColor3fv, 28)
GL_THUNK(glColor3i, 29)
GL_THUNK(glColor3iv, 30)
GL_THUNK(glColor3s, 31)
GL_THUNK(glColor3sv, 32)
GL_THUNK(glColor3ub, 33)
GL_THUNK(glColor3ubv, 34)
GL_THUNK(glColor3ui, 35)
GL_THUNK(glColor3uiv, 36)
GL_THUNK(glColor3us, 37)
GL_THUNK(glColor3usv, 38)
GL_THUNK(glColor4b, 39)
GL_THUNK(glColor4bv, 40)
GL_THUNK(glColor4d, 41)
GL_THUNK(glColor4dv, 42)
GL_THUNK(glColor4f, 43)
GL_THUNK(glColor4fv, 44)
GL_THUNK(glColor4i, 45)
GL_THUNK(glColor4iv, 46)
GL_THUNK(glColor4s, 47)
GL_THUNK(glColor4sv, 48)
GL_THUNK(glColor4ub, 49)
GL_THUNK(glColor4ubv, 50)
GL_THUNK(glColor4ui, 51)
GL_THUNK(glColor4uiv, 52)
GL_THUNK(glColor4us, 53)
GL_THUNK(glColor4usv, 54)
GL_THUNK(glColorMask, 55)
GL_THUNK(glColorMaterial, 56)
GL_THUNK(glColorPointer, 57)
GL_THUNK(glCopyPixels, 58)
GL_THUNK(glCopyTexImage1D, 59)
GL_THUNK(glCopyTexImage2D, 60)
GL_THUNK(glCopyTexSubImage1D, 61)
GL_THUNK(glCopyTexSubImage2D, 62)
GL_THUNK(glCullFace, 63)
GL_THUNK(glDebugEntry, 64)
GL_THUNK(glDeleteLists, 65)
GL_THUNK(glDeleteTextures, 66)
GL_THUNK(glDepthFunc, 67)
GL_THUNK(glDepthMask, 68)
GL_THUNK(glDepthRange, 69)
GL_THUNK(glDisable, 70)
GL_THUNK(glDisableClientState, 71)
GL_THUNK(glDrawArrays, 72)
GL_THUNK(glDrawBuffer, 73)
GL_THUNK(glDrawElements, 74)
GL_THUNK(glDrawPixels, 75)
GL_THUNK(glEdgeFlag, 76)
GL_THUNK(glEdgeFlagPointer, 77)
GL_THUNK(glEdgeFlagv, 78)
GL_THUNK(glEnable, 79)
GL_THUNK(glEnableClientState, 80)
GL_THUNK(glEnd, 81)
GL_THUNK(glEndList, 82)
GL_THUNK(glEvalCoord1d, 83)
GL_THUNK(glEvalCoord1dv, 84)
GL_THUNK(glEvalCoord1f, 85)
GL_THUNK(glEvalCoord1fv, 86)
GL_THUNK(glEvalCoord2d, 87)
GL_THUNK(glEvalCoord2dv, 88)
GL_THUNK(glEvalCoord2f, 89)
GL_THUNK(glEvalCoord2fv, 90)
GL_THUNK(glEvalMesh1, 91)
GL_THUNK(glEvalMesh2, 92)
GL_THUNK(glEvalPoint1, 93)
GL_THUNK(glEvalPoint2, 94)
GL_THUNK(glFeedbackBuffer, 95)
GL_THUNK(glFinish, 96)
GL_THUNK(glFlush, 97)
GL_THUNK(glFogf, 98)
GL_THUNK(glFogfv, 99)
GL_THUNK(glFogi, 100)
GL_THUNK(glFogiv, 101)
GL_THUNK(glFrontFace, 102)
GL_THUNK(glFrustum, 103)
GL_THUNK(glGenLists, 104)
GL_THUNK(glGenTextures, 105)
GL_THUNK(glGetBooleanv, 106)
GL_THUNK(glGetClipPlane, 107)
GL_THUNK(glGetDoublev, 108)
GL_THUNK(glGetError, 109)
GL_THUNK(glGetFloatv, 110)
// glGetIntegerv (111) — overridden in gl_hooks.cpp to lie about GL_FRAMEBUFFER_BINDING
GL_THUNK(glGetLightfv, 112)
GL_THUNK(glGetLightiv, 113)
GL_THUNK(glGetMapdv, 114)
GL_THUNK(glGetMapfv, 115)
GL_THUNK(glGetMapiv, 116)
GL_THUNK(glGetMaterialfv, 117)
GL_THUNK(glGetMaterialiv, 118)
GL_THUNK(glGetPixelMapfv, 119)
GL_THUNK(glGetPixelMapuiv, 120)
GL_THUNK(glGetPixelMapusv, 121)
GL_THUNK(glGetPointerv, 122)
GL_THUNK(glGetPolygonStipple, 123)
GL_THUNK(glGetString, 124)
GL_THUNK(glGetTexEnvfv, 125)
GL_THUNK(glGetTexEnviv, 126)
GL_THUNK(glGetTexGendv, 127)
GL_THUNK(glGetTexGenfv, 128)
GL_THUNK(glGetTexGeniv, 129)
GL_THUNK(glGetTexImage, 130)
GL_THUNK(glGetTexLevelParameterfv, 131)
GL_THUNK(glGetTexLevelParameteriv, 132)
GL_THUNK(glGetTexParameterfv, 133)
GL_THUNK(glGetTexParameteriv, 134)
GL_THUNK(glHint, 135)
GL_THUNK(glIndexMask, 136)
GL_THUNK(glIndexPointer, 137)
GL_THUNK(glIndexd, 138)
GL_THUNK(glIndexdv, 139)
GL_THUNK(glIndexf, 140)
GL_THUNK(glIndexfv, 141)
GL_THUNK(glIndexi, 142)
GL_THUNK(glIndexiv, 143)
GL_THUNK(glIndexs, 144)
GL_THUNK(glIndexsv, 145)
GL_THUNK(glIndexub, 146)
GL_THUNK(glIndexubv, 147)
GL_THUNK(glInitNames, 148)
GL_THUNK(glInterleavedArrays, 149)
GL_THUNK(glIsEnabled, 150)
GL_THUNK(glIsList, 151)
GL_THUNK(glIsTexture, 152)
GL_THUNK(glLightModelf, 153)
GL_THUNK(glLightModelfv, 154)
GL_THUNK(glLightModeli, 155)
GL_THUNK(glLightModeliv, 156)
GL_THUNK(glLightf, 157)
GL_THUNK(glLightfv, 158)
GL_THUNK(glLighti, 159)
GL_THUNK(glLightiv, 160)
GL_THUNK(glLineStipple, 161)
GL_THUNK(glLineWidth, 162)
GL_THUNK(glListBase, 163)
GL_THUNK(glLoadIdentity, 164)
GL_THUNK(glLoadMatrixd, 165)
GL_THUNK(glLoadMatrixf, 166)
GL_THUNK(glLoadName, 167)
GL_THUNK(glLogicOp, 168)
GL_THUNK(glMap1d, 169)
GL_THUNK(glMap1f, 170)
GL_THUNK(glMap2d, 171)
GL_THUNK(glMap2f, 172)
GL_THUNK(glMapGrid1d, 173)
GL_THUNK(glMapGrid1f, 174)
GL_THUNK(glMapGrid2d, 175)
GL_THUNK(glMapGrid2f, 176)
GL_THUNK(glMaterialf, 177)
GL_THUNK(glMaterialfv, 178)
GL_THUNK(glMateriali, 179)
GL_THUNK(glMaterialiv, 180)
GL_THUNK(glMatrixMode, 181)
GL_THUNK(glMultMatrixd, 182)
GL_THUNK(glMultMatrixf, 183)
GL_THUNK(glNewList, 184)
GL_THUNK(glNormal3b, 185)
GL_THUNK(glNormal3bv, 186)
GL_THUNK(glNormal3d, 187)
GL_THUNK(glNormal3dv, 188)
GL_THUNK(glNormal3f, 189)
GL_THUNK(glNormal3fv, 190)
GL_THUNK(glNormal3i, 191)
GL_THUNK(glNormal3iv, 192)
GL_THUNK(glNormal3s, 193)
GL_THUNK(glNormal3sv, 194)
GL_THUNK(glNormalPointer, 195)
GL_THUNK(glOrtho, 196)
GL_THUNK(glPassThrough, 197)
GL_THUNK(glPixelMapfv, 198)
GL_THUNK(glPixelMapuiv, 199)
GL_THUNK(glPixelMapusv, 200)
GL_THUNK(glPixelStoref, 201)
GL_THUNK(glPixelStorei, 202)
GL_THUNK(glPixelTransferf, 203)
GL_THUNK(glPixelTransferi, 204)
GL_THUNK(glPixelZoom, 205)
GL_THUNK(glPointSize, 206)
GL_THUNK(glPolygonMode, 207)
GL_THUNK(glPolygonOffset, 208)
GL_THUNK(glPolygonStipple, 209)
GL_THUNK(glPopAttrib, 210)
GL_THUNK(glPopClientAttrib, 211)
GL_THUNK(glPopMatrix, 212)
GL_THUNK(glPopName, 213)
GL_THUNK(glPrioritizeTextures, 214)
GL_THUNK(glPushAttrib, 215)
GL_THUNK(glPushClientAttrib, 216)
GL_THUNK(glPushMatrix, 217)
GL_THUNK(glPushName, 218)
GL_THUNK(glRasterPos2d, 219)
GL_THUNK(glRasterPos2dv, 220)
GL_THUNK(glRasterPos2f, 221)
GL_THUNK(glRasterPos2fv, 222)
GL_THUNK(glRasterPos2i, 223)
GL_THUNK(glRasterPos2iv, 224)
GL_THUNK(glRasterPos2s, 225)
GL_THUNK(glRasterPos2sv, 226)
GL_THUNK(glRasterPos3d, 227)
GL_THUNK(glRasterPos3dv, 228)
GL_THUNK(glRasterPos3f, 229)
GL_THUNK(glRasterPos3fv, 230)
GL_THUNK(glRasterPos3i, 231)
GL_THUNK(glRasterPos3iv, 232)
GL_THUNK(glRasterPos3s, 233)
GL_THUNK(glRasterPos3sv, 234)
GL_THUNK(glRasterPos4d, 235)
GL_THUNK(glRasterPos4dv, 236)
GL_THUNK(glRasterPos4f, 237)
GL_THUNK(glRasterPos4fv, 238)
GL_THUNK(glRasterPos4i, 239)
GL_THUNK(glRasterPos4iv, 240)
GL_THUNK(glRasterPos4s, 241)
GL_THUNK(glRasterPos4sv, 242)
GL_THUNK(glReadBuffer, 243)
GL_THUNK(glReadPixels, 244)
GL_THUNK(glRectd, 245)
GL_THUNK(glRectdv, 246)
GL_THUNK(glRectf, 247)
GL_THUNK(glRectfv, 248)
GL_THUNK(glRecti, 249)
GL_THUNK(glRectiv, 250)
GL_THUNK(glRects, 251)
GL_THUNK(glRectsv, 252)
GL_THUNK(glRenderMode, 253)
GL_THUNK(glRotated, 254)
GL_THUNK(glRotatef, 255)
GL_THUNK(glScaled, 256)
GL_THUNK(glScalef, 257)
GL_THUNK(glScissor, 258)
GL_THUNK(glSelectBuffer, 259)
GL_THUNK(glShadeModel, 260)
GL_THUNK(glStencilFunc, 261)
GL_THUNK(glStencilMask, 262)
GL_THUNK(glStencilOp, 263)
GL_THUNK(glTexCoord1d, 264)
GL_THUNK(glTexCoord1dv, 265)
GL_THUNK(glTexCoord1f, 266)
GL_THUNK(glTexCoord1fv, 267)
GL_THUNK(glTexCoord1i, 268)
GL_THUNK(glTexCoord1iv, 269)
GL_THUNK(glTexCoord1s, 270)
GL_THUNK(glTexCoord1sv, 271)
GL_THUNK(glTexCoord2d, 272)
GL_THUNK(glTexCoord2dv, 273)
GL_THUNK(glTexCoord2f, 274)
GL_THUNK(glTexCoord2fv, 275)
GL_THUNK(glTexCoord2i, 276)
GL_THUNK(glTexCoord2iv, 277)
GL_THUNK(glTexCoord2s, 278)
GL_THUNK(glTexCoord2sv, 279)
GL_THUNK(glTexCoord3d, 280)
GL_THUNK(glTexCoord3dv, 281)
GL_THUNK(glTexCoord3f, 282)
GL_THUNK(glTexCoord3fv, 283)
GL_THUNK(glTexCoord3i, 284)
GL_THUNK(glTexCoord3iv, 285)
GL_THUNK(glTexCoord3s, 286)
GL_THUNK(glTexCoord3sv, 287)
GL_THUNK(glTexCoord4d, 288)
GL_THUNK(glTexCoord4dv, 289)
GL_THUNK(glTexCoord4f, 290)
GL_THUNK(glTexCoord4fv, 291)
GL_THUNK(glTexCoord4i, 292)
GL_THUNK(glTexCoord4iv, 293)
GL_THUNK(glTexCoord4s, 294)
GL_THUNK(glTexCoord4sv, 295)
GL_THUNK(glTexCoordPointer, 296)
GL_THUNK(glTexEnvf, 297)
GL_THUNK(glTexEnvfv, 298)
GL_THUNK(glTexEnvi, 299)
GL_THUNK(glTexEnviv, 300)
GL_THUNK(glTexGend, 301)
GL_THUNK(glTexGendv, 302)
GL_THUNK(glTexGenf, 303)
GL_THUNK(glTexGenfv, 304)
GL_THUNK(glTexGeni, 305)
GL_THUNK(glTexGeniv, 306)
GL_THUNK(glTexImage1D, 307)
GL_THUNK(glTexImage2D, 308)
GL_THUNK(glTexParameterf, 309)
GL_THUNK(glTexParameterfv, 310)
GL_THUNK(glTexParameteri, 311)
GL_THUNK(glTexParameteriv, 312)
GL_THUNK(glTexSubImage1D, 313)
GL_THUNK(glTexSubImage2D, 314)
GL_THUNK(glTranslated, 315)
GL_THUNK(glTranslatef, 316)
GL_THUNK(glVertex2d, 317)
GL_THUNK(glVertex2dv, 318)
GL_THUNK(glVertex2f, 319)
GL_THUNK(glVertex2fv, 320)
GL_THUNK(glVertex2i, 321)
GL_THUNK(glVertex2iv, 322)
GL_THUNK(glVertex2s, 323)
GL_THUNK(glVertex2sv, 324)
GL_THUNK(glVertex3d, 325)
GL_THUNK(glVertex3dv, 326)
GL_THUNK(glVertex3f, 327)
GL_THUNK(glVertex3fv, 328)
GL_THUNK(glVertex3i, 329)
GL_THUNK(glVertex3iv, 330)
GL_THUNK(glVertex3s, 331)
GL_THUNK(glVertex3sv, 332)
GL_THUNK(glVertex4d, 333)
GL_THUNK(glVertex4dv, 334)
GL_THUNK(glVertex4f, 335)
GL_THUNK(glVertex4fv, 336)
GL_THUNK(glVertex4i, 337)
GL_THUNK(glVertex4iv, 338)
GL_THUNK(glVertex4s, 339)
GL_THUNK(glVertex4sv, 340)
GL_THUNK(glVertexPointer, 341)
GL_THUNK(glViewport, 342)
GL_THUNK(wglChoosePixelFormat, 343)
GL_THUNK(wglCopyContext, 344)
GL_THUNK(wglCreateContext, 345)
GL_THUNK(wglCreateLayerContext, 346)
GL_THUNK(wglDeleteContext, 347)
GL_THUNK(wglDescribeLayerPlane, 348)
GL_THUNK(wglDescribePixelFormat, 349)
GL_THUNK(wglGetCurrentContext, 350)
GL_THUNK(wglGetCurrentDC, 351)
GL_THUNK(wglGetDefaultProcAddress, 352)
GL_THUNK(wglGetLayerPaletteEntries, 353)
GL_THUNK(wglGetPixelFormat, 354)
// wglGetProcAddress (355)  — overridden in gl_hooks.cpp (returns sys_glBindFramebuffer for GL FBO names)
// wglMakeCurrent    (356)  — overridden in gl_hooks.cpp (lazy-creates per-eye FBOs)
GL_THUNK(wglRealizeLayerPalette, 357)
GL_THUNK(wglSetLayerPaletteEntries, 358)
GL_THUNK(wglSetPixelFormat, 359)
GL_THUNK(wglShareLists, 360)
// wglSwapBuffers (361) — overridden in gl_hooks.cpp (per-eye FBO blit + real swap)
GL_THUNK(wglSwapLayerBuffers, 362)
GL_THUNK(wglSwapMultipleBuffers, 363)
GL_THUNK(wglUseFontBitmapsA, 364)
GL_THUNK(wglUseFontBitmapsW, 365)
GL_THUNK(wglUseFontOutlinesA, 366)
GL_THUNK(wglUseFontOutlinesW, 367)

#endif // _M_IX86

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
            Log("=== NvDirectMode " DISPLAYED_VERSION " - opengl32 proxy loaded ===\n");
            Log("Game exe: %ls\n", exePath);
            WCHAR proxyPath[MAX_PATH];
            GetModuleFileNameW(hModule, proxyPath, MAX_PATH);
            Log("Proxy DLL: %ls\n", proxyPath);
            Log("Config:    OutputMode=%d (%s)  WrapDevices=%d  SwapEyes=%d  LoggingEnabled=%d  VerboseLogging=%d\n",
                g_outputMode, NvDM_OutputIsTopBottom() ? "Top-and-Bottom" : "Side-by-Side",
                g_wrapDevices, g_swapEyes, g_loggingEnabled, g_verboseEnabled);
        }
        LoadRealOpenGL32();
        break;

    case DLL_PROCESS_DETACH:
        Log("=== NvDirectMode opengl32 proxy unloading ===\n");
        if (g_hVEH)
        {
            RemoveVectoredExceptionHandler(g_hVEH);
            g_hVEH = NULL;
        }
        if (g_hRealOpenGL32)
        {
            FreeLibrary(g_hRealOpenGL32);
            g_hRealOpenGL32 = NULL;
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
