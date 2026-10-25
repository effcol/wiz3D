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
		m_PreTransformProjectionId(-1)
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
