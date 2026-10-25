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
#include "stdafx.h"
#include "BaseStereoRenderer.h"
#include "Trace.h"
#include "MonoRenderer_Hook.h"
#include "BaseStereoRenderer_Hook.h"
#include "..\S3DAPI\ReadData.h"
#include "..\S3DAPI\EnumToString.h"
#include "ProxyQuery.h"
#include "CommandDumper.h"
#include "ShaderHunter9.h"

STDMETHODIMP CBaseStereoRenderer::CreateVertexShader(CONST DWORD * pFunction, IDirect3DVertexShader9** ppShader)
{
	HRESULT hResult;

	// 0.4.1 ShaderHunter DX9: record the created shader so the author's
	// capture reports match what SetVertexShader will set on
	// m_CurrentShaderData.CRC32 at draw time.
	UINT  origSize = D3DXGetShaderSize(pFunction);
	DWORD origCRC  = wiz3d::dx9::HunterComputeCRC32(pFunction, origSize);
	wiz3d::dx9::HunterNoteCreated(origCRC, wiz3d::dx9::HuntStage::VS,
	                              pFunction, origSize);

	//NSCALL_TRACE2(m_pDirect3DDevice.CreateVertexShader(pFunction, ppShader),
	//	DEBUG_MESSAGE("CreateVertexShader(pFunction = %p, *ppShader = %p)",
	//	pFunction, Indirect(ppShader)));
	CComPtr<IDirect3DVertexShader9> pModifiedShader;
	NSCALL_TRACE2(m_VertexShaderConverter.Convert(pFunction, ppShader, &pModifiedShader),
		DEBUG_MESSAGE(_T("ConvertVS(pFunction = %p, *ppShader = %p)"),
		pFunction, Indirect(ppShader)));

	if (SUCCEEDED(hResult))
	{
		IDirect3DVertexShader9 *pShader = *ppShader;
		ShaderFunction function(pFunction);
		DUMP_CMD_ALL(CreateVertexShader,function,pShader);

		if (pModifiedShader)
			m_VSPipelineData.m_ShadersList.push_back(pModifiedShader);
		HOOK_GET_DEVICE(ppShader, Proxy_VertexShader_GetDevice, Original_VertexShader_GetDevice);
	}

#ifndef FINAL_RELEASE
	if(GINFO_DEBUG)
	{
		float sec = 1.0f * m_VertexShaderConverter.analyzingTime.QuadPart / m_nFreq.QuadPart;
		if (sec > 1.0f) // > 1.0s
			DEBUG_MESSAGE(_T("Vertex Shader #%d analyzed: %fs\n"), m_VertexShaderConverter.m_CurrentShaderData.shaderGlobalIndex, sec);
		m_nShaderAnalysingTime.QuadPart += m_VertexShaderConverter.analyzingTime.QuadPart;
	}
#endif
	return hResult;
}

STDMETHODIMP CBaseStereoRenderer::CreatePixelShader(CONST DWORD* pFunction, IDirect3DPixelShader9** ppShader)
{
	HRESULT hResult;

	// 0.4.1 ShaderHunter DX9: record the created shader by CRC.
	UINT  origSize = D3DXGetShaderSize(pFunction);
	DWORD origCRC  = wiz3d::dx9::HunterComputeCRC32(pFunction, origSize);
	wiz3d::dx9::HunterNoteCreated(origCRC, wiz3d::dx9::HuntStage::PS,
	                              pFunction, origSize);

	//NSCALL_TRACE2(m_pDirect3DDevice.CreatePixelShader(pFunction, ppShader),
	//	DEBUG_MESSAGE("CreatePixelShader(pFunction = %p, *ppShader = %p)",
	//	pFunction, Indirect(ppShader)));
	CComPtr<IDirect3DPixelShader9> pModifiedShader;
	NSCALL_TRACE2(m_PixelShaderConverter.Convert(pFunction, ppShader, &pModifiedShader),
		DEBUG_MESSAGE(_T("ConvertPS(pFunction = %p, *ppShader = %p)"),
		pFunction, Indirect(ppShader)));

	if (SUCCEEDED(hResult))
	{
		IDirect3DPixelShader9 *pShader = *ppShader;
		ShaderFunction function(pFunction);
		DUMP_CMD_ALL(CreatePixelShader,function,pShader);

		if (pModifiedShader)
			m_PSPipelineData.m_ShadersList.push_back(pModifiedShader);
		HOOK_GET_DEVICE(ppShader, Proxy_PixelShader_GetDevice, Original_PixelShader_GetDevice);
	}

#ifndef FINAL_RELEASE
	if(GINFO_DEBUG)
	{
		float sec = 1.0f * m_PixelShaderConverter.analyzingTime.QuadPart / m_nFreq.QuadPart;
		if (sec > 1.0f) // > 1.0s
			DEBUG_MESSAGE(_T("Pixel Shader #%d analyzed: %fs\n"), m_PixelShaderConverter.m_CurrentShaderData.shaderGlobalIndex, sec);
		m_nShaderAnalysingTime.QuadPart += m_PixelShaderConverter.analyzingTime.QuadPart;
	}
#endif

	return hResult;
}

#define DEBUG_TRACE_MODE() DEBUG_TRACE3(_T("\t%s - %s Pipeline\n"),GetModeString(), GetPipelineString(m_Pipeline));

// 0.4.1 ShaderHunter DX9: note the bound VS/PS CRCs for the frame's draw set,
// then decide whether to skip this draw entirely. Symmetric skip (no left,
// no right) so the broken effect vanishes cleanly for the hunting author.
// Called at the top of every top-level Draw method; the Splitted internal
// variants don't need this because they're reached only when the top-level
// gate let the draw through.
static inline bool HunterGateDX9(DWORD vsCRC, DWORD psCRC)
{
	wiz3d::dx9::HunterNoteDraw(vsCRC, psCRC);
	return wiz3d::dx9::HunterShouldSkipDraw(vsCRC, psCRC);
}

STDMETHODIMP CBaseStereoRenderer::DrawPrimitive(
	D3DPRIMITIVETYPE PrimitiveType, UINT StartVertex, UINT PrimitiveCount)
{
	DUMP_CMD_DRAW(DrawPrimitive,PrimitiveType,StartVertex,PrimitiveCount);
	if (HunterGateDX9(m_VSPipelineData.m_CurrentShaderData.CRC32,
	                  m_PSPipelineData.m_CurrentShaderData.CRC32))
		return S_OK;

	if( CheckDrawPassSplitting( PrimitiveType, PrimitiveCount ) )
		return DrawPrimitiveSplitted( PrimitiveType, StartVertex, PrimitiveCount);

	HRESULT hResult = S_OK;
	DEBUG_TRACE3(_T("DP(PrimitiveType = %s, StartVertex = %u, PrimitiveCount = %u);\n"), 
		GetPrimitiveTypeString(PrimitiveType), StartVertex, PrimitiveCount);

	bool bRenderTo2RT = PrepareViewStages();
	BeforeDrawCall();
	
	if (bRenderTo2RT)
	{
		RouterTypeHookCallGuard<ProxyDevice9> RTdev(&m_Direct3DDevice);
#ifndef FINAL_RELEASE
		if (!m_bRenderInStereo && m_bDebugDoNotRenderMono)
			return hResult;
#endif
		if (NeedDumpMeshes(PrimitiveCount, PrimitiveType))
			DumpMeshes( StartVertex, GetVertexCount( PrimitiveType, PrimitiveCount ));

		DEBUG_TRACE_MODE();

		SetViewStages<VIEWINDEX_RIGHT>();
		if (m_Pipeline != RHW || DEBUG_SKIP_MODIFY_RHW)
		{
			NSCALL(m_Direct3DDevice.DrawPrimitive(PrimitiveType, StartVertex, PrimitiveCount));
			SetViewStages<VIEWINDEX_LEFT>();
			NSCALL(m_Direct3DDevice.DrawPrimitive(PrimitiveType, StartVertex,	PrimitiveCount));
		}
		else
		{
			UINT VertexCount = GetVertexCount( PrimitiveType, PrimitiveCount );
			TransformPrimitiveInit( StartVertex, 0, VertexCount);
			NSCALL(m_Direct3DDevice.DrawPrimitive( PrimitiveType, StartVertex, PrimitiveCount ));
			SetViewStages<VIEWINDEX_LEFT>();
			TransformPrimitive( StartVertex);
			NSCALL(m_Direct3DDevice.DrawPrimitive( PrimitiveType, StartVertex, PrimitiveCount ));
			RestoreVertices();
		}
		SetMonoViewStage();
		if (IsStereoRender())
			UpdateTexturesType(Stereo);
	}
	else 
	{
		UpdateTexturesType(Mono);
		DEBUG_TRACE3(_T("\tMono\n"));
		NSCALL(m_Direct3DDevice.DrawPrimitive( PrimitiveType, StartVertex, PrimitiveCount ));
	}
	DUMPTEXTURESANDRT(_T("DP"), bRenderTo2RT, false);
	AfterDrawCall();
	return hResult;
}

static const unsigned	SCISSOR_SPLIT_STEP		= 384;
static const unsigned	SPLIT_STEP_SLEEP_TIME	= 0;

