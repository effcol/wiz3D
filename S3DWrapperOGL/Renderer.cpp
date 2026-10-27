/* IZ3D_FILE: $Id$ 
*
* Project : iZ3D Stereo Driver
* Copyright (C) iZ3D Inc. 2002 - 2010
*
* $Author$
* $Revision$
* $Date$
* $LastChangedBy$
* $URL$
*/
#include "StdAfx.h"
#define GLH_EXT_SINGLE_FILE
#include <glh/glh_extensions.h>
#include "Renderer.h"
#include "S3DWrapperOGL.h"
#include "Trace.h"
#include "Shaders.h"
#include "tchar.h"
#include "ProductNames.h"
#include "SRWeaveOGL.h"
#include "KatangaOGL.h"
#include "NV3DGlassesOGL.h"

std::vector<Renderer>	g_RendererList;
static HWND	m_hFrontWnd;

// Real glColorMask pointer — owned by Modified.cpp's hook setup, exposed for
// Renderer::ColorMask which needs to push the forced all-true mask to the
// driver after capturing the anaglyph-shaped mask the game requested.
typedef void (APIENTRY *PFN_glColorMask)(GLboolean r, GLboolean g, GLboolean b, GLboolean a);
extern PFN_glColorMask pfnOrig_glColorMask;

// First-frames trace state. Bumped in SwapBuffers (post-present). When
// g_traceFrameIdx < kTraceFrames, every DrawBuffer / glOrtho /
// glMatrixMode(PROJECTION) / glDisable(GL_DEPTH_TEST) call logs a line, plus
// an OVERLAY-ACTIVATED line on first trigger. After the window we go quiet
// so the log doesn't grow unbounded. Declared at file scope (not inside
// any function) so the overlay-helper definitions further down — which sit
// physically before Renderer::DrawBuffer in this TU — can reference them.
static int g_traceFrameIdx = 0;
static int g_traceCallIdx  = 0;
static const int kTraceFrames = 3;

// First-frames DrawBuffer trace. We don't know which GL call RtCW (and other
// idTech 3 family games) emit between the right-eye 3D pass and the 2D HUD
// pass, so capture the full sequence for the first few frames. After the
// trace window we go quiet so the file stays small.
static void DiagLogDrawBuffer(int frameIdx, int callIdx, GLenum mode,
                              BOOL seenLeft, BOOL seenRight,
                              BOOL overlayActive, const char* tag)
{
	TCHAR path[MAX_PATH];
	_tcscpy_s(path, MAX_PATH, gInfo.DriverDirectory);
	_tcscat_s(path, MAX_PATH, _T("\\OpenGLQuadBufferStereo.log"));
	FILE* f = NULL;
	_tfopen_s(&f, path, _T("a"));
	if (!f) return;
	const char* modeName = "?";
	switch (mode)
	{
	case GL_NONE:           modeName = "NONE";           break;
	case GL_FRONT_LEFT:     modeName = "FRONT_LEFT";     break;
	case GL_FRONT_RIGHT:    modeName = "FRONT_RIGHT";    break;
	case GL_BACK_LEFT:      modeName = "BACK_LEFT";      break;
	case GL_BACK_RIGHT:     modeName = "BACK_RIGHT";     break;
	case GL_FRONT:          modeName = "FRONT";          break;
	case GL_BACK:           modeName = "BACK";           break;
	case GL_LEFT:           modeName = "LEFT";           break;
	case GL_RIGHT:          modeName = "RIGHT";          break;
	case GL_FRONT_AND_BACK: modeName = "FRONT_AND_BACK"; break;
	}
	fprintf(f, "[wiz3D-OGL trace] frame=%d call=%d %s mode=0x%04X (%s) seenL=%d seenR=%d overlay=%d\n",
		frameIdx, callIdx, tag, (unsigned)mode, modeName,
		seenLeft ? 1 : 0, seenRight ? 1 : 0, overlayActive ? 1 : 0);
	fflush(f);
	fclose(f);
}

// One-shot always-on diagnostic logger. Survives FINAL_RELEASE (ZLOG is
// compiled out there). Writes a single line to OpenGLQuadBufferStereo.log
// in the game folder — separate from OpenGL32Proxy.log because the proxy
// keeps that file open exclusively for the whole game session. Only fires
// when window/PBuffer dims change, so it doesn't spam.
static void DiagLogOnce(const char* tag,
                        UINT winW, UINT winH,
                        UINT pbW, UINT pbH,
                        const RECT& clientRect,
                        const GLint viewport[4],
                        UINT outputMode,
                        BOOL emulateQB)
{
	TCHAR path[MAX_PATH];
	_tcscpy_s(path, MAX_PATH, gInfo.DriverDirectory);
	_tcscat_s(path, MAX_PATH, _T("\\OpenGLQuadBufferStereo.log"));
	FILE* f = NULL;
	_tfopen_s(&f, path, _T("a"));
	if (!f) return;
	fprintf(f, "[wiz3D-OGL diag %s] win=%ux%u pb=%ux%u clientRect=%ldx%ld viewport=(%d,%d,%dx%d) outMode=%u emulateQB=%d\n",
		tag, winW, winH, pbW, pbH,
		clientRect.right - clientRect.left, clientRect.bottom - clientRect.top,
		viewport[0], viewport[1], viewport[2], viewport[3],
		outputMode, emulateQB ? 1 : 0);
	fflush(f);
	fclose(f);
}

// One-shot context-capability logger. GL_STEREO is the spec-defined positive
// test for a quad-buffer stereo context (the game requested PFD_STEREO /
// WGL_STEREO_ARB) — it is FALSE for anaglyph / SBS-in-shader games, which
// render into a mono context, so GL_STEREO=0 plus no AnaglyphSteal trace line
// means the title isn't capturable by either of our OGL paths. GL_DOUBLEBUFFER
// is logged alongside for completeness. Queried from the game's own current
// context at the first SwapBuffers (see Renderer::SwapBuffers), before the
// no-PBuffer early-return, so even mono games that never trigger capture still
// record this once.
static void DiagLogContextCaps(BOOL ctxStereo, BOOL ctxDoubleBuffer)
{
	TCHAR path[MAX_PATH];
	_tcscpy_s(path, MAX_PATH, gInfo.DriverDirectory);
	_tcscat_s(path, MAX_PATH, _T("\\OpenGLQuadBufferStereo.log"));
	FILE* f = NULL;
	_tfopen_s(&f, path, _T("a"));
	if (!f) return;
	fprintf(f, "[wiz3D-OGL diag context-caps] GL_STEREO=%d GL_DOUBLEBUFFER=%d "
		"(STEREO=1 => quad-buffer context; =0 => mono: anaglyph / SBS-in-shader / no stereo)\n",
		ctxStereo ? 1 : 0, ctxDoubleBuffer ? 1 : 0);
	fflush(f);
	fclose(f);
}

// GL FBO entry points — same pattern SRWeaveOGL uses. We resolve them once
// via wglGetProcAddress when we first need them in the back-buffer context,
// then reuse for the overlay FBO management. Core names tried first, EXT
// names as fallback.
typedef void   (APIENTRY *PFN_GenFramebuffers)(GLsizei n, GLuint* framebuffers);
typedef void   (APIENTRY *PFN_BindFramebuffer)(GLenum target, GLuint framebuffer);
typedef void   (APIENTRY *PFN_DeleteFramebuffers)(GLsizei n, const GLuint* framebuffers);
typedef void   (APIENTRY *PFN_FramebufferTexture2D)(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level);
typedef GLenum (APIENTRY *PFN_CheckFramebufferStatus)(GLenum target);
#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER          0x8D40
#define GL_COLOR_ATTACHMENT0    0x8CE0
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#endif
static PFN_GenFramebuffers        pfnGenFramebuffers        = NULL;
static PFN_BindFramebuffer        pfnBindFramebuffer        = NULL;
static PFN_DeleteFramebuffers     pfnDeleteFramebuffers     = NULL;
static PFN_FramebufferTexture2D   pfnFramebufferTexture2D   = NULL;
static PFN_CheckFramebufferStatus pfnCheckFramebufferStatus = NULL;
static bool LoadFBOEntryPointsForRenderer()
{
	if (pfnGenFramebuffers) return true;
	pfnGenFramebuffers        = (PFN_GenFramebuffers)       wglGetProcAddress("glGenFramebuffers");
	pfnBindFramebuffer        = (PFN_BindFramebuffer)       wglGetProcAddress("glBindFramebuffer");
	pfnDeleteFramebuffers     = (PFN_DeleteFramebuffers)    wglGetProcAddress("glDeleteFramebuffers");
	pfnFramebufferTexture2D   = (PFN_FramebufferTexture2D)  wglGetProcAddress("glFramebufferTexture2D");
	pfnCheckFramebufferStatus = (PFN_CheckFramebufferStatus)wglGetProcAddress("glCheckFramebufferStatus");
	if (pfnGenFramebuffers && pfnBindFramebuffer && pfnDeleteFramebuffers &&
	    pfnFramebufferTexture2D && pfnCheckFramebufferStatus) return true;
	pfnGenFramebuffers        = (PFN_GenFramebuffers)       wglGetProcAddress("glGenFramebuffersEXT");
	pfnBindFramebuffer        = (PFN_BindFramebuffer)       wglGetProcAddress("glBindFramebufferEXT");
	pfnDeleteFramebuffers     = (PFN_DeleteFramebuffers)    wglGetProcAddress("glDeleteFramebuffersEXT");
	pfnFramebufferTexture2D   = (PFN_FramebufferTexture2D)  wglGetProcAddress("glFramebufferTexture2DEXT");
	pfnCheckFramebufferStatus = (PFN_CheckFramebufferStatus)wglGetProcAddress("glCheckFramebufferStatusEXT");
	return pfnGenFramebuffers && pfnBindFramebuffer && pfnDeleteFramebuffers &&
	       pfnFramebufferTexture2D && pfnCheckFramebufferStatus;
}

Renderer::Renderer(void)
{
	ZeroMemory((void *)this, sizeof(*this));
	m_nDrawBufferMode[0] = GL_BACK_LEFT;
	m_nDrawBufferMode[1] = GL_BACK_RIGHT;
	m_TextureID[0] = 0;
	m_TextureID[1] = 0;
	m_hBackBufferContext = 0;
	m_hVS = NULL;
	m_hFSBackScreen = NULL;
	m_hPOFrontScreen = NULL;
	m_hFSFrontScreen = NULL;
	m_hPOBackScreen = NULL;
	m_hFrontDC = NULL;
	m_hFrontWnd = NULL;
	m_SwapBuffersCount = 0;
}

