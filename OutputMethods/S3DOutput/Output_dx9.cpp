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
#include <tinyxml.h>
#include "S3DWrapper9\BaseSwapChain.h"

using namespace DX9Output;

#include "Shaders_dx9/shaderS3D.h"
#include "Shaders_dx9/shaderS3DAndGamma.h"
#include "Shaders_dx9/shaderS3DCFL.h"
#include "Shaders_dx9/shaderS3DCFLAndGamma.h"
#include "Shaders_dx9/shaderS3DBWCFL.h"
#include "Shaders_dx9/shaderS3DBWCFLAndGamma.h"
#include "Shaders_dx9/shaderS3DFBCFL.h"
#include "Shaders_dx9/shaderS3DFBCFLAndGamma.h"

#include "Shaders_dx9/shaderS3D1st.h"
#include "Shaders_dx9/shaderS3D2nd.h"
#include "Shaders_dx9/shaderS3DCFL2nd.h"
#include "Shaders_dx9/shaderS3DAndGamma1st.h"
#include "Shaders_dx9/shaderS3DAndGamma2nd.h"
#include "Shaders_dx9/shaderS3DCFLAndGamma2nd.h"

#include "Shaders_dx9/shaderS3DTable.h"
#include "Shaders_dx9/shaderS3DTableCFL.h"
#include "Shaders_dx9/shaderS3DTableAndGamma.h"
#include "Shaders_dx9/shaderS3DTableCFLAndGamma.h"
#include "Shaders_dx9/shaderS3DTableCFL2.h"
#include "Shaders_dx9/shaderS3DTableCFLAndGamma2.h"
#include "Shaders_dx9/shaderS3DTableBWCFL.h"
#include "Shaders_dx9/shaderS3DTableBWCFLAndGamma.h"

#include "Shaders_dx9/shaderS3DInterlaced.h"
#include "Shaders_dx9/shaderS3DCheckerboard.h"

#include <d3dx9.h>
#include <d3d9.h>
#include "..\ArchiveFile\blob.h"

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
	return new S3DOutput(mode, spanMode);
}

OUTPUT_API DWORD CALLBACK GetOutputCaps()
{
	return odResetGDIGammaRamp; // Gamma
}

OUTPUT_API void CALLBACK GetOutputName(char* name, DWORD size)
{
	strcpy_s(name, size, "S3D");
}

OUTPUT_API BOOL CALLBACK EnumOutputModes(DWORD num, char* name, DWORD size)
{
	switch(num)
	{
	case 0:
		strcpy_s(name, size, "Auto");
		return TRUE;
	case 2:
		strcpy_s(name, size, "CFL");
		return TRUE;
	case 1:
		strcpy_s(name, size, "CF");
		return TRUE;
	case 3:
		strcpy_s(name, size, "Table CFL");
		return TRUE;
	case 4:
		strcpy_s(name, size, "Table");
		return TRUE;
#ifndef FINAL_RELEASE
	case 5:
		strcpy_s(name, size, "BW CFL");
		return TRUE;
	case 6:
		strcpy_s(name, size, "BW Table CFL");
		return TRUE;
	case 7:
		strcpy_s(name, size, "L&R CFL");
		return TRUE;
	case 8:
		strcpy_s(name, size, "Panel Alignment");
		return TRUE;
	case 9:
		strcpy_s(name, size, "Interlaced");
		return TRUE;
	case 10:
		strcpy_s(name, size, "Checkerboard");
		return TRUE;
#endif
	default:
		return FALSE;
	}
}

S3DOutput::S3DOutput(DWORD mode, DWORD spanMode)
: PostprocessedOutput(mode, spanMode)
, m_bAngleCorrection(false)
, m_bCheckersMode(false)
{
	 m_bProcessGammaRamp = true;
	//--- select learning method ---
	if(m_OutputMode == 0) // auto
	{
		m_OutputMode = 3; // Table CFL
	}
}

S3DOutput::~S3DOutput(void)
{
}

void S3DOutput::ClearResources()
{
	m_Tables.clear();
	m_pFrontTable.Release();
	m_pBackTable.Release();
}

UINT S3DOutput::GetOutputChainsNumber()
{ 
	if(m_SpanMode != 0)
		return 1; 
	return 2; 
}

