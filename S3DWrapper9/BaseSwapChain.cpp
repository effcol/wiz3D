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
#include <Shlobj.h>
#include <iomanip>
#include "BaseStereoRenderer.h"
#include "BaseSwapChain.h"
#include "ShaderHunter9.h"
#include "BackgroundPresenterThread.h"
#include "PresenterMultiThread.h"
#include "..\S3DAPI\ScalingAgent.h"
#include "..\S3DAPI\ScalingHook.h"
#include "..\Shared\PanelNativeSize.h"       // 0.5.0 task #77: sub-native-fullscreen auto-enable
#include "..\S3DAPI\ReadData.h"
#include "..\S3DAPI\ShutterAPI.h"
#include "..\S3DAPI\Utils.h"
#include "Utils.h"
#include "ProductNames.h"
#include "UMDDeviceWrapper.h"
#include "UMDShutterPresenter.h"
#include "ProxySwapChain9-inl.h"
#include "CommandDumper.h"

#include "../Shared/version.h"
#ifndef WIN64
#ifdef _DEBUG
#define DRIVER_VER _T("debug x32")
#else
#define DRIVER_VER _T("release x32")
#endif
#else
#ifdef _DEBUG
#define DRIVER_VER _T("debug x64")
#else
#define DRIVER_VER _T("release x64")
#endif
#endif

CBaseSwapChain::CBaseSwapChain(CMonoRenderer* pDirect3DDevice9, UINT index)
: CMonoSwapChain(pDirect3DDevice9, index), m_LaserSightData(false)
{
	m_DstMonitorPoint.x = 0;
	m_DstMonitorPoint.y = 0;
	ZeroMemory(m_PresentationParameters, 2 * sizeof(D3DPRESENT_PARAMETERS));
	m_MultiSampleType = D3DMULTISAMPLE_NONE;
	m_MultiSampleQuality = 0;
	m_BackBufferSize.cx = 0;
	m_BackBufferSize.cy = 0;
	m_BackBufferSizeBeforeScaling.cx = 0;
	m_BackBufferSizeBeforeScaling.cy = 0;
	m_iAdditionalSwapChain = -1;
	m_bLockableBackBuffer = false;
	m_bScalingEnabled = false;

	m_pLeftBackBufferBeforeScaling = NULL;
	m_pRightBackBufferBeforeScaling = NULL;
	m_pLeftBackBuffer = NULL;
	m_pRightBackBuffer = NULL;

	m_bClassWasRegistered = false;
	m_hS3DSecondWindow = 0;	
	m_bS3DSecondWindowVisible = false;
	ZeroMemory(&m_S3DWindowClass, sizeof(WNDCLASS));
	m_hPreviousPresentResult = S_OK;

	m_bWorkInWindow = false;
	m_bUseSwapChains = false;
	m_hDestWindowOverride = 0;
	m_pSourceRect = NULL;
	m_pDestRect = NULL;
	m_pDirtyRegion = NULL;
	m_dwFlags = 0;
	m_CurrentGap = 0;

	m_pBasePresenterData = 0;
	m_pPresenterData = 0;
	for( int i = 0; i<SHARED_TEXTURE_COUNT; ++i ) 
	{
		m_SharedHandle[i] = NULL;
		m_pSharedTexture[i] = NULL;
		m_pStagingSurface[i] = NULL;
	}

	m_nPresentCounter = 0;
	m_nSessionFrameCount = 0;
	m_nFrameTimeDelta.QuadPart = 0;
	m_nLastFrameTime.QuadPart = 0;
	m_fFPS = 0;
	m_nFPSTimeDelta.QuadPart = 0;
	m_nLastDropTime.QuadPart = 0;
	m_nFPSDropShowTimeLeft.QuadPart = 0;
	m_nScreensLagTime.QuadPart = 0;

	m_VertexSinglePass1Pass = m_Vertex;
	m_VertexSinglePass2Pass = m_Vertex;

	for(WORD i = 0; i< 256; i++)
	{
		m_SavedGammaRamp.red[i]   = m_CurrentRAMP.red[i]   = m_IdentityRAMP.red[i]   = i << 8 | i;
		m_SavedGammaRamp.green[i] = m_CurrentRAMP.green[i] = m_IdentityRAMP.green[i] = i << 8 | i;
		m_SavedGammaRamp.blue[i]  = m_CurrentRAMP.blue[i]  = m_IdentityRAMP.blue[i]  = i << 8 | i;
	}
	m_CurrentRAMPisIdentity = TRUE;

	m_StopPresentButton.selectButton('R');
	m_StopRenderButton.selectButton('T');
}

CBaseSwapChain::~CBaseSwapChain(void)
{
	if(m_hS3DSecondWindow)
		DestroyWindow(m_hS3DSecondWindow);

	if(m_bClassWasRegistered)
		UnregisterClass(S3D_WINDOW_CLASS_NAME, (HINSTANCE)gData.hModule);

	SAFE_DELETE( m_pBasePresenterData );
	m_pPresenterData = 0;
}

HRESULT CBaseSwapChain::Initialize(IDirect3DSwapChain9* pSwapChain)
{
	INSIDE;
	HRESULT hResult = S_OK;
	g_pSwapChainWrapperList.AddWrapper(pSwapChain, this);
	m_SwapChain.initialize(pSwapChain);
	if(gInfo.RouterType == ROUTER_TYPE_HOOK)
		HookIDirect3DSwapChain9All();

	NSCALL(pSwapChain->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO,	&m_pPrimaryBackBuffer));
	GetBaseDevice()->m_nRenderTargetCounter++;

	INC_DEBUG_COUNTER(GetBaseDevice()->m_nRenderTargetsMonoSize, GetSurfaceSize(m_pPrimaryBackBuffer));

	if (GetBaseDevice()->m_bTwoWindows)
	{
		switch(GetBaseDevice()->m_DeviceMode)
		{
		case DEVICE_MODE_FORCEFULLSCREEN:
			if (GetD3D9Device().GetNumberOfSwapChains() > 1)
			{
				m_iAdditionalSwapChain = 1;
				NSCALL(GetD3D9Device().GetSwapChain(1, &m_pAdditionalSwapChain));
				NSCALL(GetD3D9Device().GetBackBuffer(1, 0, D3DBACKBUFFER_TYPE_MONO,	&m_pSecondaryBackBuffer));
			}
			break;
		case DEVICE_MODE_FORCEWINDOWED:
			NSCALL(GetD3D9Device().CreateAdditionalSwapChain(&m_PresentationParameters[1], &m_pAdditionalSwapChain));
			if(m_pAdditionalSwapChain)
			{
				m_iAdditionalSwapChain = 1;
				NSCALL(m_pAdditionalSwapChain->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &m_pSecondaryBackBuffer));
			}
			break;
		default:
			break;
		}
	}

	INC_DEBUG_COUNTER(GetBaseDevice()->m_nRenderTargetsStereoSize, GetSurfaceSize(m_pSecondaryBackBuffer));

	D3DSURFACE_DESC RenderTargetDesc;
	ZeroMemory( &RenderTargetDesc, sizeof( RenderTargetDesc ) );
	NSCALL(m_pPrimaryBackBuffer->GetDesc( &RenderTargetDesc ));
	SetStereoObject(m_pPrimaryBackBuffer, m_pSecondaryBackBuffer);
	if (DEBUGVER)
	{
		TCHAR buffer[128];
		_sntprintf_s(buffer, _TRUNCATE, _T("PrimaryBackBuffer-SC%d"), m_Index);
		SetObjectName(m_pPrimaryBackBuffer, buffer);
	}

	NSCALL_TRACE_RES(GetD3D9Device().CreateTexture(m_BackBufferSize.cx, m_BackBufferSize.cy, 1, 
		D3DUSAGE_RENDERTARGET, RenderTargetDesc.Format, D3DPOOL_DEFAULT, 
		&m_pLeftMethodTexture, 0), m_pLeftMethodTexture,
		DEBUG_MESSAGE(_T("CreateTexture(Width = %d, Height = %d, Levels = %d, Usage = %s, ")
		_T("Format = %s, Pool = %s, *ppTexture = %p, pSharedHandle = %p)"), 
		m_BackBufferSize.cx, m_BackBufferSize.cy, 1, GetUsageString(D3DUSAGE_RENDERTARGET), 
		GetFormatString(RenderTargetDesc.Format), GetPoolString(D3DPOOL_DEFAULT), 
		m_pLeftMethodTexture.p, 0));
	NSCALL_TRACE_RES(GetD3D9Device().CreateTexture(m_BackBufferSize.cx, m_BackBufferSize.cy, 1, 
		D3DUSAGE_RENDERTARGET, RenderTargetDesc.Format, D3DPOOL_DEFAULT, 
		&m_pRightMethodTexture, 0), m_pRightMethodTexture,
		DEBUG_MESSAGE(_T("CreateTexture(Width = %d, Height = %d, Levels = %d, Usage = %s, ")
		_T("Format = %s, Pool = %s, *ppTexture = %p, pSharedHandle = %p)"), 
		m_BackBufferSize.cx, m_BackBufferSize.cy, 1, GetUsageString(D3DUSAGE_RENDERTARGET), 
		GetFormatString(RenderTargetDesc.Format), GetPoolString(D3DPOOL_DEFAULT), 
		m_pRightMethodTexture.p, 0));
	INC_DEBUG_COUNTER(GetBaseDevice()->m_nRenderTargetsStereoSize, GetTextureSize(m_pLeftMethodTexture));
	INC_DEBUG_COUNTER(GetBaseDevice()->m_nRenderTargetsStereoSize, GetTextureSize(m_pRightMethodTexture));
	SetStereoObject(m_pLeftMethodTexture, m_pRightMethodTexture);
	if (DEBUGVER)
	{
		TCHAR buffer[128];
		_sntprintf_s(buffer, _TRUNCATE, _T("MethodTexture-SC%d"), m_Index);

		SetObjectName(m_pLeftMethodTexture, buffer);
		SetObjectName(m_pRightMethodTexture, buffer);
	}

	if (USE_UM_PRESENTER)
	{
		SetUMEvent UMEvent(GetBaseDevice(), reCreatingDoubleBuffer);
		NSCALL_TRACE_RES(GetD3D9Device().CreateRenderTarget(m_BackBufferSize.cx, 2 * m_BackBufferSize.cy, 
			RenderTargetDesc.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &m_pWidePresenterSurface, 0), m_pWidePresenterSurface,
			DEBUG_MESSAGE(_T("CreateRenderTarget(Width = %d, Height = %d, Format = %s, ")
			_T("Multisample = %s, MSQuality = %d, Lockable = %d, *ppSurface = %p, pSharedHandle = %p)"), 
			m_BackBufferSize.cx, 2 * m_BackBufferSize.cy, GetFormatString(RenderTargetDesc.Format), 
			GetMultiSampleString(D3DMULTISAMPLE_NONE), 0, FALSE, m_pWidePresenterSurface.p, 0));
	}
	
	if (m_PresentationParameters->EnableAutoDepthStencil)
	{
		NSCALL(GetD3D9Device().GetDepthStencilSurface(&m_pPrimaryDepthStencil));
		if (m_pPrimaryDepthStencil)
			GetBaseDevice()->m_nDepthStencilSurfaceCounter++;
		INC_DEBUG_COUNTER(GetBaseDevice()->m_nDepthStencilSurfacesMonoSize, GetSurfaceSize(m_pPrimaryDepthStencil));
	}

	InitializeMode(RenderTargetDesc);

	if(!m_bScalingEnabled)
	{
		m_pLeftBackBuffer = m_pLeftBackBufferBeforeScaling;
		m_pRightBackBuffer = m_pRightBackBufferBeforeScaling;
		m_LeftViewRect  = m_LeftViewRectBeforeScaling;
		m_RightViewRect = m_RightViewRectBeforeScaling;
		m_BBOffset = m_BBOffsetBeforeScaling;
	}

	if (GetBaseDevice()->m_DeviceMode == DEVICE_MODE_FORCEFULLSCREEN && m_pSecondaryBackBuffer)
	{
		NSCALL(GetD3D9Device().SetRenderTarget(0, m_pSecondaryBackBuffer));
		NSCALL(GetD3D9Device().Clear(0, NULL, D3DCLEAR_TARGET, GetBaseDevice()->m_pOutputMethod->InitSecondBBColor(), 0.0f, 0));
		if (m_pAdditionalSwapChain)
			m_pAdditionalSwapChain->Present(0, 0, 0, 0, 0);
	}

	return hResult;
}

