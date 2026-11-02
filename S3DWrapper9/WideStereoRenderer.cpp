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
#include "StdAfx.h"
#include "WideStereoRenderer.h"
#include "WideStereoRenderer_Hook.h"
#include "MonoRenderer_Hook.h"
#include <math.h>
#include "CommandDumper.h"
#include "Globals.h"   // 0.5.0 task #77 round 9: D9Log (DEBUG_MESSAGE stripped in Final Release)

#include "shaders\shaderSimple.h"
#include "shaders\shaderSimpleRAWZ.h"
#include "shaders\shaderSimpleDepth.h"
#include "shaders\shaderSimpleDepthRAWZ.h"

// {EB570782-5F0F-4ffe-8271-09B24F751809}
static const GUID LeftTexture_GUID = 
{ 0xeb570782, 0x5f0f, 0x4ffe, { 0x82, 0x71, 0x9, 0xb2, 0x4f, 0x75, 0x18, 0x9 } };
// {EB570782-5F0F-4ffe-8271-09B24F751810}
static const GUID LeftSurface_GUID = 
{ 0xeb570782, 0x5f0f, 0x4ffe, { 0x82, 0x71, 0x9, 0xb2, 0x4f, 0x75, 0x18, 0x10 } };

using namespace Wide;

CWideStereoRenderer::CWideStereoRenderer(void)
	:CBaseStereoRenderer(), m_PSPipelineDataEx(m_PSPipelineData), m_VSPipelineDataEx(m_VSPipelineData)
{
}

CWideStereoRenderer::~CWideStereoRenderer(void)
{
	UniqueUnhookCode(this, (PVOID*)&Original_Surface_GetDesc);
	UniqueUnhookCode(this, (PVOID*)&Original_TexSurface_GetDesc);
	UniqueUnhookCode(this, (PVOID*)&Original_DepthSurface_GetDesc);	
	UniqueUnhookCode(this, (PVOID*)&Original_Surface_LockRect);
	UniqueUnhookCode(this, (PVOID*)&Original_Texture_LockRect);	
	UniqueUnhookCode(this, (PVOID*)&Original_TexSurface_PDef_LockRect);
	UniqueUnhookCode(this, (PVOID*)&Original_DepthSurface_LockRect);	
	UniqueUnhookCode(this, (PVOID*)&Original_Surface_UnlockRect);
	UniqueUnhookCode(this, (PVOID*)&Original_Texture_UnlockRect);
	UniqueUnhookCode(this, (PVOID*)&Original_TexSurface_PDef_UnlockRect);
	UniqueUnhookCode(this, (PVOID*)&Original_DepthSurface_UnlockRect);	
}

CMonoSwapChain* CWideStereoRenderer::CreateSwapChainWrapper(UINT index)
{
	return DNew CWideSwapChain(this, index);
}

HRESULT GetNarrowRect(RECT *pDestRect)
{
	HRESULT hResult = S_OK;
	NSPTR(pDestRect);
	pDestRect->bottom /= HEIGHTMUL;
	return hResult;
}

HRESULT GetRightViewRect(RECT *pDestRect, CONST RECT *pSourceRect, CONST SIZE* pOffset)
{
	HRESULT hResult = S_OK;
	NSPTR(pDestRect);
	NSPTR(pOffset);
	pDestRect->left = pSourceRect->left;
	pDestRect->right = pSourceRect->right;
	pDestRect->top = pSourceRect->top + pOffset->cy;
	pDestRect->bottom = pSourceRect->bottom + pOffset->cy;
	return hResult;
}

HRESULT GetRightViewRect(D3DRECT *pDestRect, CONST D3DRECT *pSourceRect, CONST SIZE* pOffset)
{
	return GetRightViewRect((RECT *)pDestRect, (CONST RECT *)pSourceRect, pOffset);
}

HRESULT GetRightViewPort(D3DVIEWPORT9 *pDestViewPort, CONST D3DVIEWPORT9 *pSourceViewPort, CONST SIZE* pOffset)
{
	HRESULT hResult = S_OK;
	NSPTR(pDestViewPort);
	NSPTR(pSourceViewPort);
	NSPTR(pOffset);
	pDestViewPort->X        = pSourceViewPort->X;
	pDestViewPort->Y        = pSourceViewPort->Y + pOffset->cy;
	pDestViewPort->Width    = pSourceViewPort->Width;
	pDestViewPort->Height   = pSourceViewPort->Height;
	pDestViewPort->MinZ     = pSourceViewPort->MinZ;
	pDestViewPort->MaxZ     = pSourceViewPort->MaxZ;
	return hResult;
}

HRESULT GetNarrowViewPort(D3DVIEWPORT9 *pDestViewPort, CONST D3DVIEWPORT9 *pSourceViewPort)
{
	HRESULT hResult = S_OK;
	NSPTR(pDestViewPort);
	NSPTR(pSourceViewPort);
	pDestViewPort->X        = pSourceViewPort->X;
	pDestViewPort->Y        = pSourceViewPort->Y;
	pDestViewPort->Width    = pSourceViewPort->Width;
	pDestViewPort->Height   = pSourceViewPort->Height / HEIGHTMUL;
	pDestViewPort->MinZ     = pSourceViewPort->MinZ;
	pDestViewPort->MaxZ     = pSourceViewPort->MaxZ;
	return hResult;
}

HRESULT GetNarrowViewPort(D3DVIEWPORT9 *pDestViewPort)
{
	HRESULT hResult = S_OK;
	NSPTR(pDestViewPort);
	pDestViewPort->Height /= HEIGHTMUL;
	return hResult;
}

void GetSurfRect( D3DSURFACE_DESC *pDesc, RECT *pDestRect )
{
	SetRect(pDestRect, 0, 0, pDesc->Width, pDesc->Height);
}

HRESULT GetNarrowRect( IDirect3DSurface9 *pSurface, RECT *pDestRect )
{
	HRESULT hResult = S_OK;
	D3DSURFACE_DESC Desc;
	NSCALL(pSurface->GetDesc(&Desc));
	GetSurfRect(&Desc, pDestRect);
	GetNarrowRect(pDestRect);
	return hResult;
}

HRESULT CWideStereoRenderer::StartEngineMode() 
{
	INSIDE;
	HRESULT hResult = S_OK;
	m_PSPipelineDataEx.Init();
	m_VSPipelineDataEx.Init();
	if (!gInfo.MonoDepthStencilTextures)
	{
		if (m_bUseR32FDepthTextures)
		{
			const DWORD* shaderCode = (m_DepthTexturesFormat24 != FOURCC_RAWZ) ? (DWORD*)g_ps20_shaderSimple : (DWORD*)g_ps20_shaderSimpleRAWZ;
			NSCALL(m_Direct3DDevice.CreatePixelShader(shaderCode, &m_pDepthShader));
		}
		else
		{
			const DWORD* shaderCode = (m_DepthTexturesFormat24 != FOURCC_RAWZ) ? (DWORD*)g_ps20_shaderSimpleDepth : (DWORD*)g_ps20_shaderSimpleDepthRAWZ;
			NSCALL(m_Direct3DDevice.CreatePixelShader(shaderCode, &m_pDepthShader));
			NSCALL(m_Direct3DDevice.CreateRenderTarget(1, 1, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &m_pSmallSurface, NULL));
		}
		CComPtr<IDirect3DStateBlock9>   pSavedState;
		NSCALL(m_Direct3DDevice.CreateStateBlock(D3DSBT_ALL, &pSavedState));
		{
			if (m_bUseR32FDepthTextures)
			{
				m_Direct3DDevice.SetRenderState(D3DRS_ZWRITEENABLE,        FALSE);
				m_Direct3DDevice.SetRenderState(D3DRS_ZENABLE,             FALSE);
			}
			else
			{
				m_Direct3DDevice.SetRenderState(D3DRS_ZWRITEENABLE,        TRUE);
				m_Direct3DDevice.SetRenderState(D3DRS_ZENABLE,             TRUE);
				m_Direct3DDevice.SetRenderState(D3DRS_ZFUNC,               D3DCMP_ALWAYS);
				m_Direct3DDevice.SetRenderState(D3DRS_COLORWRITEENABLE,	 0);
			}
			m_Direct3DDevice.SetTexture(0, 0);
			m_Direct3DDevice.SetTexture(1, 0);
			m_Direct3DDevice.SetTexture(2, 0);
			m_Direct3DDevice.SetTexture(3, 0);
			m_Direct3DDevice.SetTexture(4, 0);
			m_Direct3DDevice.SetTexture(5, 0);
			m_Direct3DDevice.SetTexture(6, 0);
			m_Direct3DDevice.SetTexture(7, 0);
			m_Direct3DDevice.SetRenderState(D3DRS_WRAP0,               0);
			m_Direct3DDevice.SetRenderState(D3DRS_ALPHABLENDENABLE,    FALSE);
			m_Direct3DDevice.SetRenderState(D3DRS_ALPHATESTENABLE,     FALSE);
			m_Direct3DDevice.SetRenderState(D3DRS_FOGENABLE,           FALSE);
			m_Direct3DDevice.SetRenderState(D3DRS_FILLMODE,            D3DFILL_SOLID);
			m_Direct3DDevice.SetRenderState(D3DRS_CULLMODE,            D3DCULL_NONE);
			m_Direct3DDevice.SetRenderState(D3DRS_LIGHTING,			FALSE);
			m_Direct3DDevice.SetRenderState(D3DRS_STENCILENABLE,		FALSE);
			m_Direct3DDevice.SetRenderState(D3DRS_CLIPPING,			FALSE);
			m_Direct3DDevice.SetRenderState(D3DRS_CLIPPLANEENABLE,		FALSE);
			m_Direct3DDevice.SetRenderState(D3DRS_SRGBWRITEENABLE,		FALSE);

			m_Direct3DDevice.SetFVF( D3DFVF_D3DTVERTEX_1T );
			m_Direct3DDevice.SetPixelShader( m_pDepthShader );
			NSCALL(m_Direct3DDevice.CreateStateBlock(D3DSBT_ALL, &m_pState));
		}
		NSCALL(pSavedState->Apply());
	}

	m_RenderTarget[0].Init(GetWideSC()->m_pWidePrimaryBackBufferBeforeScaling, NULL);
	m_pCurOffset = m_pOffset;

	return hResult;
}

void CWideStereoRenderer::StopEngineMode() 
{
	m_PSPipelineDataEx.Clear();
	m_VSPipelineDataEx.Clear();

	if (!gInfo.MonoDepthStencilTextures)
	{
		m_pDepthShader.Release();
		m_pSmallSurface.Release();
		m_pState.Release();
	}
}

void CWideStereoRenderer::ViewStagesSynchronization()
{
	CheckSynchronizeAll();
	UpdateRTsRevision(m_dwColorWriteEnable, m_dwZWriteEnable != 0 || m_dwStencilEnable != 0 || TRUE);
	SelectOffset(m_dwColorWriteEnable[0] != 0, (LONG)m_ViewPort.Width, (LONG)m_ViewPort.Height);
}

bool CWideStereoRenderer::RenderInStereo()
{
	return (m_pCurOffset != NULL);
}

bool CWideStereoRenderer::StereoDS()
{
	if (m_DepthStencil.m_pMainSurf == NULL)
		return true;
	else
	{
		CComPtr<IWideSurfaceData> pWideSurfaceData = GetWideSurfaceData(m_DepthStencil.m_pMainSurf);
		return pWideSurfaceData != NULL;
	}
}

// Should be synchronized with CStereoRenderer::RenderInStereo
void CWideStereoRenderer::SelectOffset(bool bCW, LONG Width, LONG Height)
{
	if (m_pOffset) // SR: (m_RenderTarget[0].pRightSurf != NULL)
	{
		if (m_pDepthOffset && (/*m_pOffset->cx != m_pDepthOffset->cx || */m_pOffset->cy != m_pDepthOffset->cy))
		{
			CComPtr<IWideSurfaceData> wsd = GetWideSurfaceData(m_RenderTarget[0].m_pMainSurf);
			CONST SIZE* s = wsd->GetSize();
			if (Width <= s->cx && Height <= s->cy) {
				// return m_pOffset even if CW disabled (see Alone In the Dark)
				m_pCurOffset = m_pOffset;
			} else {
				m_pCurOffset = m_pDepthOffset;	
			}
		}
		else
		{
			if (bCW || m_pDepthOffset)
				m_pCurOffset = m_pOffset;
			else
				m_pCurOffset = NULL;
		}
	}
	else
	{
		if (bCW)
			m_pCurOffset = NULL;
		else // Should be false if colorwrite disabled or RT format == NULL
			m_pCurOffset = m_pDepthOffset;
	}
	DEBUG_TRACE3(_T("\t CurOffset=%s Offset=%s DepthOffset=%s\n"), GetOffsetString(m_pCurOffset), 
		GetOffsetString(m_pOffset), GetOffsetString(m_pDepthOffset));
}

template <VIEWINDEX viewIndex>
void CWideStereoRenderer::SetStereoRenderTarget( bool bDraw ) 
{
	HRESULT hResult = S_OK;

	if (bDraw && m_PSPipelineData.m_bShaderDataAvailable && m_PSPipelineData.m_CurrentShaderData.vPosUsed)
	{
		DEBUG_TRACE3(_T("\tUsed modified pixel shader\n"));
		if (viewIndex != VIEWINDEX_RIGHT)
		{
			float vec[4] = { 0, 0, 0, 0 };
			NSCALL(m_Direct3DDevice.SetPixelShaderConstantF(m_PSPipelineData.m_CurrentShaderData.add4VectorRegister, vec, 1));
		}
		else
		{
			float vec[4] = { (float)m_pCurOffset->cx, (float)m_pCurOffset->cy, 0, 0 };
			NSCALL(m_Direct3DDevice.SetPixelShaderConstantF(m_PSPipelineData.m_CurrentShaderData.add4VectorRegister, vec, 1));
		}
	}

	D3DVIEWPORT9 ViewPort;
	D3DVIEWPORT9* pViewPort;
	HRESULT hViewport = S_OK;

	if (viewIndex != VIEWINDEX_RIGHT)
		pViewPort = &m_ViewPort;
	else
	{
		GetRightViewPort(&ViewPort, &m_ViewPort, m_pCurOffset);
		pViewPort = &ViewPort; 
	}
	NSCALL(hViewport = m_Direct3DDevice.SetViewport(pViewPort));

	RECT ScissorRect;
	RECT* pScissorRect;
	if (viewIndex != VIEWINDEX_RIGHT)
		pScissorRect = &m_ScissorRect;
	else
	{
		GetRightViewRect(&ScissorRect, &m_ScissorRect, m_pCurOffset);
		pScissorRect = &ScissorRect;
	}
	HRESULT hScissorRect = S_OK;
	NSCALL(hScissorRect = m_Direct3DDevice.SetScissorRect(pScissorRect));
	DEBUG_TRACE3(_T("\t%s: SetViewport(%s)=%s SetScissorRect(%s)=%s\n"), 
		GetViewString(viewIndex), 
		GetViewPortString(pViewPort), DXGetErrorString(hViewport),
		GetRectString(pScissorRect), DXGetErrorString(hScissorRect));
}

