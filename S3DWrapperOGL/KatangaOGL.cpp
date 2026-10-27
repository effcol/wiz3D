/*
 * Project : wiz3D
 * Copyright (C) wiz3D contributors 2026
 *
 * Katanga OpenGL producer bridge. Lives inside S3DWrapperOGL.dll; stands
 * up a hidden D3D11 device + the Shared/KatangaProducer helper, and uses
 * WGL_NV_DX_interop2 to let GL blit the per-eye textures directly into
 * a DX11-side wide-SBS intermediate that the producer then publishes.
 *
 * See KatangaOGL.h for the API shape and the SRWeaveOGL module for the
 * sibling GL-side FBO entry-point pattern.
 */

#include "stdafx.h"
#include "KatangaOGL.h"
#include "../Shared/KatangaProducer.h"

#include <gl/GL.h>
#include <d3d11.h>

// GL/WGL enums we don't want to pull a glew header in for.
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

#define WGL_ACCESS_READ_ONLY_NV        0x0000
#define WGL_ACCESS_READ_WRITE_NV       0x0001
#define WGL_ACCESS_WRITE_DISCARD_NV    0x0002

// ---------------------------------------------------------------------------
// Entry-point typedefs. GL FBO entry points are mirror of SRWeaveOGL's
// pattern; WGL_NV_DX_interop2 entry points are DX11-specific.
// ---------------------------------------------------------------------------
typedef void   (APIENTRY *PFN_GenFramebuffers)(GLsizei n, GLuint* framebuffers);
typedef void   (APIENTRY *PFN_DeleteFramebuffers)(GLsizei n, const GLuint* framebuffers);
typedef void   (APIENTRY *PFN_BindFramebuffer)(GLenum target, GLuint framebuffer);
typedef void   (APIENTRY *PFN_FramebufferTexture2D)(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level);
typedef void   (APIENTRY *PFN_BlitFramebuffer)(GLint sx0, GLint sy0, GLint sx1, GLint sy1,
                                                GLint dx0, GLint dy0, GLint dx1, GLint dy1,
                                                GLbitfield mask, GLenum filter);

typedef HANDLE (WINAPI *PFN_wglDXOpenDeviceNV)(void* dxDevice);
typedef BOOL   (WINAPI *PFN_wglDXCloseDeviceNV)(HANDLE hDevice);
typedef HANDLE (WINAPI *PFN_wglDXRegisterObjectNV)(HANDLE hDevice, void* dxObject, GLuint name,
                                                   GLenum type, GLenum access);
typedef BOOL   (WINAPI *PFN_wglDXUnregisterObjectNV)(HANDLE hDevice, HANDLE hObject);
typedef BOOL   (WINAPI *PFN_wglDXLockObjectsNV)(HANDLE hDevice, GLint count, HANDLE* hObjects);
typedef BOOL   (WINAPI *PFN_wglDXUnlockObjectsNV)(HANDLE hDevice, GLint count, HANDLE* hObjects);

#define WGL_DX_TEXTURE_2D 0x8064   // == GL_TEXTURE_2D; interop spec reuses it

struct KatangaOGLContext
{
    bool fallback = true;

    // DX11 side.
    ID3D11Device*         d3dDevice  = nullptr;
    ID3D11DeviceContext*  d3dContext = nullptr;
    ID3D11Texture2D*      sbsTex     = nullptr;    // 2W x H interop intermediate
    wiz3d::KatangaProducer producer;               // owns the Katanga shared tex

    // GL side.
    GLuint glSbsTex = 0;        // GL name that aliases sbsTex via wglDXRegisterObjectNV
    GLuint glFBO    = 0;        // scratch FBO for blits into glSbsTex
    HANDLE hGLDX    = nullptr;  // wglDXOpenDeviceNV handle
    HANDLE hGLObj   = nullptr;  // wglDXRegisterObjectNV handle

    unsigned int sbsW = 0;      // 2 * viewWidth
    unsigned int sbsH = 0;      // viewHeight

    // Entry points.
    PFN_GenFramebuffers        glGenFramebuffers        = nullptr;
    PFN_DeleteFramebuffers     glDeleteFramebuffers     = nullptr;
    PFN_BindFramebuffer        glBindFramebuffer        = nullptr;
    PFN_FramebufferTexture2D   glFramebufferTexture2D   = nullptr;
    PFN_BlitFramebuffer        glBlitFramebuffer        = nullptr;

    PFN_wglDXOpenDeviceNV      wglDXOpenDeviceNV        = nullptr;
    PFN_wglDXCloseDeviceNV     wglDXCloseDeviceNV       = nullptr;
    PFN_wglDXRegisterObjectNV  wglDXRegisterObjectNV    = nullptr;
    PFN_wglDXUnregisterObjectNV wglDXUnregisterObjectNV = nullptr;
    PFN_wglDXLockObjectsNV     wglDXLockObjectsNV       = nullptr;
    PFN_wglDXUnlockObjectsNV   wglDXUnlockObjectsNV     = nullptr;
};