HRESULT CBaseStereoRenderer::DrawPrimitiveSplitted( 
	D3DPRIMITIVETYPE PrimitiveType, UINT StartVertex, UINT PrimitiveCount) 
{
	HRESULT hResult = S_OK;
	DEBUG_TRACE3(_T("DPS(PrimitiveType = %s, StartVertex = %u, PrimitiveCount = %u);\n"), 
		GetPrimitiveTypeString(PrimitiveType), StartVertex, PrimitiveCount);

	bool bRenderTo2RT = PrepareViewStages();
	BeforeDrawCall();

	if( bRenderTo2RT )
	{
#ifndef FINAL_RELEASE
		if (!m_bRenderInStereo && m_bDebugDoNotRenderMono)
			return hResult;
#endif
		if (NeedDumpMeshes(PrimitiveCount, PrimitiveType))
			DumpMeshes( StartVertex, GetVertexCount( PrimitiveType, PrimitiveCount ));

		DEBUG_TRACE_MODE();
	}

	for( unsigned xScissorPos=0; xScissorPos<m_ViewPort.Width; xScissorPos+=SCISSOR_SPLIT_STEP )
	{
		// split the draw pass with the scissor rect
		RECT	scissorRect;
		scissorRect.left	= xScissorPos;
		scissorRect.right	= std::min<LONG>( m_ViewPort.Width, xScissorPos+SCISSOR_SPLIT_STEP );
		scissorRect.top		= 0;
		scissorRect.bottom	= m_ViewPort.Height * (gInfo.WideRenderTarget ? 2 : 1);	// N.B. over/under format!

		NSCALL( m_Direct3DDevice.SetScissorRect( &m_ScissorRect) );

		if (bRenderTo2RT)
		{
			RouterTypeHookCallGuard<ProxyDevice9> RTdev(&m_Direct3DDevice);

			SetViewStages<VIEWINDEX_RIGHT>();
			if (m_Pipeline != RHW || DEBUG_SKIP_MODIFY_RHW)
			{

				NSCALL( m_Direct3DDevice.SetRenderState( D3DRS_SCISSORTESTENABLE, TRUE ) );
				NSCALL( m_Direct3DDevice.SetScissorRect( &scissorRect ) );
				NSCALL(m_Direct3DDevice.DrawPrimitive(PrimitiveType, StartVertex, PrimitiveCount));
				SetViewStages<VIEWINDEX_LEFT>();
				NSCALL( m_Direct3DDevice.SetRenderState( D3DRS_SCISSORTESTENABLE, TRUE ) );
				NSCALL( m_Direct3DDevice.SetScissorRect( &scissorRect ) );
				NSCALL(m_Direct3DDevice.DrawPrimitive(PrimitiveType, StartVertex,	PrimitiveCount));
			}
			else
			{
				UINT VertexCount = GetVertexCount( PrimitiveType, PrimitiveCount );
				TransformPrimitiveInit( StartVertex, 0, VertexCount);
				NSCALL( m_Direct3DDevice.SetRenderState( D3DRS_SCISSORTESTENABLE, TRUE ) );
				NSCALL( m_Direct3DDevice.SetScissorRect( &scissorRect ) );
				NSCALL(m_Direct3DDevice.DrawPrimitive( PrimitiveType, StartVertex, PrimitiveCount ));
				SetViewStages<VIEWINDEX_LEFT>();
				TransformPrimitive( StartVertex);
				NSCALL( m_Direct3DDevice.SetRenderState( D3DRS_SCISSORTESTENABLE, TRUE ) );
				NSCALL( m_Direct3DDevice.SetScissorRect( &scissorRect ) );
				NSCALL(m_Direct3DDevice.DrawPrimitive( PrimitiveType, StartVertex, PrimitiveCount ));
				RestoreVertices();
			}
			SetMonoViewStage();
			if (IsStereoRender())
				UpdateTexturesType(Stereo);
		}
		else 
		{
			UpdateTexturesType(Mono);
			DEBUG_TRACE3(_T("\tMono\n"));
			NSCALL( m_Direct3DDevice.SetRenderState( D3DRS_SCISSORTESTENABLE, TRUE ) );
			NSCALL( m_Direct3DDevice.SetScissorRect( &scissorRect ) );
			NSCALL(m_Direct3DDevice.DrawPrimitive( PrimitiveType, StartVertex, PrimitiveCount ));
		}
	} 
	DUMPTEXTURESANDRT(_T("DPS"), bRenderTo2RT, true);
	AfterDrawCall();

	NSCALL( m_Direct3DDevice.SetRenderState( D3DRS_SCISSORTESTENABLE, m_dwScissorTestEnable ) );
	NSCALL( m_Direct3DDevice.SetScissorRect( &m_ScissorRect) );

	return hResult;
}


STDMETHODIMP CBaseStereoRenderer::DrawIndexedPrimitive(
	D3DPRIMITIVETYPE PrimitiveType,
	INT BaseVertexIndex, UINT MinVertexIndex, UINT NumVertices, UINT StartIndex,
	UINT PrimitiveCount)
{
	DUMP_CMD_DRAW(DrawIndexedPrimitive,PrimitiveType,BaseVertexIndex,MinVertexIndex,NumVertices,StartIndex,PrimitiveCount);
	if (HunterGateDX9(m_VSPipelineData.m_CurrentShaderData.CRC32,
	                  m_PSPipelineData.m_CurrentShaderData.CRC32))
		return S_OK;

	if( CheckDrawPassSplitting( PrimitiveType, PrimitiveCount ) )
		return DrawIndexedPrimitiveSplitted( PrimitiveType, BaseVertexIndex, MinVertexIndex, NumVertices, StartIndex, PrimitiveCount );

	HRESULT hResult = S_OK;
	DEBUG_TRACE3(_T("DIP(PrimitiveType = %s, BaseVertexIndex = %d, ")
		_T("MinVertexIndex = %u, NumVertices = %u, StartIndex = %u, PrimitiveCount = %u);\n"),
		GetPrimitiveTypeString(PrimitiveType), BaseVertexIndex,
		MinVertexIndex, NumVertices, StartIndex, PrimitiveCount);

	bool bRenderTo2RT = PrepareViewStages();
	BeforeDrawCall();

	if (bRenderTo2RT)
	{
		RouterTypeHookCallGuard<ProxyDevice9> RTdev(&m_Direct3DDevice);

#ifndef FINAL_RELEASE
		if (!m_bRenderInStereo && m_bDebugDoNotRenderMono)
			return hResult;
#endif
		if (NeedDumpMeshes(PrimitiveCount, PrimitiveType))
			DumpIndexedMeshes(BaseVertexIndex, MinVertexIndex, NumVertices, 
				StartIndex, GetVertexCount( PrimitiveType, PrimitiveCount ));

		DEBUG_TRACE_MODE();
		
		SetViewStages<VIEWINDEX_RIGHT>();
		if (m_Pipeline != RHW || DEBUG_SKIP_MODIFY_RHW)
		{
			NSCALL( m_Direct3DDevice.DrawIndexedPrimitive( PrimitiveType,
				BaseVertexIndex, MinVertexIndex, NumVertices, StartIndex, PrimitiveCount ));
			SetViewStages<VIEWINDEX_LEFT>();
			NSCALL(m_Direct3DDevice.DrawIndexedPrimitive( PrimitiveType,
				BaseVertexIndex, MinVertexIndex, NumVertices, StartIndex, PrimitiveCount ));
		}
		else
		{
			TransformPrimitiveInit( (UINT&)BaseVertexIndex, MinVertexIndex, NumVertices );
			NSCALL(m_Direct3DDevice.DrawIndexedPrimitive( PrimitiveType,
				BaseVertexIndex, MinVertexIndex, NumVertices, StartIndex, PrimitiveCount ));
			SetViewStages<VIEWINDEX_LEFT>();
			TransformPrimitive( (UINT&)BaseVertexIndex );
			NSCALL(m_Direct3DDevice.DrawIndexedPrimitive( PrimitiveType,
				BaseVertexIndex, MinVertexIndex, NumVertices, StartIndex, PrimitiveCount ));
			RestoreVertices();
		}
		SetMonoViewStage();
		if (IsStereoRender())
			UpdateTexturesType(Stereo);
	}
	else 
	{
		UpdateTexturesType(Mono);
		DEBUG_TRACE3(_T("\tMono\n"));
		NSCALL(m_Direct3DDevice.DrawIndexedPrimitive(PrimitiveType,
			BaseVertexIndex, MinVertexIndex, NumVertices, StartIndex, PrimitiveCount));
	} 
	DUMPTEXTURESANDRT(_T("DIP"), bRenderTo2RT, false);
	AfterDrawCall();
	return hResult;
}

HRESULT CBaseStereoRenderer::DrawIndexedPrimitiveSplitted(
	D3DPRIMITIVETYPE PrimitiveType,
	INT BaseVertexIndex, UINT MinVertexIndex, UINT NumVertices, UINT StartIndex,
	UINT PrimitiveCount) 
{
	HRESULT hResult = S_OK;
	DEBUG_TRACE3(_T("DIPS(PrimitiveType = %s, BaseVertexIndex = %d, ")
		_T("MinVertexIndex = %u, NumVertices = %u, StartIndex = %u, PrimitiveCount = %u);\n"),
		GetPrimitiveTypeString(PrimitiveType), BaseVertexIndex,
		MinVertexIndex, NumVertices, StartIndex, PrimitiveCount);

	bool bRenderTo2RT = PrepareViewStages();
	BeforeDrawCall();

	if( bRenderTo2RT )
	{
#ifndef FINAL_RELEASE
		if (!m_bRenderInStereo && m_bDebugDoNotRenderMono)
			return hResult;
#endif
		if (NeedDumpMeshes(PrimitiveCount, PrimitiveType))
			DumpIndexedMeshes(BaseVertexIndex, MinVertexIndex, NumVertices, 
			StartIndex, GetVertexCount( PrimitiveType, PrimitiveCount ));

		DEBUG_TRACE_MODE();
	}

	for( unsigned xScissorPos=0; xScissorPos<m_ViewPort.Width; xScissorPos+=SCISSOR_SPLIT_STEP )
	{
		// split the draw pass with the scissor rect
		RECT	scissorRect;
		scissorRect.left	= xScissorPos;
		scissorRect.right	= std::min<LONG>( m_ViewPort.Width, xScissorPos+SCISSOR_SPLIT_STEP );
		scissorRect.top		= 0;
		scissorRect.bottom	= m_ViewPort.Height * (gInfo.WideRenderTarget ? 2 : 1);	// N.B. over/under format!

		if (bRenderTo2RT)
		{
			RouterTypeHookCallGuard<ProxyDevice9> RTdev(&m_Direct3DDevice);

			SetViewStages<VIEWINDEX_RIGHT>();
			if (m_Pipeline != RHW || DEBUG_SKIP_MODIFY_RHW)
			{
				NSCALL( m_Direct3DDevice.SetRenderState( D3DRS_SCISSORTESTENABLE, TRUE ) );
				NSCALL( m_Direct3DDevice.SetScissorRect( &scissorRect ) );
				NSCALL( m_Direct3DDevice.DrawIndexedPrimitive( PrimitiveType,
					BaseVertexIndex, MinVertexIndex, NumVertices, StartIndex, PrimitiveCount ));
				SetViewStages<VIEWINDEX_LEFT>();
				NSCALL( m_Direct3DDevice.SetRenderState( D3DRS_SCISSORTESTENABLE, TRUE ) );
				NSCALL( m_Direct3DDevice.SetScissorRect( &scissorRect ) );
				NSCALL(m_Direct3DDevice.DrawIndexedPrimitive( PrimitiveType,
					BaseVertexIndex, MinVertexIndex, NumVertices, StartIndex, PrimitiveCount ));
			}
			else
			{
				TransformPrimitiveInit( (UINT&)BaseVertexIndex, MinVertexIndex, NumVertices );
				NSCALL( m_Direct3DDevice.SetRenderState( D3DRS_SCISSORTESTENABLE, TRUE ) );
				NSCALL( m_Direct3DDevice.SetScissorRect( &scissorRect ) );
				NSCALL(m_Direct3DDevice.DrawIndexedPrimitive( PrimitiveType,
					BaseVertexIndex, MinVertexIndex, NumVertices, StartIndex, PrimitiveCount ));
				SetViewStages<VIEWINDEX_LEFT>();
				TransformPrimitive( (UINT&)BaseVertexIndex );
				NSCALL( m_Direct3DDevice.SetRenderState( D3DRS_SCISSORTESTENABLE, TRUE ) );
				NSCALL( m_Direct3DDevice.SetScissorRect( &scissorRect ) );
				NSCALL(m_Direct3DDevice.DrawIndexedPrimitive( PrimitiveType,
					BaseVertexIndex, MinVertexIndex, NumVertices, StartIndex, PrimitiveCount ));
				RestoreVertices();
			}
			SetMonoViewStage();
			if (IsStereoRender())
				UpdateTexturesType(Stereo);
		}
		else 
		{
			UpdateTexturesType(Mono);
			DEBUG_TRACE3(_T("\tMono\n"));
			NSCALL( m_Direct3DDevice.SetRenderState( D3DRS_SCISSORTESTENABLE, TRUE ) );
			NSCALL( m_Direct3DDevice.SetScissorRect( &scissorRect ) );
			NSCALL(m_Direct3DDevice.DrawIndexedPrimitive(PrimitiveType,
				BaseVertexIndex, MinVertexIndex, NumVertices, StartIndex, PrimitiveCount));
		}
	} 
	DUMPTEXTURESANDRT(_T("DIPS"), bRenderTo2RT, true);
	AfterDrawCall();

	NSCALL( m_Direct3DDevice.SetRenderState( D3DRS_SCISSORTESTENABLE, m_dwScissorTestEnable ) );
	NSCALL( m_Direct3DDevice.SetScissorRect( &m_ScissorRect) );

	return hResult;
}

