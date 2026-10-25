#include "StdAfx.h"
#include "D3DDeviceWrapper.h"
#include "ResourceWrapper.h"
#include "ShaderWrapper.h"

#include <fstream>
#include <strstream>
#include <xstring>
#include <D3Dcompiler.h>
#include "..\ShaderAnalysis\Disasm.h"
#include "..\S3DAPI\ShaderProfileData.h"
#include "Utils.h"

namespace {
	
	SHADER_PIPELINE umST2SP[] = 
	{
		SP_NONE,
		SP_GS,
		SP_VS,
		SP_PS,
		SP_HS,
		SP_DS,
		SP_CS
	};

}

typedef boost::unordered_map<DWORD, ShaderData> CacheMap;
struct ShaderStaticData 
{
	ShaderStaticData(TCHAR* filePostfix, bool* bVal, ShaderProfileDataMap* pdataMap) 
		: Count(0), FilePostfix(filePostfix), pDump(bVal), pDataMap(pdataMap) {}
	UINT		Count;
	CacheMap	CachedAnalyze;
	TCHAR*		FilePostfix;
	bool*		pDump;
	ShaderProfileDataMap* pDataMap;
};

// 0.4.2: ungated. Was #ifndef FINAL_RELEASE → NULL under the shipping
// config, so DumpVS=1 in Config / WIZ3D_FORCE_DUMP=1 silently did nothing
// on DX10/11 release builds. Dump path is driven by gData.DumpDirectory
// (<DriverDir>\wiz3D_Shaders\<Game>\Dump\) which is set up unconditionally
// in S3DWrapperD3D10.cpp::InitDirectories.
static ShaderStaticData g_VS(L"VS", &gInfo.DumpVS, &g_ProfileData.VSCRCData);
static ShaderStaticData g_HS(L"HS", &gInfo.DumpVS, NULL);
static ShaderStaticData g_DS(L"DS", &gInfo.DumpVS, NULL);
static ShaderStaticData g_GS(L"GS", &gInfo.DumpVS, &g_ProfileData.GSCRCData);
static ShaderStaticData g_PS(L"PS", &gInfo.DumpPS, &g_ProfileData.PSCRCData);
static ShaderStaticData g_CS(L"CS", &gInfo.DumpPS, NULL);

ShaderWrapper::ShaderWrapper() 
{ 
	m_ShaderType = D3D10DDISHADERUNITTYPE_UNDEFINED;
	m_ModifiedShaderData.ModifiedShaderAvailable = false; 
	m_ModifiedShaderData.ModifiedShaderHandle.pDrvPrivate = NULL;
}

const ShaderProfileData* ShaderWrapper::FindProfile(ShaderProfileDataMap &privateDataMap)
{
	ShaderProfileDataMap::const_iterator i = privateDataMap.find(m_CRC32);
	if (i != privateDataMap.end())
		return &i->second;
	i = privateDataMap.find(0);
	if (i != privateDataMap.end())
		return &i->second;
	return NULL;
}

