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
#include "MonoRenderer.h"
#include "MonoRenderer_Hook.h"
#include "MonoSwapChain.h"
#include "BaseSwapChain.h"              // 0.5.0 task #77 round 9: reach pre/post-scale RT pointers for the GetRenderTarget lie
#include "Direct3D9.h"
#include "Trace.h"
#include "Globals.h"                     // 0.5.0 task #77 round 9: D9Log (DEBUG_MESSAGE stripped in Final Release)
#include "..\S3DAPI\ScalingAgent.h"
#include "CommandDumper.h"

CMonoRenderer::CMonoRenderer()
:	m_refCount(0)
,	m_bRenderScene(false)
,	m_bInStateBlock(false)
,	m_bOutOfVideoMemory(false)
,	m_bUseMultipleSwapChains(false)
,	m_TotalSwapChainsCount(0)
,	m_pDirect3DWrapper(NULL)
{
	//--- initialize CBaseStereoRenderer members to zero ---
	m_OriginalAdapter = 0;
	m_OriginalBehaviorFlags = 0;
	m_DeviceType = D3DDEVTYPE_HAL;
	m_hFocusWindow = 0;

	m_SwapChains.resize(1);
	m_SwapChains[0] = NULL;

	m_DumpType = dtNone;
}

CMonoRenderer::~CMonoRenderer() 
{
	if (!m_SwapChains.empty())
	{
		//_ASSERT(m_SwapChains.size() == 1);
		CMonoSwapChain* pSwapChain = GetMonoSC();
		pSwapChain->Clear();
		delete pSwapChain;
	}

	//-- clear one-time created hook ---
	UNHOOK_GET_DEVICE(Original_CubeTexture_GetDevice);		
	UNHOOK_GET_DEVICE(Original_IndexBuffer_GetDevice);		
	UNHOOK_GET_DEVICE(Original_Query_GetDevice);			
	UNHOOK_GET_DEVICE(Original_StateBlock_GetDevice);		
	UNHOOK_GET_DEVICE(Original_VertexShader_GetDevice);		
	UNHOOK_GET_DEVICE(Original_VolumeTexture_GetDevice);	
	UNHOOK_GET_DEVICE(Original_PixelShader_GetDevice);		
	UNHOOK_GET_DEVICE(Original_VertexBuffer_GetDevice);		
	UNHOOK_GET_DEVICE(Original_VertexDeclaration_GetDevice);

	DEBUG_MESSAGE(_T("Count of hooked functions after IDirect3DDevice9 wrapper realising - %d\n"), g_UniqueHooks.size());

	//--- remove renderer from list ---
	g_pRendererList.RemoveWrapper(this);

	{
		RouterTypeHookCallGuard<ProxyDirect9> RTdir(GetProxyDirect3D9());
		m_pDirect3D.Release();
	}

#ifndef FINAL_RELEASE
	if (m_Direct3DDevice)
	{
		ULONG nRefCount = 0;
		m_Direct3DDevice.AddRef();
		nRefCount = m_Direct3DDevice.Release() - 1;
		DEBUG_MESSAGE(_T("%s: nRefCount = %d\n"), _T( __FUNCTION__ ) , nRefCount);
	}
#endif

	if (m_pDirect3DWrapper)
		m_pDirect3DWrapper->Release();

	DEBUG_MESSAGE(_T("Mono device released\n"));
	FLUSHLOGFILE();
}


HRESULT CMonoRenderer::InitializeEx(
	CDirect3D9* pDirect3D9Wrapper, IDirect3D9Ex* pDirect3D9, UINT nAdapter, D3DDEVTYPE DeviceType, HWND hFocusWindow, 
	DWORD dwBehaviorFlags, D3DPRESENT_PARAMETERS* pPresentationParameters, D3DDISPLAYMODEEX* pFullscreenDisplayMode) 
{
	return InitializeDevice(pDirect3D9Wrapper, pDirect3D9, nAdapter, DeviceType, hFocusWindow, dwBehaviorFlags,
		pPresentationParameters, pFullscreenDisplayMode, EXMODE_EX);
}

HRESULT CMonoRenderer::Initialize(
	CDirect3D9* pDirect3D9Wrapper, IDirect3D9* pDirect3D9, UINT nAdapter, D3DDEVTYPE DeviceType, HWND hFocusWindow, 
	DWORD dwBehaviorFlags, D3DPRESENT_PARAMETERS* pPresentationParameters) 
{
	return InitializeDevice(pDirect3D9Wrapper, pDirect3D9, nAdapter, DeviceType, hFocusWindow, dwBehaviorFlags,
		pPresentationParameters, NULL, pDirect3D9Wrapper->m_Direct3D9.getExMode());
}

HRESULT CMonoRenderer::Initialize(CDirect3D9* pDirect3D9Wrapper, IDirect3D9* pDirect3D9, IDirect3DDevice9* pDirect3DDevice9)
{
	D3DDEVICE_CREATION_PARAMETERS creationParameters;
	D3DDISPLAYMODE mode;
	pDirect3DDevice9->GetCreationParameters(&creationParameters);
	pDirect3DDevice9->GetDisplayMode(0, &mode);

	CComPtr<IDirect3DSwapChain9> pSwapChain;
	D3DPRESENT_PARAMETERS presentParameters;
	pDirect3DDevice9->GetSwapChain(0, &pSwapChain);
	pSwapChain->GetPresentParameters(&presentParameters);

	D3DPRESENT_PARAMETERS	D3Dpp;
	//--- create D3D device ---
	ZeroMemory( &D3Dpp, sizeof(D3Dpp) );
	D3Dpp.BackBufferWidth  = 1;
	D3Dpp.BackBufferHeight = 1;
	D3Dpp.BackBufferFormat = D3DFMT_UNKNOWN;
	D3Dpp.BackBufferCount  = 1;
	D3Dpp.MultiSampleType  = D3DMULTISAMPLE_NONE;
	D3Dpp.MultiSampleQuality = 0;
	D3Dpp.SwapEffect = D3DSWAPEFFECT_DISCARD;
	D3Dpp.Windowed   = TRUE;
	D3Dpp.PresentationInterval = D3DPRESENT_INTERVAL_DEFAULT;
	pDirect3DDevice9->Reset(&D3Dpp);

	return Initialize(pDirect3D9Wrapper, pDirect3D9, creationParameters.AdapterOrdinal, creationParameters.DeviceType, creationParameters.hFocusWindow, 
		creationParameters.BehaviorFlags, &presentParameters);
}

HRESULT CMonoRenderer::InitializeDevice(
	CDirect3D9* pDirect3D9Wrapper, IDirect3D9* pDirect3D9, UINT nAdapter, D3DDEVTYPE DeviceType, HWND hFocusWindow, DWORD dwBehaviorFlags, 
	D3DPRESENT_PARAMETERS* pPresentationParameters, D3DDISPLAYMODEEX* pFullscreenDisplayMode, PROXY_EXMODE exMode)
{
	DEBUG_MESSAGE(_T("Entering %s\n"), _T( __FUNCTION__ ));
	SetThreadName((DWORD)-1, "D3D9 Main Thread");	

	NSPTR(pDirect3D9);

	if (GINFO_DEBUG)
	{
		DEBUG_MESSAGE(_T("Application: %s\n"), gInfo.ApplicationName);
		DEBUG_MESSAGE(_T("Exe: %s\n"), gInfo.ApplicationFileName);
		DEBUG_MESSAGE(_T("CmdLine: %s\n"), gInfo.CmdLine);
		DEBUG_MESSAGE(_T("Application nAdapter = %d\n"), nAdapter);
		DEBUG_MESSAGE(_T("Application DeviceType = %s\n"), GetDeviceTypeString(DeviceType));
		DEBUG_MESSAGE(_T("Application hFocusWindow = %s\n"), GetWindowRectString(hFocusWindow));
		DEBUG_MESSAGE(_T("Application hDeviceWindow = %s\n"), GetWindowRectString(pPresentationParameters->hDeviceWindow));
		DEBUG_MESSAGE(_T("Application BehaviorFlags = %s\n"), GetBehaviorFlagsString(dwBehaviorFlags));
		DEBUG_MESSAGE(_T("Application pPresentationParameters: %s\n"), GetPresentationParametersString(pPresentationParameters));
	}	

	//--- save input parameters ---
	{
		RouterTypeHookCallGuard<ProxyDirect9> RT(&pDirect3D9Wrapper->m_Direct3D9);
		m_pDirect3DWrapper = pDirect3D9Wrapper;
		m_pDirect3DWrapper->AddRef();
		m_pDirect3D = pDirect3D9;
	}

	m_OriginalAdapter = nAdapter;
	m_DeviceType = DeviceType;
	m_Direct3DDevice.setExMode(exMode);
	
	ScalingAgentPtrT scalingAgent = ScalingAgent::Instance();
	scalingAgent->RemoveHWND(m_hFocusWindow);
	m_hFocusWindow = hFocusWindow;
	scalingAgent->AddHWND(m_hFocusWindow);

	m_OriginalBehaviorFlags = dwBehaviorFlags;

	CMonoSwapChain* pSC = CreateSwapChainWrapper(m_TotalSwapChainsCount);
	m_TotalSwapChainsCount++;
	pSC->AddRef();
	m_SwapChains[0] = pSC;
	GetMonoSC()->setExMode(exMode);
	GetMonoSC()->SetPresentationParameters(pPresentationParameters, pFullscreenDisplayMode);

	return DoInitialize();
}

CMonoSwapChain* CMonoRenderer::CreateSwapChainWrapper(UINT index)
{
	return DNew CMonoSwapChain(this, index);
}

HRESULT CMonoRenderer::DoInitialize()
{
	HRESULT hResult = S_OK;

	CComQIPtr<IDirect3DDevice9> pDirect3DDevice;
	if (m_Direct3DDevice.getExMode() == EXMODE_NONE)
	{
		NSCALL_TRACE1(m_pDirect3D->CreateDevice(m_OriginalAdapter, m_DeviceType, m_hFocusWindow,
			m_OriginalBehaviorFlags, &GetMonoSC()->m_OriginalPresentationParameters, &pDirect3DDevice), 
			DEBUG_MESSAGE(_T("Trying to CreateDevice(m_nAdapter = %u, m_DeviceType = %s, m_hFocusWindow = %p, ")
			_T("m_BehaviorFlags = %s, m_PresentationParameters = %s, m_pDirect3DDevice = %p)"),
			m_OriginalAdapter, GetDeviceTypeString(m_DeviceType), m_hFocusWindow,
			GetBehaviorFlagsString(m_OriginalBehaviorFlags), 
			GetPresentationParametersString(&GetMonoSC()->m_OriginalPresentationParameters), pDirect3DDevice.p));
	}
	else
	{
		CComQIPtr<IDirect3D9Ex> pD3DEx(m_pDirect3D);
		if (pD3DEx)
		{
			CComPtr<IDirect3DDevice9Ex> pDevEx;
			NSCALL_TRACE1(pD3DEx->CreateDeviceEx(m_OriginalAdapter, m_DeviceType, m_hFocusWindow,
				m_OriginalBehaviorFlags, &GetMonoSC()->m_OriginalPresentationParameters, GetMonoSC()->m_pFullscreenDisplayMode, &pDevEx), 
				DEBUG_MESSAGE(_T("Trying to CreateDeviceEx(m_nAdapter = %u, m_DeviceType = %s, m_hFocusWindow = %p, ")
				_T("m_BehaviorFlags = %s, m_PresentationParameters = %s, m_pFullscreenDisplayMode = %s, m_pDirect3DDevice = %p)"),
				m_OriginalAdapter, GetDeviceTypeString(m_DeviceType), m_hFocusWindow,
				GetBehaviorFlagsString(m_OriginalBehaviorFlags), 
				GetPresentationParametersString(&GetMonoSC()->m_OriginalPresentationParameters), 
				GetDisplayModeExString(GetMonoSC()->m_pFullscreenDisplayMode), pDevEx.p));
			pDirect3DDevice = pDevEx;
		}
	}

	if(!pDirect3DDevice)
	{
		DEBUG_MESSAGE(_T("%s: CreateDevice failed.\n"), _T( __FUNCTION__ ) );
		return hResult;
	}

	//--- add renderer to list ---
	g_pRendererList.AddWrapper(pDirect3DDevice, this);
	m_Direct3DDevice.initialize(pDirect3DDevice);
	m_Direct3DDevice.refreshMutablePointers();

	NSCALL(GetMonoSC()->InitializeEx(0));

	if(SUCCEEDED(hResult)) {
		DEBUG_MESSAGE(_T("%s: CreateDevice OK.\n"), _T( __FUNCTION__ ) );
	} else {
		DEBUG_MESSAGE(_T("%s: CreateDevice FAILED absolutely. Error code = 0x%X\n"), _T( __FUNCTION__ ) , hResult);
	}
	
	return hResult;
}

bool CMonoRenderer::Succeded(HRESULT hr)
{
	//#ifdef FINAL_RELEASE
	//		return SUCCEEDED(hr);
	//#else
	if (hr == D3DERR_OUTOFVIDEOMEMORY && !m_bOutOfVideoMemory)
	{
		m_bOutOfVideoMemory = true;
		WriteStatistic();
	}
	return SUCCEEDED(hr);
	//#endif
}

STDMETHODIMP CMonoRenderer::BeginScene(VOID) 
{
	DUMP_CMD(BeginScene);

	HRESULT hResult;
	NSCALL_TRACE2(m_Direct3DDevice.BeginScene(), DEBUG_MESSAGE(_T("BeginScene()")));
	if (SUCCEEDED(hResult))
		m_bRenderScene = true;
	return hResult;
}

STDMETHODIMP CMonoRenderer::EndScene( VOID ) 
{
	DUMP_CMD(EndScene);

	HRESULT hResult = S_OK;
	NSCALL_TRACE2(m_Direct3DDevice.EndScene(), DEBUG_MESSAGE(_T("EndScene()")));
	if (SUCCEEDED(hResult))
		m_bRenderScene = false;
	return hResult;
}

STDMETHODIMP CMonoRenderer::BeginStateBlock(void)
{

	HRESULT hResult;
	// Skip errors
	hResult = m_Direct3DDevice.BeginStateBlock();
	if(SUCCEEDED(hResult))
	{
#ifndef FINAL_RELEASE
		if (GET_DUMPER().CanDump() && GINFO_DUMP_ON && !m_bInStateBlock) 
		{
			m_tempDeviceState.Clear();
			DUMP_CMD_ALL(BeginStateBlock);
		}
#endif
		m_bInStateBlock = true;
	}
	DEBUG_TRACE2(_T("BeginStateBlock() = %s\n"), DXGetErrorString(hResult));
	return hResult;
}

STDMETHODIMP CMonoRenderer::EndStateBlock(IDirect3DStateBlock9** ppSB)
{
	HRESULT hResult;
	NSCALL_TRACE2(m_Direct3DDevice.EndStateBlock(ppSB),
		DEBUG_MESSAGE(_T("EndStateBlock(*ppSB = %p)"), Indirect(ppSB)));

	IDirect3DStateBlock9 *pSB = *ppSB;
	if(SUCCEEDED(hResult))
	{
#ifndef FINAL_RELEASE
		if (GET_DUMPER().CanDump() && GINFO_DUMP_ON && m_bInStateBlock) 
		{ 
			m_StateBlocks[pSB] = std::move(m_tempDeviceState);
			DUMP_CMD_ALL(EndStateBlock, pSB);
		} 
#endif
		m_bInStateBlock = false;
	}

	return hResult;
}

