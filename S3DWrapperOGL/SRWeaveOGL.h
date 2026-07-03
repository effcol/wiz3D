/*
 * Simulated Reality OpenGL weaver integration for S3DWrapperOGL (mode 9).
 *
 * Glue layer that wraps SR-Lib's SRInterfaceOGL behind a small C-style API.
 * Renderer.cpp only sees opaque-pointer get/set functions plus the
 * once-per-frame Render call — it doesn't include SR.hpp.
 *
 * Mirrors how the DX9 SR weave is structured (OutputMethods/Simulated
 * RealityWeaveOutput/) except this isn't a separate plugin DLL — OGL has
 * no OutputMethod plugin loader, so SR lives inside S3DWrapperOGL.dll.
 *
 * Runtime fallback: SR runtime DLLs are linked via /DELAYLOAD; SR-Lib's
 * CreateSRInterfaceOGL probes via LoadLibrary and returns E_NOINTERFACE
 * when the runtime is absent. We sticky-flag fallback mode and the caller
 * renders plain SBS instead (same Half-SBS fallback as DX9).
 */

#pragma once

#include <windows.h>

struct SRWeaveOGLContext; // opaque — full definition in SRWeaveOGL.cpp

// Lifecycle. All return true on success.
//   Initialize:  call once after the wrapper's GL context is current and
//                m_TextureID[0..1] are valid (per-eye textures).
//   Cleanup:     idempotent; safe to call multiple times. Always call
//                before destroying the GL context.
bool SRWeaveOGL_Initialize(SRWeaveOGLContext** outCtx, HWND hWnd,
                           unsigned int viewWidth, unsigned int viewHeight);
void SRWeaveOGL_Cleanup(SRWeaveOGLContext** ctx);

// Per-frame compose. Copies the two per-eye textures into the internal
// SBS framebuffer, then asks the SR runtime to weave them onto the default
// framebuffer (bound by the caller). texCoordX / texCoordY are the
// fraction of each per-eye texture that holds the live image — for the
// wrapper's power-of-two padded textures this is windowSize / pbufferSize,
// for non-pow2 contexts it's 1.0. Returns false on weave error — caller
// should sticky-flag fallback mode and not retry this session.
bool SRWeaveOGL_Render(SRWeaveOGLContext* ctx,
                       unsigned int leftTexId, unsigned int rightTexId,
                       float texCoordX, float texCoordY,
                       unsigned int windowWidth, unsigned int windowHeight);

// True if SR runtime detection failed at Initialize-time (DLL missing,
// SR Service not running, no SR display, etc.). Caller uses this to fall
// back to plain SBS rendering for the rest of the session.
bool SRWeaveOGL_IsFallback(SRWeaveOGLContext* ctx);
