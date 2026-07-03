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
// Dx8wrp.cpp : Implementation of DLL Exports.

#include "stdafx.h"
#include "resource.h"
#include "Direct3DResource8.h"
#include "Direct3DBaseTexture8.h"
#include "Direct3DTexture8.h"
#include "Direct3DVolumeTexture8.h"
#include "Direct3D8.h"
#include "Direct3DDevice8.h"
#include "Direct3DSurface8.h"
#include "Direct3DSwapChain8.h"
#include "Direct3DVolume8.h"
#include <tinyxml.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <initguid.h>
#include <algorithm>
#include "CommonUtils/StringUtils.h"
#include "CommonUtils/CommonResourceFolders.h"

#define PROFILES_VERSION 1

bool ReadConfig()
{
	TCHAR szApplicationFileName[MAX_PATH];
	lstrcpy(szApplicationFileName, gInfo.ApplicationFileName);
	_tcslwr_s(szApplicationFileName, MAX_PATH);
	TCHAR path[MAX_PATH];
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
		iz3d::resources::GetAllUsersDirectory( path );
		PathAppend(path, TEXT("wiz3D_Config.xml"));
		if (!PathFileExists(path))
		{
			iz3d::resources::GetAllUsersDirectory( path );
			PathAppend(path, TEXT("Config.xml"));
		}
	}
#ifdef ZLOG_ALLOW_TRACING
	zlog::InitializeFromXML(path, _T("Config.ZLOg"));
#endif
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
		itemElement = node->FirstChildElement("Trace");
		if (itemElement)
		{	
			itemElement->QueryIntAttribute("Value", (int*)&gInfo.Trace);
#ifdef ZLOG_ALLOW_TRACING
			if (gInfo.Trace == 2 || gInfo.Trace == 3) {
				zlog::SetupSeverity(zlog::SV_FLOOD);
			}
			else if (gInfo.Trace == 1) {
				zlog::SetupSeverity(zlog::SV_NOTICE);
			}
			else {
				zlog::SetupSeverity(zlog::SV_MESSAGE);
			}
#endif
		}
	}
	node = rootNode->FirstChild("DefaultProfile");
	if (node)
	{		
		itemElement = node->FirstChildElement("RouterType");
		if (itemElement)
		{	
			itemElement->QueryIntAttribute("Value", (int*)&gInfo.RouterType);
		}
	}
	delete docConfig;

	return true;
}

struct VALUE
{
	char *szItem;
	int *pdwParam;
};

#define DECLARE_VALUE(Value) { #Value, (int *)&gInfo.Value }
#define DECLARE_FVALUE(Value) { #Value, (FLOAT *)&gInfo.Value }

void ReadProfilePart1Data( TiXmlNode* node )
{
	if (node)
	{
#ifndef FINAL_RELEASE
		TiXmlPrinter printer;
		TiXmlElement* itemElement = node->ToElement();
		//const std::string* Name = itemElement->Attribute(std::string("Name"));
		node->Accept(&printer);
		DEBUG_MESSAGE("%s", printer.CStr());
#endif
		VALUE pValItems[] = 
		{
			DECLARE_VALUE(RouterType)
		};

		for( TiXmlElement* itemElement = node->FirstChildElement(); itemElement; itemElement = itemElement->NextSiblingElement() )
		{
			int i;
			bool ValueFound = false;
			for (i = 0; i < _countof(pValItems); i++)
			{
				if ( strcmp( itemElement->Value(), pValItems[i].szItem ) == 0 )
				{
					ValueFound = true;
					break;
				}
			}
			if (ValueFound)
			{
				itemElement->QueryIntAttribute("Value", pValItems[i].pdwParam);
			}
		}
	}
}

