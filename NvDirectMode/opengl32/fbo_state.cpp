/* NvDirectMode opengl32 - per-eye FBO state
 *
 * The two-FBO setup (color textures per eye + shared depth/stencil
 * renderbuffer) and the wglGetProcAddress-based hook strategy are adapted
 * from OGL-3DVision-Wrapper:
 *   https://github.com/Helifax/OpenGL3DVision
 *   Copyright (c) 2015 Octavian Mihai Vasilovici aka Helifax
 *   MIT License - see THIRD_PARTY_NOTICES.txt for full text.
 *
 * Conceptual changes vs the original:
 *   - The original wrapper performs ACTIVE stereoization (it analyses + injects
 *     vertex shaders to render the second eye from a single mono draw call).
 *     NvDirectMode does PASSIVE routing — the game already renders both eyes
 *     explicitly via NvAPI_Stereo_SetActiveEye, so we just route each draw to
 *     the correct FBO based on the active-eye state.
 *   - The original delivers stereo via GL/DX9 interop (wglDXLockObjectsNV) so
 *     NVIDIA's driver can present in 3D Vision. NvDirectMode just blits the
 *     active-eye FBO to the GL window backbuffer at swap time; downstream
 *     stereo composition (SR weaver / direct mode display) is a separate
 *     vertical not handled here.
 *   - The original uses an internal frame counter (1 ↔ 2) to drive eye
 *     selection. NvDirectMode reads NvAPI's active-eye via the
 *     Wiz3D_GetActiveEye bridge.
 */

#include "fbo_state.h"

#include <GL/gl.h>
#include "eye_state.h"
#include "log.h"
#include "../anaglyph_matrices.h"

// SR-Lib — static library wrapping the Simulated Reality SDK.
// All SR import libs are merged into SR-mt[d].lib; the final DLL must
// delay-load the SR runtime DLLs (see SR.hpp header comment for the list).
#include "SR.hpp"

#include <ctype.h>

// OutputMode flag from dllmain
extern "C" int NvDM_OutputIsTopBottom();
extern "C" int NvDM_SwapEyes();
extern "C" int NvDM_OutputMode();
extern "C" int NvDM_AnaglyphColour();
extern "C" int NvDM_AnaglyphMethod();
extern "C" int NvDM_ForceSRWeave();
extern "C" void NvDM_Log(const char* fmt, ...);
extern "C" int NvDM_VerboseEnabled();

// ---------------------------------------------------------------------------
// GL constants and types we need that are NOT in <GL/gl.h>'s GL 1.1 set.
// We avoid pulling in <GL/glext.h> to keep this TU's includes minimal.
// Values come from the OpenGL spec / glext.h public.
// ---------------------------------------------------------------------------
#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER                    0x8D40
#define GL_READ_FRAMEBUFFER               0x8CA8
#define GL_DRAW_FRAMEBUFFER               0x8CA9
#define GL_FRAMEBUFFER_BINDING            0x8CA6
#define GL_RENDERBUFFER                   0x8D41
#define GL_DEPTH_STENCIL_ATTACHMENT       0x821A
#define GL_DEPTH24_STENCIL8               0x88F0
#define GL_COLOR_ATTACHMENT0              0x8CE0
#define GL_FRAMEBUFFER_COMPLETE           0x8CD5
#endif

// GL constants for the shader-mode composite path (modes 4-7)
#ifndef GL_FRAGMENT_SHADER
#define GL_FRAGMENT_SHADER                0x8B30
#define GL_VERTEX_SHADER                  0x8B31
#define GL_COMPILE_STATUS                 0x8B81
#define GL_LINK_STATUS                    0x8B82
#define GL_INFO_LOG_LENGTH                0x8B84
#define GL_ARRAY_BUFFER                   0x8892
#define GL_STATIC_DRAW                    0x88E4
#define GL_TEXTURE0                       0x84C0
#define GL_TEXTURE1                       0x84C1
#endif

typedef void   (APIENTRY *PFN_glGenFramebuffers)(GLsizei n, GLuint* ids);
typedef void   (APIENTRY *PFN_glDeleteFramebuffers)(GLsizei n, const GLuint* ids);
typedef void   (APIENTRY *PFN_glBindFramebuffer)(GLenum target, GLuint id);
typedef void   (APIENTRY *PFN_glFramebufferTexture2D)(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level);
typedef void   (APIENTRY *PFN_glFramebufferRenderbuffer)(GLenum target, GLenum attachment, GLenum rbtarget, GLuint rb);
typedef void   (APIENTRY *PFN_glGenRenderbuffers)(GLsizei n, GLuint* ids);
typedef void   (APIENTRY *PFN_glDeleteRenderbuffers)(GLsizei n, const GLuint* ids);
typedef void   (APIENTRY *PFN_glBindRenderbuffer)(GLenum target, GLuint id);
typedef void   (APIENTRY *PFN_glRenderbufferStorage)(GLenum target, GLenum internalformat, GLsizei width, GLsizei height);
typedef GLenum (APIENTRY *PFN_glCheckFramebufferStatus)(GLenum target);
typedef void   (APIENTRY *PFN_glBlitFramebuffer)(GLint sx0, GLint sy0, GLint sx1, GLint sy1, GLint dx0, GLint dy0, GLint dx1, GLint dy1, GLbitfield mask, GLenum filter);