STDMETHODIMP CBaseStereoRenderer::DrawPrimitiveUP(
	D3DPRIMITIVETYPE PrimitiveType, UINT PrimitiveCount,
	CONST VOID * pVertexStreamZeroData, UINT VertexStreamZeroStride )
{
	JustData vertexStreamZeroData(pVertexStreamZeroData,VertexStreamZeroStride * GetVertexCount(PrimitiveType,PrimitiveCount));
	DUMP_CMD_DRAW(DrawPrimitiveUP,PrimitiveType,PrimitiveCount,vertexStreamZeroData,VertexStreamZeroStride);
	if (HunterGateDX9(m_VSPipelineData.m_CurrentShaderData.CRC32,
	                  m_PSPipelineData.m_CurrentShaderData.CRC32))
		return S_OK;

	if( CheckDrawPassSplitting( PrimitiveType, PrimitiveCount ) )
		return DrawPrimitiveUPSplitted( PrimitiveType, PrimitiveCount, pVertexStreamZeroData, VertexStreamZeroStride );

	HRESULT hResult = S_OK;
	DEBUG_TRACE3(_T("DPUP(PrimitiveType = %s, PrimitiveCount = %u, ")
		_T("pVertexStreamZeroData = %p, VertexStreamZeroStride = %u);\n"),
		GetPrimitiveTypeString(PrimitiveType), PrimitiveCount,
		pVertexStreamZeroData, VertexStreamZeroStride);

	bool bRenderTo2RT = PrepareViewStages(true);
	BeforeDrawCall();

	if (bRenderTo2RT)
	{
		RouterTypeHookCallGuard<ProxyDevice9> RTdev(&m_Direct3DDevice);

#ifndef FINAL_RELEASE
		if (!m_bRenderInStereo && m_bDebugDoNotRenderMono)
			return hResult;
#endif
		if (NeedDumpMeshes(PrimitiveCount, PrimitiveType))
			DumpMeshesUP(GetVertexCount( PrimitiveType, PrimitiveCount ), 
			pVertexStreamZeroData, VertexStreamZeroStride);

		DEBUG_TRACE_MODE();

		SetViewStages<VIEWINDEX_RIGHT>();
		if (m_Pipeline != RHW || DEBUG_SKIP_MODIFY_RHW)
		{
			NSCALL(m_Direct3DDevice.DrawPrimitiveUP( PrimitiveType, PrimitiveCount, 
				pVertexStreamZeroData, VertexStreamZeroStride ));
			SetViewStages<VIEWINDEX_LEFT>();
			NSCALL(m_Direct3DDevice.DrawPrimitiveUP( PrimitiveType, PrimitiveCount,
				pVertexStreamZeroData, VertexStreamZeroStride ));
		}
		else
		{
			UINT VertexCount = GetVertexCount( PrimitiveType, PrimitiveCount );
			NSCALL(m_Direct3DDevice.DrawPrimitiveUP( PrimitiveType, PrimitiveCount,
				TransformPrimitiveUPInit( VertexCount, pVertexStreamZeroData, 
				VertexStreamZeroStride ), VertexStreamZeroStride ));
			SetViewStages<VIEWINDEX_LEFT>();
			NSCALL(m_Direct3DDevice.DrawPrimitiveUP( PrimitiveType, PrimitiveCount,
				TransformPrimitiveUP( VertexCount, pVertexStreamZeroData, 
				VertexStreamZeroStride ), VertexStreamZeroStride ));
		}
		SetMonoViewStage();
		if (IsStereoRender())
			UpdateTexturesType(Stereo);
	}
	else 
	{
		UpdateTexturesType(Mono);
		DEBUG_TRACE3(_T("\tMono\n"));
		NSCALL(m_Direct3DDevice.DrawPrimitiveUP( PrimitiveType, PrimitiveCount, 
			pVertexStreamZeroData, VertexStreamZeroStride ));
	} 
	DUMPTEXTURESANDRT(_T("DPUP"), bRenderTo2RT, false);
	AfterDrawCall();
	return hResult;
}

HRESULT CBaseStereoRenderer::DrawPrimitiveUPSplitted(
	D3DPRIMITIVETYPE PrimitiveType, UINT PrimitiveCount, 
	CONST VOID * pVertexStreamZeroData, UINT VertexStreamZeroStride ) 
{
	HRESULT hResult = S_OK;
	DEBUG_TRACE3(_T("DPUPS(PrimitiveType = %s, PrimitiveCount = %u, ")
		_T("pVertexStreamZeroData = %p, VertexStreamZeroStride = %u);\n"),
		GetPrimitiveTypeString(PrimitiveType), PrimitiveCount,
		pVertexStreamZeroData, VertexStreamZeroStride);

	bool bRenderTo2RT = PrepareViewStages(true);
	BeforeDrawCall();

	if (bRenderTo2RT)
	{
		RouterTypeHookCallGuard<ProxyDevice9> RTdev(&m_Direct3DDevice);

#ifndef FINAL_RELEASE
		if (!m_bRenderInStereo && m_bDebugDoNotRenderMono)
			return hResult;
#endif
		if (NeedDumpMeshes(PrimitiveCount, PrimitiveType))
			DumpMeshesUP(GetVertexCount( PrimitiveType, PrimitiveCount ), 
			pVertexStreamZeroData, VertexStreamZeroStride);

		DEBUG_TRACE_MODE();
	}

	for( unsigned xScissorPos=0; xScissorPos<m_ViewPort.Width; xScissorPos+=SCISSOR_SPLIT_STEP )
	{
		// split the draw pass with the scissor rect
		RECT	scissorRect;
		scissorRect.left	= xScissorPos;
		scissorRect.right	= std::min<LONG>( m_ViewPort.Width, xScissorPos+SCISSOR_SPLIT_STEP );
		scissorRect.top		= 0;
		scissorRect.bottom	= m_ViewPort.Height * (gInfo.WideRenderTarget ? 2 : 1);	// N.B. over/under format!

		NSCALL( m_Direct3DDevice.SetScissorRect( &m_ScissorRect) );
		if (bRenderTo2RT)
		{
			RouterTypeHookCallGuard<ProxyDevice9> RTdev(&m_Direct3DDevice);

			SetViewStages<VIEWINDEX_RIGHT>();
			if (m_Pipeline != RHW || DEBUG_SKIP_MODIFY_RHW)
			{
				NSCALL(m_Direct3DDevice.DrawPrimitiveUP( PrimitiveType, PrimitiveCount, 
					pVertexStreamZeroData, VertexStreamZeroStride ));
				SetViewStages<VIEWINDEX_LEFT>();
				NSCALL(m_Direct3DDevice.DrawPrimitiveUP( PrimitiveType, PrimitiveCount,
					pVertexStreamZeroData, VertexStreamZeroStride ));
			}
			else
			{
				UINT VertexCount = GetVertexCount( PrimitiveType, PrimitiveCount );
				NSCALL(m_Direct3DDevice.DrawPrimitiveUP( PrimitiveType, PrimitiveCount,
					TransformPrimitiveUPInit( VertexCount, pVertexStreamZeroData, 
					VertexStreamZeroStride ), VertexStreamZeroStride ));
				SetViewStages<VIEWINDEX_LEFT>();
				NSCALL(m_Direct3DDevice.DrawPrimitiveUP( PrimitiveType, PrimitiveCount,
					TransformPrimitiveUP( VertexCount, pVertexStreamZeroData, 
					VertexStreamZeroStride ), VertexStreamZeroStride ));
			}
			SetMonoViewStage();
			if (IsStereoRender())
				UpdateTexturesType(Stereo);
		}
		else 
		{
			UpdateTexturesType(Mono);
			DEBUG_TRACE3(_T("\tMono\n"));
			NSCALL(m_Direct3DDevice.DrawPrimitiveUP( PrimitiveType, PrimitiveCount, 
				pVertexStreamZeroData, VertexStreamZeroStride ));
		}
	}
	DUMPTEXTURESANDRT(_T("DPUPS"), bRenderTo2RT, false);
	AfterDrawCall();

	NSCALL( m_Direct3DDevice.SetRenderState( D3DRS_SCISSORTESTENABLE, m_dwScissorTestEnable ) );
	NSCALL( m_Direct3DDevice.SetScissorRect( &m_ScissorRect) );

	return hResult;
}

