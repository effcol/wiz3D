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
#include <map>
#include "BaseStereoRenderer.h"
#include "BackgroundPresenterThread.h"
#include "Trace.h"
#include "..\S3DAPI\EnumToString.h"
#include "..\S3DAPI\ReadData.h"
#include "..\S3DAPI\ShutterAPI.h"
#include "..\Shared\KatangaProducerDX9.h"   // 0.5.0 Katanga shared-texture producer (DX9 variant)
#include "..\Shared\NV3DOutputDX9.h"        // 0.5.0 NV3D-Lib shutter output (DX9 native backend)
#include "UMDDeviceWrapper.h"
#include "ProxyDirect9.h"
#include "CommandDumper.h"

bool CBaseStereoRenderer::IsStereoRenderTargetSurface( UINT Width, UINT Height )
{
	switch(gInfo.RenderTargetCreationMode)
	{
	case 0:
		return false;
	case 1:
		return true;
	default:
		return !gInfo.MonoRenderTargetSurfaces
			&& !(Width == 1 || Height == 1)
			&& !(gInfo.CreateSquareRTInMono && IsSquareSize(Width, Height, gInfo.CreateBigSquareRTInStereo))
			&& !(gInfo.CreateRTThatLessThanBBInMono && IsLessThanBB(Width, Height));
	}
}

bool CBaseStereoRenderer::IsStereoDepthStencilSurface( UINT Width, UINT Height )
{
	switch(gInfo.RenderTargetCreationMode)
	{
	case 0:
		return false;
	case 1:
		return true;
	default:
		return !gInfo.MonoDepthStencilSurfaces
			&& !(gInfo.CreateSquareDSInMono && IsSquareSize(Width, Height, gInfo.CreateBigSquareDSInStereo))
			&& !(gInfo.CreateDSThatLessThanBBInMono && IsLessThanBB(Width, Height));
	}
}

bool CBaseStereoRenderer::IsStereoRenderTargetTexture( D3DFORMAT Format, DWORD Usage, UINT Width, UINT Height )
{
	if (Format == FOURCC_NVCS)  // Some NVidia specific format
		return false;
	switch(gInfo.RenderTargetCreationMode)
	{
	case 0:
		return false;
	case 1:
		return true;
	default:
		bool CreateInStereo = !ShadowFormat(Format) && !(Usage & D3DUSAGE_DMAP) && !gInfo.MonoRenderTargetTextures;
		if (gInfo.CreateSquareRTInMono && IsSquareSize(Width, Height, gInfo.CreateBigSquareRTInStereo))
			CreateInStereo = false;
		else if (gInfo.CreateRTThatLessThanBBInMono && IsLessThanBB(Width, Height))
			CreateInStereo = false;
		if (Width == 1 || Height == 1) 
			CreateInStereo = false;
		return CreateInStereo;
	}
}

bool CBaseStereoRenderer::IsStereoDepthStencilTexture( UINT Width, UINT Height )
{
	switch(gInfo.RenderTargetCreationMode)
	{
	case 0:
		return false;
	case 1:
		return true;
	default:
		bool CreateInStereo = !gInfo.MonoDepthStencilTextures;
		if (gInfo.CreateSquareDSInMono && IsSquareSize(Width, Height, gInfo.CreateBigSquareDSInStereo))
			CreateInStereo = false;
		else if (gInfo.CreateDSThatLessThanBBInMono && IsLessThanBB(Width, Height))
			CreateInStereo = false;
		if (Width == 1 || Height == 1) 
			CreateInStereo = false;	
		return CreateInStereo;
	}
}

// 0.5.0: Katanga output -- single shared-texture producer for the DX9 wrapper
// (one game per process). Lazy-opened on first Present with OutputMode=Katanga.
static wiz3d::KatangaProducerDX9 g_KatangaDX9;

// 0.5.0: NV3D-Lib shutter output. Mirrors the Katanga producer's layout:
// we own a 2W x H wide-SBS tex (created with DX9's pSharedHandle out-param
// so the KMT handle crosses to NV3D-Lib's internal D3D9Ex device), fill
// its halves per frame from the eye RTs, and hand the handle to NV3D-Lib.
static wiz3d::NV3DOutputDX9  g_NV3DDX9;
static IDirect3DTexture9*    g_NV3DSBSTex      = nullptr;
static IDirect3DSurface9*    g_NV3DSBSSurf     = nullptr;   // level-0 surface
static HANDLE                g_NV3DSBSHandle   = nullptr;   // KMT handle
static UINT                  g_NV3DSBSW        = 0;         // == eyeW * 2
static UINT                  g_NV3DSBSH        = 0;
static D3DFORMAT             g_NV3DSBSFmt      = D3DFMT_UNKNOWN;

static void ReleaseNV3DSBS()
{
    if (g_NV3DSBSSurf) { g_NV3DSBSSurf->Release(); g_NV3DSBSSurf = nullptr; }
    if (g_NV3DSBSTex)  { g_NV3DSBSTex->Release();  g_NV3DSBSTex  = nullptr; }
    g_NV3DSBSHandle = nullptr;
    g_NV3DSBSW = g_NV3DSBSH = 0;
    g_NV3DSBSFmt = D3DFMT_UNKNOWN;
}

static bool EnsureNV3DSBS(IDirect3DDevice9* dev, UINT eyeW, UINT eyeH, D3DFORMAT fmt)
{
    const UINT wantW = eyeW * 2;
    if (g_NV3DSBSTex && g_NV3DSBSW == wantW && g_NV3DSBSH == eyeH && g_NV3DSBSFmt == fmt)
        return true;
    ReleaseNV3DSBS();
    HANDLE h = nullptr;
    HRESULT hr = dev->CreateTexture(wantW, eyeH, 1, D3DUSAGE_RENDERTARGET, fmt,
                                     D3DPOOL_DEFAULT, &g_NV3DSBSTex, &h);
    if (FAILED(hr) || !g_NV3DSBSTex || !h) { ReleaseNV3DSBS(); return false; }
    if (FAILED(g_NV3DSBSTex->GetSurfaceLevel(0, &g_NV3DSBSSurf))) { ReleaseNV3DSBS(); return false; }
    g_NV3DSBSHandle = h;
    g_NV3DSBSW      = wantW;
    g_NV3DSBSH      = eyeH;
    g_NV3DSBSFmt    = fmt;
    return true;
}