STDMETHODIMP CMonoRenderer::CreateVolumeTexture(UINT Width, UINT Height, UINT Depth, UINT Levels, DWORD Usage, 
													  D3DFORMAT Format, D3DPOOL Pool, 
													  IDirect3DVolumeTexture9** ppVolumeTexture, HANDLE* pSharedHandle) 
{
	HRESULT hResult;
	NSCALL_TRACE2(m_Direct3DDevice.CreateVolumeTexture(Width, Height, Depth, Levels, 
		Usage, Format, Pool, ppVolumeTexture, pSharedHandle),
		DEBUG_MESSAGE(_T("CreateVolumeTexture(Width = %u, Height = %u, Depth = %u, ")
		_T("Levels = %u, Usage = %s, Format = %s, Pool = %s, *ppVolumeTexture = %p, pSharedHandle = %p)"), 
		Width, Height, Depth, Levels, GetUsageString(Usage), GetFormatString(Format), 
		GetPoolString(Pool), Indirect(ppVolumeTexture), pSharedHandle));

	if (Succeded(hResult))
	{
		IDirect3DVolumeTexture9 *pVolumeTexture = *ppVolumeTexture;
		{
			DUMP_CMD_ALL(CreateVolumeTexture,Width,Height,Depth,Levels,Usage,Format,Pool,pVolumeTexture,pSharedHandle);
		}
		OnNewTextureCreated(pVolumeTexture,Levels);

		HOOK_GET_DEVICE(ppVolumeTexture, Proxy_VolumeTexture_GetDevice, Original_VolumeTexture_GetDevice);
	}
	return hResult;
}

STDMETHODIMP CMonoRenderer::CreateVertexBuffer(UINT Length, DWORD Usage, DWORD FVF, D3DPOOL Pool, 
													 IDirect3DVertexBuffer9** ppVertexBuffer, HANDLE* pSharedHandle) 
{
	HRESULT hResult;
	NSCALL_TRACE2(m_Direct3DDevice.CreateVertexBuffer(Length, Usage, FVF, Pool, 
		ppVertexBuffer, pSharedHandle),
		DEBUG_MESSAGE(_T("CreateVertexBuffer(Length = %u, Usage = %s, FVF = %s, Pool = %s, ")
		_T("*ppVertexBuffer = %p, pSharedHandle = %p)"),
		Length, GetUsageString(Usage), GetFVFString(FVF), GetPoolString(Pool), 
		Indirect(ppVertexBuffer), pSharedHandle));

	if (Succeded(hResult))
	{
		IDirect3DVertexBuffer9 *pVertexBuffer = *ppVertexBuffer;
		DUMP_CMD_ALL(CreateVertexBuffer,Length,Usage,FVF,Pool,pVertexBuffer,pSharedHandle);
		HOOK_GET_DEVICE(ppVertexBuffer, Proxy_VertexBuffer_GetDevice, Original_VertexBuffer_GetDevice);
	}
	return hResult;
}

STDMETHODIMP CMonoRenderer::CreateIndexBuffer(UINT Length, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, 
													IDirect3DIndexBuffer9** ppIndexBuffer, HANDLE* pSharedHandle) 
{
	HRESULT hResult;
	NSCALL_TRACE2(m_Direct3DDevice.CreateIndexBuffer(Length, Usage, Format, Pool, 
		ppIndexBuffer, pSharedHandle),
		DEBUG_MESSAGE(_T("CreateIndexBuffer(Length = %u, Usage = %s, Format = %s, Pool = %s, ")
		_T("*ppIndexBuffer = %p, pSharedHandle = %p)"), 
		Length, GetUsageString(Usage), GetFormatString(Format), GetPoolString(Pool), 
		Indirect(ppIndexBuffer), pSharedHandle));

	if (Succeded(hResult))
	{
		IDirect3DIndexBuffer9 *pIndexBuffer = *ppIndexBuffer;
		DUMP_CMD_ALL(CreateIndexBuffer,Length,Usage,Format,Pool,pIndexBuffer,pSharedHandle);

		HOOK_GET_DEVICE(ppIndexBuffer, Proxy_IndexBuffer_GetDevice, Original_IndexBuffer_GetDevice);
	}
	return hResult;
}

STDMETHODIMP CMonoRenderer::TestCooperativeLevel(void) 
{
	HRESULT hResult;
	NSCALL_TRACE2(m_Direct3DDevice.TestCooperativeLevel(), 
		DEBUG_MESSAGE(_T("TestCooperativeLevel()")));
	return hResult;
}

STDMETHODIMP_(UINT) CMonoRenderer::GetAvailableTextureMem() 
{
	UINT nAvailableTextureMem = m_Direct3DDevice.GetAvailableTextureMem();
	DEBUG_TRACE2(_T("GetAvailableTextureMem() = %u;\n"), nAvailableTextureMem);
	return nAvailableTextureMem;
}

STDMETHODIMP CMonoRenderer::EvictManagedResources(void) 
{
	HRESULT hResult;
	NSCALL_TRACE2(m_Direct3DDevice.EvictManagedResources(),
		DEBUG_MESSAGE(_T("EvictManagedResources()")));
	return hResult;
}

STDMETHODIMP CMonoRenderer::GetDirect3D(IDirect3D9** ppD3D9) 
{
	if(ppD3D9 == NULL)
		return E_POINTER;

	if(CalledFromApp())
	{	
		m_pDirect3DWrapper->AddRef();
		*ppD3D9 = (IDirect3D9*)m_pDirect3DWrapper;
	}
	else
		m_pDirect3D.CopyTo(ppD3D9);
	
	DEBUG_TRACE2(_T("GetDirect3D(*ppD3D9 = %p)\n"), Indirect(ppD3D9));
	return S_OK;
}

STDMETHODIMP CMonoRenderer::GetDeviceCaps( D3DCAPS9 * pCaps ) 
{
	HRESULT hResult = S_OK;
	NSCALL_TRACE2(m_Direct3DDevice.GetDeviceCaps(pCaps), DEBUG_MESSAGE(_T("GetDeviceCaps(pCaps = %p)"), pCaps));
	return hResult;
}

STDMETHODIMP CMonoRenderer::GetDisplayMode(UINT iSwapChain, D3DDISPLAYMODE* pMode) 
{
	HRESULT hResult;
	NSCALL_TRACE1(m_Direct3DDevice.GetDisplayMode(iSwapChain, pMode), 
		DEBUG_MESSAGE(_T("GetDisplayMode(iSwapChain = %d, pMode: %s)"), 
		iSwapChain, GetDisplayModeString(pMode)));
	return hResult;
}

STDMETHODIMP CMonoRenderer::GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS* pParameters) 
{
	HRESULT hResult;
	NSCALL_TRACE2(m_Direct3DDevice.GetCreationParameters(pParameters), 
		DEBUG_MESSAGE(_T("GetCreationParameters(pParameters = %p)"), pParameters));
	if (SUCCEEDED(hResult))
		pParameters->AdapterOrdinal = GetOrigAdapterNumber(pParameters->AdapterOrdinal);
	return hResult;
}

STDMETHODIMP CMonoRenderer::SetCursorProperties(UINT XHotSpot, UINT YHotSpot, IDirect3DSurface9* pCursorBitmap) 
{
	HRESULT hResult;
	NSCALL_TRACE2(m_Direct3DDevice.SetCursorProperties(XHotSpot, YHotSpot, pCursorBitmap), 
		DEBUG_MESSAGE(_T("SetCursorProperties(XHotSpot = %u, YHotSpot = %u, pCursorBitmap = %p)"), 
		XHotSpot, YHotSpot, pCursorBitmap));
	return hResult;
}

STDMETHODIMP_(VOID) CMonoRenderer::SetCursorPosition(int X, int Y, DWORD Flags) 
{
	m_Direct3DDevice.SetCursorPosition(X, Y, Flags);
	DEBUG_TRACE2(_T("SetCursorPosition(X = %d, Y = %d, Flags = %08x)\n"), X, Y, Flags);
}

STDMETHODIMP_(BOOL) CMonoRenderer::ShowCursor(BOOL bShow) 
{
	BOOL bShowCursor;
	bShowCursor = m_Direct3DDevice.ShowCursor(bShow);
	DEBUG_TRACE2(_T("ShowCursor(bShow = %1d) = %d\n"), bShow, bShowCursor);
	return bShowCursor;
}

STDMETHODIMP CMonoRenderer::GetRasterStatus(UINT iSwapChain, D3DRASTER_STATUS* pRasterStatus) 
{
	HRESULT hResult;
	NSCALL_TRACE2(m_Direct3DDevice.GetRasterStatus(iSwapChain, pRasterStatus),
		DEBUG_MESSAGE(_T("GetRasterStatus(iSwapChain = %u, pRasterStatus = %p)"), 
		iSwapChain, pRasterStatus));
	return hResult;
}

STDMETHODIMP CMonoRenderer::SetDialogBoxMode(BOOL bEnableDialogs) 
{
	HRESULT hResult;
	NSCALL_TRACE2(m_Direct3DDevice.SetDialogBoxMode(bEnableDialogs),
		DEBUG_MESSAGE(_T("SetDialogBoxMode(bEnableDialogs = %1d)"), bEnableDialogs));
	return hResult;
}

STDMETHODIMP CMonoRenderer::CreateOffscreenPlainSurface(UINT Width, UINT Height, D3DFORMAT Format, D3DPOOL Pool, 
															  IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle) 
{
	HRESULT hResult;
	NSCALL_TRACE2(m_Direct3DDevice.CreateOffscreenPlainSurface(Width, Height, Format, 
		Pool, ppSurface, pSharedHandle),
		DEBUG_MESSAGE(_T("CreateOffscreenPlainSurface(Width = %u, Height = %u, Format = %s, ")
		_T("Pool = %s, *ppSurface = %p, pSharedHandle = %p)"), 
		Width, Height, GetFormatString(Format), GetPoolString(Pool), Indirect(ppSurface), pSharedHandle));

	if (Succeded(hResult))
	{
		IDirect3DSurface9 *pSurface = *ppSurface;
		DUMP_CMD_ALL(CreateOffscreenPlainSurface,Width,Height,Format,Pool,pSurface,pSharedHandle);
		HOOK_GET_DEVICE(ppSurface, Proxy_Surface_GetDevice, Original_Surface_GetDevice);
	}
	return hResult;
}

STDMETHODIMP CMonoRenderer::GetTransform(D3DTRANSFORMSTATETYPE State, D3DMATRIX* pMatrix) 
{
	HRESULT hResult;
	NSCALL_TRACE2(m_Direct3DDevice.GetTransform(State, pMatrix),
		DEBUG_MESSAGE(_T("GetTransform(State = %s, pMatrix = %p)"), GetTransformTypeString(State), pMatrix));
	return hResult;
}

STDMETHODIMP CMonoRenderer::GetViewport(D3DVIEWPORT9* pViewport) 
{
	HRESULT hResult;
	NSCALL_TRACE2(m_Direct3DDevice.GetViewport(pViewport),
		DEBUG_MESSAGE(_T("GetViewport(pViewport: %s)"), GetViewPortString(pViewport)));
	return hResult;
}

STDMETHODIMP CMonoRenderer::GetRenderTarget( DWORD RenderTargetIndex, IDirect3DSurface9** ppRenderTarget )
{
	HRESULT hResult = S_OK;
	NSCALL_TRACE2(m_Direct3DDevice.GetRenderTarget(RenderTargetIndex, ppRenderTarget),
		DEBUG_MESSAGE(_T("GetRenderTarget(RenderTargetIndex = %d, *ppRenderTarget = %p [%s])"),
		RenderTargetIndex, Indirect(ppRenderTarget), GetObjectName(*ppRenderTarget, hResult)));

	IDirect3DSurface9 *pRenderTarget = *ppRenderTarget;
	DUMP_CMD(GetRenderTarget,RenderTargetIndex,pRenderTarget);

	// 0.5.0 task #77 round 9: reverse-translation lie.
	// If round-2 auto-upscale engaged AND the real current RT is the pre-scale
	// wide RT (because we substituted it in PostInitialize / WideStereoRenderer::
	// SetRenderTarget), return m_pPrimaryBackBuffer back to the game so what it
	// gets from GetRenderTarget matches what it originally SetRenderTarget'd (or
	// inherited at CreateDevice time). Keeps the game's own tracking consistent.
	// Only applies to idx 0 (the primary RT). Only when scaling engaged.
	if (RenderTargetIndex == 0 && SUCCEEDED(hResult) && ppRenderTarget && *ppRenderTarget && !m_SwapChains.empty())
	{
		CBaseSwapChain* pBaseSC = dynamic_cast<CBaseSwapChain*>(GetMonoSC(0));
		if (pBaseSC && pBaseSC->m_bScalingEnabled &&
		    pBaseSC->m_pLeftBackBufferBeforeScaling &&
		    pBaseSC->m_pPrimaryBackBuffer &&
		    *ppRenderTarget == (IDirect3DSurface9*)pBaseSC->m_pLeftBackBufferBeforeScaling)
		{
			static int s_r9GRTLogCount = 0;
			if (s_r9GRTLogCount < 10) {
				D9Log("Task #77 round 9: reverse-translated GetRenderTarget pre-scale %p -> m_pPrimaryBackBuffer %p (idx=0, log %d/10)\n",
				      (void*)*ppRenderTarget,
				      (void*)(IDirect3DSurface9*)pBaseSC->m_pPrimaryBackBuffer,
				      s_r9GRTLogCount);
				s_r9GRTLogCount++;
			}
			// Release the pre-scale ref the real GetRenderTarget AddRef'd, then
			// return a referenced m_pPrimaryBackBuffer to the caller.
			(*ppRenderTarget)->Release();
			*ppRenderTarget = pBaseSC->m_pPrimaryBackBuffer;
			(*ppRenderTarget)->AddRef();
		}
	}

	return hResult;
}

STDMETHODIMP CMonoRenderer::GetDepthStencilSurface( IDirect3DSurface9** ppZStencilSurface ) 
{
	HRESULT hResult = S_OK;
	NSCALL_TRACE2(m_Direct3DDevice.GetDepthStencilSurface(ppZStencilSurface), 
		DEBUG_MESSAGE(_T("GetDepthStencilSurface(*ppZStencilSurface = %p [%s])"), 
		Indirect(ppZStencilSurface), GetObjectName(*ppZStencilSurface, hResult)));

	IDirect3DSurface9 *pZStencilSurface = *ppZStencilSurface;
	DUMP_CMD(GetDepthStencilSurface,pZStencilSurface);

	return hResult;
}

STDMETHODIMP CMonoRenderer::SetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD Value)
{
	DUMP_CMD_STATE(SetTextureStageState,Stage,Type,Value);

	HRESULT hResult;
	switch(Type)
	{
	case D3DTSS_BUMPENVMAT00:
	case D3DTSS_BUMPENVMAT01:
	case D3DTSS_BUMPENVMAT10:
	case D3DTSS_BUMPENVMAT11:
	case D3DTSS_BUMPENVLSCALE:
	case D3DTSS_BUMPENVLOFFSET:
		NSCALL_TRACE2(m_Direct3DDevice.SetTextureStageState(Stage, Type, Value),
			DEBUG_MESSAGE(_T("SetTextureStageState(Stage = %d, Type = %s, Value = %f)"), 
			Stage, GetTextureStageStateTypeString(Type), *reinterpret_cast<float*>(&Value)));
		break;
	default:
		NSCALL_TRACE2(m_Direct3DDevice.SetTextureStageState(Stage, Type, Value),
			DEBUG_MESSAGE(_T("SetTextureStageState(Stage = %d, Type = %s, Value = %08X)"), 
			Stage, GetTextureStageStateTypeString(Type), Value));
		break;
	}
	return hResult;
}

STDMETHODIMP CMonoRenderer::SetMaterial(CONST D3DMATERIAL9* pMaterial) 
{
	DUMP_CMD_STATE(SetMaterial,pMaterial);

	HRESULT hResult;
	NSCALL_TRACE2(m_Direct3DDevice.SetMaterial(pMaterial),
		DEBUG_MESSAGE(_T("SetMaterial(pMaterial = %p)"), pMaterial));
	return hResult;
}

STDMETHODIMP CMonoRenderer::GetMaterial(D3DMATERIAL9* pMaterial) 
{
	HRESULT hResult;
	NSCALL_TRACE2(m_Direct3DDevice.GetMaterial(pMaterial),
		DEBUG_MESSAGE(_T("GetMaterial(pMaterial = %p)"), pMaterial));
	return hResult;
}

