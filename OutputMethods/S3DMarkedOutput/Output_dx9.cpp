/* IZ3D_FILE: $Id$ 
*
* Project : iZ3D Stereo Driver
* Copyright (C) iZ3D Inc. 2002 - 2010
*
* $Author$
* $Revision$
* $Date$
* $LastChangedBy$
* $URL$
*/
// Output.cpp : Defines the entry point for the DLL application.
//

#include "stdafx.h"
#include "Output_dx9.h"
#include "S3DWrapper9\BaseSwapChain.h"

#include <tinyxml.h>
#include "ProductNames.h"
#include "..\CommonUtils\StringUtils.h"
#include "..\CommonUtils\CommonResourceFolders.h"

using namespace DX9Output;

#include "Shaders_dx9/shaderS3DTable1st.h"
#include "Shaders_dx9/shaderS3DTable2nd.h"
#include "Shaders_dx9/shaderS3DTableCFL2nd.h"
#include "Shaders_dx9/shaderS3DTableAndGamma1st.h"
#include "Shaders_dx9/shaderS3DTableAndGamma2nd.h"
#include "Shaders_dx9/shaderS3DTableCFLAndGamma2nd.h"

#include "Shaders_dx9/shaderS3DTable.h"
#include "Shaders_dx9/shaderS3DTableCFL.h"
#include "Shaders_dx9/shaderS3DTableAndGamma.h"
#include "Shaders_dx9/shaderS3DTableCFLAndGamma.h"

#include <d3dx9.h>

#include "..\ArchiveFile\blob.h"

using namespace std;

#ifdef _MANAGED
#pragma managed(push, off)
#endif

BOOL APIENTRY DllMain( HMODULE hModule,
					   DWORD  ul_reason_for_call,
					   LPVOID lpReserved
					 )
{
	switch (ul_reason_for_call)
	{
	case DLL_PROCESS_ATTACH:
	case DLL_THREAD_ATTACH:
	case DLL_THREAD_DETACH:
	case DLL_PROCESS_DETACH:
		break;
	}
	return TRUE;
}

#ifdef _MANAGED
#pragma managed(pop)
#endif

OUTPUT_API void* CALLBACK CreateOutputDX9(DWORD mode, DWORD spanMode)
{
	return new S3DMarkedOutput(mode, spanMode);
}

OUTPUT_API DWORD CALLBACK GetOutputCaps()
{
	return odShutterModeSimpleOnly | odResetGDIGammaRamp;
}

OUTPUT_API void CALLBACK GetOutputName(char* name, DWORD size)
{
	strcpy_s(name, size, "S3D 27\" mode");
}

OUTPUT_API BOOL CALLBACK EnumOutputModes(DWORD num, char* name, DWORD size)
{
	switch(num)
	{
	case 0:
		strcpy_s(name, size, "Table");
		return TRUE;
	default:
		return FALSE;
	}
}

S3DMarkedOutput::S3DMarkedOutput(DWORD mode, DWORD spanMode)
: PostprocessedOutput(mode, spanMode)
, m_bAngleCorrection(false)
, m_bCheckersMode(false)
{
	 m_bProcessGammaRamp = true;
}

S3DMarkedOutput::~S3DMarkedOutput(void)
{
}

void S3DMarkedOutput::ClearResources()
{
	m_Tables.clear();
	m_pFrontTable.Release();
	m_pBackTable.Release();
	m_pMarkerSurface.Release();
}

void ParseMarkLine(TiXmlElement* element, vector<vector<DWORD> >& MarkArray)
{
	for(int index = 0; element; index++, element = element->NextSiblingElement())
	{
		MarkArray.push_back(vector<DWORD>());
		if(const char* s = element->GetText())
		{
			const char* t = s;
			UINT length = (UINT)strlen(s);
			while(t < s + length)
			{
				DWORD color;
				sscanf(t, "%x", &color); t+= 7;
				MarkArray[index].push_back(color);  
			}
		}
	}
}