// Shader / program / VBO entry points used for OutputMode 4-7 composite paths.
// All resolved via wglGetProcAddress like the FBO ones above.
typedef GLuint (APIENTRY *PFN_glCreateShader)(GLenum type);
typedef void   (APIENTRY *PFN_glShaderSource)(GLuint shader, GLsizei count, const char* const* string, const GLint* length);
typedef void   (APIENTRY *PFN_glCompileShader)(GLuint shader);
typedef void   (APIENTRY *PFN_glGetShaderiv)(GLuint shader, GLenum pname, GLint* params);
typedef void   (APIENTRY *PFN_glGetShaderInfoLog)(GLuint shader, GLsizei bufSize, GLsizei* length, char* infoLog);
typedef void   (APIENTRY *PFN_glDeleteShader)(GLuint shader);
typedef GLuint (APIENTRY *PFN_glCreateProgram)(void);
typedef void   (APIENTRY *PFN_glAttachShader)(GLuint program, GLuint shader);
typedef void   (APIENTRY *PFN_glLinkProgram)(GLuint program);
typedef void   (APIENTRY *PFN_glGetProgramiv)(GLuint program, GLenum pname, GLint* params);
typedef void   (APIENTRY *PFN_glDeleteProgram)(GLuint program);
typedef void   (APIENTRY *PFN_glUseProgram)(GLuint program);
typedef GLint  (APIENTRY *PFN_glGetUniformLocation)(GLuint program, const char* name);
typedef void   (APIENTRY *PFN_glUniform1i)(GLint location, GLint v0);
typedef void   (APIENTRY *PFN_glUniform4fv)(GLint location, GLsizei count, const GLfloat* value);
typedef void   (APIENTRY *PFN_glActiveTexture)(GLenum texture);
typedef void   (APIENTRY *PFN_glGenBuffers)(GLsizei n, GLuint* buffers);
typedef void   (APIENTRY *PFN_glDeleteBuffers)(GLsizei n, const GLuint* buffers);
typedef void   (APIENTRY *PFN_glBindBuffer)(GLenum target, GLuint buffer);
typedef void   (APIENTRY *PFN_glBufferData)(GLenum target, ptrdiff_t size, const void* data, GLenum usage);
typedef void   (APIENTRY *PFN_glEnableVertexAttribArray)(GLuint index);
typedef void   (APIENTRY *PFN_glVertexAttribPointer)(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void* pointer);
typedef void   (APIENTRY *PFN_glBindAttribLocation)(GLuint program, GLuint index, const char* name);
typedef void   (APIENTRY *PFN_glViewport)(GLint x, GLint y, GLsizei width, GLsizei height);
typedef void   (APIENTRY *PFN_glDrawArrays)(GLenum mode, GLint first, GLsizei count);
typedef void   (APIENTRY *PFN_glDisable)(GLenum cap);

// ---------------------------------------------------------------------------
// dllmain owns g_hRealOpenGL32 + the orig_wglGetProcAddress entry. We resolve
// extension fns via that. Forward-declared here.
// ---------------------------------------------------------------------------
extern "C" HMODULE g_hRealOpenGL32;
typedef PROC (WINAPI* pfnWglGetProcAddress)(LPCSTR);
extern "C" pfnWglGetProcAddress orig_wglGetProcAddress;

namespace NvDirectMode
{

namespace
{
    // Resolved GL extension entry points
    PFN_glGenFramebuffers          p_glGenFramebuffers          = nullptr;
    PFN_glDeleteFramebuffers       p_glDeleteFramebuffers       = nullptr;
    PFN_glBindFramebuffer          p_glBindFramebuffer          = nullptr;
    PFN_glFramebufferTexture2D     p_glFramebufferTexture2D     = nullptr;
    PFN_glFramebufferRenderbuffer  p_glFramebufferRenderbuffer  = nullptr;
    PFN_glGenRenderbuffers         p_glGenRenderbuffers         = nullptr;
    PFN_glDeleteRenderbuffers      p_glDeleteRenderbuffers      = nullptr;
    PFN_glBindRenderbuffer         p_glBindRenderbuffer         = nullptr;
    PFN_glRenderbufferStorage      p_glRenderbufferStorage      = nullptr;
    PFN_glCheckFramebufferStatus   p_glCheckFramebufferStatus   = nullptr;
    PFN_glBlitFramebuffer          p_glBlitFramebuffer          = nullptr;

    // GL 1.1 entry points (resolved from g_hRealOpenGL32 directly)
    typedef void (APIENTRY *PFN_glGenTextures)(GLsizei n, GLuint* textures);
    typedef void (APIENTRY *PFN_glBindTexture)(GLenum target, GLuint texture);
    typedef void (APIENTRY *PFN_glTexImage2D)(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const void* pixels);
    typedef void (APIENTRY *PFN_glTexParameteri)(GLenum target, GLenum pname, GLint param);
    typedef void (APIENTRY *PFN_glDeleteTextures)(GLsizei n, const GLuint* textures);
    PFN_glGenTextures              p_glGenTextures              = nullptr;
    PFN_glBindTexture              p_glBindTexture              = nullptr;
    PFN_glTexImage2D               p_glTexImage2D               = nullptr;
    PFN_glTexParameteri            p_glTexParameteri            = nullptr;
    PFN_glDeleteTextures           p_glDeleteTextures           = nullptr;

    // Per-eye FBO state
    struct EyeFbos
    {
        GLuint fboLeft         = 0;
        GLuint fboRight        = 0;
        GLuint texLeft         = 0;
        GLuint texRight        = 0;
        GLuint depthStencilRb  = 0;
        int    width           = 0;
        int    height          = 0;
        bool   ready           = false;
    };
    EyeFbos g_fbos;

    // OutputMode 8 SR weave state. SR runtime DLLs are delay-loaded; first
    // RunSRWeave() call lazy-creates the SR-Lib interface + 2W × H SBS
    // intermediate texture + its FBO wrapper. ReleaseSRPipeline() tears it
    // all down on context loss / window resize / DLL detach.
    struct SRState
    {
        bool                              blacklistedOrFailed = false;
        SimulatedReality::SRInterfaceOGL* srInterfaceOGL      = nullptr;

        GLuint sbsTex              = 0;         // 2W × H, RGBA8
        GLuint sbsFbo              = 0;         // FBO whose color attachment is sbsTex
        int    sbsW                = 0;
        int    sbsH                = 0;
    };
    SRState g_sr;

    // Shader / program / VBO entry points for OutputMode 4-7.
    PFN_glCreateShader            p_glCreateShader            = nullptr;
    PFN_glShaderSource            p_glShaderSource            = nullptr;
    PFN_glCompileShader           p_glCompileShader           = nullptr;
    PFN_glGetShaderiv             p_glGetShaderiv             = nullptr;
    PFN_glGetShaderInfoLog        p_glGetShaderInfoLog        = nullptr;
    PFN_glDeleteShader            p_glDeleteShader            = nullptr;
    PFN_glCreateProgram           p_glCreateProgram           = nullptr;
    PFN_glAttachShader            p_glAttachShader            = nullptr;
    PFN_glLinkProgram             p_glLinkProgram             = nullptr;
    PFN_glGetProgramiv            p_glGetProgramiv            = nullptr;
    PFN_glDeleteProgram           p_glDeleteProgram           = nullptr;
    PFN_glUseProgram              p_glUseProgram              = nullptr;
    PFN_glGetUniformLocation      p_glGetUniformLocation      = nullptr;
    PFN_glUniform1i               p_glUniform1i               = nullptr;
    PFN_glUniform4fv              p_glUniform4fv              = nullptr;
    PFN_glActiveTexture           p_glActiveTexture           = nullptr;
    PFN_glGenBuffers              p_glGenBuffers              = nullptr;
    PFN_glDeleteBuffers           p_glDeleteBuffers           = nullptr;
    PFN_glBindBuffer              p_glBindBuffer              = nullptr;
    PFN_glBufferData              p_glBufferData              = nullptr;
    PFN_glEnableVertexAttribArray p_glEnableVertexAttribArray = nullptr;
    PFN_glVertexAttribPointer     p_glVertexAttribPointer     = nullptr;
    PFN_glBindAttribLocation      p_glBindAttribLocation      = nullptr;
    PFN_glViewport                p_glViewport                = nullptr;
    PFN_glDrawArrays              p_glDrawArrays              = nullptr;
    PFN_glDisable                 p_glDisable                 = nullptr;