void Renderer::EnsureOverlay()
{
	if (m_OverlayFBO && m_OverlayWidth == m_nWindowWidth && m_OverlayHeight == m_nWindowHeight)
		return;
	if (!LoadFBOEntryPointsForRenderer()) return;
	if (m_OverlayFBO)        { pfnDeleteFramebuffers(1, &m_OverlayFBO); m_OverlayFBO = 0; }
	if (m_OverlayTextureID)  { glDeleteTextures(1, &m_OverlayTextureID); m_OverlayTextureID = 0; }
	glGenTextures(1, &m_OverlayTextureID);
	glBindTexture(GL_TEXTURE_2D, m_OverlayTextureID);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, m_nWindowWidth, m_nWindowHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
	pfnGenFramebuffers(1, &m_OverlayFBO);
	pfnBindFramebuffer(GL_FRAMEBUFFER, m_OverlayFBO);
	pfnFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_OverlayTextureID, 0);
	pfnBindFramebuffer(GL_FRAMEBUFFER, 0);
	m_OverlayWidth  = m_nWindowWidth;
	m_OverlayHeight = m_nWindowHeight;
}

void Renderer::ActivateOverlayPhase()
{
	EnsureOverlay();
	if (!m_OverlayFBO) return;
	// Bind the overlay FBO and clear to transparent so the post-stereo
	// mono draws end up on a clean canvas. The composite blit at SwapBuffers
	// uses the alpha channel to know where the game actually wrote pixels.
	pfnBindFramebuffer(GL_FRAMEBUFFER, m_OverlayFBO);
	pfnOrig_glViewport(0, 0, m_nWindowWidth, m_nWindowHeight);
	glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	m_bOverlayActive = TRUE;
	m_bOverlayHasContent = TRUE;
	// Trace the activation event so the diag log shows when (and which
	// call) triggered the overlay. Useful when adding support for new
	// engines that don't follow the idTech 3 glOrtho pattern.
	if (g_traceFrameIdx < kTraceFrames)
	{
		DiagLogDrawBuffer(g_traceFrameIdx, g_traceCallIdx, GL_NONE,
			m_bSeenLeftEye, m_bSeenRightEye, m_bOverlayActive, "OVERLAY-ACTIVATED");
	}
}

// Called from any of the HUD-candidate hooks (glOrtho, glMatrixMode(PROJ),
// glDisable(GL_DEPTH_TEST)). Multiple triggers because engines vary on which
// GL call they emit when switching from 3D to 2D HUD; first one to fire
// after both eyes have rendered activates the overlay.
void Renderer::OnHudCandidate(const char* triggerName)
{
	if (g_traceFrameIdx < kTraceFrames)
	{
		DiagLogDrawBuffer(g_traceFrameIdx, g_traceCallIdx++, GL_NONE,
			m_bSeenLeftEye, m_bSeenRightEye, m_bOverlayActive, triggerName);
	}
	// Suppress re-trigger from our own compose-path GL calls (glDisable,
	// glOrtho, etc.). Without this, SwapBuffers' compose ends up routing
	// the wrapper's writes back into the overlay FBO and the back buffer
	// never receives the composite — symptom was full-white screen.
	if (m_bInCompose) return;
	// Activation deliberately disabled. Tracing the candidate triggers
	// confirmed that in RtCW's menu/intro phase (and likely other 2D-heavy
	// stages of idTech 3 games) every draw is in ortho projection — the
	// engine emits glMatrixMode/glOrtho/glDisable for the right-eye menu
	// rendering itself, before the per-eye loop reaches any HUD pass we
	// want to capture. Activating on the first such call after both eyes
	// are seen wrongly diverts right-eye content into the overlay FBO,
	// leaving the SR weave / SBS composite with an empty right eye and the
	// overlay blitting the right-eye menu on top — produces the
	// white/black flash. The diag trace, hook-status, hook-firstcall, and
	// the BlitOverlayOverBackBuffer plumbing all stay in place so this is
	// easy to re-enable once we have a reliable per-engine detection
	// signal. See OpenGLQuadBufferStereo.log trace for what the candidate
	// triggers look like in practice.
	(void)triggerName;
}

void Renderer::BlitOverlayOverBackBuffer(float /*fTextureCoordX*/, float /*fTextureCoordY*/)
{
	// Caller has already made m_hBackBufferContext current and bound the
	// default framebuffer. We sit on top of whatever the per-eye composite
	// just produced. Overlay texture is window-sized (not pow2-padded), so
	// texcoords are simply 0..1 across the full sampled region.
	typedef void (APIENTRY *PFN_UseProgram)(GLuint program);
	static PFN_UseProgram useProgram = NULL;
	if (!useProgram) useProgram = (PFN_UseProgram)wglGetProcAddress("glUseProgram");
	if (useProgram) useProgram(0);

	glActiveTextureARB(GL_TEXTURE0_ARB);
	glBindTexture(GL_TEXTURE_2D, m_OverlayTextureID);
	glEnable(GL_TEXTURE_2D);
	glDisable(GL_DEPTH_TEST);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glColor4f(1.0f, 1.0f, 1.0f, 1.0f);

	// Texture origin: the game drew with origin at bottom-left (GL convention),
	// so the overlay is already in GL coords. Fullscreen quad NDC -1..1.
	glBegin(GL_QUADS);
		glTexCoord2f(0.0f, 0.0f); glVertex2f(-1.0f, -1.0f);
		glTexCoord2f(1.0f, 0.0f); glVertex2f( 1.0f, -1.0f);
		glTexCoord2f(1.0f, 1.0f); glVertex2f( 1.0f,  1.0f);
		glTexCoord2f(0.0f, 1.0f); glVertex2f(-1.0f,  1.0f);
	glEnd();

	glDisable(GL_BLEND);
}

Renderer::~Renderer(void)
{
	//DestroyPB();
	// SR weave teardown must happen while the GL context is still alive so
	// the IGLWeaver1::destroy() call can free its GL resources cleanly.
	if (m_pSRWeave)
	{
		SRWeaveOGLContext* sr = (SRWeaveOGLContext*)m_pSRWeave;
		SRWeaveOGL_Cleanup(&sr);
		m_pSRWeave = nullptr;
	}
	// Katanga interop teardown must also happen with GL context alive so
	// wglDXUnregisterObjectNV / wglDXCloseDeviceNV complete cleanly.
	if (m_pKatanga)
	{
		KatangaOGLContext* kt = (KatangaOGLContext*)m_pKatanga;
		KatangaOGL_Cleanup(&kt);
		m_pKatanga = nullptr;
	}
	// NV3D-Lib shutter teardown while GL context + D3D9Ex device are both live.
	if (m_pNV3DGlasses)
	{
		NV3DGlassesOGLContext* nv = (NV3DGlassesOGLContext*)m_pNV3DGlasses;
		NV3DGlassesOGL_Cleanup(&nv);
		m_pNV3DGlasses = nullptr;
	}
	if (m_TextureID[0])
	{
		glDeleteTextures(1, &m_TextureID[0]);
		m_TextureID[0] = 0;
	}
	if (m_TextureID[1])
	{
		glDeleteTextures(1, &m_TextureID[1]);
		m_TextureID[1] = 0;
	}
	if (m_OverlayFBO && pfnDeleteFramebuffers)
	{
		pfnDeleteFramebuffers(1, &m_OverlayFBO);
		m_OverlayFBO = 0;
	}
	if (m_OverlayTextureID)
	{
		glDeleteTextures(1, &m_OverlayTextureID);
		m_OverlayTextureID = 0;
	}
	if (m_hBackBufferContext)
	{
		wglDeleteContext(m_hBackBufferContext);
		m_hBackBufferContext = 0;
	}
	// Sometimes fail
	//if (glDeleteObjectARB)
	//{
	//	if (m_hVS)
	//	{
	//		glDeleteObjectARB(m_hVS);
	//		m_hVS = NULL;
	//	}
	//	if (m_hFSBackScreen)
	//	{
	//		glDeleteObjectARB(m_hFSBackScreen);
	//		m_hFSBackScreen = NULL;
	//	}
	//	if (m_hPOFrontScreen)
	//	{
	//		glDeleteObjectARB(m_hPOFrontScreen);
	//		m_hPOFrontScreen = NULL;
	//	}
	//	if (m_hFSFrontScreen)
	//	{
	//		glDeleteObjectARB(m_hFSFrontScreen);
	//		m_hFSFrontScreen = NULL;
	//	}
	//	if (m_hPOBackScreen)
	//	{
	//		glDeleteObjectARB(m_hPOBackScreen);
	//		m_hPOBackScreen = NULL;
	//	}
	//}

	if(m_hFrontDC)
		ReleaseDC(m_hFrontWnd, m_hFrontDC);
	if (m_hFrontWnd)
		DestroyWindow(m_hFrontWnd);
}

BOOL Renderer::SetApplicationDC(HDC ApplicationDC) 
{ 
	DEBUG_TRACE1("%s(ApplicationDC = 0x%X)\n", _T( __FUNCTION__ ) , ApplicationDC);
	if (ApplicationDC == 0)
	{
		return TRUE;
	}
	m_hApplicationDC = ApplicationDC; 
	m_hWnd = WindowFromDC(ApplicationDC);
	DEBUG_TRACE1("WindowFromDC(ApplicationDC = 0x%X) returns m_hWnd = 0x%X\n", 
		m_hApplicationDC, m_hWnd);
	RECT Rect = { 0, 0, 0, 0 };
	BOOL bResult = GetClientRect(m_hWnd, &Rect); 
	DEBUG_TRACE1("GetClientRect(hWnd = 0x%X, Rect(%d, %d, %d, %d)) returns %d\n", 
		m_hWnd, Rect.left, Rect.top, Rect.right, Rect.bottom, bResult);
	m_nWindowWidth = Rect.right - Rect.left;
	m_nWindowHeight = Rect.bottom - Rect.top;
	DEBUG_TRACE1("m_nWindowWidth = %d, m_nWindowHeight = %d\n", m_nWindowWidth, m_nWindowHeight);
	HMONITOR hMonitor = MonitorFromWindow(m_hWnd, MONITOR_DEFAULTTONEAREST);
	MONITORINFO mi;
	mi.cbSize = sizeof(MONITORINFO);
	bResult = GetMonitorInfo(hMonitor, &mi);
	m_nMonitorWidth = mi.rcMonitor.right - mi.rcMonitor.left;
	m_nMonitorHeight = mi.rcMonitor.bottom - mi.rcMonitor.top;

	return TRUE;
}