HRESULT CBaseStereoRenderer::DoPresent( CBaseSwapChain* pSwapChain, HWND hDestWindowOverride, CONST RECT * pSourceRect, CONST RECT * pDestRect,
									   CONST RGNDATA * pDirtyRegion, DWORD dwFlags, bool bExMode )
{
	INSIDE;
	RouterTypeHookCallGuard<ProxyDirect9> RTdir(GetProxyDirect3D9());
	RouterTypeHookCallGuard<ProxyDevice9> RTdev(&m_Direct3DDevice);

	HRESULT hrPresent = D3DERR_INVALIDCALL;
	if (m_bRenderScene) {
		DEBUG_TRACE3(_T("ERROR: EndScene() not called before Present()\n"));
		return hrPresent;
	}

	// 0.5.0 Katanga publish. OutputMode==12 (OUTMODE_KATANGA) -> hand both eye
	// surfaces to Katanga's shared-handle producer. Normal composite path
	// still runs afterwards so the user also sees SBS on the real BB. If
	// Katanga isn't running, TryInit() returns false and this is a no-op.
	if (gInfo.OutputMode == 12 && pSwapChain && pSwapChain->IsStereoActive())
	{
		IDirect3DSurface9* leftRT  = pSwapChain->GetViewRT(TRUE);
		IDirect3DSurface9* rightRT = pSwapChain->GetViewRT(FALSE);
		if (leftRT && rightRT && g_KatangaDX9.TryInit())
		{
			IDirect3DDevice9* realDev = m_Direct3DDevice.GetReal();
			if (!g_KatangaDX9.IsReady())
			{
				D3DSURFACE_DESC bbd = {};
				leftRT->GetDesc(&bbd);
				g_KatangaDX9.OnBackbufferReady(realDev, bbd.Width, bbd.Height, bbd.Format);
			}
			g_KatangaDX9.PublishFrame(realDev, leftRT, rightRT, gInfo.Input.SwapEyes != 0);
		}
	}

	// 0.5.0 NV3D-Lib shutter publish. OutputMode==11 (OUTMODE_3DVISION_GLASSES).
	// Owns a wide-SBS 2W x H texture whose KMT shared handle crosses directly
	// into NV3D-Lib's internal D3D9Ex device -- no DX11 bridge needed. StretchRect
	// per-eye halves into the shared tex, then NV3D-Lib's AUTOMATIC-mode signature
	// path drives shutter output. Falls through: the normal on-screen composite
	// still runs below so the user sees SBS whether or not 3D Vision hardware
	// is present.
	if (gInfo.OutputMode == 11 && pSwapChain && pSwapChain->IsStereoActive())
	{
		IDirect3DSurface9* leftRT  = pSwapChain->GetViewRT(TRUE);
		IDirect3DSurface9* rightRT = pSwapChain->GetViewRT(FALSE);
		IDirect3DDevice9*  realDev = m_Direct3DDevice.GetReal();
		if (leftRT && rightRT && realDev)
		{
			D3DSURFACE_DESC eyed = {};
			leftRT->GetDesc(&eyed);
			if (EnsureNV3DSBS(realDev, eyed.Width, eyed.Height, eyed.Format))
			{
				const bool swap = (gInfo.Input.SwapEyes != 0);
				LONG halfW = (LONG)eyed.Width;
				RECT dstL = { 0,     0, halfW,                     (LONG)eyed.Height };
				RECT dstR = { halfW, 0, (LONG)g_NV3DSBSW,          (LONG)eyed.Height };
				IDirect3DSurface9* srcL = swap ? rightRT : leftRT;
				IDirect3DSurface9* srcR = swap ? leftRT  : rightRT;
				realDev->StretchRect(srcL, NULL, g_NV3DSBSSurf, &dstL, D3DTEXF_POINT);
				realDev->StretchRect(srcR, NULL, g_NV3DSBSSurf, &dstR, D3DTEXF_POINT);
				if (!g_NV3DDX9.IsReady())
					g_NV3DDX9.TryInit(realDev, NULL, swap);
				if (g_NV3DDX9.IsReady())
					g_NV3DDX9.Present(g_NV3DSBSHandle, g_NV3DSBSW, g_NV3DSBSH, g_NV3DSBSFmt);
			}
		}
	}

	HRESULT hResult = S_OK;
	hrPresent = S_OK;

	D3DPERF_BeginEvent(	D3DCOLOR_XRGB( 0, 255, 0 ), L"Before present processing");
#ifndef FINAL_RELEASE
	if(IsKeyPressed(VK_OEM_PERIOD))
		m_pOutputMethod->SetNextSubMode();
#endif

	pSwapChain->DoScaling();

	D3DVIEWPORT9 Viewport = m_ViewPort;
	RECT ScissorRect = m_ScissorRect;											// Rectangle for scissoring
	vbs* vb = NULL;																// Vertex buffer array pointer
	InitViewport(&m_ViewPort, 0, 0, pSwapChain->m_BackBufferSize.cx, pSwapChain->m_BackBufferSize.cy);	// Viewport Initialization
	SetRect(&m_ScissorRect, 0, 0, pSwapChain->m_BackBufferSize.cx, pSwapChain->m_BackBufferSize.cy);	// Conf rectangle for scissor
	CaptureAll(vb);

#ifndef DISABLE_WIZARD
	pSwapChain->m_WizardData.ProcessKeys();
#endif
	// TODO: Move to CaptureAll
	std::vector<CAdapt<CComPtr<IDirect3DSurface9> > > pRenderTarget(m_dwNumSimultaneousRTs);
	pRenderTarget[0].m_T = m_RenderTarget[0].m_pMainSurf;
	for (DWORD i = 1; i < m_dwNumSimultaneousRTs; i++)						// through all render targets
		if (m_RenderTarget[i].m_pMainSurf)									// if render target has left surf
		{
			pRenderTarget[i].m_T = m_RenderTarget[i].m_pMainSurf;			// hold reference
			m_Direct3DDevice.SetRenderTarget(i, NULL);					// sets new color buffer for the device
		}
	CComPtr<IDirect3DSurface9> pDepthStencilSurface;
	if (m_DepthStencil.m_pMainSurf)											// if depth stencil has left surf
	{
		pDepthStencilSurface = m_DepthStencil.m_pMainSurf;					// hold reference
		m_Direct3DDevice.SetDepthStencilSurface(NULL);					// disable depth stencil operation
	}

	pSwapChain->m_hDestWindowOverride = hDestWindowOverride;							// destination window override

	//--- update frame statistics ---   
	UpdateFrameStatistic(pSwapChain);
	float fFrameTimeDelta = float(pSwapChain->m_nFrameTimeDelta.QuadPart) / float(m_nFreq.QuadPart);

	//--- optimization ---
	//--- do not check m_AutoFocus.IsInitializedSuccessfully() because enough ---
	//--- checking of m_AutoFocus.IsCurrentShiftValid() flag in this case     ---
	//--- we can use valid shift value regardless input changing ---
	if(m_Input.StereoActive && pSwapChain->m_ShiftFinder.IsCalculationComplete() && pSwapChain->m_ShiftFinder.IsCurrentShiftValid())
	{
		//--- we can use m_ShiftFinder data here ---
		if(m_Input.LaserSightEnable)
			pSwapChain->m_LaserSightData.SetShift(pSwapChain->m_ShiftFinder.GetCurrentShift(), gInfo.SeparationMode);
	}

	gKbdHook.m_Access.Enter(); // one thread is beginning
#ifndef DISABLE_WIZARD
	if (pSwapChain->m_WizardData.Force3D())
	{
		gInfo.Input.StereoActive = true;
	}
#endif
	DataInput updatedInput = gInfo.Input;
	gKbdHook.m_Access.Leave(); 

	if (m_Input.IsChanged( updatedInput ))
	{
		m_nWriteSettingsAfter.QuadPart	= 5 * m_nFreq.QuadPart; // 5 sec
		m_nOSDShowTimeLeft.QuadPart		= 2 * m_nFreq.QuadPart;	// 2 sec

		//--- first, we must render new views, then calculate shift ---
		pSwapChain->m_ShiftFinder.SetShiftToHoldInvalid(pSwapChain->m_nPresentCounter);
	}
	else
	{
		//--- optimization ---
		//--- do not check pSwapChain->m_ShiftFinder.IsInitializedSuccessfully() because enough ---
		//--- checking of pSwapChain->m_ShiftFinder.IsCurrentShiftValid() flag in this case     ---
		if(m_Input.StereoActive && pSwapChain->m_ShiftFinder.IsCalculationComplete() && pSwapChain->m_ShiftFinder.IsCurrentShiftValid())
		{
			if(m_Input.GetActivePreset()->AutoFocusEnable)
			{
				gKbdHook.m_Access.Enter(); // one thread is beginning
				m_AutoFocus.UpdateZPS(&pSwapChain->m_ShiftFinder, gInfo.Input.GetActivePreset());
				updatedInput = gInfo.Input;
				gKbdHook.m_Access.Leave(); 
			}

			//--- we can use m_ShiftFinder data here ---
			if(m_Input.LaserSightEnable)
				pSwapChain->m_LaserSightData.SetShift(pSwapChain->m_ShiftFinder.GetCurrentShift(), gInfo.SeparationMode);
		}
		if (m_nWriteSettingsAfter.QuadPart > 0)
		{
			if (m_nWriteSettingsAfter.QuadPart > m_nGlobalFrameTimeDelta.QuadPart)
				m_nWriteSettingsAfter.QuadPart -= m_nGlobalFrameTimeDelta.QuadPart;
			else
			{
				WriteInputData(&m_Input);
				m_nWriteSettingsAfter.QuadPart = 0;
			}
		}
	}

	HRESULT hrTCL = m_Direct3DDevice.TestCooperativeLevel();
	if (hrTCL == D3D_OK)
	{
		//save
		SurfaceData surfData = m_RenderTarget[0];
		CONST SIZE* pOffset = m_pCurOffset;
		BOOL bSVP = m_Direct3DDevice.GetSoftwareVertexProcessing();

		// set BB
		m_RenderTarget[0].Init(pSwapChain->m_pLeftBackBuffer);
		m_pCurOffset = &pSwapChain->m_BBOffset;
		m_Direct3DDevice.SetRenderTarget(0, m_RenderTarget[0].m_pMainSurf);	
		m_Direct3DDevice.SetViewport(&m_ViewPort);								// Setting view port for device
		m_Direct3DDevice.SetScissorRect(&m_ScissorRect);							// Set scissor rectangle

		if (bSVP)
			m_Direct3DDevice.SetSoftwareVertexProcessing(FALSE); // RE4

		NSCALL(m_Direct3DDevice.BeginScene());

		if (gInfo.MakeScreenshot) // internal API for autotests
		{
			pSwapChain->WriteJPSScreenshot(false);
			gInfo.MakeScreenshot = FALSE; // single-thread only
			if (gInfo.MakeScreenshotCallback) gInfo.MakeScreenshotCallback();
		}

		if (gInfo.ExtractDepthBuffer)
			pSwapChain->ProcessDepthBuffer();

		pSwapChain->DrawOSD(fFrameTimeDelta);

		if(m_ScreenShotButton.isButtonWasPressed())
		{
			D3DPERF_BeginEvent(	D3DCOLOR_XRGB( 0, 0, 255 ), L"Write JPS");
			bool applyGammaCorrection = !IsKeyDown(VK_CONTROL);
			pSwapChain->WriteJPSScreenshot(applyGammaCorrection);
			D3DPERF_EndEvent();
		}

		//--- draw text ---
		D3DPERF_BeginEvent(	D3DCOLOR_XRGB( 0, 0, 255 ), L"DrawInfo");
		DrawInfo(pSwapChain);
		D3DPERF_EndEvent();	
		
		DrawMouseCursor(pSwapChain);

		DrawLogo(pSwapChain);

		NSCALL(m_Direct3DDevice.EndScene());

		// restore
		m_RenderTarget[0] = surfData;
		m_pCurOffset = pOffset;
		m_Direct3DDevice.SetRenderTarget(0, m_RenderTarget[0].m_pMainSurf);
		if (bSVP)
			m_Direct3DDevice.SetSoftwareVertexProcessing(TRUE);
	}

	pSwapChain->SetPresentParams(pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion, dwFlags);
	hrPresent = pSwapChain->PresentData();

	DEBUG_TRACE2(_T("OrigPresent(pSourceRect = %s, pDestRect = %s, hDestWindowOverride = %p, pDirtyRegion = %p, dwFlags = %d) = %s\n"), 
		GetRectString(pSourceRect), GetRectString(pDestRect), hDestWindowOverride, pDirtyRegion, dwFlags, DXGetErrorString(hrPresent));

	if (IS_SHUTTER_OUTPUT && !USE_ATI_PRESENTER && !USE_SIMPLE_PRESENTER && !USE_IZ3DKMSERVICE_PRESENTER)
	{
		LARGE_INTEGER time;
		QueryPerformanceCounter(&time);
		SetNextFlushTime(time);
	}

	pSwapChain->m_pLeftDepthStencilCorrectTexture.Release();
	pSwapChain->m_pRightDepthStencilCorrectTexture.Release();

	// update driver data
	m_bStereoModeChanged = (updatedInput.StereoActive != m_Input.StereoActive);
	if (m_bStereoModeChanged)
	{
		StereoModeChanged(updatedInput.StereoActive != 0);
		//if( USE_IZ3DKMSERVICE_PRESENTER && IsFullscreen() )
		//{
		//	if( updatedInput.StereoActive )
		//		gInfo.StartShutter();
		//	else
		//		gInfo.StopShutter();
		//}
	}
	bool bUpdateProjectionMatrix = false;
	if (updatedInput.StereoActive && (!m_Input.StereoActive || m_Input.IsStereoParamChanged( updatedInput )))
		bUpdateProjectionMatrix = true;

	m_Input = updatedInput;
	gInfo.ShowFPS = m_Input.ShowFPS;

	// should be after updating input data
	if (bUpdateProjectionMatrix)
		UpdateProjectionMatrix();

	//--- restore states ---
	m_ViewPort = Viewport;
	m_ScissorRect = ScissorRect;
	ApplyAll(vb);

	D3DPERF_EndEvent();

	return hrPresent;
}

void CBaseStereoRenderer::FlushCommands( )
{
	if (gInfo.PresenterSleepTime != -2)
		return;
	
	if (!m_Input.StereoActive || GetBaseSC()->m_PresentationParameters[0].Windowed)
		return;

	LARGE_INTEGER time;
	QueryPerformanceCounter(&time);
	if (time.QuadPart >= m_FlushTime.QuadPart)
	{
		HRESULT hr;
		hr = m_pFlushQuery->Issue(D3DISSUE_END);
		hr = m_pFlushQuery->GetData( NULL, 0, D3DGETDATA_FLUSH );
		m_nGetDataCounter++;
		while(S_FALSE == hr)
		{
			Sleep(0);
			hr = m_pFlushQuery->GetData( NULL, 0, D3DGETDATA_FLUSH );
			m_nGetDataCounter++;
		}
		m_nFlushCounter++;

		QueryPerformanceCounter(&time);
		SetNextFlushTime(time);
	}
}

bool CBaseStereoRenderer::SkipPresenterThread()
{
	if (!USE_UM_PRESENTER)
		return false;
#ifndef FINAL_RELEASE
	if (m_bDebugUseSimplePresenter)
		return true;
	if (DO_RTDUMP)
		return false;
#endif
	if (!m_Input.StereoActive)
		return true;
	return false;
}

void CBaseStereoRenderer::SetNextFlushTime( LARGE_INTEGER &time )
{
	m_FlushTime.QuadPart = time.QuadPart + (LONGLONG)(m_nFreq.QuadPart * m_PresenterFlushTime / 1000);
}

void CBaseStereoRenderer::StereoModeChanged(bool bNewMode)
{
	if (m_VSPipelineData.m_bShaderDataAvailable && m_VSPipelineData.m_CurrentShaderData.stereoShader) 
	{
		IDirect3DVertexShader9* pShader = bNewMode ? 
			m_VSPipelineData.m_CurrentShaderData.stereoShader : m_VSPipelineData.m_CurrentShader;
		m_Direct3DDevice.SetVertexShader(pShader);
	}
	if (m_PSPipelineData.m_bShaderDataAvailable && m_PSPipelineData.m_CurrentShaderData.stereoShader) 
	{
		IDirect3DPixelShader9* pShader = bNewMode ? 
			m_PSPipelineData.m_CurrentShaderData.stereoShader : m_PSPipelineData.m_CurrentShader;
		m_Direct3DDevice.SetPixelShader(pShader);
	}
	m_pOutputMethod->StereoModeChanged(bNewMode);
}