CComPtr<IWideSurfaceData> CWideStereoRenderer::GetWideSurfaceData( IDirect3DSurface9* pSurf )
{
	DWORD size = sizeof(IUnknown *);
	CComPtr<IWideSurfaceData> pWideSurfaceData;
	if (SUCCEEDED(pSurf->GetPrivateData(__uuidof(IWideSurfaceData), (void *)&pWideSurfaceData, &size)))
		return pWideSurfaceData;
	else
		return NULL;	
}

CONST SIZE* CWideStereoRenderer::GetOffset( IDirect3DSurface9* pSurf )
{
	CComPtr<IWideSurfaceData> pWideSurfaceData = GetWideSurfaceData(pSurf);
	if (pWideSurfaceData)
		return pWideSurfaceData->GetOffset();
	else
		return NULL;
}

void	CWideStereoRenderer::CheckSynchronizeAll()
{
	DEBUG_TRACE3(_T("CheckSynchronizeRT()\n"));
	for (DWORD i = 0; i <  m_dwNumSimultaneousRTs; i++)
	{
		if(m_RenderTarget[i].m_pMainTex)
			CheckSynchronizeRT(m_RenderTarget[i].m_pMainSurf);
	}
	DEBUG_TRACE3(_T("CheckSynchronizeTexture()\n"));
	if (DEBUG_SKIP_SYNCHRONIZE_DEPTH_TEXTURES && DEBUG_SKIP_SYNCHRONIZE_TEXTURES)
		return;
	DWORD PSStereoTexturesMask = m_PSPipelineDataEx.m_WideTexturesMask & m_PSPipelineData.m_CurrentShaderData.textures;
	if (PSStereoTexturesMask)
	{
		DWORD CurrentMask = 1;
		for (DWORD i = 0; i <  m_PSPipelineData.m_MaxTextures; i++)
		{
			if (PSStereoTexturesMask & CurrentMask)
			{
				IDirect3DTexture9* pLeftTex = (IDirect3DTexture9*)m_PSPipelineData.m_Texture[i].m_pLeft;
				IDirect3DTexture9* pRightTex = NULL;
				if(RenderToRight())
					pRightTex = (IDirect3DTexture9*)m_PSPipelineData.m_Texture[i].m_pRight.p;
				CheckSynchronizeTexture(m_PSPipelineData.m_Texture[i].m_pWideTexture, pLeftTex, pRightTex);
			}
			CurrentMask <<= 1;
		}
	}
	DWORD VSStereoTexturesMask = m_VSPipelineDataEx.m_WideTexturesMask & m_VSPipelineData.m_CurrentShaderData.textures;
	if (VSStereoTexturesMask)
	{
		DWORD CurrentMask = 1;
		for (DWORD i = 0; i <  m_VSPipelineData.m_MaxTextures; i++)
		{
			if (VSStereoTexturesMask & CurrentMask)
			{
				IDirect3DTexture9* pLeftTex = (IDirect3DTexture9*)m_VSPipelineData.m_Texture[i].m_pLeft;
				IDirect3DTexture9* pRightTex = NULL;
				if(RenderToRight())
					pRightTex = (IDirect3DTexture9*)m_VSPipelineData.m_Texture[i].m_pRight.p;
				CheckSynchronizeTexture(m_VSPipelineData.m_Texture[i].m_pWideTexture, pLeftTex, pRightTex);
			}
			CurrentMask <<= 1;
		}
	}
}

void	CWideStereoRenderer::UpdateRTsRevision(DWORD updateRTs[], bool updateDS)
{
	DEBUG_TRACE3(_T("UpdateRTsRevision();\n"));
	// update only revision for surfaces from texture
	if (updateDS && m_DepthStencil.m_pMainTex)
		IncRevision(m_DepthStencil.m_pMainSurf);
	if (updateRTs[0] && m_RenderTarget[0].m_pMainTex)
		IncRevision(m_RenderTarget[0].m_pMainSurf);
	if (updateRTs[1] && m_RenderTarget[1].m_pMainTex)
		IncRevision(m_RenderTarget[1].m_pMainSurf);
	if (updateRTs[2] && m_RenderTarget[2].m_pMainTex)
		IncRevision(m_RenderTarget[2].m_pMainSurf);
	if (updateRTs[3] && m_RenderTarget[3].m_pMainTex)
		IncRevision(m_RenderTarget[3].m_pMainSurf);
}

template <VIEWINDEX view>
HRESULT CWideStereoRenderer::SynchronizeRT( IDirect3DSurface9* pSurf, IDirect3DSurface9* pWideSurf )
{
	INSIDE;
	HRESULT hResult = S_OK;

	DEBUG_TRACE2(_T("\tSynchronizeRT(%s, pSurf = %p [%s], pWideSurf = %p [%s])\n"), 
		GetViewString(view), 
		pSurf, GetObjectName(pSurf), 
		pWideSurf, GetObjectName(pWideSurf));
	D3DSURFACE_DESC Desc;
	RECT NarrowRect;
	RECT Rect;

	NSCALL(pWideSurf->GetDesc(&Desc));
	GetSurfRect(&Desc, &NarrowRect);
	GetNarrowRect(&NarrowRect);
	RECT* pRect;
	if (view != VIEWINDEX_RIGHT)
		pRect = &NarrowRect;
	else
	{
		CONST SIZE* pOffset = GetOffset(pWideSurf);
		GetRightViewRect(&Rect, &NarrowRect, pOffset);
		pRect = &Rect; 
	}

	DEBUG_TRACE3(_T("\t\tNarrowRect =\t%s\n"), GetRectString(&NarrowRect));
	DEBUG_TRACE3(_T("\t\tRect =\t%s\n"), GetRectString(pRect));
	NSCALL(m_Direct3DDevice.StretchRect(pSurf, NULL, pWideSurf, pRect, D3DTEXF_NONE));

	return hResult;
}

template <VIEWINDEX view>
HRESULT CWideStereoRenderer::SynchronizeTexture( IDirect3DTexture9* pWideTex, IDirect3DTexture9* pTex )
{  
	INSIDE;
	HRESULT hResult = S_OK;

	DEBUG_TRACE2(_T("\tSynchronizeTexture(%s, pWideTex = %p [%s], pTex = %p [%s])\n"), 
		GetViewString(view), 
		pWideTex, GetObjectName(pWideTex), 
		pTex, GetObjectName(pTex));

	if (DEBUG_SKIP_SYNCHRONIZE_TEXTURES)
		return hResult;

	for (DWORD j = 0; j < pTex->GetLevelCount(); j++)
	{
		CComPtr<IDirect3DSurface9> pWideSurf;
		NSCALL(pWideTex->GetSurfaceLevel(j, &pWideSurf));
		D3DSURFACE_DESC Desc;
		RECT NarrowRect;
		RECT Rect;

		NSCALL(pWideSurf->GetDesc(&Desc));
		GetSurfRect(&Desc, &NarrowRect);
		GetNarrowRect(&NarrowRect);
		RECT* pRect;
		if (view != VIEWINDEX_RIGHT)
			pRect = &NarrowRect;
		else
		{
			CONST SIZE* pOffset = GetOffset(pWideSurf);
			GetRightViewRect(&Rect, &NarrowRect, pOffset);
			pRect = &Rect; 
		}

		DEBUG_TRACE3(_T("\t\tL%d NarrowRect =\t%s\n"), j, GetRectString(&NarrowRect));
		DEBUG_TRACE3(_T("\t\tL%d Rect =\t%s\n"), j, GetRectString(pRect));
		CComPtr<IDirect3DSurface9> pSurf;
		pTex->GetSurfaceLevel(j, &pSurf);
		NSCALL(m_Direct3DDevice.StretchRect(pWideSurf, pRect, pSurf, NULL, D3DTEXF_NONE));
	}
	return hResult;
}

template <VIEWINDEX view>
HRESULT CWideStereoRenderer::SynchronizeDepthTexture( IDirect3DTexture9* pWideTex, IDirect3DTexture9* pTex)
{  
	INSIDE;
	HRESULT hResult = S_OK;

	DEBUG_TRACE2(_T("\tSynchronizeDepthTexture(%s, pWideTex = %p [%s], pTex = %p [%s])\n"), 
		GetViewString(view), 
		pWideTex, GetObjectName(pWideTex), 
		pTex, GetObjectName(pTex));
	if (DEBUG_SKIP_SYNCHRONIZE_DEPTH_TEXTURES)
		return hResult;

	D3DTVERTEX_1T vertex[4];
	vertex[0].z   = 0;
	vertex[0].rhw = 1;    
	vertex[1].z   = 0;
	vertex[1].rhw = 1;    
	vertex[2].z   = 0;
	vertex[2].rhw = 1;    
	vertex[3].z   = 0;
	vertex[3].rhw = 1;    
	D3DVIEWPORT9 Viewport;
	if (!m_bUseR32FDepthTextures)
	{
		Viewport.X = 0;
		Viewport.Y = 0;
		Viewport.MinZ = 0.0f;
		Viewport.MaxZ = 1.0f;
	}
	NSCALL(m_Direct3DDevice.SetTexture(0, pWideTex));
	for (DWORD j = 0; j < pWideTex->GetLevelCount(); j++)
	{
		CComPtr<IDirect3DSurface9> pWideSurf;
		NSCALL(pWideTex->GetSurfaceLevel(j, &pWideSurf));
		D3DSURFACE_DESC Desc;
		RECT NarrowRect;
		RECT Rect;

		NSCALL(pWideSurf->GetDesc(&Desc));
		GetSurfRect(&Desc, &NarrowRect);
		GetNarrowRect(&NarrowRect);
		RECT* pRect;
		if (view != VIEWINDEX_RIGHT)
			pRect = &NarrowRect;
		else
		{
			CONST SIZE* pOffset = GetOffset(pWideSurf);
			GetRightViewRect(&Rect, &NarrowRect, pOffset);
			pRect = &Rect; 
		}

		DEBUG_TRACE3(_T("\t\tL%d NarrowRect =\t%s\n"), j, GetRectString(&NarrowRect));
		DEBUG_TRACE3(_T("\t\tL%d Width =\t%d\tHeight =\t%d\n"), j, Desc.Width, Desc.Height);
		DEBUG_TRACE3(_T("\t\tL%d Rect =\t%s\n"), j, GetRectString(pRect));

		CComPtr<IDirect3DSurface9> pSurf;
		pTex->GetSurfaceLevel(j, &pSurf);
		vertex[0].x   = NarrowRect.left - .5f;
		vertex[0].y   = NarrowRect.top - .5f;
		vertex[0].tu  = 1.0f * pRect->left / Desc.Width;
		vertex[0].tv  = 1.0f * pRect->top / Desc.Height;    

		vertex[1].x   = NarrowRect.right - .5f;
		vertex[1].y   = NarrowRect.top - .5f;
		vertex[1].tu  = 1.0f * pRect->right / Desc.Width;    
		vertex[1].tv  = 1.0f * pRect->top / Desc.Height;    

		vertex[2].x   = NarrowRect.right - .5f;
		vertex[2].y   = NarrowRect.bottom - .5f;
		vertex[2].tu  = 1.0f * pRect->right / Desc.Width;    
		vertex[2].tv  = 1.0f * pRect->bottom / Desc.Height;    

		vertex[3].x   = NarrowRect.left - .5f;
		vertex[3].y   = NarrowRect.bottom - .5f;
		vertex[3].tu  = 1.0f * pRect->left / Desc.Width;    
		vertex[3].tv  = 1.0f * pRect->bottom / Desc.Height; 
		if (m_bUseR32FDepthTextures) {
			NSCALL(m_Direct3DDevice.SetRenderTarget(0, pSurf));
		} else {
			NSCALL(m_Direct3DDevice.SetDepthStencilSurface(pSurf));
			Viewport.Width = NarrowRect.right;
			Viewport.Height = NarrowRect.bottom;
			NSCALL(m_Direct3DDevice.SetViewport(&Viewport));
			NSCALL(m_Direct3DDevice.SetScissorRect(&NarrowRect));
		}
		NSCALL(m_Direct3DDevice.DrawPrimitiveUP(D3DPT_TRIANGLEFAN, 2, vertex, sizeof(vertex[0])));
	}
	return hResult;
}

HRESULT CWideStereoRenderer::CheckSynchronizeRT( IDirect3DSurface9* pWideSurface )
{
	if (DEBUG_SKIP_SYNCHRONIZE_DEPTH_TEXTURES && DEBUG_SKIP_SYNCHRONIZE_TEXTURES)
		return S_OK;

	IDirect3DSurface9* pLeftSurface;
	if (!GetLeftSurface(pWideSurface, &pLeftSurface))
		return S_OK;

	DWORD revWide = GetSurfaceRevision(pWideSurface);
	DWORD revLeft = GetSurfaceRevision(pLeftSurface);
	DWORD revRight = 0;
	if (revLeft <= revWide)
		return S_OK;

#ifndef FINAL_RELEASE
	LARGE_INTEGER time;
	IDirect3DQuery9* pQuery = GetBaseDevice()->m_pFlushQuery;
	if (GINFO_DEBUG && m_Input.DebugInfoPage == 3)
	{
		_ASSERT(pQuery);
		pQuery->Issue(D3DISSUE_END);
		while(S_FALSE == pQuery->GetData( NULL, 0, D3DGETDATA_FLUSH ));

		QueryPerformanceCounter(&time);
	}
#endif
	SynchronizeRT<VIEWINDEX_LEFT>(pLeftSurface, pWideSurface);

	if (RenderToRight())
	{
		CComPtr<IDirect3DSurface9> pRightSurface;
		GetStereoObject(pLeftSurface, &pRightSurface);
		DWORD revRight = GetSurfaceRevision(pRightSurface);
		if (revRight > revWide)
			SynchronizeRT<VIEWINDEX_RIGHT>(pRightSurface, pWideSurface);
	}
	SetSurfaceRevision(pWideSurface, std::max(revLeft, revRight));
#ifndef FINAL_RELEASE
	if (GINFO_DEBUG && m_Input.DebugInfoPage == 3)
	{
		pQuery->Issue(D3DISSUE_END);
		while(S_FALSE == pQuery->GetData( NULL, 0, D3DGETDATA_FLUSH ));

		LARGE_INTEGER stop;
		QueryPerformanceCounter(&stop);
		m_nTexSynchRTTime.QuadPart += stop.QuadPart - time.QuadPart;
		m_nTexSynchRTCounter++;
	}
#endif
	return S_OK;
}