STDMETHODIMP CBaseStereoRenderer::DrawIndexedPrimitiveUP( 
	D3DPRIMITIVETYPE PrimitiveType, UINT MinIndex, 
	UINT NumVertices, UINT PrimitiveCount, 
	CONST VOID * pIndexData, D3DFORMAT IndexDataFormat, 
	CONST VOID * pVertexStreamZeroData, 
	UINT VertexStreamZeroStride ) 
{
#ifndef FINAL_RELEASE
	if (IndexDataFormat == D3DFMT_INDEX16 || IndexDataFormat == D3DFMT_INDEX32)
	{
		int indexSize = IndexDataFormat == D3DFMT_INDEX16 ? 2 : 4;

		int indexCount = GetVertexCount(PrimitiveType,PrimitiveCount);
		JustData indexData(pIndexData,indexCount * indexSize);

		int vertexCount = NumVertices;
		JustData vertexStreamZeroData(pVertexStreamZeroData,vertexCount * VertexStreamZeroStride);

		DUMP_CMD_DRAW(DrawIndexedPrimitiveUP,PrimitiveType,MinIndex,NumVertices,PrimitiveCount,indexData,IndexDataFormat,vertexStreamZeroData,VertexStreamZeroStride);
	}
#endif // FINAL_RELEASE

	if (HunterGateDX9(m_VSPipelineData.m_CurrentShaderData.CRC32,
	                  m_PSPipelineData.m_CurrentShaderData.CRC32))
		return S_OK;

	if( CheckDrawPassSplitting( PrimitiveType, PrimitiveCount ) )
		return DrawIndexedPrimitiveUPSplitted( PrimitiveType, MinIndex, NumVertices, PrimitiveCount,
			pIndexData, IndexDataFormat, pVertexStreamZeroData, VertexStreamZeroStride);

	HRESULT hResult = S_OK;
	DEBUG_TRACE3(_T("DIPUP(PrimitiveType = %s, MinIndex = %u, ")
		_T("NumVertices = %u, PrimitiveCount = %u, pIndexData = %p, ")
		_T("IndexDataFormat = %s, pVertexStreamZeroData = %p, VertexStreamZeroStride = %u);\n"),
		GetPrimitiveTypeString(PrimitiveType), MinIndex, NumVertices, PrimitiveCount,
		pIndexData, GetFormatString(IndexDataFormat), pVertexStreamZeroData, 
		VertexStreamZeroStride);

	bool bRenderTo2RT = PrepareViewStages(true);
	BeforeDrawCall();

	if (bRenderTo2RT)
	{
		RouterTypeHookCallGuard<ProxyDevice9> RTdev(&m_Direct3DDevice);

#ifndef FINAL_RELEASE
		if (!m_bRenderInStereo && m_bDebugDoNotRenderMono)
			return hResult;
#endif 
		if (NeedDumpMeshes(PrimitiveCount, PrimitiveType))
			DumpIndexedMeshesUP(MinIndex, NumVertices, pIndexData, IndexDataFormat,
			GetVertexCount( PrimitiveType, PrimitiveCount ), pVertexStreamZeroData,  VertexStreamZeroStride );

		DEBUG_TRACE_MODE();

		SetViewStages<VIEWINDEX_RIGHT>();
		if (m_Pipeline != RHW || DEBUG_SKIP_MODIFY_RHW)
		{
			NSCALL(m_Direct3DDevice.DrawIndexedPrimitiveUP( PrimitiveType, MinIndex,
				NumVertices, PrimitiveCount, pIndexData, IndexDataFormat, pVertexStreamZeroData, 
				VertexStreamZeroStride ));
			SetViewStages<VIEWINDEX_LEFT>();
			NSCALL(m_Direct3DDevice.DrawIndexedPrimitiveUP( PrimitiveType, MinIndex,
				NumVertices, PrimitiveCount, pIndexData, IndexDataFormat, pVertexStreamZeroData, 
				VertexStreamZeroStride ));
		}
		else
		{
			NSCALL(m_Direct3DDevice.DrawIndexedPrimitiveUP( PrimitiveType, 
				MinIndex, NumVertices, PrimitiveCount, pIndexData, IndexDataFormat,
				TransformPrimitiveUPInit( NumVertices, pVertexStreamZeroData, VertexStreamZeroStride ),
				VertexStreamZeroStride ));
			SetViewStages<VIEWINDEX_LEFT>();
			NSCALL(m_Direct3DDevice.DrawIndexedPrimitiveUP( PrimitiveType, 
				MinIndex, NumVertices, PrimitiveCount, pIndexData, IndexDataFormat,
				TransformPrimitiveUP( NumVertices, pVertexStreamZeroData, VertexStreamZeroStride ),
				VertexStreamZeroStride ));
		}
		SetMonoViewStage();
		if (IsStereoRender())
			UpdateTexturesType(Stereo);
	}
	else 
	{
		UpdateTexturesType(Mono);
		DEBUG_TRACE3(_T("\tMono\n"));
		NSCALL(m_Direct3DDevice.DrawIndexedPrimitiveUP( PrimitiveType, MinIndex, 
			NumVertices, PrimitiveCount, pIndexData, IndexDataFormat, 
			pVertexStreamZeroData, VertexStreamZeroStride ));
	} 
	DUMPTEXTURESANDRT(_T("DIPUP"), bRenderTo2RT, false);
	AfterDrawCall();
	return hResult;
}

HRESULT CBaseStereoRenderer::DrawIndexedPrimitiveUPSplitted( 
	D3DPRIMITIVETYPE PrimitiveType, UINT MinIndex, 
	UINT NumVertices, UINT PrimitiveCount, 
	CONST VOID * pIndexData, D3DFORMAT IndexDataFormat, 
	CONST VOID * pVertexStreamZeroData, 
	UINT VertexStreamZeroStride ) 
{
	HRESULT hResult = S_OK;
	DEBUG_TRACE3(_T("DIPUPS(PrimitiveType = %s, MinIndex = %u, ")
		_T("NumVertices = %u, PrimitiveCount = %u, pIndexData = %p, ")
		_T("IndexDataFormat = %s, pVertexStreamZeroData = %p, VertexStreamZeroStride = %u);\n"),
		GetPrimitiveTypeString(PrimitiveType), MinIndex, NumVertices, PrimitiveCount,
		pIndexData, GetFormatString(IndexDataFormat), pVertexStreamZeroData, 
		VertexStreamZeroStride);

	bool bRenderTo2RT = PrepareViewStages(true);
	BeforeDrawCall();

	if (bRenderTo2RT)
	{
		RouterTypeHookCallGuard<ProxyDevice9> RTdev(&m_Direct3DDevice);

#ifndef FINAL_RELEASE
		if (!m_bRenderInStereo && m_bDebugDoNotRenderMono)
			return hResult;
#endif 
		if (NeedDumpMeshes(PrimitiveCount, PrimitiveType))
			DumpIndexedMeshesUP(MinIndex, NumVertices, pIndexData, IndexDataFormat,
			GetVertexCount( PrimitiveType, PrimitiveCount ), pVertexStreamZeroData,  VertexStreamZeroStride );

		DEBUG_TRACE_MODE();
	}

	for( unsigned xScissorPos=0; xScissorPos<m_ViewPort.Width; xScissorPos+=SCISSOR_SPLIT_STEP )
	{
		// split the draw pass with the scissor rect
		RECT	scissorRect;
		scissorRect.left	= xScissorPos;
		scissorRect.right	= std::min<LONG>( m_ViewPort.Width, xScissorPos+SCISSOR_SPLIT_STEP );
		scissorRect.top		= 0;
		scissorRect.bottom	= m_ViewPort.Height * (gInfo.WideRenderTarget ? 2 : 1);	// N.B. over/under format!

		NSCALL( m_Direct3DDevice.SetScissorRect( &m_ScissorRect) );
		if (bRenderTo2RT)
		{
			RouterTypeHookCallGuard<ProxyDevice9> RTdev(&m_Direct3DDevice);

			SetViewStages<VIEWINDEX_RIGHT>();
			if (m_Pipeline != RHW || DEBUG_SKIP_MODIFY_RHW)
			{
				NSCALL(m_Direct3DDevice.DrawIndexedPrimitiveUP( PrimitiveType, MinIndex,
					NumVertices, PrimitiveCount, pIndexData, IndexDataFormat, pVertexStreamZeroData, 
					VertexStreamZeroStride ));
				SetViewStages<VIEWINDEX_LEFT>();
				NSCALL(m_Direct3DDevice.DrawIndexedPrimitiveUP( PrimitiveType, MinIndex,
					NumVertices, PrimitiveCount, pIndexData, IndexDataFormat, pVertexStreamZeroData, 
					VertexStreamZeroStride ));
			}
			else
			{
				NSCALL(m_Direct3DDevice.DrawIndexedPrimitiveUP( PrimitiveType, 
					MinIndex, NumVertices, PrimitiveCount, pIndexData, IndexDataFormat,
					TransformPrimitiveUPInit( NumVertices, pVertexStreamZeroData, VertexStreamZeroStride ),
					VertexStreamZeroStride ));
				SetViewStages<VIEWINDEX_LEFT>();
				NSCALL(m_Direct3DDevice.DrawIndexedPrimitiveUP( PrimitiveType, 
					MinIndex, NumVertices, PrimitiveCount, pIndexData, IndexDataFormat,
					TransformPrimitiveUP( NumVertices, pVertexStreamZeroData, VertexStreamZeroStride ),
					VertexStreamZeroStride ));
			}
			SetMonoViewStage();
			if (IsStereoRender())
				UpdateTexturesType(Stereo);
		}
		else 
		{
			UpdateTexturesType(Mono);
			DEBUG_TRACE3(_T("\tMono\n"));
			NSCALL(m_Direct3DDevice.DrawIndexedPrimitiveUP( PrimitiveType, MinIndex, 
				NumVertices, PrimitiveCount, pIndexData, IndexDataFormat, 
				pVertexStreamZeroData, VertexStreamZeroStride ));
		}
	}
	DUMPTEXTURESANDRT(_T("DIPUPS"), bRenderTo2RT, false);
	AfterDrawCall();

	NSCALL( m_Direct3DDevice.SetRenderState( D3DRS_SCISSORTESTENABLE, m_dwScissorTestEnable ) );
	NSCALL( m_Direct3DDevice.SetScissorRect( &m_ScissorRect) );

	return hResult;
}