void ShaderWrapper::ProcessShader( ShaderStaticData& data, CONST UINT* pCode, CONST D3D10DDIARG_STAGE_IO_SIGNATURES* pSignatures )
{
	const D3D10DDIARG_SIGNATURE_ENTRY* pOutputSignature;
	shader_analyzer::TShaderList shaderList;
	shader_analyzer::TShaderList modifiedShaderList;
	bool signatureFounded = shader_analyzer::FindOutputSignature(pSignatures, pOutputSignature);
	if(signatureFounded || GINFO_DEBUG)
	{
		shader_analyzer::ParseShader(pCode, shaderList);
	}
	
	const ShaderProfileData* profile = NULL;
	if (data.pDataMap)
		profile = FindProfile(*data.pDataMap);

	bool bStereo = true;
	if (profile && profile->m_Multiplier == 0.0f) {
		bStereo = false;
	}

	CacheMap::iterator it = data.CachedAnalyze.find(m_CRC32);
	if (it == data.CachedAnalyze.end())
	{
		//--- the same index possible with cached one.  bug!  ---
		if( bStereo && (signatureFounded || GINFO_DEBUG) ) // cache bug! we MUST cache matrices in spite off from profile->m_Multiplier !
		{
			shader_analyzer::GetProjectionMatrices( pOutputSignature, m_ProjectionData, shaderList );
		}

#ifndef FINAL_RELEASE
		shader_analyzer::GetUsedResourcesMask( shaderList, m_UserResourcesMask );
#endif
		
#if 0
		if (0) // Assassin's Creed matrix
		{		
			m_ProjectionData.matrixData.clear();		
			MatrixData md;		
			md.matrixRegister = 0;		
			md.incorrectProjection = false;		
			md.matrixIsTransposed = true;		
			m_ProjectionData.matrixData.cb[6].push_back(md);
			m_ProjectionData.matrixData.mask = 1 << 6;
		}
		if (0 && m_ProjectionData.CRC32 == 0x298547D5) // DMC4 matrix
		{
			m_ProjectionData.matrixData.clear();
		}
#endif 
		auto& cacheData = data.CachedAnalyze[m_CRC32];
		cacheData.projectionShaderData	= m_ProjectionData;
#ifndef FINAL_RELEASE
		cacheData.userResourcesMask		= m_UserResourcesMask;
#endif
	}
	else
	{
		m_ProjectionData	= it->second.projectionShaderData;
#ifndef FINAL_RELEASE
		m_UserResourcesMask = it->second.userResourcesMask;
#endif
	}

	if (bStereo)
		ProcessStereoShader(pCode, shaderList, signatureFounded ? pOutputSignature : NULL, modifiedShaderList, profile);

	// --- print shader if not cached ---
#ifndef FINAL_RELEASE
	if (it == data.CachedAnalyze.end())
	{
		if (signatureFounded || GINFO_DEBUG)
		{
			if (data.pDump && *data.pDump && data.FilePostfix)
				DumpShaderText(data, pCode, shaderList, modifiedShaderList);
		}
	}		
#endif
}

void ShaderWrapper::ProcessStereoShader( CONST UINT* pCode, const shader_analyzer::TShaderList& shList, 
	const D3D10DDIARG_SIGNATURE_ENTRY* pOutputSignature, shader_analyzer::TShaderList& modifiedShader, const ShaderProfileData* profile )
{
	bool bModifyShader = false;
	bool AddZNearCheck = false;
	if (profile)
	{
		if (profile->m_bModifyShader)
		{
			bModifyShader							= true;
			AddZNearCheck							= profile->m_bAddZNearCheck;
			m_ModifiedShaderData.Multiplier			= profile->m_Multiplier;
			m_ModifiedShaderData.ConvergenceShift	= profile->m_ConvergenceShift;
			m_ModifiedShaderData.StereoZNear		= profile->m_ZNear;
		}

		if (profile->m_PreTransformProjectionId != -1)
		{
			D3DXMATRIX projTransform = profile->m_ProjTransform;
			D3DXMATRIX invProjTransform;
			D3DXMatrixInverse(&invProjTransform, NULL, &projTransform);

			// Storing per shader 'preTransformProjection' matrix in every 'MatrixData' is less efficient, but simpler and may become useful
			for(DWORD i = 0; i < m_ProjectionData.matrixData.cb.size(); i++)
			{
				for(DWORD j = 0; j < m_ProjectionData.matrixData.cb[i].size(); j++)
				{
					m_ProjectionData.matrixData.cb[i][j].preTransformProjectionId = profile->m_PreTransformProjectionId;
					m_ProjectionData.matrixData.cb[i][j].projTransform = projTransform;
					m_ProjectionData.matrixData.cb[i][j].invProjTransform = invProjTransform;
				}
			}
		}

		if (profile->m_pMatrices)
		{
			for (DWORD i = 0; i<profile->m_pMatrices->matrixSize; ++i)
			{
				const ShaderMatrixData& shaderMatrixData = profile->m_pMatrices->matrix[i];
				if (shaderMatrixData.inverse)
				{
					UnprojectionMatrixData mData;
					mData.matrixIsTransposed = shaderMatrixData.matrixIsTransposed;
					mData.matrixRegister     = shaderMatrixData.matrixRegister;
					m_UnprojectionData.matrixData.cb[shaderMatrixData.constantBuffer].push_back(mData);
				}
				else
				{
					ProjectionMatrixData mData;
					mData.incorrectProjection = shaderMatrixData.incorrectProjection;
					mData.matrixIsTransposed  = shaderMatrixData.matrixIsTransposed;
					mData.matrixRegister      = shaderMatrixData.matrixRegister;
					m_ProjectionData.matrixData.cb[shaderMatrixData.constantBuffer].push_back(mData);
					m_ProjectionData.matrixData.mask |= (1 << shaderMatrixData.constantBuffer);
				}
			}
		}
	}

	// 0.4.0+: wiz3D_Config.xml -> TryModifyAllShaders=1 extends the per-game flag
	// to a global user-opt-in. Legacy DX10 DDI path (Win11-blocked, mostly) honours
	// it for symmetry with the DX9 and new-DX11 paths.
	if((gInfo.AlwaysModifyShaders || gInfo.TryModifyAllShaders) && m_ProjectionData.matrixData.cb.size())
		bModifyShader = true;

	if(gInfo.ModifyIncorrectProjectionShaders && m_ProjectionData.matrixData.cb.size())
	{
		for(DWORD i = 0; i < m_ProjectionData.matrixData.cb.size(); i++)
			for(DWORD j = 0; j < m_ProjectionData.matrixData.cb[i].size(); j++)
				if (m_ProjectionData.matrixData.cb[i][j].incorrectProjection)
				{
					bModifyShader = true;
					break;
				}
	}
	if (m_ShaderType != D3D10DDISHADERUNITTYPE_VERTEX)
		bModifyShader = false;

	//--- if we wish to handle another shader types, we MUST modify appropriate CreateXXXShader command ---
	if( pOutputSignature != NULL && bModifyShader )
	{
		if(ModifyShader(shList, pOutputSignature->Register, AddZNearCheck, modifiedShader, m_ModifiedShaderData))
		{
			shader_analyzer::ShaderList2Blob( modifiedShader, pCode[0], m_pCode, pCode[1] + 32 );

			m_ModifiedShaderData.ModifiedShaderAvailable = true;
		}
	}
}