STDMETHODIMP CMonoRenderer::SetLight(DWORD Index, CONST D3DLIGHT9* pLight) 
{
	DUMP_CMD_STATE(SetLight,Index,pLight);

	HRESULT hResult;
	NSCALL_TRACE2(m_Direct3DDevice.SetLight(Index, pLight),
		DEBUG_MESSAGE(_T("SetLight(Index = %08X, pLight = %p)"), Index, pLight));
	return hResult;
}

STDMETHODIMP CMonoRenderer::GetLight(DWORD Index, D3DLIGHT9* pLight) 
{
	HRESULT hResult;
	NSCALL_TRACE2(m_Direct3DDevice.GetLight(Index, pLight),
		DEBUG_MESSAGE(_T("GetLight(Index = %08X, pLight = %p)"), Index, pLight));
	return hResult;
}

STDMETHODIMP CMonoRenderer::LightEnable(DWORD Index, BOOL Enable) 
{
	DUMP_CMD_STATE(LightEnable,Index,Enable);

	HRESULT hResult;
	NSCALL_TRACE2(m_Direct3DDevice.LightEnable(Index, Enable),
		DEBUG_MESSAGE(_T("LightEnable(Index = %08X, Enable = %1d)"), Index, Enable));
	return hResult;
}

STDMETHODIMP CMonoRenderer::GetLightEnable(DWORD Index, BOOL* pEnable) 
{
	HRESULT hResult;
	NSCALL_TRACE2(m_Direct3DDevice.GetLightEnable(Index, pEnable),
		DEBUG_MESSAGE(_T("GetLightEnable(Index = %08X, *pEnable = %1d)"), Index, Indirect(pEnable)));
	return hResult;
}

STDMETHODIMP CMonoRenderer::SetClipPlane(DWORD Index, CONST float* pPlane) 
{
	ClipPlaneData plane(pPlane);
	DUMP_CMD_STATE(SetClipPlane,Index,plane);

	HRESULT hResult;
	NSCALL_TRACE2(m_Direct3DDevice.SetClipPlane(Index, pPlane),
		DEBUG_MESSAGE(_T("SetClipPlane(Index = %08X, pPlane = %p)"), Index, pPlane));
	return hResult;
}

STDMETHODIMP CMonoRenderer::GetClipPlane(DWORD Index, float* pPlane) 
{
	HRESULT hResult;
	NSCALL_TRACE2(m_Direct3DDevice.GetClipPlane(Index, pPlane),
		DEBUG_MESSAGE(_T("GetClipPlane(Index = %08X, pPlane = %p)"), Index, pPlane));
	return hResult;
}

STDMETHODIMP CMonoRenderer::GetRenderState(D3DRENDERSTATETYPE State, DWORD* pValue) 
{
	HRESULT hResult;
	NSCALL_TRACE2(m_Direct3DDevice.GetRenderState(State, pValue),
		DEBUG_MESSAGE(_T("GetRenderState(State = %s, *pValue = %08x)"), 
		GetRenderStateString(State), Indirect(pValue)));
	return hResult;
}

STDMETHODIMP CMonoRenderer::CreateStateBlock(D3DSTATEBLOCKTYPE Type, IDirect3DStateBlock9** ppSB)
{
	HRESULT hResult;
	NSCALL_TRACE2(m_Direct3DDevice.CreateStateBlock(Type, ppSB),
		DEBUG_MESSAGE(_T("CreateStateBlock(Type = %d, *ppSB = %p)"), Type, Indirect(ppSB)));

	if (Succeded(hResult))
	{
		HOOK_GET_DEVICE(ppSB, Proxy_StateBlock_GetDevice, Original_StateBlock_GetDevice);
#ifndef FINAL_RELEASE
		if (GET_DUMPER().CanDump() && GINFO_DUMP_ON) 
		{ 
			IDirect3DStateBlock9 *pSB = *ppSB;
			CDeviceState& sb = m_StateBlocks[pSB];
			sb.Create(Type, m_deviceState);
			//CaptureStateBlock(Type, sb);
			DUMP_CMD_ALL(CreateStateBlock,Type,pSB);
		}
#endif
	}
	return hResult;
}

STDMETHODIMP CMonoRenderer::SetClipStatus(CONST D3DCLIPSTATUS9* pClipStatus)
{
	DUMP_CMD_ALL(SetClipStatus,pClipStatus);

	HRESULT hResult;
	NSCALL_TRACE2(m_Direct3DDevice.SetClipStatus(pClipStatus),
		DEBUG_MESSAGE(_T("SetClipStatus(pClipStatus= %p)"), pClipStatus));
	return hResult;
}

STDMETHODIMP CMonoRenderer::GetClipStatus(D3DCLIPSTATUS9* pClipStatus)
{
	HRESULT hResult;
	NSCALL_TRACE2(m_Direct3DDevice.GetClipStatus(pClipStatus),
		DEBUG_MESSAGE(_T("GetClipStatus(pClipStatus = %p)"), pClipStatus));
	return hResult;
}

STDMETHODIMP CMonoRenderer::GetTexture(DWORD Stage, IDirect3DBaseTexture9** ppTexture)
{
	HRESULT hResult;
	NSCALL_TRACE2(m_Direct3DDevice.GetTexture(Stage, ppTexture),
		DEBUG_MESSAGE(_T("GetTexture(Stage = %d, *ppTexture = %p)"), 
		Stage, Indirect(ppTexture)));
	return hResult;
}

STDMETHODIMP CMonoRenderer::GetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD* pValue)
{
	HRESULT hResult;
	NSCALL_TRACE2(m_Direct3DDevice.GetTextureStageState(Stage, Type, pValue),
		DEBUG_MESSAGE(_T("GetTextureStageState(Stage = %d, Type = %s, *pValue = %d)"), 
		Stage, GetTextureStageStateTypeString(Type), Indirect(pValue)));
	return hResult;
}

STDMETHODIMP CMonoRenderer::GetSamplerState(DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD* pValue)
{
	HRESULT hResult;
	NSCALL_TRACE2(m_Direct3DDevice.GetSamplerState(Sampler, Type, pValue),
		DEBUG_MESSAGE(_T("GetSamplerState(Sampler = %02d, Type = %s, *pValue = %d)"), 
		Sampler, GetSamplerStateTypeString(Type), Indirect(pValue)));
	return hResult;
}

STDMETHODIMP CMonoRenderer::ValidateDevice(DWORD* pNumPasses)
{
	DUMP_CMD_ALL(ValidateDevice,pNumPasses);

	HRESULT hResult;
	NSCALL_TRACE2(m_Direct3DDevice.ValidateDevice(pNumPasses),
		DEBUG_MESSAGE(_T("ValidateDevice(*pNumPasses = %d)"), Indirect(pNumPasses)));
	return hResult;
}

STDMETHODIMP CMonoRenderer::SetPaletteEntries(UINT PaletteNumber, CONST PALETTEENTRY* pEntries)
{
	DUMP_CMD_ALL(SetPaletteEntries,PaletteNumber,pEntries);

	HRESULT hResult;
	NSCALL_TRACE2(m_Direct3DDevice.SetPaletteEntries(PaletteNumber, pEntries),
		DEBUG_MESSAGE(_T("SetPaletteEntries(PaletteNumber = %d, pEntries = %p)"), 
		PaletteNumber, pEntries));
	return hResult;
}

STDMETHODIMP CMonoRenderer::GetPaletteEntries(UINT PaletteNumber, PALETTEENTRY* pEntries)
{
	HRESULT hResult;
	NSCALL_TRACE2(m_Direct3DDevice.GetPaletteEntries(PaletteNumber, pEntries),
		DEBUG_MESSAGE(_T("GetPaletteEntries(PaletteNumber = %d, pEntries = %p)"), 
		PaletteNumber, pEntries));
	return hResult;
}

STDMETHODIMP CMonoRenderer::SetCurrentTexturePalette(UINT PaletteNumber)
{
	DUMP_CMD_STATE(SetCurrentTexturePalette,PaletteNumber);

	HRESULT hResult;
	NSCALL_TRACE2(m_Direct3DDevice.SetCurrentTexturePalette(PaletteNumber),
		DEBUG_MESSAGE(_T("SetCurrentTexturePalette(PaletteNumber = %d)"), PaletteNumber));
	return hResult;
}

STDMETHODIMP CMonoRenderer::GetCurrentTexturePalette(UINT* PaletteNumber)
{
	HRESULT hResult;
	NSCALL_TRACE2(m_Direct3DDevice.GetCurrentTexturePalette(PaletteNumber),
		DEBUG_MESSAGE(_T("GetCurrentTexturePalette(PaletteNumber = %p)"), PaletteNumber));
	return hResult;
}

STDMETHODIMP CMonoRenderer::GetScissorRect(RECT* pRect)
{
	HRESULT hResult;
	NSCALL_TRACE2(m_Direct3DDevice.GetScissorRect(pRect),
		DEBUG_MESSAGE(_T("GetScissorRect(pRect = %s)"), GetRectString(pRect)));
	return hResult;
}

STDMETHODIMP CMonoRenderer::SetSoftwareVertexProcessing(BOOL bSoftware)
{
	DUMP_CMD_ALL(SetSoftwareVertexProcessing,bSoftware);

	HRESULT hResult;
	NSCALL_TRACE2(m_Direct3DDevice.SetSoftwareVertexProcessing(bSoftware),
		DEBUG_MESSAGE(_T("SetSoftwareVertexProcessing(bSoftware = %1d)"), bSoftware));
	return hResult;
}

STDMETHODIMP_(BOOL) CMonoRenderer::GetSoftwareVertexProcessing()
{
	BOOL bSoftwareVertexProcessing = m_Direct3DDevice.GetSoftwareVertexProcessing();
	DEBUG_TRACE2(_T("GetSoftwareVertexProcessing() = %1d\n"), bSoftwareVertexProcessing);
	return bSoftwareVertexProcessing;
}

STDMETHODIMP CMonoRenderer::SetNPatchMode(float nSegments)
{
	DUMP_CMD_STATE(SetNPatchMode,nSegments);

	HRESULT hResult;
	NSCALL_TRACE2(m_Direct3DDevice.SetNPatchMode(nSegments),
		DEBUG_MESSAGE(_T("SetNPatchMode(nSegments = %8.2f)"), nSegments));
	return hResult;
}

STDMETHODIMP_(float) CMonoRenderer::GetNPatchMode()
{
	float fNPatchMode = m_Direct3DDevice.GetNPatchMode();
	DEBUG_TRACE2(_T("GetNPatchMode() = %8.2f\n"), fNPatchMode);
	return fNPatchMode;
}

STDMETHODIMP CMonoRenderer::ProcessVertices(UINT SrcStartIndex, UINT DestIndex, UINT VertexCount, 
												  IDirect3DVertexBuffer9* pDestBuffer, 
												  IDirect3DVertexDeclaration9* pVertexDecl, DWORD Flags)
{
	DUMP_CMD_ALL(ProcessVertices,SrcStartIndex,DestIndex,VertexCount,pDestBuffer,pVertexDecl,Flags);

	HRESULT hResult;
	NSCALL_TRACE2(m_Direct3DDevice.ProcessVertices(SrcStartIndex, DestIndex, VertexCount, 
		pDestBuffer, pVertexDecl, Flags),
		DEBUG_MESSAGE(_T("ProcessVertices(SrcStartIndex = %u, DestIndex = %u, VertexCount = %u, ")
		_T("pDestBuffer = %p, pVertexDecl = %p, Flags = %08X)"), 
		SrcStartIndex, DestIndex, VertexCount, pDestBuffer, pVertexDecl, Flags));
	return hResult;
}

STDMETHODIMP CMonoRenderer::CreateVertexDeclaration(CONST D3DVERTEXELEMENT9* pVertexElements, 
														  IDirect3DVertexDeclaration9** ppDecl)
{
	HRESULT hResult;
	NSCALL_TRACE2(m_Direct3DDevice.CreateVertexDeclaration(pVertexElements, ppDecl),
		DEBUG_MESSAGE(_T("CreateVertexDeclaration(pVertexElements = %p, *ppDecl = %p)"), 
		pVertexElements, Indirect(ppDecl)));

	if (Succeded(hResult))
	{
		IDirect3DVertexDeclaration9 *pDecl = *ppDecl;
		DUMP_CMD_ALL(CreateVertexDeclaration,pVertexElements,pDecl);
		HOOK_GET_DEVICE(ppDecl, Proxy_VertexDeclaration_GetDevice, Original_VertexDeclaration_GetDevice);
	}
	return hResult;
}

STDMETHODIMP CMonoRenderer::GetVertexDeclaration(IDirect3DVertexDeclaration9** ppDecl)
{
	HRESULT hResult;
	NSCALL_TRACE2(m_Direct3DDevice.GetVertexDeclaration(ppDecl),
		DEBUG_MESSAGE(_T("GetVertexDeclaration(*ppDecl = %p)"), Indirect(ppDecl)));
	return hResult;
}

STDMETHODIMP CMonoRenderer::GetFVF(DWORD* pFVF)
{
	HRESULT hResult;
	NSCALL_TRACE2(m_Direct3DDevice.GetFVF(pFVF),
		DEBUG_MESSAGE(_T("GetFVF(*pFVF = %s)"), pFVF? GetFVFString(*pFVF): 0));
	return hResult;
}

STDMETHODIMP CMonoRenderer::SetVertexShaderConstantF(UINT StartRegister, CONST float* pConstantData, UINT Vector4fCount)
{
	ShaderConstantsF constantData(pConstantData,Vector4fCount);
	DUMP_CMD_STATE(SetVertexShaderConstantF,StartRegister,constantData,Vector4fCount);

	HRESULT hResult;
	NSCALL_TRACE3(m_Direct3DDevice.SetVertexShaderConstantF(StartRegister, pConstantData, Vector4fCount),
		DEBUG_MESSAGE(_T("SetVertexShaderConstantF(StartRegister = %u, pConstantData = %p, Vector4fCount = %u)"), 
		StartRegister, pConstantData, Vector4fCount));
#ifdef ZLOG_ALLOW_TRACING
	if (zlog::GetSeverity() == zlog::SV_FLOOD)
	{
		for(UINT i=0; i< Vector4fCount; i++)
		{
			DEBUG_MESSAGE(_T("c%i: x = %f\ty = %f\tz = %f\tw = %f\n"), StartRegister+i, pConstantData[i*4+0], pConstantData[i*4+1], pConstantData[i*4+2], pConstantData[i*4+3]);
		}	
	}
#endif
	return hResult;
}

STDMETHODIMP CMonoRenderer::SetVertexShaderConstantI(UINT StartRegister, CONST int* pConstantData, UINT Vector4iCount)
{
	ShaderConstantsI constantData(pConstantData,Vector4iCount);
	DUMP_CMD_STATE(SetVertexShaderConstantI,StartRegister,constantData,Vector4iCount);

	HRESULT hResult;
	NSCALL_TRACE3(m_Direct3DDevice.SetVertexShaderConstantI(StartRegister, pConstantData, Vector4iCount),
		DEBUG_MESSAGE(_T("SetVertexShaderConstantI(StartRegister = %u, pConstantData = %p, Vector4iCount = %u)"), 
		StartRegister, pConstantData, Vector4iCount));
#ifdef ZLOG_ALLOW_TRACING
	if (zlog::GetSeverity() == zlog::SV_FLOOD)
	{
		for(UINT i=0; i< Vector4iCount; i++)
		{
			DEBUG_MESSAGE(_T("i%i: x = %d\ty = %d\tz = %d\tw = %d\n"), StartRegister+i, pConstantData[i*4+0], pConstantData[i*4+1], pConstantData[i*4+2], pConstantData[i*4+3]);
		}	
	}
#endif
	return hResult;
}

STDMETHODIMP CMonoRenderer::GetVertexShaderConstantF(UINT StartRegister, float* pConstantData, UINT Vector4fCount)
{
	HRESULT hResult;
	NSCALL_TRACE3(m_Direct3DDevice.GetVertexShaderConstantF(StartRegister, pConstantData, Vector4fCount),
		DEBUG_MESSAGE(_T("GetVertexShaderConstantI(StartRegister = %u, pConstantData = %p, Vector4fCount = %u)"), 
		StartRegister, pConstantData, Vector4fCount));
	return hResult;
}