void ReadProfileRouterType(TCHAR* szApplicationFileName, TCHAR* szProfilesFileName, TiXmlDocument* &doc, TiXmlNode* &profileNode, bool bCreate)
{
	doc = DNew TiXmlDocument( common::utils::to_multibyte(szProfilesFileName) );
	if (!doc->LoadFile())
	{
		DEBUG_MESSAGE("File %s not found or file corrupted\n", szProfilesFileName);
		if (bCreate)
		{
			TiXmlDeclaration dec;
			dec.Parse( "<?xml version='1.0' encoding='UTF-8'?>", 0, TIXML_ENCODING_UNKNOWN );
			doc->InsertEndChild(dec);
			TiXmlElement node = TiXmlElement("Profiles");
			node.SetAttribute("Version", "1");
			doc->InsertEndChild(node);
			doc->SaveFile();
		} else
		{
			delete doc;
			doc = NULL;
		}
		return;
	}

	int nVersion = 0;
	BOOL bResult = 0;
	TiXmlNode* node = 0;
	TiXmlElement* itemElement = 0;
	node = doc->FirstChild( "Profiles" );
	itemElement = node->ToElement();
	if (itemElement->QueryIntAttribute("Version", &nVersion) != TIXML_SUCCESS)
	{
		DEBUG_MESSAGE("Error reading Version = %d; in %s\n", PROFILES_VERSION, szProfilesFileName);
		delete doc;
		doc = NULL;
		return;
	}
	if (PROFILES_VERSION < nVersion)
	{
		DEBUG_MESSAGE("Unsupported Version = %d in %s, required Version = %d\n", 
			nVersion, szProfilesFileName, PROFILES_VERSION);
		delete doc;
		doc = NULL;
		return;
	}
	DEBUG_MESSAGE("Profile version: %d\n", nVersion); 
	bool FileFound = false;
	TCHAR* appFileName = PathFindFileName(szApplicationFileName);
	std::string AppCmdLine(GetCommandLineA());	
	std::transform(AppCmdLine.begin(), AppCmdLine.end(), AppCmdLine.begin(), tolower);	
	for( node = node->FirstChild("Profile"); node;	node = node->NextSibling("Profile") )
	{
		for( TiXmlElement* fileElem = node->FirstChildElement("File"); fileElem; fileElem = fileElem->NextSiblingElement("File") )
		{
			std::basic_string<TCHAR> Name = common::utils::to_unicode_simple(fileElem->Attribute("Name"));		
			std::transform(Name.begin(), Name.end(), Name.begin(), tolower);
			if (_tcsicmp(appFileName, Name.c_str()) == 0)
			{
				FileFound = true;
				profileNode = node;
			}
		}
		if (FileFound)
		{
			ReadProfilePart1Data(node);
			break;
		}
	}
	if (!FileFound)
	{
		DEBUG_MESSAGE("Profile not founded\n");
		if(!bCreate)
		{
			delete doc;
			doc = NULL;
		}
		return;
	}
}

void ReadProfilesRouterType()
{
	TCHAR szApplicationFileName[MAX_PATH];
	_tcscpy(szApplicationFileName, gInfo.ApplicationFileName);
	_tcslwr_s(szApplicationFileName, MAX_PATH);
	TCHAR szPath[MAX_PATH];
	if(iz3d::resources::GetAllUsersDirectory( szPath ))
	{
		PathAppend(szPath, TEXT("BaseProfile.xml") );
		TiXmlDocument *docSystemProfiles = NULL;
		TiXmlNode* nodeSystemProfile = NULL;
		ReadProfileRouterType(szApplicationFileName, szPath, docSystemProfiles, nodeSystemProfile, false);
		delete docSystemProfiles;
	}

	if(iz3d::resources::GetCurrentUserDirectory( szPath ))
	{
		PathAppend(szPath, TEXT("UserProfile.xml") );
		TiXmlDocument *docUserProfiles = NULL;
		TiXmlNode* nodeUserProfile = NULL;
		ReadProfileRouterType(szApplicationFileName, szPath, docUserProfiles, nodeUserProfile, true);
		delete docUserProfiles;
	}
}

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


/////////////////////////////////////////////////////////////////////////////
// DLL Entry Point

