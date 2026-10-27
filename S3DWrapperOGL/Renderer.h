/* 
* Project : iZ3D Stereo Driver
* Copyright (C) iZ3D Inc. 2002 - 2010
*/

#pragma once

// OpenGL Rendering Context (HGLRC) = 3D Device
// Device Context (HDC) = SwapChains

class Renderer
{
	HWND	m_hWnd;
	HDC		m_hApplicationDC;
	HGLRC	m_hApplicationContext;
	HGLRC	m_hBackBufferContext;
	UINT	m_nWindowWidth;
	UINT	m_nWindowHeight;
	UINT	m_nMonitorWidth;
	UINT	m_nMonitorHeight;
	UINT	m_nPBufferWidth;
	UINT	m_nPBufferHeight;

	HPBUFFERARB	m_hPBuffer;
	HDC		m_hPBufferDC;
	HGLRC	m_hPBufferContext;
	BOOL	m_bCreated;
	BOOL	Create();
	BOOL	CreatePB();
	void	DestroyPB();

	GLuint	m_TextureID[2];

	GLenum	m_nDrawBufferMode[2];
	BOOL	m_bStereoRender;
	BOOL	m_bSwapBuffersCalled;

	BOOL	m_bShadersInitialized;
	GLhandleARB m_hPOBackScreen;
	GLhandleARB	m_hFSBackScreen;
	GLhandleARB m_hPOFrontScreen;
	GLhandleARB m_hFSFrontScreen;
	GLhandleARB	m_hVS;
	BOOL	InitializeShaders();

	BOOL	m_bFrontWindowCreated;
	BOOL	CreateFrontWindow();
	HDC		m_hFrontDC;
	GLenum	m_MonoBuffer;
	UINT	m_SwapBuffersCount;
	HMONITOR	m_hBackScreen, m_hFrontScreen;
	RECT		m_BackScreenRect, m_FrontScreenRect;
	void	DumpBuffer( char * szFileName, GLenum buf, char* s );

	// Simulated Reality weave (OutputMode 9). void* avoids dragging SR headers into
	// every TU that includes Renderer.h — the full struct lives in
	// SRWeaveOGL.cpp. m_bSRWeaveTriedInit is sticky so Output() doesn't
	// keep retrying when the SR runtime is missing.
	void*	m_pSRWeave;
	BOOL	m_bSRWeaveTriedInit;

	// Katanga (OutputMode 12). Same opaque-pointer pattern as SR weave;
	// full type is KatangaOGLContext in KatangaOGL.cpp. m_bKatangaTriedInit
	// is sticky so Output() falls through to plain SBS once we've decided
	// Katanga isn't up / interop missing.
	void*	m_pKatanga;
	BOOL	m_bKatangaTriedInit;

	// NV3D-Lib shutter glasses (OutputMode 11). Opaque pointer to a
	// NV3DGlassesOGLContext; m_bNV3DTriedInit is sticky.
	void*	m_pNV3DGlasses;
	BOOL	m_bNV3DGlassesTriedInit;

	// Mono-HUD overlay state (gInfo.MonoHudOverlay). idTech 3 family games
	// (RtCW / Q3 / Doom 3) render the HUD via mono GL_BACK draws after the
	// two per-eye 3D passes, which makes the HUD/cinematics/menus appear in
	// only one eye. We catch the post-stereo mono phase and redirect those
	// draws into m_OverlayFBO; at SwapBuffers we alpha-blit the overlay on
	// top of whatever the per-eye composite produced, so the HUD lands in
	// both eyes at screen plane.
	GLuint	m_OverlayTextureID;
	GLuint	m_OverlayFBO;
	UINT	m_OverlayWidth;
	UINT	m_OverlayHeight;
	BOOL	m_bSeenLeftEye;
	BOOL	m_bSeenRightEye;
	BOOL	m_bOverlayActive;       // currently routing mono draws into overlay FBO
	BOOL	m_bOverlayHasContent;   // game drew something into overlay this frame
	BOOL	m_bInCompose;           // true while our own SwapBuffers compose path runs;
	                                // suppresses overlay re-trigger when our glDisable /
	                                // glOrtho calls feed back through the hooks
	void	EnsureOverlay();
	void	ActivateOverlayPhase();
	void	BlitOverlayOverBackBuffer(float fTextureCoordX, float fTextureCoordY);

	// Anaglyph Steal (gInfo.AnaglyphSteal). Per-frame eye counter — the engine's
	// anaglyph pattern always emits exactly two non-full glColorMask calls per
	// frame (one per eye), with a full-mask restore before SwapBuffers. We use
	// the count to route the first → left PBuffer, second → right PBuffer.
	// m_bAnaglyphActiveFrame tracks whether the current frame's stereo was
	// driven by anaglyph patterns (used to suppress the QBS DrawBuffer path
	// when both could fire).
	int		m_AnaglyphEyeIndexThisFrame;   // 0 = next is left, 1 = next is right, 2 = saturated
	BOOL	m_bAnaglyphActiveFrame;
	// QBS-observed flags, used by the auto-detect mode (gInfo.AnaglyphSteal=2):
	// "if we've seen any glDrawBuffer(BACK_LEFT/RIGHT) this frame or the prior
	// one, the game is using QBS — leave anaglyph capture off." Two-frame
	// window so an idle frame between game phases doesn't flip mode.
	BOOL	m_bSawQbsThisFrame;
	BOOL	m_bSawQbsLastFrame;
	// One-shot guard for the context-capability diag line (GL_STEREO /
	// GL_DOUBLEBUFFER). Logged on the first SwapBuffers from the game's own
	// context so we record whether the game even got a quad-buffer stereo
	// pixel format — true even for mono games that never trigger a PBuffer.
	BOOL	m_bLoggedContextCaps;

public:
	Renderer(void);
	~Renderer(void);
	BOOL	SetApplicationDC(HDC ApplicationDC);
	HDC		GetApplicationDC() { return m_hApplicationDC; }
	BOOL	SetApplicationContext(HGLRC	hContext) { m_hApplicationContext = hContext; }
	HGLRC	GetApplicationContext() { return m_hApplicationContext; }