HRESULT S3DMarkedOutput::InitializeResources()
{
	HRESULT hResult = S_OK;

	//marking
	const char* specVersion;
	const char* format = "";
	m_MarkerIndex = 0;
	m_MarkerSurfaceWidth = 0;
	m_NumberOfMarkedLines = 0;
	m_NumberOfMarks = 0;
	m_InverseAxisYDirection = 0;
	m_ScaleToFrame = 0;
	m_ScaleFactor = .75;
	vector< vector<DWORD> > LeftMark, RightMark;
	TCHAR XMLFileName[MAX_PATH];

	hResult = E_FAIL;
	if(iz3d::resources::GetAllUsersDirectory( XMLFileName ))
	{
		PathAppend(XMLFileName, _T("MarkingSpec.xml") );
		TiXmlDocument MarkFile( common::utils::to_multibyte( XMLFileName ).c_str() );
		if(MarkFile.LoadFile())
		{
			TiXmlHandle   docHandle( &MarkFile );
			TiXmlHandle   frameHandle = docHandle.FirstChild("FrameMarkingSpec");
			TiXmlElement* element = frameHandle.Element();
			if(element)
			{
				specVersion = element->Attribute("version");
				if(strcmp(specVersion,"0.2") == 0)
				{
					ParseVersion_0_2(frameHandle, format, LeftMark, RightMark);
					hResult = S_OK;
				}
				if(strcmp(specVersion,"0.3") == 0)
				{
					ParseVersion_0_2(frameHandle, format, LeftMark, RightMark);
					ParseVersion_0_3(frameHandle);
					hResult = S_OK;
				}
			}
		}
	}

	//--- if some bug found while reading XML file - draw red line on top screen line ---
	if(FAILED(hResult))
	{
		m_NumberOfMarkedLines = 16;
		m_NumberOfMarks = 1;
		m_InverseAxisYDirection = 0;
		m_ScaleToFrame = 1;
		m_ScaleFactor = 1;
		LeftMark.clear();  LeftMark.push_back(vector<DWORD>(1, 0xFF0000));
		RightMark.clear(); RightMark.push_back(vector<DWORD>(1, 0xFF0000));
		m_MarkerSurfaceWidth = (UINT)LeftMark[0].size();
	}

	if(SUCCEEDED(m_pd3dDevice->CreateOffscreenPlainSurface(m_MarkerSurfaceWidth, m_NumberOfMarks * 2, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &m_pMarkerSurface, NULL)))
	{
		D3DLOCKED_RECT rect;
		m_pMarkerSurface->LockRect(&rect, NULL, D3DLOCK_DISCARD);
		char* p1 = (char*)rect.pBits;
		char* p2 = p1 + rect.Pitch * m_NumberOfMarks;
		for(UINT i=0; i< m_NumberOfMarks; i++)
		{
			memcpy(p1,  &LeftMark[i][0], m_MarkerSurfaceWidth * sizeof(DWORD));
			memcpy(p2, &RightMark[i][0], m_MarkerSurfaceWidth * sizeof(DWORD));
			p1 += rect.Pitch;
			p2 += rect.Pitch;
		}
		m_pMarkerSurface->UnlockRect();
	}

	// iZ3D
	m_bTableMethod = true;
	if(FAILED(hResult = m_pd3dDevice->CreatePixelShader((DWORD*)g_ps30_shaderS3DTable1st, &m_pShader1stPass)))
		return hResult;
	if(FAILED(hResult = m_pd3dDevice->CreatePixelShader((DWORD*)g_ps30_shaderS3DTableAndGamma1st, &m_pShader1stPassAndGamma)))
		return hResult;
	if(FAILED(hResult = m_pd3dDevice->CreatePixelShader((DWORD*)g_ps30_shaderS3DTable2nd, &m_pShader2ndPass)))
		return hResult;
	if(FAILED(hResult = m_pd3dDevice->CreatePixelShader((DWORD*)g_ps30_shaderS3DTableAndGamma2nd, &m_pShader2ndPassAndGamma)))
		return hResult;
	
	if (m_bTableMethod)
	{  
#ifndef FINAL_RELEASE
		TCHAR path[MAX_PATH];
		HMODULE hModule = NULL;
		GetModuleHandleEx(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCTSTR)CreateOutputDX9, &hModule);
		GetModuleFileName(hModule, path, _countof(path));
		_tcsrchr(path, '\\')[0] = '\0';
		_tcsrchr(path, '\\')[0] = '\0';
		_tcsrchr(path, '\\')[0] = '\0';
		_tcscat_s(path, _T("\\S3DTables\\"));
		TCHAR fullName[MAX_PATH];
		_stprintf_s(fullName, _T("%s%s"), path, _T("*.bmp"));

		HANDLE hFind = INVALID_HANDLE_VALUE;
		WIN32_FIND_DATA FindFileData;
		hFind = FindFirstFile(fullName, &FindFileData);

		if (hFind != INVALID_HANDLE_VALUE) 
		{
			do 
			{
				CComPtr<IDirect3DTexture9> Tex;
				_stprintf_s(fullName, _T("%s%s"), path, FindFileData.cFileName);
				HRESULT hr = D3DXCreateTextureFromFileEx(m_pd3dDevice, fullName, 
					D3DX_DEFAULT, D3DX_DEFAULT, 1, 0, D3DFMT_X8R8G8B8, D3DPOOL_MANAGED, D3DX_DEFAULT, D3DX_DEFAULT,
					0, NULL, NULL, &Tex);
				m_Tables.push_back(Tex);
				m_TablesNames.push_back(FindFileData.cFileName);
			} while(FindNextFile(hFind, &FindFileData) != 0);
			FindClose(hFind);
		}
		if (m_Tables.empty())
#endif
		{
			HRESULT hr = S_OK;
			Blob b;
			bool r;
			r = b.Initialize( _T("Driver/S3DMarkedTable.bmp") );
			if (r)
			{
				CComPtr<IDirect3DTexture9> Tex;
				hr = D3DXCreateTextureFromFileInMemoryEx( m_pd3dDevice, b.GetData(), (UINT)b.GetSize(), 
					D3DX_DEFAULT, D3DX_DEFAULT, 1, 0, D3DFMT_X8R8G8B8, D3DPOOL_MANAGED, 
					D3DX_DEFAULT, D3DX_DEFAULT, 0, NULL, NULL, &Tex );
				m_Tables.push_back(Tex);
			}   

			if (m_bAngleCorrection)
			{
				r = b.Initialize( _T("Driver/S3DMarkedFront.png") );
				if (r)
				{
					hr = D3DXCreateTextureFromFileInMemoryEx( m_pd3dDevice, b.GetData(), (UINT)b.GetSize(), 
						D3DX_DEFAULT, D3DX_DEFAULT, 1, 0, D3DFMT_A16B16G16R16, D3DPOOL_MANAGED, 
						D3DX_DEFAULT, D3DX_DEFAULT, 0, NULL, NULL, &m_pFrontTable );
				}
				r = b.Initialize( _T("Driver/S3DMarkedBack.png") );
				if (r)
				{
					hr = D3DXCreateTextureFromFileInMemoryEx( m_pd3dDevice, b.GetData(), (UINT)b.GetSize(), 
						D3DX_DEFAULT, D3DX_DEFAULT, 1, 0, D3DFMT_A16B16G16R16, D3DPOOL_MANAGED, 
						D3DX_DEFAULT, D3DX_DEFAULT, 0, NULL, NULL, &m_pBackTable );
				}
			}
		}
	}

	CComPtr<IDirect3DStateBlock9> pSavedState;
	hResult = m_pd3dDevice->CreateStateBlock(D3DSBT_ALL, &pSavedState);
	{
		m_pd3dDevice->SetTexture(0, 0);
		m_pd3dDevice->SetTexture(1, 0);
		m_pd3dDevice->SetTexture(2, 0);
		m_pd3dDevice->SetTexture(3, 0);
		m_pd3dDevice->SetRenderState(D3DRS_WRAP0,               0);
		m_pd3dDevice->SetRenderState(D3DRS_ZENABLE,             FALSE);
		m_pd3dDevice->SetRenderState(D3DRS_ALPHABLENDENABLE,    FALSE);
		m_pd3dDevice->SetRenderState(D3DRS_ALPHATESTENABLE,     FALSE);
		m_pd3dDevice->SetRenderState(D3DRS_FOGENABLE,           FALSE);
		m_pd3dDevice->SetRenderState(D3DRS_FILLMODE,            D3DFILL_SOLID);
		m_pd3dDevice->SetRenderState(D3DRS_CULLMODE,            D3DCULL_NONE);
		m_pd3dDevice->SetRenderState(D3DRS_LIGHTING,			FALSE);
		m_pd3dDevice->SetRenderState(D3DRS_STENCILENABLE,		FALSE);
		m_pd3dDevice->SetRenderState(D3DRS_CLIPPING,			FALSE);
		m_pd3dDevice->SetRenderState(D3DRS_CLIPPLANEENABLE,		FALSE);
		for(int i=0; i < (!m_bAngleCorrection ? 4 : 6); i++)
		{
			m_pd3dDevice->SetTextureStageState(i, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE); 
			m_pd3dDevice->SetTextureStageState(i, D3DTSS_COLOROP,   D3DTOP_SELECTARG1 );	
			m_pd3dDevice->SetTextureStageState(i, D3DTSS_COLORARG1, D3DTA_TEXTURE);
			m_pd3dDevice->SetTextureStageState(i, D3DTSS_ALPHAOP,   D3DTOP_DISABLE );
			m_pd3dDevice->SetSamplerState( i, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP );
			m_pd3dDevice->SetSamplerState( i, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP );
			m_pd3dDevice->SetSamplerState( i, D3DSAMP_MAGFILTER, D3DTEXF_POINT );
			m_pd3dDevice->SetSamplerState( i, D3DSAMP_MINFILTER, D3DTEXF_POINT );
			m_pd3dDevice->SetSamplerState( i, D3DSAMP_MIPMAPLODBIAS, 0 );
			m_pd3dDevice->SetSamplerState( i, D3DSAMP_MAXMIPLEVEL, 0);
			m_pd3dDevice->SetSamplerState( i, D3DSAMP_MAXANISOTROPY, 1);
			m_pd3dDevice->SetSamplerState( i, D3DSAMP_SRGBTEXTURE, 0);
			m_pd3dDevice->SetSamplerState( i, D3DSAMP_ELEMENTINDEX, 0);
			m_pd3dDevice->SetSamplerState( i, D3DSAMP_DMAPOFFSET, 0);
		}

		m_pd3dDevice->SetFVF( D3DFVF_D3DTVERTEX_1T );
		m_pd3dDevice->SetRenderState(D3DRS_SRGBWRITEENABLE, TRUE);

		m_pd3dDevice->SetSamplerState( 0, D3DSAMP_SRGBTEXTURE, FALSE);
		m_pd3dDevice->SetSamplerState( 1, D3DSAMP_SRGBTEXTURE, FALSE);


		if (m_bAngleCorrection)
		{
			m_pd3dDevice->SetTexture(4, m_pBackTable);
			m_pd3dDevice->SetTexture(5, m_pFrontTable);
		}
		
		m_pd3dDevice->SetPixelShaderConstantB(0, &m_bCheckersMode, 1);
		m_pd3dDevice->SetPixelShaderConstantB(1, &m_bAngleCorrection, 1);

		m_pd3dDevice->SetTexture(4, m_pBackTable);
		m_pd3dDevice->SetTexture(5, m_pFrontTable);

		m_pd3dDevice->SetSamplerState( 3, D3DSAMP_SRGBTEXTURE, TRUE);
		m_pd3dDevice->SetSamplerState( 3, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP );
		m_pd3dDevice->SetSamplerState( 3, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP );
		m_pd3dDevice->SetPixelShader(m_pShader1stPass);
		hResult = m_pd3dDevice->CreateStateBlock(D3DSBT_ALL, &m_pState);

		m_pd3dDevice->SetSamplerState( 0, D3DSAMP_SRGBTEXTURE, FALSE);
		m_pd3dDevice->SetSamplerState( 1, D3DSAMP_SRGBTEXTURE, FALSE);
		m_pd3dDevice->SetSamplerState( 2, D3DSAMP_SRGBTEXTURE, FALSE);
		m_pd3dDevice->SetSamplerState( 2, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP );
		m_pd3dDevice->SetSamplerState( 2, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP );
		m_pd3dDevice->SetPixelShader(m_pShader1stPassAndGamma);

		hResult = m_pd3dDevice->CreateStateBlock(D3DSBT_ALL, &m_pStateWithGamma);
	}
	hResult = pSavedState->Apply();

	return hResult;
}