STDMETHODIMP CMonoRenderer::GetVertexShaderConstantI(UINT StartRegister, int* pConstantData, UINT Vector4iCount)
{
	HRESULT hResult;
	NSCALL_TRACE3(m_Direct3DDevice.GetVertexShaderConstantI(StartRegister, pConstantData, Vector4iCount),
		DEBUG_MESSAGE(_T("GetVertexShaderConstantI(StartRegister = %u, pConstantData = %p, Vector4iCount = %u)"), 
		StartRegister, pConstantData, Vector4iCount));
	return hResult;
}

STDMETHODIMP CMonoRenderer::SetVertexShaderConstantB(UINT StartRegister, CONST BOOL* pConstantData, UINT BoolCount)
{
	ShaderConstantsB constantData(pConstantData,BoolCount);
	DUMP_CMD_STATE(SetVertexShaderConstantB,StartRegister,constantData,BoolCount);

	HRESULT hResult;
	NSCALL_TRACE3(m_Direct3DDevice.SetVertexShaderConstantB(StartRegister, pConstantData, BoolCount),
		DEBUG_MESSAGE(_T("SetVertexShaderConstantB(StartRegister = %u, pConstantData = %p, BoolCount = %u)"), 
		StartRegister, pConstantData, BoolCount));
#ifdef ZLOG_ALLOW_TRACING
	if (zlog::GetSeverity() == zlog::SV_FLOOD)
	{
		for(UINT i=0; i< BoolCount; i++)
		{
			DEBUG_MESSAGE(_T("b%i: %d\n"), StartRegister+i, pConstantData[i]);
		}	
	}
#endif
	return hResult;
}

STDMETHODIMP CMonoRenderer::GetVertexShaderConstantB(UINT StartRegister, BOOL* pConstantData, UINT BoolCount)
{
	HRESULT hResult;
	NSCALL_TRACE3(m_Direct3DDevice.GetVertexShaderConstantB(StartRegister, pConstantData, BoolCount),
		DEBUG_MESSAGE(_T("GetVertexShaderConstantB(StartRegister = %u, pConstantData = %p, BoolCount = %u)"), 
		StartRegister, pConstantData, BoolCount));
	return hResult;
}

STDMETHODIMP CMonoRenderer::GetStreamSource(UINT StreamNumber, IDirect3DVertexBuffer9** ppStreamData, UINT* OffsetInBytes, UINT* pStride)
{
	HRESULT hResult;
	NSCALL_TRACE2(m_Direct3DDevice.GetStreamSource(StreamNumber, ppStreamData, OffsetInBytes, pStride),
		DEBUG_MESSAGE(_T("GetStreamSource(StreamNumber = %u, *ppStreamData = %p, *OffsetInBytes = %p, *pStride = %p)"), 
		StreamNumber, Indirect(ppStreamData), Indirect(OffsetInBytes), Indirect(pStride)));
	return hResult;
}
STDMETHODIMP CMonoRenderer::SetStreamSourceFreq(UINT StreamNumber, UINT Divider)
{
	DUMP_CMD_STATE(SetStreamSourceFreq,StreamNumber,Divider);

	HRESULT hResult;
	NSCALL_TRACE2(m_Direct3DDevice.SetStreamSourceFreq(StreamNumber, Divider),
		DEBUG_MESSAGE(_T("SetStreamSourceFreq(StreamNumber = %u, Divider = %u)"), 
		StreamNumber, Divider));
	return hResult;
}

STDMETHODIMP CMonoRenderer::GetStreamSourceFreq(UINT StreamNumber, UINT* Divider)
{
	HRESULT hResult;
	NSCALL_TRACE2(m_Direct3DDevice.GetStreamSourceFreq(StreamNumber, Divider),
		DEBUG_MESSAGE(_T("GetStreamSourceFreq(StreamNumber = %u, *Divider = %u)"), 
		StreamNumber, Indirect(Divider)));
	return hResult;
}

STDMETHODIMP CMonoRenderer::SetIndices(IDirect3DIndexBuffer9* pIndexData)
{
	DUMP_CMD_STATE(SetIndices,pIndexData);

	HRESULT hResult;
	NSCALL_TRACE2(m_Direct3DDevice.SetIndices(pIndexData),
		DEBUG_MESSAGE(_T("SetIndices(pIndexData = %p)"), pIndexData));
	return hResult;
}

STDMETHODIMP CMonoRenderer::GetIndices(IDirect3DIndexBuffer9** ppIndexData)
{
	HRESULT hResult;
	NSCALL_TRACE2(m_Direct3DDevice.GetIndices(ppIndexData),
		DEBUG_MESSAGE(_T("GetIndices(*ppIndexData = %p)"), Indirect(ppIndexData)));
	return hResult;
}

STDMETHODIMP CMonoRenderer::GetPixelShader(IDirect3DPixelShader9** ppShader)
{
	HRESULT hResult;
	NSCALL_TRACE2(m_Direct3DDevice.GetPixelShader(ppShader),
		DEBUG_MESSAGE(_T("GetPixelShader(*ppShader = %p)"), Indirect(ppShader)));
	return hResult;
}

STDMETHODIMP CMonoRenderer::SetPixelShaderConstantF(UINT StartRegister, CONST float* pConstantData, UINT Vector4fCount)
{
	ShaderConstantsF constantData(pConstantData,Vector4fCount);
	DUMP_CMD_STATE(SetPixelShaderConstantF,StartRegister,constantData,Vector4fCount);

	HRESULT hResult;
	NSCALL_TRACE2(m_Direct3DDevice.SetPixelShaderConstantF(StartRegister, pConstantData, Vector4fCount),
		DEBUG_MESSAGE(_T("SetPixelShaderConstantF(StartRegister = %u, pConstantData = %p, Vector4fCount = %u)"), 
		StartRegister, pConstantData, Vector4fCount));
#ifdef ZLOG_ALLOW_TRACING
	if (zlog::GetSeverity() == zlog::SV_FLOOD)
	{
		for(UINT i=0; i< Vector4fCount; i++)
		{
			//--- we use 'blank' in format string for sign alignment ---
			DEBUG_MESSAGE(_T("c%i: x = % f\ty = % f\tz = % f\tw = % f\n"), StartRegister+i, pConstantData[i*4+0], pConstantData[i*4+1], pConstantData[i*4+2], pConstantData[i*4+3]);
		}	
	}
#endif
	return hResult;
}

STDMETHODIMP CMonoRenderer::GetPixelShaderConstantF(UINT StartRegister, float* pConstantData, UINT Vector4fCount)
{
	HRESULT hResult;
	NSCALL_TRACE2(m_Direct3DDevice.GetPixelShaderConstantF(StartRegister, pConstantData, Vector4fCount),
		DEBUG_MESSAGE(_T("GetPixelShaderConstantF(StartRegister = %u, pConstantData = %p, Vector4fCount = %u)"), 
		StartRegister, pConstantData, Vector4fCount));
	return hResult;
}

STDMETHODIMP CMonoRenderer::SetPixelShaderConstantI(UINT StartRegister, CONST int* pConstantData, UINT Vector4iCount)
{
	ShaderConstantsI constantData(pConstantData,Vector4iCount);
	DUMP_CMD_STATE(SetPixelShaderConstantI,StartRegister,constantData,Vector4iCount);

	HRESULT hResult;
	NSCALL_TRACE2(m_Direct3DDevice.SetPixelShaderConstantI(StartRegister, pConstantData, Vector4iCount),
		DEBUG_MESSAGE(_T("SetPixelShaderConstantI(StartRegister = %u, pConstantData = %p, Vector4iCount = %u)"), 
		StartRegister, pConstantData, Vector4iCount));
#ifdef ZLOG_ALLOW_TRACING
	if (zlog::GetSeverity() == zlog::SV_FLOOD)
	{
		for(UINT i=0; i< Vector4iCount; i++)
		{
			DEBUG_MESSAGE(_T("i%i: x = %d\ty = %d\tz = %d\tw = %d\n"), StartRegister+i, pConstantData[i*4+0], pConstantData[i*4+1], pConstantData[i*4+2], pConstantData[i*4+3]);
		}	
	}
#endif
	return hResult;
}

STDMETHODIMP CMonoRenderer::GetPixelShaderConstantI(UINT StartRegister, int* pConstantData, UINT Vector4iCount)
{
	HRESULT hResult;
	NSCALL_TRACE2(m_Direct3DDevice.GetPixelShaderConstantI(StartRegister, pConstantData, Vector4iCount),
		DEBUG_MESSAGE(_T("GetPixelShaderConstantI(StartRegister = %u, pConstantData = %p, Vector4iCount = %u)"), 
		StartRegister, pConstantData, Vector4iCount));
	return hResult;
}

STDMETHODIMP CMonoRenderer::SetPixelShaderConstantB(UINT StartRegister, CONST BOOL* pConstantData, UINT BoolCount)
{
	ShaderConstantsB constantData(pConstantData,BoolCount);
	DUMP_CMD_STATE(SetPixelShaderConstantB,StartRegister,constantData,BoolCount);

	HRESULT hResult;
	NSCALL_TRACE2(m_Direct3DDevice.SetPixelShaderConstantB(StartRegister, pConstantData, BoolCount),
		DEBUG_MESSAGE(_T("SetPixelShaderConstantB(StartRegister = %u, pConstantData = %p, BoolCount = %u)"), 
		StartRegister, pConstantData, BoolCount));
#ifdef ZLOG_ALLOW_TRACING
	if (zlog::GetSeverity() == zlog::SV_FLOOD)
	{
		for(UINT i=0; i< BoolCount; i++)
		{
			DEBUG_MESSAGE(_T("b%i: %d\n"), StartRegister+i, pConstantData[i]);
		}	
	}
#endif
	return hResult;
}

STDMETHODIMP CMonoRenderer::GetPixelShaderConstantB(UINT StartRegister, BOOL* pConstantData, UINT BoolCount)
{
	HRESULT hResult;
	NSCALL_TRACE2(m_Direct3DDevice.GetPixelShaderConstantB(StartRegister, pConstantData, BoolCount),
		DEBUG_MESSAGE(_T("GetPixelShaderConstantB(StartRegister = %u, pConstantData = %p, BoolCount = %u)"), 
		StartRegister, pConstantData, BoolCount));
	return hResult;
}

STDMETHODIMP CMonoRenderer::DeletePatch(UINT Handle)
{
	DUMP_CMD_ALL(DeletePatch,Handle);

	HRESULT hResult;
	NSCALL_TRACE2(m_Direct3DDevice.DeletePatch(Handle),
		DEBUG_MESSAGE(_T("DeletePatch(Handle = %d)"), Handle));
	return hResult;
}

STDMETHODIMP CMonoRenderer::CreateQuery(D3DQUERYTYPE Type, IDirect3DQuery9** ppQuery)
{
	HRESULT hResult;
	NSCALL_TRACE2(m_Direct3DDevice.CreateQuery(Type, ppQuery),
		DEBUG_MESSAGE(_T("CreateQuery(Type = %s, *ppQuery = %p)"), GetQueryTypeString(Type), Indirect(ppQuery)));
	HOOK_GET_DEVICE(ppQuery, Proxy_Query_GetDevice, Original_Query_GetDevice);
	return hResult;
}

STDMETHODIMP CMonoRenderer::UpdateSurface(IDirect3DSurface9* pSourceSurface, CONST RECT* pSourceRect, 
												IDirect3DSurface9* pDestinationSurface, CONST POINT* pDestPoint) 
{
	DUMP_CMD_ALL(UpdateSurface,pSourceSurface,pSourceRect,pDestinationSurface,pDestPoint);

	HRESULT hResult;
	NSCALL_TRACE2(m_Direct3DDevice.UpdateSurface(pSourceSurface, pSourceRect, 
		pDestinationSurface, pDestPoint),
		DEBUG_MESSAGE(_T("UpdateSurface(pSourceSurface = %p [%s], pSourceRect = %s, ")
		_T("pDestinationSurface = %p [%s], pDestPoint = (%d, &d))"), 
		pSourceSurface, GetObjectName(pSourceSurface), GetRectString(pSourceRect), 
		pDestinationSurface, GetObjectName(pDestinationSurface), 
		pDestPoint? pDestPoint->x: 0, pDestPoint? pDestPoint->y: 0));
	return hResult;
}

STDMETHODIMP CMonoRenderer::UpdateTexture(IDirect3DBaseTexture9* pSourceTexture, IDirect3DBaseTexture9* pDestinationTexture) 
{
	DUMP_CMD(UpdateTexture,pSourceTexture,pDestinationTexture);

	HRESULT hResult;
	NSCALL_TRACE2(m_Direct3DDevice.UpdateTexture(pSourceTexture, pDestinationTexture),
		DEBUG_MESSAGE(_T("UpdateTexture(pSourceTexture = %p [%s], pDestinationTexture = %p [%s])"), 
		pSourceTexture, GetObjectName(pSourceTexture), pDestinationTexture, GetObjectName(pDestinationTexture)));
	return hResult;
}

STDMETHODIMP CMonoRenderer::GetRenderTargetData(IDirect3DSurface9* pRenderTarget, IDirect3DSurface9* pDestSurface) 
{
	HRESULT hResult;
	NSCALL_TRACE2(m_Direct3DDevice.GetRenderTargetData(pRenderTarget, pDestSurface),
		DEBUG_MESSAGE(_T("GetRenderTargetData(pRenderTarget = %p [%s], pDestSurface = %p [%s])"), 
		pRenderTarget, GetObjectName(pRenderTarget), pDestSurface, GetObjectName(pDestSurface)));

	if (SUCCEEDED(hResult))
	{
		DUMP_CMD_ALL(GetRenderTargetData,pRenderTarget,pDestSurface);
	}

	return hResult;
}

STDMETHODIMP CMonoRenderer::GetFrontBufferData(UINT iSwapChain, IDirect3DSurface9* pDestSurface) 
{
	DUMP_CMD_ALL(GetFrontBufferData,iSwapChain,pDestSurface);

	HRESULT hResult;
	NSCALL_TRACE2(m_Direct3DDevice.GetFrontBufferData(iSwapChain, pDestSurface),
		DEBUG_MESSAGE(_T("GetFrontBufferData(iSwapChain = %u, pDestSurface = %p [%s])"), 
		iSwapChain, pDestSurface, GetObjectName(pDestSurface)));
	return hResult;
}

STDMETHODIMP CMonoRenderer::SetConvolutionMonoKernel(UINT width,UINT height,float* rows,float* columns)
{
	DUMP_CMD_ALL(SetConvolutionMonoKernel,width,height,rows,columns);

	HRESULT hResult;
	NSCALL_TRACE2(m_Direct3DDevice.SetConvolutionMonoKernel(width, height, rows, columns),
		DEBUG_MESSAGE(_T("SetConvolutionMonoKernel(width = %u, height = %u, rows = %p, columns = %p)"), 
		width, height, rows, columns));
	return hResult;
}

STDMETHODIMP CMonoRenderer::ComposeRects(IDirect3DSurface9* pSrc,IDirect3DSurface9* pDst,
			IDirect3DVertexBuffer9* pSrcRectDescs,UINT NumRects,IDirect3DVertexBuffer9* pDstRectDescs,
			D3DCOMPOSERECTSOP Operation,int Xoffset,int Yoffset)
{
	DUMP_CMD_ALL(ComposeRects,pSrc,pDst,pSrcRectDescs,NumRects,pDstRectDescs,Operation,Xoffset,Yoffset);
	
	HRESULT hResult;
	NSCALL_TRACE2(m_Direct3DDevice.ComposeRects(pSrc, pDst, pSrcRectDescs, NumRects,
		pDstRectDescs, Operation, Xoffset, Yoffset),
		DEBUG_MESSAGE(_T("ComposeRects(pSrc = %p [%s], pDst = %p [%s], pSrcRectDescs = %p, NumRects = %u,")
		_T("pDstRectDescs = %p, Operation = %d, Xoffset = %d, Yoffset = %d)"), 
		pSrc, GetObjectName(pSrc), pDst, GetObjectName(pDst), 
		pSrcRectDescs, NumRects, pDstRectDescs, Operation, Xoffset, Yoffset));
	return hResult;
}