HRESULT CWideStereoRenderer::CheckSynchronizeTexture( IDirect3DTexture9* pWideTex, IDirect3DTexture9* pLeftTexture, IDirect3DTexture9* pRightTexture )
{
	INSIDE;
	HRESULT hResult = S_OK;
	BOOL NeedUpdateLeft = false;
	BOOL NeedUpdateRight = false;
	for (DWORD i = 0; i < pLeftTexture->GetLevelCount(); i++) // WideTex may contain more levels if auto mipmaps generating on
	{
		CComPtr<IDirect3DSurface9> pWideSurface;
		pWideTex->GetSurfaceLevel(i, &pWideSurface);

		DWORD revWide = GetSurfaceRevision(pWideSurface);
		CComPtr<IDirect3DSurface9> pLeftSurface;
		pLeftTexture->GetSurfaceLevel(i, &pLeftSurface);
		DWORD revLeft = GetSurfaceRevision(pLeftSurface);
		if (revWide > revLeft)
		{
			NeedUpdateLeft = true;
			SetSurfaceRevision(pLeftSurface, revWide);
		} 

		if (pRightTexture)
		{
			CComPtr<IDirect3DSurface9> pRightSurface;
			pRightTexture->GetSurfaceLevel(i, &pRightSurface);
			DWORD revRight = GetSurfaceRevision(pRightSurface);
			if (revWide > revRight)
			{
				NeedUpdateRight = true;
				SetSurfaceRevision(pRightSurface, revWide);
			}
		}
	}
	if(!NeedUpdateLeft)
		INC_DEBUG_COUNTER(m_nTexSynchCachedCounter, 1);
	if(pRightTexture && !NeedUpdateRight)
		INC_DEBUG_COUNTER(m_nTexSynchCachedCounter, 1);

	if(!NeedUpdateLeft && !NeedUpdateRight)
		return hResult;

#ifndef FINAL_RELEASE
	LARGE_INTEGER time;
	IDirect3DQuery9* pQuery = GetBaseDevice()->m_pFlushQuery;
	if (GINFO_DEBUG && m_Input.DebugInfoPage == 3)
	{
		_ASSERT(pQuery);
		pQuery->Issue(D3DISSUE_END);
		while(S_FALSE == pQuery->GetData( NULL, 0, D3DGETDATA_FLUSH ));

		QueryPerformanceCounter(&time);
	}
#endif
	D3DSURFACE_DESC Desc;
	BOOL Depth = FALSE;
	if (!gInfo.MonoDepthStencilTextures)
	{
		pWideTex->GetLevelDesc(0, &Desc);
		Depth = (Desc.Usage & D3DUSAGE_DEPTHSTENCIL);
	}
	if (!Depth)
	{
		if (!DEBUG_SKIP_SYNCHRONIZE_TEXTURES)
		{
			if (NeedUpdateLeft)
				SynchronizeTexture<VIEWINDEX_LEFT>(pWideTex, pLeftTexture);
			if (NeedUpdateRight)
				SynchronizeTexture<VIEWINDEX_RIGHT>(pWideTex, pRightTexture);
		}
	}
	else if (!DEBUG_SKIP_SYNCHRONIZE_DEPTH_TEXTURES)
	{
		vbs* vb;
		CaptureAll(vb);
		NSCALL(m_pState->Apply());

		std::vector<CAdapt<CComPtr<IDirect3DSurface9> > > pRenderTarget(m_dwNumSimultaneousRTs);
		for (DWORD i = 0; i < m_dwNumSimultaneousRTs; i++)
			if (m_RenderTarget[i].m_pMainSurf)
				pRenderTarget[i].m_T = m_RenderTarget[i].m_pMainSurf;			// hold reference
		CComPtr<IDirect3DSurface9> pDepthStencilSurface;
			if (m_DepthStencil.m_pMainSurf)
				pDepthStencilSurface = m_DepthStencil.m_pMainSurf;				// hold reference

		for (DWORD i = 1; i < m_dwNumSimultaneousRTs; i++)
			if (m_RenderTarget[i].m_pMainSurf)
				m_Direct3DDevice.SetRenderTarget(i, NULL);

		if (m_bUseR32FDepthTextures)
		{
			if (m_DepthStencil.m_pMainSurf)
				m_Direct3DDevice.SetDepthStencilSurface(NULL);
		}
		else
			m_Direct3DDevice.SetRenderTarget(0, m_pSmallSurface);
		if (NeedUpdateLeft)
			SynchronizeDepthTexture<VIEWINDEX_LEFT>(pWideTex, pLeftTexture);
		if (NeedUpdateRight)
			SynchronizeDepthTexture<VIEWINDEX_RIGHT>(pWideTex, pRightTexture);

		ApplyAll(vb);
	}
#ifndef FINAL_RELEASE
	if (GINFO_DEBUG && m_Input.DebugInfoPage == 3)
	{
		pQuery->Issue(D3DISSUE_END);
		while(S_FALSE == pQuery->GetData( NULL, 0, D3DGETDATA_FLUSH ));

		LARGE_INTEGER stop;
		QueryPerformanceCounter(&stop);
		m_nTexSynchTime.QuadPart += stop.QuadPart - time.QuadPart;
		m_nTexSynchCounter++;
	}
#endif
	return hResult;
}

bool CWideStereoRenderer::CopyDepthSurfaces(IDirect3DTexture9* pDepthTexure, CComPtr<IDirect3DSurface9> &pLeft, CComPtr<IDirect3DSurface9> &pRight)
{
	DEBUG_TRACE3(_T("CopyDepthSurfaces\n"));
	IDirect3DTexture9* pTex = pDepthTexure;
	if(!pTex)
		return false;
	CComPtr<IDirect3DTexture9> pLeftTex;
	if (!GetLeftTexture(pTex, &pLeftTex))
		return false;
	IDirect3DTexture9* pWideTex = GetWideTexture(pTex, pLeftTex);
	if(!pWideTex)
		return false;
	DEBUG_TRACE3(_T("\tpLeftTex = %p [%s]\n"), pLeftTex.p, GetObjectName(pLeftTex));
	CComPtr<IDirect3DTexture9> pRightTex;
	if(RenderToRight())
		GetStereoObject(pLeftTex, &pRightTex);
	if (!m_bRenderScene)
		m_Direct3DDevice.BeginScene();
	CheckSynchronizeTexture(pWideTex, pLeftTex, pRightTex);
	if (!m_bRenderScene)
		m_Direct3DDevice.EndScene();
	pLeft.Release();
	pLeftTex->GetSurfaceLevel(0, &pLeft);
	pRight.Release();
	if(pRightTex)
		pRightTex->GetSurfaceLevel(0, &pRight);
	return true;
}

void CWideStereoRenderer::SynchronizeDepthBuffer( CBaseSwapChain* pSwapChain )
{
	if (!pSwapChain->m_pLeftDepthStencilTexture || !pSwapChain->m_pRightDepthStencilTexture)
		return;

	CWideSwapChain* pWideSwapChain = (CWideSwapChain*) pSwapChain;
	DEBUG_TRACE3(_T("SynchronizeDepthBuffer\n"));
	IDirect3DTexture9* pLeftTex = pWideSwapChain->m_pLeftDepthStencilTexture;
	IDirect3DTexture9* pRightTex = pWideSwapChain->m_pRightDepthStencilTexture;
	IDirect3DTexture9* pWideTex;
	GetParentTexture(pWideSwapChain->m_pMainDepthStencilSurface, &pWideTex);
	if(!pWideTex)
		return;
	DEBUG_TRACE3(_T("\tpWideTex = %p [%s]\n"), pWideTex, GetObjectName(pWideTex));
	CheckSynchronizeTexture(pWideTex, pLeftTex, pRightTex);
}

STDMETHODIMP CWideStereoRenderer::SetRenderTarget(DWORD RenderTargetIndex, IDirect3DSurface9 *pRenderTarget)
{
	DUMP_CMD(SetRenderTarget,RenderTargetIndex,pRenderTarget);

	HRESULT hResult = S_OK;

	// 0.5.0 task #77 round 9: translate m_pPrimaryBackBuffer -> pre-scale wide RT.
	// When round-2 auto-upscale engages, m_pPrimaryBackBuffer is the real swap-chain
	// BB at PANEL dims. The iZ3D design assumed the game would be using
	// m_pWidePrimaryBackBuffer (post-scale wide RT) -- see the original assertion
	// at this line in iZ3D source. But games that cache the initial RT inherited
	// from CreateDevice (UDK, modern engines) end up SetRenderTarget'ing
	// m_pPrimaryBackBuffer. The right destination for them is the PRE-scale wide
	// RT (m_pLeftBackBufferBeforeScaling), so game-dim viewport writes land there
	// and DoScaling's StretchRect bridges pre-scale -> post-scale correctly.
	// `project_task77_udk_window_clamp` memory called this out in October 2026.
	// Belt-and-suspenders with the PostInitialize forced-initial-RT in BaseSwapChain.cpp.
	if (RenderTargetIndex == 0 &&
	    GetBaseSC()->m_bScalingEnabled &&
	    pRenderTarget == GetBaseSC()->m_pPrimaryBackBuffer &&
	    GetBaseSC()->m_pLeftBackBufferBeforeScaling)
	{
		static int s_r9SRTLogCount = 0;
		if (s_r9SRTLogCount < 10)
		{
			D9Log("Task #77 round 9: translated SetRenderTarget m_pPrimaryBackBuffer %p -> m_pLeftBackBufferBeforeScaling %p (idx=%u, log %d/10)\n",
			      (void*)pRenderTarget,
			      (void*)(IDirect3DSurface9*)GetBaseSC()->m_pLeftBackBufferBeforeScaling,
			      RenderTargetIndex, s_r9SRTLogCount);
			s_r9SRTLogCount++;
		}
		pRenderTarget = GetBaseSC()->m_pLeftBackBufferBeforeScaling;
	}

	NSCALL_TRACE2(m_Direct3DDevice.SetRenderTarget(RenderTargetIndex, pRenderTarget),
		DEBUG_MESSAGE(_T("SetRenderTarget(RenderTargetIndex = %d, pRenderTarget = %p [%s])"),
		RenderTargetIndex, pRenderTarget, GetObjectName(pRenderTarget)));

	if (FAILED(hResult))
		return hResult;

	if (RenderTargetIndex == 0)
	{
		if (pRenderTarget)
		{
			D3DSURFACE_DESC desc;
			pRenderTarget->GetDesc(&desc);
			InitViewport(&m_ViewPort, 0, 0, desc.Width, desc.Height);
			m_bFullSizeViewport = TRUE;
			SetRect(&m_ScissorRect, 0, 0, desc.Width, desc.Height);
			m_bFullSizeScissorRect = TRUE;

			m_pOffset = GetOffset(pRenderTarget);
			if (m_pOffset)
			{
				GetNarrowViewPort(&m_ViewPort);
				NSCALL(m_Direct3DDevice.SetViewport(&m_ViewPort));
				GetNarrowRect(&m_ScissorRect);
				NSCALL(m_Direct3DDevice.SetScissorRect(&m_ScissorRect));
			}

			m_PrimaryRTSize.cx = m_ViewPort.Width;
			m_PrimaryRTSize.cy = m_ViewPort.Height;
		}
		else
			m_pOffset = NULL;

		DEBUG_TRACE3(_T("\t Offset=%s SetViewport(%s) SetScissorRect(%s)\n"), 
			GetOffsetString(m_pOffset), GetViewPortString(&m_ViewPort), GetRectString(&m_ScissorRect));
	}

	m_RenderTarget[RenderTargetIndex].Init(pRenderTarget);

	return hResult;
}

STDMETHODIMP CWideStereoRenderer::SetDepthStencilSurface(IDirect3DSurface9 * pNewZStencil) 
{
	DUMP_CMD(SetDepthStencilSurface,pNewZStencil);

	HRESULT hResult = S_OK;

	_ASSERT(pNewZStencil == NULL || pNewZStencil != GetBaseSC()->m_pPrimaryDepthStencil); // should be m_pWidePrimaryDepthStencil

	NSCALL_TRACE2(m_Direct3DDevice.SetDepthStencilSurface(pNewZStencil),
		DEBUG_MESSAGE(_T("SetDepthStencilSurface(pNewZStencil = %p [%s])"), 
		pNewZStencil, GetObjectName(pNewZStencil)));

	if (FAILED(hResult))
		return hResult;

	if (pNewZStencil)
		m_pDepthOffset = GetOffset(pNewZStencil);
	else
		m_pDepthOffset = NULL;
	DEBUG_TRACE3(_T("\t DepthOffset=%s\n"), GetOffsetString(m_pDepthOffset));

	m_DepthStencil.Init(pNewZStencil);

	return hResult;
}

STDMETHODIMP CWideStereoRenderer::SetTexture(DWORD Stage, IDirect3DBaseTexture9* pTexture) 
{
	DUMP_CMD_STATE(SetTexture,Stage,pTexture);

	HRESULT hResult = S_OK;

	CComPtr<IDirect3DBaseTexture9> pLeftTex;
	GetLeftTexture(pTexture, &pLeftTex);
	if(!m_bInStateBlock)
	{
		NSCALL_TRACE2(m_Direct3DDevice.SetTexture(Stage, pLeftTex),
			DEBUG_MESSAGE(_T("SetTexture(Stage = %d, pTexture = %p [%s])"), 
			Stage, pLeftTex.p, GetObjectName(pLeftTex)));

		if(SUCCEEDED(hResult))
		{
			IDirect3DTexture9* pWideTex = GetWideTexture(pTexture, pLeftTex);
			if (Stage < D3DDMAPSAMPLER)
			{
				int ind = Stage;
				SetPipelineTexture(m_PSPipelineDataEx, ind, pLeftTex, pWideTex);
				UpdateYouTubeTexture(Stage);
			}
			else
			{
				int ind = Stage - D3DDMAPSAMPLER;
				SetPipelineTexture(m_VSPipelineDataEx, ind, pLeftTex, pWideTex);
			}
		}
	}
	else
		hResult = m_Direct3DDevice.SetTexture(Stage, pLeftTex);
	return hResult;
}

STDMETHODIMP CWideStereoRenderer::GetTexture(DWORD Stage, IDirect3DBaseTexture9** ppTexture)
{
	HRESULT hResult = S_OK;
	IDirect3DBaseTexture9* pTex = NULL;
	if (Stage < D3DDMAPSAMPLER)
	{
		int ind = Stage;
		pTex = m_PSPipelineData.m_Texture[ind].m_pWideTexture;
		if (pTex == NULL)
			pTex = m_PSPipelineData.m_Texture[ind].m_pLeft;
	}
	else
	{
		int ind = Stage - D3DDMAPSAMPLER;
		pTex = m_VSPipelineData.m_Texture[ind].m_pWideTexture;
		if (pTex == NULL)
			pTex = m_VSPipelineData.m_Texture[ind].m_pLeft;
	}
	if(pTex)
		pTex->AddRef();
	*ppTexture = pTex;
	DEBUG_TRACE2(_T("GetTexture(Stage = %d, *ppTexture = %p [%s])\n"), 
		Stage, Indirect(ppTexture), GetObjectName(*ppTexture, hResult));
	return hResult;
}

STDMETHODIMP CWideStereoRenderer::CreateRenderTargetEx(UINT Width,UINT Height,D3DFORMAT Format,
												   D3DMULTISAMPLE_TYPE MultiSample,DWORD MultisampleQuality,BOOL Lockable,
												   IDirect3DSurface9** ppSurface,HANDLE* pSharedHandle,DWORD Usage)
{
	if (pSharedHandle == NULL)
	{
		return CreateRenderTarget(Width, Height, Format, MultiSample, MultisampleQuality, 
			Lockable, ppSurface, pSharedHandle);
	}

	HRESULT hResult = m_Direct3DDevice.CreateRenderTargetEx(Width, Height, Format, MultiSample, MultisampleQuality, 
		Lockable, ppSurface, pSharedHandle, Usage);
	if (Succeded(hResult))
	{
		IDirect3DSurface9 *pSurface = *ppSurface;
		DUMP_CMD_ALL(CreateRenderTargetEx,Width,Height,Format,MultiSample,MultisampleQuality,Lockable,pSurface,pSharedHandle,Usage);
	}
	return hResult;
}