void CBaseStereoRenderer::UpdateFrameStatistic( CBaseSwapChain* pSwapChain )
{
	LARGE_INTEGER nCurrentTime;    
	QueryPerformanceCounter(&nCurrentTime);
	pSwapChain->m_nFrameTimeDelta.QuadPart = nCurrentTime.QuadPart - pSwapChain->m_nLastFrameTime.QuadPart;
	pSwapChain->m_nLastFrameTime = nCurrentTime;
	// time of fps drop show
	pSwapChain->m_nFPSDropShowTimeLeft.QuadPart = std::max<LONGLONG>(pSwapChain->m_nFPSDropShowTimeLeft.QuadPart - pSwapChain->m_nFrameTimeDelta.QuadPart, 0);

	m_nGlobalFrameTimeDelta.QuadPart = nCurrentTime.QuadPart - m_nGlobalLastFrameTime.QuadPart;
	m_nGlobalLastFrameTime = nCurrentTime;
	// time of showing time left
	m_nOSDShowTimeLeft.QuadPart	= std::max<LONGLONG>(m_nOSDShowTimeLeft.QuadPart - m_nGlobalFrameTimeDelta.QuadPart, 0);

	m_fCurSessionTime += m_nGlobalFrameTimeDelta.QuadPart / 60.0f / m_nFreq.QuadPart;
	if (m_bStereoModeChanged)
		UpdateStatisticData();
}

HRESULT CBaseStereoRenderer::DoReset( D3DPRESENT_PARAMETERS* pPresentationParameters, D3DDISPLAYMODEEX * pFullscreenDisplayMode, bool bExMode )
{
	HRESULT hResult = S_OK;
	
	if (m_bEngineStarted)
		StopEngine();

	UnhookDevice();
	if (!m_Direct3DDevice)
	{
		DEBUG_MESSAGE(_T("%s: m_pDirect3DDevice = NULL.\n"), _T( __FUNCTION__ ) );
		return E_FAIL;
	}
#ifndef FINAL_RELEASE
	else
	{
		ULONG nRefCount = 0;
		m_Direct3DDevice.AddRef();
		nRefCount = m_Direct3DDevice.Release();
		DEBUG_MESSAGE(_T("%s: nRefCount = %d\n"), _T( __FUNCTION__ ) , nRefCount);
	}
#endif
	
	NSPTR(m_Direct3DDevice);
	NSPTR(pPresentationParameters);
	BOOL bWindowed = GetBaseSC()->m_PresentationParameters[0].Windowed;
	if (gInfo.OutputSpanMode && 
		pPresentationParameters->BackBufferWidth == GetBaseSC()->m_PresentationParameters[0].BackBufferWidth &&
		pPresentationParameters->BackBufferHeight == GetBaseSC()->m_PresentationParameters[0].BackBufferHeight)
	{
		pPresentationParameters->BackBufferWidth = GetMonoSC()->m_OriginalPresentationParameters.BackBufferWidth;
		pPresentationParameters->BackBufferHeight = GetMonoSC()->m_OriginalPresentationParameters.BackBufferHeight;
	}
	GetMonoSC()->SetPresentationParameters(pPresentationParameters, pFullscreenDisplayMode);

	if (m_DeviceMode == DEVICE_MODE_AUTO && GetBaseDevice()->m_bTwoWindows)
	{
		D3DPRESENT_PARAMETERS& pp = *pPresentationParameters;
		if (((pp.BackBufferWidth > 1 || pp.BackBufferHeight > 1) ||
			(pp.Windowed && pp.BackBufferWidth == 0 && pp.BackBufferHeight == 0)))
		{
			DEBUG_MESSAGE(_T("%s\n"), GetDeviceModeString(m_DeviceMode));
			if (pp.Windowed)
				GetBaseDevice()->m_DeviceMode = DEVICE_MODE_FORCEWINDOWED;
			else
				GetBaseDevice()->m_DeviceMode = DEVICE_MODE_FORCEFULLSCREEN;
			DEBUG_MESSAGE(_T("%s\n"), GetDeviceModeString(GetBaseDevice()->m_DeviceMode));
		}
	}

	GetBaseSC()->ModifyPresentParameters();  
	if (USE_ATI_PRESENTER)
		m_pATICommSurface = 0;
	
	if (!bExMode)
	{
		if (USE_MULTI_THREAD_PRESENTER) 
		{
			m_pPresenterThread->SetAction( ptReset );
			NSCALL_TRACE1(m_pPresenterThread->WaitForActionDone(),
				DEBUG_MESSAGE(_T("m_pPresenterThread->WaitForActionDone()")));
		}
		else 
		{
			CBaseSwapChain* pSwapChain = GetBaseSC();
			D3DPRESENT_PARAMETERS* pPres = &pSwapChain->m_PresentationParameters[0];
			if (USE_ATI_PRESENTER && !(pPres->Windowed))
			{
				pPres->Windowed = TRUE;
				UINT hz = pPres->FullScreen_RefreshRateInHz;
				pPres->FullScreen_RefreshRateInHz = 0;
				{
					SetUMEvent UMEvent(this, reResetingDevice);
					NSCALL_TRACE1(m_Direct3DDevice.Reset(pPres), DEBUG_MESSAGE(_T("Reset()")));
				}
				if( FAILED( hResult ) )
				{
					SetUMEvent UMEvent(this, reResetingDevice);
					NSCALL_TRACE1(m_Direct3DDevice.Reset(pPres), DEBUG_MESSAGE(_T("Reset()")));
				}
				if( SUCCEEDED( hResult ) )
				{
					SetUMEvent UMEvent(this, reCreatingAQBS);
					NSCALL(m_Direct3DDevice.CreateOffscreenPlainSurface(
						10, 10, (D3DFORMAT)FOURCC_AQBS, D3DPOOL_DEFAULT,
						&m_pATICommSurface, NULL));
					SendStereoCommand(ATI_STEREO_ENABLESTEREO, 0, 0, 0, 0);
					m_pATICommSurface = 0;
				}
				pPres->Windowed = FALSE;
				pPres->FullScreen_RefreshRateInHz = hz;
			}
			{
				SetUMEvent UMEvent(this, reResetingDevice);
				NSCALL_TRACE1(m_Direct3DDevice.Reset(pPres), DEBUG_MESSAGE(_T("Reset()")));
			}
		}
	}
	else
	{
		SetUMEvent UMEvent(this, reResetingDevice);
		NSCALL_TRACE1(m_Direct3DDevice.ResetEx(&GetBaseSC()->m_PresentationParameters[0], 
			GetBaseSC()->m_pFullscreenDisplayMode), DEBUG_MESSAGE(_T("ResetEx()")));
	}

	if(USE_IZ3DKMSERVICE_PRESENTER) 
	{
		//--- we need to call StopShutter(), but for some reason video driver crash if we do it BEFORE Reset() in StopEngie() ---
		g_KMShutter.StopShutter();
	}

	HRESULT hSavedResult = hResult;
	HookDevice();
	if (FAILED(hResult)) {
		// we should ignore Reset fail result code, sometimes return D3DERR_INVALIDCALL when device OK
		NSCALL(m_Direct3DDevice.TestCooperativeLevel());
		if (SUCCEEDED(hResult)) {
			DEBUG_MESSAGE(_T("Ignoring Reset() result code\n"));
		}
	}
	if (SUCCEEDED(hResult)) { 
		if (gInfo.OutputCaps & odShutterMode)
		{
			if (USE_MULTI_DEVICE_PRESENTER) {
				m_pPresenterThread->SetAction(ptReset);
				m_pPresenterThread->WaitForActionDone();
			} else if (USE_ATI_PRESENTER || USE_UM_PRESENTER) 
			{
				if (USE_ATI_PRESENTER && !m_pATICommSurface)
				{
					SetUMEvent UMEvent(this, reCreatingAQBS);
					NSCALL(m_Direct3DDevice.CreateOffscreenPlainSurface( 10, 10, (D3DFORMAT)FOURCC_AQBS, D3DPOOL_DEFAULT, &m_pATICommSurface, NULL));
				}
				SetupUMPresenter(); 
			}
		}
		NSCALL(StartEngine());
	} else if (bWindowed) {	
		GetBaseSC()->SendOrPostMessage(SWM_HIDE_SECOND_WINDOW);
	}

	return hSavedResult;
}

void CBaseStereoRenderer::SetupUMPresenter()
{
	if (m_pATICommSurface)
		GetATILineOffset();
}

UINT CBaseStereoRenderer::GetShutterRefreshRate(UINT appRefreshRate)
{
	int PreferedRefreshRate = 0;
	if (m_pOutputMethod)
		PreferedRefreshRate = m_pOutputMethod->GetPreferredRefreshRate();
	if (PreferedRefreshRate == 0)
		PreferedRefreshRate = gInfo.ShutterRefreshRate;
	if (PreferedRefreshRate == 0)
	{
		DEBUG_MESSAGE(_T("GetShutterRefreshRate = %i\n"), appRefreshRate);
		return appRefreshRate;
	}

	D3DDISPLAYMODE Mode;
	HRESULT hr;
	UINT width;
	UINT height;
	D3DFORMAT fmt = GetBaseSC()->m_PresentationParameters[0].BackBufferFormat;
	if (GetBaseSC()->m_PresentationParameters[0].Windowed)
	{
		m_pDirect3D->GetAdapterDisplayMode(m_nAdapter[0], &Mode);
		width = Mode.Width;
		height = Mode.Height;
	}
	else
	{
		width = GetBaseSC()->m_PresentationParameters[0].BackBufferWidth;
		height = GetBaseSC()->m_PresentationParameters[0].BackBufferHeight;
	}
	switch(fmt)
	{
	case D3DFMT_A8R8G8B8:
	case D3DFMT_UNKNOWN:
		fmt = D3DFMT_X8R8G8B8;
		break;
	case D3DFMT_A1R5G5B5:
		fmt = D3DFMT_R5G6B5;
		break;
	}
	int RefreshRate = 0;
	UINT num = m_pDirect3D->GetAdapterModeCount(m_nAdapter[0], fmt);
	if (num == 0) {
		DEBUG_MESSAGE(_T("WARNING: Format %s unsupported like adapter format\n"), GetFormatString(fmt));

		m_pDirect3D->GetAdapterDisplayMode(m_nAdapter[0], &Mode);
		width = Mode.Width;
		height = Mode.Height;
		fmt = Mode.Format;
		num = m_pDirect3D->GetAdapterModeCount(m_nAdapter[0], fmt);
	}
	for (UINT i = 0; i < num; i++)
	{
		hr = m_pDirect3D->EnumAdapterModes(m_nAdapter[0], fmt, i, &Mode);
		if (width == Mode.Width && height == Mode.Height)
			RefreshRate = std::max(RefreshRate, (int)Mode.RefreshRate);
	}
	DEBUG_MESSAGE(_T("Maximum refresh rate: %d\n"), RefreshRate);

	if (PreferedRefreshRate > 0)
	{
		if (RefreshRate >= PreferedRefreshRate)
			RefreshRate = PreferedRefreshRate;
		else {
			DEBUG_MESSAGE(_T("WARNING: Config refresh rate bigger than maximum\n"));
		}
	}
	DEBUG_MESSAGE(_T("GetShutterRefreshRate = %i\n"), RefreshRate);
	return RefreshRate;
}