void	Renderer::DrawBuffer(GLenum mode)
{
	BOOL bStereo = (mode == GL_FRONT_LEFT || mode == GL_FRONT_RIGHT ||	mode == GL_BACK_LEFT || mode == GL_BACK_RIGHT || mode == GL_LEFT || mode == GL_RIGHT);
	DEBUG_TRACE2("\tStereo(on = %d)\n", bStereo);
	if (g_traceFrameIdx < kTraceFrames)
	{
		DiagLogDrawBuffer(g_traceFrameIdx, g_traceCallIdx++, mode,
			m_bSeenLeftEye, m_bSeenRightEye, m_bOverlayActive, "DrawBuffer");
	}
	SetStereoRender(bStereo);
	if (!bStereo)
	{
		if (mode != GL_FRONT)
			mode = GL_BACK;
		m_MonoBuffer = (mode == GL_FRONT ? WGL_FRONT_LEFT_ARB : WGL_BACK_LEFT_ARB);
		// Mono-HUD overlay (path 1: rare — most engines don't emit a mono
		// DrawBuffer transition between eyes and HUD; OnGlOrtho is the
		// primary trigger). Once activated, GL_BACK / GL_FRONT are invalid
		// for the bound FBO, so skip the pass-through to avoid the driver
		// rejecting it and the next FBO draw landing in GL_NONE.
		if (gInfo.MonoHudOverlay && m_bSeenLeftEye && m_bSeenRightEye && !m_bOverlayActive)
		{
			ActivateOverlayPhase();
		}
		if (!m_bOverlayActive)
			pfnOrig_glDrawBuffer(mode);
		return;
	}
	// Track which eye-specific buffers have been seen this frame so the
	// overlay routing can fire at the right transition.
	if (mode == GL_BACK_LEFT  || mode == GL_FRONT_LEFT  || mode == GL_LEFT)  m_bSeenLeftEye  = TRUE;
	if (mode == GL_BACK_RIGHT || mode == GL_FRONT_RIGHT || mode == GL_RIGHT) m_bSeenRightEye = TRUE;
	// QBS observed — feeds the AnaglyphSteal=2 (auto) gate. With QBS active
	// we don't second-guess the engine even if it happens to glColorMask for
	// non-stereo reasons (alpha-only / channel-isolated FX).
	m_bSawQbsThisFrame = TRUE;
	HGLRC hCurrentContext = pfnOrig_wglGetCurrentContext();
	if (hCurrentContext != m_hPBufferContext)
		m_hApplicationContext = hCurrentContext;
	UINT nIndex = (mode == GL_FRONT_RIGHT || mode == GL_BACK_RIGHT || mode == GL_RIGHT) ? 1 : 0;

	m_bSwapBuffersCalled = FALSE;
	m_bStereoRender = TRUE;
	m_nDrawBufferMode[nIndex] = mode;

	if (nIndex == 0)
	{
		if (!Create())
		{
			DEBUG_MESSAGE("Error: Create() returns FALSE\n");
			return;
		}
	}
	pfnOrig_wglMakeCurrent(m_hPBufferDC, m_hPBufferContext);
	pfnOrig_glDrawBuffer(nIndex == 0 ? GL_FRONT : GL_BACK);
	//wglCopyContext(hCurrentContext, m_hPBufferContext, GL_ALL_ATTRIB_BITS);
}

BOOL	Renderer::ColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a)
{
	// AnaglyphSteal values:
	//   0 = off (default). Pass through every call.
	//   1 = always on. Recognise anaglyph-shaped masks and route per-eye.
	//   2 = auto. Same as 1 BUT only when QBS hasn't been observed in the
	//       current frame or the previous one — that means the game isn't
	//       using glDrawBuffer(BACK_LEFT/RIGHT), so a non-full glColorMask
	//       is most likely an anaglyph eye marker rather than a non-stereo
	//       channel mask (alpha-only / G-buffer write etc.).
	if (gInfo.AnaglyphSteal == 0) return FALSE;
	if (gInfo.AnaglyphSteal == 2)
	{
		if (m_bSawQbsThisFrame || m_bSawQbsLastFrame) return FALSE;
	}

	// Recognise the anaglyph signatures: at least one of R/G/B disabled, alpha
	// typically enabled. Pure (T,T,T,T) is the "restore" call — pass through.
	// Pure (F,F,F,F) is rare (channel-isolated stencil-only passes) and not
	// anaglyph — pass through too.
	const BOOL allOn   = (r && g && b);
	const BOOL allOff  = (!r && !g && !b);
	const BOOL anaglyphShaped = (!allOn && !allOff);
	if (!anaglyphShaped)
	{
		// Pass through unchanged via caller.
		return FALSE;
	}

	// Per-frame eye counter. First anaglyph-mask = left eye, second = right.
	// Anything beyond the second (rare; some games re-issue the mask mid-eye
	// for post-FX) gets clamped — we keep routing to the right-eye PBuffer
	// rather than spuriously kicking off a "third eye".
	int nIndex = (m_AnaglyphEyeIndexThisFrame >= 1) ? 1 : 0;
	m_AnaglyphEyeIndexThisFrame = nIndex + 1;
	m_bAnaglyphActiveFrame = TRUE;

	// Track which eyes we've seen for the mono-HUD overlay activation path
	// (gInfo.MonoHudOverlay). Without this the overlay would never fire in
	// anaglyph-steal mode because m_bSeenLeftEye/Right are only set by the
	// glDrawBuffer path.
	if (nIndex == 0) m_bSeenLeftEye  = TRUE;
	else             m_bSeenRightEye = TRUE;

	HGLRC hCurrentContext = pfnOrig_wglGetCurrentContext();
	if (hCurrentContext != m_hPBufferContext)
		m_hApplicationContext = hCurrentContext;

	m_bSwapBuffersCalled = FALSE;
	m_bStereoRender = TRUE;
	// Record synthetic DrawBuffer modes so SwapBuffers' composite picks them
	// up identically to the QBS path. The actual GL state we set just below
	// is glDrawBuffer(FRONT/BACK) within the PBuffer context.
	m_nDrawBufferMode[nIndex] = (nIndex == 0) ? GL_BACK_LEFT : GL_BACK_RIGHT;

	if (nIndex == 0)
	{
		if (!Create())
		{
			DEBUG_MESSAGE("Error: Create() returns FALSE (anaglyph-steal)\n");
			return FALSE;
		}
	}
	pfnOrig_wglMakeCurrent(m_hPBufferDC, m_hPBufferContext);
	pfnOrig_glDrawBuffer(nIndex == 0 ? GL_FRONT : GL_BACK);

	// Wipe the eye PBuffer's colour channel. The game's own Clear pattern
	// is biased toward "color+depth on left eye, depth-only on right eye"
	// because in native anaglyph the two eyes share a buffer — the second
	// Clear would erase eye 1's red pixels. With per-eye PBuffers we have
	// the opposite need: each eye buffer must start clean every frame, so
	// we issue our own color clear here regardless of what the game does.
	glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
	glClear(GL_COLOR_BUFFER_BIT);

	// Force the real glColorMask to all-true so the game's draws aren't
	// channel-filtered. Caller (HglColorMask hook) sees TRUE and skips its
	// own pass-through of the original mask.
	if (pfnOrig_glColorMask) pfnOrig_glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

	if (g_traceFrameIdx < kTraceFrames)
	{
		DiagLogDrawBuffer(g_traceFrameIdx, g_traceCallIdx++,
			(nIndex == 0) ? GL_BACK_LEFT : GL_BACK_RIGHT,
			m_bSeenLeftEye, m_bSeenRightEye, m_bOverlayActive, "AnaglyphSteal");
	}
	return TRUE;
}

BOOL    Renderer::MakeCurrent(HGLRC hglrc)
{
	m_hApplicationContext = hglrc;
	BOOL bResult;
	if (!m_bGlInit)
		bResult = pfnOrig_wglMakeCurrent (m_hApplicationDC, m_hApplicationContext);
	if (Create()/* && GetStereoRender()*/)
	{
		bResult = pfnOrig_wglMakeCurrent(m_hPBufferDC, m_hPBufferContext);
		DEBUG_TRACE1("%s(): wglMakeCurrent(m_hPBufferDC = 0x%X, m_hPBufferContext = 0x%X) = %d\n", 
			_T( __FUNCTION__ ) , m_hPBufferDC, m_hPBufferContext, bResult);
	}
	else
	  bResult = pfnOrig_wglMakeCurrent (m_hApplicationDC, hglrc);
	return bResult;
}

inline void	Renderer::SetStereoRender(BOOL bStereoRender) 
{ 
	if (m_bStereoRender != bStereoRender)
	{
		//if (bStereoRender)
		//	pfnOrig_wglMakeCurrent(m_hPBufferDC, m_hPBufferContext);
		//else
		//	pfnOrig_wglMakeCurrent (m_hApplicationDC, m_hApplicationContext);
		m_bStereoRender = bStereoRender; 
	}
}

