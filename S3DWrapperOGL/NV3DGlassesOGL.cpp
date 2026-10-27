/*
 * Project : wiz3D
 * Copyright (C) wiz3D contributors 2026
 *
 * NV3D-Lib OpenGL shutter-glasses bridge. Builds a 2W x H plain GL SBS
 * texture each frame and hands it to NV3D::InterfaceOGL, which owns the
 * internal GL/DX interop + the D3D9Ex DIRECT-mode shutter presenter.
 */

#include "stdafx.h"
#include "NV3DGlassesOGL.h"
#include "NV3D.hpp"

#include <gl/GL.h>

#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER                 0x8D40
#endif
#ifndef GL_READ_FRAMEBUFFER
#define GL_READ_FRAMEBUFFER            0x8CA8
#endif
#ifndef GL_DRAW_FRAMEBUFFER
#define GL_DRAW_FRAMEBUFFER            0x8CA9
#endif
#ifndef GL_COLOR_ATTACHMENT0
#define GL_COLOR_ATTACHMENT0           0x8CE0
#endif
#ifndef GL_COLOR_BUFFER_BIT
#define GL_COLOR_BUFFER_BIT            0x00004000
#endif
#ifndef GL_RGBA8
#define GL_RGBA8                       0x8058
#endif

// GL FBO entry-point typedefs -- same pattern as SRWeaveOGL / KatangaOGL.
typedef void (APIENTRY *PFN_GenFramebuffers)(GLsizei, GLuint*);
typedef void (APIENTRY *PFN_DeleteFramebuffers)(GLsizei, const GLuint*);
typedef void (APIENTRY *PFN_BindFramebuffer)(GLenum, GLuint);
typedef void (APIENTRY *PFN_FramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
typedef void (APIENTRY *PFN_BlitFramebuffer)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);

struct NV3DGlassesOGLContext
{
    bool fallback = true;

    NV3D::InterfaceOGL* iface = nullptr;

    GLuint sbsTex = 0;        // 2W x H plain GL texture (RGBA8); NV3D-Lib imports it internally
    GLuint fbo    = 0;        // scratch FBO for the eye blits
    unsigned int sbsW = 0;    // == 2 * viewWidth
    unsigned int sbsH = 0;    // == viewHeight

    PFN_GenFramebuffers        glGenFramebuffers        = nullptr;
    PFN_DeleteFramebuffers     glDeleteFramebuffers     = nullptr;
    PFN_BindFramebuffer        glBindFramebuffer        = nullptr;
    PFN_FramebufferTexture2D   glFramebufferTexture2D   = nullptr;
    PFN_BlitFramebuffer        glBlitFramebuffer        = nullptr;
};

namespace {

bool LoadEntryPoints(NV3DGlassesOGLContext* c)
{
    c->glGenFramebuffers      = (PFN_GenFramebuffers)      wglGetProcAddress("glGenFramebuffers");
    c->glDeleteFramebuffers   = (PFN_DeleteFramebuffers)   wglGetProcAddress("glDeleteFramebuffers");
    c->glBindFramebuffer      = (PFN_BindFramebuffer)      wglGetProcAddress("glBindFramebuffer");
    c->glFramebufferTexture2D = (PFN_FramebufferTexture2D) wglGetProcAddress("glFramebufferTexture2D");
    c->glBlitFramebuffer      = (PFN_BlitFramebuffer)      wglGetProcAddress("glBlitFramebuffer");

    if (!c->glGenFramebuffers || !c->glDeleteFramebuffers || !c->glBindFramebuffer ||
        !c->glFramebufferTexture2D || !c->glBlitFramebuffer)
    {
        c->glGenFramebuffers      = (PFN_GenFramebuffers)      wglGetProcAddress("glGenFramebuffersEXT");
        c->glDeleteFramebuffers   = (PFN_DeleteFramebuffers)   wglGetProcAddress("glDeleteFramebuffersEXT");
        c->glBindFramebuffer      = (PFN_BindFramebuffer)      wglGetProcAddress("glBindFramebufferEXT");
        c->glFramebufferTexture2D = (PFN_FramebufferTexture2D) wglGetProcAddress("glFramebufferTexture2DEXT");
        c->glBlitFramebuffer      = (PFN_BlitFramebuffer)      wglGetProcAddress("glBlitFramebufferEXT");
    }
    return c->glGenFramebuffers && c->glDeleteFramebuffers && c->glBindFramebuffer &&
           c->glFramebufferTexture2D && c->glBlitFramebuffer;
}

} // namespace

