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
#include "ReadData.h"
#include <Shlobj.h>
#include <Shlwapi.h>
#include "ShaderProfileData.h"
#include "../../CommonUtils/StringUtils.h"
#include "../../CommonUtils/CommonResourceFolders.h"

char* LOCALIZATION_TEXT_LIST[] =
{
	"CountingFPS",
	"Mono",
	"Stereo",
	"drop",
	"Preset",
	"Convergence",
	"ConvergenceINF",
	"Separation",
	"AutoFocus",
	"ON",
	"OFF",
	"tooMuchSeparation",
	"SwapLR",
	"DaysLeft",
	"CantFindStereoDevice",
	"CantLoadOutputDLL",
	"UpdateAMDDriver",
	"NotSupported3DDevice",
};

C_ASSERT(_countof(LOCALIZATION_TEXT_LIST) == LT_TotalStrings);

TiXmlDocument *g_docSystemProfiles = NULL;
TiXmlNode* g_nodeSystemProfile = NULL;
TiXmlDocument *g_docCommunityProfiles = NULL;
TiXmlNode* g_nodeCommunityProfile = NULL;
TiXmlDocument *g_docUserProfiles = NULL;
TiXmlNode* g_nodeUserProfile = NULL;
TiXmlDocument *g_docConfig = NULL;
TiXmlNode* g_outputConfig = NULL;
int g_PreTransformProjectionId = 0;

// QueryBoolAsInt is a helper function that reads an integer attribute from an 
// XML element and converts it to a boolean value. This was previously using
// QueryBoolAttribute, but that function is not available in the ticpp version
// we are using, so this is a helper to avoid needing the a different version
// of TinyXML. 
static int QueryBoolAsInt(TiXmlElement* elem, const char* name, bool* bval)
{
	int ival;
	int result = elem->QueryIntAttribute(name, &ival);
	if (result == TIXML_SUCCESS)
		*bval = (ival != 0);
	return result;
}

enum ValueType
{
	vtInt,
	vtBool,
	vtFloat,
	vtString
};

struct XmlValue
{
	char*		pName;
	void*		pVal;
	ValueType	Type;

	void ReadData(TiXmlElement* pElem, char* pAttribute = "Value");
};

void XmlValue::ReadData(TiXmlElement* pElem, char* pAttribute)
{
	const CHAR* VendorC = pElem->Attribute("Vendor");
	std::wstring Vendor;
	if (VendorC)
		Vendor = common::utils::to_unicode_simple(pElem->Attribute("Vendor"));
	if (Vendor.empty() || _tcscmp( Vendor.c_str(), gInfo.Vendor ) == 0 )
	{
		switch(Type)
		{
		case vtInt:
			pElem->QueryIntAttribute("Value", (int*)pVal);
			break;
		case vtBool:
			QueryBoolAsInt(pElem, "Value", (bool*)pVal);
			break;
		case vtFloat:
			pElem->QueryFloatAttribute("Value", (float*)pVal);
			break;
		case vtString:
			_tcscpy((TCHAR*)pVal,	
				common::utils::to_unicode_simple(pElem->Attribute(pAttribute)).c_str());
			break;
		}
	}
}

#define DECLARE_IVALUE(Value) { #Value, (void *)&gInfo.Value, vtInt }
#define DECLARE_BVALUE(Value) { #Value, (void *)&gInfo.Value, vtBool }
#define DECLARE_FVALUE(Value) { #Value, (void *)&gInfo.Value, vtFloat }
#define DECLARE_SVALUE(Value) { #Value, (void *)&gInfo.Value, vtString }

void ReadProfilePart1Data( TiXmlNode* node, bool print = true )
{
	if (node)
	{
#ifndef FINAL_RELEASE
		if (print)
		{
			TiXmlPrinter printer;
			TiXmlElement* itemElement = node->ToElement();
			//const std::string* Name = itemElement->Attribute(std::string("Name"));
			node->Accept(&printer);
			DEBUG_MESSAGE(_T("%s"), common::utils::to_unicode_simple(printer.CStr()).c_str() );
		}
#endif
		XmlValue pValItems[] = 
		{
			DECLARE_IVALUE(RouterType),
			DECLARE_BVALUE(EnableStereo),
			DECLARE_BVALUE(DisableD3D10Driver),
			DECLARE_BVALUE(AlternativeSetFullscreenStateOrder),	
		};

		for( TiXmlElement* itemElement = node->FirstChildElement(); itemElement; itemElement = itemElement->NextSiblingElement() )
		{
			int i;
			bool ValueFound = false;
			for (i = 0; i < _countof(pValItems); i++)
			{
				if ( strcmp( itemElement->Value(), pValItems[i].pName ) == 0 )
				{
					ValueFound = true;
					break;
				}
			}
			if (ValueFound)
			{
				pValItems[i].ReadData(itemElement);
			}
		}
	}
}


