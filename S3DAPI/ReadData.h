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
// 0.4.3: S3DAPI_API export required so the DX9 + DX10/11 wrapper DLLs can
// link these at CI Release config. Without it, the symbols resolve locally
// in Final Release (which has different linker settings) but the Release
// smoke-test build gets LNK2001 on ShaderHunter9.obj / S3DWrapperD3D10.obj.
bool S3DAPI_API ReadConfigRouterType();
void S3DAPI_API ReadProfilesRouterType();
void S3DAPI_API FreeProfiles();

void S3DAPI_API ReadCurrentProfile( DWORD Vendor );
void S3DAPI_API WriteInputData(DataInput* overrideInput);

// 0.5.0 ShaderHunter mono-this-CRC (Numpad 4). Mutates g_ProfileData so the
// stereo pipeline treats this shader as mono immediately, and appends a
// <Mono><Vertex/PixelShader CRC="..."/></Mono> entry to the per-user
// UserProfile.xml so the fix survives a game restart. Idempotent — repeat
// calls with the same CRC+stage leave the file (and the map entry) unchanged.
//
// stage: 0 = VS, 1 = PS. Returns true if the entry was newly added, false if
// it was already present or the user profile infrastructure wasn't available.
// The caller is expected to be a hotkey handler on the KeyboardHook thread;
// writes to g_ProfileData are unsynchronised, matching the existing
// MaybeReloadProfileBetweenFrames pattern for Ctrl+F5.
bool S3DAPI_API SaveMonoShaderCRCToUserProfile(DWORD crc, int stage);

// 0.5.0 toggle: companion of SaveMonoShaderCRCToUserProfile. Removes the
// <VertexShader CRC=...>/<PixelShader CRC=...> entry from UserProfile.xml's
// <Mono> block AND clears m_Multiplier/m_bSkipStereo on the hot
// g_ProfileData entry so the shader renders stereo again on the very next
// draw. Returns true if an entry was actually removed, false if the CRC
// wasn't in the file.
bool S3DAPI_API RemoveMonoShaderCRCFromUserProfile(DWORD crc, int stage);

// 0.5.0 toggle query: returns true if the (crc, stage) pair has a
// <VertexShader CRC=...>/<PixelShader CRC=...> entry inside the active
// game's <Mono> block in UserProfile.xml. Used by Numpad 4 to decide
// between save and remove on the current cycle target.
bool S3DAPI_API IsMonoShaderCRCInUserProfile(DWORD crc, int stage);

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