DEFINE_GUID(LIBID_DX8LIB,0x1a2622ef, 0x6cb9, 0x4ae2, 0xb4, 0xc1, 0x26, 0xa5, 0x6c, 0xa, 0xd0, 0x9c);

class CDX8Module : public CAtlDllModuleT< CDX8Module >
{
public :
	DECLARE_LIBID(LIBID_DX8LIB)
};

CDX8Module _AtlModule;

extern "C"
BOOL WINAPI DllMain(HINSTANCE hInstance, DWORD dwReason, LPVOID lpReserved)
{
	switch (dwReason)
	{
	case DLL_PROCESS_ATTACH:
		{
			DisableThreadLibraryCalls(hInstance);
			_CrtSetDbgFlag( _CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF );
			_CrtSetReportHookW2( _CRT_RPTHOOK_INSTALL, zlog::VldReportHook );
			//_CrtSetBreakAlloc(82291);

			//--- configure directories names ---
			//--- ApplicationFileName ---
			GetModuleFileName(NULL, gInfo.ApplicationFileName, sizeof(gInfo.ApplicationFileName));	
			TCHAR* s = _tcsrchr(gInfo.ApplicationFileName, '\\');
			if(s) 
				_tcscpy(gInfo.ApplicationName, s+1);

			//--- DriverDirectory ---
			GetModuleFileName((HMODULE)hInstance, gInfo.DriverDirectory, sizeof(gInfo.DriverDirectory));
			s = _tcsrchr(gInfo.DriverDirectory, _T('\\'));
			if(s) 
				*s = 0;
			ReadConfig();
			ReadProfilesRouterType();
		#ifdef ZLOG_ALLOW_TRACING	
			InitializeLogging();
		#endif
			break;
		}
	case DLL_PROCESS_DETACH:
		{
			_RPT0(0, "S3DWrapperD3D8 memory leaks:\n");
			break;
		}
	}

	return _AtlModule.DllMain(dwReason, lpReserved);
}

// Diagnostic logging — defined in Direct3D8.cpp; declare here for use from
// Direct3DCreate8 so we can trace IDirect3D9 creation and QI results.
extern "C" void DiagLogProxy(const char* fmt, ...);

IDirect3D8 * WINAPI Direct3DCreate8(UINT SDKVersion)
{
	HRESULT hResult = S_OK;
	CComPtr<IDirect3D9> pDirect3D9;
	pDirect3D9.Attach(Direct3DCreate9(D3D_SDK_VERSION));
	DiagLogProxy("Direct3DCreate8 entry: SDKVersion=%u  Direct3DCreate9 returned IDirect3D9=%p\n",
		SDKVersion, (void*)pDirect3D9.p);
	CComPtr<IWrapperTable> pWrapperTable;
	NSCALL(CWrapperTable::CreateInstance(&pWrapperTable));
	if(pWrapperTable)
	{
		CComPtr<IWrapper> pWrapper;
		NSCALL(CDirect3D8::CreateInstance(&pWrapper));
		if(pWrapper)
		{
			InitD3DInfo();
			NSCALL(pWrapper->Init(pWrapperTable, pDirect3D9));
			IDirect3D8 *pReturnedDirect3D8 = 0;
			HRESULT hrQI = pWrapper.QueryInterface(&pReturnedDirect3D8);
			DiagLogProxy("Direct3DCreate8 QI(IDirect3D8) hr=0x%lX -> IDirect3D8=%p (wrapper=%p)\n",
				hrQI, pReturnedDirect3D8, (void*)pWrapper.p);
			DEBUG_TRACE1("Direct3DCreate8(UINT SDKVersion = %d)=%p\n",
				SDKVersion, pReturnedDirect3D8);
			return pReturnedDirect3D8;
		}
	}
	DiagLogProxy("Direct3DCreate8 returning NULL (table=%p)\n", (void*)pWrapperTable.p);
	return NULL;
}
