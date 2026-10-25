// dllmain.cpp : Defines the entry point for the DLL application.
#include "stdafx.h"
#include <atlbase.h>
#include <initguid.h>
#include "..\CommonUtils\CommonResourceFolders.h"
#include "../OutputMethods/OutputLib/OutputData.h"
#include "ReadData.h"
#include "ScalingAgent.h"
#include "../S3DAPI/HookAPI.h"
#include "../S3DAPI/HookDInput.h"
#include "MiniDump.h"
#include <fstream>
//#include <vld.h>

/////////////////////////////////////////////////////////////////////////////
// DLL Entry Point

// {FEDF0044-40A9-4e94-96CA-6234CCAF834D}
DEFINE_GUID(LIBID_S3DAPI, 0xfedf0044, 0x40a9, 0x4e94, 0x96, 0xca, 0x62, 0x34, 0xcc, 0xaf, 0x83, 0x4d);

class CS3DAPIModule : public CAtlDllModuleT< CS3DAPIModule >
{
public :
	DECLARE_LIBID(LIBID_S3DAPI)
};

CS3DAPIModule _AtlModule;
ScalingAgentPtrT g_ScalingAgent;

void InitDriverDirectory( HINSTANCE hinstDLL )
{
	//--- configure directories names ---
	//--- ApplicationFileName ---
	GetModuleFileName(NULL, gInfo.ApplicationFileName, _countof(gInfo.ApplicationFileName));
	//--- Executable name ---
	TCHAR* s = _tcsrchr(gInfo.ApplicationFileName, '\\');
	if(s) 
		_tcscpy(gInfo.ApplicationName, s+1);
	//--- DriverDirectory ---
	GetModuleFileName((HMODULE)hinstDLL, gInfo.DriverDirectory, _countof(gInfo.DriverDirectory));
	s = _tcsrchr(gInfo.DriverDirectory, '\\');
	if(s) 
		*s = 0;
	TCHAR szPath[MAX_PATH];
	if(iz3d::resources::GetCurrentUserDirectory(szPath)) 
		CreateDirectory(szPath, NULL); // special for multiple users
	// 0.4.1: wire up zlog's log-file path regardless of FINAL_RELEASE so
	// shipped builds still write to wiz3D_Logs/<Game>/<Game>.log. DEBUG_TRACE
	// call-sites remain FINAL_RELEASE-stripped — only the plumbing stays.
	InitializeLogging();
}

typedef DWORD (CALLBACK* LPFNGETOUTPUTCAPS)();

void LoadOutputDll()
{
	TCHAR  str [MAX_PATH];
	_stprintf_s(str, L"%s\\OutputMethods\\%s.dll", gInfo.DriverDirectory, gInfo.OutputMethodDll);
	DEBUG_MESSAGE(_T("Output DLL name: %s\n"), str);
	gInfo.hOutputDLL = (HMODULE)LoadLibrary(str);

	LPFNGETOUTPUTCAPS lpfnGetOutputCaps;	// Function pointer
	if (gInfo.hOutputDLL != NULL)
	{
		lpfnGetOutputCaps = (LPFNGETOUTPUTCAPS)GetProcAddress(gInfo.hOutputDLL,
			"GetOutputCaps");
		if (lpfnGetOutputCaps)
			gInfo.OutputCaps = lpfnGetOutputCaps();
	}
#ifndef FINAL_RELEASE
	else
	{
		DWORD error = GetLastError();
		TCHAR BufErr[MAX_PATH];
		FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM, NULL, error, LANG_SYSTEM_DEFAULT, BufErr, _countof(BufErr), NULL);
		DEBUG_MESSAGE(_T("LoadLibrary failed. Error %d: %s\n"), error, BufErr);
	}