STDMETHODIMP CBaseStereoRenderer::DrawRectPatch(UINT Handle, CONST float * pNumSegs, CONST D3DRECTPATCH_INFO * pRectPatchInfo) 
{
	DUMP_CMD_DRAW(DrawRectPatch,Handle,pNumSegs,pRectPatchInfo);

	HRESULT hResult = S_OK;
	DEBUG_TRACE3(_T("DRP(Handle = %u, pNumSegs = %p, pRectPatchInfo = %p);\n"), 
		Handle, pNumSegs, pRectPatchInfo);

	bool bRenderTo2RT = PrepareViewStages();
	BeforeDrawCall();

	if (bRenderTo2RT)
	{
		RouterTypeHookCallGuard<ProxyDevice9> RTdev(&m_Direct3DDevice);

#ifndef FINAL_RELEASE
		if (!m_bRenderInStereo && m_bDebugDoNotRenderMono)
			return hResult;
#endif
		DEBUG_TRACE_MODE();
		SetViewStages<VIEWINDEX_RIGHT>();
		NSCALL(m_Direct3DDevice.DrawRectPatch( Handle, pNumSegs, pRectPatchInfo ));
		SetViewStages<VIEWINDEX_LEFT>();
		NSCALL(m_Direct3DDevice.DrawRectPatch( Handle, pNumSegs, pRectPatchInfo ));
		SetMonoViewStage();
		if (IsStereoRender())
			UpdateTexturesType(Stereo);
	}
	else 
	{
		UpdateTexturesType(Mono);
		DEBUG_TRACE3(_T("\tMono\n"));
		NSCALL(m_Direct3DDevice.DrawRectPatch( Handle, pNumSegs, pRectPatchInfo ));
	} 
	DUMPTEXTURESANDRT(_T("DRP"), bRenderTo2RT, false);
	AfterDrawCall();
	return hResult;
}

STDMETHODIMP CBaseStereoRenderer::DrawTriPatch(
	UINT Handle, CONST float * pNumSegs, CONST D3DTRIPATCH_INFO * pTriPatchInfo) 
{
	DUMP_CMD_DRAW(DrawTriPatch,Handle,pNumSegs,pTriPatchInfo);

	HRESULT hResult = S_OK;
	DEBUG_TRACE3(_T("DTP(Handle = %u, pNumSegs = %p, pTriPatchInfo = %p);\n"), 
		Handle, pNumSegs, pTriPatchInfo);

	bool bRenderTo2RT = PrepareViewStages();
	BeforeDrawCall();

	if (bRenderTo2RT)
	{
		RouterTypeHookCallGuard<ProxyDevice9> RTdev(&m_Direct3DDevice);

#ifndef FINAL_RELEASE
		if (!m_bRenderInStereo && m_bDebugDoNotRenderMono)
			return hResult;
#endif
		DEBUG_TRACE_MODE();
		SetViewStages<VIEWINDEX_RIGHT>();
		NSCALL(m_Direct3DDevice.DrawTriPatch( Handle, pNumSegs, pTriPatchInfo ));
		SetViewStages<VIEWINDEX_LEFT>();
		NSCALL(m_Direct3DDevice.DrawTriPatch( Handle, pNumSegs, pTriPatchInfo ));
		SetMonoViewStage();
		if (IsStereoRender())
			UpdateTexturesType(Stereo);
	}
	else 
	{
		UpdateTexturesType(Mono);
		DEBUG_TRACE3(_T("\tMono\n"));
		NSCALL(m_Direct3DDevice.DrawTriPatch( Handle, pNumSegs, pTriPatchInfo ));
	} 
	DUMPTEXTURESANDRT(_T("DTP"), bRenderTo2RT, false);
	AfterDrawCall();
	return hResult;
}

VOID __stdcall CBaseStereoRenderer::SetGammaRamp(UINT iSwapChain, DWORD Flags, CONST D3DGAMMARAMP * pRamp ) 
{
	DUMP_CMD_ALL(SetGammaRamp,iSwapChain,Flags,pRamp);

	DEBUG_TRACE2(_T("SetGammaRamp(iSwapChain = %d, Flags = %08X, pRamp = %p);\n"), iSwapChain, Flags, pRamp );
	memcpy(&m_GlobalGammaRamp , pRamp, sizeof(D3DGAMMARAMP));
	if (gInfo.DisableGammaApplying)
		return;
	if (m_pOutputMethod->HandleGamma())
		GetBaseSC()->SetGammaRamp(pRamp, true);
	else
	{
		m_Direct3DDevice.SetGammaRamp(GetCorrectSwapChainIndex(iSwapChain), Flags, pRamp);
		if (GetBaseSC()->m_pAdditionalSwapChain)
			m_Direct3DDevice.SetGammaRamp(GetBaseSC()->m_iAdditionalSwapChain, Flags, pRamp);
	}
}

BOOL __stdcall CBaseStereoRenderer::SetDeviceGammaRamp(CONST D3DGAMMARAMP *pRamp) 
{
	DEBUG_TRACE2(_T("GDI RAMP changed! SetDeviceGammaRamp();\n"));
	memcpy(&m_GlobalGammaRamp , pRamp, sizeof(D3DGAMMARAMP));
	if (m_pOutputMethod->HandleGamma())
	{
		GetBaseSC()->SetGammaRamp(pRamp, false);
		return TRUE;
	}
	else 
		return FALSE;
}

VOID __stdcall CBaseStereoRenderer::GetGammaRamp(UINT iSwapChain, D3DGAMMARAMP * pRamp ) 
{
	if (m_pOutputMethod->HandleGamma())
		GetBaseSC()->GetGammaRamp(pRamp);
	else
		m_Direct3DDevice.GetGammaRamp( GetCorrectSwapChainIndex(iSwapChain), pRamp );
	DEBUG_TRACE2(_T("GetGammaRamp(iSwapChain = %d, pRamp = %08X);\n"), 
		iSwapChain, pRamp );
}

STDMETHODIMP CBaseStereoRenderer::StateBlock_Apply(IDirect3DStateBlock9* pBlock)
{
#ifndef FINAL_RELEASE
	if (GET_DUMPER().CanDump() && GINFO_DUMP_ON)
	{
		auto it = m_StateBlocks.find(pBlock);
		if (it != m_StateBlocks.end())
			m_deviceState.Apply((*it).second);
		DUMP_CMD(StateBlock_Apply,pBlock);
	}
	INSIDE;
#endif

	HRESULT hResult = (pBlock->*Original_StateBlock_Apply)();
	DEBUG_TRACE2(_T("IDirect3DStateBlock9->Apply();\n")); 

	D3DXMATRIX PM;
	m_Direct3DDevice.GetTransform(D3DTS_PROJECTION, &PM);
	if (PM != m_ProjectionMatrix)
	{	
		m_ProjectionMatrix = PM;
		if (m_Input.StereoActive)
			UpdateProjectionMatrix(true);
	}
	
	m_Direct3DDevice.GetRenderState(D3DRS_COLORWRITEENABLE , &m_dwColorWriteEnable[0]);
	m_Direct3DDevice.GetRenderState(D3DRS_COLORWRITEENABLE1, &m_dwColorWriteEnable[1]);
	m_Direct3DDevice.GetRenderState(D3DRS_COLORWRITEENABLE2, &m_dwColorWriteEnable[2]);
	m_Direct3DDevice.GetRenderState(D3DRS_COLORWRITEENABLE3, &m_dwColorWriteEnable[3]);
	m_Direct3DDevice.GetRenderState(D3DRS_ZWRITEENABLE,      &m_dwZWriteEnable);
	m_Direct3DDevice.GetRenderState(D3DRS_STENCILENABLE,     &m_dwStencilEnable);
	m_Direct3DDevice.GetRenderState(D3DRS_SCISSORTESTENABLE, &m_dwScissorTestEnable);

	m_Direct3DDevice.GetViewport(&m_ViewPort);
	CheckViewportZRange();
	m_Direct3DDevice.GetScissorRect(&m_ScissorRect);

	return hResult;
}

#ifndef FINAL_RELEASE
STDMETHODIMP CBaseStereoRenderer::StateBlock_Capture(IDirect3DStateBlock9* pBlock)
{
	if (GET_DUMPER().CanDump() && GINFO_DUMP_ON)
	{
		CDeviceState& sb = m_StateBlocks[pBlock];
		sb.Capture(m_deviceState);
		if (!m_bRenderScene)
			sb.Dump();
		DUMP_CMD(StateBlock_Capture,pBlock);
	}
	INSIDE;

	HRESULT hResult = (pBlock->*Original_StateBlock_Capture)();
	DEBUG_TRACE2(_T("IDirect3DStateBlock9->Capture();\n")); 

	return hResult;
}
#endif // FINAL_RELEASE

//--- Hooked for PrepareViewStage() optimization ---
STDMETHODIMP CBaseStereoRenderer::SetViewport(CONST D3DVIEWPORT9* pViewport) 
{
	DUMP_CMD_STATE(SetViewport,pViewport);

	HRESULT hResult;
	if (!m_bInStateBlock)
	{
		NSCALL_TRACE2(m_Direct3DDevice.SetViewport(pViewport),
			DEBUG_MESSAGE(_T("SetViewport(pViewport: %s)"), GetViewPortString(pViewport)));
		m_ViewPort = *pViewport;
		if (m_ViewPort.X == 0 && m_ViewPort.Y == 0 &&
			m_ViewPort.Width == m_PrimaryRTSize.cx && 
			m_ViewPort.Height == m_PrimaryRTSize.cy &&
			m_ViewPort.MinZ == 0.0f && m_ViewPort.Y == 1.0f)
			m_bFullSizeViewport = TRUE;
		else
			m_bFullSizeViewport = FALSE;
		CheckViewportZRange();
	}
	else
		hResult = m_Direct3DDevice.SetViewport(pViewport);
	return hResult;
}