void ShaderWrapper::InitializePS( D3DDeviceWrapper* pWrapper, CONST UINT* pCode, CONST D3D10DDIARG_STAGE_IO_SIGNATURES* pSignatures )
{
	m_ShaderType = D3D10DDISHADERUNITTYPE_PIXEL;
	m_ShaderIndex = g_PS.Count++;
	m_CRC32 = CalculateCRC32(pCode, sizeof(DWORD) * pCode[1]);
	ProcessShader(g_PS, pCode, pSignatures);
}

void ShaderWrapper::InitializeVS( D3DDeviceWrapper* pWrapper, CONST UINT* pCode, D3D10DDI_HRTSHADER hRTShader, CONST D3D10DDIARG_STAGE_IO_SIGNATURES* pSignatures )
{
	m_ShaderType = D3D10DDISHADERUNITTYPE_VERTEX;
	m_ShaderIndex = g_VS.Count++;
	m_CRC32 = CalculateCRC32(pCode, sizeof(DWORD) * pCode[1]);
	ProcessShader(g_VS, pCode, pSignatures);

	if( IsShaderModified() )
	{
		SIZE_T size = pWrapper->OriginalDeviceFuncs.pfnCalcPrivateShaderSize(pWrapper->hDevice, &m_pCode[0], pSignatures);

		NEW_HANDLE(m_ModifiedShaderData.ModifiedShaderHandle, size);

		pWrapper->OriginalDeviceFuncs.pfnCreateVertexShader(pWrapper->hDevice, 
			&m_pCode[0], m_ModifiedShaderData.ModifiedShaderHandle, hRTShader, pSignatures);
	}
}

void ShaderWrapper::InitializeGS( D3DDeviceWrapper* pWrapper, CONST UINT* pCode, CONST D3D10DDIARG_STAGE_IO_SIGNATURES* pSignatures )
{
	m_ShaderType = D3D10DDISHADERUNITTYPE_GEOMETRY;
	m_ShaderIndex = g_GS.Count++;
	m_CRC32 = CalculateCRC32(pCode, sizeof(DWORD) * pCode[1]);
	ProcessShader(g_GS, pCode, pSignatures);
}