STDMETHODIMP CMonoRenderer::GetGPUThreadPriority(INT* pPriority)
{
	HRESULT hResult;
	NSCALL_TRACE2(m_Direct3DDevice.GetGPUThreadPriority(pPriority),
		DEBUG_MESSAGE(_T("GetGPUThreadPriority(pPriority = %p)"), pPriority));
	return hResult;
}

STDMETHODIMP CMonoRenderer::SetGPUThreadPriority(INT Priority)
{
	DUMP_CMD_ALL(SetGPUThreadPriority,Priority);

	HRESULT hResult;
	NSCALL_TRACE2(m_Direct3DDevice.SetGPUThreadPriority(Priority),
		DEBUG_MESSAGE(_T("SetGPUThreadPriority(Priority = %d)"), Priority));
	return hResult;
}

STDMETHODIMP CMonoRenderer::WaitForVBlank(UINT iSwapChain)
{
	DUMP_CMD_ALL(WaitForVBlank,iSwapChain);

	HRESULT hResult;
	NSCALL_TRACE2(m_Direct3DDevice.WaitForVBlank(iSwapChain),
		DEBUG_MESSAGE(_T("WaitForVBlank(iSwapChain = %d)"), iSwapChain));
	return hResult;
}

STDMETHODIMP CMonoRenderer::CheckResourceResidency(IDirect3DResource9** pResourceArray, UINT32 NumResources)
{
	DUMP_CMD_ALL(CheckResourceResidency,pResourceArray,NumResources);

	HRESULT hResult;
	NSCALL_TRACE2(m_Direct3DDevice.CheckResourceResidency(pResourceArray, NumResources),
		DEBUG_MESSAGE(_T("CheckResourceResidency(pResourceArray = %p, NumResources = %u)"), pResourceArray, NumResources));
	return hResult;
}

STDMETHODIMP CMonoRenderer::SetMaximumFrameLatency(UINT MaxLatency)
{
	DUMP_CMD_ALL(SetMaximumFrameLatency,MaxLatency);

	HRESULT hResult;
	NSCALL_TRACE2(m_Direct3DDevice.SetMaximumFrameLatency(MaxLatency),
		DEBUG_MESSAGE(_T("SetMaximumFrameLatency(MaxLatency = %u)"), MaxLatency));
	return hResult;
}

STDMETHODIMP CMonoRenderer::GetMaximumFrameLatency(UINT* pMaxLatency)
{
	HRESULT hResult;
	NSCALL_TRACE2(m_Direct3DDevice.GetMaximumFrameLatency(pMaxLatency),
		DEBUG_MESSAGE(_T("GetMaximumFrameLatency(pMaxLatency = %p)"), pMaxLatency));
	return hResult;
}

STDMETHODIMP CMonoRenderer::CheckDeviceState(HWND hDestinationWindow)
{
	DUMP_CMD_ALL(CheckDeviceState,hDestinationWindow);

	HRESULT hResult;
	NSCALL_TRACE2(m_Direct3DDevice.CheckDeviceState(hDestinationWindow),
		DEBUG_MESSAGE(_T("CheckDeviceState(hDestinationWindow = %u)"), hDestinationWindow));
	return hResult;
}

STDMETHODIMP CMonoRenderer::GetDisplayModeEx(UINT iSwapChain,D3DDISPLAYMODEEX* pMode,D3DDISPLAYROTATION* pRotation)
{
	HRESULT hResult;
	NSCALL_TRACE2(m_Direct3DDevice.GetDisplayModeEx(iSwapChain, pMode, pRotation),
		DEBUG_MESSAGE(_T("GetDisplayModeEx(iSwapChain = %u, pMode = %p, pRotation = %p)"), iSwapChain, pMode, pRotation));
	return hResult;
}

STDMETHODIMP CMonoRenderer::CreateOffscreenPlainSurfaceEx(UINT Width, UINT Height, D3DFORMAT Format, D3DPOOL Pool,
													IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle, DWORD Usage) 
{
	HRESULT hResult;
	NSCALL_TRACE2(m_Direct3DDevice.CreateOffscreenPlainSurfaceEx(Width, Height, Format, 
		Pool, ppSurface, pSharedHandle, Usage),
		DEBUG_MESSAGE(_T("CreateOffscreenPlainSurfaceEx(Width = %u, Height = %u, Format = %s, ")
		_T("Pool = %s, *ppSurface = %p, pSharedHandle = %p, Usage = %d)"), 
		Width, Height, GetFormatString(Format), GetPoolString(Pool), Indirect(ppSurface), pSharedHandle, Usage));

	if (Succeded(hResult))
	{
		IDirect3DSurface9 *pSurface = *ppSurface;
		DUMP_CMD_ALL(CreateOffscreenPlainSurfaceEx,Width,Height,Format,Pool,pSurface,pSharedHandle,Usage);
		HOOK_GET_DEVICE(ppSurface, Proxy_Surface_GetDevice, Original_Surface_GetDevice);
	}
	return hResult;
}

#ifndef FINAL_RELEASE
void CMonoRenderer::OnNewTextureCreated(IDirect3DTexture9 *pTexture,UINT Levels)
{
	if (pTexture == NULL)
		return;
	HRESULT hResult = S_OK;
	for(UINT level = 0;level < Levels;level++)
	{
		CComPtr<IDirect3DSurface9> pSurface;
		NSCALL(pTexture->GetSurfaceLevel(level,&pSurface));
		DUMP_CMD_ALL(GetSurfaceLevel,pTexture,level,pSurface);
	}
}

void CMonoRenderer::OnNewTextureCreated(IDirect3DCubeTexture9 *pTexture,UINT Levels)
{
	if (pTexture == NULL)
		return;
	HRESULT hResult = S_OK;
	for(int f = 0;f < 6;f++)
	{
		for(UINT level = 0;level < Levels;level++)
		{
			D3DCUBEMAP_FACES face = (D3DCUBEMAP_FACES)f;
			CComPtr<IDirect3DSurface9> pSurface;
			NSCALL(pTexture->GetCubeMapSurface(face,level,&pSurface));
			DUMP_CMD_ALL(GetCubeMapSurface,pTexture,face,level,pSurface);
		}
	}
}

void CMonoRenderer::OnNewTextureCreated(IDirect3DVolumeTexture9 *pTexture,UINT Levels)
{
	if (pTexture == NULL)
		return;
	HRESULT hResult = S_OK;
	for(UINT level = 0;level < Levels;level++)
	{
		CComPtr<IDirect3DVolume9> pVolume;
		NSCALL(pTexture->GetVolumeLevel(level,&pVolume));
		DUMP_CMD_ALL(GetVolumeLevel,pTexture,level,pVolume);
	}
}
#endif // FINAL_RELEASE

STDMETHODIMP CMonoRenderer::CreateTexture( UINT Width,UINT Height,UINT Levels,DWORD Usage,D3DFORMAT Format,D3DPOOL Pool,IDirect3DTexture9** ppTexture,HANDLE* pSharedHandle )
{
	HRESULT hResult;
	NSCALL_TRACE1(m_Direct3DDevice.CreateTexture(Width, Height, Levels, Usage, Format, 
		Pool, ppTexture, pSharedHandle),
		DEBUG_MESSAGE(_T("\tCreateTexture(Width = %d, Height = %d, Levels = %d, Usage = %s, ")
		_T("Format = %s, Pool = %s, pRightTexture = %p, pSharedHandle = %p)"), 
		Width, Height, Levels, GetUsageString(Usage), GetFormatString(Format), 
		GetPoolString(Pool), ppTexture, pSharedHandle));
	
	if (Succeded(hResult))
	{
		IDirect3DTexture9 *pTexture = *ppTexture;
		{
			DUMP_CMD_ALL(CreateTexture,Width,Height,Levels,Usage,Format,Pool,pTexture,pSharedHandle);
		}
		OnNewTextureCreated(pTexture,Levels);

		HOOK_GET_DEVICE(ppTexture, Proxy_Texture_GetDevice, Original_Texture_GetDevice);
	}
	return hResult;
}

STDMETHODIMP CMonoRenderer::CreateCubeTexture( THIS_ UINT EdgeLength,UINT Levels,DWORD Usage,D3DFORMAT Format,D3DPOOL Pool,IDirect3DCubeTexture9** ppCubeTexture,HANDLE* pSharedHandle )
{
	HRESULT hResult;
	NSCALL_TRACE1(m_Direct3DDevice.CreateCubeTexture(EdgeLength, Levels, Usage, Format, 
		Pool, ppCubeTexture, pSharedHandle),
		DEBUG_MESSAGE(_T("CreateCubeTexture(EdgeLength = %u, Levels = %u, Usage = %s, ")
		_T("Format = %s, Pool = %s, *ppCubeTexture = %p, pSharedHandle = %p)"), 
		EdgeLength, Levels, GetUsageString(Usage), GetFormatString(Format), 
		GetPoolString(Pool), Indirect(ppCubeTexture), pSharedHandle));

	if (Succeded(hResult))
	{
		IDirect3DCubeTexture9 *pCubeTexture = *ppCubeTexture;
		{
			DUMP_CMD_ALL(CreateCubeTexture,EdgeLength,Levels,Usage,Format,Pool,pCubeTexture,pSharedHandle);
		}
		OnNewTextureCreated(pCubeTexture,Levels);

		HOOK_GET_DEVICE(ppCubeTexture, Proxy_CubeTexture_GetDevice, Original_CubeTexture_GetDevice);
	}
	return hResult;
}

STDMETHODIMP CMonoRenderer::CreateDepthStencilSurface( THIS_ UINT Width,UINT Height,D3DFORMAT Format,D3DMULTISAMPLE_TYPE MultiSample,DWORD MultisampleQuality,BOOL Discard,IDirect3DSurface9** ppSurface,HANDLE* pSharedHandle )
{
	HRESULT hResult;
	NSCALL_TRACE1(m_Direct3DDevice.CreateDepthStencilSurface(Width, Height, Format, 
		MultiSample, MultisampleQuality, Discard, ppSurface, pSharedHandle ),
		DEBUG_MESSAGE(_T("CreateDepthStencilSurface(Width = %d, Height = %d, Format = %s, MultiSample = %s,")
		_T("MultisampleQuality = %08X, Discard = %1d, *ppSurface = %p, ")
		_T("pSharedHandle = %p)"), Width, Height, GetFormatString(Format), GetMultiSampleString(MultiSample), 
		MultisampleQuality, Discard, Indirect(ppSurface), pSharedHandle));

	if (Succeded(hResult))
	{
		IDirect3DSurface9 *pSurface = *ppSurface;
		DUMP_CMD_ALL(CreateDepthStencilSurface,Width,Height,Format,MultiSample,MultisampleQuality,Discard,pSurface,pSharedHandle);
		HOOK_GET_DEVICE(ppSurface, Proxy_Surface_GetDevice, Original_Surface_GetDevice);
	}
	return hResult;
}

STDMETHODIMP CMonoRenderer::CreateDepthStencilSurfaceEx( UINT Width,UINT Height,D3DFORMAT Format,D3DMULTISAMPLE_TYPE MultiSample,DWORD MultisampleQuality,BOOL Discard,IDirect3DSurface9** ppSurface,HANDLE* pSharedHandle,DWORD Usage )
{
	HRESULT hResult;
	NSCALL_TRACE1(m_Direct3DDevice.CreateDepthStencilSurfaceEx(Width, Height, Format, 
		MultiSample, MultisampleQuality, Discard, ppSurface, pSharedHandle, Usage ),
		DEBUG_MESSAGE(_T("CreateDepthStencilSurfaceEx(Width = %d, Height = %d, Format = %s, MultiSample = %s,")
		_T("MultisampleQuality = %08X, Discard = %1d, *ppSurface = %p, ")
		_T("pSharedHandle = %p, Usage = %s)"), Width, Height, GetFormatString(Format), GetMultiSampleString(MultiSample), 
		MultisampleQuality, Discard, Indirect(ppSurface), pSharedHandle, GetUsageString(Usage)));

	if (Succeded(hResult))
	{
		IDirect3DSurface9 *pSurface = *ppSurface;
		DUMP_CMD_ALL(CreateDepthStencilSurfaceEx,Width,Height,Format,MultiSample,MultisampleQuality,Discard,pSurface,pSharedHandle,Usage);
		HOOK_GET_DEVICE(ppSurface, Proxy_Surface_GetDevice, Original_Surface_GetDevice);
	}
	return hResult;
}

STDMETHODIMP CMonoRenderer::CreateRenderTarget( THIS_ UINT Width,UINT Height,D3DFORMAT Format,D3DMULTISAMPLE_TYPE MultiSample,DWORD MultisampleQuality,BOOL Lockable,IDirect3DSurface9** ppSurface,HANDLE* pSharedHandle )
{
	HRESULT hResult;
	NSCALL_TRACE1(m_Direct3DDevice.CreateRenderTarget( Width, Height, Format, MultiSample, 
		MultisampleQuality, Lockable, ppSurface, pSharedHandle ),
		DEBUG_MESSAGE(_T("CreateRenderTarget(Width = %d, Height = %d, Format = %s, MultiSample = %s,")
		_T("MultisampleQuality = %08X, Lockable = %1d, *ppSurface = %p, pSharedHandle = %p)"), 
		Width, Height, GetFormatString(Format), GetMultiSampleString(MultiSample), 
		MultisampleQuality, Lockable, Indirect(ppSurface), pSharedHandle));

	if (Succeded(hResult))
	{
		IDirect3DSurface9 *pSurface = *ppSurface;
		DUMP_CMD_ALL(CreateRenderTarget,Width,Height,Format,MultiSample,MultisampleQuality,Lockable,pSurface,pSharedHandle);
		HOOK_GET_DEVICE(ppSurface, Proxy_Surface_GetDevice, Original_Surface_GetDevice);
	}
	return hResult;
}

STDMETHODIMP CMonoRenderer::CreateRenderTargetEx( UINT Width,UINT Height,D3DFORMAT Format,D3DMULTISAMPLE_TYPE MultiSample,DWORD MultisampleQuality,BOOL Lockable,IDirect3DSurface9** ppSurface,HANDLE* pSharedHandle,DWORD Usage )
{
	HRESULT hResult;
	NSCALL_TRACE1(m_Direct3DDevice.CreateRenderTargetEx( Width, Height, Format, MultiSample, 
		MultisampleQuality, Lockable, ppSurface, pSharedHandle, Usage ),
		DEBUG_MESSAGE(_T("CreateRenderTargetEx(Width = %d, Height = %d, Format = %s, MultiSample = %s,")
		_T("MultisampleQuality = %08X, Lockable = %1d, *ppSurface = %p, pSharedHandle = %p, Usage = %s)"), 
		Width, Height, GetFormatString(Format), GetMultiSampleString(MultiSample), 
		MultisampleQuality, Lockable, Indirect(ppSurface), pSharedHandle, GetUsageString(Usage)));

	if (Succeded(hResult))
	{
		IDirect3DSurface9 *pSurface = *ppSurface;
		DUMP_CMD_ALL(CreateRenderTargetEx,Width,Height,Format,MultiSample,MultisampleQuality,Lockable,pSurface,pSharedHandle,Usage);
		HOOK_GET_DEVICE(ppSurface, Proxy_Surface_GetDevice, Original_Surface_GetDevice);
	}
	return hResult;
}