STDMETHODIMP CBaseStereoRenderer::SetScissorRect(CONST RECT* pRect)
{
	DUMP_CMD_STATE(SetScissorRect,pRect);

	HRESULT hResult;
	if (!m_bInStateBlock)
	{
		NSCALL_TRACE2(m_Direct3DDevice.SetScissorRect(pRect),
			DEBUG_MESSAGE(_T("SetScissorRect(pRect = %s)"),GetRectString(pRect)));
		m_ScissorRect = *pRect;	
		if (m_ScissorRect.left == 0 && m_ScissorRect.top == 0 &&
			m_ScissorRect.right == m_PrimaryRTSize.cx && 
			m_ScissorRect.bottom == m_PrimaryRTSize.cy)
			m_bFullSizeScissorRect = TRUE;
		else
			m_bFullSizeScissorRect = FALSE;
	}
	else
		hResult = m_Direct3DDevice.SetScissorRect(pRect);
	return hResult;
}

STDMETHODIMP CBaseStereoRenderer::SetVertexDeclaration(IDirect3DVertexDeclaration9* pDecl)
{
	DUMP_CMD_STATE(SetVertexDeclaration,pDecl);

	HRESULT hResult;
	if (!m_bInStateBlock)
	{
		NSCALL_TRACE2(m_Direct3DDevice.SetVertexDeclaration(pDecl),
			DEBUG_MESSAGE(_T("SetVertexDeclaration(pDecl = %p)"), pDecl));
		if (SUCCEEDED(hResult) && pDecl)
		{
			m_bRHWVertexDeclaration = FALSE;
			UINT numElements;
			D3DVERTEXELEMENT9 decl[MAX_FVF_DECL_SIZE];
			if(SUCCEEDED(pDecl->GetDeclaration(decl, &numElements))) 
			{
				numElements = std::min<UINT>(numElements, MAX_FVF_DECL_SIZE);
				for(UINT i = 0; i < numElements; i++)  
				{
					if(decl[i].Usage == D3DDECLUSAGE_POSITIONT) 
					{
						m_dwStreamRHWVertices = decl[i].Stream;
						m_dwOffsetRHWVertices = decl[i].Offset;
						m_bRHWVertexDeclaration = TRUE;
						DEBUG_TRACE3(_T("\tRHW Pipeline\n"));
						break;
					}                          
				}
			}
		}
	}
	else
		hResult = m_Direct3DDevice.SetVertexDeclaration(pDecl);
	return hResult;
}

STDMETHODIMP CBaseStereoRenderer::SetFVF(DWORD FVF) 
{
	DUMP_CMD_STATE(SetFVF,FVF);

	HRESULT hResult;
	if (!m_bInStateBlock)
	{
		NSCALL_TRACE2(m_Direct3DDevice.SetFVF(FVF),
			DEBUG_MESSAGE(_T("SetFVF(FVF = %s)"), GetFVFString(FVF)));
		if (SUCCEEDED(hResult) && FVF)
		{
			if ((FVF & D3DFVF_POSITION_MASK) == D3DFVF_XYZRHW)
			{
				m_dwStreamRHWVertices = 0;
				m_dwOffsetRHWVertices = 0;
				m_bRHWVertexDeclaration = TRUE;
				DEBUG_TRACE3(_T("\tRHW Pipeline\n"));
			}
			else
				m_bRHWVertexDeclaration = FALSE;
		}
	}
	else
		hResult = m_Direct3DDevice.SetFVF(FVF);
	return hResult;
}

STDMETHODIMP CBaseStereoRenderer::SetVertexShader(IDirect3DVertexShader9* pShader) 
{
	DUMP_CMD_STATE(SetVertexShader,pShader);

	HRESULT hResult;

	IDirect3DVertexShader9* pModifiedShader = pShader;
	if (!m_bInStateBlock) {
		pModifiedShader = m_VSPipelineData.SetShader(pShader, m_Input.StereoActive, g_ProfileData.VSCRCData, 0);
	}
	DEBUG_TRACE2(_T("SetVertexShader(Original = 0x%p, Modified = 0x%p [%i])\n"), pShader, pModifiedShader, m_VSPipelineData.GetShaderIndex());
	NSCALL(m_Direct3DDevice.SetVertexShader(pModifiedShader));
	return hResult;
}

STDMETHODIMP CBaseStereoRenderer::SetVertexShaderConstantF(UINT StartRegister, CONST float* pConstantData, UINT Vector4fCount)
{
	ShaderConstantsF constantData(pConstantData,Vector4fCount);
	DUMP_CMD_STATE(SetVertexShaderConstantF,StartRegister,constantData,Vector4fCount);

	HRESULT hResult;
	if (!m_bInStateBlock)
	{
		memcpy(&m_VertexShaderRegister[StartRegister * 4 + 0], pConstantData, 4 * sizeof(float) * Vector4fCount);
		NSCALL_TRACE3(m_Direct3DDevice.SetVertexShaderConstantF(
			StartRegister, pConstantData, Vector4fCount),
			DEBUG_MESSAGE(_T("SetVertexShaderConstantF(StartRegister = %u, pConstantData = %p, Vector4fCount = %u)"), 
			StartRegister, pConstantData, Vector4fCount));
#ifdef ZLOG_ALLOW_TRACING
		if (zlog::GetSeverity() == zlog::SV_FLOOD)
		{
			for(UINT i=0; i< std::min<UINT>(8, Vector4fCount); i++)
			{
				//--- we use 'blank' in format string for sign alignment ---
				DEBUG_MESSAGE(_T("c%i: x = % f\ty = % f\tz = % f\tw = % f\n"), StartRegister+i, pConstantData[i*4+0], pConstantData[i*4+1], pConstantData[i*4+2], pConstantData[i*4+3]);
			}
		}
#endif
	}
	else
		hResult = m_Direct3DDevice.SetVertexShaderConstantF(StartRegister, pConstantData, Vector4fCount);
	return hResult;
}

STDMETHODIMP CBaseStereoRenderer::GetVertexShaderConstantF(UINT StartRegister, float* pConstantData, UINT Vector4fCount)
{
	HRESULT hResult = S_OK;
	memcpy(pConstantData, &m_VertexShaderRegister[StartRegister * 4 + 0], 4 * sizeof(float) * Vector4fCount);
	NSCALL_TRACE3(S_OK, DEBUG_MESSAGE(_T("GetVertexShaderConstantF(StartRegister = %u, pConstantData = %p, Vector4fCount = %u)"), 
		StartRegister, pConstantData, Vector4fCount));

	//NSCALL_TRACE3(m_pDirect3DDevice.GetVertexShaderConstantF(StartRegister, pConstantData, Vector4fCount),
	//	DEBUG_MESSAGE("GetVertexShaderConstantF(StartRegister = %u, pConstantData = %p, Vector4fCount = %u)", 
	//	StartRegister, pConstantData, Vector4fCount));
	return hResult;
}

STDMETHODIMP CBaseStereoRenderer::GetVertexShader(IDirect3DVertexShader9** ppShader)
{
	HRESULT hResult;
	if(ppShader)
	{
		*ppShader = m_VSPipelineData.m_CurrentShader;
		if (*ppShader)
			(*ppShader)->AddRef();
		hResult = S_OK;
	}
	else
		hResult = D3DERR_INVALIDCALL;
	NSCALL_TRACE2(hResult,	DEBUG_MESSAGE(_T("GetVertexShader(*ppShader = %p)"), Indirect(ppShader)));
	return hResult;
}

STDMETHODIMP CBaseStereoRenderer::SetPixelShader(IDirect3DPixelShader9* pShader)
{
	DUMP_CMD_STATE(SetPixelShader,pShader);

	HRESULT hResult;

	IDirect3DPixelShader9* pModifiedShader = pShader;
	if (!m_bInStateBlock) {
		pModifiedShader = m_PSPipelineData.SetShader(pShader, m_Input.StereoActive, g_ProfileData.PSCRCData, 0xFFFFFFFF);
	}
	DEBUG_TRACE2(_T("SetPixelShader(Original = 0x%p, Modified = 0x%p [%i])\n"), pShader, pModifiedShader, m_PSPipelineData.GetShaderIndex());
	NSCALL(m_Direct3DDevice.SetPixelShader(pModifiedShader));
	return hResult;
}

STDMETHODIMP CBaseStereoRenderer::SetStreamSource(UINT StreamNumber, IDirect3DVertexBuffer9* pStreamData, UINT OffsetInBytes, UINT Stride) 
{
	DUMP_CMD_STATE(SetStreamSource,StreamNumber,pStreamData,OffsetInBytes,Stride);
	HRESULT hResult;
	if (!m_bInStateBlock)
	{
		NSCALL_TRACE2(m_Direct3DDevice.SetStreamSource(StreamNumber, pStreamData, OffsetInBytes, Stride),
			DEBUG_MESSAGE(_T("SetStreamSource(StreamNumber = %u, pStreamData = %p, OffsetInBytes = %u, Stride = %u)"), 
			StreamNumber, pStreamData, OffsetInBytes, Stride));
		if(SUCCEEDED(hResult))
		{
			m_pVB[StreamNumber] = pStreamData;
			CRCMeshCheck(StreamNumber, pStreamData);
	}
	}
	else
		hResult = m_Direct3DDevice.SetStreamSource(StreamNumber, pStreamData, OffsetInBytes, Stride);
	return hResult;
}

STDMETHODIMP CBaseStereoRenderer::SetTransform(D3DTRANSFORMSTATETYPE State, CONST D3DMATRIX* pMatrix) 
{
	DUMP_CMD_STATE(SetTransform,State,pMatrix);

	HRESULT hResult;
	if(!m_bInStateBlock)
	{
		NSCALL_TRACE2(m_Direct3DDevice.SetTransform(State, pMatrix),
			DEBUG_MESSAGE(_T("SetTransform(State = %s, pMatrix = %p)"), GetTransformTypeString(State), pMatrix));
		if (SUCCEEDED(hResult))
		{
			switch(State)
			{
			case D3DTS_WORLD:
				m_WorldMatrix = *pMatrix;
				if (gInfo.MonoFixedPipelineCheckingMethodIndex == 2 || GINFO_DEBUG)
				{
					m_bIsWorldMatrixIdenty = S3DMatrixIsIdentity(&m_WorldMatrix);
					DEBUG_TRACE3(_T("Current IsWorldMatrixIdenty: %d\n"), m_bIsWorldMatrixIdenty);
				}
				break;
			case D3DTS_VIEW:
				m_ViewMatrix = *pMatrix;
				if (gInfo.MonoFixedPipelineCheckingMethodIndex == 2 || GINFO_DEBUG)
				{
					m_bIsViewMatrixIdenty = S3DMatrixIsIdentity(&m_ViewMatrix);
					DEBUG_TRACE3(_T("Current IsViewMatrixIdenty: %d\n"), m_bIsViewMatrixIdenty);
				}
				break;
			case D3DTS_PROJECTION:
				m_ProjectionMatrix = *pMatrix;
				if (m_Input.StereoActive)
					UpdateProjectionMatrix(true);
				break;
			}

			TraceMatrix(pMatrix);
		}
	}
	else
		hResult = m_Direct3DDevice.SetTransform(State, pMatrix);
	return hResult;
}