#endif

	if (gInfo.OutputCaps & odShutterMode)
	{
		if (gInfo.OutputCaps & odShutterModeSimpleOnly)
		{
			if(gInfo.ShutterMode != SHUTTER_MODE_SIMPLE)
			{
				if(gInfo.ShutterMode == SHUTTER_MODE_AUTO)
				{
					DEBUG_MESSAGE(_T("ShutterMode forced to SHUTTER_MODE_SIMPLE.\n"));
					gInfo.ShutterMode = SHUTTER_MODE_SIMPLE;
				}
				else
					gInfo.ShutterMode = SHUTTER_MODE_DISABLED;
			}
		}
		else if (gInfo.OutputCaps & odShutterModeAMDOnly)
		{
			if(gInfo.ShutterMode != SHUTTER_MODE_ATIQB)
			{
				if(gInfo.ShutterMode == SHUTTER_MODE_AUTO)
				{
					DEBUG_MESSAGE(_T("ShutterMode forced to SHUTTER_MODE_ATIQB.\n"));
					gInfo.ShutterMode = SHUTTER_MODE_ATIQB;
				}
				else
					gInfo.ShutterMode = SHUTTER_MODE_DISABLED;
			}
		}
		else if(!(gInfo.OutputCaps & odShutterModeKMAllowed))
		{
			if (gInfo.ShutterMode == SHUTTER_MODE_IZ3DKMSERVICE)
			{
				DEBUG_MESSAGE(_T("ShutterMode forced to SHUTTER_MODE_DISABLED.\n"));
				gInfo.ShutterMode = SHUTTER_MODE_DISABLED;
			}
		}
	}
	else if(gInfo.ShutterMode != SHUTTER_MODE_DISABLED)
	{
		DEBUG_MESSAGE(_T("ShutterMode forced to SHUTTER_MODE_DISABLED. Incompatible Output caps.\n"));
		gInfo.ShutterMode = SHUTTER_MODE_DISABLED;
	}

	if ((gInfo.OutputCaps & odShutterMode) && (gInfo.ShutterMode < SHUTTER_MODE_AUTO || gInfo.ShutterMode > SHUTTER_MODE_MAX_VALUE))
	{
		gInfo.OutputCaps = 0;
		if(gInfo.hOutputDLL)
		{
			FreeLibrary(gInfo.hOutputDLL);
			gInfo.hOutputDLL = NULL;
		}
		if(gInfo.ShutterMode != SHUTTER_MODE_DISABLED)
		{
			DEBUG_MESSAGE(_T("ShutterMode forced to SHUTTER_MODE_DISABLED. Incorrect Shutter mode.\n"));
			gInfo.ShutterMode = SHUTTER_MODE_DISABLED;
		}
	}
}

hook_guard_ptr g_HookGuard;

#pragma region dinput_hook

// ToDo: RESTORE_CODE_BEFORE_HOOK is removed, should be call to RestoreCode
const DWORD HookingFlags = NO_SAFE_UNHOOKING;

hook_storage	g_DInputHooks;

template<typename IDinput>
struct IDeviceFromDInput;

template<> struct IDeviceFromDInput<IDirectInput8A> { typedef IDirectInputDevice8A type; };
template<> struct IDeviceFromDInput<IDirectInput8W> { typedef IDirectInputDevice8W type; };

template<typename IDevice>
STDMETHODIMP Hooked_DirectInputDevice8_SetDataFormat(IDevice* this_, LPCDIDATAFORMAT lpdf)
{
	UNIQUE_MEMBER_FUNCTION_HOOK(this_, &IDevice::SetDataFormat, Hooked_DirectInputDevice8_SetDataFormat<IDevice>);
	
	HRESULT hResult = CALL_ORIGINAL_MEMBER(this_, lpdf);
	ZLOG_MESSAGE(zlog::SV_FLOOD, 
				 _T("DirectInputDevice8::SetDataFormat(this_ = %p, lpdf = %p) = %s"), 
				 this_, lpdf, GetResultString(hResult));
	if ( INTERFACE_HOOKED && SUCCEEDED(hResult) ) {
		ScalingAgent::Instance()->RegisterDevice(this_, lpdf);
	}
	return hResult;
}

template<typename IDinput>
STDMETHODIMP Hooked_DirectInput8_CreateDevice(IDinput* this_, REFGUID rguid, typename IDeviceFromDInput<IDinput>::type** lplpDirectInputDevice, LPUNKNOWN pUnkOuter)
{
	UNIQUE_MEMBER_FUNCTION_HOOK(this_, &IDinput::CreateDevice, Hooked_DirectInput8_CreateDevice<IDinput>);

	HRESULT hResult = CALL_ORIGINAL_MEMBER(this_, rguid, lplpDirectInputDevice, pUnkOuter);
	ZLOG_MESSAGE(zlog::SV_FLOOD, 
				 _T("DirectInput8::CreateDevice(this_ = %p, rguid = %p, lplpDirectInputDevice = %p, pUnkOuter = %p) = %s"), 
				 this_, rguid, lplpDirectInputDevice, pUnkOuter, GetResultString(hResult));
	if ( INTERFACE_HOOKED && SUCCEEDED(hResult) && rguid == GUID_SysMouse) 
	{
		typedef IDeviceFromDInput<IDinput>::type IDevice;
		
		g_DInputHooks.add_hook( MAKE_UNIQUE_MEMBER_HOOK( (IDevice*)*lplpDirectInputDevice,
														 &IDevice::SetDataFormat, 
														 Hooked_DirectInputDevice8_SetDataFormat<IDevice>, 
														 HookingFlags ) );

	}
	return hResult;
}

