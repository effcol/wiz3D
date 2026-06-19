/* 
* Project : iZ3D Stereo Driver
* Copyright (C) iZ3D Inc. 2002 - 2010
*/

#pragma once

#include "PostprocessedOutput_dx9.h"
#include <vector>

class TiXmlHandle;

namespace DX9Output
{

class S3DOutput :
	public PostprocessedOutput
{
public:
	S3DOutput(DWORD mode, DWORD spanMode);
	virtual ~S3DOutput(void);

	virtual DWORD InitSecondBBColor() { return 0xFFBBBBBB; }
	virtual UINT GetOutputChainsNumber();
	virtual HRESULT InitializeResources();
	virtual HRESULT InitializeSCResources(CBaseSwapChain* pSwapChain);
	virtual HRESULT SetStates(CBaseSwapChain* pSwapChain);
	virtual void	ReadConfigData(const char* configXml);

	virtual DWORD SetNextSubMode();
	virtual const TCHAR* GetSubModeName();
	virtual void ProcessSubMode();
	virtual void ClearResources();
	virtual bool FindAdapter( LPDIRECT3D9 pDirect3D9, UINT DeviceNum, 
		D3DDEVTYPE &DeviceType, UINT Adapters[2] );

protected:
	float cDefFrontColor[4]; 
	std::vector<CAdapt<CComPtr<IDirect3DTexture9>>>	m_Tables;
	CComPtr<IDirect3DTexture9>	m_pFrontTable;
	CComPtr<IDirect3DTexture9>	m_pBackTable;
	std::vector<std::wstring>	m_TablesNames;
	void SetDefFrontColor();
	bool m_bTableMethod;
	BOOL m_bAngleCorrection;
	BOOL m_bCheckersMode;
	bool IsCFLMode();
	bool m_bIsD3D9Ex;
};

}