BOOL	Renderer::Create()
{
	if (!m_bGlInit)
	{
		if (glh_init_extension("GL_VERSION_1_5") && glh_init_extension("GL_ARB_shader_objects") && glh_init_extension("WGL_ARB_pbuffer") && 
			glh_init_extension("WGL_ARB_render_texture") && glh_init_extension("WGL_ARB_pixel_format") && glh_init_extension("GL_ARB_multitexture"))
		{
			DEBUG_MESSAGE("Status: Using GLH\n");
			m_bGlInit = true;
		}
		else
		{
			DEBUG_MESSAGE("Error\n");
		}
		const char *extension = (const char*)glGetString(GL_EXTENSIONS);
		DEBUG_TRACE1("glGetString(GL_EXTENSIONS) returns \n%s\n", extension); 
	}

	static RECT Rect = { 0, 0, 0, 0 };
	BOOL bResult = GetClientRect(m_hWnd, &Rect); 
	if (Rect.right != m_nWindowWidth || Rect.bottom != m_nWindowHeight)
	{
		DEBUG_TRACE1("GetClientRect(hWnd = 0x%X, Rect(%d, %d, %d, %d)) returns %d\n", 
			m_hWnd, Rect.left, Rect.top, Rect.right, Rect.bottom, bResult);
		m_nWindowWidth = Rect.right - Rect.left;
		m_nWindowHeight = Rect.bottom - Rect.top;
		DEBUG_TRACE1("Window size changed\n");
		DEBUG_TRACE1("m_nWindowWidth = %d, m_nWindowHeight = %d\n", m_nWindowWidth, m_nWindowHeight);
		if (m_nWindowWidth > m_nPBufferWidth || m_nWindowHeight > m_nPBufferHeight)
			m_bCreated = 0;
	}

	if (m_bCreated)
	{
		int flag = 0;
		wglQueryPbufferARB(m_hPBuffer, WGL_PBUFFER_LOST_ARB, &flag);
		if (flag != 0)
		{
			m_bCreated = FALSE;
		}
	}
	if (!m_bCreated)
	{
		BOOL bResult = CreatePB();
		if (!bResult)
		{
			DEBUG_MESSAGE("Error: CreatePB() returns FALSE\n");
			return FALSE;
		}
		else if (gInfo.EmulateQB && gInfo.OutputMode == OGL_OUTPUT_IZ3D)
		{
			if (!m_bFrontWindowCreated && !CreateFrontWindow())
			{
				DEBUG_MESSAGE("Error: CreateFrontWindow() returns FALSE\n");
			}
		}
	}
	return TRUE;
}

BOOL	Renderer::CreatePB()
{
	DEBUG_TRACE1("%s()\n", _T( __FUNCTION__ ) );
	DestroyPB();

	PIXELFORMATDESCRIPTOR  pfd;
	int  iPixelFormat = pfnOrig_wglGetPixelFormat(m_hApplicationDC); 
	pfnOrig_wglDescribePixelFormat(m_hApplicationDC, iPixelFormat, sizeof(PIXELFORMATDESCRIPTOR), &pfd); 
	DEBUG_TRACE1("%s\n", GetPixelFormatDescriptorString(&pfd));
	int attr[] =
	{
		WGL_SUPPORT_OPENGL_ARB, TRUE,
		WGL_DRAW_TO_PBUFFER_ARB, TRUE,
		WGL_BIND_TO_TEXTURE_RGBA_ARB, TRUE,
		WGL_DRAW_TO_WINDOW_ARB, TRUE,
		WGL_ACCELERATION_ARB, WGL_FULL_ACCELERATION_ARB,
//		WGL_PIXEL_TYPE_ARB, pfd.iPixelType,
		WGL_COLOR_BITS_ARB, pfd.cColorBits,
		WGL_RED_BITS_ARB, pfd.cRedBits,
//		WGL_RED_SHIFT_ARB, pfd.cRedShift,
		WGL_GREEN_BITS_ARB, pfd.cGreenBits,
//		WGL_GREEN_SHIFT_ARB, pfd.cGreenShift,
		WGL_BLUE_BITS_ARB, pfd.cBlueBits,
//		WGL_BLUE_SHIFT_ARB, pfd.cBlueShift,
		WGL_ALPHA_BITS_ARB, pfd.cAlphaBits,
//		WGL_ALPHA_SHIFT_ARB, pfd.cAlphaShift,
		WGL_DEPTH_BITS_ARB, pfd.cDepthBits,
		WGL_STENCIL_BITS_ARB, pfd.cStencilBits,
		WGL_ACCUM_BITS_ARB, pfd.cAccumBits,
		WGL_ACCUM_RED_BITS_ARB, pfd.cAccumRedBits,
		WGL_ACCUM_GREEN_BITS_ARB, pfd.cAccumGreenBits,
		WGL_ACCUM_BLUE_BITS_ARB, pfd.cAccumBlueBits,
		WGL_ACCUM_ALPHA_BITS_ARB, pfd.cAccumAlphaBits,
		WGL_DOUBLE_BUFFER_ARB, TRUE,
//		WGL_STEREO_ARB, TRUE,
		0
	};
	unsigned int count = 0;
	int pixelFormat;
	wglChoosePixelFormatARB(m_hApplicationDC, (const int *)attr, NULL, 1, &pixelFormat, &count);
	if (count == 0)
	{
		DEBUG_TRACE1("P-buffer Error: Unable to find an acceptable pixel format");
		return FALSE;
	}
	int pAttrib[] =
	{
		WGL_TEXTURE_FORMAT_ARB,
		WGL_TEXTURE_RGBA_ARB,
		WGL_TEXTURE_TARGET_ARB,
		WGL_TEXTURE_2D_ARB,
		WGL_PBUFFER_LARGEST_ARB,
		TRUE,
		0
	};

	if (m_nWindowWidth == 0 || m_nWindowHeight == 0)
	{
		return FALSE;
	}
	UINT nHeight = FindPow2(max(m_nWindowHeight, m_nMonitorHeight));
	UINT nWidth = FindPow2(max(m_nWindowWidth, m_nMonitorWidth));
	m_hPBuffer = wglCreatePbufferARB(m_hApplicationDC, pixelFormat, 
		nWidth, nHeight, pAttrib);
	DEBUG_TRACE1("wglCreatePbufferARB(m_hApplicationDC = 0x%X) "
		"returns m_hPBuffer = 0x%X\n", m_hApplicationDC, m_hPBuffer);
	if (m_hPBuffer == 0)
	{
		DEBUG_TRACE1("P-buffer Error: Unable to create P-buffer");
		return FALSE;
	}
	m_hPBufferDC = wglGetPbufferDCARB(m_hPBuffer);
	DEBUG_TRACE1("wglGetPbufferDCARB(m_hPBuffer = 0x%X) "
		"returns m_hPBufferDC = 0x%X\n", m_hPBuffer, m_hPBufferDC);

	wglQueryPbufferARB(m_hPBuffer, WGL_PBUFFER_WIDTH_ARB, (int *)&m_nPBufferWidth);
	wglQueryPbufferARB(m_hPBuffer, WGL_PBUFFER_HEIGHT_ARB, (int *)&m_nPBufferHeight);
	DEBUG_TRACE1("m_nPBufferWidth = %d, m_nPBufferHeight = %d\n", 
		m_nPBufferWidth, m_nPBufferHeight);
	m_bCreated = TRUE;

	HGLRC hPBufferContext = m_hPBufferContext;
	m_hPBufferContext = pfnOrig_wglCreateContext(m_hPBufferDC);
	if (hPBufferContext != 0)
	{
		wglDeleteContext(hPBufferContext);
	}

	DEBUG_TRACE1("%s() returns TRUE\n", _T( __FUNCTION__ ) );
	return TRUE;
}

void	Renderer::DestroyPB()
{
	if (m_hPBuffer != 0)
	{
		if (m_hPBufferDC != 0)
		{    
			wglReleasePbufferDCARB(m_hPBuffer, m_hPBufferDC);
			m_hPBufferDC = 0;
		}
		wglDestroyPbufferARB(m_hPBuffer);
		m_hPBuffer = 0;
	}
}

BOOL	Renderer::CheckGLError()
{
	GLenum err = glGetError();
	if (err != GL_NO_ERROR)
	{   
#ifdef _DEBUG
		DEBUG_MESSAGE("OpenGL error\n");
#endif
		return FALSE;
	}  
	return TRUE;
}

inline BOOL	Renderer::CheckGLSLError(GLhandleARB Handle, GLenum Param)
{
	if (!CheckGLError())
		return FALSE;
	GLint glsl_ok;
	glGetObjectParameterivARB(Handle, Param, &glsl_ok);
	char s[1000];
	GLint l;
	glGetInfoLogARB(Handle, _countof(s), &l, s);
	DEBUG_MESSAGE("\t%s", s);
	return glsl_ok == 0;
}