bool ReadConfigRouterType()
{
	TCHAR szApplicationFileName[MAX_PATH];
	_tcscpy_s<MAX_PATH>(szApplicationFileName, gInfo.ApplicationFileName);
	_tcslwr_s<MAX_PATH>(szApplicationFileName);
	TCHAR szConfigFileName[MAX_PATH];
	GetConfigFullPath(szConfigFileName);
	g_docConfig = DNew TiXmlDocument( common::utils::to_multibyte(szConfigFileName) );
	bool res = g_docConfig->LoadFile();
	if (!res)
	{
		DEBUG_MESSAGE(_T("File %s not found or corrupted\n"), szConfigFileName);
#ifdef FINAL_RELEASE
		// Last-ditch retry next to the driver DLLs: prefer wiz3D_Config.xml, then Config.xml.
		delete g_docConfig;
		_tprintf_s(szConfigFileName, MAX_PATH, "%s\\wiz3D_Config.xml", gInfo.DriverDirectory);
		g_docConfig = DNew TiXmlDocument( common::utils::to_multibyte(szConfigFileName));
		res = g_docConfig->LoadFile();
		if (!res)
		{
			delete g_docConfig;
			_tprintf_s(szConfigFileName, MAX_PATH, "%s\\Config.xml", gInfo.DriverDirectory);
			g_docConfig = DNew TiXmlDocument( common::utils::to_multibyte(szConfigFileName));
			res = g_docConfig->LoadFile();
		}
#endif
		if(!res)
		{
			DEBUG_MESSAGE(_T("File %s not found or corrupted\n"), szConfigFileName);
			delete g_docConfig;
			g_docConfig = NULL;
			gInfo.RouterType = ROUTER_TYPE_DISABLE_DRIVER; // disable driver if we can't read config
			return false;
		}
	}

	int nVersion = 0;
	TiXmlNode* rootNode = g_docConfig->FirstChild( "Config" );
	TiXmlElement* itemElement = rootNode->ToElement();
	if (itemElement->QueryIntAttribute("Version", &nVersion) != TIXML_SUCCESS)
	{
		DEBUG_MESSAGE(_T("Error reading Version = %d; in Config.xml\n"), PROFILES_VERSION);
		delete g_docConfig;
		g_docConfig = NULL;
		return false;
	}
	if (PROFILES_VERSION < nVersion)
	{
		DEBUG_MESSAGE(_T("Unsupported Version = %d in Config.xml, required Version = %d\n"), 
			nVersion, PROFILES_VERSION);
		delete g_docConfig;
		g_docConfig = NULL;
		return false;
	}
	DEBUG_MESSAGE(_T("Config version: %d\n"), nVersion); 

	// GlobalSettings
	TiXmlNode* node = rootNode->FirstChild("GlobalSettings");
	if (node)
	{
		static XmlValue pValItems[] = 
		{
#ifndef FINAL_RELEASE
			DECLARE_BVALUE(ShowDebugInfo),
			DECLARE_BVALUE(DumpOn),
			DECLARE_BVALUE(DumpInitialData),
			DECLARE_BVALUE(DumpVS),
			DECLARE_BVALUE(DumpPS),
			DECLARE_BVALUE(DumpMeshes),
			DECLARE_BVALUE(DumpStereoImages),
			DECLARE_BVALUE(UseR32FDepthTextures),
			DECLARE_BVALUE(LockableDepthStencil),
			DECLARE_BVALUE(ClearBeforeEveryDrawWhenDoDump),
			DECLARE_BVALUE(ReplaceDSSurfacesByTextures),
			DECLARE_BVALUE(TwoPassRendering),
#endif
			DECLARE_SVALUE(OutputMethodDll),
			DECLARE_IVALUE(OutputMode),
			DECLARE_IVALUE(OutputSpanMode),
			DECLARE_IVALUE(DisplayScalingMode),
			DECLARE_IVALUE(DisplayNativeWidth),
			DECLARE_IVALUE(DisplayNativeHeight),
			DECLARE_IVALUE(ShutterMode),
			DECLARE_IVALUE(ShutterRefreshRate),
			DECLARE_BVALUE(ExtractDepthBuffer),
			DECLARE_BVALUE(DrawMonoImageOnSecondMonitor),
			DECLARE_BVALUE(UseMonoDeviceWrapper),
			DECLARE_BVALUE(UseCOMWrap),
			DECLARE_BVALUE(UseCOMWrapSwapChain),
			DECLARE_BVALUE(UseCOMWrapReplay),
			DECLARE_IVALUE(VerboseFrameTrace),
			DECLARE_IVALUE(FrameTraceStartFrame),
			DECLARE_FVALUE(COMWrapEyeShift),
			DECLARE_BVALUE(DisableBlindCBScan),
			DECLARE_BVALUE(FullColumnEyeShift),
			DECLARE_BVALUE(ModifyShadersDX11),
			DECLARE_IVALUE(ModifyShadersMaxCount),
			DECLARE_IVALUE(FrameTraceMinDraws),
			DECLARE_BVALUE(ReplayDynamicBuffers),
			DECLARE_BVALUE(DuplicateDraws),
			DECLARE_BVALUE(CollectDebugInformation),
			DECLARE_IVALUE(ScreenshotType),
			DECLARE_BVALUE(FixVistaSP1ResetBug),
			DECLARE_BVALUE(DisableGammaApplying),
			DECLARE_SVALUE(Language),
		};

		for( itemElement = node->FirstChildElement(); itemElement; itemElement = itemElement->NextSiblingElement() )
		{
			int i;
			bool ValueFound = false;
			for (i = 0; i < _countof(pValItems); i++)
			{
				if ( strcmp( itemElement->Value(), pValItems[i].pName ) == 0 )
				{
					ValueFound = true;
					break;
				}
			}
			if (ValueFound)
			{
				pValItems[i].ReadData(itemElement);
			}
		}
	}

	// DefaultProfile
	node = rootNode->FirstChild("DefaultProfile");
	ReadProfilePart1Data(node, false);

	g_outputConfig = rootNode->FirstChild("Outputs");
	if (g_outputConfig)
	{
		std::string outputDll = common::utils::to_multibyte(gInfo.OutputMethodDll);
		g_outputConfig = g_outputConfig->FirstChild(outputDll.c_str());		
	}

	return true;
}