HRESULT WINAPI Hooked_DirectInput8Create(HINSTANCE hinst, DWORD dwVersion, REFIID riidltf, LPVOID* ppvOut, LPUNKNOWN punkOuter)
{
	STATIC_FUNCTION_HOOK(DirectInput8Create, Hooked_DirectInput8Create);

	HRESULT hResult = CALL_ORIGINAL(hinst, dwVersion, riidltf, ppvOut, punkOuter);
	ZLOG_MESSAGE(zlog::SV_FLOOD, 
				 _T("DirectInput8Create(hinst = %p, dwVersion = %d, riidltf = %p, ppvOut = %p, punkOuter = %p) = %s"), 
				 hinst, dwVersion, riidltf, ppvOut, punkOuter, GetResultString(hResult));
		if ( SUCCEEDED(hResult) ) 
	{
		if (riidltf == IID_IDirectInput8A) 
		{
			g_DInputHooks.add_hook( MAKE_UNIQUE_MEMBER_HOOK( (IDirectInput8A*)*ppvOut,
															 &IDirectInput8A::CreateDevice, 
															 Hooked_DirectInput8_CreateDevice<IDirectInput8A>, 
															 HookingFlags ) );
		}
		else if (riidltf == IID_IDirectInput8W) 
		{
			g_DInputHooks.add_hook( MAKE_UNIQUE_MEMBER_HOOK( (IDirectInput8W*)*ppvOut,
															 &IDirectInput8W::CreateDevice, 
															 Hooked_DirectInput8_CreateDevice<IDirectInput8W>, 
															 HookingFlags ) );
		}
	}
	return hResult;
}

HWND WINAPI Hooked_CreateWindowExA(DWORD		dwExStyle,
								   LPCSTR		lpClassName, 
								   LPCSTR		lpWindowName, 
								   DWORD		dwStyle, 
								   int			x, 
								   int			y, 
								   int			nWidth, 
								   int			nHeight, 
								   HWND			hWndParent, 
								   HMENU		hMenu, 
								   HINSTANCE	hInstance,
								   LPVOID		lpParam)
{
	STATIC_FUNCTION_HOOK(CreateWindowExA, Hooked_CreateWindowExA);
	HWND hWnd = CALL_ORIGINAL(dwExStyle, lpClassName, lpWindowName, dwStyle, x, y, nWidth, nHeight, hWndParent, hMenu, hInstance, lpParam);
	ZLOG_MESSAGE(zlog::SV_FLOOD, 
				 _T("CreateWindowExA(dwExStyle = %d,\n\
									 lpClassName = %S,\n\
									 lpWindowName = %S,\n\
									 dwStyle = %d,\n\
									 x = %d,\n\
									 y = %d,\n\
									 nWidth = %d,\n\
									 nHeight = %d,\n\
									 hWndParent = %p,\n\
									 hMenu = %p,\n\
									 hInstance = %p,\n\
									 lpParam = %p) = %p"),
				 dwExStyle, lpClassName, lpWindowName, dwStyle, x, y, nWidth, nHeight, hWndParent, hMenu, hInstance, lpParam, hWnd);
	ScalingAgent::Instance()->AddHWND(hWnd);
	return hWnd;
}

HWND WINAPI Hooked_CreateWindowExW(DWORD		dwExStyle,
								   LPWSTR		lpClassName, 
								   LPWSTR		lpWindowName, 
								   DWORD		dwStyle, 
								   int			x, 
								   int			y, 
								   int			nWidth, 
								   int			nHeight, 
								   HWND			hWndParent, 
								   HMENU		hMenu, 
								   HINSTANCE	hInstance,
								   LPVOID		lpParam)
{
	STATIC_FUNCTION_HOOK(CreateWindowExW, Hooked_CreateWindowExW);
	HWND hWnd = CALL_ORIGINAL(dwExStyle, lpClassName, lpWindowName, dwStyle, x, y, nWidth, nHeight, hWndParent, hMenu, hInstance, lpParam);
	ZLOG_MESSAGE(zlog::SV_FLOOD, 
				 _T("CreateWindowExA(dwExStyle = %d,\n\
									 lpClassName = %s,\n\
									 lpWindowName = %s,\n\
									 dwStyle = %d,\n\
									 x = %d,\n\
									 y = %d,\n\
									 nWidth = %d,\n\
									 nHeight = %d,\n\
									 hWndParent = %p,\n\
									 hMenu = %p,\n\
									 hInstance = %p,\n\
									 lpParam = %p) = %p"),
				 dwExStyle, lpClassName, lpWindowName, dwStyle, x, y, nWidth, nHeight, hWndParent, hMenu, hInstance, lpParam, hWnd);
	ScalingAgent::Instance()->AddHWND(hWnd);
	return hWnd;
}