BOOL	Renderer::InitializeShaders()
{
	m_bShadersInitialized = TRUE;
	HDC hCurrentDC = pfnOrig_wglGetCurrentDC();
	HGLRC hCurrentContext = pfnOrig_wglGetCurrentContext();
	_TRY_BEGIN
		m_hBackBufferContext = pfnOrig_wglCreateContext(m_hApplicationDC);
		pfnOrig_wglMakeCurrent(m_hApplicationDC, m_hBackBufferContext);
		glGenTextures(2, (GLuint *)&m_TextureID);
		glBindTexture(GL_TEXTURE_2D, m_TextureID[0]);
		glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
		glBindTexture(GL_TEXTURE_2D, m_TextureID[1]);
		glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);

		m_hPOBackScreen = glCreateProgramObjectARB();
		m_hPOFrontScreen = glCreateProgramObjectARB();

		m_hVS = glCreateShaderObjectARB(GL_VERTEX_SHADER_ARB);    
		GLcharARB *str[1];
		str[0] = g_szVertexShaderText;
		glShaderSourceARB(m_hVS, 1, (const char**)str, NULL);  
		glCompileShaderARB(m_hVS);
		CheckGLSLError(m_hVS, GL_OBJECT_COMPILE_STATUS_ARB);

		m_hFSBackScreen = glCreateShaderObjectARB(GL_FRAGMENT_SHADER_ARB);
		m_hFSFrontScreen = glCreateShaderObjectARB(GL_FRAGMENT_SHADER_ARB);
		const char* szBackFS;
		const char* szFrontFS;
		// All SBS / T-B variants use the simple per-eye RAW shaders; the
		// half-vs-full distinction is purely geometry (quad position +
		// game-resolution hint to the user), not shader code.
		switch (gInfo.OutputMode)
		{
		case OGL_OUTPUT_HALF_SBS:
		case OGL_OUTPUT_FULL_SBS:
		case OGL_OUTPUT_HALF_TB:
		case OGL_OUTPUT_FULL_TB:
		case OGL_OUTPUT_KATANGA:   // Katanga publishes elsewhere; the on-screen fallback is SBS
		case OGL_OUTPUT_3DVISION_GLASSES:  // NV3D-Lib shutter publishes elsewhere; same on-screen fallback
			szBackFS  = g_szRAWLeftShaderText;
			szFrontFS = g_szRAWRightShaderText;
			break;
		case OGL_OUTPUT_ANAGLYPH:
			szBackFS  = g_szAnaglyphShaderText;
			szFrontFS = g_szRAWRightShaderText;
			break;
		case OGL_OUTPUT_ROW_INTERLEAVED:
			szBackFS  = g_szRowInterleavedShaderText;
			szFrontFS = g_szRAWRightShaderText;
			break;
		case OGL_OUTPUT_COLUMN_INTERLEAVED:
			szBackFS  = g_szColumnInterleavedShaderText;
			szFrontFS = g_szRAWRightShaderText;
			break;
		case OGL_OUTPUT_CHECKERBOARD:
			szBackFS  = g_szCheckerboardShaderText;
			szFrontFS = g_szRAWRightShaderText;
			break;
		case OGL_OUTPUT_IZ3D:
		default:
			szBackFS  = g_szBackScreenShaderText;
			szFrontFS = g_szFrontScreenCFLShaderText;
			break;
		}

		str[0] = (GLcharARB*)szBackFS;
		glShaderSourceARB(m_hFSBackScreen, 1, (const char**)str, NULL);  
		glCompileShaderARB(m_hFSBackScreen);
		CheckGLSLError(m_hFSBackScreen, GL_OBJECT_COMPILE_STATUS_ARB);

		glAttachObjectARB(m_hPOBackScreen, m_hFSBackScreen);
		glAttachObjectARB(m_hPOBackScreen, m_hVS);
		glLinkProgramARB(m_hPOBackScreen);  
		CheckGLSLError(m_hPOBackScreen, GL_OBJECT_LINK_STATUS_ARB);
		glValidateProgramARB(m_hPOBackScreen);
		CheckGLSLError(m_hPOBackScreen, GL_OBJECT_VALIDATE_STATUS_ARB);

		str[0] = (GLcharARB*)szFrontFS;
		glShaderSourceARB(m_hFSFrontScreen, 1, (const char**)str, NULL);
		glCompileShaderARB(m_hFSFrontScreen);  
		CheckGLSLError(m_hFSFrontScreen, GL_OBJECT_COMPILE_STATUS_ARB);

		glAttachObjectARB(m_hPOFrontScreen, m_hFSFrontScreen);
		glAttachObjectARB(m_hPOFrontScreen, m_hVS);
		glLinkProgramARB(m_hPOFrontScreen);  
		CheckGLSLError(m_hPOFrontScreen, GL_OBJECT_LINK_STATUS_ARB);
		glValidateProgramARB(m_hPOFrontScreen);
		CheckGLSLError(m_hPOFrontScreen, GL_OBJECT_VALIDATE_STATUS_ARB);
	_CATCH_ALL
		pfnOrig_wglMakeCurrent(hCurrentDC, hCurrentContext);
		return FALSE;
	_CATCH_END
	pfnOrig_wglMakeCurrent(hCurrentDC, hCurrentContext);
	return TRUE;
}

void Renderer::SaveBufferToFile(int width, int height, char* filename)
{
	BYTE* bmpBuffer = (BYTE*)malloc(width*height*3);
	if (!bmpBuffer)
		return;

	HDC hCurrentDC = pfnOrig_wglGetCurrentDC();
	int iPixelFormat = pfnOrig_wglGetPixelFormat(hCurrentDC);
	PIXELFORMATDESCRIPTOR pfd;
	pfnOrig_wglDescribePixelFormat(hCurrentDC, iPixelFormat, sizeof(PIXELFORMATDESCRIPTOR), &pfd);
	
	glReadPixels((GLint)0, (GLint)0,
		(GLint)width-1, (GLint)height-1,
		(pfd.cRedShift == 0 ? GL_RGB : GL_BGR), GL_UNSIGNED_BYTE, bmpBuffer);

	FILE *filePtr;	
	if (fopen_s(&filePtr, filename, "wb"))
		return;

	BITMAPFILEHEADER bitmapFileHeader;
	BITMAPINFOHEADER bitmapInfoHeader;

	bitmapFileHeader.bfType = 0x4D42; //"BM"
	bitmapFileHeader.bfSize = width*height*3;
	bitmapFileHeader.bfReserved1 = 0;
	bitmapFileHeader.bfReserved2 = 0;
	bitmapFileHeader.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);

	bitmapInfoHeader.biSize = sizeof(BITMAPINFOHEADER);
	bitmapInfoHeader.biWidth = width-1;
	bitmapInfoHeader.biHeight = height-1;
	bitmapInfoHeader.biPlanes = 1;
	bitmapInfoHeader.biBitCount = 24;
	bitmapInfoHeader.biCompression = BI_RGB;
	bitmapInfoHeader.biSizeImage = 0;
	bitmapInfoHeader.biXPelsPerMeter = 0; // ?
	bitmapInfoHeader.biYPelsPerMeter = 0; // ?
	bitmapInfoHeader.biClrUsed = 0;
	bitmapInfoHeader.biClrImportant = 0;

	fwrite(&bitmapFileHeader, sizeof(BITMAPFILEHEADER), 1, filePtr);
	fwrite(&bitmapInfoHeader, sizeof(BITMAPINFOHEADER), 1, filePtr);
	fwrite(bmpBuffer, width*height*3, 1, filePtr);
	fclose(filePtr);

	free(bmpBuffer);
}

void Renderer::DumpBuffer( char * szFileName, GLenum buf, char* s )
{
	pfnOrig_glReadBuffer(buf);
	sprintf_s(szFileName, MAX_PATH, "%s\\%s (%s).bmp", gInfo.LogFileDirectory, s, GetDrawBufferFlagsString(buf));
	SaveBufferToFile(m_nWindowWidth, m_nWindowHeight, szFileName);
	DEBUG_MESSAGE("Dump buffer %s (%s)\n", s, GetDrawBufferFlagsString(buf));
}

BOOL CALLBACK MonitorEnumProc( HMONITOR hMonitor,  HDC hdcMonitor, LPRECT lprcMonitor, LPARAM dwData)
{
	Renderer* rend = (Renderer*)dwData;
	MONITORINFOEX mi; 
	mi.cbSize = sizeof(MONITORINFOEX);
	GetMonitorInfo(hMonitor, &mi);
	if(mi.dwFlags == MONITORINFOF_PRIMARY)
	{
		rend->m_BackScreenRect = mi.rcMonitor;
		rend->m_hBackScreen = hMonitor;
	}
	else
	{
		rend->m_FrontScreenRect = mi.rcMonitor;
		rend->m_hFrontScreen = hMonitor;
	}
	if (IsRectEmpty(&rend->m_BackScreenRect) || IsRectEmpty(&rend->m_FrontScreenRect))
		return TRUE;
	else
		return FALSE;
}

