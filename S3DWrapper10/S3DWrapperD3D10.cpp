// S3DWrapper10.cpp : Defines the entry point for the DLL application.
//

#include "stdafx.h"
#include <stdio.h>
#include "S3DWrapperD3D10.h"
#include <initguid.h>
#include "..\S3DAPI\Logging.h"
#include "..\S3DAPI\GlobalData.h"
#include "..\S3DAPI\ReadData.h"
#include "..\S3DAPI\ShaderProfileData.h"    // g_ProfileData (reload clears + repopulates)
#include "../S3DAPI/ShutterAPI.h"
#include "AdapterFunctions.h"               // DDILog for profile-reload traces
#include "..\Shared\DpiAwareness.h"
#include <atomic>
#include "DXGIFactoryWrapper.h"
#include "ShaderHunter.h"           // 0.5.0 hunt-mode plumbing
#include <madchook.h>
#include <IL/il.h>
#include "../OutputMethods/OutputLib/OutputData.h"
//#include <vld.h>

#define HOOKING_FLAG NO_SAFE_UNHOOKING | DONT_RENEW_OVERWRITTEN_HOOK | RESTORE_CODE_BEFORE_HOOK

static GUID g_magicTag =  { 0x5f9fd868, 0xb2a7, 0xbf42, 0xaa, 0x13, 0xe3, 0x39, 0x34, 0xe1, 0x2a, 0x20  };

__declspec(dllexport) DWORD WINAPI SetSyncRenderCallback(void *p)
{
	static void* i;
	if(!p)
		i = p;
	else 
		return (DWORD)-1;
	return (DWORD)&g_magicTag;
}

__declspec(dllexport) DWORD WINAPI StartDDAServer()
{
	return gInfo.DisableD3D10Driver;
}

__declspec(dllexport) DWORD WINAPI InitializeExchangeServer()
{
	switch(gInfo.RouterType)
	{
	case 0:
	case 1: //--- RouterType = 1 not supported now ---
		{
			DEBUG_MESSAGE(_T("Creating D3D10 wrapper interface.\n"));
			break;
		}
	default:
		DEBUG_MESSAGE(_T("Creating original D3D10 interface.\n"));
		break;
	}
	return gInfo.RouterType; 
}

// {D9556D45-72EB-4327-A0CE-79E7DD007412}
DEFINE_GUID(LIBID_DX10LIB, 0xd9556d45, 0x72eb, 0x4327, 0xa0, 0xce, 0x79, 0xe7, 0xdd, 0x0, 0x74, 0x12);

class CDX10Module : public CAtlDllModuleT< CDX10Module >
{
public :
	DECLARE_LIBID(LIBID_DX10LIB)
};

CDX10Module _AtlModule;


// 0.4.1 ShaderHunter: register the DX10/11 wrapper's handlers with the
// S3DAPI hotkey dispatcher. KeyboardHook calls these if the slot is set;
// one of them being null is harmless (hotkey becomes a silent no-op).
// Called once from InitDirectories so it runs before any hotkey processing.
static void RegisterShaderHunterHandlers()
{
	gInfo.HuntToggleHandler  = &wiz3d::ShaderHunter::OnToggleHunt;
	gInfo.HuntPrevHandler    = &wiz3d::ShaderHunter::OnCyclePrev;
	gInfo.HuntNextHandler    = &wiz3d::ShaderHunter::OnCycleNext;
	gInfo.HuntCaptureHandler = &wiz3d::ShaderHunter::OnCapture;
}

// 0.4.1 runtime profile reload. Ctrl+F5 sets a between-frames flag; the
// actual re-parse happens from SwapChain11Proxy::OnPresentBoundaryPost
// after the frame's replay is done, so no draw is reading g_ProfileData
// while the maps rebuild. The flag is a std::atomic<bool>; writes come
// from the KeyboardHook thread, reads come from the game thread's Post
// callback, so no further synchronisation is needed.
static std::atomic<bool> g_profileReloadPending{ false };

static void RequestProfileReload()
{
	g_profileReloadPending.store(true, std::memory_order_release);
	DDILog("[reload] Ctrl+F5 pressed — BaseProfile.xml reload queued for next Present boundary\n");
}

namespace wiz3d {

// Called from SwapChain11Proxy::OnPresentBoundaryPost. Returns quickly
// when no reload is queued (one relaxed load, no lock). On reload: frees
// the current profile docs, re-parses BaseProfile.xml / UserProfile.xml /
// Config.xml via ReadCurrentProfile(). Known limitation: feature registry
// is static (CreateProfileFeatures runs once), so a newly added <RightCamera>
// tag won't take effect until the game restarts. ShaderPatch primitives
// keyed on CRC do take effect immediately on the next draw.
void MaybeReloadProfileBetweenFrames()
{
	if (!g_profileReloadPending.exchange(false, std::memory_order_acq_rel))
		return;
	DDILog("[reload] re-parsing BaseProfile.xml ...\n");
	// FreeProfiles clears the TiXmlDocument objects; the CRC maps on
	// g_ProfileData get repopulated by ReadCurrentProfile. Old entries
	// linger until the matching new entry overwrites them, but <SkipStereo/>
	// on a CRC that's removed from the new XML won't naturally clear —
	// clear the maps explicitly so the new state is authoritative.
	g_ProfileData.VSCRCData.clear();
	g_ProfileData.PSCRCData.clear();
	g_ProfileData.GSCRCData.clear();
	FreeProfiles();
	ReadCurrentProfile(gData.VendorId);
	DDILog("[reload] reload done, VS=%u PS=%u GS=%u entries\n",
	       (unsigned)g_ProfileData.VSCRCData.size(),
	       (unsigned)g_ProfileData.PSCRCData.size(),
	       (unsigned)g_ProfileData.GSCRCData.size());
}

}  // namespace wiz3d

