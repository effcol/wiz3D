#include "StdAfx.h"
#include "ConstantBufferWrapper.h"
#include "D3DDeviceWrapper.h"
#include "ShaderWrapper.h"

D3D10DDI_HRESOURCE UnwrapCurrentViewCBHandle(D3DDeviceWrapper* pWrapper, UINT index, D3D10DDI_HRESOURCE handle, SHADER_PIPELINE shaderType )
{
	D3D10DDI_HRESOURCE hRes;
	if (handle.pDrvPrivate)
	{
		// looks greasy, wants refactoring
		bool bMono = false;
		if ( (!Commands::Command::CurrentShaderData_[shaderType] || ((Commands::Command::CurrentShaderData_[shaderType]->matrixData.mask & (1 << index)) == 0))
			 && !Commands::Command::CurrentPixelShaderData_[shaderType]
			 && !Commands::Command::CurrentModifiedShaderData_[shaderType] )
		{
			bMono = true;
		}

		if (Commands::Command::CurrentShaderData_[shaderType])
		{
			DEBUG_TRACE3( L"stereo shader - %S\n", EnumToString(shaderType) );
			DEBUG_TRACE3( L"shader data - %d\n", (Commands::Command::CurrentShaderData_[shaderType] != 0) );
			DEBUG_TRACE3( L"matrix data mask - %d\n", Commands::Command::CurrentShaderData_[shaderType]->matrixData.mask );
		}

		ResourceWrapper* pWrp = ((ResourceWrapper*)handle.pDrvPrivate);
		if (pWrp->m_pCB == NULL)
		{
			DEBUG_TRACE3(L"mono CB [%p]\n", handle.pDrvPrivate);
			return pWrp->GetHandle();
		}
		else 
		{
			VIEWINDEX vi = bMono ? VIEWINDEX_MONO : Commands::Command::CurrentView_;
			DEBUG_TRACE3(L"current mode - %S [%p]\n", EnumToString(vi), handle.pDrvPrivate);
			return pWrp->m_pCB->GetHandle(pWrapper, vi);
		}
	}
	
	hRes.pDrvPrivate = NULL;
	DEBUG_TRACE3(L"NULL\n", handle.pDrvPrivate);
	return hRes;
}

inline const D3DXMATRIX& matrixFromRegister(const FLOAT* pData, unsigned reg)
{
	return *reinterpret_cast<const D3DXMATRIX*>(&pData[reg]);
}

