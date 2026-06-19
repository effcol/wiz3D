/* 
* Project : iZ3D Stereo Driver
* Copyright (C) iZ3D Inc. 2002 - 2010
*/

#pragma once

#include <atlbase.h>
#include <d3d9.h>
#include "..\S3DWrapper9\BaseStereoRenderer.h"
#include "OutputData.h"

#pragma warning( disable : 4100 )

class TiXmlNode;

namespace DX9Output{

class OutputMethod
{
public:
	OutputMethod(DWORD mode, DWORD spanMode);
	virtual ~OutputMethod(void);

	virtual void ModifyPresentParameters(IDirect3D9* pd3d, UINT nAdapter, D3DPRESENT_PARAMETERS* pPresentationParameters);
	virtual HRESULT Initialize(IDirect3DDevice9* dev, bool MultiPass = false);
	virtual HRESULT InitializeSCData(CBaseSwapChain* pSwapChain);
	virtual void Clear();
	virtual void StereoModeChanged(bool bNewMode) { }
	virtual HRESULT Output(CBaseSwapChain* pSwapChain) { return D3DERR_INVALIDCALL; }
	virtual HRESULT Output(bool bLeft, CBaseSwapChain* pSwapChain, RECT* pDestRect) { return D3DERR_INVALIDCALL; }
	virtual void AfterPresent(bool bLeft) { }

	virtual bool FindAdapter( LPDIRECT3D9 pDirect3D9, UINT DeviceNum, D3DDEVTYPE &DeviceType, UINT Adapters[2] );
	virtual UINT GetOutputChainsNumber() { return 1; }
	virtual DWORD InitSecondBBColor() { return 0xFF000000; }

	virtual int   GetPreferredRefreshRate() const { return 0; }
	virtual DWORD GetSecondWindowCaps() { return 0; }
	virtual DWORD SetNextSubMode() { return m_SubMode; }
	virtual const TCHAR* GetSubModeName() { return _T(""); }
	// configXml is the output method's config subtree serialized to a string by
	// S3DAPI (GetOutputConfigXml()). Overrides re-parse it with their OWN parser
	// so no TiXml object crosses the S3DAPI/OutputMethod DLL boundary (the two
	// sides link incompatible TinyXML/ticpp layouts). configXml is never NULL
	// when this is called (the caller guards), but may be an empty document.
	virtual void  ReadConfigData(const char* configXml) { }
	bool HandleGamma()	{ return m_bProcessGammaRamp; }
	DWORD GetSubMode()	{ return m_SubMode; }
	DWORD GetCaps()		{ return m_Caps; }
	IDirect3DTexture9* GetLogo()		{ return m_pLogo; }

	int GetTrialDaysLeft();

protected:
	DWORD						m_OutputMode;
	DWORD						m_SpanMode;
	DWORD						m_SubMode;
	DWORD						m_Caps;
	CComPtr<IDirect3DDevice9>	m_pd3dDevice;
	CComPtr<IDirect3DTexture9>	m_pLogo;
	bool						m_bProcessGammaRamp; // should be changed only in constructor
	bool						m_bTrialMode;
};

}

#pragma warning( default : 4100 )