void CBaseStereoRenderer::UpdateTexturesType(TextureType textureType) 
{
	if (GINFO_CLEARBEFOREEVERYDRAW && textureType == Mono)
	{
		HRESULT hResult = S_OK;
		//if (m_DepthStencil.pLeftSurf)
		//{
		//	hResult = m_pDirect3DDevice.Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0x00000000, 1.0f, 0);
		//	if (FAILED(hResult))
		//		hResult = m_pDirect3DDevice.Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0x00000000, 1.0f, 0);
		//} else
			hResult = m_Direct3DDevice.Clear(0, NULL, D3DCLEAR_TARGET, 0x00000000, 1.0f, 0);
	}
	if (m_dwZWriteEnable != 0 || TRUE)
		m_DepthStencil.SetSurfaceType(textureType);
	if (m_dwColorWriteEnable[0])
		m_RenderTarget[0].SetType(textureType);
	if (m_dwColorWriteEnable[1])
		m_RenderTarget[1].SetType(textureType);
	if (m_dwColorWriteEnable[2])
		m_RenderTarget[2].SetType(textureType);
	if (m_dwColorWriteEnable[3])
		m_RenderTarget[3].SetType(textureType);
}

HRESULT CBaseStereoRenderer::SetMonoModifiedShaderConstants( )
{
	HRESULT hResult = S_OK;
	if (gInfo.VertexShaderModificationMethod < 2)
	{
		FLOAT shiftVector[4] = {1, 0, 0, 0};
		m_Direct3DDevice.SetVertexShaderConstantF(m_VSPipelineData.m_CurrentShaderData.dp4VectorRegister, shiftVector, 1);
		if (m_VSPipelineData.m_CurrentShaderData.ZNearRegister != 65535)
		{
			FLOAT ZNearVector[4] = {0.0f, 0.0f, 0.0f, 0.0f};
			m_Direct3DDevice.SetVertexShaderConstantF(m_VSPipelineData.m_CurrentShaderData.ZNearRegister, ZNearVector , 1);
		}
	}
	else
	{
		FLOAT vector[4] = {0.0f, 0.0f, 0.0f, 0.0f};
		m_Direct3DDevice.SetVertexShaderConstantF(m_VSPipelineData.m_CurrentShaderData.dp4VectorRegister, vector, 1);
	}
	return hResult;
}

bool CBaseStereoRenderer::CheckDrawPassSplitting( D3DPRIMITIVETYPE PrimitiveType, UINT PrimitiveCount )
{
#ifdef	FINAL_RELEASE
	//return	false;
#endif

	if( !(m_PSPipelineData.m_CurrentShader &&
		m_PSPipelineData.m_CurrentShaderData.splitThisShaderDrawPass
#ifndef FINAL_RELEASE
		&& m_bDebugUseDrawPassSplitting
#endif
		))
		return	false;

	// check for fullscreen pass with just one or two triangles
	if( !((PrimitiveCount <= 2) &&
		((PrimitiveType == D3DPT_TRIANGLELIST) || (PrimitiveType == D3DPT_TRIANGLESTRIP) || (PrimitiveType == D3DPT_TRIANGLEFAN))))
		return	false;

	// check viewport size
	if( !((m_ViewPort.Width	== GetBaseSC()->m_PresentationParameters[0].BackBufferWidth)	&&
		(m_ViewPort.Height	== GetBaseSC()->m_PresentationParameters[0].BackBufferHeight)))
		return	false;

	// check scissor state & size
	if( !((m_dwScissorTestEnable == FALSE) ||
		(
		(m_dwScissorTestEnable == TRUE) &&		// N.B. Check this code to work okay with WideRenderTarget
		(m_ScissorRect.left	  == 0)	&&
		(m_ScissorRect.top	  == 0)	&&
		(m_ScissorRect.right  == m_ViewPort.Width)	&&
		(m_ScissorRect.bottom == m_ViewPort.Height))
		))
		return	false;

	return	true;
}

bool CBaseStereoRenderer::PrepareViewStages(bool bFromMemory) 
{
	INSIDE;
	m_Pipeline = Fixed;
	m_bRenderInStereo = FALSE;
	m_bRenderInStereoPS = FALSE;
	m_pUpdateRTs = m_dwColorWriteEnable;
	m_UpdateDS = true;
	m_bUsedStereoTextures = false;
	m_CurrentMeshMultiplier = 1.f;
	m_CurrentConvergenceShift = 0.f;
	m_CurrentPSMultiplier = 1.f;
	m_CurrentPSConvergenceShift = 0.f;
	m_ZNear = 0.f;
	m_ZFar = 0.f;
	gInfo.LastDrawStereo = false;

	if (!m_bEngineStarted)
		return FALSE;

	if(m_bRHWVertexDeclaration)
		m_Pipeline = gInfo.RHWModificationMethod == 1 ?  RHW : RHWEmul; // Should be set even in mono
	else if (m_VSPipelineData.m_CurrentShader)
		m_Pipeline = Shader;

	ViewStagesSynchronization();
	
	if ( m_Pipeline == RHWEmul )
	{
		HRESULT hResult = S_OK;
		CComPtr<IDirect3DVertexDeclaration9> pDecl;
		NSCALL(m_Direct3DDevice.GetVertexDeclaration(&pDecl));
		m_RHWEmulData = GetRHWEmulData(pDecl);
		NSCALL(m_Direct3DDevice.SetVertexDeclaration(m_RHWEmulData->second.pDecl));
		NSCALL(m_Direct3DDevice.SetVertexShader(m_RHWEmulData->second.pVS));
		UpdateRHWMatrix();
	}

	if (!RenderToRight())
	{
		// Render to one RT
		return FALSE;
	}

	if (!RenderInStereo())
	{
		// Render to one RT
		return FALSE;
	}

	// Render to 2 RT

#ifndef FINAL_RELEASE
	if (!StereoDS()) // for depth/stencil textures
	{
		DEBUG_TRACE3(_T("\tWarning: mono depth stencil surface!\n"));
		m_bDebugWarningMonoDepthStencil = TRUE;
	}
#endif

	if (!m_Input.StereoActive)
	{
		DEBUG_TRACE3(_T("\tRender to right in mono\n"));
		return !DEBUG_RENDER_IN_ONE_RT; // TRUE
	}

	if (gInfo.DrawType == 0)
	{
		DEBUG_TRACE3(_T("\tMono draw (API)\n"));
		return !DEBUG_RENDER_IN_ONE_RT; // TRUE
	}

	if (gInfo.DrawType == 2 && bFromMemory && gInfo.MonoDrawFromMemory)
	{
		DEBUG_TRACE3(_T("\tMono draw from memory\n"));
		return !DEBUG_RENDER_IN_ONE_RT; // TRUE
	}

	// Second part: no return after that

	switch(m_Pipeline)
	{
	case Shader:
		m_bRenderInStereo = CheckRenderInStereoVSPipeline();
		break;
	case Fixed:
		m_bRenderInStereo = CheckRenderInStereoFixedPipeline();
		m_ZNear = m_FixedZNear;
		m_ZFar = m_FixedZFar;
		break;
	case RHW:
	case RHWEmul:
		if (!DEBUG_RENDER_IN_MONO)
			m_bRenderInStereo = true;
		m_ZNear = m_FixedZNear;
		m_ZFar = m_FixedZFar;
		break;
	}

	if (gInfo.DrawType == 2)
		AutoDetectStereoSettings();

	gInfo.LastDrawStereo = m_bRenderInStereo;
	return !DEBUG_RENDER_IN_ONE_RT; // TRUE
}

void CBaseStereoRenderer::BeforeDrawCall()
{
	if (m_bRenderInStereo)
		return;

	HRESULT hResult = S_OK;
	if (m_Pipeline == Shader)
	{
		if (m_VSPipelineData.m_CurrentShaderData.stereoShader && m_Input.StereoActive)
		{
			DEBUG_TRACE3(_T("\tSet constants for modified shaders\n"));
			SetMonoModifiedShaderConstants();
		}
	}
	else if ( m_Pipeline == RHWEmul )
	{
		const float* pMatrix = &m_TransformAfterViewportVSMono._11;
		NSCALL(m_Direct3DDevice.SetVertexShaderConstantF(250, pMatrix, 1));
		NSCALL(m_Direct3DDevice.SetVertexShaderConstantF(251, pMatrix, 4));
	}
}

void CBaseStereoRenderer::AfterDrawCall()
{
	INSIDE;
	HRESULT hResult = S_OK;
	if (m_Pipeline == RHWEmul)
	{
		hResult = m_Direct3DDevice.SetVertexDeclaration(m_RHWEmulData->first);
		hResult = m_Direct3DDevice.SetVertexShader(m_VSPipelineData.m_CurrentShader);
		const float* pMatrix = &m_VertexShaderRegister[250 * 4 + 0];
		hResult = m_Direct3DDevice.SetVertexShaderConstantF(250, pMatrix, 5);
	}
	if (gInfo.ExtractDepthBuffer &&
		m_DepthStencil.m_pMainSurf == GetBaseSC()->m_pMainDepthStencilSurface)
		GetBaseSC()->m_DrawCountAfterClearDepthBuffer++;

	if (IS_SHUTTER_OUTPUT && !USE_ATI_PRESENTER && !USE_IZ3DKMSERVICE_PRESENTER)
	{
		FlushCommands();
	}
}

void CBaseStereoRenderer::AutoDetectStereoSettings()
{
	if (m_bRenderInStereo)
	{
		switch(m_Pipeline)
		{
		case Shader:
			CheckVSCRCMultiplier();
			break;
		case Fixed:
			m_bRenderInStereo = CheckFixedPipelineMatrices();
			break;
		case RHW:
		case RHWEmul:
			m_bRenderInStereo = !gInfo.MonoRHWPipeline;
			// Moved to TransformPrimitiveInit & TransformPrimitiveUPInit
			break;
		}
	}

	if(m_PSPipelineData.m_bShaderDataAvailable)
	{
		CheckPSCRCMultiplier();
		auto& data = m_PSPipelineData.m_CurrentShaderData.matricesData;
		if(data.matrixSize > 0 && data.matrix[0].inverse)
			CheckRenderInStereoPSPipeline();
		else
		{
			if (m_CurrentPSMultiplier != 1.0f)
				m_CurrentMeshMultiplier = m_CurrentPSMultiplier;
			if (m_CurrentPSConvergenceShift != 0.0f)
				m_CurrentConvergenceShift = m_CurrentPSConvergenceShift;
		}
	}

	if(m_bRenderInStereo)
		CheckMeshMultiplier();
}

bool CBaseStereoRenderer::CheckRenderInStereoVSPipeline()
{
	bool bRenderInStereo = FALSE;
	VS_PRIVATE_DATA& data = m_VSPipelineData.m_CurrentShaderData;
	if (!m_VSPipelineData.m_bShaderDataAvailable || data.shaderIsMono)
	{
		DEBUG_TRACE3(_T("\tVS (CRC 0x%X) is mono\n"), data.CRC32);
	}
	else if (!DEBUG_SKIP_SHADER_MATRIX_ANALYZING)
	{
		if (data.stereoShader == NULL) 
		{
			DEBUG_TRACE3(_T("Using original vertex shader (CRC 0x%X).\n"), data.CRC32);
			bRenderInStereo = CheckVSMatrices();
			m_ZNear = m_ShaderZNear;
			m_ZFar = m_ShaderZFar;
		}
		else
		{
			DEBUG_TRACE3(_T("Using modified vertex shader (CRC 0x%X).\n"), data.CRC32);
			if (!DEBUG_RENDER_IN_MONO)
				bRenderInStereo = TRUE;

			if (data.ZNearRegister != 65535)
			{
				if (m_VSPipelineData.m_pProfile && m_VSPipelineData.m_pProfile->m_bAddZNearCheck)
				{
					m_StereoZNear = m_VSPipelineData.m_pProfile->m_ZNear;
#ifndef FINAL_RELEASE
					if (m_StereoZNear == CUSTOMIZATION_NUMBER)
						m_StereoZNear = m_Input.ZNear;
#endif
				}
				else
					m_StereoZNear = 1.0f;
			}
		}
	}
	return bRenderInStereo;
}