STDMETHODIMP CWideStereoRenderer::CreateRenderTarget(UINT Width, UINT Height, D3DFORMAT Format, 
													 D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, 
													 BOOL Lockable, IDirect3DSurface9** ppSurface, HANDLE * pSharedHandle) 
{
	HRESULT hResult = S_OK;
	if (!gInfo.EnableAA)
	{
		MultiSample = D3DMULTISAMPLE_NONE;
		MultisampleQuality = 0;
	}
	SIZE Offset;
	UINT NewWidth, NewHeight;
	HRESULT hr = S_OK;
	bool bCreateStereo = false;
	if (IsStereoRenderTargetSurface(Width, Height))
	{
		hr = InitializeOffset(Width, Height, &Offset, NewWidth, NewHeight);
		if (SUCCEEDED(hr))
			bCreateStereo = true;
	}
	else
	{
		NewWidth = Width;
		NewHeight = Height;
	}
	NSCALL_TRACE1(m_Direct3DDevice.CreateRenderTarget( NewWidth, NewHeight, Format, MultiSample, 
		MultisampleQuality, Lockable, ppSurface, pSharedHandle ),
		DEBUG_MESSAGE(_T("CreateRenderTarget(Width = %d, Height = %d, Format = %s, MultiSample = %s,")
		_T("MultisampleQuality = %08X, Lockable = %1d, *ppSurface = %p, pSharedHandle = %p)"), 
		NewWidth, NewHeight, GetFormatString(Format), GetMultiSampleString(MultiSample), 
		MultisampleQuality, Lockable, Indirect(ppSurface), pSharedHandle));

	if(!Succeded(hResult))
		return hResult;

	IDirect3DSurface9 *pSurface = *ppSurface;
	DUMP_CMD_ALL(CreateRenderTarget,Width,Height,Format,MultiSample,MultisampleQuality,Lockable,pSurface,pSharedHandle);

	UINT SizeInBytes = GetSurfaceSize(*ppSurface);
	if (bCreateStereo)
	{
		CComPtr<IWideSurfaceData> pWideSurfaceData;
		CWideSurfaceData::CreateInstance(&pWideSurfaceData);
		pWideSurfaceData->SetOffset(&Offset);
		SIZE Size;
		Size.cx = Width;
		Size.cy = Height;
		pWideSurfaceData->SetSize(&Size);
		pWideSurfaceData->SetFormat(Format);
		pWideSurfaceData->SetUsage(D3DUSAGE_RENDERTARGET);
		NSCALL((*ppSurface)->SetPrivateData(__uuidof(IWideSurfaceData), (void *)pWideSurfaceData, sizeof(IUnknown *), D3DSPD_IUNKNOWN)); 

		//--- It's OK here, because RenderTarget is always D3DPOOL_DEFAULT ---
		void** p = *(void***)(*ppSurface);
		UniqueHookCode(this, p[12], Proxy_Surface_GetDesc, (PVOID*)&Original_Surface_GetDesc);
		if( !IsCodeHookedByProxy(p[13], Proxy_DepthSurface_LockRect) )
			UniqueHookCode(this, p[13], Proxy_Surface_LockRect, (PVOID*)&Original_Surface_LockRect);
		if( !IsCodeHookedByProxy(p[14], Proxy_DepthSurface_UnlockRect) )
			UniqueHookCode(this, p[14], Proxy_Surface_UnlockRect, (PVOID*)&Original_Surface_UnlockRect);
	}
#ifndef FINAL_RELEASE
	TCHAR buffer[128];
	if (bCreateStereo)
	{
		_sntprintf_s(buffer, _TRUNCATE, _T("WideRenderTarget%d"), m_nRenderTargetCounter);
		m_nRenderTargetsMonoSize += SizeInBytes / 2;
		m_nRenderTargetsStereoSize += SizeInBytes / 2;
	}
	else
	{
		_sntprintf_s(buffer, _TRUNCATE, _T("RenderTargetMono%d"), m_nRenderTargetCounter);
		m_nRenderTargetsMonoSize += SizeInBytes;
	}
	SetObjectName(*ppSurface, buffer);
	DEBUG_TRACE1(_T("\tSetObjectName(*ppSurface = %p [%s])\n"), *ppSurface, GetObjectName(*ppSurface));
#endif
	m_nRenderTargetCounter++;
	return hResult;
}

STDMETHODIMP CWideStereoRenderer::CreateDepthStencilSurfaceEx(UINT Width,UINT Height,D3DFORMAT Format,
														  D3DMULTISAMPLE_TYPE MultiSample,DWORD MultisampleQuality,
														  BOOL Discard,IDirect3DSurface9** ppSurface,HANDLE* pSharedHandle,DWORD Usage)
{
	if (pSharedHandle == NULL)
	{
		return CreateDepthStencilSurface(Width, Height, Format, MultiSample, MultisampleQuality, 
			Discard, ppSurface, pSharedHandle);
	}

	HRESULT hResult = m_Direct3DDevice.CreateDepthStencilSurfaceEx(Width, Height, Format, MultiSample, MultisampleQuality, 
		Discard, ppSurface, pSharedHandle, Usage);
	if (Succeded(hResult))
	{
		IDirect3DSurface9 *pSurface = *ppSurface;
		DUMP_CMD_ALL(CreateDepthStencilSurfaceEx,Width,Height,Format,MultiSample,MultisampleQuality,Discard,pSurface,pSharedHandle,Usage);
	}
	return hResult;
}

STDMETHODIMP CWideStereoRenderer::CreateDepthStencilSurface(UINT Width, UINT Height, D3DFORMAT Format, 
															D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, 
															BOOL Discard, IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle ) 
{
	HRESULT hResult = S_OK;
	if (!gInfo.EnableAA)
	{
		MultiSample = D3DMULTISAMPLE_NONE;
		MultisampleQuality = 0;
	}
	if (GINFO_LOCKABLEDEPTHSTENCIL && MultiSample == D3DMULTISAMPLE_NONE)
	{
		Format = D3DFMT_D16_LOCKABLE;
		Discard = FALSE;
	}
	SIZE Offset;
	UINT NewWidth, NewHeight;
	HRESULT hr = S_OK;
	bool bCreateStereo = false;
	if (IsStereoDepthStencilSurface(Width, Height)) 
	{
		hr = InitializeOffset(Width, Height, &Offset, NewWidth, NewHeight);
		if (SUCCEEDED(hr))
			bCreateStereo = true;
	}
	else
	{
		NewWidth = Width;
		NewHeight = Height;
	}
	if (!gInfo.MonoDepthStencilTextures && MultiSample == D3DMULTISAMPLE_NONE && bCreateStereo &&
		(GINFO_REPLACEDSSURFACES || (gInfo.ExtractDepthBuffer && !GetBaseSC()->m_pLeftDepthStencilTexture)))
	{
		// debug texture
		CComPtr<IDirect3DTexture9> pWideTex;
		NSCALL(GetBaseDevice()->CreateTexture(Width, Height, 1, D3DUSAGE_DEPTHSTENCIL, 
			GetBaseDevice()->m_DepthTexturesFormat24, D3DPOOL_DEFAULT, &pWideTex, NULL )); // use internal method
		if (Succeded(hResult))
		{
			NSCALL(pWideTex->GetSurfaceLevel(0, ppSurface));
			DEBUG_TRACE1(_T("\tReplaced DS surface by texture\n"));
		}
		return hResult;
	}
	NSCALL_TRACE1(m_Direct3DDevice.CreateDepthStencilSurface(NewWidth, NewHeight, Format, 
		MultiSample, MultisampleQuality, Discard, ppSurface, pSharedHandle ),
		DEBUG_MESSAGE(_T("CreateDepthStencilSurface(Width = %d, Height = %d, Format = %s, MultiSample = %s,")
		_T("MultisampleQuality = %08X, Discard = %1d, *ppSurface = %p, ")
		_T("pSharedHandle = %p)"), NewWidth, NewHeight, GetFormatString(Format), GetMultiSampleString(MultiSample), 
		MultisampleQuality, Discard, Indirect(ppSurface), pSharedHandle));

	if (!Succeded(hResult))
		return hResult;

	IDirect3DSurface9 *pSurface = *ppSurface;
	DUMP_CMD_ALL(CreateDepthStencilSurface,Width,Height,Format,MultiSample,MultisampleQuality,Discard,pSurface,pSharedHandle);

	UINT SizeInBytes = GetSurfaceSize(*ppSurface);
	SetSurfaceType(*ppSurface, Mono);
	if (bCreateStereo) 
	{
		CComPtr<IWideSurfaceData> pWideSurfaceData;
		CWideSurfaceData::CreateInstance(&pWideSurfaceData);
		pWideSurfaceData->SetOffset(&Offset);
		SIZE Size;
		Size.cx = Width;
		Size.cy = Height;
		pWideSurfaceData->SetSize(&Size);
		pWideSurfaceData->SetFormat(Format);
		pWideSurfaceData->SetUsage(D3DUSAGE_DEPTHSTENCIL);
		NSCALL((*ppSurface)->SetPrivateData(__uuidof(IWideSurfaceData), (void *)pWideSurfaceData, sizeof(IUnknown *), D3DSPD_IUNKNOWN)); 

		//--- It's OK here, because DepthStencil is always D3DPOOL_DEFAULT ---
		void** p = *(void***)(*ppSurface);
		UniqueHookCode(this, p[12], Proxy_DepthSurface_GetDesc, (PVOID*)&Original_DepthSurface_GetDesc);
		if( !IsCodeHookedByProxy(p[13], Proxy_Surface_LockRect) )
			UniqueHookCode(this, p[13], Proxy_DepthSurface_LockRect, (PVOID*)&Original_DepthSurface_LockRect);
		if( !IsCodeHookedByProxy(p[14], Proxy_Surface_UnlockRect) )
			UniqueHookCode(this, p[14], Proxy_DepthSurface_UnlockRect, (PVOID*)&Original_DepthSurface_UnlockRect);
	}
#ifndef FINAL_RELEASE
	TCHAR buffer[128];
	if (bCreateStereo)
	{
		_sntprintf_s(buffer, _TRUNCATE, _T("WideDepthStencilSurface%d"), m_nDepthStencilSurfaceCounter);
		m_nDepthStencilSurfacesMonoSize += SizeInBytes / 2;
		m_nDepthStencilSurfacesStereoSize += SizeInBytes / 2;
	}
	else
	{
		_sntprintf_s(buffer, _TRUNCATE, _T("DepthStencilSurfaceMono%d"), m_nDepthStencilSurfaceCounter);
		m_nDepthStencilSurfacesMonoSize += SizeInBytes;
	}
	SetObjectName(*ppSurface, buffer);
	DEBUG_TRACE1(_T("\tSetObjectName(*ppSurface = %p [%s])\n"), *ppSurface, GetObjectName(*ppSurface));
#endif
	m_nDepthStencilSurfaceCounter++;
	return hResult;
}