STDMETHODIMP CMonoRenderer::StretchRect( THIS_ IDirect3DSurface9* pSourceSurface,CONST RECT* pSourceRect,IDirect3DSurface9* pDestSurface,CONST RECT* pDestRect,D3DTEXTUREFILTERTYPE Filter )
{
	HRESULT hResult;
	NSCALL_TRACE2(m_Direct3DDevice.StretchRect( pSourceSurface, pSourceRect, pDestSurface, pDestRect, Filter ),
		DEBUG_MESSAGE(_T("StretchRect(pSourceSurface = %p [%s], pSourceRect = %s, ")
		_T("pDestSurface = %p [%s], pDestRect = %s, Filter = %s)"), 
		pSourceSurface, GetObjectName(pSourceSurface), GetRectString(pSourceRect), 
		pDestSurface, GetObjectName(pDestSurface), GetRectString(pDestRect), 
		GetTextureFilterTypeString(Filter)));

	if (SUCCEEDED(hResult))
	{
		DUMP_CMD(StretchRect,pSourceSurface,pSourceRect,pDestSurface,pDestRect,Filter);
	}

	return hResult;
}

STDMETHODIMP CMonoRenderer::ColorFill( THIS_ IDirect3DSurface9* pSurface,CONST RECT* pRect,D3DCOLOR color )
{
	DUMP_CMD_ALL(ColorFill,pSurface,pRect,color); // -- shift2u

	HRESULT hResult;
	NSCALL_TRACE2(m_Direct3DDevice.ColorFill( pSurface, pRect, color ),
		DEBUG_MESSAGE(_T("ColorFill(pSurface = %p [%s], pRect = %s, color = %08X)"), 
		pSurface, GetObjectName(pSurface), GetRectString(pRect), color));
	return hResult;
}

STDMETHODIMP CMonoRenderer::SetRenderTarget( THIS_ DWORD RenderTargetIndex,IDirect3DSurface9* pRenderTarget )
{
	DUMP_CMD(SetRenderTarget,RenderTargetIndex,pRenderTarget);

	HRESULT hResult;
	NSCALL_TRACE2(m_Direct3DDevice.SetRenderTarget(RenderTargetIndex, pRenderTarget),
		DEBUG_MESSAGE(_T("SetRenderTarget(RenderTargetIndex = %d, pRenderTarget = %p [%s])"), 
		RenderTargetIndex, pRenderTarget, GetObjectName(pRenderTarget)));
	return hResult;
}

STDMETHODIMP CMonoRenderer::SetDepthStencilSurface( THIS_ IDirect3DSurface9* pNewZStencil )
{
	DUMP_CMD(SetDepthStencilSurface,pNewZStencil);

	HRESULT hResult;
	NSCALL_TRACE2(m_Direct3DDevice.SetDepthStencilSurface(pNewZStencil),
		DEBUG_MESSAGE(_T("SetDepthStencilSurface(pNewZStencil = %p [%s])"), 
		pNewZStencil, GetObjectName(pNewZStencil)));
	return hResult;
}

STDMETHODIMP CMonoRenderer::Clear( THIS_ DWORD Count,CONST D3DRECT* pRects,DWORD Flags,D3DCOLOR Color,float Z,DWORD Stencil )
{
	DUMP_CMD(Clear,Count,pRects,Flags,Color,Z,Stencil);

	HRESULT hResult;
	NSCALL_TRACE2(m_Direct3DDevice.Clear(Count, pRects, Flags, Color, Z, Stencil),
		DEBUG_MESSAGE(_T("Clear(%d, %s, %s, %08X, %12.7f, %ld)"), 
		Count, GetClearRectString(Count, pRects),
		GetClearFlagsString(Flags), Color, Z, Stencil));
	return hResult;
}

STDMETHODIMP CMonoRenderer::SetTexture( THIS_ DWORD Stage,IDirect3DBaseTexture9* pTexture )
{
	DUMP_CMD_STATE(SetTexture,Stage,pTexture);

	HRESULT hResult;
	NSCALL_TRACE2(m_Direct3DDevice.SetTexture(Stage, pTexture),
		DEBUG_MESSAGE(_T("SetTexture(Stage = %d, pTexture = %p [%s])"), 
		Stage, pTexture, GetObjectName(pTexture)));
	return hResult;
}

STDMETHODIMP_(UINT) CMonoRenderer::GetNumberOfSwapChains() 
{
	UINT nNumberOfSwapChains = m_Direct3DDevice.GetNumberOfSwapChains();
	DEBUG_TRACE2(_T("GetNumberOfSwapChains() = %u\n"), nNumberOfSwapChains);
	return nNumberOfSwapChains;
}

STDMETHODIMP CMonoRenderer::Reset( THIS_ D3DPRESENT_PARAMETERS* pPresentationParameters )
{
	DUMP_CMD(Reset,pPresentationParameters);

	HRESULT hResult;
	m_SwapChains[0]->Clear();
	GetMonoSC()->SetPresentationParameters(pPresentationParameters, NULL);
	NSCALL_TRACE1(m_Direct3DDevice.Reset(&GetMonoSC()->m_OriginalPresentationParameters), DEBUG_MESSAGE(_T("Reset()")));
	NSCALL(GetMonoSC()->InitializeEx(0));
	return hResult;
}

STDMETHODIMP CMonoRenderer::Present( THIS_ CONST RECT* pSourceRect,CONST RECT* pDestRect,HWND hDestWindowOverride,CONST RGNDATA* pDirtyRegion )
{
	DUMP_CMD_ALL(Present,pSourceRect,pDestRect,hDestWindowOverride,pDirtyRegion);

	HRESULT hResult;
	DEBUG_TRACE2(_T("%\n"), _T( __FUNCTION__ ) );
	NSCALL(m_Direct3DDevice.Present(pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion));
	return hResult;
}

STDMETHODIMP CMonoRenderer::GetBackBuffer( THIS_ UINT iSwapChain,UINT iBackBuffer,D3DBACKBUFFER_TYPE Type,IDirect3DSurface9** ppBackBuffer )
{
	HRESULT hResult = S_OK;
	NSCALL_TRACE2(m_Direct3DDevice.GetBackBuffer(iSwapChain, iBackBuffer, Type, ppBackBuffer),
		DEBUG_MESSAGE(_T("GetBackBuffer(iSwapChain = %d, iBackBuffer = %d, Type = %d, *ppBackBuffer = %p [%s])"), 
		iSwapChain, iBackBuffer, Type, Indirect(ppBackBuffer), GetObjectName(*ppBackBuffer, hResult)));

	IDirect3DSurface9 *pBackBuffer = *ppBackBuffer;
	DUMP_CMD_ALL(GetBackBuffer,iSwapChain,iBackBuffer,Type,pBackBuffer);

	return hResult;
}

STDMETHODIMP CMonoRenderer::SetTransform( THIS_ D3DTRANSFORMSTATETYPE State,CONST D3DMATRIX* pMatrix )
{
	DUMP_CMD_STATE(SetTransform,State,pMatrix);

	HRESULT hResult;
	NSCALL_TRACE2(m_Direct3DDevice.SetTransform(State, pMatrix),
		DEBUG_MESSAGE(_T("SetTransform(State = %s, pMatrix = %p)"), GetTransformTypeString(State), pMatrix));
	return hResult;
}

STDMETHODIMP CMonoRenderer::MultiplyTransform( THIS_ D3DTRANSFORMSTATETYPE State,CONST D3DMATRIX* pMatrix )
{
	DUMP_CMD_ALL(MultiplyTransform,State,pMatrix);

	HRESULT hResult;
	NSCALL_TRACE2(m_Direct3DDevice.MultiplyTransform(State, pMatrix),
		DEBUG_MESSAGE(_T("MultiplyTransform(State = %s, pMatrix = %p)"), GetTransformTypeString(State), pMatrix));
	return hResult;
}

STDMETHODIMP CMonoRenderer::SetRenderState( THIS_ D3DRENDERSTATETYPE State,DWORD Value )
{
	DUMP_CMD_ALL(SetRenderState,State,Value);

	HRESULT hResult;
	NSCALL_TRACE2(m_Direct3DDevice.SetRenderState(State, Value),
		DEBUG_MESSAGE(_T("SetRenderState(State = %s, Value = %08x)"),
		GetRenderStateString(State), Value));
	return hResult;
}

STDMETHODIMP CMonoRenderer::SetScissorRect( THIS_ CONST RECT* pRect )
{
	DUMP_CMD_STATE(SetScissorRect,pRect);

	HRESULT hResult;
	NSCALL_TRACE2(m_Direct3DDevice.SetScissorRect(pRect),
		DEBUG_MESSAGE(_T("SetScissorRect(pRect = %s)"),GetRectString(pRect)));
	return hResult;
}

STDMETHODIMP CMonoRenderer::SetViewport( THIS_ CONST D3DVIEWPORT9* pViewport )
{
	DUMP_CMD_STATE(SetViewport,pViewport);

	HRESULT hResult;
	NSCALL_TRACE2(m_Direct3DDevice.SetViewport(pViewport),
		DEBUG_MESSAGE(_T("SetViewport(pViewport = %s)"),GetViewPortString(pViewport)));
	return hResult;
}

STDMETHODIMP CMonoRenderer::DrawPrimitive( THIS_ D3DPRIMITIVETYPE PrimitiveType,UINT StartVertex,UINT PrimitiveCount )
{
	DUMP_CMD_DRAW(DrawPrimitive,PrimitiveType,StartVertex,PrimitiveCount);

	HRESULT hResult;
	NSCALL_TRACE3(m_Direct3DDevice.DrawPrimitive(PrimitiveType, StartVertex, PrimitiveCount),
		DEBUG_MESSAGE(_T("DP(PrimitiveType = %s, StartVertex = %u, PrimitiveCount = %u);\n"), 
		GetPrimitiveTypeString(PrimitiveType), StartVertex, PrimitiveCount));
	return hResult;
}

STDMETHODIMP CMonoRenderer::DrawIndexedPrimitive( THIS_ D3DPRIMITIVETYPE PrimitiveType,INT BaseVertexIndex,UINT MinVertexIndex,UINT NumVertices,UINT startIndex,UINT primCount )
{
	DUMP_CMD_DRAW(DrawIndexedPrimitive,PrimitiveType,BaseVertexIndex,MinVertexIndex,NumVertices,startIndex,primCount);

	HRESULT hResult;
	NSCALL_TRACE3(m_Direct3DDevice.DrawIndexedPrimitive(PrimitiveType, BaseVertexIndex,
		MinVertexIndex, NumVertices, startIndex, primCount),
		DEBUG_MESSAGE(_T("DIP(PrimitiveType = %s, BaseVertexIndex = %d, ")
		_T("MinVertexIndex = %u, NumVertices = %u, StartIndex = %u, PrimitiveCount = %u);\n"),
		GetPrimitiveTypeString(PrimitiveType), BaseVertexIndex,
		MinVertexIndex, NumVertices, startIndex, primCount));
	return hResult;
}

STDMETHODIMP CMonoRenderer::DrawPrimitiveUP( THIS_ D3DPRIMITIVETYPE PrimitiveType,UINT PrimitiveCount,CONST void* pVertexStreamZeroData,UINT VertexStreamZeroStride )
{
	JustData vertexStreamZeroData(pVertexStreamZeroData,VertexStreamZeroStride * PrimitiveCount * GetVertexCount(PrimitiveType,PrimitiveCount));
	DUMP_CMD_DRAW(DrawPrimitiveUP,PrimitiveType,PrimitiveCount,vertexStreamZeroData,VertexStreamZeroStride);

	HRESULT hResult;
	NSCALL_TRACE3(m_Direct3DDevice.DrawPrimitiveUP(PrimitiveType, PrimitiveCount,
		pVertexStreamZeroData, VertexStreamZeroStride),
		DEBUG_MESSAGE(_T("DPUP(PrimitiveType = %s, PrimitiveCount = %u, ")
		_T("pVertexStreamZeroData = %p, VertexStreamZeroStride = %u);\n"),
		GetPrimitiveTypeString(PrimitiveType), PrimitiveCount,
		pVertexStreamZeroData, VertexStreamZeroStride));
	return hResult;
}

STDMETHODIMP CMonoRenderer::DrawIndexedPrimitiveUP( THIS_ D3DPRIMITIVETYPE PrimitiveType,UINT MinVertexIndex,UINT NumVertices,UINT PrimitiveCount,CONST void* pIndexData,D3DFORMAT IndexDataFormat,CONST void* pVertexStreamZeroData,UINT VertexStreamZeroStride )
{
#ifndef FINAL_RELEASE
	int indexSize = IndexDataFormat == D3DFMT_INDEX16 ? 2 : 4;

	int indexCount = GetVertexCount(PrimitiveType,PrimitiveCount);
	JustData indexData(pIndexData,indexCount * indexSize);

	int vertexCount = NumVertices;
	JustData vertexStreamZeroData(pVertexStreamZeroData,vertexCount * VertexStreamZeroStride);

	DUMP_CMD_DRAW(DrawIndexedPrimitiveUP,PrimitiveType,MinVertexIndex,NumVertices,PrimitiveCount,indexData,IndexDataFormat,vertexStreamZeroData,VertexStreamZeroStride);
#endif

	HRESULT hResult;
	NSCALL_TRACE3(m_Direct3DDevice.DrawIndexedPrimitiveUP(PrimitiveType, MinVertexIndex, 
		NumVertices, PrimitiveCount, pIndexData, IndexDataFormat, pVertexStreamZeroData, 
		VertexStreamZeroStride),
		DEBUG_MESSAGE(_T("DIPUP(PrimitiveType = %s, MinIndex = %u, ")
		_T("NumVertices = %u, PrimitiveCount = %u, pIndexData = %p, ")
		_T("IndexDataFormat = %s, pVertexStreamZeroData = %p, VertexStreamZeroStride = %u);\n"),
		GetPrimitiveTypeString(PrimitiveType), MinVertexIndex, NumVertices, PrimitiveCount,
		pIndexData, GetFormatString(IndexDataFormat), pVertexStreamZeroData, 
		VertexStreamZeroStride));
	return hResult;
}

STDMETHODIMP CMonoRenderer::SetVertexDeclaration( THIS_ IDirect3DVertexDeclaration9* pDecl )
{
	DUMP_CMD_STATE(SetVertexDeclaration,pDecl);

	HRESULT hResult;
	NSCALL_TRACE2(m_Direct3DDevice.SetVertexDeclaration(pDecl),
		DEBUG_MESSAGE(_T("SetVertexDeclaration(pDecl = %p)\n"), pDecl));
	return hResult;
}

STDMETHODIMP CMonoRenderer::SetFVF( THIS_ DWORD FVF )
{
	DUMP_CMD_STATE(SetFVF,FVF);

	HRESULT hResult;
	NSCALL_TRACE2(m_Direct3DDevice.SetFVF(FVF),
		DEBUG_MESSAGE(_T("SetFVF(FVF = %s)"), GetFVFString(FVF)));
	return hResult;
}

STDMETHODIMP CMonoRenderer::SetVertexShader( THIS_ IDirect3DVertexShader9* pShader )
{
	DUMP_CMD_STATE(SetVertexShader,pShader);

	HRESULT hResult;
	NSCALL_TRACE2(m_Direct3DDevice.SetVertexShader(pShader),
		DEBUG_MESSAGE(_T("SetVertexShader(Shader = %p)\n"), pShader));
	return hResult;
}

STDMETHODIMP CMonoRenderer::GetVertexShader( THIS_ IDirect3DVertexShader9** ppShader )
{
	HRESULT hResult;
	NSCALL_TRACE2(m_Direct3DDevice.GetVertexShader(ppShader),	
		DEBUG_MESSAGE(_T("GetVertexShader(*ppShader = %p)"), Indirect(ppShader)));
	return hResult;
}

STDMETHODIMP CMonoRenderer::CreateVertexShader( THIS_ CONST DWORD* pFunction,IDirect3DVertexShader9** ppShader )
{
	HRESULT hResult;
	NSCALL_TRACE2(m_Direct3DDevice.CreateVertexShader(pFunction, ppShader),	
		DEBUG_MESSAGE(_T("CreateVertexShader(pFunction = %p, *ppShader = %p)"), pFunction, Indirect(ppShader)));

	if (Succeded(hResult))
	{
		IDirect3DVertexShader9 *pShader = *ppShader;
		ShaderFunction function(pFunction);
		DUMP_CMD_ALL(CreateVertexShader,function,pShader);

		HOOK_GET_DEVICE(ppShader, Proxy_VertexShader_GetDevice, Original_VertexShader_GetDevice);
	}
	return hResult;
}