BOOL	Renderer::SwapBuffers()
{
	BOOL bStereo = GetStereoRender();
	DEBUG_TRACE2("\tStereo(on = %d)\n", bStereo);
	// One-shot: record whether the game's window actually got a quad-buffer
	// stereo context. The game's own context is current here (before any of our
	// wglMakeCurrent calls below), and this sits ahead of the no-PBuffer early
	// return, so mono games that never trigger capture still log it exactly
	// once. GL_STEREO=0 + no AnaglyphSteal trace = nothing we can capture.
	if (!m_bLoggedContextCaps)
	{
		m_bLoggedContextCaps = TRUE;
		GLboolean ctxStereo = GL_FALSE, ctxDouble = GL_FALSE;
		glGetBooleanv(GL_STEREO, &ctxStereo);
		glGetBooleanv(GL_DOUBLEBUFFER, &ctxDouble);
		DiagLogContextCaps(ctxStereo != GL_FALSE, ctxDouble != GL_FALSE);
	}
	if (!m_hPBuffer)
		return pfnOrig_wglSwapBuffers(m_hApplicationDC);
	m_bSwapBuffersCalled = TRUE;

	HDC hCurrentDC = pfnOrig_wglGetCurrentDC();
	HGLRC hCurrentContext = pfnOrig_wglGetCurrentContext();

	if (!m_bShadersInitialized && !InitializeShaders())
	{
		DEBUG_MESSAGE("Error: InitializeShaders() returns FALSE\n");
		return FALSE;
	}

	if(gInfo.OutputMode == OGL_OUTPUT_IZ3D && m_SwapBuffersCount % 30 == 0)
	{
		MONITORINFOEX mi;
		mi.cbSize = sizeof MONITORINFOEX;
		GetMonitorInfo(m_hBackScreen, &mi);				
		DEVMODE deviceMode;
		deviceMode.dmSize = sizeof(DEVMODE);
		deviceMode.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL | DM_DISPLAYFREQUENCY;
		EnumDisplaySettings(mi.szDevice, ENUM_CURRENT_SETTINGS, &deviceMode);

		MONITORINFOEX mi2;
		mi2.cbSize = sizeof MONITORINFOEX;
		GetMonitorInfo(m_hFrontScreen, &mi2);
		DEVMODE deviceMode2;
		deviceMode2.dmSize = sizeof(DEVMODE);
		deviceMode2.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL | DM_DISPLAYFREQUENCY;
		EnumDisplaySettings(mi2.szDevice, ENUM_CURRENT_SETTINGS, &deviceMode2);

		if(deviceMode.dmBitsPerPel != deviceMode2.dmBitsPerPel ||
			deviceMode.dmPelsWidth != deviceMode2.dmPelsWidth ||
			deviceMode.dmPelsHeight != deviceMode2.dmPelsHeight)
		{
			DEBUG_MESSAGE("WARNING: Different display mode for secondary adapter\n");
			DEBUG_MESSAGE("Set display mode on second adapter\n");
			if (deviceMode2.dmPosition.x < 0)
				deviceMode2.dmPosition.x += deviceMode2.dmPelsWidth - deviceMode.dmPelsWidth;
			deviceMode.dmPosition = deviceMode2.dmPosition;
			ChangeDisplaySettingsEx((LPTSTR)mi2.szDevice, &deviceMode, NULL, CDS_FULLSCREEN, NULL);

			SetRectEmpty(&m_BackScreenRect);
			SetRectEmpty(&m_FrontScreenRect);
			EnumDisplayMonitors(NULL, NULL, MonitorEnumProc, (LPARAM)this);
			if (IsRectEmpty(&m_FrontScreenRect))
			{
				m_hFrontScreen = m_hBackScreen;
				SetRect(&m_FrontScreenRect, m_BackScreenRect.right, m_BackScreenRect.top, m_BackScreenRect.left + m_BackScreenRect.right, m_BackScreenRect.bottom);
			}
		} else if(deviceMode.dmDisplayFrequency != deviceMode2.dmDisplayFrequency)
		{
			DEBUG_MESSAGE("WARNING: Refresh rate doesn't match\n");
		}
	}

	if (gInfo.EmulateQB && m_bFrontWindowCreated)
	{
		WINDOWINFO wi;
		ZeroMemory(&wi, sizeof(WINDOWINFO));
		wi.cbSize = sizeof(WINDOWINFO);
		if (!GetWindowInfo(m_hWnd, &wi))
		{
			DEBUG_MESSAGE("Error: GetWindowInfo() returns FALSE\n");
			DestroyWindow(m_hFrontWnd);
			m_hFrontWnd = 0;
			m_bFrontWindowCreated = false;
		}
		else
		{
			SetWindowPos(m_hFrontWnd, HWND_TOP, 
				wi.rcClient.left + m_FrontScreenRect.left, wi.rcClient.top + m_FrontScreenRect.top, 
				wi.rcClient.right - wi.rcClient.left,
				wi.rcClient.bottom - wi.rcClient.top,
				SWP_NOZORDER | SWP_NOACTIVATE);	
		}	
	}
	BOOL bDoDump = FALSE;
	if(GINFO_DEBUG && IsKeyDown(VK_SCROLL))
	{
		bDoDump = TRUE;
		char szFileName[MAX_PATH];
		GLenum oldBuf;
		glGetIntegerv(GL_READ_BUFFER, (GLint*)&oldBuf);
		DumpBuffer(szFileName, GL_FRONT, "1. Left");
		DumpBuffer(szFileName, GL_BACK, "1. Right");
		pfnOrig_glReadBuffer(oldBuf);
	}
	pfnOrig_wglMakeCurrent(m_hApplicationDC, m_hBackBufferContext);

	// If the game routed mono draws into our overlay FBO this frame, unbind
	// it now so the per-eye composite below writes to the actual back buffer.
	// The overlay texture stays populated and gets blitted on top at the end.
	if (m_bOverlayActive && pfnBindFramebuffer)
	{
		pfnBindFramebuffer(GL_FRAMEBUFFER, 0);
		m_bOverlayActive = FALSE;
	}
	// Mark "inside compose" so the wrapper's own glDisable/glOrtho/glMatrixMode
	// calls (the SBS / SR weave / anaglyph shaders all touch GL state) don't
	// feed back through OnHudCandidate and re-activate the overlay mid-compose.
	m_bInCompose = TRUE;

	//glClearColor(0, 1, 0, 0);
	//glClear(GL_COLOR_BUFFER_BIT);
	// Diagnostic: log dims once per change so we can spot viewport/back-buffer
	// mismatches in FINAL_RELEASE builds (where ZLOG is compiled out).
	{
		static UINT s_lastW = 0, s_lastH = 0, s_lastPW = 0, s_lastPH = 0;
		if (m_nWindowWidth != s_lastW || m_nWindowHeight != s_lastH ||
		    m_nPBufferWidth != s_lastPW || m_nPBufferHeight != s_lastPH)
		{
			s_lastW  = m_nWindowWidth;  s_lastH  = m_nWindowHeight;
			s_lastPW = m_nPBufferWidth; s_lastPH = m_nPBufferHeight;
			RECT cr = { 0, 0, 0, 0 };
			GetClientRect(m_hWnd, &cr);
			GLint vp[4] = { 0, 0, 0, 0 };
			glGetIntegerv(GL_VIEWPORT, vp);
			DiagLogOnce("pre-compose", m_nWindowWidth, m_nWindowHeight,
				m_nPBufferWidth, m_nPBufferHeight, cr, vp,
				gInfo.OutputMode, gInfo.EmulateQB);
		}
	}
	pfnOrig_glViewport(0, 0, m_nWindowWidth, m_nWindowHeight);
	float fTextureCoordX = (float)m_nWindowWidth / m_nPBufferWidth;
	float fTextureCoordY = (float)m_nWindowHeight / m_nPBufferHeight;
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_STENCIL_TEST);

	// SwapEyes applies uniformly to every OutputMode: swap which PBuffer eye
	// binds to sampler 0 (sL) vs sampler 1 (sR). Replaces the legacy
	// "Crosseyed" output mode, which only swapped for SBS.
	GLenum leftEye  = gInfo.SwapEyes ? WGL_BACK_LEFT_ARB  : WGL_FRONT_LEFT_ARB;
	GLenum rightEye = gInfo.SwapEyes ? WGL_FRONT_LEFT_ARB : WGL_BACK_LEFT_ARB;

	glEnable(GL_TEXTURE_2D);
	glActiveTextureARB( GL_TEXTURE0_ARB );
	glBindTexture(GL_TEXTURE_2D, m_TextureID[0]);
	wglBindTexImageARB(m_hPBuffer, bStereo ? leftEye  : m_MonoBuffer);
	glActiveTextureARB( GL_TEXTURE1_ARB );
	glBindTexture(GL_TEXTURE_2D, m_TextureID[1]);
	wglBindTexImageARB(m_hPBuffer, bStereo ? rightEye : m_MonoBuffer);

	// NV3D-Lib shutter publish runs independently of (and before) the
	// on-screen composite. Same pattern as the Katanga block below: the
	// game window still gets an SBS fallback so the user sees content on
	// their monitor whether or not 3D Vision hardware is present.
	if (gInfo.OutputMode == OGL_OUTPUT_3DVISION_GLASSES && bStereo)
	{
		if (!m_pNV3DGlasses && !m_bNV3DGlassesTriedInit)
		{
			m_bNV3DGlassesTriedInit = TRUE;
			NV3DGlassesOGLContext* nv = nullptr;
			HGLRC glctx = wglGetCurrentContext();
			HDC   gldc  = wglGetCurrentDC();
			NV3DGlassesOGL_Initialize(&nv, glctx, gldc, m_hWnd,
				m_nWindowWidth, m_nWindowHeight, gInfo.SwapEyes != 0);
			m_pNV3DGlasses = nv;

			TCHAR path[MAX_PATH];
			_tcscpy_s(path, MAX_PATH, gInfo.DriverDirectory);
			_tcscat_s(path, MAX_PATH, _T("\\OpenGLQuadBufferStereo.log"));
			FILE* f = NULL;
			_tfopen_s(&f, path, _T("a"));
			if (f)
			{
				bool fb = nv && NV3DGlassesOGL_IsFallback((NV3DGlassesOGLContext*)nv);
				const char* status = (nv && !fb) ? "ok"
					: fb ? "init returned fallback (no 3D Vision / NVAPI / interop) - SBS only"
					     : "init returned false - SBS only";
				fprintf(f, "[wiz3D-OGL diag NV3D-init] viewW=%u viewH=%u status=%s\n",
					m_nWindowWidth, m_nWindowHeight, status);
				fflush(f);
				fclose(f);
			}
		}
		if (m_pNV3DGlasses && !NV3DGlassesOGL_IsFallback((NV3DGlassesOGLContext*)m_pNV3DGlasses))
		{
			NV3DGlassesOGL_Render((NV3DGlassesOGLContext*)m_pNV3DGlasses,
				(unsigned int)m_TextureID[0], (unsigned int)m_TextureID[1],
				m_nWindowWidth, m_nWindowHeight,
				gInfo.SwapEyes != 0);
		}
	}

	// Katanga publish runs independently of (and before) the on-screen
	// composite. The game window still gets an SBS fallback below so the
	// user sees content whether or not a Katanga consumer is attached.
	if (gInfo.OutputMode == OGL_OUTPUT_KATANGA && bStereo)
	{
		if (!m_pKatanga && !m_bKatangaTriedInit)
		{
			m_bKatangaTriedInit = TRUE;
			KatangaOGLContext* kt = nullptr;
			KatangaOGL_Initialize(&kt, m_hWnd, m_nWindowWidth, m_nWindowHeight);
			m_pKatanga = kt;

			TCHAR path[MAX_PATH];
			_tcscpy_s(path, MAX_PATH, gInfo.DriverDirectory);
			_tcscat_s(path, MAX_PATH, _T("\\OpenGLQuadBufferStereo.log"));
			FILE* f = NULL;
			_tfopen_s(&f, path, _T("a"));
			if (f)
			{
				bool fb = kt && KatangaOGL_IsFallback((KatangaOGLContext*)kt);
				const char* status = (kt && !fb) ? "ok"
					: fb ? "init returned fallback (d3d11 / interop / Katanga missing) - SBS only"
					     : "init returned false - SBS only";
				fprintf(f, "[wiz3D-OGL diag Katanga-init] viewW=%u viewH=%u status=%s\n",
					m_nWindowWidth, m_nWindowHeight, status);
				fflush(f);
				fclose(f);
			}
		}
		if (m_pKatanga && !KatangaOGL_IsFallback((KatangaOGLContext*)m_pKatanga))
		{
			KatangaOGL_Render((KatangaOGLContext*)m_pKatanga,
				(unsigned int)m_TextureID[0], (unsigned int)m_TextureID[1],
				fTextureCoordX, fTextureCoordY,
				m_nWindowWidth, m_nWindowHeight,
				gInfo.SwapEyes != 0);
		}
	}

	// ---- Output compositing ----
	GLint sL, sR;
	if (gInfo.OutputMode == OGL_OUTPUT_SR_WEAVE)
	{
		// Simulated Reality weave path. Lazy-init the SR weaver on first call (HWND,
		// per-eye textures and GL context all need to be live by now);
		// then per frame copy the eyes into the SBS texture and let the
		// SR runtime weave to the default framebuffer. Falls back to SBS
		// on any SR failure for the rest of the session.
		if (!m_pSRWeave && !m_bSRWeaveTriedInit)
		{
			m_bSRWeaveTriedInit = TRUE;
			SRWeaveOGLContext* sr = nullptr;
			bool ok = SRWeaveOGL_Initialize(&sr, m_hWnd, m_nWindowWidth, m_nWindowHeight);
			m_pSRWeave = sr;
			// Diagnostic: log SR init outcome to OpenGLQuadBufferStereo.log so
			// users can tell whether SR runtime / context / weaver came up, or
			// we're silently falling back to plain SBS.
			TCHAR path[MAX_PATH];
			_tcscpy_s(path, MAX_PATH, gInfo.DriverDirectory);
			_tcscat_s(path, MAX_PATH, _T("\\OpenGLQuadBufferStereo.log"));
			FILE* f = NULL;
			_tfopen_s(&f, path, _T("a"));
			if (f)
			{
				bool fb = sr && SRWeaveOGL_IsFallback((SRWeaveOGLContext*)sr);
				const char* status = ok && !fb ? "ok"
					: fb ? "init returned fallback (SR runtime/context/weaver missing) - using plain SBS"
					: "init returned false - using plain SBS";
				fprintf(f, "[wiz3D-OGL diag SR-init] viewWidth=%u viewHeight=%u status=%s\n",
					m_nWindowWidth, m_nWindowHeight, status);
				fflush(f);
				fclose(f);
			}
		}
		bool srOk = false;
		if (m_pSRWeave && !SRWeaveOGL_IsFallback((SRWeaveOGLContext*)m_pSRWeave))
		{
			pfnOrig_glDrawBuffer(m_nDrawBufferMode[0]);
			srOk = SRWeaveOGL_Render((SRWeaveOGLContext*)m_pSRWeave,
				(unsigned int)m_TextureID[0], (unsigned int)m_TextureID[1],
				fTextureCoordX, fTextureCoordY,
				m_nWindowWidth, m_nWindowHeight);
		}
		if (!srOk)
		{
			// SR runtime missing / broken — render plain Half SBS as
			// fallback so the user still sees an image. Two textured
			// quads, left into left half of the back buffer, right into
			// right half. Same as the SBS path further down but
			// inlined to keep the dispatch flat.
			pfnOrig_glDrawBuffer(m_nDrawBufferMode[0]);
			glUseProgramObjectARB(0);
			glEnable(GL_TEXTURE_2D);
			glActiveTextureARB(GL_TEXTURE0_ARB);
			glBindTexture(GL_TEXTURE_2D, m_TextureID[0]);
			glBegin(GL_QUADS);
				glTexCoord2f(0,              0             ); glVertex2f(-1, -1);
				glTexCoord2f(0,              fTextureCoordY); glVertex2f(-1,  1);
				glTexCoord2f(fTextureCoordX, fTextureCoordY); glVertex2f( 0,  1);
				glTexCoord2f(fTextureCoordX, 0             ); glVertex2f( 0, -1);
			glEnd();
			glBindTexture(GL_TEXTURE_2D, m_TextureID[1]);
			glBegin(GL_QUADS);
				glTexCoord2f(0,              0             ); glVertex2f( 0, -1);
				glTexCoord2f(0,              fTextureCoordY); glVertex2f( 0,  1);
				glTexCoord2f(fTextureCoordX, fTextureCoordY); glVertex2f( 1,  1);
				glTexCoord2f(fTextureCoordX, 0             ); glVertex2f( 1, -1);
			glEnd();
		}
	}
	else if (gInfo.OutputMode == OGL_OUTPUT_IZ3D)
	{
		// iZ3D dual-panel path: FrontScreen to second adapter, BackScreen to primary
		GLenum buf;
		if (gInfo.EmulateQB)
		{
			if (m_bFrontWindowCreated)
			{
				pfnOrig_wglMakeCurrent(m_hFrontDC, m_hBackBufferContext);
				buf = m_nDrawBufferMode[1] == GL_BACK || m_nDrawBufferMode[1] == GL_BACK_LEFT || m_nDrawBufferMode[1] == GL_BACK_RIGHT ? GL_BACK : GL_FRONT;
				DEBUG_TRACE2("\tFront = %s\n", GetDrawBufferFlagsString(buf));
				pfnOrig_glDrawBuffer(buf);
			}
		}
		else
		{
			buf = m_nDrawBufferMode[1];
			DEBUG_TRACE2("\tFront = %s\n", GetDrawBufferFlagsString(buf));
			pfnOrig_glDrawBuffer(m_nDrawBufferMode[1]); // RIGHT
		}

		glUseProgramObjectARB(m_hPOFrontScreen);
		sL = glGetUniformLocationARB(m_hPOFrontScreen, "sL");
		if (sL != -1)
			glUniform1iARB(sL, 0);
		sR = glGetUniformLocationARB(m_hPOFrontScreen, "sR");
		if (sR != -1)
			glUniform1iARB(sR, 1);
		CheckGLError();
		glBegin( GL_QUADS );
		glTexCoord2f(0, 0);
		glVertex2f(-1, -1);
		glTexCoord2f(0, fTextureCoordY);
		glVertex2f(-1, 1);
		glTexCoord2f(fTextureCoordX, fTextureCoordY);
		glVertex2f( 1, 1);
		glTexCoord2f(fTextureCoordX, 0);
		glVertex2f( 1, -1);
		glEnd();
		if(bDoDump)
		{
			char szFileName[MAX_PATH];
			GLenum oldBuf;
			glGetIntegerv(GL_READ_BUFFER, (GLint*)&oldBuf);
			DumpBuffer(szFileName, buf, "2. Front screen");
			pfnOrig_glReadBuffer(oldBuf);
		}

		// render to back screen (primary adapter)
		if (gInfo.EmulateQB && m_bFrontWindowCreated)
		{
			pfnOrig_wglSwapBuffers(m_hFrontDC);
			pfnOrig_wglMakeCurrent(m_hApplicationDC, m_hBackBufferContext);
		}
		DEBUG_TRACE2("\nBack = %s\n", GetDrawBufferFlagsString(m_nDrawBufferMode[0]));
		pfnOrig_glDrawBuffer(m_nDrawBufferMode[0]); // LEFT

		glUseProgramObjectARB(m_hPOBackScreen);
		sL = glGetUniformLocationARB(m_hPOBackScreen, "sL");
		if (sL != -1)
			glUniform1iARB(sL, 0);
		sR = glGetUniformLocationARB(m_hPOBackScreen, "sR");
		if (sR != -1)
			glUniform1iARB(sR, 1);
		CheckGLError();
		glBegin( GL_QUADS );
		glTexCoord2f(0, 0);
		glVertex2f(-1, -1);
		glTexCoord2f(0, fTextureCoordY);
		glVertex2f(-1, 1);
		glTexCoord2f(fTextureCoordX, fTextureCoordY);
		glVertex2f( 1, 1);
		glTexCoord2f(fTextureCoordX, 0);
		glVertex2f( 1, -1);
		glEnd();
		if(bDoDump)
		{
			char szFileName[MAX_PATH];
			GLenum oldBuf;
			glGetIntegerv(GL_READ_BUFFER, (GLint*)&oldBuf);
			DumpBuffer(szFileName, m_nDrawBufferMode[0], "2. Back screen");
			pfnOrig_glReadBuffer(oldBuf);
		}
	}
	else if (gInfo.OutputMode == OGL_OUTPUT_HALF_SBS ||
			 gInfo.OutputMode == OGL_OUTPUT_FULL_SBS ||
			 gInfo.OutputMode == OGL_OUTPUT_HALF_TB ||
			 gInfo.OutputMode == OGL_OUTPUT_FULL_TB ||
			 gInfo.OutputMode == OGL_OUTPUT_KATANGA ||          // Katanga publishes above; the on-screen fallback is SBS
			 gInfo.OutputMode == OGL_OUTPUT_3DVISION_GLASSES)   // NV3D-Lib publishes above; same SBS fallback
	{
		// Split-screen modes: left eye to one half of the back buffer, right
		// eye to the other. Half-vs-Full is identical compositor geometry —
		// the difference is which game resolution the user picks. At native
		// res the Half modes squash each eye to fit; at 2x-axis the user
		// gets per-eye full resolution (matches AmdQbProxy / NvDM doubled
		// back-buffer behaviour without an OGL swap-chain hook of our own).
		const bool tb = (gInfo.OutputMode == OGL_OUTPUT_HALF_TB ||
		                 gInfo.OutputMode == OGL_OUTPUT_FULL_TB);

		pfnOrig_glDrawBuffer(m_nDrawBufferMode[0]);

		// First half: left eye
		glUseProgramObjectARB(m_hPOBackScreen);
		sL = glGetUniformLocationARB(m_hPOBackScreen, "sL");
		if (sL != -1)
			glUniform1iARB(sL, 0);
		sR = glGetUniformLocationARB(m_hPOBackScreen, "sR");
		if (sR != -1)
			glUniform1iARB(sR, 1);
		CheckGLError();
		glBegin( GL_QUADS );
		if (!tb)
		{
			// SBS: left eye in left half of screen
			glTexCoord2f(0,              0             ); glVertex2f(-1, -1);
			glTexCoord2f(0,              fTextureCoordY); glVertex2f(-1,  1);
			glTexCoord2f(fTextureCoordX, fTextureCoordY); glVertex2f( 0,  1);
			glTexCoord2f(fTextureCoordX, 0             ); glVertex2f( 0, -1);
		}
		else
		{
			// T-B: left eye in top half (NDC +y up = screen top)
			glTexCoord2f(0,              0             ); glVertex2f(-1,  0);
			glTexCoord2f(0,              fTextureCoordY); glVertex2f(-1,  1);
			glTexCoord2f(fTextureCoordX, fTextureCoordY); glVertex2f( 1,  1);
			glTexCoord2f(fTextureCoordX, 0             ); glVertex2f( 1,  0);
		}
		glEnd();

		// Second half: right eye
		glUseProgramObjectARB(m_hPOFrontScreen);
		sL = glGetUniformLocationARB(m_hPOFrontScreen, "sL");
		if (sL != -1)
			glUniform1iARB(sL, 0);
		sR = glGetUniformLocationARB(m_hPOFrontScreen, "sR");
		if (sR != -1)
			glUniform1iARB(sR, 1);
		CheckGLError();
		glBegin( GL_QUADS );
		if (!tb)
		{
			// SBS: right eye in right half of screen
			glTexCoord2f(0,              0             ); glVertex2f( 0, -1);
			glTexCoord2f(0,              fTextureCoordY); glVertex2f( 0,  1);
			glTexCoord2f(fTextureCoordX, fTextureCoordY); glVertex2f( 1,  1);
			glTexCoord2f(fTextureCoordX, 0             ); glVertex2f( 1, -1);
		}
		else
		{
			// T-B: right eye in bottom half
			glTexCoord2f(0,              0             ); glVertex2f(-1, -1);
			glTexCoord2f(0,              fTextureCoordY); glVertex2f(-1,  0);
			glTexCoord2f(fTextureCoordX, fTextureCoordY); glVertex2f( 1,  0);
			glTexCoord2f(fTextureCoordX, 0             ); glVertex2f( 1, -1);
		}
		glEnd();
	}
	else
	{
		// Single-pass shader composite. Anaglyph (5), Line Interleaved (6),
		// Column Interleaved (7), Checkerboard (8). The fragment shader takes
		// both eyes as samplers and produces one merged output buffer.
		pfnOrig_glDrawBuffer(m_nDrawBufferMode[0]);

		glUseProgramObjectARB(m_hPOBackScreen);
		sL = glGetUniformLocationARB(m_hPOBackScreen, "sL");
		if (sL != -1)
			glUniform1iARB(sL, 0);
		sR = glGetUniformLocationARB(m_hPOBackScreen, "sR");
		if (sR != -1)
			glUniform1iARB(sR, 1);
		CheckGLError();
		glBegin( GL_QUADS );
		glTexCoord2f(0,              0             ); glVertex2f(-1, -1);
		glTexCoord2f(0,              fTextureCoordY); glVertex2f(-1,  1);
		glTexCoord2f(fTextureCoordX, fTextureCoordY); glVertex2f( 1,  1);
		glTexCoord2f(fTextureCoordX, 0             ); glVertex2f( 1, -1);
		glEnd();
	}

	// Release the same eye slots we bound at the top (SwapEyes-aware).
	glActiveTextureARB( GL_TEXTURE0_ARB );
	glBindTexture(GL_TEXTURE_2D, m_TextureID[0]);
	wglReleaseTexImageARB(m_hPBuffer, bStereo ? leftEye  : m_MonoBuffer);
	glActiveTextureARB( GL_TEXTURE1_ARB );
	glBindTexture(GL_TEXTURE_2D, m_TextureID[1]);
	wglReleaseTexImageARB(m_hPBuffer, bStereo ? rightEye : m_MonoBuffer);
	glActiveTextureARB( GL_TEXTURE0_ARB );

	glUseProgramObjectARB(0);
	glDisable(GL_TEXTURE_2D);
	//----