STDMETHODIMP CWideStereoRenderer::CreateTexture(UINT Width, UINT Height, UINT Levels, DWORD Usage, D3DFORMAT Format,
												D3DPOOL Pool, IDirect3DTexture9** ppTexture, HANDLE * pSharedHandle) 
{
	HRESULT hResult = S_OK;
	//if (GINFO_LOCKABLEDEPTHSTENCIL && (Usage & D3DUSAGE_DEPTHSTENCIL) > 0)
	//{
	//	if (SUCCEEDED(m_pDirect3D->CheckDeviceFormat( D3DADAPTER_DEFAULT,	D3DDEVTYPE_HAL,	
	//		D3DFMT_X8R8G8B8, D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_TEXTURE, D3DFMT_D32F_LOCKABLE)))
	//		Format = D3DFMT_D32F_LOCKABLE;
	//}

	if (gInfo.DisableNVCSTextureInWRTMode && Format == FOURCC_NVCS)  // Some NVidia specific format
	{
		DEBUG_TRACE1(_T("CreateTexture(Width = %d, Height = %d, Levels = %d, Usage = %s, ")
			_T("Format = %s, Pool = %s, *ppTexture = %p, pSharedHandle = %p)"), 
			Width, Height, Levels, GetUsageString(Usage), GetFormatString(Format), 
			GetPoolString(Pool), Indirect(ppTexture), pSharedHandle);
		DEBUG_TRACE1(_T("D3DERR_INVALIDCALL (Disabled in profile)"));
		return D3DERR_INVALIDCALL;
	}
	
	bool CreateInStereo = false;
	if (Usage & D3DUSAGE_RENDERTARGET) 
		CreateInStereo = IsStereoRenderTargetTexture(Format, Usage, Width, Height);
	else if (Usage & D3DUSAGE_DEPTHSTENCIL)
		CreateInStereo = IsStereoDepthStencilTexture(Width, Height);

	UINT NewWidth = Width;
	UINT NewHeight = Height;
	D3DFORMAT WideFormat = Format;
	SIZE Offset;
	if (CreateInStereo)
	{
		HRESULT hr = InitializeOffset(Width, Height, &Offset, NewWidth, NewHeight);
		if (SUCCEEDED(hr))
		{
			if ((Usage & D3DUSAGE_DEPTHSTENCIL))
			{
				bool DepthTextureSupported = true;
				switch(Format)
				{
				case FOURCC_DF16:
				case FOURCC_DF24:
				case FOURCC_INTZ:
				case FOURCC_RAWZ:
					// Supported formats
					break;
				case D3DFMT_D16:
					if (m_DepthTexturesFormat16 != D3DFMT_UNKNOWN)
						WideFormat = m_DepthTexturesFormat16;
					else
						DepthTextureSupported = false;
					break;
				case D3DFMT_D24S8:
				case D3DFMT_D24X8:
				case D3DFMT_D32:
					if (m_DepthTexturesFormat24 != D3DFMT_UNKNOWN)
						WideFormat = m_DepthTexturesFormat24;
					else
						DepthTextureSupported = false;
					break;
				default:
					DepthTextureSupported = false;
				}
				if (DepthTextureSupported) {
					DEBUG_TRACE1(_T("\tOriginal Format = %s\n"), GetFormatString(Format));
				}
			}
		}
		else
			CreateInStereo = false;
	}

	NSCALL_TRACE1(m_Direct3DDevice.CreateTexture(NewWidth, NewHeight, Levels, Usage, WideFormat, 
		Pool, ppTexture, pSharedHandle),
		DEBUG_MESSAGE(_T("CreateTexture(Width = %d, Height = %d, Levels = %d, Usage = %s, ")
		_T("Format = %s, Pool = %s, *ppTexture = %p, pSharedHandle = %p)"), 
		NewWidth, NewHeight, Levels, GetUsageString(Usage), GetFormatString(WideFormat), 
		GetPoolString(Pool), Indirect(ppTexture), pSharedHandle));

	if(!Succeded(hResult))
		return hResult;

	IDirect3DTexture9 *pTexture = *ppTexture;
	{
		DUMP_CMD_ALL(CreateTexture,Width,Height,Levels,Usage,Format,Pool,pTexture,pSharedHandle);
	}
	OnNewTextureCreated(pTexture,Levels);

	UINT SizeInBytes = GetTextureSize(*ppTexture);

#ifndef FINAL_RELEASE
	TCHAR buffer[128];
	if (CreateInStereo)
	{
		if ((Usage & D3DUSAGE_RENDERTARGET) > 0) 
		{
			_sntprintf_s(buffer, _TRUNCATE, _T("WideRenderTargetTexture%d"), m_nRenderTargetTextureCounter);
			m_nRenderTargetTexturesStereoSize += SizeInBytes;
		}
		else 
		{
			_sntprintf_s(buffer, _TRUNCATE, _T("WideDepthStencilTexture%d"), m_nDepthStencilTextureCounter);
			m_nDepthStencilTexturesStereoSize += SizeInBytes;
		}
	}
	else
	{
		if ((Usage & D3DUSAGE_RENDERTARGET) > 0) 
		{
			_sntprintf_s(buffer, _TRUNCATE, _T("RenderTargetTextureMono%d"), m_nRenderTargetTextureCounter);
			m_nRenderTargetTexturesMonoSize += SizeInBytes;
		}
		else if ((Usage & D3DUSAGE_DEPTHSTENCIL) > 0) 
		{
			_sntprintf_s(buffer, _TRUNCATE, _T("DepthStencilTextureMono%d"), m_nDepthStencilTextureCounter);
			m_nDepthStencilTexturesMonoSize += SizeInBytes;
		}
		else if (Pool != D3DPOOL_DEFAULT)
		{
			_sntprintf_s(buffer, _TRUNCATE, _T("Texture%d"), m_nTextureCounter);
			m_nTexturesSize += SizeInBytes;
		}
		else
		{
			_sntprintf_s(buffer, _TRUNCATE, _T("TextureDef%d"), m_nTextureDefaultCounter);
			m_nTexturesDefaultSize += SizeInBytes;
		}
	}
	SetObjectName(*ppTexture, buffer);
	DEBUG_TRACE1(_T("\tSetObjectName(*ppTexture = %p [%s])\n"), *ppTexture, GetObjectName(*ppTexture));
#endif

	if (CreateInStereo)
	{
		IDirect3DTexture9* pWideTexture = *ppTexture;
		SetTextureType(pWideTexture, Mono);

		//--- It's OK here, because RT Texture is always D3DPOOL_DEFAULT ---
		void** p = *(void***)(pWideTexture);
		//UniqueHookCode(this, p[2], Wide::Proxy_Texture_Release, (PVOID*)&Wide::Original_Texture_Release);
		UniqueHookCode(this, p[17], Proxy_Texture_GetLevelDesc, (PVOID*)&Original_Texture_GetLevelDesc);
		//UniqueHookCode(this, p[18], Proxy_Texture_GetSurfaceLevel, (PVOID*)&Original_Texture_GetSurfaceLevel);
		UniqueHookCode(this, p[19], Proxy_Texture_LockRect, (PVOID*)&Original_Texture_LockRect);
		UniqueHookCode(this, p[20], Proxy_Texture_UnlockRect, (PVOID*)&Original_Texture_UnlockRect);

		CComPtr<IDirect3DSurface9> pFirstSurf;
		pWideTexture->GetSurfaceLevel(0, &pFirstSurf);
		p = *(void***)(pFirstSurf.p);
		//UniqueHookCode(this, p[2], Wide::Proxy_Surface_Release, (PVOID*)&Wide::Original_Surface_Release);
		//UniqueHookCode(this, p[11], Proxy_TexSurface_GetContainer, (PVOID*)&Original_TexSurface_GetContainer);
		UniqueHookCode(this, p[12], Proxy_TexSurface_GetDesc, (PVOID*)&Original_TexSurface_GetDesc);
		UniqueHookCode(this, p[13], Proxy_TexSurface_LockRect, (PVOID*)&Original_TexSurface_PDef_LockRect);
		UniqueHookCode(this, p[14], Proxy_TexSurface_UnlockRect, (PVOID*)&Original_TexSurface_PDef_UnlockRect);

		D3DFORMAT LeftFormat = Format;
		DWORD LeftUsage = Usage;

		if (m_bUseR32FDepthTextures)
		{
			switch(Format)
			{
			case D3DFMT_D16:
			case D3DFMT_D24S8:
			case D3DFMT_D24X8:
			case D3DFMT_D32:
			case FOURCC_DF16:
			case FOURCC_DF24:
			case FOURCC_INTZ:
				LeftUsage = D3DUSAGE_RENDERTARGET;
				LeftFormat = D3DFMT_R32F;
				break;
			case FOURCC_RAWZ:
				LeftUsage = D3DUSAGE_RENDERTARGET;
				LeftFormat = D3DFMT_A8R8G8B8;
				break;
			}
		}

		CComPtr<IDirect3DTexture9> pLeftTexture;
		NSCALL_TRACE1(m_Direct3DDevice.CreateTexture(Width, Height, Levels, LeftUsage, LeftFormat, 
			Pool, &pLeftTexture, pSharedHandle),
			DEBUG_MESSAGE(_T("\tCreateTexture(Width = %d, Height = %d, Levels = %d, Usage = %s, ")
			_T("Format = %s, Pool = %s, *ppTexture = %p, pSharedHandle = %p)"), 
			Width, Height, Levels, GetUsageString(LeftUsage), GetFormatString(LeftFormat), 
			GetPoolString(Pool), pLeftTexture.p, pSharedHandle));
		NSCALL(pWideTexture->SetPrivateData(LeftTexture_GUID, pLeftTexture, sizeof(IUnknown *), D3DSPD_IUNKNOWN));

		if(Succeded(hResult))
		{
			SIZE Size;
			Size.cx = Width;
			Size.cy = Height;
			DWORD levelCount = pWideTexture->GetLevelCount();
			DWORD leftLevelCount = pLeftTexture->GetLevelCount();
			for (DWORD i = 0; i < levelCount; i++)
			{
				CComPtr<IDirect3DSurface9> pWideSurf;
				pWideTexture->GetSurfaceLevel(i, &pWideSurf);    

				CComPtr<IWideSurfaceData> pWideSurfaceData;
				CWideSurfaceData::CreateInstance(&pWideSurfaceData);
				pWideSurfaceData->SetOffset(&Offset);
				pWideSurfaceData->SetSize(&Size);
				pWideSurfaceData->SetFormat(Format);
				pWideSurfaceData->SetUsage(Usage);
				NSCALL(pWideSurf->SetPrivateData(__uuidof(IWideSurfaceData), (void *)pWideSurfaceData, sizeof(IUnknown *), D3DSPD_IUNKNOWN)); 

				if (i < leftLevelCount)
				{
					CComPtr<IDirect3DSurface9> pLeftSurf;
					pLeftTexture->GetSurfaceLevel(i, &pLeftSurf);   
					NSCALL(pWideSurf->SetPrivateData(LeftSurface_GUID, &pLeftSurf.p, sizeof(IUnknown *), 0));
				}

				if ((i + 1) == levelCount)
					break;
				// for next level
				Size.cx = std::max(Size.cx / 2, 1l);
				Size.cy = std::max(Size.cy / 2, 1l);
				Offset.cy = Size.cy;
			}

#ifndef FINAL_RELEASE
			TCHAR buffer[128];
			if ((Usage & D3DUSAGE_RENDERTARGET) > 0) 
			{
				_sntprintf_s(buffer, _TRUNCATE, _T("LeftRenderTargetTexture%d"), m_nRenderTargetTextureCounter);
				m_nRenderTargetTexturesMonoSize += GetTextureSize(pLeftTexture);
			}
			else 
			{
				_sntprintf_s(buffer, _TRUNCATE, _T("LeftDepthStencilTexture%d"), m_nDepthStencilTextureCounter);
				m_nDepthStencilTexturesMonoSize += GetTextureSize(pLeftTexture);
			}

			SetObjectName(pLeftTexture, buffer);
			DEBUG_TRACE1(_T("\t\tSetObjectName(*ppTexture = %p [%s])\n"), pLeftTexture.p, GetObjectName(pLeftTexture));
#endif

			CComPtr<IDirect3DTexture9> pRightTexture;
			NSCALL_TRACE1(m_Direct3DDevice.CreateTexture(Width, Height, Levels, LeftUsage, LeftFormat, 
				Pool, &pRightTexture, pSharedHandle),
				DEBUG_MESSAGE(_T("\tCreateTexture(Width = %d, Height = %d, Levels = %d, Usage = %s, ")
				_T("Format = %s, Pool = %s, *ppTexture = %p, pSharedHandle = %p)"), 
				Width, Height, Levels, GetUsageString(LeftUsage), GetFormatString(LeftFormat), 
				GetPoolString(Pool), pRightTexture.p, pSharedHandle));
#ifndef FINAL_RELEASE
			if(SUCCEEDED(hResult))
			{
				if ((Usage & D3DUSAGE_RENDERTARGET) > 0) 
				{
					_sntprintf_s(buffer, _TRUNCATE, _T("RightRenderTargetTexture%d"), m_nRenderTargetTextureCounter);
					m_nRenderTargetTexturesStereoSize += SizeInBytes;
				}
				else 
				{
					_sntprintf_s(buffer, _TRUNCATE, _T("RightDepthStencilTexture%d"), m_nDepthStencilTextureCounter);
					m_nDepthStencilTexturesStereoSize += SizeInBytes;
				}
				SetObjectName(pRightTexture, buffer);
				DEBUG_TRACE1(_T("\t\tSetObjectName(*ppTexture = %p [%s])\n"), pRightTexture.p, GetObjectName(pRightTexture));
			}
#endif
			SetStereoObject(pLeftTexture, pRightTexture);

			for (DWORD i = 0; i < pLeftTexture->GetLevelCount(); i++)
			{
				CComPtr<IDirect3DSurface9> pLeftSurface;
				NSCALL(pLeftTexture->GetSurfaceLevel( i, &pLeftSurface ));
				CComPtr<IDirect3DSurface9> pRightSurface;
				if (pRightTexture)
				{
					NSCALL(pRightTexture->GetSurfaceLevel( i, &pRightSurface ));
				}
				SetStereoObject(pLeftSurface, pRightSurface);
			}

			if ((Usage & D3DUSAGE_DEPTHSTENCIL) &&
				(LeftFormat != D3DFMT_D16) && (LeftFormat != D3DFMT_D24S8) &&
				(LeftFormat != D3DFMT_D32) && (LeftFormat != D3DFMT_D24X8) &&
				(GINFO_REPLACEDSSURFACES || gInfo.ExtractDepthBuffer) && 
				!GetBaseSC()->m_pLeftDepthStencilTexture)
			{
				GetBaseSC()->m_pMainDepthStencilSurface = pFirstSurf;
				GetBaseSC()->m_pLeftDepthStencilTexture = pLeftTexture;
				GetBaseSC()->m_pRightDepthStencilTexture = pRightTexture;
			}
		}

	}
	else
	{
		SetStereoObject(*ppTexture, 0);
		for (DWORD i = 0; i < (*ppTexture)->GetLevelCount(); i++)
		{
			CComPtr<IDirect3DSurface9> pLeftSurface;
			NSCALL((*ppTexture)->GetSurfaceLevel( i, &pLeftSurface ));
			SetStereoObject(pLeftSurface, 0);
		}

		if (m_Direct3DDevice.getExMode() == EXMODE_EMULATE && Pool == D3DPOOL_MANAGED)
		{
			void** p = *(void***)(*ppTexture);
			UniqueHookCode(this, p[19], Proxy_Texture_LockRect, (PVOID*)&Original_Texture_LockRect);
			UniqueHookCode(this, p[20], Proxy_Texture_UnlockRect, (PVOID*)&Original_Texture_UnlockRect);
			CComPtr<IDirect3DSurface9> pFirstSurf;
			(*ppTexture)->GetSurfaceLevel(0, &pFirstSurf);
			if(pFirstSurf)
			{
				// Pool changed to D3DPOOL_DEFAULT
				p = *(void***)(pFirstSurf.p);
				UniqueHookCode(this, p[13], Proxy_TexSurface_LockRect, (PVOID*)&Original_TexSurface_PDef_LockRect);
				UniqueHookCode(this, p[14], Proxy_TexSurface_UnlockRect, (PVOID*)&Original_TexSurface_PDef_UnlockRect);
			}
		}

		CheckYouTubeTexture(Width, Height, Usage, Format, Pool, *ppTexture);
	}
	if ((Usage & D3DUSAGE_RENDERTARGET) > 0) 
		m_nRenderTargetTextureCounter++;
	else if ((Usage & D3DUSAGE_DEPTHSTENCIL) > 0) 
		m_nDepthStencilTextureCounter++;
	else if (Pool != D3DPOOL_DEFAULT)
		m_nTextureCounter++;
	else
		m_nTextureDefaultCounter++;
	if ((Usage & D3DUSAGE_RENDERTARGET) > 0 || (Usage & D3DUSAGE_DEPTHSTENCIL) > 0) 
	{
		if (Width == Height) 
			m_nRenderTargetSquareTextureCounter++;
		else if (IsLessThanBB(Width, Height))
			m_nRenderTargetLessThanBBTextureCounter++;
	}
	return hResult;
}

STDMETHODIMP CWideStereoRenderer::CreateCubeTexture(UINT EdgeLength, UINT Levels, DWORD Usage, D3DFORMAT Format, 
													D3DPOOL Pool, IDirect3DCubeTexture9** ppCubeTexture, HANDLE* 
													pSharedHandle) 
{
	HRESULT hResult;
	NSCALL_TRACE1(m_Direct3DDevice.CreateCubeTexture(EdgeLength, Levels, Usage, Format, 
		Pool, ppCubeTexture, pSharedHandle),
		DEBUG_MESSAGE(_T("CreateCubeTexture(EdgeLength = %u, Levels = %u, Usage = %s, ")
		_T("Format = %s, Pool = %s, *ppCubeTexture = %p, pSharedHandle = %p)"), 
		EdgeLength, Levels, GetUsageString(Usage), GetFormatString(Format), 
		GetPoolString(Pool), Indirect(ppCubeTexture), pSharedHandle));

	if(!Succeded(hResult))
		return hResult;

	IDirect3DCubeTexture9 *pCubeTexture = *ppCubeTexture;
	{
		DUMP_CMD_ALL(CreateCubeTexture,EdgeLength,Levels,Usage,Format,Pool,pCubeTexture,pSharedHandle);
	}
	OnNewTextureCreated(pCubeTexture,Levels);

	HOOK_GET_DEVICE(ppCubeTexture, Proxy_CubeTexture_GetDevice, Original_CubeTexture_GetDevice);

	SetStereoObject(*ppCubeTexture, 0);
	for (DWORD j = 0; j < 6; j++)
	{
		for (DWORD i = 0; i < (*ppCubeTexture)->GetLevelCount(); i++)
		{
			CComPtr<IDirect3DSurface9> pLeftSurface;
			NSCALL((*ppCubeTexture)->GetCubeMapSurface((D3DCUBEMAP_FACES)j, i, &pLeftSurface));
			SetStereoObject(pLeftSurface, 0);
		}
	}
#ifndef FINAL_RELEASE
	TCHAR buffer[128];
	if ((Usage & D3DUSAGE_RENDERTARGET) > 0) 
	{
		_sntprintf_s(buffer, _TRUNCATE, _T("RenderTargetCubeTextureMono%d"), m_nRenderTargetCubeTextureCounter);
		m_nRenderTargetCubeTexturesMonoSize += GetCubeTextureSize(*ppCubeTexture);
	}
	else if ((Usage & D3DUSAGE_DEPTHSTENCIL) > 0) 
	{
		_sntprintf_s(buffer, _TRUNCATE, _T("DepthStencilCubeTextureMono%d"), m_nDepthStencilCubeTextureCounter);
		m_nDepthStencilCubeTexturesMonoSize += GetCubeTextureSize(*ppCubeTexture);
	}
	else if (Pool != D3DPOOL_DEFAULT)
	{
		_sntprintf_s(buffer, _TRUNCATE, _T("CubeTexture%d"), m_nCubeTextureCounter);
		m_nCubeTexturesSize += GetCubeTextureSize(*ppCubeTexture);
	}
	else
	{
		_sntprintf_s(buffer, _TRUNCATE, _T("CubeTextureDef%d"), m_nCubeTextureDefaultCounter);
		m_nCubeTexturesDefaultSize += GetCubeTextureSize(*ppCubeTexture);
	}
	SetObjectName(*ppCubeTexture, buffer);
	DEBUG_TRACE1(_T("\tSetObjectName(*ppTexture = %p [%s])\n"), *ppCubeTexture, GetObjectName(*ppCubeTexture));
#endif
	if ((Usage & D3DUSAGE_RENDERTARGET) > 0) 
		m_nRenderTargetCubeTextureCounter++;
	else if ((Usage & D3DUSAGE_DEPTHSTENCIL) > 0) 
		m_nDepthStencilCubeTextureCounter++;
	else if (Pool != D3DPOOL_DEFAULT)
		m_nCubeTextureCounter++;
	else
		m_nCubeTextureDefaultCounter++;
	return hResult;
}