/** Update application perspective matrices */
class ProjectionMatrixModifier :
	public ShaderConstantModifier
{
public:
	ProjectionMatrixModifier(const ProjectionCBMatrices& matrices, size_t elementCount)
	:	m_InverseLR(false)
	,	m_ShaderProjectionMatrices(matrices)
	,	m_ElementCount(elementCount)
	{
	}

	// Override ShaderConstantModifier
	bool CompareData(const CBUpdateData& updateData)
	{
		const ProjectionCBMatrices* matrices = boost::get<ProjectionCBMatrices>(&updateData);
		if (!matrices) {
			return false;
		}

		return m_ShaderProjectionMatrices == (*matrices);
	}

	bool CheckMatrices(D3DDeviceWrapper* pWrapper, const FLOAT* pData)
	{
		const ProjectionMatrixData& matrix = m_ShaderProjectionMatrices.front();
		DWORD registerIndex = matrix.matrixRegister * 4;
		if (registerIndex >= m_ElementCount) {
			return false;
		}

		const D3DXMATRIX& CurrentShaderMatrix = matrixFromRegister(pData, registerIndex);
		if (matrix.matrixIsTransposed) {
			return CheckMatricesTransposed(pData, CurrentShaderMatrix);
		}
		else {
			return CheckMatricesNonTransposed(pData, CurrentShaderMatrix);
		}
	}

	void UpdateMatrices(D3DDeviceWrapper* pWrapper, VIEWINDEX vi, const FLOAT* pMonoData, FLOAT* pData)
	{
		DEBUG_TRACE3(_T("Transforming projection matrix:\n"));

		if (m_InverseLR) {
			vi = VIEWINDEX(1 - vi); // left <-> right
		}
		float a = pWrapper->GetTransformationMatrix(vi)._31;
		float b = pWrapper->GetTransformationMatrix(vi)._41;

		for (DWORD i = 0; i < m_ShaderProjectionMatrices.size(); i++)
		{
			DWORD registerIndex = m_ShaderProjectionMatrices[i].matrixRegister * sizeof(float);
			if (registerIndex + 16 > m_ElementCount)
			{
				// Probably shader analyzer bug
				ZLOG_MESSAGE(zlog::SV_WARNING, _T("Elements (%d..%d) less than ElementCount (%d)\n"),
					registerIndex, registerIndex + 15, m_ElementCount);
				continue;
			}
			D3DXMATRIX curShaderMatrix = matrixFromRegister(pMonoData, registerIndex);

			bool bTransposed = m_ShaderProjectionMatrices[i].matrixIsTransposed != 0;
			DEBUG_TRACE3(_T("Shader matrix %d (transposed %d):\n"), i, bTransposed); 
			TraceMatrix(&curShaderMatrix, bTransposed);

			// Here we perform perspective matrix transformation:
			// P' = M^-1 * T * M * P, where:
			// T - transforms perspective matrix to left-perspective or right-perspective respectively
			// M - defines transformation between reference frame of projection and normal projection frame 
			// (X, Y axes of the projection frame correspond to the X, Y axes of render target, scaling is identity)
			D3DXMATRIX& stereoMatrix = (D3DXMATRIX&)pData[registerIndex];
			if (m_ShaderProjectionMatrices[i].preTransformProjectionId != -1) {
				D3DXMatrixMultiply(&stereoMatrix, &m_ShaderProjectionMatrices[i].projTransform, &stereoMatrix);
			}

			if(m_ShaderProjectionMatrices[i].matrixIsTransposed)
			{
				stereoMatrix._11 += a*curShaderMatrix._31 + b*curShaderMatrix._41;
				stereoMatrix._12 += a*curShaderMatrix._32 + b*curShaderMatrix._42;
				stereoMatrix._13 += a*curShaderMatrix._33 + b*curShaderMatrix._43;
				stereoMatrix._14 += a*curShaderMatrix._34 + b*curShaderMatrix._44;
			}
			else
			{
				stereoMatrix._11 += a*curShaderMatrix._13 + b*curShaderMatrix._14;
				stereoMatrix._21 += a*curShaderMatrix._23 + b*curShaderMatrix._24;
				stereoMatrix._31 += a*curShaderMatrix._33 + b*curShaderMatrix._34;
				stereoMatrix._41 += a*curShaderMatrix._43 + b*curShaderMatrix._44;
			}

			if (m_ShaderProjectionMatrices[i].preTransformProjectionId != -1) {
				D3DXMatrixMultiply(&stereoMatrix, &m_ShaderProjectionMatrices[i].invProjTransform, &stereoMatrix);
			}
		}
	}

private:
	bool CheckMatricesTransposed(const FLOAT* pData, const D3DXMATRIX& CurrentShaderMatrix)
	{
		DEBUG_TRACE3(_T("\tMatrix for modification:\n"));
		TraceMatrix(&CurrentShaderMatrix, true);
		//--- skip ORTO matrices  ---  
		if (!gInfo.SkipCheckOrthoMatrix && 
			CurrentShaderMatrix._41 == 0.0f && CurrentShaderMatrix._42 == 0.0f && 
			CurrentShaderMatrix._43 == 0.0f && CurrentShaderMatrix._44 != 0.0f ) 
		{
			DEBUG_TRACE3(_T("\tOrtho matrix\n")); 
			return false;
		}

		D3DXMATRIX invCurrentShaderMatrix;
		bool bCalculateInverseMatrix = gInfo.CheckShadowMatrix || gInfo.CheckInverseLR || gInfo.CheckExistenceInverseMatrix;
		float k = (CurrentShaderMatrix._31 + CurrentShaderMatrix._32 + CurrentShaderMatrix._33) / 
			      (CurrentShaderMatrix._41 + CurrentShaderMatrix._42 + CurrentShaderMatrix._43);
		float ZNear = CurrentShaderMatrix._44 - CurrentShaderMatrix._34 / k;
		float ZFar = -k * ZNear / (1 - k);
		DEBUG_TRACE3(_T("Zn %f, Zf %f\n"), ZNear, ZFar);
		
		m_InverseLR = false;
		if (GINFO_DEBUG || bCalculateInverseMatrix)
		{
			D3DXMATRIX* mInv = D3DXMatrixInverse(&invCurrentShaderMatrix, NULL, &CurrentShaderMatrix);
			if (mInv)
			{
				DEBUG_TRACE3(_T("Vertex shader inverse matrix:\n")); 
				TraceMatrix(mInv, true);
				if (fabs(invCurrentShaderMatrix._43) < 0.01f)
				{
					DEBUG_TRACE3(_T("\tPerspective shadow map transformation matrix\n")); 
					if (gInfo.CheckShadowMatrix) {
						return false;
					}
					else {
						DEBUG_TRACE3(_T("Hint: CheckShadowMatrix off\n")); 
					}
				}
				else if (invCurrentShaderMatrix._43 * invCurrentShaderMatrix._44 > 0.0f)
				{
					DEBUG_TRACE3(_T("\tInverse Z %f\n"), (mInv ? invCurrentShaderMatrix._43 * invCurrentShaderMatrix._44 : 1000000.0f)); 
					m_InverseLR = gInfo.CheckInverseLR;
					if (!m_InverseLR) {
						DEBUG_TRACE3(_T("Hint: CheckInverseLR off\n"));
					}
				}
			}
			else
			{
				DEBUG_TRACE3(_T("\tInverse matrix doesn't exist\n")); 
				if (gInfo.CheckExistenceInverseMatrix) {
					return false;
				}
				else {
					DEBUG_TRACE3(_T("Hint: CheckExistenceInverseMatrix off\n")); 
				}
			}
		}

		return true;
	}

	bool CheckMatricesNonTransposed(const FLOAT* pData, const D3DXMATRIX& CurrentShaderMatrix)
	{
		DEBUG_TRACE3(_T("\tMatrix for modification:\n"));
		TraceMatrix(&CurrentShaderMatrix, false);

		//--- skip ORTO matrices  --- 
		if (!gInfo.SkipCheckOrthoMatrix && 
			CurrentShaderMatrix._14 == 0.0f && CurrentShaderMatrix._24 == 0.0f && 
			CurrentShaderMatrix._34 == 0.0f && CurrentShaderMatrix._44 != 0.0f ) 
		{
			DEBUG_TRACE3(_T("\tOrtho matrix\n")); 
			return false;
		}

		D3DXMATRIX invCurrentShaderMatrix;
		bool bCalculateInverseMatrix = gInfo.CheckShadowMatrix || gInfo.CheckInverseLR || gInfo.CheckExistenceInverseMatrix;
		float k = (CurrentShaderMatrix._31 + CurrentShaderMatrix._32 + CurrentShaderMatrix._33) / 
			      (CurrentShaderMatrix._41 + CurrentShaderMatrix._42 + CurrentShaderMatrix._43);
		float ZNear = CurrentShaderMatrix._44 - CurrentShaderMatrix._34 / k;
		float ZFar = -k * ZNear / (1 - k);
		DEBUG_TRACE3(_T("Zn %f, Zf %f\n"), ZNear, ZFar);
		
		m_InverseLR = false;
		if (GINFO_DEBUG || bCalculateInverseMatrix)
		{
			D3DXMATRIX* mInv = D3DXMatrixInverse(&invCurrentShaderMatrix, NULL, &CurrentShaderMatrix);
			if (mInv)
			{
				DEBUG_TRACE3(_T("Vertex shader inverse matrix:\n")); 
				TraceMatrix(mInv, false);
				if (fabs(invCurrentShaderMatrix._34) < 0.01f)
				{
					DEBUG_TRACE3(_T("\tPerspective shadow map transformation matrix\n")); 
					if (gInfo.CheckShadowMatrix) {
						return false;
					}
					else {
						DEBUG_TRACE3(_T("Hint: CheckShadowMatrix off\n")); 
					}
				}
				else if (invCurrentShaderMatrix._34 * invCurrentShaderMatrix._44 > 0.0f)
				{
					DEBUG_TRACE3(_T("\tInverse Z %f\n"), (mInv ? invCurrentShaderMatrix._34 * invCurrentShaderMatrix._44 : 1000000.0f)); 
					m_InverseLR = gInfo.CheckInverseLR;
					if (!m_InverseLR) {
						DEBUG_TRACE3(_T("Hint: CheckInverseLR off\n"));
					}
				}
			}
			else 
			{
				DEBUG_TRACE3(_T("\tInverse matrix doesn't exist\n")); 
				if (gInfo.CheckExistenceInverseMatrix) {
					return false;
				}
				else {
					DEBUG_TRACE3(_T("Hint: CheckExistenceInverseMatrix off\n")); 
				}
			}
		}

		return true;
	}

private:
	bool					m_InverseLR;
	size_t					m_ElementCount;
	ProjectionCBMatrices	m_ShaderProjectionMatrices;
};