void S3DMarkedOutput::ReadConfigData( const char* configXml )
{
	m_bAngleCorrection = false;
	m_bCheckersMode = false;
	// Re-parse with our own (ticpp-shim) TinyXML so no node crosses from S3DAPI.
	if (!configXml || !*configXml)
		return;
	TiXmlDocument doc;
	doc.Parse(configXml);
	if (doc.Error())
		return;
	TiXmlElement* item = doc.RootElement();
	if (!item)
		return;
	TiXmlElement* pitem = item->FirstChildElement("AngleCorrection");
	if (pitem)
		pitem->QueryIntAttribute("Value", &m_bAngleCorrection);
	pitem = item->FirstChildElement("CheckersMode");
	if (pitem)
		pitem->QueryIntAttribute("Value", &m_bCheckersMode);
}

HRESULT S3DMarkedOutput::InitializeSCResources(CBaseSwapChain* pSwapChain)
{
	HRESULT res = S_OK;
	m_bMultiPassRendering = TRUE;
	return res;
}

HRESULT S3DMarkedOutput::Output(bool bLeft, CBaseSwapChain* pSwapChain, RECT* pDestRect)
{
	HRESULT hResult = S_OK;

	IDirect3DSurface9* primary = pSwapChain->GetPresenterBackBuffer();
	IDirect3DSurface9* view = pSwapChain->GetViewRT(bLeft);
	RECT* pViewRect = pSwapChain->GetViewRect(bLeft);

	SetStatesBeforeDraw(pSwapChain);
	if (bLeft)
		DrawFirstPass(pSwapChain, primary, NULL);
	else
		DrawSecondPass(pSwapChain, primary);

	SIZE bbSize = pSwapChain->m_BackBufferSize;

	RECT MarkRectSrc, MarkRectDst;
	int width = min(bbSize.cx, (int)m_MarkerSurfaceWidth);
	if (bLeft)
		SetRect(&MarkRectSrc,  0, m_MarkerIndex, width, m_MarkerIndex + 1);
	else
		SetRect(&MarkRectSrc, 0, m_MarkerIndex + m_NumberOfMarks, width, m_MarkerIndex + m_NumberOfMarks + 1);
	int destSizeX = width, destStartY = 0;
	if(m_InverseAxisYDirection)
		destStartY = bbSize.cy - m_NumberOfMarkedLines;
	if(m_ScaleToFrame)
		destSizeX = (int)(m_ScaleFactor * bbSize.cx);
	SetRect(&MarkRectDst,  0,  destStartY, destSizeX, destStartY + m_NumberOfMarkedLines);
	if (pDestRect)
	{
		MarkRectDst.left	+= pDestRect->left;
		MarkRectDst.top		+= pDestRect->top;
		MarkRectDst.right	+= pDestRect->left;
		MarkRectDst.bottom	+= pDestRect->top;
	}
	m_MarkerIndex = (++m_MarkerIndex) % m_NumberOfMarks;

	NSCALL(m_pd3dDevice->StretchRect(m_pMarkerSurface, &MarkRectSrc, primary, &MarkRectDst, D3DTEXF_POINT));

	return hResult;
}