bool NV3DGlassesOGL_Initialize(NV3DGlassesOGLContext** outCtx, HGLRC glContext, HDC glDC,
                                HWND hostHwnd, unsigned int viewWidth, unsigned int viewHeight,
                                bool eyeSwap)
{
    if (!outCtx) return false;
    NV3DGlassesOGLContext* c = new NV3DGlassesOGLContext;
    *outCtx = c;

    if (!LoadEntryPoints(c))               return false;
    if (viewWidth == 0 || viewHeight == 0) return false;
    if (!glContext || !glDC)               return false;

    NV3D::InitParams params = {};
    params.host_hwnd         = hostHwnd;
    params.eye_swap          = eyeSwap;
    params.enable_lightboost = true;
    params.enable_suppressor = true;
    params.on_top            = true;

    HRESULT hr = NV3D::CreateInterfaceOGL(glContext, glDC, &params, &c->iface);
    if (FAILED(hr) || !c->iface) return false;

    // Allocate the SBS GL texture. 2W x H RGBA8, no mipmaps.
    glGenTextures(1, &c->sbsTex);
    if (!c->sbsTex) return false;
    glBindTexture(GL_TEXTURE_2D, c->sbsTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)(viewWidth * 2), (GLsizei)viewHeight,
                 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindTexture(GL_TEXTURE_2D, 0);

    c->glGenFramebuffers(1, &c->fbo);
    if (!c->fbo) return false;

    // Register the SBS tex with NV3D-Lib. flip_y=true because OpenGL's origin
    // is bottom-left; the library flips during its own GL/DX interop blit.
    if (FAILED(c->iface->SetInputTexture(c->sbsTex, (int)(viewWidth * 2), (int)viewHeight, /*flip_y*/ true)))
        return false;

    c->sbsW = viewWidth * 2u;
    c->sbsH = viewHeight;
    c->fallback = false;
    return true;
}

void NV3DGlassesOGL_Cleanup(NV3DGlassesOGLContext** inoutCtx)
{
    if (!inoutCtx || !*inoutCtx) return;
    NV3DGlassesOGLContext* c = *inoutCtx;

    if (c->iface)
    {
        c->iface->Delete();
        c->iface = nullptr;
    }
    if (c->fbo && c->glDeleteFramebuffers) c->glDeleteFramebuffers(1, &c->fbo);
    if (c->sbsTex) glDeleteTextures(1, &c->sbsTex);
    c->fbo = 0;
    c->sbsTex = 0;

    delete c;
    *inoutCtx = nullptr;
}

bool NV3DGlassesOGL_IsFallback(NV3DGlassesOGLContext* ctx)
{
    return !ctx || ctx->fallback;
}

bool NV3DGlassesOGL_Render(NV3DGlassesOGLContext* c,
                            unsigned int leftTexId, unsigned int rightTexId,
                            unsigned int windowWidth, unsigned int windowHeight,
                            bool swapEyes)
{
    if (!c || c->fallback || !c->iface)     return false;
    if (!leftTexId || !rightTexId)          return false;

    const GLint srcW  = (GLint)windowWidth;
    const GLint srcH  = (GLint)windowHeight;
    const GLint halfW = (GLint)(c->sbsW / 2u);
    const GLint fullH = (GLint)(c->sbsH);

    const unsigned int srcLeft  = swapEyes ? rightTexId : leftTexId;
    const unsigned int srcRight = swapEyes ? leftTexId  : rightTexId;

    GLint prevReadFBO = 0, prevDrawFBO = 0;
    glGetIntegerv(0x8CAA /*GL_READ_FRAMEBUFFER_BINDING*/, &prevReadFBO);
    glGetIntegerv(0x8CA6 /*GL_DRAW_FRAMEBUFFER_BINDING*/, &prevDrawFBO);

    c->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, c->fbo);
    c->glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, c->sbsTex, 0);
    c->glBindFramebuffer(GL_READ_FRAMEBUFFER, c->fbo);

    c->glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, (GLuint)srcLeft, 0);
    c->glBlitFramebuffer(0, 0, srcW, srcH, 0, 0, halfW, fullH, GL_COLOR_BUFFER_BIT, GL_NEAREST);

    c->glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, (GLuint)srcRight, 0);
    c->glBlitFramebuffer(0, 0, srcW, srcH, halfW, 0, (GLint)c->sbsW, fullH, GL_COLOR_BUFFER_BIT, GL_NEAREST);

    c->glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    c->glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    c->glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)prevReadFBO);
    c->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)prevDrawFBO);

    return SUCCEEDED(c->iface->Present());
}