/** Update application inverse perspective matrices */
class UnprojectionMatrixModifier :
	public ShaderConstantModifier
{
public:
	UnprojectionMatrixModifier(const UnprojectionCBMatrices& matrices, size_t elementCount)
	:	m_ShaderUnprojectionMatrices(matrices)
	,	m_ElementCount(elementCount)
	{
	}

	// Override ShaderConstantModifier
	bool CompareData(const CBUpdateData& updateData)
	{
		if ( boost::get<UnprojectionCBMatrices>(&updateData) ) {
			return false;
		}

		return m_ShaderUnprojectionMatrices == boost::get<UnprojectionCBMatrices>(updateData);
	}

	bool CheckMatrices(D3DDeviceWrapper* pWrapper, const FLOAT* pData)
	{
		return true;
	}

	void UpdateMatrices(D3DDeviceWrapper* pWrapper, VIEWINDEX vi, const FLOAT* pMonoData, FLOAT* pData)
	{
		DEBUG_TRACE3(_T("Transforming inverse projection matrix:\n"));
		for (DWORD i = 0; i<m_ShaderUnprojectionMatrices.size(); ++i)
		{ 
			DWORD registerIndex = m_ShaderUnprojectionMatrices[i].matrixRegister * sizeof(float);
			if (registerIndex + 16 > m_ElementCount)
			{
				// Probably shader analyzer bug
				ZLOG_MESSAGE(zlog::SV_WARNING, _T("Elements (%d..%d) less than ElementCount (%d)\n"), registerIndex, registerIndex + 15, m_ElementCount);
				continue;
			}

			D3DXMATRIX curShaderMatrix = matrixFromRegister(pMonoData, registerIndex);

			bool bTransposed = m_ShaderUnprojectionMatrices[i].matrixIsTransposed != 0;
			DEBUG_TRACE3(_T("Shader matrix %d (transposed %d):\n"), i, bTransposed); 
			TraceMatrix(&curShaderMatrix, bTransposed);

			// Here we perform inverse perspective matrix transformation:
			// (P^-1)' = P^-1 * T^-1, where:
			// T - transforms perspective matrix to left-perspective or right-perspective respectively
			// (X, Y axes of the projection frame correspond to the X, Y axes of render target, scaling is identity)
			D3DXMATRIX& stereoMatrix = (D3DXMATRIX&)pData[registerIndex];
			D3DXMATRIX  transform    = pWrapper->GetTransformationMatrix(vi);

			DEBUG_TRACE3(_T("transform:\n")); 
			TraceMatrix(&transform, bTransposed);

			D3DXMatrixInverse(&transform, NULL, &transform);
			DEBUG_TRACE3(_T("invTransform:\n")); 
			TraceMatrix(&transform, bTransposed);

			if (m_ShaderUnprojectionMatrices[i].matrixIsTransposed) {
				D3DXMatrixTranspose(&transform, &transform);
			}
			D3DXMatrixMultiply(&stereoMatrix, &stereoMatrix, &transform);
		
			DEBUG_TRACE3(_T("stereoMatrix:\n")); 
			TraceMatrix(&stereoMatrix, bTransposed);
		}
	}

private:
	D3DXMATRIX				m_InverseTransformMatrix;
	size_t					m_ElementCount;
	UnprojectionCBMatrices	m_ShaderUnprojectionMatrices;
};