    // OutputMode 4-7 shader pipeline state. One program per mode (Line/Col/
    // Checker/Anaglyph), shared fullscreen-quad VBO. Compiled lazily on first
    // composite. Anaglyph uniform array carries the selected coefficients.
    enum ShaderProgIdx { PROG_LINE = 0, PROG_COL = 1, PROG_CHECKER = 2, PROG_ANAGLYPH = 3, PROG_COUNT = 4 };
    struct CompositePrograms
    {
        GLuint prog[PROG_COUNT]    = {};
        GLint  locLeftTex[PROG_COUNT]  = { -1, -1, -1, -1 };
        GLint  locRightTex[PROG_COUNT] = { -1, -1, -1, -1 };
        GLint  locAnaglyphRows = -1;     // valid only on PROG_ANAGLYPH (slot 3)
        GLuint quadVBO         = 0;
        bool   compileFailed   = false;  // set if any compile/link errored — won't retry
    };
    CompositePrograms g_progs;

    template<typename T>
    bool ResolveExt(T& dst, const char* name)
    {
        if (!orig_wglGetProcAddress) return false;
        dst = (T)orig_wglGetProcAddress(name);
        return dst != nullptr;
    }
}

bool ResolveFboExtensions()
{
    if (!g_hRealOpenGL32) return false;

    // GL 1.1 fns: pull directly from opengl32.dll
    p_glGenTextures   = (PFN_glGenTextures)  GetProcAddress(g_hRealOpenGL32, "glGenTextures");
    p_glBindTexture   = (PFN_glBindTexture)  GetProcAddress(g_hRealOpenGL32, "glBindTexture");
    p_glTexImage2D    = (PFN_glTexImage2D)   GetProcAddress(g_hRealOpenGL32, "glTexImage2D");
    p_glTexParameteri = (PFN_glTexParameteri)GetProcAddress(g_hRealOpenGL32, "glTexParameteri");
    p_glDeleteTextures= (PFN_glDeleteTextures)GetProcAddress(g_hRealOpenGL32, "glDeleteTextures");

    // FBO + blit: GL 3.0 extensions, pulled via orig_wglGetProcAddress
    bool ok = true;
    ok &= ResolveExt(p_glGenFramebuffers,         "glGenFramebuffers");
    ok &= ResolveExt(p_glDeleteFramebuffers,      "glDeleteFramebuffers");
    ok &= ResolveExt(p_glBindFramebuffer,         "glBindFramebuffer");
    ok &= ResolveExt(p_glFramebufferTexture2D,    "glFramebufferTexture2D");
    ok &= ResolveExt(p_glFramebufferRenderbuffer, "glFramebufferRenderbuffer");
    ok &= ResolveExt(p_glGenRenderbuffers,        "glGenRenderbuffers");
    ok &= ResolveExt(p_glDeleteRenderbuffers,     "glDeleteRenderbuffers");
    ok &= ResolveExt(p_glBindRenderbuffer,        "glBindRenderbuffer");
    ok &= ResolveExt(p_glRenderbufferStorage,     "glRenderbufferStorage");
    ok &= ResolveExt(p_glCheckFramebufferStatus,  "glCheckFramebufferStatus");
    ok &= ResolveExt(p_glBlitFramebuffer,         "glBlitFramebuffer");

    // Some pre-3.0 contexts may only expose GL_EXT_framebuffer_object — try the
    // EXT-suffixed names for fns we couldn't resolve. Both forms are ABI-compatible.
    if (!p_glGenFramebuffers)         ResolveExt(p_glGenFramebuffers,         "glGenFramebuffersEXT");
    if (!p_glDeleteFramebuffers)      ResolveExt(p_glDeleteFramebuffers,      "glDeleteFramebuffersEXT");
    if (!p_glBindFramebuffer)         ResolveExt(p_glBindFramebuffer,         "glBindFramebufferEXT");
    if (!p_glFramebufferTexture2D)    ResolveExt(p_glFramebufferTexture2D,    "glFramebufferTexture2DEXT");
    if (!p_glFramebufferRenderbuffer) ResolveExt(p_glFramebufferRenderbuffer, "glFramebufferRenderbufferEXT");
    if (!p_glGenRenderbuffers)        ResolveExt(p_glGenRenderbuffers,        "glGenRenderbuffersEXT");
    if (!p_glDeleteRenderbuffers)     ResolveExt(p_glDeleteRenderbuffers,     "glDeleteRenderbuffersEXT");
    if (!p_glBindRenderbuffer)        ResolveExt(p_glBindRenderbuffer,        "glBindRenderbufferEXT");
    if (!p_glRenderbufferStorage)     ResolveExt(p_glRenderbufferStorage,     "glRenderbufferStorageEXT");
    if (!p_glCheckFramebufferStatus)  ResolveExt(p_glCheckFramebufferStatus,  "glCheckFramebufferStatusEXT");

    // Shader/program/VBO entry points (modes 4-7). Resolution failure here
    // is non-fatal — the SBS / TB blit path doesn't need them. We just refuse
    // to enter the shader-mode composite path if these aren't all present.
    ResolveExt(p_glCreateShader,            "glCreateShader");
    ResolveExt(p_glShaderSource,            "glShaderSource");
    ResolveExt(p_glCompileShader,           "glCompileShader");
    ResolveExt(p_glGetShaderiv,             "glGetShaderiv");
    ResolveExt(p_glGetShaderInfoLog,        "glGetShaderInfoLog");
    ResolveExt(p_glDeleteShader,            "glDeleteShader");
    ResolveExt(p_glCreateProgram,           "glCreateProgram");
    ResolveExt(p_glAttachShader,            "glAttachShader");
    ResolveExt(p_glLinkProgram,             "glLinkProgram");
    ResolveExt(p_glGetProgramiv,            "glGetProgramiv");
    ResolveExt(p_glDeleteProgram,           "glDeleteProgram");
    ResolveExt(p_glUseProgram,              "glUseProgram");
    ResolveExt(p_glGetUniformLocation,      "glGetUniformLocation");
    ResolveExt(p_glUniform1i,               "glUniform1i");
    ResolveExt(p_glUniform4fv,              "glUniform4fv");
    ResolveExt(p_glActiveTexture,           "glActiveTexture");
    ResolveExt(p_glGenBuffers,              "glGenBuffers");
    ResolveExt(p_glDeleteBuffers,           "glDeleteBuffers");
    ResolveExt(p_glBindBuffer,              "glBindBuffer");
    ResolveExt(p_glBufferData,              "glBufferData");
    ResolveExt(p_glEnableVertexAttribArray, "glEnableVertexAttribArray");
    ResolveExt(p_glVertexAttribPointer,     "glVertexAttribPointer");
    ResolveExt(p_glBindAttribLocation,      "glBindAttribLocation");
    if (g_hRealOpenGL32)
    {
        // glViewport / glDrawArrays / glDisable are GL 1.x — pull from
        // opengl32.dll directly (wglGetProcAddress can return NULL for them).
        p_glViewport   = (PFN_glViewport)  GetProcAddress(g_hRealOpenGL32, "glViewport");
        p_glDrawArrays = (PFN_glDrawArrays)GetProcAddress(g_hRealOpenGL32, "glDrawArrays");
        p_glDisable    = (PFN_glDisable)   GetProcAddress(g_hRealOpenGL32, "glDisable");
    }

    return p_glGenFramebuffers      && p_glBindFramebuffer        &&
           p_glFramebufferTexture2D && p_glFramebufferRenderbuffer &&
           p_glGenRenderbuffers     && p_glRenderbufferStorage     &&
           p_glBlitFramebuffer      && p_glGenTextures             &&
           p_glBindTexture          && p_glTexImage2D              &&
           p_glTexParameteri;
}

bool EyeFbosCreate(int width, int height)
{
    if (!ResolveFboExtensions()) return false;
    if (width <= 0 || height <= 0) return false;
    if (g_fbos.ready && g_fbos.width == width && g_fbos.height == height) return true;
    EyeFbosDestroy();

    // Color textures (one per eye)
    p_glGenTextures(1, &g_fbos.texLeft);
    p_glBindTexture(GL_TEXTURE_2D, g_fbos.texLeft);
    p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    p_glGenTextures(1, &g_fbos.texRight);
    p_glBindTexture(GL_TEXTURE_2D, g_fbos.texRight);
    p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    p_glBindTexture(GL_TEXTURE_2D, 0);

    // Shared depth/stencil renderbuffer
    p_glGenRenderbuffers(1, &g_fbos.depthStencilRb);
    p_glBindRenderbuffer(GL_RENDERBUFFER, g_fbos.depthStencilRb);
    p_glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height);
    p_glBindRenderbuffer(GL_RENDERBUFFER, 0);