void InitDirectories( )
{
	RegisterShaderHunterHandlers();
	gInfo.ProfileReloadHandler = &RequestProfileReload;
#ifdef ZLOG_ALLOW_TRACING
	InitializeLogging();

	// 0.4.2 layout: <DriverDir>\wiz3D_Shaders\<Game>\Dump\ for mass dumps
	// (DumpVS/DumpPS/DumpMeshes). Hunter/ sibling folder is reserved for
	// user-captured single-shader reports from the ShaderHunter OSD mode.
	// Shared across every stereo-injector wrapper (DX7/8/9/10-11) so an
	// author authoring across APIs sees a single Shaders folder next to
	// the DLLs rather than per-API scattered trees. (The historical
	// %ALLUSERSPROFILE%\DX9ShaderDataCache\<Profile>.vsc/.psc runtime
	// binary cache from iZ3D is unrelated and stays where it is.)
	_tcscpy(gData.DumpDirectory, gInfo.DriverDirectory);
	PathAppend(gData.DumpDirectory, _T("wiz3D_Shaders"));
	CreateDirectory(gData.DumpDirectory, 0);
	{
		TCHAR game[MAX_PATH];
		_tcscpy(game, gInfo.ApplicationName);
		TCHAR* dot = _tcsrchr(game, _T('.'));
		if (dot) *dot = 0;
		PathAppend(gData.DumpDirectory, game);
	}
	CreateDirectory(gData.DumpDirectory, 0);
	PathAppend(gData.DumpDirectory, _T("Dump"));
	CreateDirectory(gData.DumpDirectory, 0);
	ClearDirectory(gData.DumpDirectory);

	wchar_t	CommandFlow[MAX_PATH];
#ifndef DISABLE_D3DDEVICE_WRAPPER
	swprintf_s(CommandFlow, MAX_PATH, L"%s\\CommandFlow.xml", gData.DumpDirectory);
#endif
	WriteStreamer::Get().OpenFile( CommandFlow, "iZ3DCommandFlow");
#endif
}

void HookCreateD3DDevice();

void DetectShutterMode()
{
	// Unsupported modes
	if (gData.ShutterMode == SHUTTER_MODE_MULTITHREADED	||
		gData.ShutterMode == SHUTTER_MODE_MULTIDEVICE)
		gData.ShutterMode = SHUTTER_MODE_AUTO;
	else if (gData.ShutterMode == SHUTTER_MODE_UMHOOK_D3D9EX)
		gData.ShutterMode = SHUTTER_MODE_UMHOOK_D3D10;

	if( (gData.ShutterMode == SHUTTER_MODE_AUTO || gData.ShutterMode == SHUTTER_MODE_IZ3DKMSERVICE) && (gInfo.OutputCaps & odShutterModeKMAllowed) )
	{	
		// for skipping problems with Aero
		if( TRUE == g_KMShutter.InitShutter() )
			gData.ShutterMode = SHUTTER_MODE_IZ3DKMSERVICE;
		else
		{
			if(	gData.ShutterMode != SHUTTER_MODE_AUTO )
				gData.ShutterMode = SHUTTER_MODE_DISABLED;
		}
	}

	if (gData.ShutterMode == SHUTTER_MODE_AUTO)
	{
		// TODO: Add detection when ATIQB not supported
		gData.ShutterMode = SHUTTER_MODE_ATIQB;
	}
}

BOOL WINAPI DllMain( HMODULE hModule,
					DWORD  ul_reason_for_call,
					LPVOID lpReserved
					)
{
	switch (ul_reason_for_call)
	{
	case DLL_PROCESS_ATTACH:
	{
		DisableThreadLibraryCalls(hModule);
		// 0.5.0: process DPI awareness BEFORE game window / swap chain exists.
		// See Shared/DpiAwareness.h for the rationale (SR weave + any
		// per-panel-pixel output mode needs 1:1 panel-pixel landing).
		wiz3d::SetProcessDpiAware();
		_CrtSetDbgFlag( _CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF );
		_CrtSetReportHookW2( _CRT_RPTHOOK_INSTALL, zlog::VldReportHook );
		//VLDSetReportHook( VLD_RPTHOOK_INSTALL, zlog::VldReportHook );
		//_CrtSetBreakAlloc(304370);
		gData.hModule = hModule;
		InitDirectories();
		DetectShutterMode();
		InitializeMadCHook();
		// Initialize DevIL.
		ilInit();
		break;
	}
	case DLL_THREAD_ATTACH:
	case DLL_THREAD_DETACH:
		break;
	case DLL_PROCESS_DETACH:
		FinalizeMadCHook();
		_RPT0(0, "S3DWrapperD3D10 memory leaks:\n");
		break;
	}

	return _AtlModule.DllMain(ul_reason_for_call, lpReserved);
}