/** Update modified shader constants */
class ModifiedShaderModifier :
	public ShaderConstantModifier
{
public:
	ModifiedShaderModifier(const ModifiedShaderData& mData)
	:	m_ShaderData(mData)
	{
	}

	// Override ShaderConstantModifier
	bool CompareData(const CBUpdateData& updateData)
	{
		// ModifiedShaderData not comparable
		return false;
	}

	bool CheckMatrices(D3DDeviceWrapper* pWrapper, const FLOAT* pData)
	{
		return true;
	}

	void UpdateMatrices(D3DDeviceWrapper* pWrp, VIEWINDEX vi, const FLOAT* /*pMonoData*/, FLOAT* pData)
	{
		DEBUG_TRACE3(_T("Transforming modified shader constants:\n"));
		
		//--- эти две переменных пока не поддерживаются в DX10 драйвере ---
		float m_CurrentMeshMultiplier = 1.0f;
		float m_CurrentConvergenceShift = 0.0f;
		if (gInfo.VSSeparationUpdateMode == 0) 
		{
			m_CurrentMeshMultiplier = 1.0f;
			m_CurrentConvergenceShift = m_ShaderData.ConvergenceShift;
		}

		// 0.4.3: Add global user ConvergenceShift (Alt+Numpad +/-) on top of
		// per-shader profile shift. Default 0.0f so users who don't press the
		// hotkey see no behaviour change. Mirrors the DX9 wiring in
		// BaseStereoRenderer_Main.cpp AutoDetectStereoSettings.
		m_CurrentConvergenceShift += pWrp->m_Input.ConvergenceShift;

		if (gInfo.VertexShaderModificationMethod < 2)
		{
			float m31 = m_CurrentMeshMultiplier * pWrp->GetTransformationMatrix(vi)._31;
			if (m31 != 0.0f && m_CurrentConvergenceShift != 0.0f)
			{
				float One_div_ZPS = pWrp->m_Input.GetActivePreset()->One_div_ZPS;
				m31 = m31 / One_div_ZPS * (One_div_ZPS + m_CurrentConvergenceShift);
			}
			float m41 = m_CurrentMeshMultiplier * pWrp->GetTransformationMatrix(vi)._41;
			//--- dp4Vector ---
			pData[0] = 1; pData[1] = 0; pData[2] = m31; pData[3] = m41;
			//--- ZNearVector ---
			pData[4] = 0; pData[5] = 0; pData[6] =   0; pData[7] = m_ShaderData.StereoZNear;
		}
		else
		{
			float conv = -(pWrp->m_Input.GetActivePreset()->One_div_ZPS + m_CurrentConvergenceShift);
			float sep = pWrp->m_Input.GetActivePreset()->StereoBase * m_CurrentMeshMultiplier;
			if (vi == VIEWINDEX_LEFT)
				sep *= -1;
			//--- dp4Vector ---
			pData[0] = sep; pData[1] = conv * conv; pData[2] = 0; pData[3] = m_ShaderData.StereoZNear;
		}
	}

private:
	ModifiedShaderData m_ShaderData;
};