    // FBO_left
    p_glGenFramebuffers(1, &g_fbos.fboLeft);
    p_glBindFramebuffer(GL_FRAMEBUFFER, g_fbos.fboLeft);
    p_glFramebufferTexture2D   (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,        GL_TEXTURE_2D, g_fbos.texLeft, 0);
    p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, g_fbos.depthStencilRb);
    GLenum sl = p_glCheckFramebufferStatus ? p_glCheckFramebufferStatus(GL_FRAMEBUFFER) : GL_FRAMEBUFFER_COMPLETE;

    // FBO_right (shares depth/stencil with left)
    p_glGenFramebuffers(1, &g_fbos.fboRight);
    p_glBindFramebuffer(GL_FRAMEBUFFER, g_fbos.fboRight);
    p_glFramebufferTexture2D   (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,        GL_TEXTURE_2D, g_fbos.texRight, 0);
    p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, g_fbos.depthStencilRb);
    GLenum sr = p_glCheckFramebufferStatus ? p_glCheckFramebufferStatus(GL_FRAMEBUFFER) : GL_FRAMEBUFFER_COMPLETE;

    // Restore default binding so we don't leave game state altered
    p_glBindFramebuffer(GL_FRAMEBUFFER, 0);

    g_fbos.width  = width;
    g_fbos.height = height;
    g_fbos.ready  = (sl == GL_FRAMEBUFFER_COMPLETE) && (sr == GL_FRAMEBUFFER_COMPLETE);
    return g_fbos.ready;
}

static void ReleaseSRPipeline();   // forward decl — defined below

void EyeFbosDestroy()
{
    ReleaseSRPipeline();
    if (p_glDeleteFramebuffers)
    {
        if (g_fbos.fboLeft)  p_glDeleteFramebuffers(1, &g_fbos.fboLeft);
        if (g_fbos.fboRight) p_glDeleteFramebuffers(1, &g_fbos.fboRight);
    }
    if (p_glDeleteRenderbuffers && g_fbos.depthStencilRb)
        p_glDeleteRenderbuffers(1, &g_fbos.depthStencilRb);
    if (p_glDeleteTextures)
    {
        if (g_fbos.texLeft)  p_glDeleteTextures(1, &g_fbos.texLeft);
        if (g_fbos.texRight) p_glDeleteTextures(1, &g_fbos.texRight);
    }
    g_fbos = EyeFbos{};
}

// ---------------------------------------------------------------------------
// SR weave (OutputMode 8) — mirror of the DX9/DX10/DX11 path but using GL.
//
// Pipeline: per frame
//   1. glBlitFramebuffer left-eye  FBO  ->  left half  of m_srSBSFbo
//   2. glBlitFramebuffer right-eye FBO  ->  right half of m_srSBSFbo
//   3. glBindFramebuffer(GL_FRAMEBUFFER, 0)
//   4. weaver->setInputViewTexture(m_srSBSTex, 2W, H, GL_RGBA8)
//   5. weaver->weave()  — writes to the now-bound default framebuffer
//
// SR DLLs are DelayLoad'd; missing runtime cleanly downgrades to SBS via
// SafeSRContextCreate's SEH catch on 0xC06D007E (MOD_NOT_FOUND).
// ---------------------------------------------------------------------------
#ifndef GL_RGBA8
#define GL_RGBA8                          0x8058
#endif
#ifndef GL_LINEAR
#define GL_LINEAR                         0x2601
#endif
#ifndef GL_TEXTURE_2D
#define GL_TEXTURE_2D                     0x0DE1
#endif
#ifndef GL_TEXTURE_MIN_FILTER
#define GL_TEXTURE_MIN_FILTER             0x2801
#endif
#ifndef GL_TEXTURE_MAG_FILTER
#define GL_TEXTURE_MAG_FILTER             0x2800
#endif
#ifndef GL_RGBA
#define GL_RGBA                           0x1908
#endif
#ifndef GL_UNSIGNED_BYTE
#define GL_UNSIGNED_BYTE                  0x1401
#endif
#ifndef GL_COLOR_BUFFER_BIT
#define GL_COLOR_BUFFER_BIT               0x00004000
#endif