void CBaseSwapChain::Clear()
{
	RestoreDeviceMode();

	m_iAdditionalSwapChain = -1;
	m_pAdditionalSwapChain.Release();

	m_pLeftBackBufferBeforeScaling = NULL;
	m_pRightBackBufferBeforeScaling = NULL;
	m_pLeftBackBuffer = NULL;
	m_pRightBackBuffer = NULL;

	m_pPrimaryBackBuffer.Release();
	m_pSecondaryBackBuffer.Release();
	m_pLeftMethodTexture.Release();
	m_pRightMethodTexture.Release();
	m_pPrimaryDepthStencil.Release();
	m_pSecondaryDepthStencil.Release();

	m_pWidePresenterSurface.Release();

	m_pLeftDepthStencilTexture.Release();
	m_pRightDepthStencilTexture.Release();
	m_pMainDepthStencilSurface.Release();
	m_pLeftDepthStencilCorrectTexture.Release();
	m_pRightDepthStencilCorrectTexture.Release();

	m_GammaRAMPTexture.Release();

	m_ShiftFinder.Clear();
#ifndef DISABLE_WIZARD
	m_WizardData.Clear();
#endif
	GetBaseDevice()->m_HotKeyOSD.Clear();
	if (GetBaseDevice()->m_pPresenter)
	{
		GetBaseDevice()->m_pPresenter->SetAction(ptClearSCData, this);
		if (m_pPresenterData)
			ClearSharedTextures();
		GetBaseDevice()->m_pPresenter->WaitForActionDone();
	}

	m_ScalingHook.reset();
	
	if( gInfo.RouterType == ROUTER_TYPE_HOOK)
	{
		UnHookIDirect3DSwapChain9All();
		DEBUG_MESSAGE(_T("Count of hooked functions after IDirect3DSwapChain9 wrapper realising - %d\n"), g_UniqueHooks.size());
	}
	g_pSwapChainWrapperList.RemoveWrapper(this);
}

void CBaseSwapChain::RestoreDeviceMode( )
{
	if(GetBaseDevice()->m_DeviceMode == DEVICE_MODE_FORCEWINDOWED && !m_bWorkInWindow)
	{
		DEBUG_MESSAGE(_T("Restore original resolution\n"));
		if (m_DevScreen1.cx != 0 && m_DevScreen1.cy != 0)
		{
			MONITORINFOEX mi;
			mi.cbSize = sizeof(MONITORINFOEX);
			HMONITOR hMonitor1 = GetMonitorHandle(0);
			GetMonitorInfo(hMonitor1, &mi);				
			DEVMODE deviceMode;
			deviceMode.dmSize = sizeof(DEVMODE);
			deviceMode.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT;
			EnumDisplaySettings(mi.szDevice, ENUM_CURRENT_SETTINGS, &deviceMode);

			deviceMode.dmPelsWidth = m_DevScreen1.cx;
			deviceMode.dmPelsHeight = m_DevScreen1.cy;
			ChangeDisplaySettingsEx(mi.szDevice, &deviceMode, NULL, CDS_FULLSCREEN, NULL);
		}
		if (m_DevScreen2.cx != 0 && m_DevScreen2.cy != 0)
		{
			MONITORINFOEX mi2;
			mi2.cbSize = sizeof(MONITORINFOEX);
			HMONITOR hMonitor2 = GetMonitorHandle(1);
			GetMonitorInfo(hMonitor2, &mi2);
			DEVMODE deviceMode2;
			deviceMode2.dmSize = sizeof(DEVMODE);
			deviceMode2.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT;
			EnumDisplaySettings(mi2.szDevice, ENUM_CURRENT_SETTINGS, &deviceMode2);

			deviceMode2.dmPelsWidth = m_DevScreen2.cx;
			deviceMode2.dmPelsHeight = m_DevScreen2.cy;
			ChangeDisplaySettingsEx(mi2.szDevice, &deviceMode2, NULL, CDS_FULLSCREEN, NULL);
		}
	}
}