D3D10DDI_HRESOURCE UnwrapCBHandle(D3DDeviceWrapper* pWrapper, D3D10DDI_HRESOURCE handle )
{
	D3D10DDI_HRESOURCE hRes;
	if (handle.pDrvPrivate)
	{
		ResourceWrapper* pWrp = ((ResourceWrapper*)handle.pDrvPrivate);
		return pWrp->GetHandle();
	}
	else
		hRes.pDrvPrivate = NULL;
	return hRes;
}

ConstantBufferWrapper::ConstantBufferWrapper( D3DDeviceWrapper* pWrapper, ResourceWrapper* pResourceWrapper )
:	m_pData(NULL)
,	m_pMonoData(NULL)
,	m_bNeedToCheckMatrices(false)
,	m_bHaveStereoMatrices(false)
,	m_ElementCount(0)
,	m_ShaderCRC (0)
{
	m_pResourceWrapper = pResourceWrapper;
	m_ElementCount = m_pResourceWrapper->m_MipInfoList[0].PhysicalWidth / sizeof(FLOAT);
	_ASSERT(m_ElementCount > 0);
	m_bBufferUpdated[VIEWINDEX_LEFT]  = false;
	m_bBufferUpdated[VIEWINDEX_RIGHT] = false;
}

ConstantBufferWrapper::~ConstantBufferWrapper()
{
	if (m_pMonoData)
		_aligned_free(m_pMonoData);
}

bool ConstantBufferWrapper::Initialize( D3DDeviceWrapper* pWrapper, const CBUpdateData& updateData, DWORD shaderCRC )
{
	m_ShaderCRC = shaderCRC;
	if ( m_ShaderConstantModifier && m_ShaderConstantModifier->CompareData(updateData) ) 
	{
		DEBUG_TRACE3( _T("Matrices for modification already updated") );
		return m_pMonoData != NULL;
	}

	if ( boost::get<ProjectionCBMatrices>(&updateData) ) 
	{
		DEBUG_TRACE3( _T("Initializing projection matrices modification") );
		m_ShaderConstantModifier.reset( new ProjectionMatrixModifier(boost::get<ProjectionCBMatrices>(updateData), m_ElementCount) );
	}
	else if ( boost::get<UnprojectionCBMatrices>(&updateData) ) 
	{
		DEBUG_TRACE3( _T("Initializing inverse projection matrices modification") );
		m_ShaderConstantModifier.reset( new UnprojectionMatrixModifier(boost::get<UnprojectionCBMatrices>(updateData), m_ElementCount) );
	}
	else if ( boost::get<ModifiedShaderData>(&updateData) ) 
	{
		DEBUG_TRACE3( _T("Initializing modified shader constants modification") );
		m_ShaderConstantModifier.reset( new ModifiedShaderModifier(boost::get<ModifiedShaderData>(updateData)) );
	}

	m_bBufferUpdated[VIEWINDEX_LEFT]  = false;
	m_bBufferUpdated[VIEWINDEX_RIGHT] = false;
	m_bNeedToCheckMatrices = true;

	return m_pMonoData != NULL;
}