// Mirror of the d3d11 blacklist. Keep aligned manually.
static bool IsSRIncompatibleExe()
{
    wchar_t exePath[MAX_PATH] = {};
    if (!GetModuleFileNameW(nullptr, exePath, MAX_PATH)) return false;
    for (wchar_t* p = exePath; *p; ++p) *p = (wchar_t)towlower(*p);
    static const wchar_t* const kBlacklist[] = {
        L"tombraider.exe",   // TR2013 — see project_tr2013_sr_dead_end memory
    };
    for (auto entry : kBlacklist)
        if (wcsstr(exePath, entry)) return true;
    return false;
}

static void ReleaseSRPipeline()
{
    if (g_sr.sbsFbo && p_glDeleteFramebuffers)
        p_glDeleteFramebuffers(1, &g_sr.sbsFbo);
    if (g_sr.sbsTex && p_glDeleteTextures)
        p_glDeleteTextures(1, &g_sr.sbsTex);
    g_sr.sbsFbo = 0; g_sr.sbsTex = 0; g_sr.sbsW = 0; g_sr.sbsH = 0;

    if (g_sr.srInterfaceOGL)
    {
        g_sr.srInterfaceOGL->Delete();
        g_sr.srInterfaceOGL = nullptr;
    }
}

static bool EnsureSRSBSTexture()
{
    if (!p_glGenTextures || !p_glBindTexture || !p_glTexImage2D ||
        !p_glTexParameteri || !p_glGenFramebuffers ||
        !p_glBindFramebuffer || !p_glFramebufferTexture2D ||
        !p_glCheckFramebufferStatus)
        return false;
    if (g_fbos.width == 0 || g_fbos.height == 0) return false;

    int wantW = g_fbos.width * 2;
    int wantH = g_fbos.height;
    if (g_sr.sbsTex && g_sr.sbsFbo && g_sr.sbsW == wantW && g_sr.sbsH == wantH)
        return true;

    if (g_sr.sbsFbo) { p_glDeleteFramebuffers(1, &g_sr.sbsFbo); g_sr.sbsFbo = 0; }
    if (g_sr.sbsTex) { p_glDeleteTextures(1, &g_sr.sbsTex);    g_sr.sbsTex = 0; }

    GLuint tex = 0;
    p_glGenTextures(1, &tex);
    if (!tex) return false;
    p_glBindTexture(GL_TEXTURE_2D, tex);
    p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, wantW, wantH, 0,
                   GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    p_glBindTexture(GL_TEXTURE_2D, 0);

    GLuint fbo = 0;
    p_glGenFramebuffers(1, &fbo);
    if (!fbo) { p_glDeleteTextures(1, &tex); return false; }
    p_glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                             GL_TEXTURE_2D, tex, 0);
    GLenum status = p_glCheckFramebufferStatus(GL_FRAMEBUFFER);
    p_glBindFramebuffer(GL_FRAMEBUFFER, 0);

    if (status != GL_FRAMEBUFFER_COMPLETE)
    {
        p_glDeleteFramebuffers(1, &fbo);
        p_glDeleteTextures(1, &tex);
        NvDM_Log("  opengl32 EnsureSRSBSTexture: FBO incomplete (status=0x%X)\n",
                 (unsigned)status);
        return false;
    }

    g_sr.sbsTex = tex; g_sr.sbsFbo = fbo;
    g_sr.sbsW = wantW;  g_sr.sbsH = wantH;

    // Bind SBS texture to the SR interface once per texture creation (not per frame).
    if (g_sr.srInterfaceOGL)
        g_sr.srInterfaceOGL->SetInputTexture(g_sr.sbsTex);

    return true;
}

static bool EnsureSRWeaver()
{
    if (g_sr.blacklistedOrFailed) return false;
    if (g_sr.srInterfaceOGL) return true;

    static bool s_blacklistChecked = false;
    static bool s_isBlacklisted    = false;
    if (!s_blacklistChecked)
    {
        s_blacklistChecked = true;
        s_isBlacklisted    = IsSRIncompatibleExe();
        if (s_isBlacklisted)
        {
            if (NvDM_ForceSRWeave())
            {
                NvDM_Log("  opengl32 EnsureSRWeaver: exe is SR-blacklisted but ForceSRWeave=1 — attempting anyway (diagnostic)\n");
                s_isBlacklisted = false;
            }
            else if (NvDM_VerboseEnabled())
            {
                NvDM_Log("  opengl32 EnsureSRWeaver: exe is SR-blacklisted; falling back to SBS (set ForceSRWeave=1 to override)\n");
            }
        }
    }
    if (s_isBlacklisted) { g_sr.blacklistedOrFailed = true; return false; }

    // HWND from the current GL context's HDC. Resolve wglGetCurrentDC via
    // g_hRealOpenGL32 — our own DLL IS opengl32 from the game's POV, so we
    // can't directly link against opengl32.lib's wglGetCurrentDC; calling
    // our own exported thunk works but going to the real one is cleaner.
    typedef HDC (WINAPI *PFN_wglGetCurrentDC)();
    static PFN_wglGetCurrentDC s_pWglGetCurrentDC = nullptr;
    if (!s_pWglGetCurrentDC && g_hRealOpenGL32)
        s_pWglGetCurrentDC = (PFN_wglGetCurrentDC)
            GetProcAddress(g_hRealOpenGL32, "wglGetCurrentDC");
    HDC  hdc  = s_pWglGetCurrentDC ? s_pWglGetCurrentDC() : nullptr;
    HWND hWnd = hdc ? WindowFromDC(hdc) : nullptr;

    // SR-Lib: single-call init — creates SRContext + weaver internally.
    HRESULT hr = SimulatedReality::CreateSRInterfaceOGL(hWnd, &g_sr.srInterfaceOGL);
    if (FAILED(hr) || !g_sr.srInterfaceOGL)
    {
        NvDM_Log("  opengl32 EnsureSRWeaver: CreateSRInterfaceOGL failed (hr=0x%08lX hWnd=%p)\n",
                 hr, (void*)hWnd);
        g_sr.blacklistedOrFailed = true;
        return false;
    }

    NvDM_Log("  opengl32 EnsureSRWeaver: ready (hWnd=%p srInterface=%p)\n",
             (void*)hWnd, (void*)g_sr.srInterfaceOGL);
    return true;
}