bool CBaseStereoRenderer::CheckFixedPipelineMatrices()
{
	if (gInfo.MonoFixedPipeline)
		return false;

	bool bRenderInStereo = true;
	switch(gInfo.MonoFixedPipelineCheckingMethodIndex)
	{
	case 1:
		// HACK: Oblivion matrix check
		if(	m_ViewMatrix.m[0][0] == 1.0f && m_ViewMatrix.m[0][1] == 0.0f && m_ViewMatrix.m[0][2] == 0.0f && m_ViewMatrix.m[0][3] == 0.0f &&
			m_ViewMatrix.m[1][0] == 0.0f && m_ViewMatrix.m[1][1] == 0.0f && m_ViewMatrix.m[1][2] == 1.0f && m_ViewMatrix.m[1][3] == 0.0f &&
			m_ViewMatrix.m[2][0] == 0.0f && m_ViewMatrix.m[2][1] == 1.0f && m_ViewMatrix.m[2][2] == 0.0f && m_ViewMatrix.m[2][3] == 0.0f &&
			m_ViewMatrix.m[3][0] == 0.0f && m_ViewMatrix.m[3][1] == 0.0f && m_ViewMatrix.m[3][2] == 0.0f && m_ViewMatrix.m[3][3] == 1.0f)
		{
			DEBUG_TRACE3(_T("Oblivion mono view matrix\n"));
			if (m_WorldMatrix.m[0][0] != 0.0f && m_WorldMatrix.m[0][1] == 0.0f && m_WorldMatrix.m[0][2] == 0.0f && m_WorldMatrix.m[0][3] == 0.0f &&
				m_WorldMatrix.m[1][0] == 0.0f && ((m_WorldMatrix.m[1][1] != 0.0f && m_WorldMatrix.m[1][2] == 0.0f) || (abs(m_WorldMatrix.m[1][1]) < 0.00001f && m_WorldMatrix.m[1][2] != 0.0f)) && m_WorldMatrix.m[1][3] == 0.0f &&
				m_WorldMatrix.m[2][0] == 0.0f && ((m_WorldMatrix.m[2][1] == 0.0f && m_WorldMatrix.m[2][2] != 0.0f) || (m_WorldMatrix.m[2][1] != 0.0f && abs(m_WorldMatrix.m[2][2]) < 0.00001f)) && m_WorldMatrix.m[2][3] == 0.0f &&
				m_WorldMatrix.m[3][3] == 1.0f)
			{
				DEBUG_TRACE3(_T("Oblivion mono world matrix\n"));
				bRenderInStereo = false;
			}
			else if (abs(m_WorldMatrix.m[0][0] - m_WorldMatrix.m[1][2]) < 0.00001f && 
				abs(m_WorldMatrix.m[0][2] + m_WorldMatrix.m[1][0]) < 0.00001f && 
				m_WorldMatrix.m[0][3] == 0.0f && m_WorldMatrix.m[1][3] == 0.0f &&
				m_WorldMatrix.m[2][0] == 0.0f && m_WorldMatrix.m[2][3] == 0.0f &&
				m_WorldMatrix.m[3][3] == 1.0f)
			{
				DEBUG_TRACE3(_T("Oblivion mono world matrix (2)\n"));
				bRenderInStereo = false;
			}
		}
		break;
	case 2:
		if (m_bIsViewMatrixIdenty && m_bIsWorldMatrixIdenty)
		{
			DEBUG_TRACE3(_T("Identy view and world matrix\n"));
			bRenderInStereo = false;
		}
		break;
	case 3:			
		if ((abs(abs(m_ViewMatrix._11)-1) < 0.1) && (abs(abs(m_ViewMatrix._22)-1) < 0.1) && // SexyBeach 3
			(abs(abs(m_ViewMatrix._33)-1) < 0.1) && (abs(abs(m_ViewMatrix._44)-1) < 0.1) && 
			m_ViewMatrix._12 == 0 && m_ViewMatrix._13 == 0 && m_ViewMatrix._14 == 0 && 
			m_ViewMatrix._21 == 0 && m_ViewMatrix._23 == 0 && m_ViewMatrix._24 == 0 && 
			m_ViewMatrix._31 == 0 && m_ViewMatrix._32 == 0 && m_ViewMatrix._34 == 0 && 
			m_ViewMatrix._41 == 0 && m_ViewMatrix._42 == 0) 
		{
			DEBUG_TRACE3(_T("Identy view matrix\n"));
			bRenderInStereo = false;
		}
		break;
	}	
	return bRenderInStereo;
}

bool CBaseStereoRenderer::CheckRenderInStereoFixedPipeline()
{
	bool bRenderInStereo = FALSE;
	if(!DEBUG_SKIP_SET_PROJECTION_MATRIX && m_bPerspectiveMatrix)
	{
		if (!DEBUG_RENDER_IN_MONO)
			bRenderInStereo = true;
	};
	return bRenderInStereo;
}

void CBaseStereoRenderer::CheckRenderInStereoPSPipeline()
{
	m_bRenderInStereoPS = TRUE;
	auto& data = m_PSPipelineData.m_CurrentShaderData.matricesData;
	m_Direct3DDevice.GetPixelShaderConstantF(data.matrix[0].matrixRegister, m_PSMatrix, 4);
	bool bTransposed = data.matrix[0].matrixIsTransposed != 0;
	DEBUG_TRACE3(_T("Pixel shader matrix (%stransposed):\n"), bTransposed ? _T("") : _T("non "));
	TraceMatrix(&m_PSMatrix, bTransposed);

#ifndef FINAL_RELEASE
	if (m_CurrentPSMultiplier != 1.0f)
	{
		DEBUG_TRACE3(_T("\tPS multiplier == %f\n"), m_CurrentPSMultiplier);
		if (m_CurrentPSMultiplier == CUSTOMIZATION_NUMBER)
			m_CurrentPSMultiplier = m_Input.Multiplier;
	}
#endif

	m_CurrentPSMultiplier *= gInfo.SeparationScale;
}

void CBaseStereoRenderer::CheckVSCRCMultiplier()
{
	if (!DEBUG_SKIP_CRC_CHECK && m_VSPipelineData.m_pProfile) 
	{
		DEBUG_TRACE3(_T("ShaderCRC = 0x%X Multiplier = %f detected\n"), 
			m_VSPipelineData.m_CurrentShaderData.CRC32, m_VSPipelineData.m_pProfile->m_Multiplier);
		m_CurrentMeshMultiplier = m_VSPipelineData.m_pProfile->m_Multiplier;
		m_CurrentConvergenceShift = m_VSPipelineData.m_pProfile->m_ConvergenceShift;
	}
}

void CBaseStereoRenderer::CheckPSCRCMultiplier()
{
	if (!DEBUG_SKIP_CRC_CHECK && m_PSPipelineData.m_pProfile) 
	{
		DEBUG_TRACE3(_T("ShaderCRC = 0x%X Multiplier = %f detected\n"), 
			m_PSPipelineData.m_CurrentShaderData.CRC32, m_PSPipelineData.m_pProfile->m_Multiplier);
		m_CurrentPSMultiplier = m_PSPipelineData.m_pProfile->m_Multiplier;
		m_CurrentPSConvergenceShift = m_PSPipelineData.m_pProfile->m_ConvergenceShift;
	}
}

void CBaseStereoRenderer::CheckMeshMultiplier()
{
	if(!DEBUG_SKIP_CRC_CHECK && m_CurrentMeshMultiplier == 1.0f)
	{
		if (g_ProfileData.TextureCRCMultiplier.size() > 0)
		{
			//--- collect CRC values for all textures ---
			m_CurrentMeshMultiplier = m_PSPipelineData.GetMultiplayerFromTextures(m_CurrentConvergenceShift);
			if (m_CurrentMeshMultiplier == 1.0f)
				m_CurrentMeshMultiplier = m_VSPipelineData.GetMultiplayerFromTextures(m_CurrentConvergenceShift);
		}
		if (m_CurrentMeshMultiplier == 1.0f && g_ProfileData.MeshCRCMultiplier.size() > 0)
		{
			//--- collect CRC values for all meshes ---
			for(UINT i=0; i < m_dwMaxStreams; i++)
			{
				if(m_MeshCRCMultiplier[i])
				{
					if(m_MeshCRCMultiplier[i]->second.m_Multiplier != 1.f)
					{
						m_CurrentMeshMultiplier = m_MeshCRCMultiplier[i]->second.m_Multiplier;
					}
					if(m_MeshCRCMultiplier[i]->second.m_ConvergenceShift != 0.f)
					{
						m_CurrentConvergenceShift = m_MeshCRCMultiplier[i]->second.m_ConvergenceShift;
					}
				}
			} 
		}
		if (IsWeaponDrawCall() && !gInfo.UseSimpleStereoProjectionMethod)
		{
			if(g_ProfileData.WeaponMultiplier.m_Multiplier != 1.f)
			{
				m_CurrentMeshMultiplier = g_ProfileData.WeaponMultiplier.m_Multiplier;
			}
			if(g_ProfileData.WeaponMultiplier.m_ConvergenceShift != 0.f)
			{
				m_CurrentConvergenceShift = g_ProfileData.WeaponMultiplier.m_ConvergenceShift;
			}
		}
	}

	if (m_CurrentMeshMultiplier == 0.0f)
	{
		DEBUG_TRACE3(_T("\tMesh multiplier == 0.0f\n"));
		m_bRenderInStereo = FALSE;
		return;
	}

	//--- draw small view port to custom separation ---
	if(gInfo.ScaleSeparationForSmallViewPorts != 1.f)
	{
		D3DSURFACE_DESC desc;
		m_RenderTarget[0].m_pMainSurf->GetDesc(&desc);
		if(m_ViewPort.Width < (desc.Width >> 1) && m_ViewPort.Height < (desc.Height >> 1))
		{
			DEBUG_TRACE3(_T("\tSmall viewport forced to custom separation\n"));
			m_CurrentMeshMultiplier = gInfo.ScaleSeparationForSmallViewPorts;

			if (m_CurrentMeshMultiplier == 0.0f)
			{
				DEBUG_TRACE3(_T("\tMesh multiplier == 0.0f\n"));
				m_bRenderInStereo = FALSE;
				return;
			}
		}
	}

#ifndef FINAL_RELEASE
	if (m_CurrentMeshMultiplier != 1.0f)
	{
		DEBUG_TRACE3(_T("\tMesh multiplier == %f\n"), m_CurrentMeshMultiplier);
		if (m_CurrentMeshMultiplier == CUSTOMIZATION_NUMBER)
			m_CurrentMeshMultiplier = m_Input.Multiplier;
	}

	if (m_CurrentConvergenceShift != 0.0f)
	{
		DEBUG_TRACE3(_T("\tConvergence shift == %f\n"), m_CurrentConvergenceShift);
		if (m_CurrentConvergenceShift == CUSTOMIZATION_NUMBER)
			m_CurrentConvergenceShift = m_Input.ConvergenceShift;
	}
#endif

	// 0.4.3: Add global user-driven convergence shift (Alt+Numpad +/-) on
	// top of whatever the per-shader profile or weapon/mesh entries set.
	// Default is 0.0f so users who don't press the hotkey see no behaviour
	// change. Also wiring the hotkey into actual rendering (pre-0.4.3 the
	// field itself was FINAL_RELEASE-gated so this value never existed
	// in shipped builds).
	m_CurrentConvergenceShift += m_Input.ConvergenceShift;

	if (m_Pipeline == Shader && m_bInverseZ)
		m_CurrentMeshMultiplier *= -1.0f;
	m_CurrentMeshMultiplier *= gInfo.SeparationScale;
}

