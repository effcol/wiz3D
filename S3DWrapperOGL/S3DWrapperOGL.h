/* 
* Project : iZ3D Stereo Driver
* Copyright (C) iZ3D Inc. 2002 - 2010
*/

#pragma once

#include <glh/glh_extensions.h>
#include <GL/glext.h>

#ifdef S3DWRAPPEROGL_EXPORTS
#define S3DWRAPPEROGL_API __declspec(dllexport)
#else
#define S3DWRAPPEROGL_API __declspec(dllimport)
#endif

// OutputMode values for OGL wrapper. Single-digit so config files can't be
// mis-parsed (an earlier 2-digit mode 10 was misread as 1 by hand-edits).
// 0 = iZ3D dual-panel hardware (legacy)
// 1 = Half Side-by-Side  (each eye squashed to half width — works at the game's native res)
// 2 = Full Side-by-Side  (each eye at full width — game must render at half display width)
// 3 = Half Top-and-Bottom (each eye squashed to half height — works at native res)
// 4 = Full Top-and-Bottom (each eye at full height — game must render at half display height)
// 5 = Anaglyph Red/Cyan (Dubois colour-optimized — single shipped anaglyph; left=red, right=cyan)
// 6 = Row Interleaved (passive 3D TVs / row-polarised monitors)
// 7 = Column Interleaved (rare column-polarised displays)
// 8 = Checkerboard (DLP-Link 3D projectors)
// 9 = Simulated Reality Weave (default — Samsung Odyssey 3D / Acer SpatialLabs lightfield
//                              displays; falls back to plain Half-SBS on systems
//                              without the SR runtime)
// Eye swapping is controlled separately by the SwapEyes config flag, which
// applies across every OutputMode rather than living as its own mode.
#define OGL_OUTPUT_IZ3D                0
#define OGL_OUTPUT_HALF_SBS            1
#define OGL_OUTPUT_FULL_SBS            2
#define OGL_OUTPUT_HALF_TB             3
#define OGL_OUTPUT_FULL_TB             4
#define OGL_OUTPUT_ANAGLYPH            5
#define OGL_OUTPUT_ROW_INTERLEAVED     6
#define OGL_OUTPUT_COLUMN_INTERLEAVED  7
#define OGL_OUTPUT_CHECKERBOARD        8
#define OGL_OUTPUT_SR_WEAVE            9
#define OGL_OUTPUT_3DVISION_GLASSES    11  // NV3D-Lib DIRECT-mode shutter glasses; see NV3DGlassesOGL.{h,cpp}
#define OGL_OUTPUT_KATANGA             12  // bo3b's shared-texture viewer; see KatangaOGL.{h,cpp}

struct GlobalInfo
{
	UINT		Trace;
	UINT		EmulateQB;
	UINT		RouterType;
	UINT		OutputMode;
	UINT		SwapEyes;
	UINT		MonoHudOverlay;
	// Anaglyph Steal mode: when on, hook glColorMask and detect the engine's
	// anaglyph stereo pattern (e.g. ioquake3 r_anaglyphMode). The first
	// anaglyph-shaped mask of a frame routes subsequent draws into the
	// left-eye PBuffer, the second routes into the right-eye PBuffer, and we
	// force the real glColorMask to all-true so neither eye gets channel-
	// filtered. Lets games with anaglyph but no QBS (or buggy QBS — see the
	// ioquake3 r_zProj one-eye-black bug) drive our full SBS/T-B/SR/etc.
	// compositor. 0 = off (default). 1 = on.
	UINT		AnaglyphSteal;
	TCHAR		DriverDirectory[MAX_PATH];
	TCHAR		DriverFileName[MAX_PATH];
	TCHAR       ApplicationFileName[MAX_PATH];
	TCHAR		ApplicationName[MAX_PATH];
	TCHAR       LogFileName[MAX_PATH];
	TCHAR       LogFileDirectory[MAX_PATH];
	GlobalInfo()
	{
		ZeroMemory(this, sizeof(GlobalInfo));
	}
	void Initialize(HMODULE hModule);
};

extern HMODULE g_hDllInstance;
extern GlobalInfo gInfo;

#define PROFILES_VERSION	1
#define GINFO_ENABLED		(gInfo.RouterType != 2)
