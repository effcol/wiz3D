/* 
* Project : iZ3D Stereo Driver
* Copyright (C) iZ3D Inc. 2002 - 2010
*/

#pragma once

#include "S3DAPI.h"
#include <boost/unordered_map.hpp>
#include <boost/unordered_set.hpp>
#include <unordered_map>
#include <vector>
#include <d3dx9math.h>
#include <limits>
#include <list>
#include <map>
#include <string>

#define SHADER_MATRIX_SIZE 2

struct ShaderMatrixData
{
	DWORD					constantBuffer;
	DWORD                   matrixRegister;             // shader matrix register index
	BOOL					matrixIsTransposed;
	BOOL					incorrectProjection;
	BOOL					inverse;
};

struct ShaderMatrices
{
	BYTE					matrixSize;
	ShaderMatrixData		matrix[SHADER_MATRIX_SIZE];
};

class ResourceProfileData
{
public:
	ResourceProfileData() : m_Multiplier(1.0f), m_ConvergenceShift(0.0f) {}
	float	m_Multiplier;
	float	m_ConvergenceShift;
};

#ifdef _DEBUG
typedef std::unordered_map<DWORD, ResourceProfileData>			ResourceCRCMultiplierMap;
typedef std::unordered_map<DWORD, ResourceCRCMultiplierMap>		ResourcesCRCMultiplierMapBySize;
#else
typedef boost::unordered_map<DWORD, ResourceProfileData>		ResourceCRCMultiplierMap;
typedef boost::unordered_map<DWORD, ResourceCRCMultiplierMap>	ResourcesCRCMultiplierMapBySize;
#endif
typedef std::pair<DWORD, ResourceProfileData>					ResourceCRCMultiplier;
typedef std::pair<DWORD, ResourceCRCMultiplierMap>				ResourcesCRCMultiplierMapWithSize;

class ShaderProfileData : public ResourceProfileData
{
public:
	ShaderProfileData() :
		ResourceProfileData(),
		m_bModifyShader(false),
		m_bAddZNearCheck(false),
		m_bSkipStereo(false),
		m_ZNear(1.0f),
		m_pMatrices(NULL),
		m_PreTransformProjectionId(-1),
		m_SpriteWorldInput(-1),
		m_SpriteCameraCB(-1),
		m_SpriteCameraReg(-1),
		m_FixedDepth(std::numeric_limits<float>::quiet_NaN()),
		m_DepthFromCB_Slot(-1),
		m_DepthFromCB_OffsetBytes(-1),
		m_ShiftUV_OutputReg(-1),
		m_ShiftUV_Component(0),
		m_ShiftUV_Amount(0.0f),
		m_AnimDepth_ProgressCB(-1),
		m_AnimDepth_ProgressOffset(-1),
		m_AnimDepth_Start(0.0f),
		m_AnimDepth_End(0.0f),
		m_PerEye_Slot(-1),
		m_PerEye_LeftCRC(0),
		m_PerEye_RightCRC(0),
		m_OverrideCB_Slot(-1),
		m_OverrideCB_OffsetBytes(-1),
		m_OverrideCB_Value(std::numeric_limits<float>::quiet_NaN())
	{}

	~ShaderProfileData()
	{
		delete m_pMatrices;
	}

	bool				m_bModifyShader;
	bool				m_bAddZNearCheck;
	// 0.4.1 ShaderPatch <SkipStereo/>: when true, Context11Proxy::EditRightCBs
	// skips the right-eye CB feature-edit pass if this shader is bound. Both
	// eyes end up rendering with the game's own (unshifted) CBs, so the
	// shader draws mono-both-eyes. Set via <ShaderPatch CRC="..." Stage=
	// "VS|PS"><SkipStereo/></ShaderPatch> in BaseProfile.xml. Complements the
	// legacy <Mono><VertexShader CRC/></Mono> path (which sets m_Multiplier=0)
	// by covering the CB-math feature engine added in PR #31 — the legacy
	// path affected only the DX9-era bStereo gate in ShaderWrapper.
	bool				m_bSkipStereo;
	float				m_ZNear;
	ShaderMatrices*		m_pMatrices;
	int					m_PreTransformProjectionId;	// id of the matrix for shader modification in particular frame (for Bioshock2 shadows), -1 - not used
	float				m_ProjTransform[16];

	// 0.5.0 <SpriteDepth/> primitive: XML-driven override for Akshay's auto-
	// detected sprite-depth path (Context11Proxy_RHW.cpp:BindSpriteDepth).
	// When the shader analyser's auto-detect misses a sprite VS (common for
	// Unity UI anchors that pack world-pos in an unusual input register),
	// a user can hunt the shader (Numpad 3), read the VS disassembly to find
	// which input register holds world-pos and which CB+register holds the
	// scene camera's w-row, and declare them here:
	//
	//   <ShaderPatch CRC="0x..." Stage="VS">
	//     <SpriteDepth WorldInput="3" CameraCB="0" CameraReg="4"/>
	//   </ShaderPatch>
	//
	// -1 on any of these = unused; auto-detection stays in control. The
	// SpriteDepth11.cpp twin reads these at bind time and feeds the DXBC
	// patch's two CB registers (w row + {b, m41, 0, 0}).
	int					m_SpriteWorldInput;    // vs input register with (world.xyz, 1)
	int					m_SpriteCameraCB;      // CB slot holding scene camera matrix
	int					m_SpriteCameraReg;     // first float4 register inside that CB