HRESULT CBaseSwapChain::ModifyPresentParameters()
{
	HRESULT hResult = S_OK;

	if (GetBaseDevice()->m_pPresenterThread)
		GetBaseDevice()->m_pPresenterThread->SetPresentationParameters(&m_OriginalPresentationParameters);

	// for Reset()
	m_hDestWindowOverride = 0;

	//--- hDeviceWindow may change after Reset() ---
	m_PresentationParameters[0] = m_OriginalPresentationParameters;
	if (m_pFullscreenDisplayMode)
	{
		m_FullscreenDisplayMode[0] = m_OriginalFullscreenDisplayMode;
		m_pFullscreenDisplayMode = m_FullscreenDisplayMode;
	}
	else if (m_SwapChain.getExMode() == EXMODE_EMULATE)
	{
		m_OriginalFullscreenDisplayMode.Size = sizeof( D3DDISPLAYMODEEX );
		m_OriginalFullscreenDisplayMode.Width				= m_OriginalPresentationParameters.BackBufferWidth;
		m_OriginalFullscreenDisplayMode.Height				= m_OriginalPresentationParameters.BackBufferHeight;
		m_OriginalFullscreenDisplayMode.RefreshRate			= m_OriginalPresentationParameters.FullScreen_RefreshRateInHz;
		m_OriginalFullscreenDisplayMode.Format				= m_OriginalPresentationParameters.BackBufferFormat;
		m_OriginalFullscreenDisplayMode.ScanLineOrdering	= D3DSCANLINEORDERING_PROGRESSIVE;
		m_FullscreenDisplayMode[0] = m_OriginalFullscreenDisplayMode;
		if (!m_OriginalPresentationParameters.Windowed)
			m_pFullscreenDisplayMode = m_FullscreenDisplayMode;
	}

	if (GetBaseDevice()->m_bTwoWindows)
	{
		m_PresentationParameters[0].Flags &= ~D3DPRESENTFLAG_DEVICECLIP;
	}

	if (!gInfo.EnableAA)
	{
		m_MultiSampleType = D3DMULTISAMPLE_NONE;
		m_MultiSampleQuality = 0;
	}
	else
	{
		m_MultiSampleType = m_PresentationParameters[0].MultiSampleType;
		m_MultiSampleQuality = m_PresentationParameters[0].MultiSampleQuality;
	}

	if (USE_IZ3DKMSERVICE_PRESENTER)
	{
		m_PresentationParameters[0].BackBufferCount = 3;
		m_PresentationParameters[0].PresentationInterval = D3DPRESENT_INTERVAL_ONE;
		m_PresentationParameters[0].SwapEffect= D3DSWAPEFFECT_FLIP;
	}

	if (USE_ATI_PRESENTER)
	{
		m_PresentationParameters[0].MultiSampleType = D3DMULTISAMPLE_2_SAMPLES;
		m_PresentationParameters[0].MultiSampleQuality = 0;
		m_PresentationParameters[0].SwapEffect	= D3DSWAPEFFECT_DISCARD;
	} 
	else if (!gInfo.EnableAA || !(GetBaseDevice()->m_pOutputMethod->GetCaps() & ocRenderDirectlyToBB) || gInfo.WideRenderTarget)
	{
		m_PresentationParameters[0].MultiSampleType = D3DMULTISAMPLE_NONE;
		m_PresentationParameters[0].MultiSampleQuality = 0;
	}

	if (gInfo.ForceVSyncOff)
	{
		m_PresentationParameters[0].PresentationInterval &= ~(D3DPRESENT_INTERVAL_ONE | D3DPRESENT_INTERVAL_TWO | 
			D3DPRESENT_INTERVAL_THREE | D3DPRESENT_INTERVAL_FOUR);
		m_PresentationParameters[0].PresentationInterval |= D3DPRESENT_INTERVAL_IMMEDIATE;
	}

	if (GetBaseDevice()->m_pOutputMethod->HandleGamma())
	{
		D3DDISPLAYMODE mode;
		GetBaseDevice()->m_pDirect3D->GetAdapterDisplayMode(GetBaseDevice()->m_nAdapter[0], &mode);
		if (FAILED(GetBaseDevice()->m_pDirect3D->CheckDeviceFormat(GetBaseDevice()->m_nAdapter[0], GetBaseDevice()->m_DeviceType, mode.Format,
			D3DUSAGE_RENDERTARGET | D3DUSAGE_QUERY_SRGBREAD | D3DUSAGE_QUERY_SRGBWRITE, D3DRTYPE_TEXTURE, m_PresentationParameters[0].BackBufferFormat)))
		{
			DEBUG_MESSAGE(_T("Backbuffer format replaced by D3DFMT_A8R8G8B8\n"));
			// for 16-bit BB force 32-bit format
			m_PresentationParameters[0].BackBufferFormat = D3DFMT_A8R8G8B8;
		}
	}

	m_PresentationParameters[0].Flags &= ~(D3DPRESENTFLAG_DISCARD_DEPTHSTENCIL); // ?!?
	if (GINFO_LOCKABLEDEPTHSTENCIL)								    
	{
		m_PresentationParameters[0].AutoDepthStencilFormat = D3DFMT_D16_LOCKABLE;
	}
	m_bLockableBackBuffer = (m_PresentationParameters[0].Flags & D3DPRESENTFLAG_LOCKABLE_BACKBUFFER) && (m_MultiSampleType == D3DMULTISAMPLE_NONE);

	if (USE_ATI_PRESENTER)
		m_PresentationParameters[0].Flags &= ~D3DPRESENTFLAG_LOCKABLE_BACKBUFFER;
	m_bWorkInWindow = FALSE;
	switch(GetBaseDevice()->m_DeviceMode)
	{
	case DEVICE_MODE_FORCEWINDOWED:
		if (!gInfo.DisableFullscreenModeEmulation)
			m_bWorkInWindow = m_OriginalPresentationParameters.Windowed != 0;
		else
			m_bWorkInWindow = true;
		m_PresentationParameters[0].Windowed = true;
		m_PresentationParameters[0].FullScreen_RefreshRateInHz = 0;
		m_pFullscreenDisplayMode = NULL;

		m_DevScreen1.cx = 0;
		m_DevScreen1.cy = 0;
		m_DevScreen2.cx = 0;
		m_DevScreen2.cy = 0;

		ModifyParamsByOutput();

		if (GetBaseDevice()->m_bTwoWindows)
		{
#ifdef FINAL_RELEASE
			if(GetBaseDevice()->m_NumberOfAdaptersInGroup > 1)
#endif
			{          
				if(m_bWorkInWindow)
				{
					MONITORINFOEX mi;
					mi.cbSize = sizeof(MONITORINFOEX);
					HMONITOR hMonitor1 = GetMonitorHandle(0);
					GetMonitorInfo(hMonitor1, &mi);				
					DEVMODE deviceMode;
					deviceMode.dmSize = sizeof(DEVMODE);
					deviceMode.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL | DM_DISPLAYFREQUENCY;
					EnumDisplaySettings(mi.szDevice, ENUM_CURRENT_SETTINGS, &deviceMode);

					MONITORINFOEX mi2;
					mi2.cbSize = sizeof(MONITORINFOEX);
					HMONITOR hMonitor2 = GetMonitorHandle(1);
					GetMonitorInfo(hMonitor2, &mi2);
					DEVMODE deviceMode2;
					deviceMode2.dmSize = sizeof(DEVMODE);
					deviceMode2.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL | DM_DISPLAYFREQUENCY;
					EnumDisplaySettings(mi2.szDevice, ENUM_CURRENT_SETTINGS, &deviceMode2);

					if(deviceMode.dmBitsPerPel != deviceMode2.dmBitsPerPel ||
						deviceMode.dmPelsWidth != deviceMode2.dmPelsWidth ||
						deviceMode.dmPelsHeight != deviceMode2.dmPelsHeight)
					{
						DEBUG_MESSAGE(_T("WARNING: Different display mode for secondary adapter\n"));
						m_DevScreen2.cx = deviceMode2.dmPelsWidth;
						m_DevScreen2.cy = deviceMode2.dmPelsHeight;
						DEBUG_MESSAGE(_T("Set display mode on second adapter\n"));
						//ChangeDisplaySettingsEx((LPSTR)mi.szDevice, &deviceMode, NULL, CDS_FULLSCREEN, NULL);
						if (deviceMode2.dmPosition.x < 0)
							deviceMode2.dmPosition.x += deviceMode2.dmPelsWidth - deviceMode.dmPelsWidth;
						deviceMode.dmPosition = deviceMode2.dmPosition;
						ChangeDisplaySettingsEx((LPTSTR)mi2.szDevice, &deviceMode, NULL, CDS_FULLSCREEN, NULL);
					} else if(deviceMode.dmDisplayFrequency != deviceMode2.dmDisplayFrequency)
					{
						DEBUG_MESSAGE(_T("WARNING: Refresh rate doesn't match\n"));
					}
				}

				RECT rcClient;
				GetSecondaryWindowRect(&m_S3DSecondWindowRect, &rcClient);
				SendOrPostMessage(SWM_MOVE_SECOND_WINDOW);

				m_PresentationParameters[1] = m_PresentationParameters[0];
				m_PresentationParameters[1].hDeviceWindow = m_hS3DSecondWindow;
				GetBaseDevice()->m_BehaviorFlags &= ~D3DCREATE_ADAPTERGROUP_DEVICE;
			}
#ifdef FINAL_RELEASE
			else
				GetBaseDevice()->m_bTwoWindows = false;
#else
			if(GetBaseDevice()->m_NumberOfAdaptersInGroup == 1)
				DEBUG_TRACE1(_T("Warning: D3DCaps.NumberOfAdaptersInGroup = %d.\n"), GetBaseDevice()->m_NumberOfAdaptersInGroup);
#endif
		}
		if(!m_bWorkInWindow)
		{
			DEBUG_MESSAGE(_T("Emulate fullscreen mode\n"));
			GetBaseDevice()->m_bDebugWarningEmulateFullscreenMode = TRUE;
			MONITORINFOEX mi;
			mi.cbSize = sizeof(MONITORINFOEX);
			HMONITOR hMonitor1 = GetMonitorHandle(0);
			GetMonitorInfo(hMonitor1, &mi);				
			DEVMODE deviceMode;
			deviceMode.dmSize = sizeof(DEVMODE);
			deviceMode.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT;
			EnumDisplaySettings(mi.szDevice, ENUM_CURRENT_SETTINGS, &deviceMode);

			if (deviceMode.dmPelsWidth != m_PresentationParameters[0].BackBufferWidth || deviceMode.dmPelsHeight != m_PresentationParameters[0].BackBufferHeight)
			{
				m_DevScreen1.cx = deviceMode.dmPelsWidth;
				m_DevScreen1.cy = deviceMode.dmPelsHeight;
				deviceMode.dmPelsWidth = m_PresentationParameters[0].BackBufferWidth;
				deviceMode.dmPelsHeight = m_PresentationParameters[0].BackBufferHeight;
				ChangeDisplaySettingsEx(mi.szDevice, &deviceMode, NULL, CDS_FULLSCREEN, NULL);
			}

			if (GetBaseDevice()->m_bTwoWindows)
			{
				MONITORINFOEX mi2;
				mi2.cbSize = sizeof(MONITORINFOEX);
				HMONITOR hMonitor2 = GetMonitorHandle(1);
				GetMonitorInfo(hMonitor2, &mi2);
				DEVMODE deviceMode2;
				deviceMode2.dmSize = sizeof(DEVMODE);
				deviceMode2.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT;
				EnumDisplaySettings(mi2.szDevice, ENUM_CURRENT_SETTINGS, &deviceMode2);

				if (deviceMode2.dmPelsWidth != m_PresentationParameters[1].BackBufferWidth || deviceMode2.dmPelsHeight != m_PresentationParameters[1].BackBufferHeight)
				{
					m_DevScreen2.cx = deviceMode2.dmPelsWidth;
					m_DevScreen2.cy = deviceMode2.dmPelsHeight;
					deviceMode2.dmPelsWidth = m_PresentationParameters[1].BackBufferWidth;
					deviceMode2.dmPelsHeight = m_PresentationParameters[1].BackBufferHeight;
					ChangeDisplaySettingsEx(mi2.szDevice, &deviceMode2, NULL, CDS_FULLSCREEN, NULL);
				}
			}
		}

		if(m_OriginalPresentationParameters.Windowed == FALSE)
			SendOrPostMessage(SWM_MOVE_APP_WINDOW);
		else if (GetBaseDevice()->m_bTwoWindows)
			SendOrPostMessage(SWM_RESTORE_APP_WINDOW);
		break;
	case DEVICE_MODE_FORCEFULLSCREEN:
		{
			m_PresentationParameters[0].Windowed = FALSE;
			m_pFullscreenDisplayMode = m_FullscreenDisplayMode;
			if(m_OriginalPresentationParameters.Windowed == TRUE)
			{
				if(m_PresentationParameters[0].BackBufferFormat == D3DFMT_UNKNOWN)
					m_PresentationParameters[0].BackBufferFormat = D3DFMT_A8R8G8B8;
				if (m_PresentationParameters[0].BackBufferWidth == 0 && m_PresentationParameters[0].BackBufferHeight == 0)
				{
					HWND hAppWindow = GetAppWindow();
					RECT rc;
					GetClientRect(hAppWindow, &rc);
					m_PresentationParameters[0].BackBufferWidth = rc.right - rc.left;
					m_PresentationParameters[0].BackBufferHeight = rc.bottom - rc.top;
				}
				GetBaseDevice()->FindBestFullScreenResolution(m_PresentationParameters[0].BackBufferWidth, m_PresentationParameters[0].BackBufferHeight, m_PresentationParameters[0].BackBufferFormat);
			}

			ModifyParamsByOutput();

			if (GetBaseDevice()->m_bTwoWindows)
			{
				if (GetBaseDevice()->m_NumberOfAdaptersInGroup == 2)
				{
					m_PresentationParameters[1] = m_PresentationParameters[0];
					m_PresentationParameters[1].hDeviceWindow = m_hS3DSecondWindow;
					m_FullscreenDisplayMode[1] = m_FullscreenDisplayMode[0];
					GetBaseDevice()->m_BehaviorFlags |= D3DCREATE_ADAPTERGROUP_DEVICE;
					SetRect(&m_S3DSecondWindowRect, 0, 0, m_PresentationParameters[1].BackBufferWidth, m_PresentationParameters[1].BackBufferHeight);
					SendOrPostMessage(SWM_MOVE_SECOND_WINDOW);
				}
				else
				{
					GetBaseDevice()->m_bTwoWindows = false;
					DEBUG_TRACE1(_T("Warning: D3DCaps.NumberOfAdaptersInGroup = %d. Disable second window.\n"), GetBaseDevice()->m_NumberOfAdaptersInGroup);
				}
			}
		}
		break;
	default:
		ModifyParamsByOutput();
		break;
	}

	//--- 0.5.0 task #77: auto-enable GPU scaling for sub-native fullscreen ---
	// Mirrors the DX11 Stereo-Injector's task #32 pattern (SwapChain11Proxy::
	// EnsureStereoBackBuffer): when the game picks a sub-native fullscreen BB
	// and no scaling mode is configured, override to SCALEDASPECT with panel-
	// native dims so SR Weave (and other per-panel-pixel output modes) land
	// 1:1 on the display. ScalingHook handles the game-side lies about
	// window/cursor coords. QueryPanelNativeSize prefers the LeiaSR Displays
	// SDK and falls back to the OS monitor the focus window lives on, so
	// multi-monitor games on secondary panels get the right target dims.
	if (gInfo.DisplayScalingMode == 0 &&
		!m_PresentationParameters[0].Windowed &&
		m_PresentationParameters[0].BackBufferWidth  > 1 &&
		m_PresentationParameters[0].BackBufferHeight > 1)
	{
		HWND focus = m_PresentationParameters[0].hDeviceWindow;
		int panelW = 0, panelH = 0;
		if (focus && wiz3d::QueryPanelNativeSize(focus, panelW, panelH) &&
			panelW > 0 && panelH > 0 &&
			((UINT)panelW != m_PresentationParameters[0].BackBufferWidth ||
			 (UINT)panelH != m_PresentationParameters[0].BackBufferHeight))
		{
			gInfo.DisplayNativeWidth  = (UINT)panelW;
			gInfo.DisplayNativeHeight = (UINT)panelH;
			gInfo.DisplayScalingMode  = DISPLAY_SCALING_SCALEDASPECT;   // stretch preserving aspect; letterbox otherwise
			DEBUG_MESSAGE(_T("DX9 task #77: auto-enabling panel-native scaling for sub-native fullscreen BB %ux%u -> panel %dx%d\n"),
				m_PresentationParameters[0].BackBufferWidth,
				m_PresentationParameters[0].BackBufferHeight,
				panelW, panelH);
		}
	}

	//--- enable GPU scaling ---
	m_bScalingEnabled = false;
	m_BackBufferSizeBeforeScaling = m_BackBufferSize;
	if(	gInfo.DisplayScalingMode && (!m_PresentationParameters[0].Windowed) &&
		(m_PresentationParameters[0].BackBufferWidth > 1 && m_PresentationParameters[0].BackBufferHeight > 1) )
	{
		UINT width = gInfo.DisplayNativeWidth;
		UINT height = gInfo.DisplayNativeHeight;
		if(m_PresentationParameters[0].BackBufferWidth != width 
		   || m_PresentationParameters[0].BackBufferHeight != height)
		{
			m_bScalingEnabled = true;
			m_PresentationParameters[0].BackBufferWidth = width;
			m_PresentationParameters[0].BackBufferHeight = height;
			m_PresentationParameters[1].BackBufferWidth = width;
			m_PresentationParameters[1].BackBufferHeight = height;
			m_BackBufferSize.cx = width;
			m_BackBufferSize.cy = height;
			if (m_SwapChain.getExMode() == EXMODE_EMULATE)
			{
				m_FullscreenDisplayMode[0].Width = width;
				m_FullscreenDisplayMode[0].Height = height;
				m_FullscreenDisplayMode[1].Width = width;
				m_FullscreenDisplayMode[1].Height = height;
			}

			m_ScalingHook = ScalingAgent::Instance()->InitializeHook( TDisplayScalingMode(gInfo.DisplayScalingMode), m_BackBufferSizeBeforeScaling, m_BackBufferSize );
		}
		else {
			m_ScalingHook.reset();
		}
	}
	else {
		m_ScalingHook.reset();
	}

	if (USE_SIMPLE_PRESENTER)
	{
		m_PresentationParameters[0].PresentationInterval = D3DPRESENT_INTERVAL_ONE;
		if (m_PresentationParameters[0].SwapEffect != D3DSWAPEFFECT_COPY)
			m_PresentationParameters[0].BackBufferCount = 2; // Triple buffering
	}

	if (USE_ATI_PRESENTER)
	{
		m_PresentationParameters[0].PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
	}

	if (USE_UM_PRESENTER)
	{
		m_PresentationParameters[0].PresentationInterval = D3DPRESENT_INTERVAL_ONE;
		if (m_PresentationParameters[0].SwapEffect != D3DSWAPEFFECT_COPY)
		{
			if ( !m_PresentationParameters[0].Windowed )
				m_PresentationParameters[0].BackBufferCount = 2; // Triple buffering for fullscreen mode
			else
				m_PresentationParameters[0].BackBufferCount = 1; // Double buffering for windowed mode
		}
	}

	if (!m_PresentationParameters[0].Windowed &&
		(USE_ATI_PRESENTER || USE_UM_PRESENTER || USE_SIMPLE_PRESENTER || USE_IZ3DKMSERVICE_PRESENTER))
	{
		m_PresentationParameters[0].FullScreen_RefreshRateInHz = GetBaseDevice()->GetShutterRefreshRate(m_PresentationParameters[0].FullScreen_RefreshRateInHz);
		m_FullscreenDisplayMode[0].RefreshRate = m_PresentationParameters[0].FullScreen_RefreshRateInHz;
	}

	m_bUseSwapChains = m_PresentationParameters[0].Windowed != 0;
	DEBUG_MESSAGE(_T("BackBufferBeforeScaling Width: %d Height: %d\n"), m_BackBufferSizeBeforeScaling.cx, m_BackBufferSizeBeforeScaling.cy);
	DEBUG_MESSAGE(_T("BackBuffer Width: %d Height: %d\n"), m_BackBufferSize.cx, m_BackBufferSize.cy);

	DEBUG_MESSAGE(_T("Application PresentationParameters: %s\n"), GetPresentationParametersString(&m_OriginalPresentationParameters));
	DEBUG_MESSAGE(_T("Modified m_PresentationParameters[0]: %s\n"), GetPresentationParametersString(&m_PresentationParameters[0]));
	if (GetBaseDevice()->m_bTwoWindows)
	{
		DEBUG_MESSAGE(_T("Modified m_PresentationParameters[1]: %s\n"), GetPresentationParametersString(&m_PresentationParameters[1]));
	}

	return hResult;
}