HRESULT S3DMarkedOutput::SetStates(CBaseSwapChain* pSwapChain)
{
	HRESULT res = S_OK;

	/*SHORT State = GetAsyncKeyState(VK_F6);
	BOOL bNewFront = (State & 0xFFFF) == 0;
	m_pd3dDevice->SetPixelShaderConstantB(0, &bNewFront, 1);*/

	if (m_bCheckersMode)
	{
		D3DSURFACE_DESC desc;
		res = pSwapChain->m_pPrimaryBackBuffer->GetDesc(&desc);
		float c[4] = { (float)desc.Width, (float)desc.Height, 1.0f/desc.Width, 1.0f/desc.Height };
		m_pd3dDevice->SetPixelShaderConstantF(3, c, 1);
	}

	/*SHORT State = GetAsyncKeyState(VK_F6);
	BOOL bAngleCorrection = (State & 0xFFFF) == 0;
	m_pd3dDevice->SetPixelShaderConstantB(1, &bAngleCorrection, 1);*/

	if (m_OutputMode == 8)
	{
		static int x = 0;
		if (GetAsyncKeyState(VK_LEFT) & 0x7FFF)
			x++;
		else if (GetAsyncKeyState(VK_RIGHT) & 0x7FFF)
			x--;
		static int y = 0;
		if (GetAsyncKeyState(VK_UP) & 0x7FFF)
			y++;
		else if (GetAsyncKeyState(VK_DOWN) & 0x7FFF)
			y--;
		if (GetAsyncKeyState(VK_CLEAR) & 0x7FFF)
		{
			x = 0;
			y = 0;
		}
		D3DSURFACE_DESC desc;
		res = pSwapChain->m_pPrimaryBackBuffer->GetDesc(&desc);
		float c[4] = { (float)x / desc.Width, (float)y / desc.Height, 0, 0 };  
		m_pd3dDevice->SetPixelShaderConstantF(4, c, 1);
	}
		
	return res;
}

