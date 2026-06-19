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

class S3DMarkedOutput :
	public PostprocessedOutput
{
public:
	S3DMarkedOutput(DWORD mode, DWORD spanMode);
	virtual ~S3DMarkedOutput(void);

	virtual HRESULT InitializeResources();
	virtual HRESULT InitializeSCResources(CBaseSwapChain* pSwapChain);
	virtual HRESULT SetStates(CBaseSwapChain* pSwapChain);
	virtual void	ReadConfigData(const char* configXml);
	virtual HRESULT Output(bool bLeft, CBaseSwapChain* pSwapChain, RECT* pDestRect);

	virtual DWORD SetNextSubMode();
	virtual const TCHAR* GetSubModeName();
	virtual void ProcessSubMode();
	virtual void ClearResources();
	virtual bool FindAdapter( LPDIRECT3D9 pDirect3D9, UINT DeviceNum, 
		D3DDEVTYPE &DeviceType, UINT Adapters[2] );

protected:
	std::vector<CAdapt<CComPtr<IDirect3DTexture9>>>	m_Tables;
	CComPtr<IDirect3DTexture9>	m_pFrontTable;
	CComPtr<IDirect3DTexture9>	m_pBackTable;
	std::vector<std::wstring>	m_TablesNames;
	bool m_bTableMethod;
	BOOL m_bAngleCorrection;
	BOOL m_bCheckersMode;

	UINT						m_MarkerIndex;
	UINT						m_NumberOfMarks;
	UINT						m_NumberOfMarkedLines;
	UINT						m_MarkerSurfaceWidth;
	UINT						m_InverseAxisYDirection;
	UINT						m_ScaleToFrame;
	float						m_ScaleFactor;
	CComPtr<IDirect3DSurface9>	m_pMarkerSurface;

	void ParseVersion_0_2(TiXmlHandle& frameHandle, const char* &format, std::vector< std::vector<DWORD> > &LeftMark, std::vector< std::vector<DWORD> > &RightMark);
	void ParseVersion_0_3(TiXmlHandle& frameHandle);
};

}