	// 0.5.0 <FixedDepth Value="X"/> primitive: static per-shader depth with
	// no world-pos dependency. Useful for 2D sprite games (Castlevania
	// Anniversary Collection) where a shader has no world-pos input but a
	// known depth tier (background ~ 0.0, sprites ~ 0.3, HUD ~ 0.0, etc.).
	// NaN sentinel = unset; any finite value activates. Actual per-eye shift
	// at render time is wiz3D_GetEffectiveEyeShift() * m_FixedDepth * EyeShiftSign().
	float				m_FixedDepth;

	// 0.5.0 <DepthFromCB CB="N" OffsetBytes="M"/> primitive: read depth live
	// from a float at (byte offset M) inside CB slot N on the bound VS. Covers
	// the "depth varies per draw via a game-filled constant" case — Fallout
	// Shelter ready-icon above a room writes the room's world Z into some CB
	// slot; this pulls it at bind-time and uses it as the depth. -1 slot = unset.
	int					m_DepthFromCB_Slot;
	int					m_DepthFromCB_OffsetBytes;

	// 0.5.0 <ShiftUV OutputReg="N" Component="x|y" Amount="0.004"/> primitive:
	// shift a UV output register in the VS by `Amount` per eye instead of the
	// clip-space position. Useful for sprites / text that would clip badly
	// under a position shift. Component: 0 = x, 1 = y. -1 reg = unset.
	int					m_ShiftUV_OutputReg;
	int					m_ShiftUV_Component;
	float				m_ShiftUV_Amount;

	// 0.5.0 <When TextureCRC="0x..."/> sub-filter: when non-empty, the
	// containing <ShaderPatch> primitives only apply if the current draw has
	// one of these texture CRCs bound at ANY PS resource slot. Closes the
	// "same CRC drawn many times with different textures" case — the FS
	// ready-icon uses the same UI shader as every other HUD element but a
	// different _MainTex. Capture the texture CRC via Numpad 3 in Draw mode,
	// then declare it here to isolate the one draw.
	std::vector<DWORD>	m_WhenTextureCRCs;

	// 0.5.0 <AnimateDepth StartDepth="X" EndDepth="Y" ProgressCB="N" ProgressOffset="M"/>:
	// lerps between StartDepth and EndDepth by a game-filled progress float
	// (read from CB N at byte offset M). Covers the Fallout-Shelter coins-
	// flying-to-counter pattern: the game writes animation progress (0..1)
	// into a constant; we read it per-draw and compute depth = lerp(Start,
	// End, progress). Slot < 0 = unset. Reuses the FixedDepth DXBC plan
	// (single ADD-constant-to-position.x); only the per-draw constant
	// computation differs.
	int					m_AnimDepth_ProgressCB;
	int					m_AnimDepth_ProgressOffset;
	float				m_AnimDepth_Start;
	float				m_AnimDepth_End;

	// 0.6.0 <PerEyeTexture Slot="N" LeftCRC="0x..." RightCRC="0x..."/>:
	// swap a bound PS SRV for a different one on the right-eye draw. For
	// games that ship pre-rendered stereo texture pairs (rare). LeftCRC
	// must match the game's current bind at Slot; RightCRC identifies the
	// right-eye SRV via wiz3D's lazy SRV-hash cache. Slot < 0 = unset.
	int					m_PerEye_Slot;
	DWORD				m_PerEye_LeftCRC;
	DWORD				m_PerEye_RightCRC;

	// 0.4.3 <OverrideCBValue CB="N" OffsetBytes="M" Value="X"/>: at right-eye
	// draw time, Map the game's CB slot N, overwrite the float at byte offset
	// M with X, Unmap. Covers HelixMod patterns where fixing stereo means
	// pushing a correct value into a specific constant (e.g. disabling a
	// broken effect by zeroing its intensity constant, or overriding a wrong
	// stereo-compensation coefficient). Slot < 0 = unset. Finite m_OverrideCB_Value
	// activates (NaN sentinel = unset so we can distinguish "0.0 is desired"
	// from "not configured").
	int					m_OverrideCB_Slot;
	int					m_OverrideCB_OffsetBytes;
	float				m_OverrideCB_Value;
};

typedef std::pair<DWORD, ShaderProfileData>				ShaderProfileDataWithCRC;
typedef boost::unordered_map<DWORD, ShaderProfileData>	ShaderProfileDataMap;

class ProfileData
{
public:
	ResourcesCRCMultiplierMapBySize	MeshCRCMultiplier;
	ResourcesCRCMultiplierMapBySize	TextureCRCMultiplier;
	ResourceProfileData				WeaponMultiplier;
	float							WeaponMaxZ;
	boost::unordered_set<float>		MonoRHWValues;
	ShaderProfileDataMap			VSCRCData;
	ShaderProfileDataMap			GSCRCData;
	ShaderProfileDataMap			PSCRCData;
	std::list<std::string>			AdditionalMatrixName;
	// <Profile> elements no other reader handles, by name, for the DX11 features; a repeat merges over (the profile over its <Engine>).
	// Kept last: the DX9 wrapper reads this object across the DLL boundary, so the offsets above must not move.
	std::map<std::string, std::map<std::string, std::string> >	FeatureEntries;

	void clear();
};

extern S3DAPI_API ProfileData	_g_ProfileData;

extern ProfileData& g_ProfileData;