bool CBaseStereoRenderer::NeedCalculateMeshCRC(DWORD Size, ResourcesCRCMultiplierMapBySize::const_iterator &iter) 
{ 
	iter = g_ProfileData.MeshCRCMultiplier.find(Size);
	if (iter == g_ProfileData.MeshCRCMultiplier.end()) 
		iter = g_ProfileData.MeshCRCMultiplier.find(0);
	if (iter != g_ProfileData.MeshCRCMultiplier.end()) 
		return true;
	else
		return false;
};

bool	CBaseStereoRenderer::NeedCalculateTextureCRC(DWORD Size, ResourcesCRCMultiplierMapBySize::const_iterator &iter) 
{ 
	iter = g_ProfileData.TextureCRCMultiplier.find(Size);
	if (iter == g_ProfileData.TextureCRCMultiplier.end()) 
		iter = g_ProfileData.TextureCRCMultiplier.find(0);
	if (iter != g_ProfileData.TextureCRCMultiplier.end()) 
		return true;
	else
		return false;
};

HRESULT CBaseStereoRenderer::CRCMeshCheck(UINT StreamNumber, IDirect3DVertexBuffer9 * pStreamData)
{
	HRESULT hResult = S_OK;
	bool bCalculateForAll = 
#ifdef ZLOG_ALLOW_TRACING
		(zlog::GetSeverity() == zlog::SV_FLOOD);
#else 
		false;
#endif
	if(pStreamData && !DEBUG_SKIP_CRC_CHECK && (!g_ProfileData.MeshCRCMultiplier.empty() || bCalculateForAll))
	{
		const ResourceCRCMultiplier *pCRCMultiplier = GetCRCMultiplier(pStreamData);
		if (bCalculateForAll && pCRCMultiplier != NULL && pCRCMultiplier->first == 0) 
			pCRCMultiplier = NULL;
		if (pCRCMultiplier != NULL)
			m_MeshCRCMultiplier[StreamNumber] = pCRCMultiplier;
		else
		{
			bool CantCalculateCRC = true;
#ifndef FINAL_RELEASE
			LARGE_INTEGER currentTime;
			if(GINFO_DEBUG)
				QueryPerformanceCounter(&currentTime);
#endif
			D3DVERTEXBUFFER_DESC desc;
			pStreamData->GetDesc(&desc);
			ResourcesCRCMultiplierMapBySize::const_iterator si;
			if (NeedCalculateMeshCRC(desc.Size, si) || bCalculateForAll)
			{
				DWORD CRC32 = CalculateVBCRC(pStreamData, &desc);
				if (CRC32)
				{
					ResourceCRCMultiplier pair;
					pair.first = CRC32;
					if (si != g_ProfileData.MeshCRCMultiplier.end())
					{
						ResourceCRCMultiplierMap::const_iterator iter = si->second.find(pair.first);
						if (iter != si->second.end())
						{
							pair.second = iter->second;
							DEBUG_TRACE3(_T("MeshCRC=0x%X Size=%d Multiplier=%f detected\n"), 
								iter->first, desc.Size, iter->second.m_Multiplier);
						}
					}
					m_MeshCRCMultiplier[StreamNumber] = SetCRCMultiplier(pStreamData, &pair);
					CantCalculateCRC = false;
				} 
			}
			if(CantCalculateCRC)
			{
				ResourceCRCMultiplier pair;
				pair.first = 0;
				m_MeshCRCMultiplier[StreamNumber] = SetCRCMultiplier(pStreamData, &pair);
			}
#ifndef FINAL_RELEASE
			if(GINFO_DEBUG)
			{
				LARGE_INTEGER stop;
				QueryPerformanceCounter(&stop);
				m_nCRCCalculatingTime.QuadPart += stop.QuadPart - currentTime.QuadPart;
			}
#endif
		}
	} else
	{
		m_MeshCRCMultiplier[StreamNumber] = NULL;
	}
	return hResult;
}

void CBaseStereoRenderer::SetMonoViewStage()
{
	HRESULT hResult = S_OK;

	m_CurrentView = VIEWINDEX_MONO;
	if (m_bRenderInStereo && !IsLeftViewUnmodified())
	{
		if (m_Pipeline == Shader) 
		{
			auto& data = m_VSPipelineData.m_CurrentShaderData.matricesData;
			if (data.matrixSize != 0)
			{
				for (DWORD i = 0; i < data.matrixSize; i++)
				{
					UINT vector4fCount = data.matrix[i].matrixIsTransposed ? 1 : 4;
					hResult = m_Direct3DDevice.SetVertexShaderConstantF(data.matrix[i].matrixRegister, GetVSMatrix(i), vector4fCount);
				}
			}
		}
		else if (m_Pipeline == Fixed)
		{
			hResult = m_Direct3DDevice.SetTransform( D3DTS_PROJECTION, &m_ProjectionMatrix );
		}
	}

	if(m_bRenderInStereoPS)
	{
		auto& data = m_PSPipelineData.m_CurrentShaderData.matricesData;
		hResult = m_Direct3DDevice.SetPixelShaderConstantF(data.matrix[0].matrixRegister, m_PSMatrix, 4);
	}
}

void CBaseStereoRenderer::CheckViewportZRange()
{
	if (m_Input.StereoActive && !gInfo.UseSimpleStereoProjectionMethod && 
		(/*m_OldMinZ != m_ViewPort.MinZ ||*/ m_OldMaxZ !=  m_ViewPort.MaxZ))
	{
		DEBUG_TRACE3(_T("MinZ or MaxZ changed: update stereo matrices\n"));
		UpdateProjectionMatrix();
	}
}

bool AlmostEqual(float A, float B, float maxError)
{
	if (fabs(A - B) <= maxError)
		return true;
	return false;
}