HRESULT S3DOutput::InitializeResources()
{
	HRESULT res = S_OK;
	m_bTableMethod = false;
	CComQIPtr<IDirect3DDevice9Ex> pExDev( m_pd3dDevice ) ;
	m_bIsD3D9Ex = pExDev != NULL;
	if (!m_bMultiPassRendering)
	{
		switch(m_OutputMode)
		{
		case 1: // CFL
			if(FAILED(res = m_pd3dDevice->CreatePixelShader((DWORD*)g_ps20_shaderS3DCFL, &m_pShader1stPass)))
				return res;
			if(FAILED(res = m_pd3dDevice->CreatePixelShader((DWORD*)g_ps20_shaderS3DCFLAndGamma, &m_pShader1stPassAndGamma)))
				return res;
			break;
		case 2: // CF
			if(FAILED(res = m_pd3dDevice->CreatePixelShader((DWORD*)g_ps20_shaderS3D, &m_pShader1stPass)))
				return res;
			if(FAILED(res = m_pd3dDevice->CreatePixelShader((DWORD*)g_ps20_shaderS3DAndGamma, &m_pShader1stPassAndGamma)))
				return res;
			break;
		case 4: // Table
			m_bTableMethod = true;
			if(FAILED(res = m_pd3dDevice->CreatePixelShader((DWORD*)g_ps30_shaderS3DTable, &m_pShader1stPass)))
				return res;
			if(FAILED(res = m_pd3dDevice->CreatePixelShader((DWORD*)g_ps30_shaderS3DTableAndGamma, &m_pShader1stPassAndGamma)))
				return res;
			break;
#ifndef FINAL_RELEASE
		case 5: // BW CFL
			if(FAILED(res = m_pd3dDevice->CreatePixelShader((DWORD*)g_ps20_shaderS3DBWCFL, &m_pShader1stPass)))
				return res;
			if(FAILED(res = m_pd3dDevice->CreatePixelShader((DWORD*)g_ps20_shaderS3DBWCFLAndGamma, &m_pShader1stPassAndGamma)))
				return res;
			break;
		case 6: // BW Table CFL
			m_bTableMethod = true;
			if(FAILED(res = m_pd3dDevice->CreatePixelShader((DWORD*)g_ps30_shaderS3DTableBWCFL, &m_pShader1stPass)))
				return res;
			if(FAILED(res = m_pd3dDevice->CreatePixelShader((DWORD*)g_ps30_shaderS3DTableBWCFLAndGamma, &m_pShader1stPassAndGamma)))
				return res;
			break;
		case 7: // FB CFL
			if(FAILED(res = m_pd3dDevice->CreatePixelShader((DWORD*)g_ps20_shaderS3DFBCFL, &m_pShader1stPass)))
				return res;
			if(FAILED(res = m_pd3dDevice->CreatePixelShader((DWORD*)g_ps20_shaderS3DFBCFLAndGamma, &m_pShader1stPassAndGamma)))
				return res;
			break;
		case 8: // Panel Alignment
			m_bTableMethod = true;
			if(FAILED(res = m_pd3dDevice->CreatePixelShader((DWORD*)g_ps30_shaderS3DTableCFL2, &m_pShader1stPass)))
				return res;
			if(FAILED(res = m_pd3dDevice->CreatePixelShader((DWORD*)g_ps30_shaderS3DTableCFLAndGamma2, &m_pShader1stPassAndGamma)))
				return res;
			break;
		case 9: // Interlaced
			if(FAILED(res = m_pd3dDevice->CreatePixelShader((DWORD*)g_ps20_shaderS3DInterlaced, &m_pShader1stPass)))
				return res;
			m_pShader1stPassAndGamma = NULL;
			break;
		case 10: // Checkerboard
			if(FAILED(res = m_pd3dDevice->CreatePixelShader((DWORD*)g_ps20_shaderS3DCheckerboard, &m_pShader1stPass)))
				return res;
			m_pShader1stPassAndGamma = NULL;
			break;
#endif
		default: // 3 - TableCFL
			m_OutputMode = 3;
		case 3:
			m_bTableMethod = true;
			if(FAILED(res = m_pd3dDevice->CreatePixelShader((DWORD*)g_ps30_shaderS3DTableCFL, &m_pShader1stPass)))
				return res;
			if(FAILED(res = m_pd3dDevice->CreatePixelShader((DWORD*)g_ps30_shaderS3DTableCFLAndGamma, &m_pShader1stPassAndGamma)))
				return res;
			break;
		}
	}
	else
	{
		if(FAILED(res = m_pd3dDevice->CreatePixelShader((DWORD*)g_ps14_shaderS3D1st, &m_pShader1stPass)))
			return res;
		if(FAILED(res = m_pd3dDevice->CreatePixelShader((DWORD*)g_ps20_shaderS3DAndGamma1st, &m_pShader1stPassAndGamma)))
			return res;
		if (m_OutputMode == 2) // CF
		{
			if(FAILED(res = m_pd3dDevice->CreatePixelShader((DWORD*)g_ps20_shaderS3D2nd, &m_pShader2ndPass)))
				return res;
			if(FAILED(res = m_pd3dDevice->CreatePixelShader((DWORD*)g_ps20_shaderS3DAndGamma2nd, &m_pShader2ndPassAndGamma)))
				return res;
		}
		else
		{
			m_OutputMode = 1;
			if(FAILED(res = m_pd3dDevice->CreatePixelShader((DWORD*)g_ps20_shaderS3DCFL2nd, &m_pShader2ndPass)))
				return res;
			if(FAILED(res = m_pd3dDevice->CreatePixelShader((DWORD*)g_ps20_shaderS3DCFLAndGamma2nd, &m_pShader2ndPassAndGamma)))
				return res;
		}
	}
	
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
				D3DPOOL texMemType = m_bIsD3D9Ex ? D3DPOOL_DEFAULT : D3DPOOL_MANAGED;

				HRESULT hr = D3DXCreateTextureFromFileEx(m_pd3dDevice, fullName, 
					D3DX_DEFAULT, D3DX_DEFAULT, 1, 0, D3DFMT_X8R8G8B8, texMemType, D3DX_DEFAULT, D3DX_DEFAULT,
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
			r = b.Initialize( _T("Driver/S3DTableCFL.bmp") );
			if (r)
			{
				CComPtr<IDirect3DTexture9> Tex;
				D3DPOOL texMemType = m_bIsD3D9Ex ? D3DPOOL_DEFAULT : D3DPOOL_MANAGED;
				hr = D3DXCreateTextureFromFileInMemoryEx( m_pd3dDevice, b.GetData(), (UINT)b.GetSize(), 
					D3DX_DEFAULT, D3DX_DEFAULT, 1, 0, D3DFMT_X8R8G8B8, texMemType, 
					D3DX_DEFAULT, D3DX_DEFAULT, 0, NULL, NULL, &Tex );
				m_Tables.push_back(Tex);
			}

			if (m_bAngleCorrection)
			{
				r = b.Initialize( _T("Driver/S3DFront.png") );
				if (r)
				{
					hr = D3DXCreateTextureFromFileInMemoryEx( m_pd3dDevice, b.GetData(), (UINT)b.GetSize(), 
						D3DX_DEFAULT, D3DX_DEFAULT, 1, 0, D3DFMT_A16B16G16R16, D3DPOOL_MANAGED, 
						D3DX_DEFAULT, D3DX_DEFAULT, 0, NULL, NULL, &m_pFrontTable );
				}
				r = b.Initialize( _T("Driver/S3DBack.png") );
				if (r)
				{
					hr = D3DXCreateTextureFromFileInMemoryEx( m_pd3dDevice, b.GetData(), (UINT)b.GetSize(), 
						D3DX_DEFAULT, D3DX_DEFAULT, 1, 0, D3DFMT_A16B16G16R16, D3DPOOL_MANAGED, 
						D3DX_DEFAULT, D3DX_DEFAULT, 0, NULL, NULL, &m_pBackTable );
				}
			}
		}
		if (IsCFLMode())
			SetDefFrontColor();
	}

	if (!m_bTableMethod)
		m_bAngleCorrection = false;

	CComPtr<IDirect3DStateBlock9> pSavedState;
	res = m_pd3dDevice->CreateStateBlock(D3DSBT_ALL, &pSavedState);
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
			if (m_OutputMode != 9)
			{
				m_pd3dDevice->SetSamplerState( i, D3DSAMP_MAGFILTER, D3DTEXF_POINT );
				m_pd3dDevice->SetSamplerState( i, D3DSAMP_MINFILTER, D3DTEXF_POINT );
			}
			else
			{
				m_pd3dDevice->SetSamplerState( i, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR );
				m_pd3dDevice->SetSamplerState( i, D3DSAMP_MINFILTER, D3DTEXF_LINEAR );
			}
			m_pd3dDevice->SetSamplerState( i, D3DSAMP_MIPMAPLODBIAS, 0 );
			m_pd3dDevice->SetSamplerState( i, D3DSAMP_MAXMIPLEVEL, 0);
			m_pd3dDevice->SetSamplerState( i, D3DSAMP_MAXANISOTROPY, 1);
			m_pd3dDevice->SetSamplerState( i, D3DSAMP_SRGBTEXTURE, 0);
			m_pd3dDevice->SetSamplerState( i, D3DSAMP_ELEMENTINDEX, 0);
			m_pd3dDevice->SetSamplerState( i, D3DSAMP_DMAPOFFSET, 0);
		}

		m_pd3dDevice->SetFVF( D3DFVF_D3DTVERTEX_1T );
		m_pd3dDevice->SetRenderState(D3DRS_SRGBWRITEENABLE, TRUE);

		if (!m_bTableMethod)
		{
			m_pd3dDevice->SetSamplerState( 0, D3DSAMP_SRGBTEXTURE, TRUE);
			m_pd3dDevice->SetSamplerState( 1, D3DSAMP_SRGBTEXTURE, TRUE);
		}
		else
		{
			m_pd3dDevice->SetSamplerState( 0, D3DSAMP_SRGBTEXTURE, FALSE);
			m_pd3dDevice->SetSamplerState( 1, D3DSAMP_SRGBTEXTURE, FALSE);


			if (m_bAngleCorrection)
			{
				m_pd3dDevice->SetTexture(4, m_pBackTable);
				m_pd3dDevice->SetTexture(5, m_pFrontTable);
			}
		}
		m_pd3dDevice->SetPixelShaderConstantB(0, &m_bCheckersMode, 1);
		m_pd3dDevice->SetPixelShaderConstantB(1, &m_bAngleCorrection, 1);
		m_pd3dDevice->SetSamplerState( 3, D3DSAMP_SRGBTEXTURE, TRUE);
		m_pd3dDevice->SetSamplerState( 3, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP );
		m_pd3dDevice->SetSamplerState( 3, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP );
		m_pd3dDevice->SetPixelShader(m_pShader1stPass);
