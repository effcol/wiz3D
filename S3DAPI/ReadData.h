/* 
* Project : iZ3D Stereo Driver
* Copyright (C) iZ3D Inc. 2002 - 2010
*/

#pragma once

#include "S3DAPI.h"
#include <tinyxml.h>
#include <vector>
#include <string>

struct DataInput;

#define PROFILES_VERSION 1
bool ReadConfigRouterType();
void ReadProfilesRouterType();
void FreeProfiles();

void S3DAPI_API ReadCurrentProfile( DWORD Vendor );
void S3DAPI_API WriteInputData(DataInput* overrideInput);
extern S3DAPI_API TiXmlNode* g_outputConfig;

// Serialize the active output method's config subtree (g_outputConfig) to an
// XML string. MUST be used instead of handing g_outputConfig across a DLL
// boundary: S3DAPI links plain TinyXML 2.6.2 (no TIXML_USE_TICPP) while
// S3DWrapper* and every OutputMethod link the ticpp shim, which makes
// TiXmlBase derive from TiCppRC — a different vtable / sizeof / member layout.
// Walking an S3DAPI-allocated node from a shim-built DLL reads garbage offsets
// and crashes (observed as a GPF in AnaglyphOutput on Batman: Arkham Asylum).
// The node is serialized here, inside S3DAPI, with S3DAPI's own TinyXML; the
// caller re-parses the returned string with its own parser. Returns NULL when
// there is no output config. The returned pointer is owned by S3DAPI and stays
// valid until the next call to this function or FreeProfiles().
S3DAPI_API const char* GetOutputConfigXml();

bool S3DAPI_API GetConfigFullPath(TCHAR path[MAX_PATH]);

enum LOCALIZATION_TEXT_ID
{
	LT_CountingFPS = 0,
	LT_Mono,
	LT_Stereo,
	LT_drop,
	LT_Preset,
	LT_Convergence,
	LT_ConvergenceINF,
	LT_Separation,
	LT_AutoFocus,
	LT_ON,
	LT_OFF,
	LT_tooMuchSeparation,
	LT_SwapLR,
	LT_DaysLeft,
	LT_CantFindStereoDevice,
	LT_CantLoadOutputDLL,
	LT_UpdateAMDDriver,
	LT_NotSupported3DDevice,

	LT_TotalStrings //--- just string counter ---
};

class LocalizationData
{
public:
	LocalizationData();
	void ReadLocalizationText();
	const std::wstring& GetText(LOCALIZATION_TEXT_ID id)
	{
		const std::wstring& text = Text[id];
		return text;
	}

private:
	std::vector<std::wstring> Text;
};

extern S3DAPI_API LocalizationData g_LocalData;