DWORD S3DMarkedOutput::SetNextSubMode() 
{ 
	if (m_bTableMethod)
	{
		if (m_SubMode + 1 >= m_Tables.size())
			m_SubMode = 0;
		else
			m_SubMode++;
	}
	return m_SubMode; 
}

void S3DMarkedOutput::ProcessSubMode() 
{
	if (m_SubMode < m_Tables.size())
	{
		m_pd3dDevice->SetTexture(3, m_Tables[m_SubMode].m_T);
	}
};

const TCHAR* S3DMarkedOutput::GetSubModeName()
{
	if (m_SubMode < m_TablesNames.size())
	{
		return m_TablesNames[m_SubMode].c_str();
	}
	else return _T("");
}

bool CheckBackPanel( const char* model )
{
//#ifndef FINAL_RELEASE
	return true;
//#else
//		// iZ3D Old 17"
//	return strcmp(model, "AAA0000") == 0 ||
//		// iZ3D Old
//		strcmp(model, "CMO3228") == 0 || strcmp(model, "CMO3229") == 0 ||
//		// iZ3D New
//		strcmp(model, "CMO3238") == 0 || strcmp(model, "CMO3237") == 0 ||
//		// Matrox TripleHead2Go Digital Edition 
//		strcmp(model, "MTX0510") == 0 || strcmp(model, "MTX0511") == 0;
//#endif
}