#if 0
	//if (gInfo.Debug)
	{
		glEnable(GL_TEXTURE_2D);
		glBindTexture(GL_TEXTURE_2D, m_TextureID[0]);
		wglBindTexImageARB(m_hPBuffer[0], WGL_FRONT_LEFT_ARB);
		pfnOrig_glDrawBuffer(m_nDrawBufferMode[0]);
		glBegin( GL_QUADS );
		glTexCoord2f(0, 0);
		glVertex2f(-1, 0.5);
		glTexCoord2f(0, fTextureCoordY);
		glVertex2f(-1, 1);
		glTexCoord2f(fTextureCoordX, fTextureCoordY);
		glVertex2f( -0.5, 1);
		glTexCoord2f(fTextureCoordX, 0);
		glVertex2f( -0.5, 0.5);
		glEnd();	
		wglReleaseTexImageARB(m_hPBuffer[0], WGL_FRONT_LEFT_ARB);
		glBindTexture(GL_TEXTURE_2D, m_TextureID[1]);
		wglBindTexImageARB(m_hPBuffer[1], WGL_FRONT_LEFT_ARB);
		pfnOrig_glDrawBuffer(m_nDrawBufferMode[1]);
		glBegin( GL_QUADS );
		glTexCoord2f(0, 0);
		glVertex2f(-1, 0.5);
		glTexCoord2f(0, fTextureCoordY);
		glVertex2f(-1, 1);
		glTexCoord2f(fTextureCoordX, fTextureCoordY);
		glVertex2f( -0.5, 1);
		glTexCoord2f(fTextureCoordX, 0);
		glVertex2f( -0.5, 0.5);
		glEnd();	
		wglReleaseTexImageARB(m_hPBuffer[1], WGL_FRONT_LEFT_ARB);
		glDisable(GL_TEXTURE_2D);
	}