#ifdef DYNAMIC_CONSTANTS
		static float c[LEARNING_CONSTANT * 4] = { 0.003f, 0.5f, 1.0f, 0.0f,  0.0f, 1.0f/3, 2.0f/3, 3.0f/3 };  
		m_pd3dDevice->SetPixelShaderConstantF(0, c, LEARNING_CONSTANT);
#endif
		res = m_pd3dDevice->CreateStateBlock(D3DSBT_ALL, &m_pState);

		m_pd3dDevice->SetSamplerState( 0, D3DSAMP_SRGBTEXTURE, FALSE);
		m_pd3dDevice->SetSamplerState( 1, D3DSAMP_SRGBTEXTURE, FALSE);
		if (!m_bTableMethod)
			m_pd3dDevice->SetSamplerState( 2, D3DSAMP_SRGBTEXTURE, TRUE);
		else
			m_pd3dDevice->SetSamplerState( 2, D3DSAMP_SRGBTEXTURE, FALSE);
		m_pd3dDevice->SetSamplerState( 2, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP );
		m_pd3dDevice->SetSamplerState( 2, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP );
		m_pd3dDevice->SetPixelShader(m_pShader1stPassAndGamma);

		res = m_pd3dDevice->CreateStateBlock(D3DSBT_ALL, &m_pStateWithGamma);
	}
	res = pSavedState->Apply();

	return res;
}