STDMETHODIMP CWideStereoRenderer::Clear(DWORD Count, CONST D3DRECT * pRects, DWORD Flags, D3DCOLOR Color, float Z, DWORD Stencil ) 
{
	DUMP_CMD(Clear,Count,pRects,Flags,Color,Z,Stencil);

	HRESULT hResult = S_OK;
	if (GINFO_LOCKABLEDEPTHSTENCIL)
		Flags &= ~D3DCLEAR_STENCIL;
	if ((Flags & D3DCLEAR_ZBUFFER) && gInfo.ExtractDepthBuffer &&
		(GetBaseDevice()->m_DepthStencil.m_pMainSurf == GetBaseSC()->m_pMainDepthStencilSurface))
		GetWideSC()->CheckDepthBuffer();
	DWORD updateRTs[4];
	bool updateDS;
	bool bRenderToRight = false;
	if (m_bEngineStarted)
	{
		updateRTs[0] = updateRTs[1] = updateRTs[2] = updateRTs[3] = Flags & D3DCLEAR_TARGET;
		updateDS = (Flags & (D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL)) != 0;
		UpdateRTsRevision(updateRTs, updateDS);
		bRenderToRight = RenderToRight();
		if (bRenderToRight && 
			(updateRTs[0] == 0 || m_pOffset == NULL) &&
			(updateDS == 0 || m_pDepthOffset == NULL))
				bRenderToRight = false;
	}
	if (bRenderToRight)
	{
		// Select offset
		if (Count <= 0)
			SelectOffset((Flags & D3DCLEAR_TARGET) != 0, (LONG)m_ViewPort.Width, (LONG)m_ViewPort.Height);
		else
			SelectOffset((Flags & D3DCLEAR_TARGET) != 0, pRects[0].x2, pRects[0].y2);
		bool bClear2RT = RenderInStereo();

		CONST SIZE* pOffset = m_pCurOffset;
		DWORD RightFlags = Flags;
		if (!bClear2RT)
		{
			if (updateRTs[0] != 0 && m_pOffset != NULL)
			{
				RightFlags = Flags & D3DCLEAR_TARGET;
				m_pCurOffset = m_pOffset;
			}
			else if	(updateDS != 0 && m_pDepthOffset != NULL)
			{
				RightFlags = Flags & (D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL);
				m_pCurOffset = m_pDepthOffset;
			}
		}
		if (!DEBUG_SKIP_SET_STEREO_RENDERTARGET)
			SetStereoRenderTarget<VIEWINDEX_RIGHT>(false);
		D3DRECT *pRightRects = (Count > 0 ? DNew D3DRECT[Count] : NULL);
		for (ULONG i = 0; i < Count; i++) {
			GetRightViewRect(&pRightRects[i], &pRects[i], m_pCurOffset);
		}
		NSCALL_TRACE2(m_Direct3DDevice.Clear(Count, pRightRects, RightFlags, Color, Z, Stencil),
			DEBUG_MESSAGE(_T("Clear(%d, %s, %s, %08X, %12.7f, %ld)"), 
			Count, GetClearRectString(Count, pRightRects),
			GetClearFlagsString(RightFlags), Color, Z, Stencil));
		if (pRightRects) delete [] pRightRects;
		m_pCurOffset = pOffset;

		if (!DEBUG_SKIP_SET_STEREO_RENDERTARGET)
			SetStereoRenderTarget<VIEWINDEX_LEFT>(false);
		NSCALL_TRACE2(m_Direct3DDevice.Clear(Count, pRects, Flags, Color, Z, Stencil),
			DEBUG_MESSAGE(_T("\tClear(%d, %s, %s, %08X, %12.7f, %ld)"), 
			Count, GetClearRectString(Count, pRects),
			GetClearFlagsString(Flags), Color, Z, Stencil));

		if (Count == 0)
		{
			if (Flags & D3DCLEAR_TARGET)
			{
				m_RenderTarget[0].SetType(Cleared);
				m_RenderTarget[1].SetType(Cleared);
				m_RenderTarget[2].SetType(Cleared);
				m_RenderTarget[3].SetType(Cleared);
			}
			if (Flags & D3DCLEAR_ZBUFFER && Flags & D3DCLEAR_STENCIL)
				m_DepthStencil.SetSurfaceType(Cleared);
		}
	}
	else
	{
		NSCALL_TRACE2(m_Direct3DDevice.Clear(Count, pRects, Flags, Color, Z, Stencil),
			DEBUG_MESSAGE(_T("Clear(%d, %s, %s, %08X, %12.7f, %ld)"), 
			Count, GetClearRectString(Count, pRects),
			GetClearFlagsString(Flags), Color, Z, Stencil));

		if (Flags & D3DCLEAR_TARGET)
		{
			m_RenderTarget[0].SetType(Mono);
			m_RenderTarget[1].SetType(Mono);
			m_RenderTarget[2].SetType(Mono);
			m_RenderTarget[3].SetType(Mono);
		}
		if (Flags & D3DCLEAR_ZBUFFER && Flags & D3DCLEAR_STENCIL)
			m_DepthStencil.SetSurfaceType(Mono);
	}

	if (DO_RTDUMP)
	{
		if (Flags & D3DCLEAR_TARGET) {
			DumpRTSurfaces(bRenderToRight, _T("Clear"), _T(""), TRUE);
		}
		if (Flags & D3DCLEAR_ZBUFFER) {
			DumpDSSurfaces(bRenderToRight, _T("Clear"), _T(""));
		}
		DEBUG_TRACE3(_T("---------- Event %i complete ----------\n"), m_nEventCounter);
		m_nEventCounter++;
	}
	return hResult;
}

void CWideStereoRenderer::StretchRectToWideDest( 
	IDirect3DSurface9 *pSourceSurface, CONST RECT * pSourceRect, CONST SIZE* pSourceOffset, 
	IDirect3DSurface9 *pDestSurface, CONST RECT * pDestRect, CONST SIZE* pDestOffset, D3DTEXTUREFILTERTYPE Filter )
{
	INSIDE;
	HRESULT hResult = S_OK;
	bool bWideSource = pSourceOffset != NULL;
	_ASSERT(pDestOffset != NULL);
	bool bRenderToRight = RenderToRight();
	DWORD DestWideRev = GetSurfaceRevision(pDestSurface);
	CComPtr<IDirect3DTexture9> pDestParentTex;
	GetParentTexture(pDestSurface, &pDestParentTex);
	CComPtr<IDirect3DTexture9> pDestLeftTex;
	GetLeftTexture(pDestParentTex, &pDestLeftTex);
	IDirect3DTexture9* pDestWideTex = GetWideTexture(pDestParentTex, pDestLeftTex);

	RECT DstRect;
	RECT ModDstRightRect;
	CONST RECT * pDestLeftRect;
	CONST RECT * pDestRightRect;

	CComPtr<IDirect3DSurface9> pDestLeftSurf;
	CComPtr<IDirect3DSurface9> pDestRightSurf;
	D3DSURFACE_DESC destDesc;
	pDestSurface->GetDesc(&destDesc);
	bool bCopyDirectlyToDestTexture = false;
	DWORD DestRev;
	if (pDestWideTex != NULL && pDestLeftTex != NULL)
	{
		pDestLeftTex->GetSurfaceLevel(0, &pDestLeftSurf);
		DWORD DestLeftRev = GetSurfaceRevision(pDestLeftSurf);
		DestRev = std::max(DestWideRev, DestLeftRev) + 1;
		if (DestLeftRev >= DestWideRev)
			bCopyDirectlyToDestTexture = true;
		else if (pDestRect == NULL || (pDestRect->left == 0 && pDestRect->top == 0 && 
			pDestRect->right == destDesc.Width && pDestRect->bottom == destDesc.Height / HEIGHTMUL))
			bCopyDirectlyToDestTexture = true;
	}
	else
		DestRev = DestWideRev + 1;
	if (bCopyDirectlyToDestTexture)
	{
		pDestLeftRect = pDestRect;
		if (bRenderToRight)
		{
			GetStereoObject(pDestLeftSurf, &pDestRightSurf);
			pDestRightRect = pDestRect;
		}
		DEBUG_TRACE3(_T("\tStretchRect to texture\n"));
	}
	else  // Copy to wide surface
	{
		pDestLeftSurf = pDestSurface;
		if (pDestRect == NULL)
			GetNarrowRect(pDestSurface, &DstRect);
		else
			DstRect = *pDestRect;
		pDestLeftRect =	&DstRect;
		if (bRenderToRight)
		{
			pDestRightSurf = pDestSurface;
			pDestRightRect = &ModDstRightRect;
			GetRightViewRect(&ModDstRightRect, &DstRect, pDestOffset);
		}
		DEBUG_TRACE3(_T("\tStretchRect to wide surface\n"));
	}
	if (pDestWideTex)
	{
		SetSurfaceRevision(pDestLeftSurf, DestRev);
		if (pDestRightSurf && pDestLeftSurf != pDestRightSurf)
			SetSurfaceRevision(pDestRightSurf, DestRev);
	}

	if (bWideSource)
	{
		DEBUG_TRACE3(_T("\tWideSource\n"));
		DWORD SrcWideRev = GetSurfaceRevision(pSourceSurface);
		CComPtr<IDirect3DTexture9> pSrcParentTex;
		GetParentTexture(pSourceSurface, &pSrcParentTex);
		CComPtr<IDirect3DTexture9> pSrcLeftTex;
		GetLeftTexture(pSrcParentTex, &pSrcLeftTex);
		IDirect3DTexture9* pSrcWideTex = GetWideTexture(pSrcParentTex, pSrcLeftTex);

		RECT SrcRect;
		RECT ModSrcRightRect;
		CONST RECT * pSrcLeftRect;
		CONST RECT * pSrcRightRect;

		CComPtr<IDirect3DSurface9> pSrcLeftSurf;
		CComPtr<IDirect3DSurface9> pSrcRightSurf;
		DWORD SrcLeftRev = 0;
		TextureType textureType = Mono;
		if (pSrcWideTex != NULL && pSrcLeftTex != NULL)
		{
			pSrcLeftTex->GetSurfaceLevel(0, &pSrcLeftSurf);
			SrcLeftRev = GetSurfaceRevision(pSrcLeftSurf);
			textureType = GetTextureType(pSrcWideTex);
			if (textureType == Mono && bRenderToRight && gInfo.BltDstEye == 1)
				textureType = Stereo;
		}
		else if (bRenderToRight)
			textureType = Stereo;
		if (pDestWideTex)
		{
			DEBUG_TRACE3(_T("\tSetTextureType %p [%s]: TextureType = %s\n"), 
				pDestWideTex, GetObjectName(pDestWideTex), GetTextureTypeString(textureType));
			SetTextureType(pDestWideTex, textureType);
		}
		bool bFullSurfCopy = false; // Copy full wide source surface directly to wide destination surface
		if (SrcWideRev >= SrcLeftRev || pSrcLeftTex == NULL) // Copy from wide surface
		{
			pSrcLeftSurf = pSourceSurface;
			bool res = false;
			// Make correct SrcRect and DstRect
			if (pDestLeftTex == NULL)
			{
				D3DSURFACE_DESC srcDesc;
				pSourceSurface->GetDesc(&srcDesc);
				bool bCorrectRects;
				if (pSourceRect == NULL)
				{
					bCorrectRects = true;
					GetSurfRect(&srcDesc, &SrcRect);
					GetNarrowRect(&SrcRect);
				}
				else
				{
					SrcRect = *pSourceRect;
					bCorrectRects = (SrcRect.left == 0 && SrcRect.top == 0 && 
						SrcRect.right == srcDesc.Width && SrcRect.bottom == (srcDesc.Height / HEIGHTMUL));
				}
				if (bCorrectRects)
				{
					if (pDestRect == NULL)
					{
						GetSurfRect(&destDesc, &DstRect);
						GetNarrowRect(&DstRect);
					}
					else
					{
						DstRect = *pDestRect;
						bCorrectRects = (DstRect.left == 0 && DstRect.top == 0 && 
							DstRect.right == destDesc.Width && DstRect.bottom == (destDesc.Height / HEIGHTMUL));
					}
				}
				bCorrectRects = bCorrectRects && EqualRect(&SrcRect, &DstRect);
				if (bCorrectRects)
				{
					if (bRenderToRight)
						bFullSurfCopy = true;
					else
					{
						if (srcDesc.Usage & D3DUSAGE_DEPTHSTENCIL) // DepthStencil surface not support copy using rect's
							bFullSurfCopy = true;
					}
				}
			}

			if (!bFullSurfCopy)
			{
				if (pSourceRect == NULL)
					GetNarrowRect(pSourceSurface, &SrcRect);
				else
					SrcRect = *pSourceRect;
				pSrcLeftRect = &SrcRect;
				if (bRenderToRight)
				{
					pSrcRightSurf = pSourceSurface;
					pSrcRightRect = &ModSrcRightRect;
					GetRightViewRect(&ModSrcRightRect, &SrcRect, pSourceOffset);
				}
			} else
			{
				DEBUG_TRACE3(_T("\tFull surface copy\n"));
				pDestRightSurf.Release(); // disable second StretchRect() call
				SrcRect.bottom *= HEIGHTMUL; // Make full surface rect
				pSrcLeftRect = &SrcRect;
				DstRect.bottom *= HEIGHTMUL;
				pDestLeftRect = &DstRect;
			}
			DEBUG_TRACE3(_T("\tStretchRect from wide surface\n"));
		}
		else  // Copy from texture
		{
			pSrcLeftRect = pSourceRect;
			if (bRenderToRight)
			{
				GetStereoObject(pSrcLeftSurf, &pSrcRightSurf);
				pSrcRightRect = pSourceRect;
			}
			DEBUG_TRACE3(_T("\tStretchRect from texture\n"));
		}

		IDirect3DSurface9* pDestSurf = pDestLeftSurf;
		CONST RECT* pDestRect = pDestLeftRect;
		if (bRenderToRight && gInfo.BltDstEye == 1)
		{
			pDestSurf = pDestRightSurf;
			pDestRect = pDestRightRect;
		}

		NSCALL_TRACE2(m_Direct3DDevice.StretchRect( pSrcLeftSurf, pSrcLeftRect, pDestSurf, pDestRect, Filter ),
			DEBUG_MESSAGE(_T("StretchRect(pSourceSurface = %p [%s], pSourceRect = %s, ")
			_T("pDestSurface = %p [%s], pDestRect = %s, Filter = %s)"), 
			pSrcLeftSurf.p, GetObjectName(pSrcLeftSurf), GetRectString(pSrcLeftRect), 
			pDestSurf, GetObjectName(pDestSurf), GetRectString(pDestRect), 
			GetTextureFilterTypeString(Filter)));

		if (DO_RTDUMP)
		{
			TCHAR szFileName[MAX_PATH];
			_stprintf_s(szFileName, L"%s\\%.4d.StretchRect.Source.%s.%s", 
				DRAW_HISTORY, m_nEventCounter, GetObjectName(pSrcLeftSurf), (bFullSurfCopy ? _T("Wide") : _T("Left")));
			SaveSurfaceToFile(szFileName, pSrcLeftSurf);
			DEBUG_TRACE2(_T("Dumping %s\n"), szFileName);

			_stprintf_s(szFileName, L"%s\\%.4d.StretchRect.Dest.%s.%s", 
				DRAW_HISTORY, m_nEventCounter, GetObjectName(pDestSurf), (bFullSurfCopy ? _T("Wide") : _T("Left")));
			SaveSurfaceToFile(szFileName, pDestSurf);
			DEBUG_TRACE2(_T("Dumping %s\n"), szFileName);
		}

		if (pDestRightSurf && textureType != Mono && gInfo.BltDstEye == 0)
		{
			NSCALL_TRACE2(m_Direct3DDevice.StretchRect( pSrcRightSurf, pSrcRightRect, pDestRightSurf, pDestRightRect, Filter ),
				DEBUG_MESSAGE(_T("\tStretchRect(pSourceSurface = %p [%s], pSourceRect = %s, ")
				_T("pDestSurface = %p [%s], pDestRect = %s, Filter = %s)"), 
				pSrcRightSurf.p, GetObjectName(pSrcRightSurf), GetRectString(pSrcRightRect), 
				pDestRightSurf.p, GetObjectName(pDestRightSurf), GetRectString(pDestRightRect), 
				GetTextureFilterTypeString(Filter)));

			if (DO_RTDUMP)
			{
				TCHAR szFileName[MAX_PATH];
				_stprintf_s(szFileName, L"%s\\%.4d.StretchRect.Source.%s.Right", 
					DRAW_HISTORY, m_nEventCounter, GetObjectName(pSrcRightSurf));
				SaveSurfaceToFile(szFileName, pSrcRightSurf);
				DEBUG_TRACE2(_T("Dumping %s\n"), szFileName);

				_stprintf_s(szFileName, L"%s\\%.4d.StretchRect.Dest.%s.Right", 
					DRAW_HISTORY, m_nEventCounter, GetObjectName(pDestRightSurf));
				SaveSurfaceToFile(szFileName, pDestRightSurf);
				DEBUG_TRACE2(_T("Dumping %s\n"), szFileName);
			}
		}
	}
	else
	{
		IDirect3DSurface9* pDestSurf = pDestLeftSurf;
		CONST RECT* pDestRect = pDestLeftRect;
		TextureType textureType = Mono;
		bool bLeft = true;
		if (bRenderToRight && gInfo.BltDstEye == 1)
		{
			pDestSurf = pDestRightSurf;
			pDestRect = pDestRightRect;
			textureType = Stereo;
			bLeft = false;
		}
		NSCALL_TRACE2(m_Direct3DDevice.StretchRect( pSourceSurface, pSourceRect, pDestSurf, pDestRect, Filter ),
			DEBUG_MESSAGE(_T("StretchRect(pSourceSurface = %p [%s], pSourceRect = %s, ")
			_T("pDestSurface = %p [%s], pDestRect = %s, Filter = %s)"), 
			pSourceSurface, GetObjectName(pSourceSurface), GetRectString(pSourceRect), 
			pDestSurf, GetObjectName(pDestSurf), GetRectString(pDestRect), 
			GetTextureFilterTypeString(Filter)));

		if (DO_RTDUMP)
		{
			TCHAR szFileName[MAX_PATH];
			D3DSURFACE_DESC srcDesc;
			pSourceSurface->GetDesc(&srcDesc);
			if (srcDesc.Usage & D3DUSAGE_RENDERTARGET) // otherwise cause looping
			{
				_stprintf_s(szFileName, _T("%s\\%.4d.StretchRect.Source.%s.Wide"), 
					DRAW_HISTORY, m_nEventCounter, GetObjectName(pSourceSurface));
				SaveSurfaceToFile(szFileName, pSourceSurface);
				DEBUG_TRACE2(_T("Dumping %s\n"), szFileName);
			}

			_stprintf_s(szFileName, _T("%s\\%.4d.StretchRect.Dest.%s.%s"), 
				DRAW_HISTORY, m_nEventCounter, GetObjectName(pDestSurf), (bLeft ? _T("Left") : _T("Right")));
			SaveSurfaceToFile(szFileName, pDestSurf);
			DEBUG_TRACE2(_T("Dumping %s\n"), szFileName);
		}

		if (pDestWideTex)
		{
			DEBUG_TRACE3(_T("\tSetTextureType %p [%s]: TextureType = %s\n"), 
				pDestWideTex, GetObjectName(pDestWideTex), GetTextureTypeString(textureType));
			SetTextureType(pDestWideTex, textureType); // Not copy for textures because they have texture type
		}
	}
}