namespace {

bool LoadEntryPoints(KatangaOGLContext* c)
{
    // Core GL FBO (3.0+). Try ARB names first, then EXT fallback.
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

    // WGL_NV_DX_interop2.
    c->wglDXOpenDeviceNV        = (PFN_wglDXOpenDeviceNV)        wglGetProcAddress("wglDXOpenDeviceNV");
    c->wglDXCloseDeviceNV       = (PFN_wglDXCloseDeviceNV)       wglGetProcAddress("wglDXCloseDeviceNV");
    c->wglDXRegisterObjectNV    = (PFN_wglDXRegisterObjectNV)    wglGetProcAddress("wglDXRegisterObjectNV");
    c->wglDXUnregisterObjectNV  = (PFN_wglDXUnregisterObjectNV)  wglGetProcAddress("wglDXUnregisterObjectNV");
    c->wglDXLockObjectsNV       = (PFN_wglDXLockObjectsNV)       wglGetProcAddress("wglDXLockObjectsNV");
    c->wglDXUnlockObjectsNV     = (PFN_wglDXUnlockObjectsNV)     wglGetProcAddress("wglDXUnlockObjectsNV");

    return c->glGenFramebuffers && c->glBindFramebuffer && c->glFramebufferTexture2D &&
           c->glBlitFramebuffer && c->glDeleteFramebuffers &&
           c->wglDXOpenDeviceNV && c->wglDXRegisterObjectNV &&
           c->wglDXLockObjectsNV && c->wglDXUnlockObjectsNV &&
           c->wglDXUnregisterObjectNV && c->wglDXCloseDeviceNV;
}

} // namespace

bool KatangaOGL_Initialize(KatangaOGLContext** outCtx, HWND /*hWnd*/,
                           unsigned int viewWidth, unsigned int viewHeight)
{
    if (!outCtx) return false;
    KatangaOGLContext* c = new KatangaOGLContext;
    *outCtx = c;

    if (!LoadEntryPoints(c))             return false;
    if (viewWidth == 0 || viewHeight == 0) return false;

    // Need Katanga running up front — the DX11 producer's TryInit returns
    // false if the setup mutex isn't present. No point creating a D3D11
    // device otherwise.
    if (!c->producer.TryInit())          return false;

    // Create a hidden D3D11 device. No swap chain, no debug — minimum
    // viable. BGRA for Katanga compat.
    D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        nullptr, 0, D3D11_SDK_VERSION,
        &c->d3dDevice, &fl, &c->d3dContext);
    if (FAILED(hr) || !c->d3dDevice)     return false;

    // Hand the producer a backbuffer-shaped desc so it can create the
    // KMT-shared texture at 2W x H. Format matches Katanga's expectation
    // (R8G8B8A8_UNORM is fine; the producer strips _SRGB).
    D3D11_TEXTURE2D_DESC bbDesc = {};
    bbDesc.Width            = viewWidth;
    bbDesc.Height           = viewHeight;
    bbDesc.MipLevels        = 1;
    bbDesc.ArraySize        = 1;
    bbDesc.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
    bbDesc.SampleDesc.Count = 1;
    bbDesc.Usage            = D3D11_USAGE_DEFAULT;
    bbDesc.BindFlags        = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    if (!c->producer.OnBackbufferReady(c->d3dDevice, bbDesc)) return false;

    // Intermediate SBS texture. GL writes here via WGL_NV_DX_interop2,
    // producer copies this into the KMT-shared tex under setup mutex.
    // D3D11_RESOURCE_MISC_SHARED is required for the GL interop registration.
    const unsigned int wideW = viewWidth * 2u;
    D3D11_TEXTURE2D_DESC sbsDesc = {};
    sbsDesc.Width            = wideW;
    sbsDesc.Height           = viewHeight;
    sbsDesc.MipLevels        = 1;
    sbsDesc.ArraySize        = 1;
    sbsDesc.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
    sbsDesc.SampleDesc.Count = 1;
    sbsDesc.Usage            = D3D11_USAGE_DEFAULT;
    sbsDesc.BindFlags        = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    sbsDesc.MiscFlags        = D3D11_RESOURCE_MISC_SHARED;
    hr = c->d3dDevice->CreateTexture2D(&sbsDesc, nullptr, &c->sbsTex);
    if (FAILED(hr) || !c->sbsTex)        return false;

    // Open the GL/DX interop device.
    c->hGLDX = c->wglDXOpenDeviceNV(c->d3dDevice);
    if (!c->hGLDX)                        return false;

    // Allocate the GL name that will alias sbsTex, then register.
    glGenTextures(1, &c->glSbsTex);
    c->hGLObj = c->wglDXRegisterObjectNV(c->hGLDX, c->sbsTex, c->glSbsTex,
                                         WGL_DX_TEXTURE_2D, WGL_ACCESS_WRITE_DISCARD_NV);
    if (!c->hGLObj)                       return false;

    // Scratch FBO for FramebufferTexture2D + BlitFramebuffer.
    c->glGenFramebuffers(1, &c->glFBO);
    if (!c->glFBO)                        return false;

    c->sbsW = wideW;
    c->sbsH = viewHeight;
    c->fallback = false;
    return true;
}