static bool RunSRWeave()
{
    if (!g_fbos.ready) return false;
    if (!EnsureSRWeaver())     return false;
    if (!EnsureSRSBSTexture()) return false;

    NVDM_TRACE_FIRST_N(5, "  opengl32 RunSRWeave: entry sbsTex=%u sbsFbo=%u %dx%d\n",
                       g_sr.sbsTex, g_sr.sbsFbo, g_sr.sbsW, g_sr.sbsH);

    const int W = g_fbos.width;
    const int H = g_fbos.height;
    const bool swap = (NvDM_SwapEyes() != 0);
    GLuint leftSlotFbo  = swap ? g_fbos.fboRight : g_fbos.fboLeft;
    GLuint rightSlotFbo = swap ? g_fbos.fboLeft  : g_fbos.fboRight;

    // Step A: blit each eye's color attachment into the corresponding half
    // of the SBS FBO. Linear filter — eye dims match each half exactly so
    // it's effectively a 1:1 copy.
    p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_sr.sbsFbo);
    p_glBindFramebuffer(GL_READ_FRAMEBUFFER, leftSlotFbo);
    p_glBlitFramebuffer(0, 0, W, H,
                        0, 0, W, H,
                        GL_COLOR_BUFFER_BIT, GL_LINEAR);
    p_glBindFramebuffer(GL_READ_FRAMEBUFFER, rightSlotFbo);
    p_glBlitFramebuffer(0, 0, W, H,
                        W, 0, 2 * W, H,
                        GL_COLOR_BUFFER_BIT, GL_LINEAR);

    // Step B: bind default FB and call Weave() — the SR-Lib interface
    // already has the SBS texture from SetInputTexture (bound once in
    // EnsureSRSBSTexture).
    p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (p_glViewport) p_glViewport(0, 0, W, H);

    NVDM_TRACE_FIRST_N(5, "  opengl32 RunSRWeave: weave call - srInterface=%p\n", (void*)g_sr.srInterfaceOGL);
    g_sr.srInterfaceOGL->Weave();
    NVDM_TRACE_FIRST_N(5, "  opengl32 RunSRWeave: weave returned OK\n");

    static volatile long s_weaveCount = 0;
    long n = _InterlockedIncrement(&s_weaveCount);
    if (n == 60 || n == 180 || n == 600 || n == 1800)
        NvDM_Log("  opengl32 RunSRWeave: heartbeat #%ld (SR still alive)\n", n);

    return true;
}

void EyeFbosBindForActiveEye(GLenum target)
{
    if (!g_fbos.ready || !p_glBindFramebuffer)
    {
        if (p_glBindFramebuffer) p_glBindFramebuffer(target, 0);
        return;
    }
    const int eye = GetActiveEye();
    GLuint id = (eye == kEyeRight) ? g_fbos.fboRight
              : (eye == kEyeLeft)  ? g_fbos.fboLeft
              :                       0;          // MONO -> real default
    p_glBindFramebuffer(target, id);
}

// ---------------------------------------------------------------------------
// OutputMode 4-7 composite shaders + helpers
//
// Common vertex shader: a fullscreen quad in NDC (-1..1) with vUV = (0..1).
// Fragment shaders sample the two eye textures and select per-pixel based on
// gl_FragCoord (interleaved/checkerboard) or blend channel-wise (anaglyph).
// GLSL 120 syntax keeps us compatible with older OpenGL contexts; gl_FragCoord
// and texture2D are both available there.
// ---------------------------------------------------------------------------
namespace
{
    const char* const kCompositeVS =
        "#version 120\n"
        "attribute vec2 aPos;\n"
        "varying   vec2 vUV;\n"
        "void main() {\n"
        "  vUV = aPos * 0.5 + 0.5;\n"
        "  gl_Position = vec4(aPos, 0.0, 1.0);\n"
        "}\n";

    const char* const kCompositeFS_Line =
        "#version 120\n"
        "uniform sampler2D leftTex;\n"
        "uniform sampler2D rightTex;\n"
        "varying vec2 vUV;\n"
        "void main() {\n"
        "  vec4 L = texture2D(leftTex,  vUV);\n"
        "  vec4 R = texture2D(rightTex, vUV);\n"
        "  float ev = mod(floor(gl_FragCoord.y), 2.0);\n"
        "  gl_FragColor = (ev < 1.0) ? L : R;\n"
        "}\n";

    const char* const kCompositeFS_Col =
        "#version 120\n"
        "uniform sampler2D leftTex;\n"
        "uniform sampler2D rightTex;\n"
        "varying vec2 vUV;\n"
        "void main() {\n"
        "  vec4 L = texture2D(leftTex,  vUV);\n"
        "  vec4 R = texture2D(rightTex, vUV);\n"
        "  float ev = mod(floor(gl_FragCoord.x), 2.0);\n"
        "  gl_FragColor = (ev < 1.0) ? L : R;\n"
        "}\n";

    const char* const kCompositeFS_Checker =
        "#version 120\n"
        "uniform sampler2D leftTex;\n"
        "uniform sampler2D rightTex;\n"
        "varying vec2 vUV;\n"
        "void main() {\n"
        "  vec4 L = texture2D(leftTex,  vUV);\n"
        "  vec4 R = texture2D(rightTex, vUV);\n"
        "  float ev = mod(floor(gl_FragCoord.x) + floor(gl_FragCoord.y), 2.0);\n"
        "  gl_FragColor = (ev < 1.0) ? L : R;\n"
        "}\n";

    // Anaglyph: 6 vec4 rows uploaded via glUniform4fv to gAnaglyphRows[].
    // GLSL 120 lacks UBOs; an array uniform handles it cleanly.
    const char* const kCompositeFS_Anaglyph =
        "#version 120\n"
        "uniform sampler2D leftTex;\n"
        "uniform sampler2D rightTex;\n"
        "uniform vec4 gAnaglyphRows[6];\n"  // lR, lG, lB, rR, rG, rB
        "varying vec2 vUV;\n"
        "void main() {\n"
        "  vec3 L = texture2D(leftTex,  vUV).rgb;\n"
        "  vec3 R = texture2D(rightTex, vUV).rgb;\n"
        "  vec3 a;\n"
        "  a.r = dot(gAnaglyphRows[0].xyz, L) + dot(gAnaglyphRows[3].xyz, R);\n"
        "  a.g = dot(gAnaglyphRows[1].xyz, L) + dot(gAnaglyphRows[4].xyz, R);\n"
        "  a.b = dot(gAnaglyphRows[2].xyz, L) + dot(gAnaglyphRows[5].xyz, R);\n"
        "  gl_FragColor = vec4(clamp(a, 0.0, 1.0), 1.0);\n"
        "}\n";