void CWideStereoRenderer::StretchRectToMonoDest( IDirect3DSurface9 * pSourceSurface, CONST RECT * pSourceRect, CONST SIZE* pSourceOffset, 
												IDirect3DSurface9 * pDestSurface, CONST RECT * pDestRect, D3DTEXTUREFILTERTYPE Filter )
{
	HRESULT hResult = S_OK;
	BOOL bWideSource = pSourceOffset != NULL;
	bool bCopyFromRight = RenderToRight() && gInfo.BltSrcEye == 1;
	DWORD SrcWideRev = GetSurfaceRevision(pSourceSurface);
	CComPtr<IDirect3DTexture9> pSrcParentTex;
	GetParentTexture(pSourceSurface, &pSrcParentTex);
	CComPtr<IDirect3DTexture9> pSrcLeftTex;
	GetLeftTexture(pSrcParentTex, &pSrcLeftTex);
	IDirect3DTexture9* pSrcWideTex = GetWideTexture(pSrcParentTex, pSrcLeftTex);
	CComPtr<IDirect3DSurface9> pSrcLeftSurf;
	DWORD SrcLeftRev = 0;
	if (pSrcWideTex != NULL && pSrcLeftTex != NULL)
	{
		pSrcLeftTex->GetSurfaceLevel(0, &pSrcLeftSurf);
		SrcLeftRev = GetSurfaceRevision(pSrcLeftSurf);
	}
	CONST RECT * pSrcLeftRect;
	RECT SrcRect, ModSrcRightRect;
	if (SrcWideRev >= SrcLeftRev || pSrcLeftSurf == NULL)
	{
		if (pSourceRect == NULL)
			GetNarrowRect(pSourceSurface, &SrcRect);
		else
			SrcRect = *pSourceRect;
		pSrcLeftRect = &SrcRect;
		if (!bCopyFromRight)
			pSrcLeftSurf = pSourceSurface;
		else
			GetStereoObject(pSourceSurface, &pSrcLeftSurf);
		DEBUG_TRACE3(_T("\tStretchRect from texture\n"));
	}
	else
	{
		if (!bCopyFromRight)
			pSrcLeftRect = pSourceRect;
		else
		{
			pSrcLeftRect = &ModSrcRightRect;
			if (pSourceRect == NULL)
				GetNarrowRect(pSourceSurface, &SrcRect);
			else
				SrcRect = *pSourceRect;
			GetRightViewRect(&ModSrcRightRect, &SrcRect, pSourceOffset);
		}
		DEBUG_TRACE3(_T("\tStretchRect from wide surface\n"));
	}
	NSCALL_TRACE2(m_Direct3DDevice.StretchRect( pSrcLeftSurf, pSrcLeftRect, pDestSurface, pDestRect, Filter ),
		DEBUG_MESSAGE(_T("StretchRect(pSourceSurface = %p [%s], pSourceRect = %s, ")
		_T("pDestSurface = %p [%s], pDestRect = %s, Filter = %s)"), 
		pSrcLeftSurf.p, GetObjectName(pSrcLeftSurf), GetRectString(pSrcLeftRect), 
		pDestSurface, GetObjectName(pDestSurface), GetRectString(pDestRect), 
		GetTextureFilterTypeString(Filter)));

	if (DO_RTDUMP)
	{
		TCHAR szFileName[MAX_PATH];
		_stprintf_s(szFileName, L"%s\\%.4d.StretchRect.Source.%s.Left", 
			DRAW_HISTORY, m_nEventCounter, GetObjectName(pSrcLeftSurf));
		SaveSurfaceToFile(szFileName, pSrcLeftSurf);
		DEBUG_TRACE2(_T("Dumping %s\n"), szFileName);

		_stprintf_s(szFileName, L"%s\\%.4d.StretchRect.Dest.%s.Wide", 
			DRAW_HISTORY, m_nEventCounter, GetObjectName(pDestSurface));
		SaveSurfaceToFile(szFileName, pDestSurface);
		DEBUG_TRACE2(_T("Dumping %s\n"), szFileName);
	}
}

STDMETHODIMP CWideStereoRenderer::StretchRect( 
	IDirect3DSurface9 *pSourceSurface, CONST RECT * pSourceRect, 
	IDirect3DSurface9 *pDestSurface, CONST RECT * pDestRect, D3DTEXTUREFILTERTYPE Filter ) 
{
	HRESULT hResult = S_OK;
	CONST SIZE* pSourceOffset = GetOffset(pSourceSurface);
	CONST SIZE* pDestOffset = GetOffset(pDestSurface);
	BOOL bWideSource = pSourceOffset != NULL; 
	BOOL bWideDest = pDestOffset != NULL;
	if (pSourceSurface != pDestSurface && (bWideSource || bWideDest))
	{
		if (bWideDest) {
			DEBUG_TRACE3(_T("\tWideDest\n"));
			StretchRectToWideDest(pSourceSurface, pSourceRect, pSourceOffset, pDestSurface, pDestRect, pDestOffset, Filter);
		} else
			StretchRectToMonoDest(pSourceSurface, pSourceRect, pSourceOffset, pDestSurface, pDestRect, Filter);
	} else
	{
		INSIDE;
		DEBUG_TRACE3(_T("\tMono\n"));
		NSCALL_TRACE2(m_Direct3DDevice.StretchRect( pSourceSurface, pSourceRect, pDestSurface, pDestRect, Filter ),
			DEBUG_MESSAGE(_T("StretchRect(pSourceSurface = %p [%s], pSourceRect = %s, ")
			_T("pDestSurface = %p [%s], pDestRect = %s, Filter = %s)"), 
			pSourceSurface, GetObjectName(pSourceSurface), GetRectString(pSourceRect), 
			pDestSurface, GetObjectName(pDestSurface), GetRectString(pDestRect), 
			GetTextureFilterTypeString(Filter)));

		if (DO_RTDUMP)
		{
			TCHAR szFileName[MAX_PATH];
			D3DSURFACE_DESC srcDesc;
			pSourceSurface->GetDesc(&srcDesc);
			if (srcDesc.Usage & D3DUSAGE_RENDERTARGET) // otherwise cause looping
			{
				_stprintf_s(szFileName, _T("%s\\%.4d.StretchRect.Source.%s.Left"), 
					DRAW_HISTORY, m_nEventCounter, GetObjectName(pSourceSurface));
				SaveSurfaceToFile(szFileName, pSourceSurface);
				DEBUG_TRACE2(_T("Dumping %s\n"), szFileName);
			}

			_stprintf_s(szFileName, L"%s\\%.4d.StretchRect.Dest.%s.Left", 
				DRAW_HISTORY, m_nEventCounter, GetObjectName(pDestSurface));
			SaveSurfaceToFile(szFileName, pDestSurface);
			DEBUG_TRACE2(_T("Dumping %s\n"), szFileName);
		}
	}
	if (DO_RTDUMP)
	{
		DEBUG_TRACE3(_T("---------- Event %i complete ----------\n"), m_nEventCounter);
		m_nEventCounter++;
	}
	if (USE_UM_PRESENTER)
	{
		FlushCommands();
	}
	
	if (SUCCEEDED(hResult))
	{
		DUMP_CMD(StretchRect,pSourceSurface,pSourceRect,pDestSurface,pDestRect,Filter);
	}

	return hResult;
}

STDMETHODIMP CWideStereoRenderer::ColorFill(IDirect3DSurface9 * pSurface, CONST RECT * pRect, D3DCOLOR color ) 
{
	DUMP_CMD_ALL(ColorFill,pSurface,pRect,color); // -- shift2u

	HRESULT hResult = S_OK;

	CONST SIZE* pOffset = NULL;
	if (pSurface)
		pOffset = GetOffset(pSurface);
	if (pOffset != NULL)
	{
		DWORD WideRev = GetSurfaceRevision(pSurface);
		DWORD Rev = WideRev + 1;
		CComPtr<IDirect3DTexture9> pParentTex;
		GetParentTexture(pSurface, &pParentTex);
		CComPtr<IDirect3DTexture9> pLeftTex;
		GetLeftTexture(pParentTex, &pLeftTex);
		IDirect3DTexture9* pWideTex = GetWideTexture(pParentTex, pLeftTex);
		if (pWideTex != NULL)
		{
			if (!RenderToRight())
			{
				DEBUG_TRACE3(_T("\tSetTextureType %p [%s]: TextureType = %s\n"), 
					pWideTex, GetObjectName(pWideTex), GetTextureTypeString(Mono));
				SetTextureType(pWideTex, Mono);
			}
			else if (pRect != NULL)
			{
				DEBUG_TRACE3(_T("\tSetTextureType %p [%s]: TextureType = %s\n"), 
					pWideTex, GetObjectName(pWideTex), GetTextureTypeString(Cleared));
				SetTextureType(pWideTex, Cleared);
			}
			CComPtr<IDirect3DSurface9> pLeftSurf;
			pLeftTex->GetSurfaceLevel(0, &pLeftSurf);
			DWORD LeftRev = GetSurfaceRevision(pLeftSurf);
			Rev = std::max(WideRev, LeftRev) + 1;
		} 
		if (pRect != NULL && RenderToRight())
		{
			RECT Rect;
			GetRightViewRect(&Rect, pRect, pOffset);
			NSCALL(m_Direct3DDevice.ColorFill( pSurface, &Rect, color ));
			DEBUG_TRACE2(_T("ColorFill(pSurface = %p [%s], pRect = %s, color = %08X);\n"), 
				pSurface, GetObjectName(pSurface), GetRectString(&Rect), color);
		}
		NSCALL(m_Direct3DDevice.ColorFill( pSurface, pRect, color ));
		DEBUG_TRACE2(_T("ColorFill(pSurface = %p [%s], pRect = %s, color = %08X);\n"), 
			pSurface, GetObjectName(pSurface), GetRectString(pRect), color);
		if (pWideTex)
			SetSurfaceRevision(pSurface, Rev);
	} else
	{
		NSCALL(m_Direct3DDevice.ColorFill( pSurface, pRect, color ));
		DEBUG_TRACE2(_T("ColorFill(pSurface = %p [%s], pRect = %s, color = %08X);\n"), 
			pSurface, GetObjectName(pSurface), GetRectString(pRect), color);
	}
	if (DO_RTDUMP)
	{
		DumpSurfaces(_T("ColorFill"), pSurface, NULL);
		DEBUG_TRACE3(_T("---------- Event %i complete ----------\n"), m_nEventCounter);
		m_nEventCounter++;
	}
	return hResult;
}