void CBaseSwapChain::ModifyParamsByOutput()
{
	if (m_PresentationParameters[0].Windowed && m_PresentationParameters[0].BackBufferWidth == 0 && m_PresentationParameters[0].BackBufferHeight == 0)
	{
		_ASSERT(FALSE); // moved to CMonoSwapChain::SetPresentationParameters
	}
	m_BackBufferSize.cx = m_PresentationParameters[0].BackBufferWidth;
	m_BackBufferSize.cy = m_PresentationParameters[0].BackBufferHeight;
	GetBaseDevice()->m_pOutputMethod->ModifyPresentParameters(GetBaseDevice()->m_pDirect3D, GetBaseDevice()->m_nAdapter[0], m_PresentationParameters);
	if (m_BackBufferSize.cx != m_PresentationParameters[0].BackBufferWidth || m_BackBufferSize.cy != m_PresentationParameters[0].BackBufferHeight)
	{
		gInfo.SpanModeOn = TRUE;
		DEBUG_MESSAGE(_T("SpanMode ON\n"));
	}
}

HRESULT CBaseSwapChain::CreateSharedResources() 
{
	HRESULT		hResult = S_OK;

	for( UINT i = 0; i < SHARED_TEXTURE_COUNT; ++i )
	{
		_ASSERT( m_pSharedTexture[ i ] == NULL );
		_ASSERT( m_SharedHandle[ i ] == NULL );

		NSCALL(GetD3D9Device().CreateTexture(
			m_BackBufferSize.cx,
			m_BackBufferSize.cy,
			1,
			D3DUSAGE_RENDERTARGET,
			m_PresentationParameters[0].BackBufferFormat /*D3DFMT_X8R8G8B8*/,
			D3DPOOL_DEFAULT,
			&m_pSharedTexture[ i ],
			USE_MULTI_DEVICE_PRESENTER ? &m_SharedHandle	[ i ] : NULL
		));
		if( FAILED( hResult ) ) return hResult;
	}

	if (USE_MULTI_DEVICE_PRESENTER)
	{
		for( DWORD	i = 0; i<SHARED_TEXTURE_COUNT; ++i	)
		{
			NSCALL(GetD3D9Device().CreateRenderTarget(
				16,
				16,
				m_PresentationParameters[0].BackBufferFormat /*D3DFMT_X8R8G8B8*/,
				D3DMULTISAMPLE_NONE,
				0,
				TRUE,
				&m_pStagingSurface	[i],
				NULL ));
			if( FAILED(hResult) )
				return E_FAIL;
		}
	}

	return	S_OK;
}

void CBaseSwapChain::SendImageToPresenter()
{
	INSIDE;
	_ASSERT(USE_MULTI_DEVICE_PRESENTER || USE_MULTI_THREAD_PRESENTER);

	HRESULT hResult = S_OK;
	UINT IndexPair	= m_pPresenterData->GetRenderingPairIndex();
	UINT IndexLeft	= 2 * IndexPair + 0;
	UINT IndexRight	= 2 * IndexPair + 1;

	// get the surfaces and copy the data
	// TODO: change shared Textures to shared Surfaces
	CComPtr<IDirect3DSurface9>	pSurfaceLeft;
	NSCALL(m_pSharedTexture[ IndexLeft  ]->GetSurfaceLevel( 0, &pSurfaceLeft  ));
	// Literal: shared-texture pre-copy is internal data movement, not user output.
	// Applying IsSwapEyes here would cancel against the swap-aware accessor the
	// output method also uses, so SwapEyes toggled visibly nothing (SR) and
	// crashed WideSwapChain (SBS). project_dx9_swapeyes.md.
	NSCALL(GetD3D9Device().StretchRect( GetLeftBackBufferRT_Literal(),  GetLeftBackBufferRect_Literal(),
		pSurfaceLeft,  NULL, D3DTEXF_NONE ));

	CComPtr<IDirect3DSurface9>	pSurfaceRight;
	NSCALL(m_pSharedTexture[ IndexRight ]->GetSurfaceLevel( 0, &pSurfaceRight ));
	NSCALL(GetD3D9Device().StretchRect( GetRightBackBufferRT_Literal(), GetRightBackBufferRect_Literal(),
		pSurfaceRight, NULL, D3DTEXF_NONE ));

	if (USE_MULTI_DEVICE_PRESENTER)
	{
		RECT rect = { 0, 0, 16, 16 };
		NSCALL(GetD3D9Device().StretchRect( m_pStagingSurface[IndexLeft],  &rect, 
			pSurfaceLeft,  &rect, D3DTEXF_NONE ));

		NSCALL(GetD3D9Device().StretchRect( m_pStagingSurface[IndexRight],  &rect, 
			pSurfaceRight,  &rect, D3DTEXF_NONE ));

		UINT PresentImagePair = IndexPair;
		for(int i = 0; i < SHARED_TEXTURE_PAIR_COUNT; i++)
		{
			UINT CurIndexPair = SHARED_TEXTURE_PAIR_COUNT + IndexPair - i;
			CurIndexPair %= SHARED_TEXTURE_PAIR_COUNT;
			UINT IndexLeft	= 2 * CurIndexPair + 0;
			UINT IndexRight	= 2 * CurIndexPair + 1;

			// Attempt to lock the staging surface to see if the rendering
			// is complete.
			D3DLOCKED_RECT lockedRect;
			hResult = m_pStagingSurface[IndexLeft]->LockRect(&lockedRect, NULL, D3DLOCK_READONLY | D3DLOCK_DONOTWAIT);
			if (SUCCEEDED(hResult))
			{
				hResult = m_pStagingSurface[IndexLeft]->UnlockRect();

				// Attempt to lock the staging surface to see if the rendering
				// is complete.
				hResult = m_pStagingSurface[IndexRight]->LockRect(&lockedRect, NULL, D3DLOCK_READONLY | D3DLOCK_DONOTWAIT);
				if (SUCCEEDED(hResult))
				{
					hResult = m_pStagingSurface[IndexRight]->UnlockRect();

					PresentImagePair = CurIndexPair;
					break;
				}
			}
		}
		// pass the presented indices to the presenter thread, and update the presented indices 
		m_pPresenterData->SetPresentPairIndex( PresentImagePair );		// critical section is used
	}
	else
	{
		// pass the presented indices to the presenter thread, and update the presented indices 
		m_pPresenterData->SetPresentPairIndex( IndexPair );		// critical section is used
	}

	GetBaseDevice()->m_pPresenterThread->SetAction(ptRender);
}

BOOL CBaseSwapChain::SendOrPostMessage(WPARAM wParam)
{
	DEBUG_TRACE2(_T("SendOrPostMessage(%s)\n"), GetSecondWindowMessageString(wParam));
	if (wParam != SWM_MOVE_APP_WINDOW && GetBaseDevice()->IsCurrentThreadMain())
		return (BOOL)S3DWindowProc(m_hS3DSecondWindow, WM_USER, wParam, (LPARAM)this);
	else if (m_hS3DSecondWindow)
		return PostMessage(m_hS3DSecondWindow, WM_USER, wParam, (LPARAM)this);
	else return FALSE;
}

HRESULT CBaseSwapChain::InitWindows()
{
	HRESULT hResult = S_OK;

	if (GetBaseDevice()->m_pPresenterThread)
	{
		m_pBasePresenterData = GetBaseDevice()->m_pPresenter->CreateSwapChainData(this);
		m_pPresenterData = (CPresenterSCData*)m_pBasePresenterData;
	}
	else if (USE_UM_PRESENTER)
	{
		m_pBasePresenterData = DNew UMDShutterPresenterSCData(NULL, this);
	}

	//--- create second window ---
	if(!GetBaseDevice()->m_bTwoWindows)
		return hResult;

	DEBUG_MESSAGE(_T("SecondaryAdapterOrdinal = %u\n"), GetBaseDevice()->m_nAdapter[1]);

	if(!GetClassInfo((HINSTANCE)gData.hModule, S3D_WINDOW_CLASS_NAME, &m_S3DWindowClass))
	{
		ZeroMemory(&m_S3DWindowClass, sizeof(m_S3DWindowClass));
		m_S3DWindowClass.lpfnWndProc = S3DWindowProc; 
		m_S3DWindowClass.hInstance = (HINSTANCE)gData.hModule;
		m_S3DWindowClass.hCursor = LoadCursor(NULL, IDC_ARROW);
#ifdef DEBUG
		//--- we not need to redraw background ---
		m_S3DWindowClass.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);   
#endif
		m_S3DWindowClass.lpszClassName = S3D_WINDOW_CLASS_NAME;
		if(RegisterClass(&m_S3DWindowClass) == 0)
		{
			DWORD error = GetLastError();
			DEBUG_MESSAGE(_T("%s: S3DWindow class registration failed, eror code = %u\n"), _T( __FUNCTION__ ) , error);
		}
		else
			m_bClassWasRegistered = true;
	}
	//--- second window must be synchronously minimized and maximized this main window ---
	HWND hParent = GetAppWindow();
	//--- second window must be visible for swapchain drawing ---
	DWORD dwStyle = WS_POPUP;
	if(GetBaseDevice()->m_DeviceMode == DEVICE_MODE_FORCEFULLSCREEN) 
		hParent = HWND_MESSAGE;
#ifdef FINAL_RELEASE
	m_hS3DSecondWindow = CreateWindowEx(WS_EX_TOOLWINDOW|WS_EX_TOPMOST, S3D_WINDOW_CLASS_NAME, TEXT("S3D Driver Window"), 
		dwStyle, 0, 0, 1, 1, hParent, 0, (HINSTANCE)gData.hModule, 0);
#else
	m_hS3DSecondWindow = CreateWindowEx(WS_EX_TOOLWINDOW, S3D_WINDOW_CLASS_NAME, TEXT("S3D Driver Window"), 
		dwStyle, 0, 0, 1, 1, hParent, 0, (HINSTANCE)gData.hModule, 0);
#endif
	m_bS3DSecondWindowVisible = false;
	if(!m_hS3DSecondWindow)
	{
		DWORD error = GetLastError();
		DEBUG_MESSAGE(_T("%s: CreateWindowEx() failed, eror code = %u\n"), _T( __FUNCTION__ ) , error);
		return E_FAIL;
	}
	SetWindowLongPtr(m_hS3DSecondWindow, GWLP_USERDATA, (LONG_PTR)this);

	return hResult;
}