void ConstantBufferWrapper::Map(void* pData) 
{ 
	m_pData = pData; 
}

void ConstantBufferWrapper::Unmap(D3DDeviceWrapper* pWrapper)
{
	if (!m_pMonoData) 
	{
		m_pMonoData = (FLOAT*)_aligned_malloc(sizeof(FLOAT) * m_ElementCount, 16);
		if (!m_pMonoData) {
			ZLOG_MESSAGE( zlog::SV_FATAL_ERROR, _T("Can't allocate memory for mono data in constant buffer wrapper") );
		}
	}
	memcpy(m_pMonoData, m_pData, sizeof(FLOAT) * m_ElementCount);
	m_pData = NULL;
	
	m_bBufferUpdated[VIEWINDEX_LEFT]  = false;
	m_bBufferUpdated[VIEWINDEX_RIGHT] = false;
	m_bNeedToCheckMatrices = true;
}

void ConstantBufferWrapper::UpdateSubresourceUP(D3DDeviceWrapper* pWrapper, const void* pData)
{
	if (!m_pMonoData) 
	{
		m_pMonoData = (FLOAT*)_aligned_malloc(sizeof(FLOAT) * m_ElementCount, 16);
		if (!m_pMonoData) {
			ZLOG_MESSAGE( zlog::SV_FATAL_ERROR, _T("Can't allocate memory for mono data in constant buffer wrapper") );
		}
	}	
	memcpy(m_pMonoData, pData, sizeof(FLOAT) * m_ElementCount);

	m_bBufferUpdated[VIEWINDEX_LEFT]  = false;
	m_bBufferUpdated[VIEWINDEX_RIGHT] = false;
	m_bNeedToCheckMatrices = true;
}

void ConstantBufferWrapper::UpdateMatrices(D3DDeviceWrapper* pWrapper, VIEWINDEX vi, bool mono, FLOAT* pData)
{
	memcpy(pData, m_pMonoData, sizeof(FLOAT) * m_ElementCount);
	if ( !mono && m_ShaderConstantModifier) {
		m_ShaderConstantModifier->UpdateMatrices(pWrapper, vi, m_pMonoData, pData);
	}
	m_bBufferUpdated[vi] = true;
}

D3D10DDI_HRESOURCE ConstantBufferWrapper::GetHandle(D3DDeviceWrapper* pWrapper, VIEWINDEX vi)
{
	if (vi == VIEWINDEX_MONO) {
		return m_pResourceWrapper->GetHandle();
	}

	if (m_pMonoData)
	{
		// search for stereo matrices in buffer
		if (m_bNeedToCheckMatrices) 
		{
			if (!m_ShaderConstantModifier) {
				m_bHaveStereoMatrices = false;
			}
			else 
			{
				m_bHaveStereoMatrices = m_ShaderConstantModifier->CheckMatrices(pWrapper, m_pMonoData);
				m_bNeedToCheckMatrices = false;
			}
		}

		// no projection matrix found or modification method not specified, so do not update matrices
		if (!m_bHaveStereoMatrices) 
		{
			ZLOG_MESSAGE(zlog::SV_FLOOD, _T("CB matrix modification method not defined or CB has no stereo matrix, ignoring matrix modification"));
			return m_pResourceWrapper->GetHandle();
		}

		// create stereo buffers if needed
		if (!m_pResourceWrapper->m_hRightHandle.pDrvPrivate && vi == VIEWINDEX_RIGHT) {
			m_pResourceWrapper->CreateRightResource(pWrapper);
		}

		if (!m_pResourceWrapper->m_hLeftHandle.pDrvPrivate && vi == VIEWINDEX_LEFT) {
			m_pResourceWrapper->CreateLeftResource(pWrapper);
		}
		
		// update matrices
		if (!m_bBufferUpdated[vi]) {
			UpdateCurrentPool(pWrapper, vi, false);
		}
	}
	else {
		ZLOG_MESSAGE(zlog::SV_WARNING, _T("CB has no mono data, modification not applied"));
	}

	return m_pResourceWrapper->GetViewHandle(vi);
}
