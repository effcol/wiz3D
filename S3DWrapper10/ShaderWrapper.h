#pragma once

#include "UMHandleWrapper.h"
#include <bitset>
#include <boost/unordered_map.hpp>
#include "..\ShaderAnalysis\AnalyzeData.h"
#include "..\ShaderAnalysis\modify.h"
#include "..\ShaderAnalysis\ShaderInstruction.h"

struct ShaderStaticData;
class ShaderProfileData;
typedef boost::unordered_map<DWORD, ShaderProfileData>	ShaderProfileDataMap;

struct ModifiedShaderData;
class ShaderWrapper : public UMHandleWrapper<D3D10DDI_HSHADER>
{
public:
	ShaderWrapper();

	void InitializePS(D3DDeviceWrapper* pWrapper, CONST UINT*  pCode, 
		CONST D3D10DDIARG_STAGE_IO_SIGNATURES*  pSignatures);
	void InitializeVS(D3DDeviceWrapper* pWrapper, CONST UINT*  pCode, D3D10DDI_HRTSHADER hRTShader, 
		CONST D3D10DDIARG_STAGE_IO_SIGNATURES*  pSignatures);
	void InitializeDS(D3DDeviceWrapper* pWrapper, CONST UINT*  pCode, 
		CONST D3D11DDIARG_TESSELLATION_IO_SIGNATURES*  pSignatures);
	void InitializeHS(D3DDeviceWrapper* pWrapper, CONST UINT*  pCode, 
		CONST D3D11DDIARG_TESSELLATION_IO_SIGNATURES*  pSignatures);
	void InitializeCS(D3DDeviceWrapper* pWrapper, CONST UINT*  pCode);
	void InitializeGS(D3DDeviceWrapper* pWrapper, CONST UINT*  pCode, 
		CONST D3D10DDIARG_STAGE_IO_SIGNATURES*  pSignatures);
	void InitializeGSWithSO(D3DDeviceWrapper* pWrapper, 
		CONST D3D10DDIARG_CREATEGEOMETRYSHADERWITHSTREAMOUTPUT*  pCreateGeometryWithShaderOutput, 
		CONST D3D10DDIARG_STAGE_IO_SIGNATURES*  pSignatures);
	void InitializeGSWithSO(D3DDeviceWrapper* pWrapper, 
		CONST D3D11DDIARG_CREATEGEOMETRYSHADERWITHSTREAMOUTPUT*  pCreateGeometryWithShaderOutput, 
		CONST D3D10DDIARG_STAGE_IO_SIGNATURES*  pSignatures);
	void Destroy( D3DDeviceWrapper* pWrapper );
	D3D10DDI_HSHADER GetHandle()
	{	D3D10DDI_HSHADER hOrig = { ((BYTE*)this + sizeof(ShaderWrapper)) }; return hOrig;	}

	bool IsMatrixFounded()	{ return !m_ProjectionData.matrixData.cb.empty() || !m_UnprojectionData.matrixData.cb.empty(); }
	bool IsShaderModified()	{ return m_ModifiedShaderData.ModifiedShaderAvailable; }

	D3D10DDI_SHADERUNITTYPE m_ShaderType;
	DWORD					m_CRC32;
	DWORD					m_ShaderIndex;
	ProjectionShaderData	m_ProjectionData;
	UnprojectionShaderData	m_UnprojectionData;
	//--- we don't need to cache ModifiedShaderData, because we don't cache modified shader ---
	//--- and generate ones data again while modification process  ---
	ModifiedShaderData		m_ModifiedShaderData;
	std::vector<UINT>		m_pCode;

	// 0.5.0: ungated. ShaderWrapper.cpp dumps this bitset into every shader
	// entry so DumpVS/DumpPS output includes the real texture-slot mask (was
	// previously all-zero in release because the member didn't exist).
	std::bitset<128>		m_UserResourcesMask;

private:
	void ProcessShader( ShaderStaticData& data, CONST UINT* pCode, CONST D3D10DDIARG_STAGE_IO_SIGNATURES* pSignatures );
	void ProcessStereoShader( CONST UINT* pCode, const shader_analyzer::TShaderList& shList, const D3D10DDIARG_SIGNATURE_ENTRY* pOutputSignature, 
		shader_analyzer::TShaderList& modifiedShader, const ShaderProfileData* profile );
	CONST UINT* LookForFullShaderCode(CONST UINT* pCode, SIZE_T& fullSize);
	const ShaderProfileData* FindProfile(ShaderProfileDataMap &privateDataMap);
	void DumpShaderText( ShaderStaticData& data, CONST UINT* pCode, const shader_analyzer::TShaderList& shList, const shader_analyzer::TShaderList& modifiedShList);
};

#define	ADDITIONAL_SHADER_SIZE				sizeof(ShaderWrapper)
#define	CREATE_SHADER_WRAPPER()				new(pWrp) ShaderWrapper()
#define	GET_SHADER_HANDLE(hShader)			GetOriginalHandle<ShaderWrapper>(hShader)
#define	DESTROY_SHADER_WRAPPER()			pWrp->Destroy(D3D10_GET_WRAPPER()); pWrp->~ShaderWrapper()
