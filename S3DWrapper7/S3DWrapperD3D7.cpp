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
#include "resource.h"
#include <tinyxml.h>
#include <shlobj.h>
#include <shlwapi.h>
#include "Direct3D7.h"
#include <initguid.h>
#include <algorithm>
#include "Trace.h"
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

DEFINE_GUID(LIBID_DX7LIB,0x172622ef, 0x64b9, 0x4a42, 0x14, 0xcd, 0x26, 0xa3, 0x4c, 0xa, 0xd5, 0x9c);

class CDX8Module : public CAtlDllModuleT< CDX8Module >
{
public :
	DECLARE_LIBID(LIBID_DX7LIB)
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
			_RPT0(0, "S3DWrapperD3D7 memory leaks:\n");
			break;
		}
	}

    return _AtlModule.DllMain(dwReason, lpReserved); 
}

HRESULT WINAPI DirectDrawCreateHooked( GUID FAR * lpGuid, LPDIRECTDRAW  *lplpDD, IUnknown FAR *pUnkOuter )
{
	HRESULT hResult = S_OK;
	hResult = DirectDrawCreate( lpGuid, lplpDD, pUnkOuter );
	if (SUCCEEDED(hResult))
	{
		CDirect3D7* pWrapper;
		NSCALL(CDirect3D7::CreateInstance<IWrapperTable>((IWrapperTable**)&pWrapper));
		if(pWrapper)
		{
			IDirectDraw *pReturnedDDraw = 0;
			hResult = pWrapper->QueryInterface(IID_IDirectDraw, (void**)&pReturnedDDraw);
			pWrapper->Init((IDirectDraw*)*lplpDD);
			DEBUG_TRACE1("DirectDrawCreate(GUID FAR * lpGuid = %p, LPVOID  *lplpDD = %p,IUnknown FAR *pUnkOuter = %p)\n", 
				lpGuid, lplpDD, pUnkOuter);
			*lplpDD = pReturnedDDraw;
			return hResult;
		}
	}
	return hResult;
}

HRESULT WINAPI DirectDrawCreateExHooked( GUID FAR * lpGuid, LPVOID  *lplpDD, REFIID  iid, IUnknown FAR *pUnkOuter )
{
	HRESULT hResult = S_OK;
	hResult = DirectDrawCreateEx( lpGuid, lplpDD, iid, pUnkOuter );
	if(!InlineIsEqualGUID(iid, IID_IDirectDraw7))
		return hResult;
	if (SUCCEEDED(hResult))
	{
		CDirect3D7* pWrapper;
		NSCALL(CDirect3D7::CreateInstance<IWrapperTable>((IWrapperTable**)&pWrapper));
		if(pWrapper)
		{
			IDirectDraw7 *pReturnedDDraw7 = 0;
			hResult = pWrapper->QueryInterface(IID_IDirectDraw7, (void**)&pReturnedDDraw7);
			pWrapper->Init((IDirectDraw7*)*lplpDD);
			DEBUG_TRACE1("DirectDrawCreateEx(GUID FAR * lpGuid = %p, LPVOID  *lplpDD = %p, REFIID  iid = %p,IUnknown FAR *pUnkOuter = %p)\n", 
				lpGuid, lplpDD, iid, pUnkOuter);
			*lplpDD = pReturnedDDraw7;
			return hResult;
		}
	}
	return hResult;
}