void CBaseStereoRenderer::UpdateProjectionMatrix(bool bOnlyFP/* = false*/) 
{
	if(DEBUG_SKIP_UPDATE_PROJECTION_MATRIX)
		return;

	DEBUG_TRACE3(_T("\nUpdateProjectionMatrix beginning\n"));

	m_bUpdatedSkewMatrices = TRUE;

	DEBUG_TRACE3(_T("Application ProjectionMatrix matrix:\n")); 
	TraceMatrix(&m_ProjectionMatrix);

	m_bPerspectiveMatrix = FALSE;
	m_FixedZNear = 0.0f;
	m_FixedZFar = 0.0f;

	// detect matrix type
	D3DXMATRIX fixedInvProjection;
	if (m_ProjectionMatrix._14 == 0.0f && AlmostEqual(m_ProjectionMatrix._24, 0.0f, 1.0e-7f)) // for 3D Studio Max 2010
	{
		if (m_ProjectionMatrix._34 != 0.0f && m_ProjectionMatrix._44 == 0.0f)
		{
			DEBUG_TRACE3(_T("%s: probably PERSPECTIVE projection matrix\n"), _T( __FUNCTION__ ) );
			if(m_ProjectionMatrix._33 == 0)
			{
				DEBUG_TRACE3(_T("%s: Error! Bad projection matrix: m.33 = %f\n"), _T( __FUNCTION__ ) , m_ProjectionMatrix._33);
			}
			else if (!D3DXMatrixInverse(&fixedInvProjection, NULL, &m_ProjectionMatrix)) 
			{
				DEBUG_TRACE3(_T("%s: Error! Bad projection matrix: inverse matrix don't exist\n"), _T( __FUNCTION__ ) );
			}
			else
			{
				m_bPerspectiveMatrix = TRUE;
			}
		} 
	}

	//--- Calculate left, right matrices ---
	// fixed pipeline
	float cameraShift = m_Input.GetActivePreset()->StereoBase * 0.5f;
	float One_div_ZPS = m_Input.GetActivePreset()->One_div_ZPS;
	bool bRightHandedFP = false;
	if (m_bPerspectiveMatrix)
	{
		DEBUG_TRACE3(_T("Stereo projection matrix\n"));
		m_FixedZNear = - m_ProjectionMatrix._43 / m_ProjectionMatrix._33;		// WoW: 0.2f
		m_FixedZFar = - m_ProjectionMatrix._43 / (m_ProjectionMatrix._33 - 1);	// WoW: 1280.0f
		DEBUG_TRACE3(_T("\tZn %f, Zf %f\n"), m_FixedZNear, m_FixedZFar); 

		//  support right handed coordinate system
		if(m_FixedZNear < 0)
		{
			DEBUG_TRACE3(_T("\tRight handed coordinate system\n")); 
			bRightHandedFP = true;
		}

		//--- some games use wrong negative aspect ratio, fix it --- 
		if (m_ProjectionMatrix._11 < 0)
		{
			DEBUG_TRACE3(_T("\tNegative aspect ratio\n")); 
			cameraShift = -cameraShift;
		}
	}
	else
	{
		DEBUG_TRACE3(_T("Non stereo projection matrix\n"));
	}

	if (bOnlyFP && !m_bPerspectiveMatrix)
	{
		DEBUG_TRACE3(_T("Early end of UpdateProjectionMatrix\n\n"));
		return;
	}

	// vertex shader pipeline
	D3DXMATRIX DriverProjMatrix;
	D3DXMATRIX DriverInvProjection;

	bool bPerspectiveMatrix = m_bPerspectiveMatrix != 0;
	if (!gInfo.AlwaysUseDefaultProjectionMatrixForVS && !bPerspectiveMatrix && m_bPerspectiveVSMatrix)
	{
		DEBUG_TRACE3(_T("Use old FP Projection matrix for VS\n"));
		DriverProjMatrix = m_VSProjectionMatrix;
		D3DXMatrixInverse(&DriverInvProjection, NULL, &DriverProjMatrix);
		DEBUG_TRACE3(_T("Inverse Projection matrix:\n")); 
		TraceMatrix(&DriverInvProjection);

		DEBUG_TRACE3(_T("Stereo VS projection matrix\n"));
		// TODO: Remove code duplication
		float Zn = - DriverProjMatrix._43 / DriverProjMatrix._33;		// WoW: 0.2f
		float Zf = - DriverProjMatrix._43 / (DriverProjMatrix._33 - 1);	// WoW: 1280.0f
		DEBUG_TRACE3(_T("\tZn %f, Zf %f\n"), Zn, Zf); 

		//  support right handed coordinate system
		if(Zn < 0)
		{
			DEBUG_TRACE3(_T("\tRight handed coordinate system\n")); 
			One_div_ZPS *= -1;
		}

		//--- some games use wrong negative aspect ratio, fix it --- 
		if (DriverProjMatrix._11 < 0)
		{
			DEBUG_TRACE3(_T("\tNegative aspect ratio\n")); 
			cameraShift = -cameraShift;
		}
	}
	else
	{
		if (gInfo.AlwaysUseDefaultProjectionMatrixForVS || !bPerspectiveMatrix) 
		{
			float aspectRatio = (float)GetBaseSC()->m_BackBufferSizeBeforeScaling.cx / (float)GetBaseSC()->m_BackBufferSizeBeforeScaling.cy;
			float yScale = 1/tan(DEFAULT_FOV/2);
			float m33 = DEFAULT_ZFAR / (DEFAULT_ZFAR - DEFAULT_ZNEAR);
			DriverProjMatrix = D3DXMATRIX(
				yScale / aspectRatio,	0.0f,	0.0f,					0.0f, 
				0.0f,					yScale,	0.0f,					0.0f, 
				0.0f,					0.0f,	m33,					1.0f,
				0.0f,					0.0f,	-DEFAULT_ZNEAR * m33,	0.0f
				);
			DEBUG_TRACE3(_T("Our Projection matrix:\n")); 
			TraceMatrix(&DriverProjMatrix);
			//D3DXMatrixPerspectiveFovLH(&m_ProjectionMatrix, DEFAULT_FOV, aspectRatio,
			//	DEFAULT_ZNEAR, DEFAULT_ZFAR );
			D3DXMatrixInverse(&DriverInvProjection, NULL, &DriverProjMatrix);
			DEBUG_TRACE3(_T("Our Inverse Projection matrix:\n")); 
			TraceMatrix(&DriverInvProjection);
		}
		else
		{
			DEBUG_TRACE3(_T("Use FP Projection matrix for VS\n")); 
			m_bPerspectiveVSMatrix = bPerspectiveMatrix;
			m_VSProjectionMatrix = m_ProjectionMatrix;
			DriverProjMatrix = m_VSProjectionMatrix;
			DriverInvProjection = fixedInvProjection;
			if (bRightHandedFP)
			{
				One_div_ZPS *= -1;
				bRightHandedFP = false;
			}
		}
	}

	float A, B;
	float scale;
	if (!gInfo.UseSimpleStereoProjectionMethod)
	{
		m_OldMinZ = m_ViewPort.MinZ;
		m_OldMaxZ = m_ViewPort.MaxZ;
		DEBUG_TRACE3(_T("MinZ %f, MaxZ %f:\n"), m_ViewPort.MinZ, m_ViewPort.MaxZ); 
		if (m_OldMinZ != 0.0f)
		{
			DEBUG_TRACE3(_T("Warning: MinZ != 0.0\n")); 
		}
		if (abs(m_OldMinZ - m_OldMaxZ) < 0.01f)
		{
			DEBUG_TRACE3(_T("Warning: MinZ(%f) == MaxZ\n"), m_OldMinZ); 
		}
		/*float scale = m_OldMaxZ - m_OldMinZ;
		scale = scale > 0.0001f ? scale : 1.0f;
		One_div_ZPS = 1 / (1/One_div_ZPS - m_OldMinZ);
		One_div_ZPS *= scale;*/
		scale = m_OldMaxZ /*- m_OldMinZ*/;
		scale = scale > 0.01f ? (1.0f / scale) : 1.0f;
	}
	else
	{
		scale = 1.0f;
	}

	//--- stereo matrix ---
	D3DXMATRIX stereoProjMatrix;
	stereoProjMatrix._11 = DriverProjMatrix._11;
	stereoProjMatrix._12 = DriverProjMatrix._12;
	stereoProjMatrix._13 = DriverProjMatrix._13;
	stereoProjMatrix._14 = DriverProjMatrix._14;
	stereoProjMatrix._21 = DriverProjMatrix._21;
	stereoProjMatrix._22 = DriverProjMatrix._22;
	stereoProjMatrix._23 = DriverProjMatrix._23;
	stereoProjMatrix._24 = DriverProjMatrix._24;

	// left
	switch(gInfo.SeparationMode)
	{
	case 0:
		A   = -cameraShift * One_div_ZPS;
		B   =  cameraShift * scale;
		break;
	case 1:
		A   =  0;
		B   =  0;
		break;
	default:
		A   = -2 * cameraShift * One_div_ZPS;
		B   =  2 * cameraShift * scale;
		break;
	}
	MONO_CAMERA &cameraLeft = m_StereoCamera.GetCamera(VIEWINDEX_LEFT);
	cameraLeft.A = A;
	cameraLeft.B = B;

	stereoProjMatrix._31 = DriverProjMatrix._31 + A * DriverProjMatrix._11;
	stereoProjMatrix._32 = DriverProjMatrix._32 + A * DriverProjMatrix._12;
	stereoProjMatrix._33 = DriverProjMatrix._33 + A * DriverProjMatrix._13;
	stereoProjMatrix._34 = DriverProjMatrix._34 + A * DriverProjMatrix._14;
	stereoProjMatrix._41 = DriverProjMatrix._41 + B * DriverProjMatrix._11;
	stereoProjMatrix._42 = DriverProjMatrix._42 + B * DriverProjMatrix._12;
	stereoProjMatrix._43 = DriverProjMatrix._43 + B * DriverProjMatrix._13;
	stereoProjMatrix._44 = DriverProjMatrix._44 + B * DriverProjMatrix._14;
	DEBUG_TRACE3(_T("%s stereo projection matrix:\n"), GetViewString(VIEWINDEX_LEFT));
	TraceMatrix(&stereoProjMatrix);

	CalculateMatrix(VIEWINDEX_LEFT, &stereoProjMatrix, &DriverInvProjection);
	if (bPerspectiveMatrix)
		CalculateFPMatrix(VIEWINDEX_LEFT, &m_ProjectionMatrix, bRightHandedFP);

	// right
	switch(gInfo.SeparationMode)
	{
	case 0:
		A  = -A;
		B  = -B;
		break;
	case 1:
		A  =  2 * cameraShift * One_div_ZPS;
		B  = -2 * cameraShift * scale;
		break;
	default:
		A  =  0;
		B  =  0;
		break;
	}
	MONO_CAMERA &cameraRight = m_StereoCamera.GetCamera(VIEWINDEX_RIGHT);
	cameraRight.A = A;
	cameraRight.B = B;

	stereoProjMatrix._31 = DriverProjMatrix._31 + A * DriverProjMatrix._11;
	stereoProjMatrix._32 = DriverProjMatrix._32 + A * DriverProjMatrix._12;
	stereoProjMatrix._33 = DriverProjMatrix._33 + A * DriverProjMatrix._13;
	stereoProjMatrix._34 = DriverProjMatrix._34 + A * DriverProjMatrix._14;
	stereoProjMatrix._41 = DriverProjMatrix._41 + B * DriverProjMatrix._11;
	stereoProjMatrix._42 = DriverProjMatrix._42 + B * DriverProjMatrix._12;
	stereoProjMatrix._43 = DriverProjMatrix._43 + B * DriverProjMatrix._13;
	stereoProjMatrix._44 = DriverProjMatrix._44 + B * DriverProjMatrix._14;
	DEBUG_TRACE3(_T("%s stereo projection matrix:\n"), GetViewString(VIEWINDEX_RIGHT));
	TraceMatrix(&stereoProjMatrix);

	CalculateMatrix(VIEWINDEX_RIGHT, &stereoProjMatrix, &DriverInvProjection);
	if (bPerspectiveMatrix)
		CalculateFPMatrix(VIEWINDEX_RIGHT, &m_ProjectionMatrix, bRightHandedFP);

	DEBUG_TRACE3(_T("End of UpdateProjectionMatrix\n\n"));
}

void CBaseStereoRenderer::CalculateMatrix( VIEWINDEX viewIndex, const D3DXMATRIX* pStereoProjMatrix, const D3DXMATRIX* pInvProjection )
{
	MONO_CAMERA &camera = m_StereoCamera.GetCamera(viewIndex);

	// for shader skew matrix
	D3DXMatrixMultiply(&camera.TransformBeforeViewport, pInvProjection, pStereoProjMatrix);
	DEBUG_TRACE3(_T("%s VS TransformBeforeViewport matrix:\n"), GetViewString(viewIndex));
	TraceMatrix(&camera.TransformBeforeViewport);
}

void CBaseStereoRenderer::CalculateFPMatrix( VIEWINDEX viewIndex, const D3DXMATRIX* pProjMatrix, bool bRightHanded )
{
	MONO_CAMERA &camera = m_StereoCamera.GetCamera(viewIndex);
	D3DXMATRIX &projMatrix = camera.ProjectionMatrix;
	projMatrix = *pProjMatrix;
#if 1 // Multiply S * P
	float A = camera.A;
	float B = camera.B;
	if (bRightHanded)
		A *= -1;
	projMatrix._31 += A * projMatrix._11;
	projMatrix._32 += A * projMatrix._12;
	projMatrix._33 += A * projMatrix._13;
	projMatrix._34 += A * projMatrix._14;
	projMatrix._41 += B * projMatrix._11;
	projMatrix._42 += B * projMatrix._12;
	projMatrix._43 += B * projMatrix._13;
	projMatrix._44 += B * projMatrix._14;
#else // Multiply P * DP^-1 * S * DP^-1
	float m31 = camera.TransformBeforeViewport._31;
	float m41 = camera.TransformBeforeViewport._41;
	if (bRightHanded)
		m31 *= -1;
	projMatrix._11 += m31 * projMatrix._13 + m41 * projMatrix._14;
	projMatrix._21 += m31 * projMatrix._23 + m41 * projMatrix._24;
	projMatrix._31 += m31 * projMatrix._33 + m41 * projMatrix._34;
	projMatrix._41 += m31 * projMatrix._43 + m41 * projMatrix._44;
#endif
	DEBUG_TRACE3(_T("%s FP projection matrix:\n"), GetViewString(viewIndex));
	TraceMatrix(&projMatrix);
}

bool CBaseStereoRenderer::CheckVSMatrices()
{
	auto& data = m_VSPipelineData.m_CurrentShaderData.matricesData;
	for (DWORD i = 0; i < data.matrixSize; i++)
	{
		bool bTransposed = data.matrix[i].matrixIsTransposed != 0;
		DEBUG_TRACE3(_T("Vertex shader matrix %d (transposed %d):\n"), i, bTransposed); 
		TraceMatrix(&GetVSMatrix(i), bTransposed);
	}

	if (m_OldShaderMatrix == GetVSMatrix(0))
	{
		DEBUG_TRACE3(_T("Skip update VS matrix\n"));
		if (!DEBUG_RENDER_IN_MONO)
		{
			m_bInverseZ = m_bOldShaderInverseZ;
			m_bOnlyProjectionMatrix = m_bOldShaderOnlyProjectionMatrix;
			return m_bOldShaderPerspectiveMatrix;
		}
		return false;  //--- not need to update matrix ---
	}

	m_bOldShaderPerspectiveMatrix = TRUE;
	m_bOldShaderOnlyProjectionMatrix = FALSE;
	m_bOldShaderInverseZ = FALSE;
	m_ShaderZNear = 0.0f;
	m_ShaderZFar = 0.0f;
	D3DXMATRIX& CurrentShaderMatrix = GetVSMatrix(0);
	if(data.matrix[0].matrixIsTransposed)
		CheckVSMatricesTransposed(CurrentShaderMatrix);
	else
		CheckVSMatricesNonTransposed(CurrentShaderMatrix);

	m_OldShaderMatrix = CurrentShaderMatrix;
	if (!DEBUG_RENDER_IN_MONO)
	{
		m_bInverseZ = m_bOldShaderInverseZ;
		m_bOnlyProjectionMatrix = m_bOldShaderOnlyProjectionMatrix;
		return m_bOldShaderPerspectiveMatrix;
	}
	return false;
}