    // Compile + link a VS+FS pair. Returns 0 on failure (with the failure
    // recorded via NvDM_Log so the user can find it). The bound aPos
    // attribute is location 0 — we glBindAttribLocation before linking.
    GLuint CompileProgram(const char* fsSource)
    {
        if (!p_glCreateShader || !p_glShaderSource || !p_glCompileShader ||
            !p_glGetShaderiv || !p_glCreateProgram || !p_glAttachShader ||
            !p_glLinkProgram || !p_glGetProgramiv || !p_glBindAttribLocation)
            return 0;

        auto compile = [](GLenum stage, const char* src) -> GLuint {
            GLuint sh = p_glCreateShader(stage);
            if (!sh) return 0;
            p_glShaderSource(sh, 1, &src, nullptr);
            p_glCompileShader(sh);
            GLint ok = 0;
            p_glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
            if (!ok)
            {
                char log[512] = {};
                if (p_glGetShaderInfoLog) p_glGetShaderInfoLog(sh, sizeof(log) - 1, nullptr, log);
                NvDM_Log("  ogl CompileProgram: shader stage=0x%04X compile FAILED: %s\n",
                         (unsigned)stage, log);
                if (p_glDeleteShader) p_glDeleteShader(sh);
                return 0;
            }
            return sh;
        };

        GLuint vs = compile(GL_VERTEX_SHADER, kCompositeVS);
        if (!vs) return 0;
        GLuint fs = compile(GL_FRAGMENT_SHADER, fsSource);
        if (!fs) { if (p_glDeleteShader) p_glDeleteShader(vs); return 0; }

        GLuint prog = p_glCreateProgram();
        if (!prog) { p_glDeleteShader(vs); p_glDeleteShader(fs); return 0; }
        p_glAttachShader(prog, vs);
        p_glAttachShader(prog, fs);
        p_glBindAttribLocation(prog, 0, "aPos");
        p_glLinkProgram(prog);
        GLint linked = 0;
        p_glGetProgramiv(prog, GL_LINK_STATUS, &linked);
        // Shaders can be deleted after link — program retains its own refs.
        if (p_glDeleteShader) { p_glDeleteShader(vs); p_glDeleteShader(fs); }
        if (!linked)
        {
            NvDM_Log("  ogl CompileProgram: link FAILED\n");
            if (p_glDeleteProgram) p_glDeleteProgram(prog);
            return 0;
        }
        return prog;
    }

    // Lazy-build all four programs + the fullscreen-quad VBO. Returns true
    // when the requested mode's program is ready; false if compile failed
    // (caller falls back to SBS blit).
    bool EnsureCompositePrograms()
    {
        if (g_progs.compileFailed) return false;
        if (g_progs.prog[PROG_LINE]     && g_progs.prog[PROG_COL] &&
            g_progs.prog[PROG_CHECKER]  && g_progs.prog[PROG_ANAGLYPH] &&
            g_progs.quadVBO != 0)
            return true;
        if (!p_glCreateProgram || !p_glGenBuffers || !p_glBufferData)
            { g_progs.compileFailed = true; return false; }

        const char* sources[PROG_COUNT] = {
            kCompositeFS_Line,
            kCompositeFS_Col,
            kCompositeFS_Checker,
            kCompositeFS_Anaglyph,
        };
        const char* names[PROG_COUNT] = { "Line", "Col", "Checker", "Anaglyph" };

        for (int i = 0; i < PROG_COUNT; ++i)
        {
            if (g_progs.prog[i]) continue;
            g_progs.prog[i] = CompileProgram(sources[i]);
            if (!g_progs.prog[i])
            {
                NvDM_Log("  ogl EnsureCompositePrograms: %s shader unavailable — modes 4-7 disabled\n",
                         names[i]);
                g_progs.compileFailed = true;
                return false;
            }
            if (p_glGetUniformLocation)
            {
                g_progs.locLeftTex[i]  = p_glGetUniformLocation(g_progs.prog[i], "leftTex");
                g_progs.locRightTex[i] = p_glGetUniformLocation(g_progs.prog[i], "rightTex");
                if (i == PROG_ANAGLYPH)
                    g_progs.locAnaglyphRows = p_glGetUniformLocation(g_progs.prog[i], "gAnaglyphRows");
            }
        }

        // Fullscreen quad: two triangles, NDC positions (no separate UV — VS
        // derives uv from position).
        if (g_progs.quadVBO == 0)
        {
            const GLfloat verts[] = {
                -1.0f, -1.0f,
                 3.0f, -1.0f,
                -1.0f,  3.0f,
            };
            p_glGenBuffers(1, &g_progs.quadVBO);
            if (!g_progs.quadVBO) { g_progs.compileFailed = true; return false; }
            p_glBindBuffer(GL_ARRAY_BUFFER, g_progs.quadVBO);
            p_glBufferData(GL_ARRAY_BUFFER, (ptrdiff_t)sizeof(verts), verts, GL_STATIC_DRAW);
            p_glBindBuffer(GL_ARRAY_BUFFER, 0);
        }

        NvDM_Log("  ogl EnsureCompositePrograms: OK (Line/Col/Checker/Anaglyph + quadVBO=%u)\n",
                 (unsigned)g_progs.quadVBO);
        return true;
    }