bool S3DMarkedOutput::FindAdapter( LPDIRECT3D9 pDirect3D9, UINT DeviceNum, D3DDEVTYPE &DeviceType, UINT Adapters[2] )
{
	UINT AdapterCount = pDirect3D9->GetAdapterCount();
	char model[8] = "";
	DISPLAY_DEVICEA	MonitorDevice;
	MonitorDevice.cb = sizeof( DISPLAY_DEVICEA );
	for( UINT i = 0; i < AdapterCount; i++ ) {
		D3DADAPTER_IDENTIFIER9	Adapter_Identify;
		pDirect3D9->GetAdapterIdentifier( i, 0, &Adapter_Identify );
		int j = 0;
		for( ; EnumDisplayDevicesA(Adapter_Identify.DeviceName, j, &MonitorDevice, 0x0 ); j++ ) {
			if( MonitorDevice.StateFlags & DISPLAY_DEVICE_ACTIVE &&
				MonitorDevice.StateFlags & DISPLAY_DEVICE_ATTACHED ) // for ATI, Windows XP
			{
				char* s = strchr(MonitorDevice.DeviceID, '\\') + 1;
				size_t len = strchr(s, '\\') - s;
				if (len >= _countof(model))
					len = _countof(model) - 1;
				strncpy_s(model, s, len);		
				if (CheckBackPanel(model))
				{
					Adapters[0] = i;
					D3DCAPS9 D3DBackCaps;
					pDirect3D9->GetDeviceCaps(i, DeviceType, &D3DBackCaps);
					return true;
				}
			}
		}
	}
	return false;
}

void S3DMarkedOutput::ParseVersion_0_2(TiXmlHandle& frameHandle, const char* &format, vector< vector<DWORD> > &LeftMark, vector< vector<DWORD> > &RightMark)
{
	TiXmlElement* element = frameHandle.Element();
	element->QueryIntAttribute("NumberOfMarkedLines", (int*)&m_NumberOfMarkedLines);
	TiXmlHandle lineHandle = frameHandle.FirstChild();
	element = lineHandle.Element();
	if(element)
	{
		format = element->Attribute("ColorFormat");
		element->QueryIntAttribute("NumberOfMarks", (int*)&m_NumberOfMarks);
	}
	if(strcmpi(format, "r8g8b8_hex") == 0)
	{
		element = lineHandle.Child("LeftFrameMarks", 0).FirstChild().Element();
		ParseMarkLine(element, LeftMark);
		element = lineHandle.Child("RightFrameMarks", 0).FirstChild().Element();
		ParseMarkLine(element, RightMark);
		m_MarkerSurfaceWidth = (UINT)LeftMark[0].size();
	}
}

void S3DMarkedOutput::ParseVersion_0_3(TiXmlHandle& frameHandle)
{
	TiXmlHandle lineHandle = frameHandle.FirstChild();
	TiXmlElement* element = lineHandle.Element();
	if(element)
	{
		element->QueryIntAttribute("InverseAxisYDirection", (int*)&m_InverseAxisYDirection);
		element->QueryIntAttribute("ScaleToFrame", (int*)&m_ScaleToFrame);
		element->QueryFloatAttribute("ScaleFactor", (float*)&m_ScaleFactor);
	}
}