BOOL WINAPI Hooked_DestroyWindow(HWND hWnd)
{
	STATIC_FUNCTION_HOOK(DestroyWindow, Hooked_DestroyWindow);
	ZLOG_MESSAGE(zlog::SV_FLOOD, _T("DestroyWindow(hWnd = %p)"), hWnd);
	ScalingAgent::Instance()->RemoveHWND(hWnd);
	return CALL_ORIGINAL(hWnd);
}

HWND WINAPI Hooked_CreateDialogParamA(HINSTANCE hInstance, LPCSTR lpTemplate, HWND hWndParent, DLGPROC lpDialogFunc, LPARAM dwInitParam)
{
	STATIC_FUNCTION_HOOK(CreateDialogParamA, Hooked_CreateDialogParamA);
	HWND hWnd = CALL_ORIGINAL(hInstance, lpTemplate, hWndParent, lpDialogFunc, dwInitParam);
	ZLOG_MESSAGE(zlog::SV_FLOOD, 
				 _T("CreateDialogParamA(hInstance = %p,\n\
									    lpTemplate = %S,\n\
									    hWndParent = %p,\n\
									    lpDialogFunc = %p,\n\
									    dwInitParam = %d) = %p"),
				 hInstance, lpTemplate, hWndParent, lpDialogFunc, dwInitParam);
	ScalingAgent::Instance()->AddHWND(hWnd);
	return hWnd;
}

HWND WINAPI Hooked_CreateDialogParamW(HINSTANCE hInstance, LPCWSTR lpTemplate, HWND hWndParent, DLGPROC lpDialogFunc, LPARAM dwInitParam)
{
	STATIC_FUNCTION_HOOK(CreateDialogParamW, Hooked_CreateDialogParamW);
	HWND hWnd = CALL_ORIGINAL(hInstance, lpTemplate, hWndParent, lpDialogFunc, dwInitParam);
	ZLOG_MESSAGE(zlog::SV_FLOOD, 
				 _T("CreateDialogParamW(hInstance = %p,\n\
									    lpTemplate = %s,\n\
									    hWndParent = %p,\n\
									    lpDialogFunc = %p,\n\
									    dwInitParam = %d) = %p"),
				 hInstance, lpTemplate, hWndParent, lpDialogFunc, dwInitParam);
	ScalingAgent::Instance()->AddHWND(hWnd);
	return hWnd;
}

HWND WINAPI Hooked_CreateDialogIndirectParamA(HINSTANCE hInstance, LPCDLGTEMPLATE lpTemplate, HWND hWndParent, DLGPROC lpDialogFunc, LPARAM dwInitParam)
{
	STATIC_FUNCTION_HOOK(CreateDialogIndirectParamA, Hooked_CreateDialogIndirectParamA);
	HWND hWnd = CALL_ORIGINAL(hInstance, lpTemplate, hWndParent, lpDialogFunc, dwInitParam);
	ZLOG_MESSAGE(zlog::SV_FLOOD, 
				 _T("CreateDialogIndirectParamA(hInstance = %p,\n\
												lpTemplate = %p,\n\
												hWndParent = %p,\n\
												lpDialogFunc = %p,\n\
												dwInitParam = %d) = %p"),
				 hInstance, lpTemplate, hWndParent, lpDialogFunc, dwInitParam);
	ScalingAgent::Instance()->AddHWND(hWnd);
	return hWnd;
}