void KatangaOGL_Cleanup(KatangaOGLContext** inoutCtx)
{
    if (!inoutCtx || !*inoutCtx) return;
    KatangaOGLContext* c = *inoutCtx;

    if (c->hGLObj && c->hGLDX && c->wglDXUnregisterObjectNV)
        c->wglDXUnregisterObjectNV(c->hGLDX, c->hGLObj);
    if (c->hGLDX && c->wglDXCloseDeviceNV)
        c->wglDXCloseDeviceNV(c->hGLDX);
    c->hGLObj = nullptr;
    c->hGLDX  = nullptr;

    if (c->glFBO && c->glDeleteFramebuffers)
        c->glDeleteFramebuffers(1, &c->glFBO);
    if (c->glSbsTex)
        glDeleteTextures(1, &c->glSbsTex);
    c->glFBO = 0;
    c->glSbsTex = 0;

    c->producer.Shutdown();

    if (c->sbsTex)     { c->sbsTex->Release();     c->sbsTex = nullptr; }
    if (c->d3dContext) { c->d3dContext->Release(); c->d3dContext = nullptr; }
    if (c->d3dDevice)  { c->d3dDevice->Release();  c->d3dDevice = nullptr; }

    delete c;
    *inoutCtx = nullptr;
}

bool KatangaOGL_IsFallback(KatangaOGLContext* ctx)
{
    return !ctx || ctx->fallback;
}

bool KatangaOGL_Render(KatangaOGLContext* c,
                       unsigned int leftTexId, unsigned int rightTexId,
                       float texCoordX, float texCoordY,
                       unsigned int windowWidth, unsigned int windowHeight,
                       bool swapEyes)
{
    if (!c || c->fallback)             return false;
    if (!leftTexId || !rightTexId)     return false;

    // Each per-eye texture is pow2-padded; the live image occupies the
    // top-left (windowWidth x windowHeight) pixel block. BlitFramebuffer
    // works in texel space so texCoord{X,Y} aren't needed here — those
    // normalise to texture coords when the OGL fallback path draws
    // textured quads.
    (void)texCoordX; (void)texCoordY;
    const GLint srcW  = (GLint)windowWidth;
    const GLint srcH  = (GLint)windowHeight;
    const GLint halfW = (GLint)(c->sbsW / 2u);
    const GLint fullH = (GLint)(c->sbsH);

    const unsigned int srcLeft  = swapEyes ? rightTexId : leftTexId;
    const unsigned int srcRight = swapEyes ? leftTexId  : rightTexId;

    HANDLE objs[1] = { c->hGLObj };
    if (!c->wglDXLockObjectsNV(c->hGLDX, 1, objs)) return false;

    // Save FBO binding so we don't disturb the game. Core profile has no
    // glPushAttrib so we read back explicitly.
    GLint prevReadFBO = 0, prevDrawFBO = 0;
    glGetIntegerv(0x8CAA /*GL_READ_FRAMEBUFFER_BINDING*/, &prevReadFBO);
    glGetIntegerv(0x8CA6 /*GL_DRAW_FRAMEBUFFER_BINDING*/, &prevDrawFBO);

    // Dest FBO wraps glSbsTex at COLOR_ATTACHMENT0.
    c->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, c->glFBO);
    c->glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, c->glSbsTex, 0);

    // We need a *read* FBO too, swapping its color attachment per-eye.
    // Reuse the same FBO name via GL_READ_FRAMEBUFFER target.
    c->glBindFramebuffer(GL_READ_FRAMEBUFFER, c->glFBO);

    // Left eye -> left half of glSbsTex.
    c->glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, (GLuint)srcLeft, 0);
    c->glBlitFramebuffer(0, 0, srcW, srcH,
                          0, 0, halfW, fullH,
                          GL_COLOR_BUFFER_BIT, GL_NEAREST);

    // Right eye -> right half.
    c->glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, (GLuint)srcRight, 0);
    c->glBlitFramebuffer(0, 0, srcW, srcH,
                          halfW, 0, (GLint)c->sbsW, fullH,
                          GL_COLOR_BUFFER_BIT, GL_NEAREST);

    // Detach + unbind our FBOs so we return the pipeline clean.
    c->glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    c->glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    c->glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)prevReadFBO);
    c->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)prevDrawFBO);

    if (!c->wglDXUnlockObjectsNV(c->hGLDX, 1, objs)) return false;

    // Publish on DX11 side: producer copies sbsTex -> KMT-shared tex under
    // the setup mutex.
    return c->producer.PublishFrame(c->d3dContext, c->sbsTex);
}