STDMETHODIMP CWideStereoRenderer::UpdateSurface(IDirect3DSurface9* pSourceSurface, CONST RECT* pSourceRect, 
													   IDirect3DSurface9* pDestinationSurface, CONST POINT* pDestPoint) 
{
	DUMP_CMD_ALL(UpdateSurface,pSourceSurface,pSourceRect,pDestinationSurface,pDestPoint);

	HRESULT hResult;

	NSCALL_TRACE2(m_Direct3DDevice.UpdateSurface(pSourceSurface, pSourceRect, 
		pDestinationSurface, pDestPoint),
		DEBUG_MESSAGE(_T("UpdateSurface(pSourceSurface = %p [%s], pSourceRect = %s, ")
		_T("pDestinationSurface = %p [%s], pDestPoint = (%d, %d))"), 
		pSourceSurface, GetObjectName(pSourceSurface), GetRectString(pSourceRect),
		pDestinationSurface, GetObjectName(pDestinationSurface), pDestPoint? pDestPoint->x: 0, pDestPoint? pDestPoint->y: 0));
	
	if (SUCCEEDED(hResult) && (!pDestPoint || (pDestPoint->x == 0 && pDestPoint->y == 0)))
	{
		bool bCalculateForAll = 
#ifdef ZLOG_ALLOW_TRACING
			(zlog::GetSeverity() == zlog::SV_FLOOD);
#else
			false;
#endif
		if(!DEBUG_SKIP_CRC_CHECK && (!g_ProfileData.TextureCRCMultiplier.empty() || bCalculateForAll))
		{
			CComPtr<IDirect3DBaseTexture9> pDestBaseTex;
			if (GetParentTexture(pDestinationSurface, &pDestBaseTex))
				CRCTextureFromSurfaceCheck(pSourceSurface, pSourceRect, pDestBaseTex);
		}
	}

	DWORD Rev;
	DWORD WideRev = GetSurfaceRevision(pDestinationSurface);
	CComPtr<IDirect3DTexture9> pWideParentTex;
	GetParentTexture(pDestinationSurface, &pWideParentTex);
	CComPtr<IDirect3DTexture9> pLeftDestTex;
	GetLeftTexture(pWideParentTex, &pLeftDestTex);
	IDirect3DTexture9* pWideDestTex = GetWideTexture(pWideParentTex, pLeftDestTex);
	if (pWideDestTex != NULL)
	{
		CComPtr<IDirect3DSurface9> pLeftSurf;
		pLeftDestTex->GetSurfaceLevel(0, &pLeftSurf);
		DWORD LeftRev = GetSurfaceRevision(pLeftSurf);
		Rev = std::max(WideRev, LeftRev) + 1;
		DEBUG_TRACE3(_T("\tSetTextureType %p [%s]: TextureType = %s\n"), 
			pWideDestTex, GetObjectName(pWideDestTex), GetTextureTypeString(Mono));
		SetTextureType(pWideDestTex, Mono);
		DEBUG_MESSAGE(_T("WARNING: UpdateSurface not implemented for RT!\n"));
	}
	else
		Rev = WideRev + 1;
	if (pWideDestTex)
		SetSurfaceRevision(pDestinationSurface, Rev);
	if (DO_RTDUMP)
	{
		DumpSurfaces(_T("UpdateSurface.Src"), pSourceSurface, NULL);
		DumpSurfaces(_T("UpdateSurface.Dst"), pDestinationSurface, NULL);
		DEBUG_TRACE3(_T("---------- Event %i complete ----------\n"), m_nEventCounter);
		m_nEventCounter++;
	}
	return hResult;
}

STDMETHODIMP CWideStereoRenderer::UpdateTexture(IDirect3DBaseTexture9* pSourceTexture, IDirect3DBaseTexture9* pDestinationTexture) 
{
	DUMP_CMD(UpdateTexture,pSourceTexture,pDestinationTexture);

	HRESULT hResult;
	CComPtr<IDirect3DBaseTexture9> pLeftDestBaseTex;
	GetLeftTexture(pDestinationTexture, &pLeftDestBaseTex);
	NSCALL_TRACE2(m_Direct3DDevice.UpdateTexture(pSourceTexture, pLeftDestBaseTex),
		DEBUG_MESSAGE(_T("UpdateTexture(pSourceTexture = %p [%s], pDestinationTexture = %p [%s])"), 
		pSourceTexture, GetObjectName(pSourceTexture), pDestinationTexture, GetObjectName(pDestinationTexture)));

	if (FAILED(hResult))
		return hResult;

	bool bCalculateForAll = 
#ifdef ZLOG_ALLOW_TRACING
		(zlog::GetSeverity() == zlog::SV_FLOOD);
#else
		false;
#endif
	if (!DEBUG_SKIP_CRC_CHECK && (!g_ProfileData.TextureCRCMultiplier.empty() || bCalculateForAll))
	{
		CComQIPtr<IDirect3DTexture9> pSrcTexture(pSourceTexture);
		CComQIPtr<IDirect3DTexture9> pDstTexture(pDestinationTexture);
		if (pSrcTexture && pDstTexture)
		{
			CComPtr<IDirect3DSurface9> pSourceSurface;
			pSrcTexture->GetSurfaceLevel(0, &pSourceSurface);
			if (pSourceSurface)
				CRCTextureFromSurfaceCheck(pSourceSurface, NULL, pDstTexture);
		}
		else
		{
			CComQIPtr<IDirect3DCubeTexture9> pSrcCubeTexture(pSourceTexture);
			CComQIPtr<IDirect3DCubeTexture9> pDstCubeTexture(pDestinationTexture);
			if (pSrcCubeTexture && pDstCubeTexture)
			{
				CComPtr<IDirect3DSurface9> pSourceSurface;
				pSrcCubeTexture->GetCubeMapSurface(D3DCUBEMAP_FACE_POSITIVE_X, 0, &pSourceSurface);
				if (pSourceSurface)
					CRCTextureFromSurfaceCheck(pSourceSurface, NULL, pDstCubeTexture);
			}
		}
	}

	CComQIPtr<IDirect3DTexture9> pLeftDestTex(pLeftDestBaseTex);
	if (pLeftDestTex == NULL)
		return hResult;

	IDirect3DTexture9* pWideDestTex = GetWideTexture(pDestinationTexture, pLeftDestTex);
	if (pWideDestTex == NULL)
		return hResult;

	CComPtr<IDirect3DSurface9> pWideSurf;
	pWideDestTex->GetSurfaceLevel(0, &pWideSurf);
	DWORD WideRev = GetSurfaceRevision(pWideSurf);
	CComPtr<IDirect3DSurface9> pLeftSurf;
	pLeftDestTex->GetSurfaceLevel(0, &pLeftSurf);
	DWORD LeftRev = GetSurfaceRevision(pLeftSurf);
	DWORD Rev = std::max(WideRev, LeftRev) + 1;
	SetSurfaceRevision(pLeftSurf, Rev);
	DEBUG_TRACE3(_T("\tSetTextureType %p [%s]: TextureType = %s\n"), 
		pWideDestTex, GetObjectName(pWideDestTex), GetTextureTypeString(Mono));
	SetTextureType(pWideDestTex, Mono);
	if (DO_RTDUMP)
	{
		DumpTextures(_T("UpdateTexture.Src"), pSourceTexture, NULL);
		DumpTextures(_T("UpdateTexture.Dst"), pDestinationTexture, NULL);
		DEBUG_TRACE3(_T("---------- Event %i complete ----------\n"), m_nEventCounter);
		m_nEventCounter++;
	}
	return hResult;
}

STDMETHODIMP CWideStereoRenderer::GetRenderTargetData(IDirect3DSurface9* pRenderTarget, IDirect3DSurface9* pDestSurface) 
{
	HRESULT hResult;
	CComPtr<IWideSurfaceData> pWideSurfaceData = GetWideSurfaceData(pRenderTarget);
	if (pWideSurfaceData)
	{
		DEBUG_TRACE3(_T("\tCheck synchronize RT\n"));
		CheckSynchronizeRT(pRenderTarget);

		D3DSURFACE_DESC Desc;
		NSCALL(pRenderTarget->GetDesc(&Desc));
		RECT Rect;
		GetSurfRect(&Desc, &Rect);
		GetNarrowRect(&Rect);
		CComPtr<IDirect3DSurface9> pSurf;
		NSCALL(m_Direct3DDevice.CreateRenderTarget(Rect.right - Rect.left, Rect.bottom - Rect.top, Desc.Format, 
			D3DMULTISAMPLE_NONE, 0, FALSE, &pSurf, 0));
		NSCALL(m_Direct3DDevice.StretchRect(pRenderTarget, &Rect, pSurf, NULL, D3DTEXF_NONE));
		NSCALL_TRACE2(m_Direct3DDevice.GetRenderTargetData(pSurf, pDestSurface),
			DEBUG_MESSAGE(_T("GetRenderTargetData(pRenderTarget = %p [%s], pDestSurface = %p [%s])"), 
			pRenderTarget, GetObjectName(pRenderTarget), pDestSurface, GetObjectName(pDestSurface)));
	}
	else
		NSCALL_TRACE2(m_Direct3DDevice.GetRenderTargetData(pRenderTarget, pDestSurface),
		DEBUG_MESSAGE(_T("GetRenderTargetData(pRenderTarget = %p [%s], pDestSurface = %p [%s])"), 
		pRenderTarget, GetObjectName(pRenderTarget), pDestSurface, GetObjectName(pDestSurface)));

	if (SUCCEEDED(hResult))
	{
		DUMP_CMD_ALL(GetRenderTargetData,pRenderTarget,pDestSurface);
	}

	return hResult;
}

inline HRESULT	CWideStereoRenderer::InitializeOffset(IDirect3DSurface9 *pOriginalSurface, SIZE *pOffset, UINT &NewWidth, UINT &NewHeight)
{
	HRESULT hResult = S_OK;
	D3DSURFACE_DESC RenderTargetDesc;
	ZeroMemory(&RenderTargetDesc, sizeof( RenderTargetDesc));
	NSCALL(pOriginalSurface->GetDesc(&RenderTargetDesc));
	return InitializeOffset( RenderTargetDesc.Width, RenderTargetDesc.Height, pOffset, NewWidth, NewHeight );
}

inline bool GetLeftTexture(IDirect3DBaseTexture9* pWideTexture, IDirect3DBaseTexture9** ppLeftTex)
{
	*ppLeftTex = NULL;
	if (!pWideTexture)
		return false;

	DWORD Size = sizeof(IUnknown *);
	pWideTexture->GetPrivateData(LeftTexture_GUID, (void*)ppLeftTex, &Size);
	if (*ppLeftTex != NULL)
		return true;
	else
	{
		*ppLeftTex = pWideTexture;
		pWideTexture->AddRef();
	}
	return false;
}

inline bool GetLeftTexture(IDirect3DBaseTexture9* pWideTexture, IDirect3DTexture9** ppLeftTex)
{
	*ppLeftTex = NULL;
	if (!pWideTexture)
		return false;

	DWORD Size = sizeof(IUnknown *);
	pWideTexture->GetPrivateData(LeftTexture_GUID, (void*)ppLeftTex, &Size);
	if (*ppLeftTex == NULL)
	{
		*ppLeftTex = (IDirect3DTexture9*)pWideTexture;
		pWideTexture->AddRef();
	}
	return *ppLeftTex != NULL;
}

inline IDirect3DTexture9* CWideStereoRenderer::GetWideTexture(IDirect3DBaseTexture9* pTexture, IDirect3DBaseTexture9* pLeftTex)
{
	IDirect3DTexture9* pWideTexture = NULL;
	if (pTexture != pLeftTex)
		pWideTexture = (IDirect3DTexture9*)pTexture;
	return pWideTexture;
}

void CWideStereoRenderer::GetStereoSurfRects( IDirect3DSurface9* pLeft, IDirect3DSurface9* &pRight, D3DFORMAT &format, RECT *pLeftRect, RECT *pRightRect )
{
	D3DSURFACE_DESC desc;
	pLeft->GetDesc(&desc);
	format = desc.Format;
	SetRect(pLeftRect, 0, 0, desc.Width, desc.Height);
	*pRightRect = *pLeftRect;
	if (pRight == NULL)
	{
		pLeftRect->bottom /= HEIGHTMUL;
		pRightRect->top = desc.Height / HEIGHTMUL;
		pRight = pLeft;
	}
}

void CWideStereoRenderer::IncRevision(IDirect3DSurface9* pWideSurf)
{
	if(!pWideSurf)
		return;

	DWORD Rev;
	DWORD WideRev = GetSurfaceRevision(pWideSurf);
	IDirect3DSurface9* pLeftSurf;
	if (GetLeftSurface(pWideSurf, &pLeftSurf))
	{
		DWORD LeftRev = GetSurfaceRevision(pLeftSurf);
		Rev = std::max(WideRev, LeftRev) + 1;
	}
	else
		Rev = WideRev + 1;
	SetSurfaceRevision(pWideSurf, Rev);
}

template<typename T, typename Q> 
void CWideStereoRenderer::SetPipelineTexture( ShaderPipelineDataEx<T, Q>& pipeline, DWORD ind, IDirect3DBaseTexture9* pLeftTexture, IDirect3DTexture9* pWideTexture )
{
	ShaderPipelineData<T, Q>& data = pipeline.m_Data;
	if (ind < data.m_MaxTextures && data.m_Texture[ind].m_pLeft != pLeftTexture)
	{
		CRCTextureCheck(data, ind, pLeftTexture);
		data.m_Texture[ind].Init(pLeftTexture, pWideTexture);
		data.m_Texture[ind].m_pWideTexture = pWideTexture;
		DEBUG_TRACE2(_T("\tpWideTex = %p [%s])\n"), data.m_Texture[ind].m_pWideTexture, GetObjectName(data.m_Texture[ind].m_pWideTexture));
		if (pWideTexture)
			pipeline.m_WideTexturesMask |= 1 << ind;
		else
			pipeline.m_WideTexturesMask &= ~(1 << ind);
		if (data.m_Texture[ind].m_pRight)
			data.m_StereoTexturesMask |= 1 << ind;
		else
			data.m_StereoTexturesMask &= ~(1 << ind);
	}
}

IDirect3DSurface9* CWideStereoRenderer::GetLeftSurface( IDirect3DSurface9* pWideSurf, IDirect3DSurface9** ppLeftSurf )
{
	*ppLeftSurf = NULL;
	DWORD size = sizeof(IUnknown *);
	pWideSurf->GetPrivateData(LeftSurface_GUID, ppLeftSurf, &size);
	return *ppLeftSurf;
}

template <typename T, typename Q>
void ShaderPipelineDataEx<T, Q>::Init()
{
	m_WideTexturesMask = 0;
}

template <typename T, typename Q>
void ShaderPipelineDataEx<T, Q>::Clear()
{
	m_WideTexturesMask = 0;
}