STDMETHODIMP CMonoRenderer::SetStreamSource( THIS_ UINT StreamNumber,IDirect3DVertexBuffer9* pStreamData,UINT OffsetInBytes,UINT Stride )
{
	DUMP_CMD_STATE(SetStreamSource,StreamNumber,pStreamData,OffsetInBytes,Stride);

	HRESULT hResult;
	NSCALL_TRACE2(m_Direct3DDevice.SetStreamSource(StreamNumber, pStreamData, OffsetInBytes, Stride),	
		DEBUG_MESSAGE(_T("SetStreamSource(StreamNumber = %d, pStreamData = %p, OffsetInBytes = %d, Stride = %d)"), StreamNumber, pStreamData, OffsetInBytes, Stride));
	return hResult;
}

STDMETHODIMP CMonoRenderer::CreatePixelShader( THIS_ CONST DWORD* pFunction,IDirect3DPixelShader9** ppShader )
{
	HRESULT hResult;
	NSCALL_TRACE2(m_Direct3DDevice.CreatePixelShader(pFunction, ppShader),	
		DEBUG_MESSAGE(_T("CreatePixelShader(pFunction = %p, *ppShader = %p)"), pFunction, Indirect(ppShader)));

	if (Succeded(hResult))
	{
		IDirect3DPixelShader9 *pShader = *ppShader;
		ShaderFunction function(pFunction);
		DUMP_CMD_ALL(CreatePixelShader,function,pShader);

		HOOK_GET_DEVICE(ppShader, Proxy_PixelShader_GetDevice, Original_PixelShader_GetDevice);
	}
	return hResult;
}

STDMETHODIMP CMonoRenderer::SetPixelShader( THIS_ IDirect3DPixelShader9* pShader )
{
	DUMP_CMD_STATE(SetPixelShader,pShader);

	HRESULT hResult;
	NSCALL_TRACE2(m_Direct3DDevice.SetPixelShader(pShader),
		DEBUG_MESSAGE(_T("SetPixelShader(Shader = %p)\n"), pShader));
	return hResult;
}

STDMETHODIMP CMonoRenderer::DrawRectPatch( THIS_ UINT Handle,CONST float* pNumSegs,CONST D3DRECTPATCH_INFO* pRectPatchInfo )
{
	DUMP_CMD_ALL(DrawRectPatch,Handle,pNumSegs,pRectPatchInfo);

	HRESULT hResult;
	NSCALL_TRACE2(m_Direct3DDevice.DrawRectPatch(Handle, pNumSegs, pRectPatchInfo),
		DEBUG_MESSAGE(_T("DrawRectPatch(Handle = %d, pNumSegs = %p, pRectPatchInfo = %p)\n"), Handle, pNumSegs, pRectPatchInfo));
	return hResult;
}

STDMETHODIMP CMonoRenderer::DrawTriPatch( THIS_ UINT Handle,CONST float* pNumSegs,CONST D3DTRIPATCH_INFO* pTriPatchInfo )
{
	DUMP_CMD_ALL(DrawTriPatch,Handle,pNumSegs,pTriPatchInfo);

	HRESULT hResult;
	NSCALL_TRACE2(m_Direct3DDevice.DrawTriPatch(Handle, pNumSegs, pTriPatchInfo),
		DEBUG_MESSAGE(_T("DrawTriPatch(Handle = %d, pNumSegs = %p, pTriPatchInfo = %p)\n"), Handle, pNumSegs, pTriPatchInfo));
	return hResult;
}

STDMETHODIMP CMonoRenderer::PresentEx( THIS_ CONST RECT* pSourceRect,CONST RECT* pDestRect,HWND hDestWindowOverride,CONST RGNDATA* pDirtyRegion,DWORD dwFlags )
{
	DUMP_CMD(PresentEx,pSourceRect,pDestRect,hDestWindowOverride,pDirtyRegion,dwFlags);

	HRESULT hResult;
	DEBUG_TRACE2(_T("%\n"), _T( __FUNCTION__ ) );
	NSCALL(m_Direct3DDevice.PresentEx(pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion, dwFlags));
	return hResult;
}

STDMETHODIMP CMonoRenderer::ResetEx( THIS_ D3DPRESENT_PARAMETERS* pPresentationParameters,D3DDISPLAYMODEEX *pFullscreenDisplayMode )
{
	DUMP_CMD_ALL(ResetEx,pPresentationParameters,pFullscreenDisplayMode);

	HRESULT hResult;
	GetMonoSC()->SetPresentationParameters(pPresentationParameters, pFullscreenDisplayMode);
	NSCALL_TRACE1(m_Direct3DDevice.ResetEx(&GetMonoSC()->m_OriginalPresentationParameters, GetMonoSC()->m_pFullscreenDisplayMode), DEBUG_MESSAGE(_T("ResetEx()")));
	return hResult;
}

STDMETHODIMP_(void) CMonoRenderer::GetGammaRamp(THIS_ UINT iSwapChain,D3DGAMMARAMP* pRamp)
{
	DEBUG_TRACE2(_T("GetGammaRamp(iSwapChain = %d, pRamp = %08X);\n"), 
		iSwapChain, pRamp );
	m_Direct3DDevice.GetGammaRamp( iSwapChain, pRamp );
}

STDMETHODIMP_(void) CMonoRenderer::SetGammaRamp(THIS_ UINT iSwapChain,DWORD Flags,CONST D3DGAMMARAMP* pRamp)
{
	DUMP_CMD_ALL(SetGammaRamp,iSwapChain,Flags,pRamp);

	DEBUG_TRACE2(_T("SetGammaRamp(iSwapChain = %d, Flags = %08X, pRamp = %p);\n"), iSwapChain, Flags, pRamp );
	m_Direct3DDevice.SetGammaRamp( iSwapChain, Flags, pRamp );
}

#ifndef FINAL_RELEASE
void CMonoRenderer::CaptureStateBlock(D3DSTATEBLOCKTYPE Type, CDeviceState& sb)
{
	HRESULT hResult = S_OK;
	sb.Clear();
	bool bAll = (Type == D3DSBT_ALL);
	bool bPixel = bAll || (Type == D3DSBT_PIXELSTATE);
	bool bVertex = bAll || (Type == D3DSBT_VERTEXSTATE);
	DWORD Value;
#define CAPTURE_RS(state) {hResult = m_Direct3DDevice.GetRenderState(state, &Value); \
	if (SUCCEEDED(hResult)) sb.SetRenderState(state, Value);}

	if (bPixel)
	{
		CAPTURE_RS(D3DRS_ZENABLE);
		CAPTURE_RS(D3DRS_FILLMODE);
		CAPTURE_RS(D3DRS_SHADEMODE);
		CAPTURE_RS(D3DRS_ZWRITEENABLE);
		CAPTURE_RS(D3DRS_ALPHATESTENABLE);
		CAPTURE_RS(D3DRS_LASTPIXEL);
		CAPTURE_RS(D3DRS_SRCBLEND);
		CAPTURE_RS(D3DRS_DESTBLEND);
		CAPTURE_RS(D3DRS_ZFUNC);
		CAPTURE_RS(D3DRS_ALPHAREF);                  
		CAPTURE_RS(D3DRS_ALPHAFUNC);                 
		CAPTURE_RS(D3DRS_DITHERENABLE);              
		CAPTURE_RS(D3DRS_ALPHABLENDENABLE);                 
		CAPTURE_RS(D3DRS_SPECULARENABLE);    
		CAPTURE_RS(D3DRS_STENCILENABLE);             
		CAPTURE_RS(D3DRS_STENCILFAIL);               
		CAPTURE_RS(D3DRS_STENCILZFAIL);              
		CAPTURE_RS(D3DRS_STENCILPASS);               
		CAPTURE_RS(D3DRS_STENCILFUNC);               
		CAPTURE_RS(D3DRS_STENCILREF);                
		CAPTURE_RS(D3DRS_STENCILMASK);               
		CAPTURE_RS(D3DRS_STENCILWRITEMASK);          
		CAPTURE_RS(D3DRS_TEXTUREFACTOR);             
		CAPTURE_RS(D3DRS_WRAP0);                     
		CAPTURE_RS(D3DRS_WRAP1);                     
		CAPTURE_RS(D3DRS_WRAP2);                     
		CAPTURE_RS(D3DRS_WRAP3);                     
		CAPTURE_RS(D3DRS_WRAP4);                     
		CAPTURE_RS(D3DRS_WRAP5);                     
		CAPTURE_RS(D3DRS_WRAP6);                     
		CAPTURE_RS(D3DRS_WRAP7);                    
		CAPTURE_RS(D3DRS_COLORWRITEENABLE);         
		CAPTURE_RS(D3DRS_BLENDOP);      
		CAPTURE_RS(D3DRS_SCISSORTESTENABLE);         
		CAPTURE_RS(D3DRS_SLOPESCALEDEPTHBIAS);       
		CAPTURE_RS(D3DRS_ANTIALIASEDLINEENABLE); 
		CAPTURE_RS(D3DRS_TWOSIDEDSTENCILMODE);       
		CAPTURE_RS(D3DRS_CCW_STENCILFAIL);           
		CAPTURE_RS(D3DRS_CCW_STENCILZFAIL);          
		CAPTURE_RS(D3DRS_CCW_STENCILPASS);           
		CAPTURE_RS(D3DRS_CCW_STENCILFUNC);           
		CAPTURE_RS(D3DRS_COLORWRITEENABLE1);         
		CAPTURE_RS(D3DRS_COLORWRITEENABLE2);         
		CAPTURE_RS(D3DRS_COLORWRITEENABLE3);         
		CAPTURE_RS(D3DRS_BLENDFACTOR);               
		CAPTURE_RS(D3DRS_SRGBWRITEENABLE);          
		CAPTURE_RS(D3DRS_DEPTHBIAS);                 
		CAPTURE_RS(D3DRS_WRAP8);                     
		CAPTURE_RS(D3DRS_WRAP9);                     
		CAPTURE_RS(D3DRS_WRAP10);                    
		CAPTURE_RS(D3DRS_WRAP11);                    
		CAPTURE_RS(D3DRS_WRAP12);                    
		CAPTURE_RS(D3DRS_WRAP13);                    
		CAPTURE_RS(D3DRS_WRAP14);                    
		CAPTURE_RS(D3DRS_WRAP15);                    
		CAPTURE_RS(D3DRS_SEPARATEALPHABLENDENABLE);  
		CAPTURE_RS(D3DRS_SRCBLENDALPHA);             
		CAPTURE_RS(D3DRS_DESTBLENDALPHA);            
		CAPTURE_RS(D3DRS_BLENDOPALPHA);
	}

	if (bVertex)
	{
		CAPTURE_RS(D3DRS_CULLMODE);         
		CAPTURE_RS(D3DRS_FOGENABLE);            
		CAPTURE_RS(D3DRS_FOGCOLOR);                  
		CAPTURE_RS(D3DRS_FOGTABLEMODE);
		CAPTURE_RS(D3DRS_RANGEFOGENABLE);                    
		CAPTURE_RS(D3DRS_CLIPPING);                  
		CAPTURE_RS(D3DRS_LIGHTING);                  
		CAPTURE_RS(D3DRS_AMBIENT);                   
		CAPTURE_RS(D3DRS_FOGVERTEXMODE);             
		CAPTURE_RS(D3DRS_COLORVERTEX);               
		CAPTURE_RS(D3DRS_NORMALIZENORMALS); //???      
		CAPTURE_RS(D3DRS_VERTEXBLEND);               
		CAPTURE_RS(D3DRS_CLIPPLANEENABLE);           
		CAPTURE_RS(D3DRS_POINTSIZE);                 
		CAPTURE_RS(D3DRS_POINTSIZE_MIN);             
		CAPTURE_RS(D3DRS_POINTSPRITEENABLE);         
		CAPTURE_RS(D3DRS_POINTSCALEENABLE);          
		CAPTURE_RS(D3DRS_POINTSCALE_A);              
		CAPTURE_RS(D3DRS_POINTSCALE_B);              
		CAPTURE_RS(D3DRS_POINTSCALE_C);              
		CAPTURE_RS(D3DRS_MULTISAMPLEANTIALIAS);      
		CAPTURE_RS(D3DRS_MULTISAMPLEMASK);           
		CAPTURE_RS(D3DRS_PATCHEDGESTYLE);          
		CAPTURE_RS(D3DRS_POINTSIZE_MAX);             
		CAPTURE_RS(D3DRS_INDEXEDVERTEXBLENDENABLE);  
		CAPTURE_RS(D3DRS_TWEENFACTOR);              
		CAPTURE_RS(D3DRS_POSITIONDEGREE);            
		CAPTURE_RS(D3DRS_NORMALDEGREE);             
		CAPTURE_RS(D3DRS_MINTESSELLATIONLEVEL);      
		CAPTURE_RS(D3DRS_MAXTESSELLATIONLEVEL);      
		CAPTURE_RS(D3DRS_ADAPTIVETESS_X);            
		CAPTURE_RS(D3DRS_ADAPTIVETESS_Y);            
		CAPTURE_RS(D3DRS_ADAPTIVETESS_Z);            
		CAPTURE_RS(D3DRS_ADAPTIVETESS_W);            
		CAPTURE_RS(D3DRS_ENABLEADAPTIVETESSELLATION);
	}

	if (bPixel || bVertex)
	{               
		CAPTURE_RS(D3DRS_FOGSTART);                  
		CAPTURE_RS(D3DRS_FOGEND);              
		CAPTURE_RS(D3DRS_FOGDENSITY);        
		CAPTURE_RS(D3DRS_LOCALVIEWER);
		CAPTURE_RS(D3DRS_DIFFUSEMATERIALSOURCE);     
		CAPTURE_RS(D3DRS_SPECULARMATERIALSOURCE);    
		CAPTURE_RS(D3DRS_AMBIENTMATERIALSOURCE);     
		CAPTURE_RS(D3DRS_EMISSIVEMATERIALSOURCE); 
	} 

	if (bAll)
	{
		CAPTURE_RS(D3DRS_DEBUGMONITORTOKEN);  //???  
	}  

#undef CAPTURE_RS

	D3DCAPS9 D3DCaps;
	NSCALL(m_pDirect3D->GetDeviceCaps(m_OriginalAdapter, m_DeviceType, &D3DCaps));

#define CAPTURE_TSS(y,state) {hResult = m_Direct3DDevice.GetTextureStageState(y, state, &Value); \
	if (SUCCEEDED(hResult)) sb.SetTextureStageState(y, state, Value);}