STDMETHODIMP CBaseStereoRenderer::MultiplyTransform(D3DTRANSFORMSTATETYPE State, CONST D3DMATRIX* pMatrix)
{
	DUMP_CMD_ALL(MultiplyTransform,State,pMatrix);

	HRESULT hResult;
	if(!m_bInStateBlock)
	{
		NSCALL_TRACE2(m_Direct3DDevice.MultiplyTransform(State, pMatrix),
			DEBUG_MESSAGE(_T("MultiplyTransform(State = %s, pMatrix = %p)"), GetTransformTypeString(State), pMatrix));
		if (SUCCEEDED(hResult))
		{
			switch(State)
			{
			case D3DTS_WORLD:
				D3DXMatrixMultiply(&m_WorldMatrix, (D3DXMATRIX*)pMatrix, &m_WorldMatrix);
				if (gInfo.MonoFixedPipelineCheckingMethodIndex == 2 || GINFO_DEBUG)
				{
					m_bIsWorldMatrixIdenty = D3DXMatrixIsIdentity(&m_WorldMatrix) != 0;
					DEBUG_TRACE3(_T("Current IsWorldMatrixIdenty: %d\n"), m_bIsWorldMatrixIdenty);
				}
				break;
			case D3DTS_VIEW:
				D3DXMatrixMultiply(&m_ViewMatrix, (D3DXMATRIX*)pMatrix, &m_ViewMatrix);
				if (gInfo.MonoFixedPipelineCheckingMethodIndex == 2 || GINFO_DEBUG)
				{
					m_bIsViewMatrixIdenty = D3DXMatrixIsIdentity(&m_ViewMatrix) != 0;
					DEBUG_TRACE3(_T("Current IsViewMatrixIdenty: %d\n"), m_bIsViewMatrixIdenty);
				}
				break;
			case D3DTS_PROJECTION:
				D3DXMatrixMultiply(&m_ProjectionMatrix, (D3DXMATRIX*)pMatrix, &m_ProjectionMatrix);
				if (m_Input.StereoActive)
					UpdateProjectionMatrix(true);
				break;
			}
			
			TraceMatrix(pMatrix);
		}
	}
	else
		hResult = m_Direct3DDevice.MultiplyTransform(State, pMatrix);
	return hResult;
}

STDMETHODIMP CBaseStereoRenderer::GetBackBuffer(UINT iSwapChain, UINT iBackBuffer, D3DBACKBUFFER_TYPE Type, IDirect3DSurface9** ppBackBuffer) 
{
	HRESULT hResult = S_OK;
#ifdef WRAP_ONLY_PRIMARY_SWAPCHAIN
	if (iSwapChain == 0 && iBackBuffer == 0 && m_bEngineStarted)
		GetMonoSC()->GetBackBuffer(iBackBuffer, Type, ppBackBuffer);
#else
	if (m_bEngineStarted)
	{
		if (iSwapChain < m_SwapChains.size())
			hResult = GetMonoSC(iSwapChain)->GetBackBuffer(iBackBuffer, Type, ppBackBuffer);
		else
			hResult = D3DERR_INVALIDCALL;
	}
#endif
	else
		NSCALL_TRACE2(m_Direct3DDevice.GetBackBuffer(GetCorrectSwapChainIndex(iSwapChain), iBackBuffer, Type, ppBackBuffer),
		DEBUG_MESSAGE(_T("GetBackBuffer(iSwapChain = %d, iBackBuffer = %d, Type = %d, *ppBackBuffer = %p);\n"), 
		iSwapChain, iBackBuffer, Type, Indirect(ppBackBuffer)));
	return hResult;
}

STDMETHODIMP_(UINT) CBaseStereoRenderer::GetNumberOfSwapChains() 
{
	UINT nNumberOfSwapChains = __super::GetNumberOfSwapChains();
	if (m_bTwoWindows)
		nNumberOfSwapChains = std::max<UINT>(nNumberOfSwapChains - 1, 1);
	DEBUG_TRACE2(_T("GetNumberOfSwapChains() = %u\n"), nNumberOfSwapChains);
	return nNumberOfSwapChains;
}

STDMETHODIMP CBaseStereoRenderer::CreateVertexBuffer(UINT Length, DWORD Usage, DWORD FVF, D3DPOOL Pool, 
													 IDirect3DVertexBuffer9** ppVertexBuffer, HANDLE* pSharedHandle) 
{
	HRESULT hResult;
	NSCALL_TRACE2(m_Direct3DDevice.CreateVertexBuffer(Length, Usage, FVF, Pool, 
		ppVertexBuffer, pSharedHandle),
		DEBUG_MESSAGE(_T("CreateVertexBuffer(Length = %u, Usage = %s, FVF = %s, Pool = %s, ")
		_T("*ppVertexBuffer = %p, pSharedHandle = %p)"),
		Length, GetUsageString(Usage), GetFVFString(FVF), GetPoolString(Pool), 
		Indirect(ppVertexBuffer), pSharedHandle));

	if(Succeded(hResult) && *ppVertexBuffer)										
	{
		IDirect3DVertexBuffer9 *pVertexBuffer = *ppVertexBuffer;
		DUMP_CMD_ALL(CreateVertexBuffer,Length,Usage,FVF,Pool,pVertexBuffer,pSharedHandle)

		void** p = *(void***)(*ppVertexBuffer);	
		HOOK_GET_DEVICE(ppVertexBuffer, Proxy_VertexBuffer_GetDevice, Original_VertexBuffer_GetDevice);

		if (gInfo.RHWModificationMethod == 1)
		{
			if (!IsAlreadyHooked(p[11], (PVOID*)&Original_VertexBuffer_Lock))
			{
				// CVertexBuffer
				UniqueHookCode(this, p[11], Proxy_VertexBuffer_Lock,   (PVOID*)&Original_VertexBuffer_Lock);
				UniqueHookCode(this, p[12], Proxy_VertexBuffer_Unlock, (PVOID*)&Original_VertexBuffer_Unlock);
			}
			else
			{
				// CDriverVertexBuffer
				UniqueHookCode(this, p[11], Proxy_DriverVertexBuffer_Lock,   (PVOID*)&Original_DriverVertexBuffer_Lock);
				UniqueHookCode(this, p[12], Proxy_DriverVertexBuffer_Unlock, (PVOID*)&Original_DriverVertexBuffer_Unlock);
			}
		}
	}
	return hResult;
}

STDMETHODIMP CBaseStereoRenderer::CreateVolumeTexture(UINT Width, UINT Height, UINT Depth, UINT Levels, DWORD Usage, 
												D3DFORMAT Format, D3DPOOL Pool, 
												IDirect3DVolumeTexture9** ppVolumeTexture, HANDLE* pSharedHandle) 
{
	HRESULT hResult;
	NSCALL_TRACE1(m_Direct3DDevice.CreateVolumeTexture(Width, Height, Depth, Levels, 
		Usage, Format, Pool, ppVolumeTexture, pSharedHandle),
		DEBUG_MESSAGE(_T("CreateVolumeTexture(Width = %u, Height = %u, Depth = %u, ")
		_T("Levels = %u, Usage = %s, Format = %s, Pool = %s, *ppVolumeTexture = %p, pSharedHandle = %p)"), 
		Width, Height, Depth, Levels, GetUsageString(Usage), GetFormatString(Format), 
		GetPoolString(Pool), Indirect(ppVolumeTexture), pSharedHandle));

	if(Succeded(hResult))
	{
		IDirect3DVolumeTexture9 *pVolumeTexture = *ppVolumeTexture;
		{
			DUMP_CMD_ALL(CreateVolumeTexture,Width,Height,Depth,Levels,Usage,Format,Pool,pVolumeTexture,pSharedHandle);
		}
		OnNewTextureCreated(pVolumeTexture,Levels);

#ifndef FINAL_RELEASE
		TCHAR buffer[128];
		if (Pool != D3DPOOL_DEFAULT)
		{
			_sntprintf_s(buffer, _TRUNCATE, _T("VolumeTexture%d"), m_nVolumeTextureCounter);
			m_nVolumeTexturesSize += GetVolumeTextureSize(*ppVolumeTexture);
		}
		else
		{
			_sntprintf_s(buffer, _TRUNCATE, _T("VolumeTextureDef%d"), m_nVolumeTextureDefaultCounter);
			m_nVolumeTexturesDefaultSize += GetVolumeTextureSize(*ppVolumeTexture);
		}
		SetObjectName(*ppVolumeTexture, buffer);
		DEBUG_TRACE1(_T("\tSetObjectName(*ppTexture = %p [%s])\n"), *ppVolumeTexture, GetObjectName(*ppVolumeTexture));
#endif
		if (Pool != D3DPOOL_DEFAULT)
			m_nVolumeTextureCounter++;
		else
			m_nVolumeTextureDefaultCounter++;
		HOOK_GET_DEVICE(ppVolumeTexture, Proxy_VolumeTexture_GetDevice, Original_VolumeTexture_GetDevice);

		if (m_Direct3DDevice.getExMode() == EXMODE_EMULATE)
		{
			CComPtr<IDirect3DVolume9> pFirstVolume;
			(*ppVolumeTexture)->GetVolumeLevel(0, &pFirstVolume);
			void** p = *(void***)(pFirstVolume.p);
			UniqueHookCode(this, p[9], Proxy_Volume_LockBox, (PVOID*)&Original_Volume_LockBox);
			//UniqueHookCode(this, p[10], Proxy_Volume_UnlockBox, (PVOID*)&Original_Volume_UnlockBox);
		}
	}
	return hResult;
}