void CBaseStereoRenderer::CheckVSMatricesTransposed( D3DXMATRIX &CurrentShaderMatrix )
{
	//--- skip ORTO matrices  ---  
	if (CurrentShaderMatrix._41 == 0.0f && CurrentShaderMatrix._42 == 0.0f && 
		CurrentShaderMatrix._43 == 0.0f && CurrentShaderMatrix._44 != 0.0f ) 
	{
		DEBUG_TRACE3(_T("\tOrtho matrix\n")); 
		m_bOldShaderPerspectiveMatrix = FALSE;
		return;
	}

	bool bCalculateInverseMatrix = gInfo.CheckShadowMatrix || gInfo.CheckInverseLR || gInfo.CheckExistenceInverseMatrix;
	D3DXMATRIX invCurrentShaderMatrix;
	float k = (CurrentShaderMatrix._31 + CurrentShaderMatrix._32 + CurrentShaderMatrix._33) / 
		(CurrentShaderMatrix._41 + CurrentShaderMatrix._42 + CurrentShaderMatrix._43);
	m_ShaderZNear = CurrentShaderMatrix._44 - CurrentShaderMatrix._34 / k;
	m_ShaderZFar = -k * m_ShaderZNear / (1 - k);
	DEBUG_TRACE3(_T("Zn %f, Zf %f\n"), m_ShaderZNear, m_ShaderZFar);

	if (CurrentShaderMatrix._12 == 0.0f && CurrentShaderMatrix._13 == 0.0f && 
		CurrentShaderMatrix._14 == 0.0f && CurrentShaderMatrix._21 == 0.0f && 
		CurrentShaderMatrix._23 == 0.0f && CurrentShaderMatrix._24 == 0.0f &&
		CurrentShaderMatrix._31 == 0.0f && CurrentShaderMatrix._32 == 0.0f && 
		CurrentShaderMatrix._41 == 0.0f && CurrentShaderMatrix._42 == 0.0f && 
		CurrentShaderMatrix._43 == 1.0f && CurrentShaderMatrix._44 == 0.0f ) 
	{
		DEBUG_TRACE3(_T("\tOnly projection matrix\n")); 
		m_bOldShaderOnlyProjectionMatrix = TRUE;
	}
	if (GINFO_DEBUG || bCalculateInverseMatrix)
	{
		D3DXMATRIX* mInv = D3DXMatrixInverse(&invCurrentShaderMatrix, NULL, &CurrentShaderMatrix);
		if(mInv)
		{
			DEBUG_TRACE3(_T("Vertex shader inverse matrix:\n")); 
			TraceMatrix(mInv, true);
			if(fabs(invCurrentShaderMatrix._43) < 0.01f)
			{
				DEBUG_TRACE3(_T("\tPerspective shadow map transformation matrix\n")); 
				if (gInfo.CheckShadowMatrix)
					m_bOldShaderPerspectiveMatrix = FALSE;
				else
				{
					DEBUG_TRACE3(_T("Hint: CheckShadowMatrix off\n")); 
				}
			}
			else if(invCurrentShaderMatrix._43 * invCurrentShaderMatrix._44 > 0.0f)
			{
				DEBUG_TRACE3(_T("\tInverse Z %f\n"), (mInv ? invCurrentShaderMatrix._43 * invCurrentShaderMatrix._44 : 1000000.0f)); 
				if(gInfo.CheckInverseLR)
					m_bOldShaderInverseZ = TRUE;
				else
				{
					DEBUG_TRACE3(_T("Hint: CheckInverseLR off\n"));
				}
			}
		}
		else
		{
			DEBUG_TRACE3(_T("\tInverse matrix doesn't exist\n")); 
			if (gInfo.CheckExistenceInverseMatrix)
				m_bOldShaderPerspectiveMatrix = FALSE;
			else
			{
				DEBUG_TRACE3(_T("Hint: CheckExistenceInverseMatrix off\n")); 
			}
		}
	}
		}

void CBaseStereoRenderer::CheckVSMatricesNonTransposed( D3DXMATRIX &CurrentShaderMatrix )
	{
	//--- skip ORTO matrices  ---  
	if (CurrentShaderMatrix._14 == 0.0f && CurrentShaderMatrix._24 == 0.0f && 
		CurrentShaderMatrix._34 == 0.0f && CurrentShaderMatrix._44 != 0.0f ) 
	{
		DEBUG_TRACE3(_T("\tOrtho matrix\n")); 
		m_bOldShaderPerspectiveMatrix = FALSE;
		return;
	}

	bool bCalculateInverseMatrix = gInfo.CheckShadowMatrix || gInfo.CheckInverseLR || gInfo.CheckExistenceInverseMatrix;
	D3DXMATRIX invCurrentShaderMatrix;
	float k = (CurrentShaderMatrix._13 + CurrentShaderMatrix._23 + CurrentShaderMatrix._33) / 
		(CurrentShaderMatrix._14 + CurrentShaderMatrix._24 + CurrentShaderMatrix._34);
	m_ShaderZNear = CurrentShaderMatrix._44 - CurrentShaderMatrix._43 / k;
	m_ShaderZFar = -k * m_ShaderZNear / (1 - k);
	DEBUG_TRACE3(_T("Zn %f, Zf %f\n"), m_ShaderZNear, m_ShaderZFar);

	if (CurrentShaderMatrix._21 == 0.0f && CurrentShaderMatrix._31 == 0.0f && 
		CurrentShaderMatrix._41 == 0.0f && CurrentShaderMatrix._12 == 0.0f && 
		CurrentShaderMatrix._32 == 0.0f && CurrentShaderMatrix._42 == 0.0f &&
		CurrentShaderMatrix._13 == 0.0f && CurrentShaderMatrix._23 == 0.0f && 
		CurrentShaderMatrix._14 == 0.0f && CurrentShaderMatrix._24 == 0.0f && 
		CurrentShaderMatrix._34 == 1.0f && CurrentShaderMatrix._44 == 0.0f ) 
	{
		DEBUG_TRACE3(_T("\tOnly projection matrix\n")); 
		m_bOldShaderOnlyProjectionMatrix = TRUE;
	}
	if (GINFO_DEBUG || bCalculateInverseMatrix)
	{
		D3DXMATRIX* mInv = D3DXMatrixInverse(&invCurrentShaderMatrix, NULL, &CurrentShaderMatrix);
		if(mInv)
		{
			DEBUG_TRACE3(_T("Vertex shader inverse matrix:\n")); 
			TraceMatrix(mInv, false);
			if(fabs(invCurrentShaderMatrix._34) < 0.01f)
			{
				DEBUG_TRACE3(_T("\tPerspective shadow map transformation matrix\n")); 
				if (gInfo.CheckShadowMatrix)
					m_bOldShaderPerspectiveMatrix = FALSE;
				else
				{
					DEBUG_TRACE3(_T("Hint: CheckShadowMatrix off\n")); 
				}
			}
			else if(invCurrentShaderMatrix._34 * invCurrentShaderMatrix._44 > 0.0f)
			{
				DEBUG_TRACE3(_T("\tInverse Z %f\n"), (mInv ? invCurrentShaderMatrix._34 * invCurrentShaderMatrix._44 : 1000000.0f)); 
				if (gInfo.CheckInverseLR)
					m_bOldShaderInverseZ = TRUE;
				else
				{
					DEBUG_TRACE3(_T("Hint: CheckInverseLR off\n")); 
				}
			}
		}
		else 
		{
			DEBUG_TRACE3(_T("\tInverse matrix doesn't exist\n")); 
			if (gInfo.CheckExistenceInverseMatrix)
				m_bOldShaderPerspectiveMatrix = FALSE;
			else
			{
				DEBUG_TRACE3(_T("Hint: CheckExistenceInverseMatrix off\n")); 
			}
		}
	}
		}

void CBaseStereoRenderer::UpdateRHWMatrix()
{
	// Updated separately because ViewPort updates frequently then Projection matrix
	DEBUG_TRACE3(_T("UpdateRHWMatrix\n"));

#ifdef ZLOG_ALLOW_TRACING
	if (!m_bUpdatedSkewMatrices && (memcmp(&m_OldViewPort, &m_ViewPort, sizeof(D3DVIEWPORT9)) == 0))
	{
		if (zlog::GetSeverity() == zlog::SV_FLOOD)
		{
			DEBUG_TRACE3(_T("Skip parameters update\n"));
			DEBUG_TRACE3(_T("Current Viewport: %s\n"), GetViewPortString(&m_ViewPort));
			DEBUG_TRACE3(_T("Current TransformAfterViewport matrix:\n"));
			TraceMatrix(&m_StereoCamera.GetCamera(VIEWINDEX_LEFT).TransformAfterViewport);
		}
		return;  //--- not need to update matrix ---
	}
#endif
	m_OldViewPort     = m_ViewPort;
	m_bUpdatedSkewMatrices = FALSE;

	// for RHW transformation
	DEBUG_TRACE3(_T("\tViewport: %s\n"), GetViewPortString(&m_ViewPort));
	float Hw = .5f * m_ViewPort.Width;
	float Hh = .5f * m_ViewPort.Height;
	D3DXMATRIX viewPortMatrix = D3DXMATRIX(
		Hw,					0,					0,									0, 
		0,					-Hh,				0,									0, 
		0,					0,					m_ViewPort.MaxZ - m_ViewPort.MinZ,	0, 
		m_ViewPort.X + Hw,	m_ViewPort.Y + Hh,	m_ViewPort.MinZ,					1);
	D3DXMATRIX invViewPortMatrix;
	D3DXMatrixInverse(&invViewPortMatrix, NULL, &viewPortMatrix);

	D3DXMatrixTranspose(&m_TransformAfterViewportVSMono, &invViewPortMatrix);
	DEBUG_TRACE3(_T("Current TransformAfterViewportVSMono matrix:\n"));
	TraceMatrix(&m_TransformAfterViewportVSMono, true);

	CalculateRHWMatrix(VIEWINDEX_LEFT, &viewPortMatrix, &invViewPortMatrix);
	CalculateRHWMatrix(VIEWINDEX_RIGHT, &viewPortMatrix, &invViewPortMatrix);
}

void CBaseStereoRenderer::CalculateRHWMatrix( VIEWINDEX viewIndex, const D3DXMATRIX* pViewPortMatrix, const D3DXMATRIX* pInvViewPortMatrix )
{
	D3DXMATRIX tmpMatrix;
	MONO_CAMERA &camera = m_StereoCamera.GetCamera(viewIndex);
	D3DXMatrixMultiply(&tmpMatrix, pInvViewPortMatrix, &camera.TransformBeforeViewport);

	D3DXMatrixTranspose(&camera.TransformAfterViewportVS, &tmpMatrix);
	DEBUG_TRACE3(_T("%s TransformAfterViewportVS matrix:\n"), GetViewString(viewIndex));
	TraceMatrix(&camera.TransformAfterViewportVS, true);

	D3DXMatrixMultiply(&camera.TransformAfterViewport, &tmpMatrix, pViewPortMatrix);
	DEBUG_TRACE3(_T("%s TransformAfterViewport matrix:\n"), GetViewString(viewIndex));
	TraceMatrix(&camera.TransformAfterViewport);
	//--- not need to fix calculation errors, because we use only _31 and _41 elements ---
}

void CBaseStereoRenderer::GetViewportOffset(FLOAT &x, FLOAT &y)
{
	if (m_CurrentView == VIEWINDEX_RIGHT && m_pCurOffset)
	{
		x = (FLOAT)m_pCurOffset->cx;
		y = (FLOAT)m_pCurOffset->cy;
	}
	else
	{
		x = 0.0f;
		y = 0.0f;
	}
}