void CBaseSwapChain::ProcessDepthBuffer( )
{
	HRESULT hResult = S_OK;
	CheckDepthBuffer();
	/*GetBaseDevice()->SynchronizeDepthBuffer( this );

	if (m_bDepthBufferFounded && 
		m_PrevDrawCountAfterClearDepthBuffer < >m_DrawCountAfterClearDepthBuffer)
	{
		m_bDepthBufferFounded = FALSE;
		m_pLeftDepthStencilCorrectTexture.Release();
		m_pRightDepthStencilCorrectTexture.Release();
	}

	if (!m_bDepthBufferFounded && 
		m_pLeftDepthStencilTexture && m_pRightDepthStencilTexture)
	{
		m_bDepthBufferFounded = TRUE;
		m_pLeftDepthStencilCorrectTexture = m_pLeftDepthStencilTexture;
		m_pRightDepthStencilCorrectTexture = m_pRightDepthStencilTexture;
	}*/

#ifndef FINAL_RELEASE
	if (IsKeyDown(VK_OEM_MINUS) && m_bDepthBufferFounded)
	{
		vbs* vb;
		GetBaseDevice()->CaptureAll(vb);
		NSCALL(GetBaseDevice()->m_pCopyState->Apply());
		float oldValues[4];
		GetD3D9Device().GetPixelShaderConstantF( 0, oldValues, 1 );
		GetD3D9Device().SetPixelShader( GetBaseDevice()->m_pViewShader );
		std::vector<DWORD> histogram(258 + 8);
		GetBaseDevice()->GetMinMaxValues(m_fMinDepthValue, m_fMaxDepthValue, &histogram[0],
			m_pLeftDepthStencilCorrectTexture, 
			m_pRightDepthStencilCorrectTexture, 
			GetBaseDevice()->m_ScreenShotButton.isButtonWasPressed());
		for (int i = 0; i < 8; i++)
			m_MiniHistogram[i] = 1.0f * histogram[258 + i] / histogram[257];
		float newValues[4] = { m_fMinDepthValue, m_fMaxDepthValue, 
			1.0f / (m_fMaxDepthValue - m_fMinDepthValue), 0.0f };
		GetD3D9Device().SetPixelShaderConstantF( 0, newValues, 1 );
		DWORD rt[4] = { true, false, false, false };
		bool bStereoOn = GetBaseDevice()->RenderToRight() && GetBaseDevice()->RenderInStereo();
		if (bStereoOn)
		{
			GetBaseDevice()->m_pUpdateRTs = rt;
			GetBaseDevice()->m_UpdateDS = false;
			GetBaseDevice()->SetStereoRenderTarget<VIEWINDEX_RIGHT>();
			GetBaseDevice()->OneToOneRender(m_pRightDepthStencilCorrectTexture, NULL, GetBaseDevice()->m_pCurOffset);
			GetBaseDevice()->SetStereoRenderTarget<VIEWINDEX_LEFT>();
		}
		GetBaseDevice()->OneToOneRender(m_pLeftDepthStencilCorrectTexture, NULL, NULL);
		GetBaseDevice()->ApplyAll(vb);
		GetD3D9Device().SetPixelShaderConstantF( 0, oldValues, 1 );
	}
#endif

	m_bDepthBufferFounded = FALSE;
	m_DrawCountAfterClearDepthBuffer = 0;
	m_PrevDrawCountAfterClearDepthBuffer = 0;
}

HMONITOR CBaseSwapChain::GetMonitorHandle(int n)
{
	HMONITOR hMonitor = GetBaseDevice()->m_pDirect3D->GetAdapterMonitor(GetBaseDevice()->m_nAdapter[n]);
	return hMonitor;
}

void CBaseSwapChain::GetSecondaryWindowRect(RECT* rcWindow, RECT* rcClient)
{
	MONITORINFO miFirst; 
	miFirst.cbSize = sizeof(MONITORINFO);
	HMONITOR hMonitorFirst = GetMonitorHandle(0);
	GetMonitorInfo(hMonitorFirst, &miFirst);

	RECT primaryWindowRect;
	HWND hWnd = m_hDestWindowOverride ? m_hDestWindowOverride : GetAppWindow();
	DEBUG_TRACE2(_T("Application hDestWindowOverride = %s\n"), GetWindowRectString(m_hDestWindowOverride));
	DEBUG_TRACE2(_T("Application hFocusWindow = %s\n"), GetWindowRectString(GetBaseDevice()->m_hFocusWindow));
	DEBUG_TRACE2(_T("Application hDeviceWindow = %s\n"), GetWindowRectString(m_OriginalPresentationParameters.hDeviceWindow));
	GetClientRect(hWnd, &primaryWindowRect);
	POINT pt;
	pt.x = primaryWindowRect.left;
	pt.y = primaryWindowRect.top;
	ClientToScreen(hWnd, &pt);

	if (gInfo.MultiWindowsMode != MULTI_WINDOWS_MODE_SIDEBYSIDEWINDOWED)
	{
		DWORD c = GetBaseDevice()->m_pOutputMethod->GetSecondWindowCaps();
		if (c & swcInvertHorizontal)
			pt.x = miFirst.rcMonitor.right - (pt.x + primaryWindowRect.right - primaryWindowRect.left);
		if (c & swcInvertVertical)
			pt.y = miFirst.rcMonitor.bottom - (pt.y + primaryWindowRect.bottom -  primaryWindowRect.top);

		MONITORINFO mi; 
		mi.cbSize = sizeof(MONITORINFO);
		HMONITOR hMonitor = GetMonitorHandle(1);
		GetMonitorInfo(hMonitor, &mi);
#ifndef FINAL_RELEASE
		if (hMonitor == NULL)
		{
			hMonitor = hMonitorFirst;
			mi = miFirst;
			mi.rcMonitor.left += mi.rcMonitor.right;
			mi.rcMonitor.right += mi.rcMonitor.right;
		}
#endif
		rcClient->left   = pt.x + mi.rcMonitor.left - miFirst.rcMonitor.left;
		rcClient->top    = pt.y + mi.rcMonitor.top - miFirst.rcMonitor.top;
	}
	else
	{
		rcClient->left   = pt.x - miFirst.rcMonitor.left + 
			primaryWindowRect.right - primaryWindowRect.left + 10;
		rcClient->top    = pt.y;
	}

	rcClient->right  = rcClient->left + primaryWindowRect.right - primaryWindowRect.left;
	rcClient->bottom = rcClient->top + primaryWindowRect.bottom -  primaryWindowRect.top;

	*rcWindow = *rcClient;
	AdjustWindowRect(rcWindow, (DWORD)GetWindowLongPtr(m_hS3DSecondWindow, GWL_STYLE), FALSE);
}

void CBaseSwapChain::CheckSecondWindowPosition()
{
	if (!GetBaseDevice()->m_bTwoWindows)
	{
#ifndef FINAL_RELEASE
		if (IsDebuggerPresent())
			ClipCursor(NULL);
#endif
		return;
	}

	if (GetBaseDevice()->m_DeviceMode == DEVICE_MODE_FORCEWINDOWED)
	{
		//--- use SW_SHOWNA flag to save focus ---
		if (!m_bS3DSecondWindowVisible)
		{
			m_bS3DSecondWindowVisible = true;
			SendOrPostMessage(SWM_SHOW_SECOND_WINDOW);
		}
		//--- synchronize secondary window position ---
		RECT rcClient, rcFront;
		GetSecondaryWindowRect(&m_S3DSecondWindowRect, &rcClient);
		GetWindowRect(m_hS3DSecondWindow, &rcFront);
		if(!EqualRect(&m_S3DSecondWindowRect, &rcFront))
			SendOrPostMessage(SWM_MOVE_SECOND_WINDOW);
	}
	if (!m_PresentationParameters[0].Windowed) // fullscreen
	{
		RECT rcWindow;
		RECT primaryWindowRect;
		HWND hWnd = m_hDestWindowOverride ? m_hDestWindowOverride : GetAppWindow();
		GetClientRect(hWnd, &primaryWindowRect);
		POINT pt;
		pt.x = primaryWindowRect.left;
		pt.y = primaryWindowRect.top;
		ClientToScreen(hWnd, &pt);
		rcWindow.left   = pt.x;
		rcWindow.top    = pt.y;
		rcWindow.right  = rcWindow.left + primaryWindowRect.right - primaryWindowRect.left;
		rcWindow.bottom = rcWindow.top + primaryWindowRect.bottom -  primaryWindowRect.top;

		DEBUG_TRACE2(_T("Clip rect = %s\n"), GetRectString(&rcWindow));
#ifndef FINAL_RELEASE
		if (IsDebuggerPresent())
			ClipCursor(NULL);
		else
#endif
			ClipCursor(&rcWindow);
		if (GetBaseDevice()->m_pVar)
			*GetBaseDevice()->m_pVar = GetCurrentProcessId();
	}
}

HRESULT	SaveScreenshotToFile(LPTSTR pDestFile, LPDIRECT3DSURFACE9 pSrcSurface, D3DXIMAGE_FILEFORMAT DestFormat)
{
	INSIDE;
	HRESULT hResult = S_OK;
	D3DSURFACE_DESC desc;
	NSCALL(pSrcSurface->GetDesc(&desc));
	if (desc.Type != D3DRTYPE_SURFACE)
		return E_FAIL;
	CComPtr<IDirect3DDevice9>  pDevice;
	CComPtr<IDirect3DSurface9> offScreenSurface;
	NSCALL(pSrcSurface->GetDevice(&pDevice));
	if (desc.Usage & D3DUSAGE_RENDERTARGET)
	{
		if (desc.MultiSampleType != D3DMULTISAMPLE_NONE)
			return E_FAIL;
		NSCALL(pDevice->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM, &offScreenSurface, 0));
		if (SUCCEEDED(hResult)) {
			NSCALL(pDevice->GetRenderTargetData(pSrcSurface, offScreenSurface));
		}
	}
	else 
		offScreenSurface = pSrcSurface;
	if (DestFormat == D3DXIFF_FORCE_DWORD)
	{
		DestFormat = GetImageFileFormat( );
		_tcscat_s(pDestFile, MAX_PATH, GetImageFileExtension());
	}
	if (SUCCEEDED(hResult)) {
		hResult = D3DXSaveSurfaceToFile(pDestFile, DestFormat, offScreenSurface, NULL, NULL);
	}
	if (FAILED(hResult))
	{
		DEBUG_TRACE3(_T("\tFailed save screenshot: Type = %s, Format = %s, MultiSample = %s,")
			_T("MultisampleQuality = %08X, Pool = %s\n"), 
			GetResourceTypeString(desc.Type), GetFormatString(desc.Format), 
			GetMultiSampleString(desc.MultiSampleType), desc.MultiSampleQuality, 
			GetPoolString(desc.Pool));
	}
	return hResult;
}

void MakeGammaCorrection(IDirect3DSurface9* pSourceSurface, D3DGAMMARAMP* pRamp, IDirect3DSurface9** ppDestSurface)
{
	INSIDE;
	HRESULT hResult;
	CComPtr<IDirect3DDevice9> pDevice;
	CComPtr<IDirect3DSurface9> formattedSurface;
	CComPtr<IDirect3DSurface9> destSurface;
	*ppDestSurface = NULL;
	pSourceSurface->GetDevice(&pDevice);
	D3DSURFACE_DESC desc;
	pSourceSurface->GetDesc(&desc);
	NSCALL(pDevice->CreateRenderTarget(desc.Width, desc.Height, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &formattedSurface, 0));
	if (FAILED(hResult)) // out of memory
		return;
	//--- convert surface to D3DFMT_X8R8G8B8 format ---
	NSCALL(pDevice->StretchRect(pSourceSurface, 0, formattedSurface, 0, D3DTEXF_NONE));
	if (FAILED(hResult))
		return;
	NSCALL(pDevice->CreateOffscreenPlainSurface(desc.Width, desc.Height, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &destSurface, 0));
	if (FAILED(hResult)) // out of memory
		return;
	NSCALL(pDevice->GetRenderTargetData(formattedSurface, destSurface));
	if (FAILED(hResult))
		return;
	D3DLOCKED_RECT pRect;
	NSCALL(destSurface->LockRect(&pRect, NULL, D3DLOCK_READONLY));
	if (FAILED(hResult))
		return;
	for(UINT j = 0; j< desc.Height; j++)
	{
		BYTE* p = (BYTE*)pRect.pBits + pRect.Pitch * j;
		for(UINT i = 0; i< desc.Width; i++, p+= 4)
		{
			p[0] = pRamp->red  [p[0]] >> 8;
			p[1] = pRamp->green[p[1]] >> 8;
			p[2] = pRamp->blue [p[2]] >> 8;
		}

	}
	destSurface->UnlockRect();
	*ppDestSurface = destSurface.Detach();
}

