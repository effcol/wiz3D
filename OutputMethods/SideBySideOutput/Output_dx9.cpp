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

using namespace DX9Output;

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
	return new SideBySideOutput(mode, spanMode);
}

OUTPUT_API DWORD CALLBACK GetOutputCaps()
{
	return 0;
}

OUTPUT_API void CALLBACK GetOutputName(char* name, DWORD size)
{
	strcpy_s(name, size, "SideBySide");
}

OUTPUT_API BOOL CALLBACK EnumOutputModes(DWORD num, char* name, DWORD size)
{
	switch(num)
	{
	case 0:
		strcpy_s(name, size, "Default");
		return TRUE;
	case 1:
		strcpy_s(name, size, "Over/Under");
		return TRUE;
	case 2:
		strcpy_s(name, size, "Crosseyed");
		return TRUE;
	default:
		return FALSE;
	}
}

SideBySideOutput::SideBySideOutput(DWORD mode, DWORD spanMode)
: OutputMethod(mode, spanMode), m_bCrosseyed(false), m_DefaultGap(0)
{
	if (mode != 2)
	{
		m_OutputMode &= 1;
	}
	else
	{
		m_bCrosseyed = true;
		m_OutputMode = 0;
	}
	m_Caps = ocHardwareMouseCursorNotSupported;
}

UINT DX9Output::SideBySideOutput::GetOutputChainsNumber()
{
	return 1;
}

HRESULT SideBySideOutput::InitializeSCData(CBaseSwapChain* pSwapChain)
{
	HRESULT hResult = OutputMethod::InitializeSCData(pSwapChain);
	if(SUCCEEDED(hResult))
	{
		D3DSURFACE_DESC desc;
		pSwapChain->m_pPrimaryBackBuffer->GetDesc(&desc);
		pSwapChain->m_CurrentGap = m_DefaultGap;
		for (int i = 0; i < (int)m_Gap.size(); i++)
		{
			if (desc.Width == m_Gap[i].Width && desc.Height == m_Gap[i].Height)
			{
				pSwapChain->m_CurrentGap = m_Gap[i].Gap;
				break;
			}
		}
	}
	return hResult;
}

void SideBySideOutput::ReadConfigData( const char* configXml )
{
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
	TiXmlElement* pitem = item->FirstChildElement("Gap");
	while (pitem)
	{
		ResolutionGap val = { 0, 0, 0 };
		pitem->QueryIntAttribute("Width", &val.Width);
		pitem->QueryIntAttribute("Height", &val.Height);
		pitem->QueryIntAttribute("Value", &val.Gap);
		if (val.Width == 0 && val.Height == 0)
			m_DefaultGap = val.Gap;
		else
			m_Gap.push_back(val);
		pitem = pitem->NextSiblingElement("Gap");
	}
}

SideBySideOutput::~SideBySideOutput(void)
{
}

HRESULT SideBySideOutput::Output(CBaseSwapChain* pSwapChain)
{
	HRESULT hResult = S_OK;
	IDirect3DSurface9* left = pSwapChain->GetLeftBackBufferRT();
	IDirect3DSurface9* right = pSwapChain->GetRightBackBufferRT();
	RECT* pLeftRect = pSwapChain->GetLeftBackBufferRect();
	RECT* pRightRect = pSwapChain->GetRightBackBufferRect();
	
	IDirect3DSurface9* primary = pSwapChain->m_pPrimaryBackBuffer;
	IDirect3DSurface9* secondary = pSwapChain->m_pSecondaryBackBuffer;
	D3DSURFACE_DESC desc;
	primary->GetDesc(&desc);
	if (m_bCrosseyed || pSwapChain->m_CurrentGap != 0) {
		NSCALL(m_pd3dDevice->ColorFill(primary, NULL, 0));
	}
	RECT Rect;
	Rect.left = 0;
	Rect.top = 0;

	int horizontalGap = 0;
	int verticalGap = 0;
	if ( pSwapChain->m_pSourceRect ){
		horizontalGap = pLeftRect->right - pLeftRect->left + pSwapChain->m_pSourceRect->left - pSwapChain->m_pSourceRect->right;
		verticalGap = pLeftRect->bottom - pLeftRect->top + pSwapChain->m_pSourceRect->top - pSwapChain->m_pSourceRect->bottom;
	}
	// make left eye
	if(m_OutputMode == 0)
	{
		Rect.right = desc.Width / 2 - pSwapChain->m_CurrentGap / 2;
		Rect.bottom = desc.Height;
	}
	else
	{
		Rect.right = desc.Width;
		Rect.bottom = desc.Height / 2 - pSwapChain->m_CurrentGap / 2;
	}

	if (!m_bCrosseyed)
	{
		NSCALL(m_pd3dDevice->StretchRect(left, pLeftRect, 
			primary, &Rect, D3DTEXF_LINEAR));
	}
	else
	{
		float aspect = 1.0f * desc.Width / desc.Height;
		if(m_OutputMode == 0)
		{
			float Offset = (desc.Height - Rect.right / aspect) / 2;
			Rect.top = (DWORD)(Offset + 0.5f);
			Rect.bottom = (DWORD)(desc.Height - Offset + 0.5f);
		}
		else
		{
			float Offset = (desc.Width - Rect.bottom * aspect) / 2;
			Rect.left = (DWORD)(Offset + 0.5f);
			Rect.right = (DWORD)(desc.Width - Offset + 0.5f);
		}
		NSCALL(m_pd3dDevice->StretchRect(right, pRightRect, 
			primary, &Rect, D3DTEXF_LINEAR));
	}


	// make right eye
	if( m_OutputMode == 0 )
	{
		Rect.left = desc.Width / 2 + (pSwapChain->m_CurrentGap - pSwapChain->m_CurrentGap / 2 - horizontalGap/2);
		Rect.right = desc.Width - horizontalGap / 2;
	}
	else
	{
		Rect.top = desc.Height / 2 + (pSwapChain->m_CurrentGap - pSwapChain->m_CurrentGap / 2 - verticalGap / 2);
		Rect.bottom = desc.Height - verticalGap / 2;
	}

	if (!m_bCrosseyed)
	{
		NSCALL(m_pd3dDevice->StretchRect(right, pRightRect, 
			primary, &Rect, D3DTEXF_LINEAR));
	}
	else 
	{
		NSCALL(m_pd3dDevice->StretchRect(left, pLeftRect, 
			primary, &Rect, D3DTEXF_LINEAR));
	}
	return hResult;
}