// profileSource: 0=Base, 1=Community, 2=User. Used to set the right
// gInfo.bMatchedIn* flag when a real <File> match is found, so per-wrapper
// startup diagnostics can show which XML the active profile came from
// (and so authors of community / user profile fixes can confirm their
// XML is being picked up).
void ReadProfileRouterType(TCHAR* szApplicationFileName, TCHAR* szProfilesFileName, TiXmlDocument* &doc, TiXmlNode* &profileNode, bool bCreate, int profileSource = 0)
{
	std::string CharProfilesFileName = common::utils::to_multibyte(szProfilesFileName);
	doc = DNew TiXmlDocument( CharProfilesFileName.c_str() );
	if (!doc->LoadFile())
	{
		DEBUG_MESSAGE(_T("File %s not found or file corrupted\n"), szProfilesFileName);
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
	if (!node)
	{
		// Malformed XML (no <Profiles> root) — bail out cleanly instead of NULL-deref.
		// Seen with CommunityProfile.xml whose outer comment was prematurely closed
		// by a nested <!-- ... --> block, which TinyXML parsed as broken structure.
		DEBUG_MESSAGE(_T("No <Profiles> root in %s\n"), szProfilesFileName);
		delete doc;
		doc = NULL;
		return;
	}
	itemElement = node->ToElement();
	if (itemElement->QueryIntAttribute("Version", &nVersion) != TIXML_SUCCESS)
	{
		DEBUG_MESSAGE(_T("Error reading Version = %d; in %s\n"), PROFILES_VERSION, szProfilesFileName);
		delete doc;
		doc = NULL;
		return;
	}
	if (PROFILES_VERSION < nVersion)
	{
		DEBUG_MESSAGE(_T("Unsupported Version = %d in %s, required Version = %d\n"), 
			nVersion, szProfilesFileName, PROFILES_VERSION);
		delete doc;
		doc = NULL;
		return;
	}
	DEBUG_MESSAGE(_T("Profile version: %d\n"), nVersion); 
	bool FileFound = false;
	bool bChangeProfileName;
	TCHAR* appFileName = PathFindFileName(szApplicationFileName);
	std::basic_string<TCHAR> AppCmdLine = gInfo.CmdLine;	
	std::transform(AppCmdLine.begin(), AppCmdLine.end(), AppCmdLine.begin(), tolower);	
	for( node = node->FirstChild("Profile"); node;	node = node->NextSibling("Profile") )
	{
		for( TiXmlElement* fileElem = node->FirstChildElement("File"); fileElem; fileElem = fileElem->NextSiblingElement("File") )
		{
			std::basic_string<TCHAR> Name = common::utils::from_utf8(fileElem->Attribute("Name"));		
			std::transform(Name.begin(), Name.end(), Name.begin(), tolower);
			if (_tcsicmp(appFileName, Name.c_str()) == 0)
			{
				if (!bCreate) // BaseProfile
					bChangeProfileName = true;
				else if (gInfo.ProfileName[0] == '\0')
					bChangeProfileName = true;
				else
				{
					int bUserChangeProfileName = 0;
					node->ToElement()->QueryIntAttribute("UserChangeProfileName", &bUserChangeProfileName);
					bChangeProfileName = bUserChangeProfileName != 0;
				}
				const char* cmdLine = fileElem->Attribute("CmdLine");
				if (cmdLine != NULL)
				{
					std::string CmdLine = cmdLine;
					std::transform(CmdLine.begin(), CmdLine.end(), CmdLine.begin(), tolower);	
					std::wstring cmdLineUTF8 = common::utils::from_utf8(CmdLine);
					if (_tcsstr(AppCmdLine.c_str(), cmdLineUTF8.c_str()) != 0)
					{
						if (!bCreate) // BaseProfile
						{					
							_tcscpy_s<MAX_PATH>(gInfo.Params, cmdLineUTF8.c_str());
							FileFound = true;
							profileNode = node;
						}
						else if (_tcscmp(gInfo.Params, cmdLineUTF8.c_str()) == 0)
						{
							FileFound = true;
							profileNode = node;
						}
					}
				}
				else if (!bCreate || _tcscmp(gInfo.Params, L"") == 0)
				{
					FileFound = true;
					profileNode = node;
				}
			}
		}
		if (FileFound)
		{
			// Real XML profile match. Set per-source flag so diagnostics
			// can distinguish Base / Community / User matches separately,
			// not just "matched or not."
			switch (profileSource)
			{
			case 1:  gInfo.bMatchedInCommunity = true; break;
			case 2:  gInfo.bMatchedInUser      = true; break;
			default: gInfo.bMatchedInBase      = true; break;
			}
			if (bChangeProfileName)
			{
				_tcscpy_s<MAX_PATH>(gInfo.ProfileName,
					common::utils::from_utf8(node->ToElement()->Attribute("Name")).c_str());
			}
			else
			{
				node->ToElement()->SetAttribute("Name",
					common::utils::to_utf8(gInfo.ProfileName).c_str() );
			}
			ReadProfilePart1Data(node);
			break;
		}
	}
	if (!FileFound)
	{
		DEBUG_MESSAGE(_T("Profile not founded\n"));
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
	_tcscpy_s<MAX_PATH>(szApplicationFileName, gInfo.ApplicationFileName);
	_tcslwr_s<MAX_PATH>(szApplicationFileName);
	TCHAR szProfilesFileName[MAX_PATH];
	TCHAR szPath[MAX_PATH];

	_tcscpy_s<MAX_PATH>(gInfo.ProfileName, _T(""));
	_tcscpy_s<MAX_PATH>(gInfo.Params, _T(""));
	if (iz3d::resources::GetAllUsersDirectory( szPath ))
	{
		_stprintf_s<MAX_PATH>(szProfilesFileName, _T("%s\\BaseProfile.xml"), szPath);
		DEBUG_MESSAGE(_T("Base profile:\n"));
		ReadProfileRouterType(szApplicationFileName, szProfilesFileName, g_docSystemProfiles, g_nodeSystemProfile, false, 0 /*Base*/);

		if (g_nodeSystemProfile == NULL)
		{
			_stprintf_s<MAX_PATH>(szProfilesFileName, _T("%s\\CommunityProfile.xml"), szPath);
			DEBUG_MESSAGE(_T("Community profile:\n"));
			ReadProfileRouterType(szApplicationFileName, szProfilesFileName, g_docCommunityProfiles, g_nodeCommunityProfile, false, 1 /*Community*/);
		}
	}

	if (iz3d::resources::GetCurrentUserDirectory( szPath ))
	{
		PathAppend(szPath, _T("UserProfile.xml") );
		DEBUG_MESSAGE(_T("User profile:\n"));
		ReadProfileRouterType(szApplicationFileName, szPath, g_docUserProfiles, g_nodeUserProfile, true, 2 /*User*/);
	}

	if (gInfo.ProfileName[0] == '\0')
	{
		const TCHAR* ch = _tcsrchr(gInfo.ApplicationFileName, '\\') + 1;
		DWORD len = (DWORD)(_tcsrchr(gInfo.ApplicationFileName, '.') - ch);
		_tcsncpy_s<MAX_PATH>(gInfo.ProfileName, ch, len);
		gInfo.ProfileName[len] = '\0';
	}
}

void ReadInputData(TiXmlNode* node)
{
	//--- read input parameters ---
	TiXmlElement* item = node->ToElement();
	TiXmlElement* pitem = item->FirstChildElement("ShowWizardAtStartup");
	if (pitem)
		QueryBoolAsInt(pitem, "Value", &gInfo.Input.ShowWizardAtStartup);
	pitem = item->FirstChildElement("SwapEyes");
	if (pitem)
		QueryBoolAsInt(pitem, "Value", &gInfo.Input.SwapEyes);
	pitem = item->FirstChildElement("LaserSightEnable");
	if (pitem)
		QueryBoolAsInt(pitem, "Value", &gInfo.Input.LaserSightEnable);
	// NvAPI bridge: stereo on/off persists in UserProfile so the in-game
	// 3D Vision-style toggle round-trips across sessions (and across the
	// launcher/game split for titles like Batman AA).
	pitem = item->FirstChildElement("StereoActive");
	if (pitem)
		QueryBoolAsInt(pitem, "Value", &gInfo.Input.StereoActive);
	node = node->FirstChild("Presets");
	if (node)
	{
		for( item = node->FirstChildElement("Preset"); item; item = item->NextSiblingElement("Preset") )
		{
			int i;
			item->QueryIntAttribute("Index", &i);
			if (i >= 0 && i < 3)
			{
				pitem = item->FirstChildElement("One_div_ZPS");
				if (pitem)
				{
					pitem->QueryFloatAttribute("Value", &gInfo.Input.Preset[i].One_div_ZPS);
					gInfo.Input.Preset[i].One_div_ZPS_Saved = gInfo.Input.Preset[i].One_div_ZPS;
				}

				pitem = item->FirstChildElement("StereoBase");
				if (pitem)
					pitem->QueryFloatAttribute("Value", &gInfo.Input.Preset[i].StereoBase);

				pitem = item->FirstChildElement("AutoFocusEnable");
				if (pitem)
					QueryBoolAsInt(pitem, "Value", &gInfo.Input.Preset[i].AutoFocusEnable);
			}
		}
	}
}

TiXmlNode* GetNode(const char *appFileNameUTF8, TiXmlNode* &profileNode)
{
	if (g_nodeUserProfile)
	{
		profileNode = g_nodeUserProfile;
		return profileNode;
	}
	else
	{
		if (g_docUserProfiles)
		{
			profileNode = g_docUserProfiles->FirstChild( "Profiles" );
			TiXmlElement item = TiXmlElement("Profile");
			item.SetAttribute("Name", common::utils::to_utf8(gInfo.ProfileName));
			TiXmlElement fileItem = TiXmlElement("File");
			fileItem.SetAttribute("Name", strrchr(appFileNameUTF8, '\\') + 1);
			if (_tcscmp(gInfo.Params, _T("")) != 0)
				fileItem.SetAttribute("CmdLine", common::utils::to_utf8(gInfo.Params).c_str());
			item.InsertEndChild(fileItem);
			g_nodeUserProfile = profileNode->InsertEndChild(item);
			profileNode = g_nodeUserProfile;
		}
		else
			profileNode = NULL;
		return profileNode;
	}
}

TiXmlElement* FindOrCreateElement(TiXmlNode* pNode, const char* value)
{
	TiXmlElement* pitem = pNode->FirstChildElement(value);
	if (pitem == NULL)
		pitem = pNode->InsertEndChild(TiXmlElement(value))->ToElement();
	return pitem;
}

void WriteInputData(DataInput* overrideInput)
{
	TiXmlNode* node = 0;
	//--- save camera parameters ---
	if(!overrideInput || !GetNode(common::utils::to_utf8(gInfo.ApplicationFileName).c_str(), node))
		return;

	TiXmlElement* pitem = FindOrCreateElement(node, "ShowWizardAtStartup");
	pitem->SetAttribute("Value", overrideInput->ShowWizardAtStartup);
	pitem = FindOrCreateElement(node, "SwapEyes");
	pitem->SetAttribute("Value", overrideInput->SwapEyes);
	pitem = FindOrCreateElement(node, "LaserSightEnable");
	pitem->SetAttribute("Value", overrideInput->LaserSightEnable);
	pitem = FindOrCreateElement(node, "StereoActive");
	pitem->SetAttribute("Value", overrideInput->StereoActive);
	TiXmlNode* node2 = node->FirstChild("Presets");
	if (!node2)
		node2 = node->InsertEndChild(TiXmlElement("Presets"));
	else 
		node2->Clear();
	for( int i = 0; i < 3; i++ )
	{
		TiXmlElement* item = node2->InsertEndChild(TiXmlElement("Preset"))->ToElement();
		item->SetAttribute("Index", i);
		pitem = item->InsertEndChild(TiXmlElement("One_div_ZPS"))->ToElement();
		pitem->SetDoubleAttribute("Value", overrideInput->Preset[i].One_div_ZPS);
		pitem = item->InsertEndChild(TiXmlElement("StereoBase"))->ToElement();
		pitem->SetDoubleAttribute("Value", overrideInput->Preset[i].StereoBase);
		pitem = item->InsertEndChild(TiXmlElement("AutoFocusEnable"))->ToElement();
		pitem->SetAttribute("Value", overrideInput->Preset[i].AutoFocusEnable);
	}
	g_docUserProfiles->SaveFile();
}

void ReadShaderData( TiXmlElement* elem, int shaderType, float fMultiplier, float fConvergenceShift );
void ReadVectorData( TiXmlElement* itemElement, float fMultiplier )
{
	struct CRC_VECTORS
	{
		char *szItem;
		ResourcesCRCMultiplierMapBySize *pVector;
	};
	CRC_VECTORS pVecItems[] = 
	{
		{ "Texture",	&g_ProfileData.TextureCRCMultiplier },
		{ "Mesh",		&g_ProfileData.MeshCRCMultiplier },
	};

	float fConvergenceShift = 0.0f;
	itemElement->QueryFloatAttribute("ConvergenceShift", &fConvergenceShift);

	for( TiXmlElement* elem = itemElement->FirstChildElement(); elem; elem = elem->NextSiblingElement() )
	{
		if (fMultiplier == 0.0f)
		{
			if ( strcmp( elem->Value(), "RHW" ) == 0 )
			{
				float fRHW = 0.0f;
				if (elem->QueryFloatAttribute("Value", &fRHW)==TIXML_SUCCESS)
				{
					g_ProfileData.MonoRHWValues.insert(fRHW);
				}
				continue;
			}
		}
		bool bVS = strcmp( elem->Value(), "VertexShader" ) == 0;
		bool bPS = strcmp( elem->Value(), "PixelShader" ) == 0;
		bool bGS = strcmp( elem->Value(), "GeometryShader" ) == 0;
		bool bWeapon = strcmp( elem->Value(), "Weapon" ) == 0;
		if ( bVS || bPS || bGS )
			ReadShaderData(elem, bVS ? 1 : (bPS ? 2 : 3), fMultiplier, fConvergenceShift);
		else if ( bWeapon )
		{
			g_ProfileData.WeaponMultiplier.m_Multiplier = fMultiplier;
			g_ProfileData.WeaponMultiplier.m_ConvergenceShift = fConvergenceShift;
			g_ProfileData.WeaponMaxZ = 0.5f;
			elem->QueryFloatAttribute("MaxZ", &g_ProfileData.WeaponMaxZ);
		}
		else
		{
			for (int i = 0; i < _countof(pVecItems); i++)
			{
				if ( strcmp( elem->Value(), pVecItems[i].szItem ) == 0 )
				{
					DWORD Size;
					if (elem->QueryIntAttribute("Size", (int*)&Size) != TIXML_SUCCESS)
						Size = 0;
					ResourcesCRCMultiplierMapWithSize item(Size, ResourceCRCMultiplierMap());
					pVecItems[i].pVector->insert(item);
					ResourcesCRCMultiplierMapBySize::iterator iter = pVecItems[i].pVector->find(Size);
					const char* CRC = elem->Attribute("CRC");
					if (CRC)
					{
						DWORD dwValue = 0;
						sscanf_s(CRC, "0x%x", &dwValue);

						ResourceProfileData* pProfile = &iter->second[dwValue];
						pProfile->m_Multiplier = fMultiplier;
						pProfile->m_ConvergenceShift = fConvergenceShift;
					}
					break;
				}
			}
		}
	}
}

void ReadProfilePart2Data(TiXmlNode* node)
{
	if (!node)
		return;

	TiXmlElement* itemElement = 0;
	static XmlValue pValItems[] = 
	{
		DECLARE_BVALUE(ShowFPS),
		DECLARE_BVALUE(ShowOSD),
		DECLARE_IVALUE(SeparationMode),
		DECLARE_IVALUE(FrustumAdjustMode),
		DECLARE_FVALUE(BlackAreaWidth),
		DECLARE_IVALUE(PresenterSleepTime),
		DECLARE_FVALUE(PresenterFlushTime),
		DECLARE_FVALUE(PresenterMaxLagPercent),
		DECLARE_IVALUE(DeviceMode),
		DECLARE_IVALUE(MultiWindowsMode),
		DECLARE_IVALUE(ModeObtainingDepthBuffer),
		DECLARE_BVALUE(DisableFullscreenModeEmulation),
		DECLARE_BVALUE(ForceVSyncOff),
		DECLARE_BVALUE(EnableAA),
		DECLARE_BVALUE(WideRenderTarget),
		DECLARE_BVALUE(CommandBuffering),
		DECLARE_BVALUE(RenderTargetBuffering),
		// 0=never stereo, 1=always stereo, 2=heuristic (size/shape/MonoRT-flag aware).
		// Default 2. Setting to 1 force-flags every RT as stereo — useful for
		// games where the heuristic excludes intermediate RTs that should be
		// stereo (causes "scene mono, only UI stereo" symptom).
		DECLARE_IVALUE(RenderTargetCreationMode),
		DECLARE_BVALUE(MonoRenderTargetTextures),
		DECLARE_BVALUE(MonoRenderTargetSurfaces),
		DECLARE_BVALUE(MonoDepthStencilTextures),
		DECLARE_BVALUE(MonoDepthStencilSurfaces),
		DECLARE_BVALUE(AlwaysModifyShaders),
		DECLARE_IVALUE(VSSeparationUpdateMode),
		DECLARE_IVALUE(VertexShaderModificationMethod),
		DECLARE_BVALUE(ModifyIncorrectProjectionShaders),
		DECLARE_BVALUE(AlwaysUseDefaultProjectionMatrixForVS),
		DECLARE_BVALUE(CreateOneComponentRTInMono),
		DECLARE_BVALUE(CreateSquareRTInMono),
		DECLARE_BVALUE(CreateRTThatLessThanBBInMono),
		DECLARE_BVALUE(CreateSquareDSInMono),
		DECLARE_BVALUE(CreateDSThatLessThanBBInMono),
		DECLARE_BVALUE(CreateBigSquareRTInStereo),
		DECLARE_BVALUE(CreateBigSquareDSInStereo),
		DECLARE_BVALUE(DisableNVCSTextureInWRTMode),
		DECLARE_IVALUE(RenderToRightInMono),
		DECLARE_IVALUE(GameSpecific),
		DECLARE_BVALUE(HideAdditionalAdapters),
		DECLARE_BVALUE(MonoFixedPipeline),
		DECLARE_IVALUE(MonoFixedPipelineCheckingMethodIndex),
		DECLARE_BVALUE(MonoDrawFromMemory),
		DECLARE_BVALUE(MonoRHWPipeline),
		DECLARE_IVALUE(MonoRHWCheckingMethodIndex),
		DECLARE_BVALUE(RenderNonMatrixShadersInMono),
		DECLARE_BVALUE(RenderIncorrectProjectionShadersInMono),
		DECLARE_BVALUE(DX10MonoNonIndexedDraw),
		DECLARE_IVALUE(RHWModificationMethod),
		DECLARE_BVALUE(UseSimpleStereoProjectionMethod),
		DECLARE_BVALUE(SkipCheckOrthoMatrix),
		DECLARE_BVALUE(CheckOnlyProjectionMatrix),
		DECLARE_BVALUE(CheckExistenceInverseMatrix),
		DECLARE_BVALUE(CheckShadowMatrix),
		DECLARE_BVALUE(CheckInverseLR),
		DECLARE_BVALUE(FixNone16x9TextPosition),
		DECLARE_FVALUE(SeparationScale),
		DECLARE_FVALUE(ScaleSeparationForSmallViewPorts),
		DECLARE_IVALUE(SearchFrameSizeX),
		DECLARE_IVALUE(SearchFrameSizeY),
		DECLARE_FVALUE(ShiftSpeed),
		DECLARE_FVALUE(Max_ABS_rZPS),
		DECLARE_FVALUE(FrameYCoordinate),
		DECLARE_IVALUE(LaserSightIconIndex),
		DECLARE_SVALUE(CustomLaserSightIconPath),
		DECLARE_FVALUE(LaserSightXCoordinate),
		DECLARE_FVALUE(LaserSightYCoordinate),
	};

	float fMultiplier = 0.0f;
	for( itemElement = node->FirstChildElement(); itemElement; itemElement = itemElement->NextSiblingElement() )
	{
		int i;
		bool ValueFound = false;
		bool VectorFound = false;
		for (i = 0; i < _countof(pValItems); i++)
		{
			if ( strcmp( itemElement->Value(), pValItems[i].pName ) == 0 )
			{
				ValueFound = true;
				break;
			}
		}
		if (!ValueFound)
		{
			if ( strcmp( itemElement->Value(), "Mono" ) == 0 )
			{
				fMultiplier = 0.0f;
				VectorFound = true;
			}
			else if ( strcmp( itemElement->Value(), "Stereo" ) == 0 )
			{
				fMultiplier = 1.0f;
				VectorFound = true;
			}
			else if ( strcmp( itemElement->Value(), "Multiplier" ) == 0 )
			{
				itemElement->QueryFloatAttribute("Value", &fMultiplier);
				VectorFound = true;
			}
		}
		if (ValueFound)
		{
			pValItems[i].ReadData(itemElement);
		}
		else if (VectorFound)
		{
			ReadVectorData(itemElement, fMultiplier);
		}
		else if ( strcmp( itemElement->Value(), "AdditionalMatrixName" ) == 0 )
		{
			g_ProfileData.AdditionalMatrixName.push_back(std::string(itemElement->Attribute("Value")));
		}
		else if ( strcmp( itemElement->Value(), "Keys" ) == 0 )
		{
			for( TiXmlElement* itemElement2 = itemElement->FirstChildElement(); itemElement2; itemElement2 = itemElement2->NextSiblingElement() )
			{
				int j;
				bool KeyFound = false;
				for (j = 0; j < _countof(gInfo.HotKeyList); j++)
				{
					if ( strcmp( itemElement2->Value(), gInfo.HotKeyList[j].HotKeyName ) == 0 )
					{
						KeyFound = true;
						break;
					}
				}
				if (KeyFound)
				{
					itemElement2->QueryIntAttribute("Key", (int*)&gInfo.HotKeyList[j].code);
				}
			}
		}
	}
	if (gInfo.DeviceMode == DEVICE_MODE_RESERVED)
		gInfo.DeviceMode = DEVICE_MODE_AUTO;
	if (gInfo.VertexShaderModificationMethod == 2 && gInfo.AlwaysModifyShaders)
		gInfo.UseSimpleStereoProjectionMethod = true;
	ReadInputData(node);
}

bool ReadConfig(DWORD Vendor)
{
	g_ProfileData.clear();

	if (g_docConfig == NULL)
		return false;
	TiXmlNode* rootNode = g_docConfig->FirstChild( "Config" );
	TiXmlNode* node = rootNode->FirstChild("Vendors");
	TiXmlElement* itemElement;
	if (node)
	{
		for( itemElement = node->FirstChildElement("Vendor"); itemElement; itemElement = itemElement->NextSiblingElement("Vendor") )
		{
			DWORD VendorVal = 0;
			const char* Value = itemElement->Attribute("Value");
			if (Value)
			{
				sscanf_s(Value, "%x", &VendorVal);
			}
			if (VendorVal == Vendor)
			{
				_tcscpy_s<_countof(gInfo.Vendor)>(gInfo.Vendor,					
					common::utils::to_unicode_simple( itemElement->Attribute("Name") ).c_str() );
				break;
			}
		}
		if (gInfo.Vendor[0] == '\0')
			_tcscpy_s<_countof(gInfo.Vendor)>(gInfo.Vendor, _T("UNKNOWN"));
	}
	DEBUG_MESSAGE(_T("Device vendor: %s\n"), gInfo.Vendor);
	node = rootNode->FirstChild("DefaultProfile");
	ReadProfilePart2Data(node);

	return true;
}

void ReadProfiles()
{
	gInfo.Input.StereoActive = gInfo.EnableStereo;
	ReadProfilePart2Data(g_nodeSystemProfile);
	ReadProfilePart2Data(g_nodeCommunityProfile);
	ReadProfilePart2Data(g_nodeUserProfile);
	gInfo.Input.ShowFPS = gInfo.ShowFPS;
}

void ReadCurrentProfile( DWORD Vendor )
{
	ReadConfig(Vendor);
	ReadProfiles();
}

void FreeProfiles()
{
	if (g_docSystemProfiles)
	{
		delete g_docSystemProfiles;
		g_docSystemProfiles = NULL;
	}
	g_nodeSystemProfile = NULL;
	if (g_docCommunityProfiles)
	{
		delete g_docCommunityProfiles;
		g_docCommunityProfiles = NULL;
	}
	g_nodeCommunityProfile = NULL;
	if (g_docUserProfiles)
	{
		delete g_docUserProfiles;
		g_docUserProfiles = NULL;
	}
	g_nodeUserProfile = NULL;
	if (g_docConfig)
	{
		delete g_docConfig;
		g_docConfig = NULL;
	}
	g_outputConfig = NULL;
}

LocalizationData g_LocalData;

LocalizationData::LocalizationData()
: Text(LT_TotalStrings, std::wstring(L""))
{
}


bool GetConfigFullPath(TCHAR path[MAX_PATH])
{
	bool ret = true;

	// First check: next to the driver DLLs (portable/proxy deployment).
	// Prefer wiz3D_Config.xml, fall back to legacy Config.xml.
	_tcscpy_s(path, MAX_PATH, gInfo.DriverDirectory);
	PathAppend(path, TEXT("wiz3D_Config.xml"));
	if (PathFileExists(path))
		return ret;
	_tcscpy_s(path, MAX_PATH, gInfo.DriverDirectory);
	PathAppend(path, TEXT("Config.xml"));
	if (PathFileExists(path))
		return ret;

#ifndef FINAL_RELEASE
	// Dev check: parent directory (legacy dev layout)
	_tcscpy_s(path, MAX_PATH, gInfo.DriverDirectory);
	_tcscat(path, _T("\\.."));
	PathAppend(path, TEXT("wiz3D_Config.xml"));
	if (PathFileExists(path))
		return ret;
	_tcscpy_s(path, MAX_PATH, gInfo.DriverDirectory);
	_tcscat(path, _T("\\.."));
	PathAppend(path, TEXT("Config.xml"));
	if (PathFileExists(path))
		return ret;
#endif

	// Fallback: ProgramData shared directory.
	// Prefer wiz3D_Config.xml, fall back to legacy Config.xml.
	if (!iz3d::resources::GetAllUsersDirectory( path ))
		ret = false;
	PathAppend(path, _T("wiz3D_Config.xml"));
	if (PathFileExists(path))
		return ret;
	if (!iz3d::resources::GetAllUsersDirectory( path ))
		ret = false;
	PathAppend(path, _T("Config.xml"));
	return ret;
}

void LocalizationData::ReadLocalizationText()
{
	C_ASSERT(LT_TotalStrings == _countof(LOCALIZATION_TEXT_LIST));

	TCHAR	XMLFileName[MAX_PATH];

	if(!iz3d::resources::GetLanguageFilesPath(XMLFileName))
		return;

	_stprintf_s(XMLFileName + _tcslen(XMLFileName), _countof(XMLFileName) - _tcslen(XMLFileName), TEXT("\\%s.xml"), gInfo.Language);
	if(!PathFileExists(XMLFileName))
	{
		//--- select English file by default ---
		PathRemoveFileSpec(XMLFileName);
		PathAppend(XMLFileName, _T("English.xml"));
	}
	//--- read localization file ---
	CHAR XMLFileNameC[MAX_PATH];

	wcstombs(XMLFileNameC, XMLFileName, MAX_PATH);
	TiXmlDocument StringList(XMLFileNameC);
	if(StringList.LoadFile())
	{
		TiXmlHandle docHandle( &StringList );
		TiXmlElement* itemElement = docHandle.FirstChild("LanguagePack").FirstChild("Driver").FirstChild().Element();
		for( ; itemElement; itemElement = itemElement->NextSiblingElement())
		{
			const char* s = itemElement->GetText();
			if(s)
			{
				WCHAR textString[MAX_PATH];
				common::utils::UTF8ToUCS2(s, textString);
				for(int i=0; i< LT_TotalStrings; i++)
					if(stricmp(itemElement->Value(), LOCALIZATION_TEXT_LIST[i]) == 0)
						Text[i] = textString;
			}
		}
	}
}

void ReadShaderData( TiXmlElement* elem, int shaderType, float fMultiplier, float fConvergenceShift )
{
	const char* CRC = elem->Attribute("CRC");
	if (!CRC)
		return;

	DWORD dwValue = 0;
	sscanf_s(CRC, "0x%x", &dwValue);
	ShaderProfileData* pProfile;
	if (shaderType == 1) // VS
		pProfile = &g_ProfileData.VSCRCData[dwValue];
	else if (shaderType == 2) // PS
		pProfile = &g_ProfileData.PSCRCData[dwValue];
	else // GS
		pProfile = &g_ProfileData.GSCRCData[dwValue];

	pProfile->m_Multiplier = fMultiplier;
	pProfile->m_ConvergenceShift = fConvergenceShift;

	bool bModifyShader = false;
	if (shaderType == 1 && QueryBoolAsInt(elem, "Modify", &bModifyShader)==TIXML_SUCCESS)
		pProfile->m_bModifyShader = bModifyShader;

	float ZNear;
	if (shaderType == 1 && elem->QueryFloatAttribute("ZNear", &ZNear)==TIXML_SUCCESS)
	{
		pProfile->m_bModifyShader = true;
		pProfile->m_bAddZNearCheck = true;
		pProfile->m_ZNear = ZNear;
	}

	const char* ExtCRC = elem->Attribute("UseMatricesFrom");
	if (ExtCRC)
	{
		DWORD dwExtValue = 0;
		sscanf_s(ExtCRC, "0x%x", &dwExtValue);
		ShaderProfileDataMap::const_iterator it;
		if (shaderType == 1) // VS
		{
			it = g_ProfileData.VSCRCData.find(dwExtValue);
			if (it != g_ProfileData.VSCRCData.end())
				pProfile->m_pMatrices = it->second.m_pMatrices;
		}
		else if (shaderType == 2) // PS
		{
			it = g_ProfileData.PSCRCData.find(dwExtValue);
			if (it != g_ProfileData.PSCRCData.end())
				pProfile->m_pMatrices = it->second.m_pMatrices;
		}
		else // GS
		{
			it = g_ProfileData.GSCRCData.find(dwExtValue);
			if (it != g_ProfileData.GSCRCData.end())
				pProfile->m_pMatrices = it->second.m_pMatrices;
		}
		return;
	}

	for( TiXmlElement* vsElem = elem->FirstChildElement(); vsElem; vsElem = vsElem->NextSiblingElement() )
	{
		if ( strcmp( vsElem->Value(), "Matrices" ) == 0 )
		{
			ShaderMatrices* pMatrices = DNew ShaderMatrices;
			pProfile->m_pMatrices = pMatrices;

			int cnt = 0;
			vsElem->QueryIntAttribute("Count", &cnt);
			if (cnt > SHADER_MATRIX_SIZE)
			{
				cnt = SHADER_MATRIX_SIZE;
				DEBUG_MESSAGE(_T("WARNING: More matrices, more...\n"));
			}
			int i = 0;
			for( TiXmlElement* matrixElem = vsElem->FirstChildElement("Matrix"); matrixElem && i < cnt; matrixElem = matrixElem->NextSiblingElement("Matrix"), i++ )
			{
				TiXmlElement* el = matrixElem->FirstChildElement("Register");
				pMatrices->matrix[i].matrixRegister = 0;
				if (el)
					el->QueryIntAttribute("Value", (int*)&pMatrices->matrix[i].matrixRegister);
								
				pMatrices->matrix[i].constantBuffer = 0;
				el = matrixElem->FirstChildElement("ConstantBuffer");
				if (el)
					el->QueryIntAttribute("Value", (int*)&pMatrices->matrix[i].constantBuffer);
				pMatrices->matrix[i].matrixIsTransposed = false;
				el = matrixElem->FirstChildElement("IsTransposed");
				if (el)
					el->QueryIntAttribute("Value", (int*)&pMatrices->matrix[i].matrixIsTransposed);
				pMatrices->matrix[i].incorrectProjection = false;
				el = matrixElem->FirstChildElement("IncorrectProjection");
				if (el)
					el->QueryIntAttribute("Value", (int*)&pMatrices->matrix[i].incorrectProjection);
				pMatrices->matrix[i].inverse = false;
				el = matrixElem->FirstChildElement("Inverse");
				if (el)
					el->QueryIntAttribute("Value", (int*)&pMatrices->matrix[i].inverse);
			}
			pMatrices->matrixSize = i;
		}
		else if ( strcmp(vsElem->Value(), "ProjTransform") == 0 )
		{
			pProfile->m_PreTransformProjectionId = ++g_PreTransformProjectionId;
			
			FLOAT* m = pProfile->m_ProjTransform;
			const char* mtext = vsElem->GetText();
			sscanf_s(mtext, "%f %f %f %f\
							 %f %f %f %f\
							 %f %f %f %f\
							 %f %f %f %f",
							&m[0],  &m[1],  &m[2],  &m[3],
							&m[4],  &m[5],  &m[6],  &m[7],
							&m[8],  &m[9],  &m[10], &m[11],
							&m[12], &m[13], &m[14], &m[15]);
		}
	}
}