void CBaseSwapChain::WriteJPSScreenshot(bool applyGammaCorrection)
{
	INSIDE;
	HRESULT hResult;
	DEBUG_MESSAGE(_T("Making Screenshot..."));
	TCHAR str[MAX_PATH], path[MAX_PATH], exeName[MAX_PATH];

	CComPtr<IDirect3DSurface9> JPSSurface;
	int backBufferSizeX = IsStereoActive() ? m_BackBufferSize.cx * 2 : m_BackBufferSize.cx;
	NSCALL(GetD3D9Device().CreateRenderTarget(backBufferSizeX, m_BackBufferSize.cy, m_PresentationParameters[0].BackBufferFormat, 
		D3DMULTISAMPLE_NONE, 0, FALSE, &JPSSurface, 0));
	if (FAILED(hResult)) // out of memory
		return;
	if(IsStereoActive())
	{
		RECT RightRect = { 0, 0, m_BackBufferSize.cx, m_BackBufferSize.cy };
		NSCALL(GetD3D9Device().StretchRect(m_pRightBackBuffer, &m_RightViewRect, 
			JPSSurface, &RightRect, D3DTEXF_LINEAR));
		RECT LeftRect = { m_BackBufferSize.cx, 0, m_BackBufferSize.cx * 2, m_BackBufferSize.cy };
		NSCALL(GetD3D9Device().StretchRect(m_pLeftBackBuffer, &m_LeftViewRect, 
			JPSSurface, &LeftRect, D3DTEXF_LINEAR));
	}
	else
	{
		RECT monoRect = { 0, 0, m_BackBufferSize.cx, m_BackBufferSize.cy };
		NSCALL(GetD3D9Device().StretchRect(m_pLeftBackBuffer, &m_LeftViewRect, 
			JPSSurface, &monoRect, D3DTEXF_LINEAR));
	}

#define FORBIDDEN_CHARS _T("\\/:*?\"<>|")

	if (SUCCEEDED(hResult))
	{
		if (gInfo.MakeScreenshot)
		{
			NSCALL(SaveScreenshotToFile(gInfo.MakeScreenshotPath, JPSSurface, GetImageFileFormat()));
		}
		else if(SUCCEEDED(SHGetFolderPath(NULL, CSIDL_MYPICTURES|CSIDL_FLAG_CREATE, NULL, 0, path)))
		{
			PathAppend(path, _T(PRODUCT_NAME) _T(" Screenshots") );
			_tcscpy_s(exeName, _countof(exeName), gInfo.ProfileName);
			size_t p = _tcscspn(exeName, FORBIDDEN_CHARS);
			while (p != _tcslen(exeName))
			{
				exeName[p]='_';
				p = _tcscspn(exeName, FORBIDDEN_CHARS);
			}
			CreateDirectory(path, 0);
			PathAppend(path, exeName );
			CreateDirectory(path, 0);

			while(1)
			{
				const TCHAR* ext = IsStereoActive() ? GetStereoImageFileExtension() : GetImageFileExtension();
				_stprintf_s(str, L"%s\\%s #%04i%s", path, exeName, GetBaseDevice()->m_nScreenShotCounter, ext);
				//--- skip existing files ---
				if(!PathFileExists(str))
					break;
				GetBaseDevice()->m_nScreenShotCounter++;
			}
			if(applyGammaCorrection && !m_PresentationParameters[0].Windowed) // only in fullscreen mode
			{
				D3DGAMMARAMP pRamp, *pRampForApply;
				if (GetBaseDevice()->m_pOutputMethod->HandleGamma())
				{
					GetGammaRamp(&pRamp);
					pRampForApply = &pRamp;
				}
				else
					pRampForApply = &GetBaseDevice()->m_GlobalGammaRamp;
				CComPtr<IDirect3DSurface9> pCorrectedSurface;
				MakeGammaCorrection(JPSSurface, pRampForApply, &pCorrectedSurface);
				JPSSurface.Release();
				if (pCorrectedSurface) {
					NSCALL(SaveScreenshotToFile(str, pCorrectedSurface, GetImageFileFormat()));
				}
			}
			else
			{
				NSCALL(SaveScreenshotToFile(str, JPSSurface, GetImageFileFormat()));
			}
			if (SUCCEEDED(hResult))
			{
				DEBUG_MESSAGE(_T(" %s OK.\n"), str);
				return;
			}
		}
	}
	DEBUG_MESSAGE(_T("FAILED.\n"));
}

void CBaseSwapChain::SetPresentParams( CONST RECT * pSourceRect, CONST RECT * pDestRect, HWND hDestWindowOverride, CONST RGNDATA * pDirtyRegion, DWORD dwFlags /*= 0 */ )
{
	m_pSourceRect = pSourceRect;
	m_pDestRect = pDestRect;
	m_hDestWindowOverride = hDestWindowOverride;
	m_pDirtyRegion = pDirtyRegion;
	m_dwFlags = dwFlags;

	RECT rect;
	if( pDestRect )
		rect = *pDestRect;
	else
		GetClientRect( GetAppWindow(), &rect );
	m_DstMonitorPoint.x = rect.left;
	m_DstMonitorPoint.y = rect.top;
	ClientToScreen( GetAppWindow(), &m_DstMonitorPoint );

	HMONITOR hMon = MonitorFromPoint(m_DstMonitorPoint, MONITOR_DEFAULTTONEAREST);
	MONITORINFO  info;
	info.cbSize = sizeof ( MONITORINFO );
	GetMonitorInfo(hMon, &info);

	m_DstMonitorPoint.x -= info.rcMonitor.left;
	m_DstMonitorPoint.y -= info.rcMonitor.top;
}

HRESULT CBaseSwapChain::CallPresent()
{
	HRESULT hPresent = S_OK;
	if (m_bUseSwapChains)
	{
		hPresent = m_SwapChain.Present(m_pSourceRect, m_pDestRect, m_hDestWindowOverride, m_pDirtyRegion, 0);
		if(m_pAdditionalSwapChain)
		{
			LARGE_INTEGER start;
			QueryPerformanceCounter(&start);
			//--- Ignore hDestWindowOverride for second SwapChain ---
			//--- we MUST pass parameters "pSourceRect, pDestRect, pDirtyRegion" in Present() ---
			m_pAdditionalSwapChain->Present(m_pSourceRect, m_pDestRect, 0, m_pDirtyRegion, 0);
			LARGE_INTEGER stop;
			QueryPerformanceCounter(&stop);
			m_nScreensLagTime.QuadPart = stop.QuadPart - start.QuadPart;
		}
	}
	else
	{
		bool bFullscreen = !m_PresentationParameters[0].Windowed;
		if (m_SwapChain.getExMode() == EXMODE_NONE)
		{
			if (bFullscreen)
			{
				//--- Multihead mode doesn't support regions in Present() ---
				hPresent = GetD3D9Device().Present(0, 0, 0, 0);
			}
			else
			{
				hPresent = GetD3D9Device().Present(m_pSourceRect, m_pDestRect, 0, m_pDirtyRegion);
			}
		}
		else
		{
			CComQIPtr<IDirect3DDevice9Ex> pD3D9DevEx(GetD3D9Device());
			if (bFullscreen)
			{
				//--- Multihead mode doesn't support regions in Present() ---
				hPresent = pD3D9DevEx->PresentEx(0, 0, 0, 0, m_dwFlags);
			}
			else
			{
				hPresent = pD3D9DevEx->PresentEx(m_pSourceRect, m_pDestRect, 0, m_pDirtyRegion, m_dwFlags);
			}
		}
	}
	return hPresent;
}

HRESULT CBaseSwapChain::Compose()
{
	INSIDE;
	HRESULT hResult = S_OK;
	HRESULT hPresent = S_OK;

	//--- compose ---
	D3DPERF_BeginEvent(	D3DCOLOR_XRGB( 0, 0, 255 ), L"Compose");
#ifndef FINAL_RELEASE
	//--- dump method data ---
	if (DO_RTDUMP)
	{
		GetBaseDevice()->DumpSurface(_T("0.Primary"), m_pPrimaryBackBuffer);
		GetBaseDevice()->DumpSurface(_T("0.Secondary"), m_pSecondaryBackBuffer);
	}
#endif
	if (GetBaseDevice()->m_pOutputMethod->GetCaps() & ocOutputFromTexture)
	{
		CComPtr<IDirect3DSurface9> pLeftSurface;
		NSCALL(m_pLeftMethodTexture->GetSurfaceLevel(0, &pLeftSurface));
		if (m_pLeftBackBuffer != pLeftSurface)
		{
			NSCALL(GetD3D9Device().StretchRect(m_pLeftBackBuffer, &m_LeftViewRect, 
				pLeftSurface, 0, D3DTEXF_NONE));
			if (IsStereoActive())
			{
				CComPtr<IDirect3DSurface9> pRightSurface;
				NSCALL(m_pRightMethodTexture->GetSurfaceLevel(0, &pRightSurface));
				NSCALL(GetD3D9Device().StretchRect(m_pRightBackBuffer, &m_RightViewRect, 
					pRightSurface, 0, D3DTEXF_NONE));
			}
		}
#ifndef FINAL_RELEASE
		if (DO_RTDUMP)
		{
			GetBaseDevice()->DumpTexture(_T("0._LeftTex"), m_pLeftMethodTexture);
			if (IsStereoActive())
				GetBaseDevice()->DumpTexture(_T("0._RightTex"), m_pRightMethodTexture);
			GetBaseDevice()->DumpTexture(_T("0.Gamma"), m_GammaRAMPTexture);
		}
#endif
	}
	else
	{
#ifndef FINAL_RELEASE
		if (DO_RTDUMP)
		{
			GetBaseDevice()->DumpSurface(_T("0._Left"), m_pLeftBackBuffer);
			if (IsStereoActive())
				GetBaseDevice()->DumpSurface(_T("0._Right"), m_pRightBackBuffer);
		}
#endif
	}
	D3DPERF_EndEvent();
	return hPresent;
}

HRESULT CBaseSwapChain::PresentData( )
{
	INSIDE;
	HRESULT hResult = S_OK;
	HRESULT hrPresent = S_OK;

	NSCALL(Compose());
	if (IS_SHUTTER_OUTPUT)
	{
		CPresentHistory* pHistory = NULL;
		switch(gData.ShutterMode)
		{
		case SHUTTER_MODE_IZ3DKMSERVICE:
			if(SUCCEEDED(m_hPreviousPresentResult))
			{
				NSCALL(GetBaseDevice()->m_pOutputMethod->Output(true, this, NULL));
				NSCALL(hrPresent = CallPresent());
				m_hPreviousPresentResult = hrPresent;
			}
			else
				m_hPreviousPresentResult = S_OK;
			if(SUCCEEDED(m_hPreviousPresentResult))
			{
				NSCALL(GetBaseDevice()->m_pOutputMethod->Output(false, this, NULL));
				NSCALL(hrPresent = CallPresent());
				m_hPreviousPresentResult = hrPresent;
			}
			else
				m_hPreviousPresentResult = S_OK;
			break;
		case SHUTTER_MODE_SIMPLE:
			if (GetBaseDevice()->m_pPresenter)
				pHistory = GetBaseDevice()->m_pPresenter->m_pPresentHistory;
			if (pHistory) pHistory->AfterSleep();
			NSCALL(GetBaseDevice()->m_pOutputMethod->Output(true, this, NULL));
			if (pHistory) pHistory->BeforePresent();
			NSCALL(hrPresent = CallPresent());
			GetBaseDevice()->m_pOutputMethod->AfterPresent(true);
			NSCALL(GetBaseDevice()->m_pOutputMethod->Output(false, this, NULL));
			NSCALL(hrPresent = CallPresent());
			if (pHistory) pHistory->AfterPresent();
			GetBaseDevice()->m_pOutputMethod->AfterPresent(false);
			break;
		case SHUTTER_MODE_UMHOOK_D3D9:
		case SHUTTER_MODE_UMHOOK_D3D9EX:
			{
				RECT rect = { 0, 0, m_BackBufferSize.cx, m_BackBufferSize.cy };
				NSCALL(GetBaseDevice()->m_pOutputMethod->Output(true, this, &rect));
				rect.top	+= m_BackBufferSize.cy;
				rect.bottom	+= m_BackBufferSize.cy;
				NSCALL(GetBaseDevice()->m_pOutputMethod->Output(false, this, &rect));
				NSCALL(hrPresent = CallPresent());

				if (GetBaseDevice()->m_pPresenter)
				{
					GetBaseDevice()->m_pPresenter->SetAction(ptRender);

					if (!m_PresentationParameters[0].Windowed && !GetBaseDevice()->SkipPresenterThread())
					{
						ULONG refreshRate = GetBaseDevice()->m_pPresenter->m_RefreshRate;
						if (refreshRate != 0)
						{
							float targetTime = 1000.0f / 60;
							float currentTime = m_nFrameTimeDelta.QuadPart * 1000.0f / GetBaseDevice()->m_nFreq.QuadPart;
							if (currentTime < targetTime)
								Sleep((int)(targetTime - currentTime + 0.5f));
						}
						else
						{
							DEBUG_MESSAGE(_T("refreshRate == 0: WTF\n"));
						}
					}
				}
			}
			break;
		case SHUTTER_MODE_ATIQB:
			GetBaseDevice()->SetATIBltEye(ATI_STEREO_SETDSTEYE, ATI_STEREO_LEFTEYE);
			NSCALL(GetBaseDevice()->m_pOutputMethod->Output(true, this, NULL));
			GetBaseDevice()->SetATIBltEye(ATI_STEREO_SETDSTEYE, ATI_STEREO_RIGHTEYE);
			NSCALL(GetBaseDevice()->m_pOutputMethod->Output(false, this, NULL));
			NSCALL(hrPresent = CallPresent());
			break;
		default:
			SendImageToPresenter();
			hrPresent = m_pPresenterData ? m_pPresenterData->GetLastPresentResult() : S_OK;
			break;
		}
	}
	else {
		NSCALL(GetBaseDevice()->m_pOutputMethod->Output(this));
		NSCALL(hrPresent = CallPresent());
	}
	if (GetBaseDevice()->m_bDrawMonoImageOnSecondMonitor)
	{
		// Literal: the mono-image mirror to the secondary monitor is internal
		// data movement, same reasoning as the shared-texture pre-copy above.
		NSCALL(GetD3D9Device().StretchRect(GetLeftBackBufferRT_Literal(), GetLeftBackBufferRect_Literal(),
			m_pSecondaryBackBuffer, 0, D3DTEXF_NONE));
	}
#ifndef FINAL_RELEASE
	if (DO_RTDUMP)
	{
		GetBaseDevice()->DumpSurface(_T("1.Primary"), m_pPrimaryBackBuffer);
		GetBaseDevice()->DumpSurface(_T("1.Secondary"), m_pSecondaryBackBuffer);
		GetBaseDevice()->DumpSurfaces(_T("Present"), m_pPrimaryBackBuffer, m_pSecondaryBackBuffer);
	}
#endif
	return hrPresent;
}

