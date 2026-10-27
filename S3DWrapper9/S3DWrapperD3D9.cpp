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
#include "stdafx.h"
#include <string.h>
#include <setupapi.h>
#include <initguid.h>
#include "Globals.h"
#include <madCHook.h>
#include "Hook.h"
#include "WDirect3DCreate9.h" 
#include <IL/il.h>
#include "BaseStereoRenderer.h"
#include "ShaderHunter9.h"
#include "../S3DAPI/ReadData.h"
#include "../Shared/DpiAwareness.h"
//#include <vld.h>

BOOL (WINAPI *Original_SetDeviceGammaRamp)(HDC hDC, LPVOID lpRamp);
BOOL (WINAPI *Original_GetDeviceGammaRamp)(HDC hDC, LPVOID lpRamp);

D3DGAMMARAMP g_DeviceGammaRamp;

static CComModule g_Module;
extern bool g_bD3D9DllAlreadyLoaded;

// Init guard. Heavy init runs at first InitializeExchangeServer / Direct3DCreate9
// instead of in DllMain — see EnsureWrapperInitialized below.
static volatile LONG g_bWrapperInitialized = 0;

CRITICAL_SECTION g_DirectWrapperListLock;
static volatile LONG g_DirectWrapperListLockInit = 0;

// Direct appender to wiz3D_proxy.log — mirror of S3DWrapper10's DDILog. Opens
// the file next to the game exe on first call, keeps the FILE* alive for
// the process. ANSI format. ZLOg / DEBUG_MESSAGE writes elsewhere; this is
// the high-signal channel for crash-investigation diagnostics that need to
// show up where the tester is looking.
void D9Log(const char* fmt, ...)
{
	static FILE* fp = NULL;
	if (!fp)
	{
		WCHAR dir[MAX_PATH];
		GetModuleFileNameW(NULL, dir, MAX_PATH);
		WCHAR* pSlash = wcsrchr(dir, L'\\');
		if (pSlash) *(pSlash + 1) = L'\0';
		lstrcatW(dir, L"wiz3D_Logs");
		CreateDirectoryW(dir, NULL);
		lstrcatW(dir, L"\\wiz3D_proxy.log");
		fp = _wfopen(dir, L"a");
		if (fp) fputs("\n--- DX9 wrapper diagnostic stream ---\n", fp);
	}
	if (!fp) return;
	va_list ap;
	va_start(ap, fmt);
	vfprintf(fp, fmt, ap);
	va_end(ap);
	fflush(fp);
}

// IID → name table for the D3D9-side QI fall-through logger. Recognises the
// canonical IDirect3D9 family and the common IUnknown probe; unknown IIDs
// fall back to hex {xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx} form so the GUID
// can still be hand-grepped against d3d9.h. Mirror of FormatGUID in
// S3DWrapper10/AdapterFunctions.cpp.
void FormatD9IID(REFIID riid, char* buf, size_t bufLen)
{
	struct Known { const IID* iid; const char* name; };
	static const Known table[] = {
		{ &IID_IUnknown,           "IUnknown" },
		{ &IID_IDirect3D9,         "IDirect3D9" },
		{ &IID_IDirect3D9Ex,       "IDirect3D9Ex" },
		{ &IID_IDirect3DDevice9,   "IDirect3DDevice9" },
		{ &IID_IDirect3DDevice9Ex, "IDirect3DDevice9Ex" },
	};
	for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); ++i)
	{
		if (IsEqualGUID(riid, *table[i].iid))
		{
			_snprintf_s(buf, bufLen, _TRUNCATE, "%s", table[i].name);
			return;
		}
	}
	_snprintf_s(buf, bufLen, _TRUNCATE,
		"{%08lX-%04hX-%04hX-%02X%02X-%02X%02X%02X%02X%02X%02X}",
		(unsigned long)riid.Data1, (unsigned short)riid.Data2, (unsigned short)riid.Data3,
		riid.Data4[0], riid.Data4[1], riid.Data4[2], riid.Data4[3],
		riid.Data4[4], riid.Data4[5], riid.Data4[6], riid.Data4[7]);
}