    // Run shader-mode composite for OutputMode 4-7. The default framebuffer
    // (target=0) is bound by the caller. Returns true on success.
    bool RunShaderComposite(int mode)
    {
        if (!EnsureCompositePrograms()) return false;

        ShaderProgIdx idx = PROG_LINE;
        switch (mode)
        {
            case 4: idx = PROG_LINE;     break;
            case 5: idx = PROG_COL;      break;
            case 6: idx = PROG_CHECKER;  break;
            case 7: idx = PROG_ANAGLYPH; break;
            default: return false;
        }
        GLuint prog = g_progs.prog[idx];
        if (!prog || !p_glUseProgram || !p_glActiveTexture ||
            !p_glBindBuffer || !p_glEnableVertexAttribArray ||
            !p_glVertexAttribPointer || !p_glDrawArrays || !p_glViewport)
            return false;

        const bool swap = NvDM_SwapEyes() != 0;
        const GLuint leftTexId  = swap ? g_fbos.texRight : g_fbos.texLeft;
        const GLuint rightTexId = swap ? g_fbos.texLeft  : g_fbos.texRight;

        p_glViewport(0, 0, g_fbos.width, g_fbos.height);
        if (p_glDisable)
        {
            // GL_DEPTH_TEST = 0x0B71, GL_BLEND = 0x0BE2, GL_CULL_FACE = 0x0B44
            p_glDisable(0x0B71);
            p_glDisable(0x0BE2);
            p_glDisable(0x0B44);
        }
        p_glUseProgram(prog);
        p_glActiveTexture(GL_TEXTURE0);
        p_glBindTexture(GL_TEXTURE_2D, leftTexId);
        p_glActiveTexture(GL_TEXTURE1);
        p_glBindTexture(GL_TEXTURE_2D, rightTexId);
        if (p_glUniform1i)
        {
            if (g_progs.locLeftTex[idx]  >= 0) p_glUniform1i(g_progs.locLeftTex[idx],  0);
            if (g_progs.locRightTex[idx] >= 0) p_glUniform1i(g_progs.locRightTex[idx], 1);
        }

        if (idx == PROG_ANAGLYPH && g_progs.locAnaglyphRows >= 0 && p_glUniform4fv)
        {
            int colour = NvDM_AnaglyphColour();
            int method = NvDM_AnaglyphMethod();
            if (colour < 0 || colour > 2) colour = 0;
            if (method < 0 || method > 6) method = 0;
            const NvDirectMode::AnaglyphMatrix& m =
                NvDirectMode::kAnaglyphMatrices[colour][method];
            // 6 vec4 rows: lR, lG, lB, rR, rG, rB (xyz used; w = 0).
            const float* rows[6] = { m.lR, m.lG, m.lB, m.rR, m.rG, m.rB };
            float buf[6 * 4] = {};
            for (int i = 0; i < 6; ++i)
            {
                buf[i * 4 + 0] = rows[i][0];
                buf[i * 4 + 1] = rows[i][1];
                buf[i * 4 + 2] = rows[i][2];
                buf[i * 4 + 3] = 0.0f;
            }
            p_glUniform4fv(g_progs.locAnaglyphRows, 6, buf);
        }

        // Bind quad VBO and feed position to attribute 0.
        p_glBindBuffer(GL_ARRAY_BUFFER, g_progs.quadVBO);
        p_glEnableVertexAttribArray(0);
        p_glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(GLfloat), nullptr);
        p_glDrawArrays(GL_TRIANGLES, 0, 3);

        // Detach our state so the game's next frame doesn't pick up our
        // texture bindings on TEXTURE0/TEXTURE1. Game rebinds program +
        // attribs on its own draw setup so we leave those.
        p_glUseProgram(0);
        p_glActiveTexture(GL_TEXTURE1);
        p_glBindTexture(GL_TEXTURE_2D, 0);
        p_glActiveTexture(GL_TEXTURE0);
        p_glBindTexture(GL_TEXTURE_2D, 0);
        p_glBindBuffer(GL_ARRAY_BUFFER, 0);
        return true;
    }
} // anonymous namespace

void EyeFbosBlitToDefault()
{
    if (!g_fbos.ready || !p_glBindFramebuffer || !p_glBlitFramebuffer) return;

    p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);

    const int mode = NvDM_OutputMode();
    if (mode == 8 && RunSRWeave())
    {
        // Weaver wrote to the now-bound default framebuffer. Re-bind our
        // LEFT FBO so the next frame's game-side draws land in our eye
        // texture (game rebinds 0 first thing, which our hook redirects).
        p_glBindFramebuffer(GL_FRAMEBUFFER, g_fbos.fboLeft);
        return;
    }
    if (mode >= 4 && mode <= 7 && RunShaderComposite(mode))
    {
        // Re-bind our LEFT FBO for next frame (game rebinds 0 first thing
        // anyway, which our hook redirects to the active eye).
        p_glBindFramebuffer(GL_FRAMEBUFFER, g_fbos.fboLeft);
        return;
    }
    // Modes 0-3 (and any fallback from the shader path or SR weave)
    // use the existing glBlitFramebuffer route — cheaper and well-tested.

    const int W = g_fbos.width;
    const int H = g_fbos.height;
    const bool topBottom = NvDM_OutputIsTopBottom() != 0;
    const bool swap      = NvDM_SwapEyes() != 0;

    // SwapEyes flips which captured eye lands in which display half — game
    // still rendered LEFT-eye-content into fboLeft (live FBO redirect on
    // glBindFramebuffer(0)), but at composite time we put RIGHT content
    // in the LEFT slot and vice versa.
    GLuint leftSlotFbo  = swap ? g_fbos.fboRight : g_fbos.fboLeft;
    GLuint rightSlotFbo = swap ? g_fbos.fboLeft  : g_fbos.fboRight;

    if (topBottom)
    {
        // Half T-B: "left" eye (logical) -> top half, "right" -> bottom half
        p_glBindFramebuffer(GL_READ_FRAMEBUFFER, leftSlotFbo);
        p_glBlitFramebuffer(0, 0, W, H,
                            0, H / 2, W, H,
                            GL_COLOR_BUFFER_BIT, GL_NEAREST);
        p_glBindFramebuffer(GL_READ_FRAMEBUFFER, rightSlotFbo);
        p_glBlitFramebuffer(0, 0, W, H,
                            0, 0, W, H / 2,
                            GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }
    else
    {
        // Half SBS (default): "left" -> left half, "right" -> right half
        p_glBindFramebuffer(GL_READ_FRAMEBUFFER, leftSlotFbo);
        p_glBlitFramebuffer(0, 0, W, H,
                            0, 0, W / 2, H,
                            GL_COLOR_BUFFER_BIT, GL_NEAREST);
        p_glBindFramebuffer(GL_READ_FRAMEBUFFER, rightSlotFbo);
        p_glBlitFramebuffer(0, 0, W, H,
                            W / 2, 0, W, H,
                            GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }

    // Re-bind our LEFT FBO for the next frame (game rebinds 0 first thing
    // anyway, which our hook redirects to the active eye).
    p_glBindFramebuffer(GL_FRAMEBUFFER, g_fbos.fboLeft);
}

bool IsOurFboId(GLuint id)
{
    return g_fbos.ready && (id == g_fbos.fboLeft || id == g_fbos.fboRight);
}

int  GetFboWidth()  { return g_fbos.width; }
int  GetFboHeight() { return g_fbos.height; }
bool IsFboReady()   { return g_fbos.ready; }

} // namespace NvDirectMode