void ShaderWrapper::InitializeGSWithSO( D3DDeviceWrapper* pWrapper, CONST D3D10DDIARG_CREATEGEOMETRYSHADERWITHSTREAMOUTPUT* pCreateGeometryWithShaderOutput, CONST D3D10DDIARG_STAGE_IO_SIGNATURES* pSignatures )
{
	m_ShaderType = D3D10DDISHADERUNITTYPE_GEOMETRY;
	CONST UINT* pCode = pCreateGeometryWithShaderOutput->pShaderCode;
	if (pCode)
	{
		m_ShaderIndex = g_GS.Count++;
		m_CRC32 = CalculateCRC32(pCode, sizeof(DWORD) * pCode[1]);
		ProcessShader(g_GS, pCode, pSignatures);
	}
}

void ShaderWrapper::InitializeGSWithSO( D3DDeviceWrapper* pWrapper, CONST D3D11DDIARG_CREATEGEOMETRYSHADERWITHSTREAMOUTPUT* pCreateGeometryWithShaderOutput, CONST D3D10DDIARG_STAGE_IO_SIGNATURES* pSignatures )
{
	m_ShaderType = D3D10DDISHADERUNITTYPE_GEOMETRY;
	CONST UINT* pCode = pCreateGeometryWithShaderOutput->pShaderCode;
	if (pCode)
	{
		m_ShaderIndex = g_GS.Count++;
		m_CRC32 = CalculateCRC32(pCode, sizeof(DWORD) * pCode[1]);
		ProcessShader(g_GS, pCode, pSignatures);
	}
}

void ShaderWrapper::InitializeDS( D3DDeviceWrapper* pWrapper, CONST UINT* pCode, CONST D3D11DDIARG_TESSELLATION_IO_SIGNATURES* pSignatures )
{
	m_ShaderType = D3D11DDISHADERUNITTYPE_DOMAIN;
	m_ShaderIndex = g_DS.Count++;
	m_CRC32 = CalculateCRC32(pCode, sizeof(DWORD) * pCode[1]);
	ProcessShader(g_DS, pCode, (D3D10DDIARG_STAGE_IO_SIGNATURES*)pSignatures);
}

void ShaderWrapper::InitializeHS( D3DDeviceWrapper* pWrapper, CONST UINT* pCode, CONST D3D11DDIARG_TESSELLATION_IO_SIGNATURES* pSignatures )
{
	m_ShaderType = D3D11DDISHADERUNITTYPE_HULL;
	m_ShaderIndex = g_HS.Count++;
	m_CRC32 = CalculateCRC32(pCode, sizeof(DWORD) * pCode[1]);
#ifndef FINAL_RELEASE
	ProcessShader(g_HS, pCode, (D3D10DDIARG_STAGE_IO_SIGNATURES*)pSignatures);
#endif
}

void ShaderWrapper::InitializeCS( D3DDeviceWrapper* pWrapper, CONST UINT* pCode )
{
	m_ShaderType = D3D11DDISHADERUNITTYPE_COMPUTE;
	m_ShaderIndex = g_CS.Count++;
	m_CRC32 = CalculateCRC32(pCode, sizeof(DWORD) * pCode[1]);
#ifndef FINAL_RELEASE
	ProcessShader(g_CS, pCode, NULL);
#endif
}

void ShaderWrapper::Destroy( D3DDeviceWrapper* pWrapper )
{
	if (m_ShaderType != D3D10DDISHADERUNITTYPE_UNDEFINED) 
	{
		SHADER_PIPELINE sp = umST2SP[m_ShaderType];
		if (Commands::Command::CurrentShaderData_[sp] == &m_ProjectionData) { 
			Commands::Command::CurrentShaderData_[sp] = 0;
		}
		if (Commands::Command::CurrentPixelShaderData_[sp] == &m_UnprojectionData) { 
			Commands::Command::CurrentPixelShaderData_[sp] = 0;
		}
		if (Commands::Command::CurrentModifiedShaderData_[sp] == &m_ModifiedShaderData) { 
			Commands::Command::CurrentModifiedShaderData_[sp] = 0;
		}
	}

	if( IsShaderModified() )
	{
		pWrapper->OriginalDeviceFuncs.pfnDestroyShader(pWrapper->hDevice, m_ModifiedShaderData.ModifiedShaderHandle);
		DELETE_HANDLE(m_ModifiedShaderData.ModifiedShaderHandle);
	}
}