#define CAPTURE_SS(y,state) {hResult = m_Direct3DDevice.GetSamplerState(y, state, &Value); \
	if (SUCCEEDED(hResult)) sb.SetSamplerState(y, state, Value);}

	DWORD maxTextures = D3DCaps.MaxSimultaneousTextures;
	DWORD PSVersion = D3DCaps.PixelShaderVersion & 0xFFFF;
	if (PSVersion >= 0x0200)
		maxTextures = std::max<DWORD>(16, maxTextures);
	else if (PSVersion >= 0x0104)
		maxTextures = std::max<DWORD>(6, maxTextures);
	else 
		maxTextures = std::max<DWORD>(4, maxTextures);

	for (DWORD i = 0; i < maxTextures; i++)
	{
		if (bPixel)
		{      
			CAPTURE_TSS(i, D3DTSS_COLOROP);
			CAPTURE_TSS(i, D3DTSS_COLORARG1);
			CAPTURE_TSS(i, D3DTSS_COLORARG2);
			CAPTURE_TSS(i, D3DTSS_ALPHAOP);
			CAPTURE_TSS(i, D3DTSS_ALPHAARG1);
			CAPTURE_TSS(i, D3DTSS_ALPHAARG2);
			CAPTURE_TSS(i, D3DTSS_BUMPENVMAT00);
			CAPTURE_TSS(i, D3DTSS_BUMPENVMAT01);
			CAPTURE_TSS(i, D3DTSS_BUMPENVMAT10);
			CAPTURE_TSS(i, D3DTSS_BUMPENVMAT11);
			CAPTURE_TSS(i, D3DTSS_BUMPENVLSCALE);
			CAPTURE_TSS(i, D3DTSS_BUMPENVLOFFSET);
			CAPTURE_TSS(i, D3DTSS_COLORARG0);
			CAPTURE_TSS(i, D3DTSS_ALPHAARG0);
			CAPTURE_TSS(i, D3DTSS_RESULTARG);
			CAPTURE_TSS(i, D3DTSS_CONSTANT); // ???

			CAPTURE_SS(i, D3DSAMP_ADDRESSU);
			CAPTURE_SS(i, D3DSAMP_ADDRESSV);
			CAPTURE_SS(i, D3DSAMP_ADDRESSW);
			CAPTURE_SS(i, D3DSAMP_BORDERCOLOR);
			CAPTURE_SS(i, D3DSAMP_MAGFILTER);
			CAPTURE_SS(i, D3DSAMP_MINFILTER);
			CAPTURE_SS(i, D3DSAMP_MIPFILTER);
			CAPTURE_SS(i, D3DSAMP_MIPMAPLODBIAS);
			CAPTURE_SS(i, D3DSAMP_MAXMIPLEVEL);
			CAPTURE_SS(i, D3DSAMP_MAXANISOTROPY);
			CAPTURE_SS(i, D3DSAMP_SRGBTEXTURE);
			CAPTURE_SS(i, D3DSAMP_ELEMENTINDEX);
		}

		if (bVertex)
		{    
			CAPTURE_SS(i, D3DSAMP_DMAPOFFSET);
		}

		if (bPixel || bVertex)
		{      
			CAPTURE_TSS(i, D3DTSS_TEXCOORDINDEX);
			CAPTURE_TSS(i, D3DTSS_TEXTURETRANSFORMFLAGS);
		}

		if (bAll)
		{      
			CComPtr<IDirect3DBaseTexture9> pBaseTexture;
			hResult = m_Direct3DDevice.GetTexture(i, &pBaseTexture);
			if (SUCCEEDED(hResult)) sb.SetTexture(i, pBaseTexture);
		}
	}

	DWORD VSVersion = D3DCaps.VertexShaderVersion & 0xFFFF;
	if (VSVersion >= 0x0300)
		maxTextures = 4 + 1;
	else 
		maxTextures = 1;

	for (DWORD i = D3DDMAPSAMPLER; i < D3DDMAPSAMPLER + maxTextures; i++)
	{
		if (bPixel)
		{      
			CAPTURE_TSS(i, D3DTSS_COLOROP);
			CAPTURE_TSS(i, D3DTSS_COLORARG1);
			CAPTURE_TSS(i, D3DTSS_COLORARG2);
			CAPTURE_TSS(i, D3DTSS_ALPHAOP);
			CAPTURE_TSS(i, D3DTSS_ALPHAARG1);
			CAPTURE_TSS(i, D3DTSS_ALPHAARG2);
			CAPTURE_TSS(i, D3DTSS_BUMPENVMAT00);
			CAPTURE_TSS(i, D3DTSS_BUMPENVMAT01);
			CAPTURE_TSS(i, D3DTSS_BUMPENVMAT10);
			CAPTURE_TSS(i, D3DTSS_BUMPENVMAT11);
			CAPTURE_TSS(i, D3DTSS_BUMPENVLSCALE);
			CAPTURE_TSS(i, D3DTSS_BUMPENVLOFFSET);
			CAPTURE_TSS(i, D3DTSS_COLORARG0);
			CAPTURE_TSS(i, D3DTSS_ALPHAARG0);
			CAPTURE_TSS(i, D3DTSS_RESULTARG);
			CAPTURE_TSS(i, D3DTSS_CONSTANT); // ???

			CAPTURE_SS(i, D3DSAMP_ADDRESSU);
			CAPTURE_SS(i, D3DSAMP_ADDRESSV);
			CAPTURE_SS(i, D3DSAMP_ADDRESSW);
			CAPTURE_SS(i, D3DSAMP_BORDERCOLOR);
			CAPTURE_SS(i, D3DSAMP_MAGFILTER);
			CAPTURE_SS(i, D3DSAMP_MINFILTER);
			CAPTURE_SS(i, D3DSAMP_MIPFILTER);
			CAPTURE_SS(i, D3DSAMP_MIPMAPLODBIAS);
			CAPTURE_SS(i, D3DSAMP_MAXMIPLEVEL);
			CAPTURE_SS(i, D3DSAMP_MAXANISOTROPY);
			CAPTURE_SS(i, D3DSAMP_SRGBTEXTURE);
			CAPTURE_SS(i, D3DSAMP_ELEMENTINDEX);
		}

		if (bVertex)
		{    
			CAPTURE_SS(i, D3DSAMP_DMAPOFFSET);
		}

		if (bPixel || bVertex)
		{      
			CAPTURE_TSS(i, D3DTSS_TEXCOORDINDEX);
			CAPTURE_TSS(i, D3DTSS_TEXTURETRANSFORMFLAGS);
		}

		if (bAll)
		{      
			CComPtr<IDirect3DBaseTexture9> pBaseTexture;
			hResult = m_Direct3DDevice.GetTexture(i, &pBaseTexture);
			if (SUCCEEDED(hResult)) sb.SetTexture(i, pBaseTexture);
		}
	}

#undef CAPTURE_TSS
#undef CAPTURE_SS

#define CAPTURE_VALUE(x, y) {hResult = m_Direct3DDevice.Get##x(&y); \
	if (SUCCEEDED(hResult)) sb.Set##x(y);}
#define CAPTURE_VALUE2(x, z, y) {hResult = m_Direct3DDevice.Get##x(z, &y); \
	if (SUCCEEDED(hResult)) sb.Set##x(z, y);}
#define CAPTURE(x, ...) {hResult = m_Direct3DDevice.Get##x(__VA_ARGS__); \
	if (SUCCEEDED(hResult)) sb.Set##x(__VA_ARGS__);}

	if (bPixel)
	{
		CComPtr<IDirect3DPixelShader9> pShader;
		CAPTURE_VALUE(PixelShader, pShader);
		std::vector<float> floatConstants;
		UINT floatConsts = 224; 
		if (PSVersion < 0x0300)
			floatConsts = 32;
		floatConstants.resize(floatConsts * 4);
		CAPTURE(PixelShaderConstantF, 0, floatConstants.data(), floatConsts);
		int intConstants[16*4];
		CAPTURE(PixelShaderConstantI, 0, intConstants, 16);
		BOOL boolConstants[16];
		CAPTURE(PixelShaderConstantB, 0, boolConstants, 16);
	}

	if (bVertex)
	{
		CComPtr<IDirect3DVertexShader9> pShader;
		CAPTURE_VALUE(VertexShader, pShader);
		UINT floatConsts = D3DCaps.MaxVertexShaderConst; 
		std::vector<float> floatConstants(floatConsts*4);
		CAPTURE(VertexShaderConstantF, 0, floatConstants.data(), floatConsts);
		int intConstants[16*4];
		CAPTURE(VertexShaderConstantI, 0, intConstants, 16);
		BOOL boolConstants[16];
		CAPTURE(VertexShaderConstantB, 0, boolConstants, 16);

		CAPTURE_VALUE(FVF, Value);
		CComPtr<IDirect3DVertexDeclaration9> pDecl;
		CAPTURE_VALUE(VertexDeclaration, pDecl);
		float nSegments = m_Direct3DDevice.GetNPatchMode();
		sb.SetNPatchMode(nSegments);
		/*for (DWORD i = 0; i < D3DCaps.MaxActiveLights; i++)
		{
			BOOL Enable;
			hResult = m_Direct3DDevice.GetLightEnable(i, &Enable);
			if (FAILED(hResult))
				break;
			sb.LightEnable(i, Enable);
			D3DLIGHT9 light;
			CAPTURE(Light, i, &light);
		}*/
		for (DWORD i = 0; i < D3DCaps.MaxStreams; i++)
		{
			UINT freq;
			CAPTURE_VALUE2(StreamSourceFreq, i, freq);
		}
	}

	if (bAll)
	{      
		UINT pallete;
		CAPTURE_VALUE(CurrentTexturePalette, pallete);
		CComPtr<IDirect3DIndexBuffer9> pIndexData;
		CAPTURE_VALUE(Indices, pIndexData);
		D3DVIEWPORT9 viewport;
		CAPTURE(Viewport, &viewport);
		RECT rect;
		CAPTURE(ScissorRect, &rect);

		for (DWORD i = 0; i < D3DCaps.MaxStreams; i++)
		{
			CComPtr<IDirect3DVertexBuffer9> pVB;
			UINT Offset, Stride;
			hResult = m_Direct3DDevice.GetStreamSource(i, &pVB, &Offset, &Stride);
			if (SUCCEEDED(hResult))
				sb.SetStreamSource(i, pVB, Offset, Stride);
		}

		for (DWORD i = 2; i <= 3; i++)
		{
			D3DMATRIX matrix;
			CAPTURE(Transform, (D3DTRANSFORMSTATETYPE)i, &matrix);
		}
		for (DWORD i = 16; i <= 23; i++)
		{
			D3DMATRIX matrix;
			CAPTURE(Transform, (D3DTRANSFORMSTATETYPE)i, &matrix);
		}
		for (DWORD i = 256; i <= 256 /*511*/; i++)
		{
			D3DMATRIX matrix;
			CAPTURE(Transform, (D3DTRANSFORMSTATETYPE)i, &matrix);
		}

		for (DWORD i = 0; i < D3DCaps.MaxUserClipPlanes; i++)
		{
			float plane[4];
			CAPTURE(ClipPlane, i, plane);
		}

		D3DMATERIAL9 material;
		CAPTURE(Material, &material);
	}

#undef CAPTURE_VALUE
#undef CAPTURE_VALUE2
#undef CAPTURE
}
#endif

#pragma optimize( "y", off ) // for Steam Community

STDMETHODIMP CMonoRenderer::CreateAdditionalSwapChain(D3DPRESENT_PARAMETERS* pPresentationParameters, IDirect3DSwapChain9** ppSwapChain) 
{
	HRESULT hResult;
	NSCALL_TRACE1(m_Direct3DDevice.CreateAdditionalSwapChain(pPresentationParameters, ppSwapChain),
		DEBUG_MESSAGE(_T("CreateAdditionalSwapChain(pPresentationParameters: %s, *ppSwapChain = %p)"), 
		GetPresentationParametersString(pPresentationParameters), Indirect(ppSwapChain)));
#ifdef WRAP_ONLY_PRIMARY_SWAPCHAIN
	if (SUCCEEDED(hResult))
	{
		m_bUseMultipleSwapChains = TRUE;
		DUMP_CMD_ALL(CreateAdditionalSwapChain,pPresentationParameters,ppSwapChain);
	}
#else
	if (SUCCEEDED(hResult))
	{
		DUMP_CMD_ALL(CreateAdditionalSwapChain,pPresentationParameters,ppSwapChain);
		CMonoSwapChain* pSwapChain = CreateSwapChainWrapper(m_TotalSwapChainsCount);
		m_TotalSwapChainsCount++;
		DEBUG_MESSAGE(_T("Added SwapChain %d\n"), m_SwapChains.size());
		m_SwapChains.push_back(pSwapChain);
		pSwapChain->SetPresentationParameters(pPresentationParameters, NULL);
		pSwapChain->Initialize(*ppSwapChain);
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
		*ppSwapChain = pSwapChain;
		(*ppSwapChain)->AddRef();
	}
#endif
	return hResult;
}

STDMETHODIMP CMonoRenderer::GetSwapChain(UINT iSwapChain, IDirect3DSwapChain9** ppSwapChain) 
{
	HRESULT hResult;
	if (iSwapChain == 0)
	{
		*ppSwapChain = GetMonoSC(iSwapChain);
		(*ppSwapChain)->AddRef();
		hResult = S_OK;
	}
	else 
	{
		CComPtr<IDirect3DSwapChain9> pSwapChain;
		NSCALL(m_Direct3DDevice.GetSwapChain(iSwapChain, &pSwapChain));
		if (SUCCEEDED(hResult))
		{
#ifndef WRAP_ONLY_PRIMARY_SWAPCHAIN
			CMonoSwapChain* pSwapChainWrap = NULL;
			for (size_t i = 1; i < m_SwapChains.size(); i++) 
			{
				if (m_SwapChains[i]->m_SwapChain == pSwapChain)				
				{
					pSwapChainWrap = m_SwapChains[i];
					break;
				}
			}
			if (pSwapChainWrap)
			{
				pSwapChain.Release();
				pSwapChain = pSwapChainWrap;
			}
			else
			{
				DEBUG_MESSAGE(_T("SwapChain Wrapper not founded\n"));
			}
#endif
			pSwapChain.QueryInterface(ppSwapChain);
		}
	}
	NSCALL_TRACE2(hResult, DEBUG_MESSAGE(_T("GetSwapChain(iSwapChain = %u, *ppSwapChain = %p)\n"), 
		iSwapChain, Indirect(ppSwapChain)));

	IDirect3DSwapChain9 *pSwapChain = *ppSwapChain;
	DUMP_CMD(GetSwapChain,iSwapChain,pSwapChain);

	return hResult;
}

STDMETHODIMP CMonoRenderer::SetSamplerState(DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD Value)
{
	DUMP_CMD_STATE(SetSamplerState,Sampler,Type,Value);

	HRESULT hResult;
	NSCALL_TRACE2(m_Direct3DDevice.SetSamplerState(Sampler, Type, Value),
		DEBUG_MESSAGE(_T("SetSamplerState(Sampler = %02d, Type = %s, Value = %08X)"), 
		Sampler, GetSamplerStateTypeString(Type), Value));
	return hResult;
}

// IUnknown
HRESULT CMonoRenderer::QueryInterface( 
	REFIID riid,
	void** ppvObject)
{
	if( ppvObject == NULL ) {
		//DEBUG_TRACE3(_T("CMonoRenderer::QueryInterface() E_POINTER\n"));
		return E_POINTER;
	} 
	else if(InlineIsEqualGUID(riid, IID_IDirect3DDevice9)) {
		//DEBUG_TRACE3(_T("CMonoRenderer::QueryInterface(IID_IDirect3DDevice9) S_OK\n"));
		*ppvObject = static_cast<IDirect3DDevice9*>( this );
		AddRef();
		return S_OK;
	} 
	else if(InlineIsEqualGUID(riid, IID_IDirect3DDevice9Ex)) {
		if (m_Direct3DDevice.getExMode() == EXMODE_EX)
		{
			//DEBUG_TRACE3(_T("CMonoRenderer::QueryInterface(IID_IDirect3DDevice9Ex) S_OK\n"));
			*ppvObject = static_cast<IDirect3DDevice9Ex*>( this );
			AddRef();
			return S_OK;
		}
		else // disable for D3DX
		{
			//DEBUG_TRACE3(_T("CMonoRenderer::QueryInterface(IID_IDirect3DDevice9Ex) E_NOINTERFACE\n"));
			return E_NOINTERFACE;
		}
	} 
	else 
	{
		// {126D0349-4787-4AA6-8E1B-40C177C60A01} - CDXVA2Container (33 virtual functions)
		DEBUG_TRACE3(_T("CMonoRenderer::QueryInterface()\n"));
		return m_Direct3DDevice.QueryInterface(riid, ppvObject);
	}
}

ULONG CMonoRenderer::AddRef()
{
	//DEBUG_TRACE3(_T("CMonoRenderer::AddRef(), %d\n"), m_refCount);
	return InterlockedIncrement(& m_refCount);
}

ULONG CMonoRenderer::Release()
{
	//DEBUG_TRACE3(_T("CMonoRenderer::Release(), %d\n"), m_refCount);
	ULONG ret = InterlockedDecrement(& m_refCount);
	if( ret == 0 )
		delete this;
	return ret;
}

#pragma optimize( "y", on )
