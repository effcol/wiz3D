/*
 * Katanga OpenGL producer bridge for S3DWrapperOGL (OutputMode 12).
 *
 * Katanga is a DX11-shared-texture consumer, so this module stands up a
 * hidden D3D11 device inside the OGL wrapper and uses WGL_NV_DX_interop2
 * to let GL write into a DX11 texture directly. Each frame:
 *
 *   wglDXLockObjectsNV   -> GL FBO-blit left eye tex into [0..W) half
 *                        -> GL FBO-blit right eye tex into [W..2W) half
 *   wglDXUnlockObjectsNV -> KatangaProducer::PublishFrame
 *
 * The producer helper (Shared/KatangaProducer.{h,cpp}) owns the Katanga
 * named-mutex / mapped-file / shared-texture lifecycle; we just feed it
 * a 2W x H DX11 intermediate texture that the GL side wrote.
 *
 * Runtime fallback: if d3d11.dll fails to load, WGL_NV_DX_interop2 is
 * unavailable, or Katanga isn't running, Initialize sticky-flags fallback
 * mode and the caller renders plain SBS instead (same shape as the SR
 * weave bridge).
 */

#pragma once

#include <windows.h>

struct KatangaOGLContext; // opaque — full definition in KatangaOGL.cpp

// Lifecycle. Call once after GL context is current + m_TextureID[0..1]
// are valid. Cleanup is idempotent.
bool KatangaOGL_Initialize(KatangaOGLContext** outCtx, HWND hWnd,
                           unsigned int viewWidth, unsigned int viewHeight);
void KatangaOGL_Cleanup(KatangaOGLContext** ctx);

// Per-frame compose. Blit left + right per-eye GL textures into the two
// halves of the DX11-interop intermediate texture under the WGL interop
// lock, then publish to Katanga. Returns false on any error — caller
// should render fallback SBS afterwards. texCoord{X,Y} are the fraction
// of each per-eye texture that holds the live image (same semantics as
// SRWeaveOGL_Render).
bool KatangaOGL_Render(KatangaOGLContext* ctx,
                       unsigned int leftTexId, unsigned int rightTexId,
                       float texCoordX, float texCoordY,
                       unsigned int windowWidth, unsigned int windowHeight,
                       bool swapEyes);

// True if any Initialize step failed (d3d11 missing, WGL_NV_DX_interop2
// unsupported, Katanga not running, shared texture create failed, …).
// Caller sticky-flags fallback and doesn't retry for the session.
bool KatangaOGL_IsFallback(KatangaOGLContext* ctx);