CONST UINT* ShaderWrapper::LookForFullShaderCode(CONST UINT* pCode, SIZE_T& fullSize)
{
	// look for full shader code
	const DWORD sig = MAKEFOURCC('D','X','B','C');

	CONST UINT *base = pCode;
	MEMORY_BASIC_INFORMATION mbi;
	if ( VirtualQuery(pCode, &mbi, sizeof(MEMORY_BASIC_INFORMATION)) == sizeof(MEMORY_BASIC_INFORMATION) )
	{
		base = (CONST UINT *) mbi.BaseAddress;
	}

	CONST UINT *p = pCode;
	fullSize = 0;
	for(int i = 0; *p != sig; i++)
	{
		if (i > 10000 || p <= base)	
		{ 
			// give up
			p = pCode;
			return NULL;
		}
		p--;
	}

	fullSize = p == pCode ? pCode[1] * 4 : *(p + 6);
	return p;	
}

void ShaderWrapper::DumpShaderText( ShaderStaticData &data, CONST UINT* pCode, const shader_analyzer::TShaderList& shList, const shader_analyzer::TShaderList& modifiedShList )
{
#ifndef FINAL_RELEASE
	UINT version = pCode[0];
	std::wstring fullPath = gData.DumpDirectory;
	fullPath += L"\\";
	fullPath += data.FilePostfix;
	fullPath += L".txt";
	std::ofstream disShader;
	if (m_ShaderIndex == 0)
		disShader.open( fullPath.c_str() );
	else
		disShader.open( fullPath.c_str(), std::ios_base::out | std::ios_base::app );


	disShader << ";*******************************************************************************" << std::endl;
	disShader << "; Shader #" << m_ShaderIndex << std::endl << std::endl;

	const char* versionToString[] = { "ps", "vs", "gs", "hs", "ds", "cs" };
	disShader << "Shader Version: " << versionToString[DECODE_D3D10_SB_TOKENIZED_PROGRAM_TYPE(version)]
									<< "_" << DECODE_D3D10_SB_TOKENIZED_PROGRAM_MAJOR_VERSION(version) 
									<< "_" << DECODE_D3D10_SB_TOKENIZED_PROGRAM_MINOR_VERSION(version) << std::endl;

	disShader << "CRC32: 0x" << std::hex << std::uppercase << m_CRC32 << std::dec << std::endl << std::endl;

	size_t cnt;
	for (cnt = 127; cnt > 0; cnt--) // at least symbol should be printed
		if (m_UserResourcesMask.test(cnt))
			break;
	cnt++;
	std::string str;
	str.reserve(cnt);
	for (size_t pos = 0; pos < cnt; pos++)
		if (m_UserResourcesMask.test(pos))
			str += "1";
		else
			str += "0";
	disShader << "Resources Mask: " << str << std::endl;

	if (!m_ProjectionData.matrixData.cb.empty())
		shader_analyzer::PrintMatrixData(m_ProjectionData, disShader);
	else
		disShader << "Shader is mono" << std::endl;

	disShader << std::endl;

	SIZE_T shaderSize = 0;
	CONST UINT* fullCode = LookForFullShaderCode(pCode, shaderSize);
	if (fullCode)
	{
		CComPtr<ID3D10Blob> pDisassembly;
		HRESULT hr = D3DDisassemble(fullCode, shaderSize, D3D_DISASM_ENABLE_DEFAULT_VALUE_PRINTS, NULL, &pDisassembly);
		if (pDisassembly)
		{
			disShader << (LPCSTR)pDisassembly->GetBufferPointer() << std::endl;
			disShader << "iZ3D shader disassembler:" << std::endl;
		}
	}

	//if( pOutputSignature )
	//{
	//	boost::GraphvizDigraph vizGraph;
	//	GraphVizShaderTree( graph, vizGraph );
	//	boost::write_graphviz( disShader, vizGraph );
	//}
	shader_analyzer::PrintShader(shList, disShader);
	
	if ( !modifiedShList.empty() )
	{
		disShader << "//////////////////////////////" << std::endl;
		disShader << "Modified shader:" << std::endl;
		shader_analyzer::PrintShader(modifiedShList, disShader);
	}

	disShader << std::endl;
#endif
}