STDMETHODIMP CBaseStereoRenderer::GetPixelShader(IDirect3DPixelShader9** ppShader)
{
	HRESULT hResult;
	if(ppShader)
	{
		*ppShader = m_PSPipelineData.m_CurrentShader;
		if (*ppShader)
			(*ppShader)->AddRef();
		hResult = S_OK;
	}
	else
		hResult = D3DERR_INVALIDCALL;
	NSCALL_TRACE2(hResult,	DEBUG_MESSAGE(_T("GetPixelShader(*ppShader = %p)"), Indirect(ppShader)));
	return hResult;
}

STDMETHODIMP CBaseStereoRenderer::TestCooperativeLevel(void) 
{
	HRESULT hResult;
	if (USE_MULTI_THREAD_PRESENTER) 
	{
		m_pPresenterThread->SetAction( ptTestCooperativeLevel );
		NSCALL_TRACE3(m_pPresenterThread->WaitForActionDone(),
			DEBUG_MESSAGE(_T("m_pPresenterThread->WaitForActionDone()")));
	}
	else 
	{
		NSCALL_TRACE2(m_Direct3DDevice.TestCooperativeLevel(), 
			DEBUG_MESSAGE(_T("TestCooperativeLevel()")));
	}
	return hResult;
}

STDMETHODIMP CBaseStereoRenderer::CreateQuery(D3DQUERYTYPE Type, IDirect3DQuery9** ppQuery)
{
	HRESULT hResult;
	NSCALL_TRACE2(m_Direct3DDevice.CreateQuery(Type, ppQuery),
		DEBUG_MESSAGE(_T("CreateQuery(Type = %s, *ppQuery = %p)"), GetQueryTypeString(Type), Indirect(ppQuery)));
	
	if(ppQuery == NULL || FAILED(hResult))
		return hResult;

	HOOK_GET_DEVICE(ppQuery, Proxy_Query_GetDevice, Original_Query_GetDevice);
	
#ifndef FINAL_RELEASE
	//--- also disabled for routerType = ROUTER_TYPE_HOOK
	if(*ppQuery)
		*ppQuery = new CProxyQuery(*ppQuery, Type);
#endif

	return hResult;
}

#pragma optimize( "y", off ) // for Steam Community

STDMETHODIMP CBaseStereoRenderer::CreateAdditionalSwapChain(D3DPRESENT_PARAMETERS* pPresentationParameters, IDirect3DSwapChain9** ppSwapChain) 
{
	HRESULT hResult;
#ifdef WRAP_ONLY_PRIMARY_SWAPCHAIN
	NSCALL_TRACE1(m_Direct3DDevice.CreateAdditionalSwapChain(pPresentationParameters, ppSwapChain),
		DEBUG_MESSAGE(_T("CreateAdditionalSwapChain(pPresentationParameters: %s, *ppSwapChain = %p)"), 
		GetPresentationParametersString(pPresentationParameters), Indirect(ppSwapChain)));
	if (SUCCEEDED(hResult))
	{
		m_bUseMultipleSwapChains = TRUE;
		DUMP_CMD_ALL(CreateAdditionalSwapChain,pPresentationParameters,ppSwapChain);
	}
#else
	CMonoSwapChain* pSwapChain = CreateSwapChainWrapper(m_TotalSwapChainsCount);
	m_TotalSwapChainsCount++;
	pSwapChain->AddRef();
	pSwapChain->setExMode(m_Direct3DDevice.getExMode());
	pSwapChain->SetPresentationParameters(pPresentationParameters, NULL);

	if (m_DeviceMode == DEVICE_MODE_AUTO && GetBaseDevice()->m_bTwoWindows)
	{
		D3DPRESENT_PARAMETERS& pp = *pPresentationParameters;
		if (((pp.BackBufferWidth > 1 || pp.BackBufferHeight > 1) ||
			(pp.Windowed && pp.BackBufferWidth == 0 && pp.BackBufferHeight == 0)))
		{
			DEBUG_MESSAGE(_T("%s\n"), GetDeviceModeString(m_DeviceMode));
			GetBaseDevice()->m_DeviceMode = DEVICE_MODE_FORCEWINDOWED;
			DEBUG_MESSAGE(_T("%s\n"), GetDeviceModeString(GetBaseDevice()->m_DeviceMode));
		}
	}

	CBaseSwapChain* pBaseSwapChain = (CBaseSwapChain*)pSwapChain;
	NSCALL(pBaseSwapChain->InitWindows());
	NSCALL(pBaseSwapChain->ModifyPresentParameters());
	NSCALL_TRACE1(m_Direct3DDevice.CreateAdditionalSwapChain(&pBaseSwapChain->m_PresentationParameters[0], ppSwapChain),
		DEBUG_MESSAGE(_T("CreateAdditionalSwapChain(pPresentationParameters: %s, *ppSwapChain = %p)"), 
		GetPresentationParametersString(&pBaseSwapChain->m_PresentationParameters[0]), ppSwapChain, Indirect(ppSwapChain)));
	if (SUCCEEDED(hResult))
	{
		DUMP_CMD_ALL(CreateAdditionalSwapChain,pPresentationParameters,ppSwapChain);
		DEBUG_MESSAGE(_T("Added %d SwapChain\n"), m_SwapChains.size());
		m_SwapChains.push_back(pSwapChain);

		RouterTypeHookCallGuard<ProxyDirect9> RTdir(GetProxyDirect3D9());
		RouterTypeHookCallGuard<ProxyDevice9> RTdev(&m_Direct3DDevice);
		NSCALL(pSwapChain->Initialize(*ppSwapChain));
		pBaseSwapChain->PostInitialize();
#ifndef FINAL_RELEASE
		unsigned int i = 0;
		unsigned int count = m_Direct3DDevice.GetNumberOfSwapChains();
		for (; i < m_Direct3DDevice.GetNumberOfSwapChains(); i++)
		{
			CComPtr<IDirect3DSwapChain9> pTempSwapChain;
			if (FAILED(m_Direct3DDevice.GetSwapChain(i, &pTempSwapChain)))
				continue;
			if (pTempSwapChain == *ppSwapChain)
			{
				DEBUG_MESSAGE(_T("Real SwapChain index: %d\n"), i);
				break;
			}
		}
		if (i == count)
		{
			DEBUG_MESSAGE(_T("Real SwapChain not founded\n"));
		}
#endif
		pSwapChain->AddRef();
		if(gInfo.RouterType == ROUTER_TYPE_HOOK)
		{
			*ppSwapChain = pSwapChain->m_SwapChain.GetReal();
		}
		else
			*ppSwapChain = pSwapChain;
	}
	pSwapChain->Release();
#endif
	return hResult;
}
#pragma optimize( "y", on )

STDMETHODIMP CBaseStereoRenderer::SetCursorProperties(UINT XHotSpot, UINT YHotSpot, IDirect3DSurface9* pCursorBitmap) 
{
	HRESULT hResult;
	NSCALL_TRACE2(m_Direct3DDevice.SetCursorProperties(XHotSpot, YHotSpot, pCursorBitmap), 
		DEBUG_MESSAGE(_T("SetCursorProperties(XHotSpot = %u, YHotSpot = %u, pCursorBitmap = %p)"), 
		XHotSpot, YHotSpot, pCursorBitmap));
	if (SUCCEEDED(hResult) && (m_pOutputMethod->GetCaps() & ocHardwareMouseCursorNotSupported))
	{
		m_MouseXHotSpot = XHotSpot;
		m_MouseYHotSpot = YHotSpot;
		m_pMouseCursor.Release();
		m_pMouseCursor = pCursorBitmap;
		m_pMouseCursorTexture.Release();
		if (m_pMouseCursor)
		{
			HRESULT hr = m_pMouseCursor->GetContainer(IID_IDirect3DTexture9, (void**)&m_pMouseCursorTexture);
			if (!m_pMouseCursorTexture)
			{
				INSIDE;
				D3DSURFACE_DESC desc;
				m_pMouseCursor->GetDesc(&desc);
				hr = m_Direct3DDevice.CreateTexture(desc.Width, desc.Height, 1, 0, desc.Format, desc.Pool,
					&m_pMouseCursorTexture, NULL);
				if (m_pMouseCursorTexture)
				{
					CComPtr<IDirect3DSurface9> pSurf;
					m_pMouseCursorTexture->GetSurfaceLevel(0, &pSurf);
					hr = D3DXLoadSurfaceFromSurface(pSurf, NULL, NULL, 
						m_pMouseCursor, NULL, NULL, D3DX_DEFAULT, 0);
				}
			}
		}
	}
	return hResult;
}

STDMETHODIMP_(VOID) CBaseStereoRenderer::SetCursorPosition(int X, int Y, DWORD Flags) 
{
	m_Direct3DDevice.SetCursorPosition(X, Y, Flags);
	DEBUG_TRACE2(_T("SetCursorPosition(X = %d, Y = %d, Flags = %08x)\n"), X, Y, Flags);
}

STDMETHODIMP_(BOOL) CBaseStereoRenderer::ShowCursor(BOOL bShow) 
{
	BOOL bShowCursor;
	if (m_pOutputMethod->GetCaps() & ocHardwareMouseCursorNotSupported)
	{
		m_bShowMouse = bShow;
		bShow = FALSE;
	}
	bShowCursor = m_Direct3DDevice.ShowCursor(bShow);
	DEBUG_TRACE2(_T("ShowCursor(bShow = %1d) = %d\n"), bShow, bShowCursor);
	return bShowCursor;
}

STDMETHODIMP CBaseStereoRenderer::GetDisplayMode(UINT iSwapChain, D3DDISPLAYMODE* pMode)
{
	if ( iSwapChain >= m_SwapChains.size() ) 
	{
		DEBUG_TRACE3( _T("GetDisplayMode(iSwapChain = %u, pMode = %p)[Error]: Invalid swap chain index"), iSwapChain, pMode );
		return D3DERR_INVALIDCALL;
	}

	return m_SwapChains[iSwapChain]->GetDisplayMode(pMode);
}
	
STDMETHODIMP CBaseStereoRenderer::GetDisplayModeEx(UINT iSwapChain, D3DDISPLAYMODEEX* pMode, D3DDISPLAYROTATION* pRotation)
{
	if ( iSwapChain >= m_SwapChains.size() ) 
	{
		DEBUG_TRACE3( _T("GetDisplayMode(iSwapChain = %u, pMode = %p, pRotation = %p)[Error]: Invalid swap chain index"), iSwapChain, pMode, pRotation );
		return D3DERR_INVALIDCALL;
	}

	return m_SwapChains[iSwapChain]->GetDisplayModeEx(pMode, pRotation);
}