HWND WINAPI Hooked_CreateDialogIndirectParamW(HINSTANCE hInstance, LPCDLGTEMPLATE lpTemplate, HWND hWndParent, DLGPROC lpDialogFunc, LPARAM dwInitParam)
{
	STATIC_FUNCTION_HOOK(CreateDialogIndirectParamW, Hooked_CreateDialogIndirectParamW);
	HWND hWnd = CALL_ORIGINAL(hInstance, lpTemplate, hWndParent, lpDialogFunc, dwInitParam);
	ZLOG_MESSAGE(zlog::SV_FLOOD, 
				 _T("CreateDialogIndirectParamW(hInstance = %p,\n\
												lpTemplate = %p,\n\
												hWndParent = %p,\n\
												lpDialogFunc = %p,\n\
												dwInitParam = %d) = %p"),
				 hInstance, lpTemplate, hWndParent, lpDialogFunc, dwInitParam);
	ScalingAgent::Instance()->AddHWND(hWnd);
	return hWnd;
}

#pragma endregion

BOOL APIENTRY DllMain( HMODULE hModule,
					   DWORD  ul_reason_for_call,
					   LPVOID lpReserved
					 )
{
	switch (ul_reason_for_call)
	{
	case DLL_PROCESS_ATTACH:
		DisableThreadLibraryCalls(hModule);
		_CrtSetDbgFlag( _CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF );
		_CrtSetReportHookW2( _CRT_RPTHOOK_INSTALL, zlog::VldReportHook );
		//_CrtSetBreakAlloc(9860);
		//VLDSetReportHook( VLD_RPTHOOK_INSTALL, zlog::VldReportHook );
		gInfo.CmdLine = GetCommandLine();
		InitDriverDirectory(hModule);

		// setup dinput hooks
		g_HookGuard.reset(new hook_guard);
		//g_DInputHooks.add_hook( MAKE_STATIC_HOOK(DirectInput8Create, Hooked_DirectInput8Create, HookingFlags) );
		//g_DInputHooks.add_hook( MAKE_STATIC_HOOK(CreateWindowExA, Hooked_CreateWindowExA, HookingFlags) );
		//g_DInputHooks.add_hook( MAKE_STATIC_HOOK(CreateWindowExW, Hooked_CreateWindowExW, HookingFlags) );
		//g_DInputHooks.add_hook( MAKE_STATIC_HOOK(CreateDialogParamA, Hooked_CreateDialogParamA, HookingFlags) );
		//g_DInputHooks.add_hook( MAKE_STATIC_HOOK(CreateDialogParamW, Hooked_CreateDialogParamW, HookingFlags) );
		//g_DInputHooks.add_hook( MAKE_STATIC_HOOK(CreateDialogIndirectParamA, Hooked_CreateDialogIndirectParamA, HookingFlags) );
		//g_DInputHooks.add_hook( MAKE_STATIC_HOOK(CreateDialogIndirectParamW, Hooked_CreateDialogIndirectParamW, HookingFlags) );
		//g_DInputHooks.add_hook( MAKE_STATIC_HOOK(DestroyWindow, Hooked_DestroyWindow, HookingFlags) );
		
		g_ScalingAgent = ScalingAgent::Initialize();
		ReadConfigRouterType();

		// Developer convenience: WIZ3D_FORCE_DUMP=1 force-enables shader
		// dumping (DumpVS/DumpPS) regardless of wiz3D_Config.xml. One env
		// var set once on a dev machine, works for every game, never
		// ships (CI/release envs don't set it). Not a committed Config.xml
		// change so there's no risk of accidentally shipping dump=on.
		{
			wchar_t fd[8] = {};
			DWORD n = GetEnvironmentVariableW(L"WIZ3D_FORCE_DUMP", fd, _countof(fd));
			if (n > 0 && n < _countof(fd) &&
			    (fd[0] == L'1' || fd[0] == L't' || fd[0] == L'T' ||
			     fd[0] == L'y' || fd[0] == L'Y'))
			{
				gInfo.DumpVS = true;
				gInfo.DumpPS = true;
			}
		}

		LoadOutputDll();
		if (gInfo.CollectDebugInformation)
			HookExceptions();
		ReadProfilesRouterType();
		g_LocalData.ReadLocalizationText();
		break;
	case DLL_PROCESS_DETACH:
		FreeProfiles();
		if (gInfo.CollectDebugInformation)
			UnhookExceptions();

		// release dinput hooks
		g_DInputHooks.clear();
		g_HookGuard.reset();

		g_ScalingAgent.reset();
		if (gInfo.hOutputDLL)
		{
			FreeLibrary(gInfo.hOutputDLL);
			gInfo.hOutputDLL = NULL;
		}
		_RPT0(0, "S3DAPI memory leaks:\n");
		break;
	case DLL_THREAD_ATTACH:
	case DLL_THREAD_DETACH:
		break;
	}
	return _AtlModule.DllMain(ul_reason_for_call, lpReserved);
}