void S3DOutput::ReadConfigData( const char* configXml )
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

HRESULT S3DOutput::InitializeSCResources(CBaseSwapChain* pSwapChain)
{
	HRESULT res = S_OK;

	if(m_SpanMode != 0)
	{
		m_bMultiPassRendering = TRUE;
		float sizeX = pSwapChain->m_Vertex[1].x - pSwapChain->m_Vertex[0].x;
		for(int i = 0; i< 4; i++)
		{
			pSwapChain->m_Vertex[i]     = pSwapChain->m_Vertex[i];
			pSwapChain->m_Vertex[i+4]   = pSwapChain->m_Vertex[i];
			pSwapChain->m_Vertex[i].x   = (pSwapChain->m_Vertex[i].x + 0.5f) * 0.5f - 0.5f;
			pSwapChain->m_Vertex[i+4].x = pSwapChain->m_Vertex[i].x + 0.5f * sizeX ;
		}
		pSwapChain->m_VertexSinglePass1Pass = &pSwapChain->m_Vertex[0];
		pSwapChain->m_VertexSinglePass2Pass = &pSwapChain->m_Vertex[4];
	}
	return res;
}

HRESULT S3DOutput::SetStates(CBaseSwapChain* pSwapChain)
{
	HRESULT res = S_OK;
	if (m_OutputMode >= 9)
	{
		D3DSURFACE_DESC desc;
		res = pSwapChain->m_pPrimaryBackBuffer->GetDesc(&desc);
		float c[4] = { desc.Width * 0.5f, desc.Height * 0.5f, 0.5f / desc.Width, 0.5f / desc.Height };  
		m_pd3dDevice->SetPixelShaderConstantF(2, c, 1);
	}
	if (m_bTableMethod)
	{
		/*SHORT State = GetAsyncKeyState(VK_F6);
		BOOL bNewFront = !((State & 0xFFFF) == 0);
		m_pd3dDevice->SetPixelShaderConstantB(0, &bNewFront, 1);*/

		if (m_bCheckersMode)
		{
			D3DSURFACE_DESC desc;
			res = pSwapChain->m_pPrimaryBackBuffer->GetDesc(&desc);
			float c[4] = { (float)desc.Width, (float)desc.Height, 1.0f/desc.Width, 1.0f/desc.Height };
			m_pd3dDevice->SetPixelShaderConstantF(3, c, 1);
		}

		/*State = GetAsyncKeyState(VK_F7);
		BOOL bAngleCorrection = !((State & 0xFFFF) == 0);
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
	}
	return res;
}

void S3DOutput::SetDefFrontColor() 
{ 
	if (m_SubMode < m_Tables.size())
	{
		D3DLOCKED_RECT lr;
		RECT r = { 511, 255, 512, 256 };
		if (SUCCEEDED(m_Tables[m_SubMode].m_T->LockRect(0, &lr, &r, D3DLOCK_READONLY)))
		{
			PBYTE pb = (PBYTE)lr.pBits;
			cDefFrontColor[0] = pb[0] / 255.0f;
			cDefFrontColor[0] = pow(cDefFrontColor[0], 2.2f);
			cDefFrontColor[1] = pb[1] / 255.0f;
			cDefFrontColor[1] = pow(cDefFrontColor[1], 2.2f);
			cDefFrontColor[2] = pb[2] / 255.0f;
			cDefFrontColor[2] = pow(cDefFrontColor[2], 2.2f);
			cDefFrontColor[3] = 0.0f;
			m_Tables[m_SubMode].m_T->UnlockRect(0);
			//--- for CFL color must be gray  ---
			if(IsCFLMode())  // Table CFL || Table BW CFL
			{
				float color = 0.333333f * (cDefFrontColor[0] + cDefFrontColor[1] + cDefFrontColor[2]);
				cDefFrontColor[0] = cDefFrontColor[1] = cDefFrontColor[2] = color;
			}
		}
	}
}

DWORD S3DOutput::SetNextSubMode() 
{ 
	if (m_bTableMethod)
	{
		if (m_SubMode + 1 >= m_Tables.size())
			m_SubMode = 0;
		else
			m_SubMode++;

		if (IsCFLMode())
			SetDefFrontColor();
	}
	return m_SubMode; 
}

void S3DOutput::ProcessSubMode() 
{
	if (m_SubMode < m_Tables.size())
	{
		m_pd3dDevice->SetTexture(3, m_Tables[m_SubMode].m_T);
		if (IsCFLMode())
			m_pd3dDevice->SetPixelShaderConstantF(2, cDefFrontColor, 1);
	}
};

const TCHAR* S3DOutput::GetSubModeName()
{
	if (m_SubMode < m_TablesNames.size())
	{
		return m_TablesNames[m_SubMode].c_str();
	}
	else return _T("");
}

bool CheckBackPanel( const char* model )
{
#ifndef FINAL_RELEASE
	return true;
#else
		// iZ3D Old 17"
	return strcmp(model, "AAA0000") == 0 ||
		// iZ3D Old
		strcmp(model, "CMO3228") == 0 || strcmp(model, "CMO3229") == 0 ||
		// iZ3D New
		strcmp(model, "CMO3238") == 0 || strcmp(model, "CMO3237") == 0 ||
		// Matrox TripleHead2Go Digital Edition 
		strcmp(model, "MTX0510") == 0 || strcmp(model, "MTX0511") == 0;
#endif
}

bool S3DOutput::FindAdapter( LPDIRECT3D9 pDirect3D9, UINT DeviceNum, D3DDEVTYPE &DeviceType, UINT Adapters[2] )
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

					//--- search second adapter ordinal for current master adapter ---
					D3DCAPS9 D3DFrontCaps;
					for(UINT j = 0; i < pDirect3D9->GetAdapterCount(); j++)
					{
						if (i != j)
						{
							pDirect3D9->GetDeviceCaps(j, DeviceType, &D3DFrontCaps);
							if(D3DFrontCaps.MasterAdapterOrdinal == j || D3DBackCaps.MasterAdapterOrdinal == i)
							{
								Adapters[1] = j;
								return true;
							}
						}
					}
				}
			}
		}
		if (j == 0 && m_SpanMode) // probably span mode
			return true;
	}
	return false;
}

bool S3DOutput::IsCFLMode()
{
	return (m_OutputMode == 3 || m_OutputMode == 6 || m_OutputMode == 8);
}