// Heavy initialisation deferred from DllMain. Runs the first time the proxy
// calls into us (via InitializeExchangeServer or WDirect3DCreate9), AFTER the
// loader lock has been released by LoadLibraryW returning.
//
// Why this isn't in DllMain: DetectShutterMode calls Direct3DCreate9 and
// instantiates an IDirect3D9 to probe AQBS support. Under AMD's D3D9
// user-mode driver, that init path waits on a worker thread which cannot run
// while the loader lock is held by our DllMain — deadlocking the entire
// process at launch. NVIDIA's UMD happens not to take that path, masking the
// bug for years. madCHook hook installation and DevIL init are similarly
// loader-lock-sensitive.
// 0.4.1 ShaderHunter DX9: register the DX9 wrapper's hotkey handlers with
// the S3DAPI dispatcher. KeyboardHook calls these if non-null; one wrapper
// per game, so no conflict with the DX10/11 handlers. See
// docs/SHADER_HUNTER_DESIGN.md for the full flow.
static void RegisterDX9ShaderHunterHandlers()
{
	gInfo.HuntToggleHandler  = &wiz3d::dx9::HunterOnToggle;
	gInfo.HuntPrevHandler    = &wiz3d::dx9::HunterOnCyclePrev;
	gInfo.HuntNextHandler    = &wiz3d::dx9::HunterOnCycleNext;
	gInfo.HuntCaptureHandler = &wiz3d::dx9::HunterOnCapture;
	gInfo.ProfileReloadHandler = &wiz3d::dx9::RequestProfileReload;
}

void EnsureWrapperInitialized()
{
	if (InterlockedCompareExchange(&g_bWrapperInitialized, 1, 0) != 0)
		return;

	InitDirectories();
	RegisterDX9ShaderHunterHandlers();
	DetectShutterMode();
	InitializeMadCHook();

	if (!gInfo.UseMonoDeviceWrapper)
	{
		if (gInfo.OutputCaps & odResetGDIGammaRamp)
			InitGammaRamp();
		HDC dc = GetDC(0);
		GetDeviceGammaRamp(dc, &g_DeviceGammaRamp);
		ReleaseDC(0, dc);

		HookCode(SetDeviceGammaRamp, Hooked_SetDeviceGammaRamp, (PVOID*)&Original_SetDeviceGammaRamp, HOOKING_FLAG);
		HookCode(GetDeviceGammaRamp, Hooked_GetDeviceGammaRamp, (PVOID*)&Original_GetDeviceGammaRamp, HOOKING_FLAG);
	}

	//--- check for "d3d9VistaNoSP1.dll" existance ---
	{
		TCHAR PathD3D9[MAX_PATH];
		GetD3D9DllFullPath(PathD3D9);
		if (!PathFileExists(PathD3D9))
		{
			DEBUG_MESSAGE(_T("DLL d3d9VistaNoSP1.dll not founded\n"));
			gInfo.FixVistaSP1ResetBug = FALSE;
		}
	}

	g_RenderInfo.OneTimeInitialize();

#ifndef FINAL_RELEASE
	ilInit();
#endif
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD dwReason, LPVOID lpvReserved)
{
	switch (dwReason)
	{
	case DLL_PROCESS_ATTACH:
		{
			DisableThreadLibraryCalls(hinstDLL);
			// 0.5.0: process DPI awareness BEFORE the game creates its window
			// or swap chain. Prevents OS from DPI-virtualising our window on
			// scaled displays, which would collapse SR weave + any per-panel-
			// pixel output mode. See Shared/DpiAwareness.h.
			wiz3d::SetProcessDpiAware();
			_CrtSetDbgFlag( _CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF );
			_CrtSetReportHookW2( _CRT_RPTHOOK_INSTALL, zlog::VldReportHook );
			gData.hModule = hinstDLL;
			if (InterlockedCompareExchange(&g_DirectWrapperListLockInit, 1, 0) == 0)
				InitializeCriticalSection(&g_DirectWrapperListLock);
			break;
		}
	case DLL_PROCESS_DETACH:
		{
			if (g_bWrapperInitialized)
			{
				if (!gInfo.UseMonoDeviceWrapper)
				{
					UnhookCode((PVOID*)&Original_SetDeviceGammaRamp);
					UnhookCode((PVOID*)&Original_GetDeviceGammaRamp);
					//--- restore gammaRAMP ---
					HDC dc = GetDC(0);
					SetDeviceGammaRamp(dc, &g_DeviceGammaRamp);
					ReleaseDC(0, dc);
				}
				FinalizeMadCHook();
			}

			if (g_bD3D9DllAlreadyLoaded)
			{
				TCHAR name[MAX_PATH];
				if(GetD3D9DllFullPath(name))
				{
					HMODULE D3D9Handle = GetModuleHandle(name);
					if(D3D9Handle)
						FreeLibrary(D3D9Handle);
				}
			}
			_RPT0(0, "S3DWrapperD3D9 memory leaks:\n");
			break;
		}
	case DLL_THREAD_ATTACH:
	case DLL_THREAD_DETACH:
		break;
	}

	return true;
}