	BOOL	IsDCMatched(HDC hDC);
	void	DrawBuffer(GLenum mode);
	// Called from the glColorMask hook (Modified.cpp:HglColorMask). When
	// gInfo.AnaglyphSteal is on and the mask matches an anaglyph signature
	// (one of: R / G / B / RG / RB / GB with A=true), routes the next draws
	// to the appropriate PBuffer eye and forces the real mask to all-true.
	// Returns TRUE if the call was handled (caller skips its own glColorMask),
	// FALSE for pass-through.
	BOOL	ColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a);
	BOOL	SwapBuffers();

	void	SetStereoRender(BOOL bStereoRender);
	inline BOOL	GetStereoRender() { return m_bStereoRender; }
	BOOL    MakeCurrent(HGLRC hglrc);
	void	OnHudCandidate(const char* triggerName);
	BOOL	CheckGLError();
	BOOL	CheckGLSLError(GLhandleARB Handle, GLenum Param);
	void	SaveBufferToFile(int width, int height, char* filename);
	BOOL	m_bGlInit;
	friend BOOL CALLBACK MonitorEnumProc(HMONITOR hMonitor, HDC hdcMonitor, LPRECT lprcMonitor, LPARAM dwData);
};

inline BOOL	Renderer::IsDCMatched(HDC hDC)
{
	if (hDC == m_hApplicationDC || hDC == m_hPBufferDC)
	{
		return TRUE;
	}
	return FALSE;
}

extern std::vector<Renderer>	g_RendererList;

typedef void ( APIENTRY* PFNGLGETBOOLEANV )(GLenum pname, GLboolean *params);
typedef int ( APIENTRY* PFNGDIRELEASEDC )(HWND hWnd, HDC hDC);
typedef HGLRC ( APIENTRY* PFNWGLCREATECONTEX )(HDC hdc);
typedef BOOL ( APIENTRY* PFNWGLDELETECONTEX )(HGLRC hrc);
typedef HDC ( APIENTRY* PFNWGLGETCURRENTDC )(VOID);
typedef HGLRC ( APIENTRY* PFNWGLGETCURRENTCONTEX )(VOID);
typedef BOOL ( APIENTRY* PFNWGLMAKECURRENT )(HDC hdc, HGLRC hglrc);
typedef void ( APIENTRY* PFNWGLVIEWPORT )(GLint x, GLint y, GLsizei width, GLsizei height);
typedef void ( APIENTRY* PFNWGLSCISSOR )(GLint x, GLint y, GLsizei width, GLsizei height);
typedef BOOL ( APIENTRY* PFNWGLSWAPBUFFERS )(HDC hdc);
typedef void ( APIENTRY* PFNWGLDRAWBUFFER )(GLenum mode);
typedef void ( APIENTRY* PFNWGLREADBUFFER )(GLenum mode);
typedef int ( APIENTRY* PFNWGLCHOOSEPIXELFORMAT )( __in HDC hdc, __in CONST PIXELFORMATDESCRIPTOR *ppfd);
typedef int ( APIENTRY* PFNWGLDESCRIBEPIXELFORMAT )(  __in HDC hdc, __in int iPixelFormat, __in UINT nBytes, __out_bcount_opt(nBytes) LPPIXELFORMATDESCRIPTOR ppfd);
typedef int ( APIENTRY* PFNWGLGETPIXELFORMAT )(__in HDC hdc);
typedef BOOL ( APIENTRY* PFNWGLSETPIXELFORMAT )(__in HDC hdc, __in int format, __in CONST PIXELFORMATDESCRIPTOR * ppfd);
typedef PROC ( APIENTRY* PFNWGLAPICALL )( LPCSTR );

extern PFNWGLCREATECONTEX pfnOrig_wglCreateContext;
extern PFNWGLGETCURRENTDC pfnOrig_wglGetCurrentDC;
extern PFNWGLGETCURRENTCONTEX pfnOrig_wglGetCurrentContext;
extern PFNWGLMAKECURRENT pfnOrig_wglMakeCurrent;
extern PFNWGLVIEWPORT pfnOrig_glViewport;
extern PFNWGLSCISSOR pfnOrig_glScissor;
extern PFNWGLSWAPBUFFERS pfnOrig_wglSwapBuffers;
extern PFNWGLDRAWBUFFER pfnOrig_glDrawBuffer;
extern PFNWGLREADBUFFER pfnOrig_glReadBuffer;
extern PFNWGLCHOOSEPIXELFORMAT pfnOrig_wglChoosePixelFormat;
extern PFNWGLDESCRIBEPIXELFORMAT pfnOrig_wglDescribePixelFormat;
extern PFNWGLGETPIXELFORMAT pfnOrig_wglGetPixelFormat;
extern PFNWGLSETPIXELFORMAT pfnOrig_wglSetPixelFormat;
extern PFNWGLAPICALL pfnOrig_wglGetProcAddress;

inline UINT FindPow2( UINT x )
{
	UINT val = 2;	
	while (val < x)
		val <<= 1;
	return val;
}