void CBaseSwapChain::PrepareFPSMessage()
{
	m_nFPSTimeDelta.QuadPart += m_nFrameTimeDelta.QuadPart;
	m_nSessionFrameCount++;

	if (!gInfo.ShowFPS)
		return;

	bool bCleared = false;
	if (GetBaseDevice()->m_bStereoModeChanged ||
		m_nLastFrameTime.QuadPart > m_nLastDropTime.QuadPart + GetBaseDevice()->m_nFreq.QuadPart)
	{
		bCleared = true;
		m_szFPS.str(L"");
#ifndef FINAL_RELEASE
		m_szFPS << VERSION << L" (" << DRIVER_VER << L")\n" << gInfo.ProfileName << L"\n";
#endif
		if (GetBaseDevice()->m_OrigD3DVersion == 9)
			m_szFPS << (m_SwapChain.getExMode() != EXMODE_EX ? L"D3D9 " : (m_SwapChain.getExMode() == EXMODE_EX ? L"D3D9Ex " : L"D3D9ExEmulate "));
		else
			m_szFPS << (GetBaseDevice()->m_OrigD3DVersion == 8 ? L"D3D8 " : L"D3D7 ");
		m_fFPS = 0.0;
		if (GetBaseDevice()->m_bStereoModeChanged)
			m_szFPS << g_LocalData.GetText(LT_CountingFPS);
		else {
			m_fFPS = m_nSessionFrameCount / ( 1.0 * m_nFPSTimeDelta.QuadPart / GetBaseDevice()->m_nFreq.QuadPart);
			m_szFPS << std::fixed << std::setprecision(2) << m_fFPS;
		}
		m_szFPS << L"  "  << (IsStereoActive() ? g_LocalData.GetText(LT_Stereo) : g_LocalData.GetText(LT_Mono));
		if (!GetBaseDevice()->m_bStereoModeChanged)
		{
#ifndef FINAL_RELEASE
			if (gInfo.ShowDebugInfo)
			{
				if (!IsStereoActive())
					m_fLastMonoFPS = m_fFPS;
				else if (m_nFPSDropShowTimeLeft.QuadPart > 0)
				{
					m_szFPS << L" (" <<  g_LocalData.GetText(LT_drop) << L" " << std::setprecision(2) << 
						100 * (1 - m_fFPS / m_fLastMonoFPS) << L"%)";
				}
			}
#endif
			m_nSessionFrameCount = 0;
			m_nFPSTimeDelta.QuadPart = 0;
		} else if (GINFO_DEBUG && m_fLastMonoFPS != 0.0f)
			m_nFPSDropShowTimeLeft.QuadPart = (7 + 1)*GetBaseDevice()->m_nFreq.QuadPart;
		m_nLastDropTime = m_nLastFrameTime;
	}

	if (bCleared && (gInfo.OutputCaps & odShutterMode))
	{
		if (m_pBasePresenterData)
		{
			m_szFPS << L"\n";
			if (USE_MULTI_THREAD_PRESENTER)
				m_szFPS << L"MT ";
			else if (USE_MULTI_DEVICE_PRESENTER)
				m_szFPS << L"MD ";
			else if (USE_UM_PRESENTER)
				m_szFPS << L"UM ";
			if (!GetBaseDevice()->SkipPresenterThread())
			{
				int sleepTime = m_pBasePresenterData->GetLastSleepTime();
				m_szFPS << L"Presenter: FPS " << std::fixed << std::setprecision(2) << m_pBasePresenterData->GetFPS() << 
					L" sleepTime " << sleepTime <<
					L" SPS " << m_pBasePresenterData->GetLagsPerSecond();

				if (GetBaseDevice()->m_pPresenter)
					m_szFPS << L"\nFramesFromLastVSync " << GetBaseDevice()->m_pPresenter->m_SuperSynchronizer.GetFramesFromLastVSync();

				//if (GetBaseDevice()->m_pPresenter)
				//	m_szFPS << L"\nRefreshRate " << GetBaseDevice()->m_pPresenter->m_RefreshRate <<
				//	L" SuperSynch RefreshRate " << GetBaseDevice()->m_pPresenter->m_SuperSynchronizer.GetRefreshRate() <<				
				//	L" FPS RefreshRate " << m_pBasePresenterData->m_AverageFPS.GetRefreshRate();

				if (gInfo.PresenterSleepTime == -2 && !m_PresentationParameters[0].Windowed)
					m_szFPS << L"\nFlush: Time " << GetBaseDevice()->m_PresenterFlushTime;
			}
			else
				m_szFPS << L"Presenter: Simple";

#ifndef FINAL_RELEASE
			if (USE_UM_PRESENTER && m_pBasePresenterData)
			{
				UMDShutterPresenterSCData* presenter = (UMDShutterPresenterSCData*)m_pBasePresenterData;
				if (presenter->GetPresentCall())
				{
					m_szFPS << L"\n    Present(";
					if (presenter->GetPresentData()->Flags.Blt)
						m_szFPS << L"Blt)";
					else if (presenter->GetPresentData()->Flags.ColorFill)
						m_szFPS << L"ColorFill)";
					else if (presenter->GetPresentData()->Flags.Flip)
						m_szFPS << L"Flip->" << GetFlipIntervalString(presenter->GetPresentData()->FlipInterval) << L")";
					else
						m_szFPS << L")";
				}
				else
					m_szFPS << L"\n    Blt()";
			}
#endif
		}
		else if (USE_ATI_PRESENTER) {
			m_szFPS << L"\nATI Presenter";
		}
		else if (USE_IZ3DKMSERVICE_PRESENTER) {
			m_szFPS << L"\niZ3D Shutter Presenter";
		}
	}
}

void CBaseSwapChain::DrawOSD( float fFrameTimeDelta )
{
	if(IsStereoActive())
	{
		if(m_ShiftFinder.IsInitializedSuccessfully())
		{
			D3DPERF_BeginEvent(	D3DCOLOR_XRGB( 0, 255, 0 ), L"Autoshift & Lasersight");
			//--- search new shift ---
			if(GetBaseDevice()->m_Input.GetActivePreset()->AutoFocusEnable || GetBaseDevice()->m_Input.LaserSightEnable)
				m_ShiftFinder.FindShift(this);

			//--- draw laser sight ---
			if(GetBaseDevice()->m_Input.LaserSightEnable)
				GetBaseDevice()->m_LaserSight.Draw(this, fFrameTimeDelta);
			D3DPERF_EndEvent();
		}

		//--- drawing black area ---
		if(gInfo.FrustumAdjustMode)
		{
			D3DPERF_BeginEvent(	D3DCOLOR_XRGB( 0, 0, 255 ), L"Drawing black area");
			DWORD width = (DWORD)(gInfo.BlackAreaWidth * m_BackBufferSize.cx);
			RECT rectLeft, rectRight;
			switch(gInfo.SeparationMode)
			{
			case 0:
				SetRect(&rectLeft, 0, 0, width, m_BackBufferSize.cy);
				OffsetRect(&rectLeft, m_LeftViewRect.left, m_LeftViewRect.top);
				GetD3D9Device().ColorFill(m_pLeftBackBuffer, &rectLeft, D3DCOLOR_XRGB(0, 0, 0));
				rectRight = rectLeft;
				OffsetRect(&rectRight, m_RightViewRect.left, m_RightViewRect.top);
				GetD3D9Device().ColorFill(m_pRightBackBuffer, &rectRight, D3DCOLOR_XRGB(0, 0, 0));

				OffsetRect(&rectLeft, m_LeftViewRect.right - width,  0);
				GetD3D9Device().ColorFill(m_pLeftBackBuffer, &rectLeft, D3DCOLOR_XRGB(0, 0, 0));
				OffsetRect(&rectRight, m_RightViewRect.right - width, 0);
				GetD3D9Device().ColorFill(m_pRightBackBuffer, &rectRight, D3DCOLOR_XRGB(0, 0, 0));
				break;
			case 1:
				SetRect(&rectRight, 0, 0, 2 * width, m_BackBufferSize.cy);
				OffsetRect(&rectRight, m_RightViewRect.left, m_RightViewRect.top);
				GetD3D9Device().ColorFill(m_pRightBackBuffer, &rectRight, D3DCOLOR_XRGB(0, 0, 0));
				break;
			default:
				SetRect(&rectLeft, 0, 0, 2 * width, m_BackBufferSize.cy);
				OffsetRect(&rectLeft, m_LeftViewRect.right - 2 * width, m_LeftViewRect.top);
				GetD3D9Device().ColorFill(m_pLeftBackBuffer, &rectLeft, D3DCOLOR_XRGB(0, 0, 0));
				break;
			}
			D3DPERF_EndEvent();
		}
	}

	// 0.4.2: uilib Wizard + HotKeyOSD render call sites removed.
	//
	// Wizard: WizardSCData::RenderDX9 has no visibility check and paints
	// the m_pDesktop control tree every frame once Initialize() has run,
	// regardless of ShowWizardAtStartup / WizardWasShown. That made the
	// Wizard appear on screen even with ShowWizardAtStartup=0. The whole
	// Wizard flow is vestigial anyway -- it was iZ3D's shutter-glass-era
	// calibration tool and depends on %ALLUSERSPROFILE%\resources.zip +
	// OSDFont.fnt that no longer ship -- so rendering has no useful
	// content. Shift+Numpad* ProcessKeys() still runs in BaseStereoRenderer
	// _Main.cpp so the hotkey chain doesn't fall off.
	//
	// HotKeyOSD: Shift+F1 now drives the modern OSDRenderer9::Slot::
	// HotkeyHelp card (via gInfo.Input.ShowHotKeyOSD read by
	// WantHotkeyHelp()). The uilib HotKeyOSD.Render* path is dead.

}

HRESULT STDMETHODCALLTYPE CBaseSwapChain::GetBackBuffer(THIS_ UINT iBackBuffer,D3DBACKBUFFER_TYPE Type,IDirect3DSurface9** ppBackBuffer)
{
	m_pLeftBackBufferBeforeScaling->AddRef();
	*ppBackBuffer = m_pLeftBackBufferBeforeScaling;
	DEBUG_TRACE2(_T("GetBackBuffer %s\n"), GetObjectName(m_pLeftBackBufferBeforeScaling));
	return S_OK;
}