#endif
	// Mono-HUD overlay: if the game wrote anything into the overlay FBO
	// after both per-eye 3D passes, blit it on top of the composite output
	// now. Works for every output mode — for SR weave the overlay sits at
	// screen plane (visible to both eye-angles of the lenticular weave); for
	// Half/Full SBS+T-B / Anaglyph / Line / Column / Checker it just covers
	// every back-buffer pixel uniformly so both eyes end up seeing it.
	if (gInfo.MonoHudOverlay && m_bOverlayHasContent && m_OverlayTextureID)
	{
		pfnOrig_glDrawBuffer(m_nDrawBufferMode[0]);
		pfnOrig_glViewport(0, 0, m_nWindowWidth, m_nWindowHeight);
		BlitOverlayOverBackBuffer(0.0f, 0.0f);
	}

	// Compose is finished — clear the re-entry guard before SwapBuffers
	// hands control back to the game (and any teardown/setup the game does
	// between frames goes through OnHudCandidate normally again).
	m_bInCompose = FALSE;

	BOOL bResult = pfnOrig_wglSwapBuffers(m_hApplicationDC);
	pfnOrig_wglMakeCurrent(hCurrentDC, hCurrentContext);
	m_SwapBuffersCount++;

	// Trace frame boundary so the captured log clearly shows where one
	// frame ends and the next begins, then advance/quiet down.
	if (g_traceFrameIdx < kTraceFrames)
	{
		DiagLogDrawBuffer(g_traceFrameIdx, g_traceCallIdx, GL_NONE,
			m_bSeenLeftEye, m_bSeenRightEye, m_bOverlayActive, "SwapBuffers-END");
		g_traceFrameIdx++;
		g_traceCallIdx = 0;
	}

	// Reset per-frame phase tracking so next frame's eye-detection starts
	// from a clean slate. The overlay FBO itself is reused; it's cleared on
	// the next ActivateOverlayPhase call when the game drops back to mono
	// after the two eye passes.
	m_bSeenLeftEye       = FALSE;
	m_bSeenRightEye      = FALSE;
	m_bOverlayActive     = FALSE;
	m_bOverlayHasContent = FALSE;
	// Anaglyph Steal per-frame reset. Next frame's first non-full glColorMask
	// will land on left-eye routing again.
	m_AnaglyphEyeIndexThisFrame = 0;
	m_bAnaglyphActiveFrame      = FALSE;
	// Rotate the QBS-observed window: this-frame becomes last-frame, this
	// frame's slot starts FALSE. Auto-mode gating uses both — see ColorMask.
	m_bSawQbsLastFrame  = m_bSawQbsThisFrame;
	m_bSawQbsThisFrame  = FALSE;

	return bResult;
}

#define WINDOW_CLASS_NAME (_T(SHORT_COMPANY_NAME) _T(" QBS Driver") )
BOOL	Renderer::CreateFrontWindow()
{
	if (m_hFrontWnd)
		DestroyWindow(m_hFrontWnd);
	WNDCLASS wcls;
	if (!GetClassInfo(g_hDllInstance, WINDOW_CLASS_NAME, &wcls))
	{
		memset(&wcls, 0, sizeof(wcls));
		wcls.lpfnWndProc = DefWindowProc;
		wcls.hInstance = g_hDllInstance;
		wcls.hCursor = LoadCursor(NULL, IDC_ARROW);
		wcls.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
		wcls.lpszClassName = WINDOW_CLASS_NAME;
		RegisterClass(&wcls);
	}

	WINDOWINFO wi;
	ZeroMemory(&wi, sizeof(WINDOWINFO));
	wi.cbSize = sizeof(WINDOWINFO);
	if (!GetWindowInfo(m_hWnd, &wi))
	{
		DEBUG_MESSAGE("Error: GetWindowInfo() returns FALSE\n");
		return FALSE;
	}
#if 1
	SetRectEmpty(&m_BackScreenRect);
	SetRectEmpty(&m_FrontScreenRect);
	EnumDisplayMonitors(NULL, NULL, MonitorEnumProc, (LPARAM)this);
	if (IsRectEmpty(&m_FrontScreenRect))
	{
		m_hFrontScreen = m_hBackScreen;
		SetRect(&m_FrontScreenRect, m_BackScreenRect.right, m_BackScreenRect.top, m_BackScreenRect.left + m_BackScreenRect.right, m_BackScreenRect.bottom);
	}
	MONITORINFO mi;
	mi.cbSize = sizeof(MONITORINFO);
	m_hFrontWnd = CreateWindowEx(WS_EX_NOACTIVATE|WS_EX_TOOLWINDOW|WS_EX_TOPMOST, WINDOW_CLASS_NAME, _T(MONITOR_NAME) _T(" Window"), 
		WS_POPUP | WS_VISIBLE, 
		wi.rcClient.left + m_FrontScreenRect.left, wi.rcClient.top + m_FrontScreenRect.top, 
		m_nWindowWidth, m_nWindowHeight, 0/*m_hWnd*/, 0, g_hDllInstance, 0);
	if (m_hFrontWnd == NULL)
	{
		DEBUG_MESSAGE("Error: CreateWindowA() returns NULL\n");
		return FALSE;
	}
	m_hFrontDC = GetDC(m_hFrontWnd);
	HDC hCurrentDC = pfnOrig_wglGetCurrentDC();
	int iPixelFormat = pfnOrig_wglGetPixelFormat(hCurrentDC);
	PIXELFORMATDESCRIPTOR pfd;
	pfnOrig_wglDescribePixelFormat(hCurrentDC, iPixelFormat, sizeof(PIXELFORMATDESCRIPTOR), &pfd);
	_TRY_BEGIN
		// fail on ATI when primary window on second adapter
		int iFrontPixelFormat = pfnOrig_wglChoosePixelFormat(m_hFrontDC, &pfd);
		pfnOrig_wglSetPixelFormat(m_hFrontDC, iFrontPixelFormat, &pfd);   
		m_bFrontWindowCreated = TRUE;
		DEBUG_MESSAGE("Front window created\n");
	_CATCH_ALL
		DEBUG_MESSAGE("Front window not created\n");
	_CATCH_END
#endif
	return TRUE;
}
