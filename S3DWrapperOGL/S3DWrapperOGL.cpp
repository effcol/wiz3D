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
// S3DWrapperOGL.cpp : Defines the entry point for the DLL application.
//

#include "stdafx.h"
#include "S3DWrapperOGL.h"
#include <tinyxml.h>
#include "trace.h"
#include <MinHook.h>
#include <tchar.h>
#include <shlobj.h>
#include <shlwapi.h>
#include "Trace.h"
#include "ProductNames.h"
#include "..\Shared\DpiAwareness.h"
#include "../CommonUtils/StringUtils.h"
#include "../CommonUtils/CommonResourceFolders.h"

#ifdef _MANAGED
#pragma managed(push, off)
#endif

void InitializeLogging()
{
#ifdef ZLOG_ALLOW_TRACING
	_tcscpy(gInfo.LogFileDirectory, gInfo.DriverDirectory);
	PathAppend(gInfo.LogFileDirectory, _T("Logs"));
	CreateDirectory(gInfo.LogFileDirectory, 0);
	PathAppend(gInfo.LogFileDirectory, gInfo.ApplicationName);
	TCHAR* c = _tcsrchr(gInfo.LogFileDirectory, '.');
	if (c != NULL)
		c[0] = '\0';
	CreateDirectory(gInfo.LogFileDirectory, 0);

	//--- LogFileName ---
	_tcscpy(gInfo.LogFileName, gInfo.LogFileDirectory);
	PathAppend(gInfo.LogFileName, gInfo.ApplicationName);
	_tcscat(gInfo.LogFileName, _T(".log"));

	//--- Start ZLOg ---
	if ( !zlog::IsLoggerOpened() ) {
		zlog::ReopenLogger(gInfo.LogFileName, true);
	}
#endif
}

extern BOOL WINAPI HookOGL();
bool ReadConfig();

BOOL APIENTRY DllMain( HMODULE hModule,
					   DWORD  ul_reason_for_call,
					   LPVOID lpReserved
					 )
{
	switch (ul_reason_for_call)
	{
	case DLL_PROCESS_ATTACH:
		DisableThreadLibraryCalls(hModule);
		// 0.5.0: process DPI awareness before the game creates its GL window.
		wiz3d::SetProcessDpiAware();
		_CrtSetDbgFlag( _CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF );
		_CrtSetReportHookW2( _CRT_RPTHOOK_INSTALL, zlog::VldReportHook );
		//_CrtSetBreakAlloc(82291);
		gInfo.Initialize(hModule);
		ReadConfig();
		MH_Initialize();
#ifdef ZLOG_ALLOW_TRACING	
		InitializeLogging();
#endif
		break;
	case DLL_THREAD_ATTACH:
	case DLL_THREAD_DETACH:
		break;
	case DLL_PROCESS_DETACH:
		MH_Uninitialize();
		_RPT0(0, "S3DWrapperOGL memory leaks:\n");
		break;
	}
	return TRUE;
}

#ifdef _MANAGED
#pragma managed(pop)
#endif

#include "..\Shared\OutputModeNames.h"

HMODULE g_hDllInstance = 0;
GlobalInfo gInfo;