HRESULT STDMETHODCALLTYPE CBaseSwapChain::GetDisplayMode(D3DDISPLAYMODE* pMode)
{
	DEBUG_TRACE3( __FUNCTIONW__ _T("\n") );
	HRESULT hr = m_SwapChain.GetDisplayMode(pMode);
	ScalingHookPtrT scalingHook = ScalingAgent::Instance()->Hook();
	if ( SUCCEEDED(hr) && pMode && scalingHook ) 
	{
		pMode->Width  = scalingHook->originalSize.cx;
		pMode->Height = scalingHook->originalSize.cy;
		DEBUG_TRACE3( _T("Modified pMode: %s\n"), GetDisplayModeString(pMode) );
	}
	return hr;
}
	
HRESULT STDMETHODCALLTYPE CBaseSwapChain::GetDisplayModeEx(D3DDISPLAYMODEEX* pMode, D3DDISPLAYROTATION* pRotation)
{
	DEBUG_TRACE3( __FUNCTIONW__ _T("\n") );
	HRESULT hr = m_SwapChain.GetDisplayModeEx(pMode, pRotation);
	ScalingHookPtrT scalingHook = ScalingAgent::Instance()->Hook();
	if ( SUCCEEDED(hr) && pMode && scalingHook ) 
	{
		pMode->Width  = scalingHook->originalSize.cx;
		pMode->Height = scalingHook->originalSize.cy;
		DEBUG_TRACE3( _T("Modified pMode: %s\n"), GetDisplayModeExString(pMode) );
	}
	return hr;
}

#pragma optimize( "y", off ) // for Steam Community

HRESULT STDMETHODCALLTYPE CBaseSwapChain::Present(CONST RECT* pSourceRect,CONST RECT* pDestRect,HWND hDestWindowOverride,CONST RGNDATA* pDirtyRegion,DWORD dwFlags)
{
	DEBUG_TRACE3(_T("IDirect3DSwapChain9::Present(This = %p, pSourceRect = %s, pDestRect = %s, hDestWindowOverride = %p, pDirtyRegion = %p, dwFlags = %s)\n"),
		m_SwapChain.GetReal(), GetRectString(pSourceRect), GetRectString(pDestRect), hDestWindowOverride, pDirtyRegion, GetPresentFlagsString(dwFlags));

	HRESULT hResult = S_OK;
	DEBUG_TRACE2(_T("%\n"), _T( __FUNCTION__ ) );

	if (GetBaseDevice()->m_bEngineStarted) {
		hResult = GetBaseDevice()->DoPresent(this, hDestWindowOverride, pSourceRect, pDestRect, pDirtyRegion, dwFlags);
	} else {
		NSCALL(m_SwapChain.Present(pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion, dwFlags));
	}

	if (hResult == D3DERR_INVALIDCALL) // skip Present() because of game bug
		return hResult;

	DEBUG_TRACE2(_T("Present #%d\n"), m_nPresentCounter);

	DEBUG_TRACE3(_T("--------- Frame %d complete ---------\n\n"), m_nPresentCounter);
	m_nPresentCounter++;

	// 0.4.1 ShaderHunter DX9: FrameEnd rebuilds the stable prev/next cycle
	// from drawn CRCs and resets per-frame counters. No-op when hunt mode
	// is off.
	wiz3d::dx9::HunterFrameEnd();
	// 0.4.1 runtime profile reload (DX9 half of task #52). The handler
	// registered from the DX9 wrapper's init sets a between-frames flag;
	// here on the Present boundary we consume it.
	wiz3d::dx9::MaybeReloadProfileBetweenFrames();

	CheckSecondWindowPosition();
	GetBaseDevice()->CheckEngine();
	FLUSHLOGFILE();
	return hResult;
}

#pragma optimize( "y", on ) 

int WINAPI CBaseSwapChain::D3DPERF_BeginEvent( D3DCOLOR col, LPCWSTR wszName )
{
	return GetBaseDevice()->D3DPERF_BeginEvent(col, wszName);
}

int WINAPI CBaseSwapChain::D3DPERF_EndEvent( void )
{
	return GetBaseDevice()->D3DPERF_EndEvent();
}

HRESULT CBaseSwapChain::PostInitialize()
{
	HRESULT hResult = S_OK;

	GetBaseDevice()->m_pOutputMethod->InitializeSCData(this);
	if (m_pPresenterData)
	{
		// create shared resources and start the presenter thread
		_ASSERT( NULL == m_pPresenterData->GetSharedTextureHandle() );
		// create shared resources
		CreateSharedResources();
		// pass shared resource handles to the presenter
		m_pPresenterData->UpdateSharedRenderTextures( m_SharedHandle );
	}

	if (GetBaseDevice()->m_pPresenter)
	{
		GetBaseDevice()->m_pPresenter->SetAction(ptInitializeSCData, this);
		GetBaseDevice()->m_pPresenter->WaitForActionDone();
	}

	if (GetBaseDevice()->m_pOutputMethod->HandleGamma())
	{
		NSCALL(GetD3D9Device().CreateTexture(256, 1, 1, D3DUSAGE_DYNAMIC, 
			D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &m_GammaRAMPTexture, 0));
		hResult = UpdateGammaRampTexture(&m_SavedGammaRamp);
	}

	if (m_BackBufferSizeBeforeScaling.cx > 1 || m_BackBufferSizeBeforeScaling.cy > 1)
	{
		NSCALL(m_ShiftFinder.Initialize(GetD3D9Device(), GetBaseDevice()->m_BehaviorFlags, &m_LeftViewRect, &m_RightViewRect));
		m_LaserSightData.Initialize(GetBaseDevice()->m_LaserSight.GetTextureSize(), &m_LeftViewRect, &m_RightViewRect);

#ifndef DISABLE_WIZARD
		if (m_BackBufferSize.cx > 400 && m_BackBufferSize.cy > 300)
		{
			m_WizardData.Initialize(&GetBaseDevice()->m_Wizard,
				m_BackBufferSize.cx,
				m_BackBufferSize.cy);
			if (gInfo.Input.ShowWizardAtStartup && !gInfo.WizardWasShown)
				m_WizardData.Show();

			GetBaseDevice()->m_HotKeyOSD.SetSize( m_BackBufferSize.cx, m_BackBufferSize.cy );
			GetBaseDevice()->m_HotKeyOSD.Show();
		}
#endif
	}

	QueryPerformanceCounter(&m_nLastFrameTime);
	m_nFPSDropShowTimeLeft.QuadPart = 0;
	m_fLastMonoFPS = 0.0;

	m_bDepthBufferFounded = FALSE;
	m_DrawCountAfterClearDepthBuffer = 0;
	m_fMinDepthValue = 0.0f;
	m_fMaxDepthValue = 1.0f;
	m_MiniHistogram.resize(8, 0);

	m_hPreviousPresentResult = S_OK;

	return hResult;
}

void  CBaseSwapChain::GetGammaRamp(D3DGAMMARAMP *pRamp)
{
	memcpy((void*)pRamp, &m_CurrentRAMP, sizeof(D3DGAMMARAMP));
}

void CBaseSwapChain::SetGammaRamp(CONST D3DGAMMARAMP *pRamp, bool bUpdateCurrentGamma)
{ 
	memcpy(&m_SavedGammaRamp, pRamp, sizeof(D3DGAMMARAMP));
	if (bUpdateCurrentGamma)
		memcpy(&m_CurrentRAMP, pRamp, sizeof(D3DGAMMARAMP));
	
	if(memcmp(&m_IdentityRAMP, pRamp, sizeof(D3DGAMMARAMP)) == 0)
	{
		m_CurrentRAMPisIdentity = TRUE;
		if(GetBaseDevice()->m_IdentityRAMPisSetToDevice)  
			GetBaseDevice()->m_CurrentRAMPisSetToDevice = TRUE;
		else
			GetBaseDevice()->m_CurrentRAMPisSetToDevice = FALSE;
	}
	else
	{
		m_CurrentRAMPisIdentity = FALSE;
		GetBaseDevice()->m_CurrentRAMPisSetToDevice = FALSE;
	}
	UpdateGammaRampTexture(pRamp);
}

struct SRGBCOLOR
{
	BYTE b, g, r, x;
};

HRESULT CBaseSwapChain::UpdateGammaRampTexture( CONST D3DGAMMARAMP *pRamp )
{
	D3DLOCKED_RECT rect;
	HRESULT hr;
	if (m_GammaRAMPTexture)
	{
		hr = m_GammaRAMPTexture->LockRect(0, &rect, NULL, 0);
		if(SUCCEEDED(hr))
		{
			SRGBCOLOR* p = (SRGBCOLOR*)rect.pBits;
			for(int i=0; i< 256; i++)
			{
				p->x = 255;
				p->r = (BYTE)(255.0*(pRamp->red[i]	 / 65535.0));
				p->g = (BYTE)(255.0*(pRamp->green[i] / 65535.0));
				p->b = (BYTE)(255.0*(pRamp->blue[i]  / 65535.0));
				p++;
			}
			m_GammaRAMPTexture->UnlockRect(0);
		}
		else
			return hr;
	}
	return D3D_OK;
}

void CBaseSwapChain::ClearSharedTextures()
{
	for( UINT i=0; i<SHARED_TEXTURE_COUNT; ++i )
	{
		SAFE_RELEASE( m_pSharedTexture[ i ] );
		m_SharedHandle[ i ]	= NULL;
		SAFE_RELEASE( m_pStagingSurface[ i ] );
	}
}

void CBaseSwapChain::ReleaseSC()
{
	m_SwapChain.Release();
	if(GetBaseDevice()->m_pPresenter)
	{
		GetBaseDevice()->m_pPresenter->SetAction(ptReleaseSC, this);
		GetBaseDevice()->m_pPresenter->WaitForActionDone();
	}
}

HRESULT CBaseSwapChain::DoScaling()
{
	INSIDE;
	HRESULT hResult = S_OK;
	if(!m_bScalingEnabled)
		return hResult;

	if(gInfo.DisplayScalingMode != DISPLAY_SCALING_SCALED && gInfo.DisplayScalingMode != DISPLAY_SCALING_SCALEDASPECTEX)
		GetD3D9Device().ColorFill(m_pLeftBackBuffer, NULL, 0);
	NSCALL(GetD3D9Device().StretchRect(m_pLeftBackBufferBeforeScaling, &m_SrcLeft, m_pLeftBackBuffer, &m_DstLeft, D3DTEXF_LINEAR));
	if(IsStereoActive())
	{
		if(m_pRightBackBuffer != m_pLeftBackBuffer && (gInfo.DisplayScalingMode != DISPLAY_SCALING_SCALED && gInfo.DisplayScalingMode != DISPLAY_SCALING_SCALEDASPECTEX))
			GetD3D9Device().ColorFill(m_pRightBackBuffer, NULL, 0);
		NSCALL(GetD3D9Device().StretchRect(m_pRightBackBufferBeforeScaling, &m_SrcRight, 	m_pRightBackBuffer, &m_DstRight, D3DTEXF_LINEAR));
	}
	return hResult;
}

LRESULT CALLBACK S3DWindowProc(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam)
{
	if (Msg != WM_USER)
		return DefWindowProc(hWnd, Msg, wParam, lParam);

	CBaseSwapChain* pSwapChain = (CBaseSwapChain*)lParam;
	RECT rcWindow;
	switch(wParam) 
	{
	case SWM_MOVE_SECOND_WINDOW:
		rcWindow = pSwapChain->m_S3DSecondWindowRect;
		SetWindowPos(hWnd, HWND_NOTOPMOST, rcWindow.left, rcWindow.top, 
			rcWindow.right - rcWindow.left, rcWindow.bottom - rcWindow.top, SWP_NOACTIVATE);
		break;
	case SWM_SHOW_SECOND_WINDOW:
		ShowWindow(hWnd, SW_SHOWNA);
		break;
	case SWM_HIDE_SECOND_WINDOW:
		pSwapChain->m_bS3DSecondWindowVisible = false;	
		ShowWindow(hWnd, SW_HIDE);
		break;
	case SWM_MOVE_APP_WINDOW: // forcing windowed mode from fullscreen
		SetWindowPos(pSwapChain->GetAppWindow(), HWND_NOTOPMOST, 0, 0,  
			pSwapChain->m_BackBufferSize.cx, pSwapChain->m_BackBufferSize.cy, 0);
		break;
	case SWM_RESTORE_APP_WINDOW: // restore after switch from pseudo full-screen mode ---
		SetWindowPos(pSwapChain->GetAppWindow(), HWND_TOP, 
			0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
		break;
	}
	return S_OK;
}
