/*
 * NV3D-Lib OpenGL shutter-glasses bridge for S3DWrapperOGL
 * (OutputMode 11 = 3DVisionGlasses).
 *
 * NV3D::InterfaceOGL owns its own hidden D3D9Ex device + the GL/DX
 * interop plumbing internally; we just build a plain 2W x H GL texture
 * each frame (FBO-blit left + right eye textures into the two halves)
 * and hand it to SetInputTexture + Present. See
 * ThirdPartyLibs/NV3D-Lib/include/NV3D.hpp (upstream oneup03/NV3D-Lib).
 *
 * Runtime fallback: on any failure (no NVIDIA GPU / nvapi / no 3D
 * Vision hardware / WGL_NV_DX_interop2 unsupported inside NV3D-Lib),
 * Initialize sticky-flags fallback and the caller renders plain SBS
 * instead.
 */

#pragma once

#include <windows.h>

struct NV3DGlassesOGLContext; // opaque -- full definition in NV3DGlassesOGL.cpp

// Lifecycle. Call once after the GL context is current + m_TextureID[0..1]
// are valid. Cleanup is idempotent.
bool NV3DGlassesOGL_Initialize(NV3DGlassesOGLContext** outCtx, HGLRC glContext, HDC glDC,
                                HWND hostHwnd, unsigned int viewWidth, unsigned int viewHeight,
                                bool eyeSwap);
void NV3DGlassesOGL_Cleanup(NV3DGlassesOGLContext** ctx);

// Per-frame compose. FBO-blit left + right per-eye GL textures into
// the two halves of the plain GL SBS texture, then NV3D-Lib's
// SetInputTexture + Present drive the shutter-glass output. Returns
// false on error.
bool NV3DGlassesOGL_Render(NV3DGlassesOGLContext* ctx,
                            unsigned int leftTexId, unsigned int rightTexId,
                            unsigned int windowWidth, unsigned int windowHeight,
                            bool swapEyes);

// Sticky fallback flag set by any failed init step.
bool NV3DGlassesOGL_IsFallback(NV3DGlassesOGLContext* ctx);