bool ReadConfig()
{
	TCHAR szApplicationFileName[MAX_PATH];
	_tcscpy_s(szApplicationFileName, _countof(szApplicationFileName), gInfo.ApplicationFileName);
	_tcslwr_s(szApplicationFileName, MAX_PATH);
	TCHAR path[MAX_PATH];
	// Portable install: wiz3D_Config.xml sits next to the wrapper DLL in the
	// game folder. Check there first so OGL works without the BaseProfile.xml
	// sentinel the legacy iZ3D path resolver requires.
	_tcscpy_s(path, MAX_PATH, gInfo.DriverDirectory);
	PathAppend(path, TEXT("wiz3D_Config.xml"));
	if (!PathFileExists(path))
	{
#ifndef FINAL_RELEASE
		// Dev check: parent dir, prefer wiz3D_Config.xml, fall back to Config.xml
		_tcscpy_s(path, MAX_PATH, gInfo.DriverDirectory);
		_tcscat(path, _T("\\.."));
		PathAppend(path, TEXT("wiz3D_Config.xml"));
		if (!PathFileExists(path))
		{
			_tcscpy_s(path, MAX_PATH, gInfo.DriverDirectory);
			_tcscat(path, _T("\\.."));
			PathAppend(path, TEXT("Config.xml"));
		}
		if (!PathFileExists(path))
#endif
		{
			iz3d::resources::GetAllUsersDirectory(path);
			PathAppend(path, TEXT("wiz3D_Config.xml"));
			if (!PathFileExists(path))
			{
				iz3d::resources::GetAllUsersDirectory(path);
				PathAppend(path, TEXT("Config.xml"));
			}
		}
	}
	TiXmlDocument* docConfig = DNew TiXmlDocument( common::utils::to_multibyte(path) );
	bool res = docConfig->LoadFile();
	if (!res)
	{
		DEBUG_MESSAGE("File %s not found or corrupted\n", path);
		delete docConfig;
		docConfig = NULL;
		return false;
	}

	int nVersion = 0;
	BOOL bResult = 0;
	TiXmlElement* itemElement = 0;
	TiXmlNode* rootNode = docConfig->FirstChild( "Config" );
	itemElement = rootNode->ToElement();
	if (itemElement->QueryIntAttribute("Version", &nVersion) != TIXML_SUCCESS)
	{
		DEBUG_MESSAGE("Error reading Version = %d; in Config.xml\n", PROFILES_VERSION);
		delete docConfig;
		docConfig = NULL;
		return false;
	}
	if (PROFILES_VERSION < nVersion)
	{
		DEBUG_MESSAGE("Unsupported Version = %d in Config.xml, required Version = %d\n", 
			nVersion, PROFILES_VERSION);
		delete docConfig;
		docConfig = NULL;
		return false;
	}
	DEBUG_MESSAGE("Config version: %d\n", nVersion); 
	TiXmlNode* node = rootNode->FirstChild("GlobalSettings");
	if (node)
	{		
		itemElement = node->FirstChildElement("RouterType");
		if (itemElement)
		{	
			itemElement->QueryIntAttribute("Value", (int*)&gInfo.RouterType);
		}
		itemElement = node->FirstChildElement("Trace");
		if (itemElement)
		{	
			itemElement->QueryIntAttribute("Value", (int*)&gInfo.Trace);
		}
		itemElement = node->FirstChildElement("EmulateQB");
		if (itemElement)
		{	
			itemElement->QueryIntAttribute("Value", (int*)&gInfo.EmulateQB);
		}
		itemElement = node->FirstChildElement("OutputMode");
		if (itemElement)
		{
			itemElement->QueryIntAttribute("Value", (int*)&gInfo.OutputMode);
		}
		// OutputMethodDll string override — unified naming across all wiz3D
		// wrappers (see Shared/OutputModeNames.h). When the string matches a
		// canonical shortname or iZ3D-era legacy DLL name, it overrides the
		// numeric OutputMode. Unknown strings pass through unchanged.
		itemElement = node->FirstChildElement("OutputMethodDll");
		if (itemElement)
		{
			const char* v = itemElement->Attribute("Value");
			if (v && *v)
			{
				int resolved = wiz3d::ResolveOutputModeName(v);
				if (resolved != wiz3d::OUTMODE_UNKNOWN) gInfo.OutputMode = (UINT)resolved;
			}
		}
		// MonoHudOverlay defaults to OFF when the config doesn't list it.
		// The activation path (OnHudCandidate -> ActivateOverlayPhase) is
		// currently a no-op pending a reliable per-engine detection signal
		// for the post-stereo HUD phase. Hook/trace plumbing remains in
		// place; see project_sr_weave_ogl_wip memory for the reason.
		gInfo.MonoHudOverlay = 0;
		itemElement = node->FirstChildElement("MonoHudOverlay");
		if (itemElement)
		{
			itemElement->QueryIntAttribute("Value", (int*)&gInfo.MonoHudOverlay);
		}
		// AnaglyphSteal defaults OFF. When ON the renderer hooks glColorMask
		// and treats anaglyph-shaped masks (R-only / cyan / etc.) as stereo
		// eye boundaries — see Renderer::ColorMask. Lets games that only
		// support anaglyph drive our SBS/T-B/SR composer.
		gInfo.AnaglyphSteal = 0;
		itemElement = node->FirstChildElement("AnaglyphSteal");
		if (itemElement)
		{
			itemElement->QueryIntAttribute("Value", (int*)&gInfo.AnaglyphSteal);
		}
	}
	// SwapEyes lives under DefaultProfile in the iZ3D-shape config; for the
	// OGL wrapper we surface it as a global on/off flag. Look under either
	// node so users can put it wherever feels natural.
	{
		TiXmlNode* defaultProfile = rootNode->FirstChild("DefaultProfile");
		TiXmlElement* swapEl = NULL;
		if (defaultProfile)
			swapEl = defaultProfile->FirstChildElement("SwapEyes");
		if (!swapEl && node)
			swapEl = node->FirstChildElement("SwapEyes");
		if (swapEl)
			swapEl->QueryIntAttribute("Value", (int*)&gInfo.SwapEyes);
	}
	//node = rootNode->FirstChild("DefaultProfile");
	//if (node)
	//{		
	//}

	if (GINFO_DEBUG)
	{
		//--- Create directories for log file   ----
		TCHAR  str [MAX_PATH];
		_stprintf_s(str, _countof(str), _T("%s\\Logs"), gInfo.DriverDirectory);
		CreateDirectory(str, 0);
		if(GetFileAttributes(gInfo.LogFileDirectory) == INVALID_FILE_ATTRIBUTES)
			CreateDirectory(gInfo.LogFileDirectory, 0);
		DeleteFile(gInfo.LogFileName);
	}
	delete docConfig;

	return true;
}

void GlobalInfo::Initialize(HMODULE hModule)
{
	//--- configure directories names ---
	//--- ApplicationFileName ---
	GetModuleFileName(NULL, gInfo.ApplicationFileName, sizeof(gInfo.ApplicationFileName));
	//--- Executable name ---
	TCHAR* s = _tcsrchr(gInfo.ApplicationFileName, '\\');
	if(s) 
		_tcscpy_s(gInfo.ApplicationName, _countof(gInfo.ApplicationName), s+1);
	//--- DriverDirectory ---
	GetModuleFileName(hModule, gInfo.DriverDirectory, sizeof(gInfo.DriverDirectory));
	s = _tcsrchr(gInfo.DriverDirectory, '\\');
	if(s) 
		*s = 0;
}

static GUID g_magicTag =  { 0x5f9fd868, 0xb2a7, 0xbf42, 0xaa, 0x13, 0xe3, 0x39, 0x34, 0xe1, 0x2a, 0x20  };

__declspec(dllexport) DWORD WINAPI SetSyncRenderCallback(void *p)
{
	static void* i;
	if(!p)
		i = p;
	else 
		return -1;
	return (DWORD)&g_magicTag;
}
