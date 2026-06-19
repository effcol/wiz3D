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
#include <Shlobj.h>
#include <iomanip>
#include "BaseStereoRenderer.h"
#include "BaseStereoRenderer_Hook.h"
#include "MonoRenderer_Hook.h"
#include "StereoRenderer_Hook.h"
#include "WDirect3DCreate9.h"
#include "..\Font\acgfx_dynrender_dx9.h"
#include "..\Font\acgfx_font.h"
#include "Trace.h"
#include "..\S3DAPI\EnumToString.h"
#include "..\S3DAPI\ReadData.h"
#include "..\S3DAPI\ShutterAPI.h"
#include "..\uilib\Wizard.h"
#include "DumpManager.h"
#include "atiqbstereo.h"
#include "../../CommonUtils/CommonResourceFolders.h"
#include "MonoOutput_dx9.h"
#include "PresenterMultiThread.h"
#include "BackgroundPresenterThread.h"
#include "ProxyDevice9_Hook.h"
#include "ProxyMulti_Hook.h"

#include "Shaders/shaderSimple.h"
#include "Shaders/shaderViewDepth.h"
#include "Shaders/PSFont.h"
#include "Shaders/VSFont.h"
#include "CommandDumper.h"
#include "..\uilib\HotKeyOSD.h"
#include "DX9OsdDrawer.h"

//--- Use this variable only for UMDeviceWrapper iniutialization ---
//--- Set to NULL in another cases, just for safety ---
CBaseStereoRenderer* g_pCurrentStereoRenderer = NULL;
	
// {FDD3AB8F-F5BB-4dc0-B49D-A87E18E460FD}
const GUID PitchCorrectionGUID = { 0xfdd3ab8f, 0xf5bb, 0x4dc0, { 0xb4, 0x9d, 0xa8, 0x7e, 0x18, 0xe4, 0x60, 0xfd } };

//#define TEREHOV_RESEARCH

using namespace IDirect3DDevice9MethodNames;

BOOL WINAPI Hooked_SetDeviceGammaRamp(HDC hDC, LPVOID lpRamp)
{
	DEBUG_TRACE2(_T("SetDeviceGammaRamp()\n"));
	memcpy(&g_DeviceGammaRamp, lpRamp, sizeof(g_DeviceGammaRamp));
	BOOL b = FALSE;
	
	if (!gInfo.DisableGammaApplying)
	{
		RendererListClass::AddressPairIterator _First = g_pRendererList.m_WrapperList.begin();
		RendererListClass::AddressPairIterator _Last  = g_pRendererList.m_WrapperList.end();
		for (; _First != _Last; ++_First)
			b |= ((CBaseStereoRenderer*)_First->second)->SetDeviceGammaRamp(&g_DeviceGammaRamp);
	}
	if (!gInfo.DisableGammaApplying && !b)
		return Original_SetDeviceGammaRamp(hDC, lpRamp);
	else
		return TRUE;
}

BOOL WINAPI Hooked_GetDeviceGammaRamp(HDC hDC, LPVOID lpRamp)
{
	DEBUG_TRACE3(_T("GetDeviceGammaRamp()\n"));
	memcpy(lpRamp, &g_DeviceGammaRamp, sizeof(g_DeviceGammaRamp));
	if (gInfo.DisableGammaApplying)
		return Original_GetDeviceGammaRamp(hDC, lpRamp);
	else
		return TRUE;
}

// Forward decl rather than #include to keep the header dependency tight.
extern "C" void Wiz3D_RegisterActiveRenderer(class CBaseStereoRenderer* pRenderer);
extern "C" void Wiz3D_UnregisterActiveRenderer(class CBaseStereoRenderer* pRenderer);

CBaseStereoRenderer::CBaseStereoRenderer()
:	CMonoRenderer()
,	m_MainThreadId(0)
,	m_OrigD3DVersion(9)
,	m_BehaviorFlags(0)
,	m_bUsingMonoOutput(FALSE)
,	m_bCantFindStereoDevice(FALSE)
,	m_bUpdateAQBSDriver(FALSE)
,	m_bUnsupported3DDevice(FALSE)
,	m_bDrawMonoImageOnSecondMonitor(false)
,	m_pOutputMethod(0)
,	m_pPresenterThread(0)
,	m_pPresenter(0)
,	m_pUMDeviceWrapper(0)
,	m_bTwoWindows(FALSE)
,	m_bEngineStarted(FALSE)
,	m_NumberOfAdaptersInGroup(1)
,	m_dwNumSimultaneousRTs(1)
,	m_dwMaxTextureWidth(512)
,	m_dwMaxTextureHeight(512)
,	m_dwMaxStreams(1)
,	m_nScreenShotCounter(0)
,	m_DepthTexturesFormat16(D3DFMT_UNKNOWN)
,	m_DepthTexturesFormat24(D3DFMT_UNKNOWN)
,	m_bShowGDIMouse(FALSE)
,	m_hGDIMouseCursor(NULL)
,	m_bShowMouse(FALSE)
,	m_MouseXHotSpot(0)
,	m_MouseYHotSpot(0)
,	m_bDebugWarningCantCreateWideSurface(FALSE)
,	m_CurrentRAMPisSetToDevice(TRUE)
,	m_IdentityRAMPisSetToDevice(TRUE)
,	m_VertexShaderRegister(NULL)
,	m_nTextureCounter(0)
,	m_nCubeTextureCounter(0)
,	m_nVolumeTextureCounter(0)
,	m_bNVPerfHUDInitOk( false )

#ifndef FINAL_RELEASE
,	m_nTexturesSize(0)
,	m_nCubeTexturesSize(0)
,	m_nVolumeTexturesSize(0)

,	m_bDebugSkipUpdateProjectionMatrix(FALSE)
,	m_bDebugSkipModifyRHW(FALSE)
,	m_bDebugSkipSetProjection(FALSE)
,	m_bDebugSkipSynchronizeTextures(FALSE)
,	m_bDebugSkipSynchronizeDepthTextures(FALSE)
,	m_bDebugSkipSetStereoRenderTarget(FALSE)
,	m_bDebugRenderInMono(FALSE)
,	m_bDebugSkipSetStereoTextures(FALSE)
,	m_bDebugSkipCheckCRC(FALSE)
,	m_bDebugSkipShaderMatrixAnalyzing(FALSE)
,	m_bDebugSkipVDAnalyzing(FALSE)
,	m_bDebugSkipTextureUsingCheck(FALSE)
,	m_bDebugRenderInOneRT(FALSE)
,	m_bDebugDoNotRenderMono(FALSE)
,	m_bDebugUseSimplePresenter(FALSE)
,	m_bDebugUseDrawPassSplitting(FALSE)
,	m_RendererEvents(reNone)
#endif
{
	// Register as the active renderer so the wiz3D-stereo bridge (consumed by
	// NvApiProxy) can read/write our preset state.
	Wiz3D_RegisterActiveRenderer(this);

	QueryPerformanceFrequency(&m_nFreq);
#ifndef FINAL_RELEASE
	HMODULE hD3D9Lib = GetD3D9HMODULE();
	_D3DPERF_BeginEvent = (D3DPERF_BeginEvent_type) GetProcAddress(hD3D9Lib, "D3DPERF_BeginEvent");
	_D3DPERF_EndEvent = (D3DPERF_EndEvent_type) GetProcAddress(hD3D9Lib, "D3DPERF_EndEvent");
#endif

	//--- initialize CBaseStereoRenderer members to zero ---
	m_nAdapter[0] = 0;
	m_nAdapter[1] = 1;
	m_bUseR32FDepthTextures = GET_DEBUG_VALUE(gInfo.UseR32FDepthTextures);

	for(WORD i = 0; i< 256; i++)
	{
		m_GlobalGammaRamp.red[i]   = i << 8 | i;
		m_GlobalGammaRamp.green[i] = i << 8 | i;
		m_GlobalGammaRamp.blue[i]  = i << 8 | i;
	}
	
	m_VertexShaderRegister = (FLOAT*)_aligned_malloc(8192 * sizeof(FLOAT) * 4, 16);

#ifndef FINAL_RELEASE
	m_nShaderAnalysingTime.QuadPart = 0;
	m_nCRCCalculatingTime.QuadPart = 0;
	m_DumpDirectory[0] = '\0';
	m_MethodDirectory[0] = '\0';
#endif
}

typedef OutputMethod* (CALLBACK* LPFNCREATEOUTPUT)(DWORD, DWORD);

void CBaseStereoRenderer::CreateOutput()
{
	LPFNCREATEOUTPUT lpfnCreateOutput;	// Function pointer

	if (gInfo.hOutputDLL != NULL)
	{
		lpfnCreateOutput = (LPFNCREATEOUTPUT)GetProcAddress(gInfo.hOutputDLL,
			"CreateOutputDX9");
		if (!lpfnCreateOutput)
		{
			FreeLibrary(gInfo.hOutputDLL);
			gInfo.hOutputDLL = NULL;
		}
		else
		{
			g_RenderInfo.SetOutputDll(gInfo.hOutputDLL);
			m_pOutputMethod = lpfnCreateOutput(gInfo.OutputMode, gInfo.OutputSpanMode);
		}
	}
	if (m_pOutputMethod == NULL)
	{
		DEBUG_MESSAGE(_T("Failed to load output module. Mono output selected.\n"));
		m_bUsingMonoOutput = TRUE;
		m_pOutputMethod = DNew MonoOutput(gInfo.SeparationMode != 2 ? 0 : 1, gInfo.OutputSpanMode);
		gInfo.OutputCaps = 0;
		gData.ShutterMode = SHUTTER_MODE_DISABLED;
	}
}

HRESULT CBaseStereoRenderer::DoInitialize()
{
	HRESULT hResult = S_OK;

	//--- save input parameters ---
	m_nAdapter[0] = m_OriginalAdapter;
	m_BehaviorFlags = m_OriginalBehaviorFlags;

	m_MainThreadId = GetCurrentThreadId();

	//--- find native resolution. only one time ---
	FindNativeFullScreenResolution();

	//--- read camera setting from registry and initialize keyboard read thread ---
	m_ScreenShotButton.selectButton(gInfo.HotKeyList[SCREENSHOT].code);
	m_Input = gInfo.Input;
	m_Input.StereoActive |= gInfo.EnableStereo;
	gKbdHook.initialize(&m_Input);

	CreateOutput();
	if (!m_pOutputMethod->FindAdapter(m_pDirect3D, 0, m_DeviceType, m_nAdapter))
	{
		DEBUG_MESSAGE(_T("Not found output adapter. Mono output selected.\n"));
		delete m_pOutputMethod;
		m_bCantFindStereoDevice = TRUE;
		gInfo.OutputCaps = 0;
		gData.ShutterMode = SHUTTER_MODE_DISABLED;
		m_pOutputMethod = DNew MonoOutput(gInfo.SeparationMode != 2 ? 0 : 1, gInfo.OutputSpanMode);
	}
	// Pass the config as a serialized XML string, NOT the raw g_outputConfig
	// TiXmlNode*: S3DAPI and this DLL link incompatible TinyXML/ticpp layouts,
	// so dereferencing an S3DAPI-allocated node here crashes. GetOutputConfigXml()
	// serializes it inside S3DAPI; the output method re-parses with its own parser.
	if (const char* outputConfigXml = GetOutputConfigXml())
		m_pOutputMethod->ReadConfigData(outputConfigXml);
	m_pOutputMethod->StereoModeChanged(m_Input.StereoActive != 0);

	m_bTwoWindows = m_pOutputMethod->GetOutputChainsNumber() > 1;
	if (!m_bTwoWindows && gInfo.DrawMonoImageOnSecondMonitor)
	{
		DEBUG_MESSAGE(_T("Activated DrawMonoImageOnSecondMonitor\n"));
		m_bTwoWindows = true;
		m_bDrawMonoImageOnSecondMonitor = true;

		D3DCAPS9 D3DCaps;
		//--- search second adapter ordinal for current master adapter ---
		for(UINT i=0; i< m_pDirect3D->GetAdapterCount(); i++)
		{
			m_pDirect3D->GetDeviceCaps(i, m_DeviceType, &D3DCaps);
			if(D3DCaps.MasterAdapterOrdinal == m_nAdapter[0] && D3DCaps.AdapterOrdinalInGroup == 1)
			{
				m_nAdapter[1] = i;
				return true;
			}
		}
	}
	m_DeviceMode = (DeviceModes)gInfo.DeviceMode;
	if (m_BehaviorFlags & D3DCREATE_ADAPTERGROUP_DEVICE)
	{
		DEBUG_MESSAGE(_T("%s: Appication requests D3DCREATE_ADAPTERGROUP_DEVICE.\n"), _T( __FUNCTION__ ) );
	}
	m_BehaviorFlags &= ~D3DCREATE_PUREDEVICE;
	if (m_pOutputMethod->GetCaps() & ocNeedMultiThreadedDevice)
		m_BehaviorFlags |= D3DCREATE_MULTITHREADED;

	D3DCAPS9 D3DCaps;
	NSCALL(m_pDirect3D->GetDeviceCaps(m_nAdapter[0], m_DeviceType, &D3DCaps));
	m_NumberOfAdaptersInGroup = D3DCaps.NumberOfAdaptersInGroup;
	DWORD maxTextures = D3DCaps.MaxSimultaneousTextures;
	DWORD PSVersion = D3DCaps.PixelShaderVersion & 0xFFFF;
	if (PSVersion >= 0x0200)
		maxTextures = std::max<DWORD>(16, maxTextures);
	else if (PSVersion >= 0x0104)
		maxTextures = std::max<DWORD>(6, maxTextures);
	else 
		maxTextures = std::max<DWORD>(4, maxTextures);
	m_PSPipelineData.SetMaxTextures(maxTextures);
	DWORD VSVersion = D3DCaps.VertexShaderVersion & 0xFFFF;
	if (VSVersion >= 0x0300)
		maxTextures = 4 + 1;
	else 
		maxTextures = 1;
	m_VSPipelineData.SetMaxTextures(maxTextures);

	m_dwNumSimultaneousRTs = std::min<DWORD>(4, D3DCaps.NumSimultaneousRTs); // Can't be higher than 4
	m_RenderTarget.resize(4);
	m_dwMaxTextureWidth = D3DCaps.MaxTextureWidth;
	m_dwMaxTextureHeight = D3DCaps.MaxTextureHeight;
	m_dwMaxStreams = D3DCaps.MaxStreams;
	m_MeshCRCMultiplier.assign(m_dwMaxStreams, NULL);
	m_pVB.assign(m_dwMaxStreams, NULL);

	if (gInfo.ExtractDepthBuffer)
		gInfo.EnableAA = FALSE;

	if (gInfo.OutputCaps & odShutterMode)
	{
		// re-create shared resources
		SAFE_DELETE( m_pPresenterThread );		// stop & re-create presenter thread

		if (USE_MULTI_THREAD_PRESENTER)
		{
			m_pPresenterThread	= PresenterMultiThread::CreatePresenterMultiThread(
				this,
				m_pDirect3D,
				m_nAdapter[0],
				m_hFocusWindow	//NULL				// hWnd
				);
		}
		else if (USE_MULTI_DEVICE_PRESENTER)
		{
			CComQIPtr<IDirect3D9Ex>	pD3DEx	= m_pDirect3D;
			m_pPresenterThread	= BackgroundPresenterThread::CreateBackgroundPresenterThread(
				this,
				pD3DEx,
				m_nAdapter[0],
				m_hFocusWindow	//NULL				// hWnd
				);
		}
		m_pPresenter = m_pPresenterThread;
	}

	DEBUG_MESSAGE(_T("%s\n"), GetDeviceModeString(m_DeviceMode));
	if (USE_MULTI_THREAD_PRESENTER || USE_MULTI_DEVICE_PRESENTER || USE_ATI_PRESENTER)
	{
		gInfo.DisableFullscreenModeEmulation = TRUE;
		m_DeviceMode = DEVICE_MODE_FORCEWINDOWED;
	}	
	else if (m_bTwoWindows && m_DeviceMode == DEVICE_MODE_AUTO)
	{
		switch(gInfo.MultiWindowsMode)
		{
		case MULTI_WINDOWS_MODE_APPLICATION:
			m_bTwoWindows = FALSE;
			break;
		case MULTI_WINDOWS_MODE_MULTIHEAD:
			m_DeviceMode = DEVICE_MODE_FORCEFULLSCREEN;
			break;
		case MULTI_WINDOWS_MODE_SWAPCHAINS:
		case MULTI_WINDOWS_MODE_SIDEBYSIDEWINDOWED:
			m_DeviceMode = DEVICE_MODE_FORCEWINDOWED;
			break;
		case MULTI_WINDOWS_MODE_AUTO:
		default:
			{
				D3DPRESENT_PARAMETERS& pp = GetMonoSC()->m_OriginalPresentationParameters;
				if ((pp.BackBufferWidth > 1 || pp.BackBufferHeight > 1) ||
					(pp.Windowed && pp.BackBufferWidth == 0 && pp.BackBufferHeight == 0))
				{
					if (pp.Windowed)
						m_DeviceMode = DEVICE_MODE_FORCEWINDOWED;
					else
						m_DeviceMode = DEVICE_MODE_FORCEFULLSCREEN;
				}
			}
		}
	}
	DEBUG_MESSAGE(_T("%s\n"), GetDeviceModeString(GetBaseDevice()->m_DeviceMode));

	GetBaseSC()->InitWindows();
	GetBaseSC()->ModifyPresentParameters();

	CComPtr<IDirect3DDevice9Ex> pDevEx;
	CComQIPtr<IDirect3DDevice9> pDirect3DDevice;
	if (USE_MULTI_THREAD_PRESENTER)
	{
		m_pPresenterThread->ResumeThread();
		NSCALL(m_pPresenterThread->WaitD3DCreation());
		m_pPresenterThread->GetDirect3DDevice9( &pDirect3DDevice );
	}
	else
	{
		if (m_Direct3DDevice.getExMode() == EXMODE_NONE)
		{
			D3DPRESENT_PARAMETERS* pPar = &GetBaseSC()->m_PresentationParameters[0];
			UINT oldHZ = 0;
			if( ! pPar->Windowed )
			{
				if( !USE_ATI_PRESENTER && USE_UM_PRESENTER )
				{
					oldHZ = pPar->FullScreen_RefreshRateInHz;
					pPar->FullScreen_RefreshRateInHz = 59;
				}
			}
			if (USE_ATI_PRESENTER)
			{
				m_BehaviorFlags &= ~(D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_MIXED_VERTEXPROCESSING);
				m_BehaviorFlags |= D3DCREATE_HARDWARE_VERTEXPROCESSING;
			}
			{
				SetUMEvent UMEvent(this, reCreatingDevice);
				NSCALL_TRACE1(m_pDirect3D->CreateDevice(m_nAdapter[0], m_DeviceType, m_hFocusWindow,
					m_BehaviorFlags, pPar, &pDirect3DDevice), 
					DEBUG_MESSAGE(_T("Trying to CreateDevice(m_nAdapter = %u, m_DeviceType = %s, m_hFocusWindow = %p, ")
					_T("m_BehaviorFlags = %s, m_PresentationParameters = %s, m_pDirect3DDevice = %p)"),
					m_nAdapter[0], GetDeviceTypeString(m_DeviceType), m_hFocusWindow,
					GetBehaviorFlagsString(m_BehaviorFlags), 
					GetPresentationParametersString(pPar), pDirect3DDevice));
			}
			if( oldHZ )
			{
				pPar->FullScreen_RefreshRateInHz = oldHZ;
				SetUMEvent UMEvent(this, reResetingDevice);
				pDirect3DDevice->Reset(pPar);
			}
		}
		else
		{
			CComQIPtr<IDirect3D9Ex> pD3DEx = m_pDirect3D;
			if (pD3DEx)
			{
				SetUMEvent UMEvent(this, reCreatingDevice);
				NSCALL_TRACE1(pD3DEx->CreateDeviceEx(m_nAdapter[0], m_DeviceType, m_hFocusWindow,
					m_BehaviorFlags, &GetBaseSC()->m_PresentationParameters[0], GetBaseSC()->m_pFullscreenDisplayMode, &pDevEx), 
					DEBUG_MESSAGE(_T("Trying to CreateDeviceEx(m_nAdapter = %u, m_DeviceType = %s, m_hFocusWindow = %p, ")
					_T("m_BehaviorFlags = %s, m_PresentationParameters = %s, m_pFullscreenDisplayMode = %s, m_pDirect3DDevice = %p)"),
					m_nAdapter[0], GetDeviceTypeString(m_DeviceType), m_hFocusWindow,
					GetBehaviorFlagsString(m_BehaviorFlags), 
					GetPresentationParametersString(&GetBaseSC()->m_PresentationParameters[0]), 
					GetDisplayModeExString(GetBaseSC()->m_pFullscreenDisplayMode), pDevEx));
				pDirect3DDevice = pDevEx;
			}
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
	HookDevice();

	if(gInfo.OutputCaps & odShutterMode)
	{ 
		if (USE_MULTI_DEVICE_PRESENTER)
		{
			m_pPresenterThread->ResumeThread();
			m_pPresenterThread->WaitD3DCreation();		// N.B. The last created device could use fullscreen mode!
		} 
		else if (USE_ATI_PRESENTER || USE_UM_PRESENTER)
		{
			//--- we create D3D object in this function, for ROUTER_TYPE = 1  it MUST be after HookDevice() call ---
			InitATIShutterMode();
		}
	}

	while(hResult == D3DERR_DEVICELOST)
	{
		DEBUG_MESSAGE(_T("%s: D3DERR_DEVICELOST. Trying to Reset.\n"), _T( __FUNCTION__ ) );
		hResult = pDirect3DDevice->TestCooperativeLevel();
	}
	if (hResult == D3DERR_DEVICENOTRESET)
	{
		DEBUG_MESSAGE(_T("%s: D3DERR_DEVICENOTRESET\n"), _T( __FUNCTION__ ) );
		hResult = pDirect3DDevice->Reset(&GetBaseSC()->m_PresentationParameters[0]);
	}

	if(SUCCEEDED(hResult))
	{
		DEBUG_MESSAGE(_T("%s: CreateDevice OK.\n"), _T( __FUNCTION__ ) );
	}
	else
	{
		DEBUG_MESSAGE(_T("%s: CreateDevice FAILED absolutely. Error code = 0x%X\n"), _T( __FUNCTION__ ) , hResult);
		return hResult;
	}

#ifndef FINAL_RELEASE
	if (m_BehaviorFlags & D3DCREATE_MULTITHREADED)
		DumpManager::Get().SetMTDevice();
#endif

	DEBUG_MESSAGE(_T("D3DCaps.MaxVertexShaderConst = %d\n"), D3DCaps.MaxVertexShaderConst );
	DEBUG_MESSAGE(_T("D3DCaps.NumTemps = %d\n"), D3DCaps.VS20Caps.NumTemps );
	{
		m_VertexShaderConverter.Initialize(&m_Direct3DDevice);
		m_PixelShaderConverter.Initialize(&m_Direct3DDevice);
		if (!gInfo.MonoDepthStencilTextures)
			DetectDepthStencilTextureFormat();
	}

	NSCALL(StartEngine());

	//--- fix bug in "Deep Exploration" application ---
	//--- it's  NOT an our bug! ---
	/*
	HWND hParent = GetAppWindow();
	InvalidateRect(hParent, NULL, TRUE);
	UpdateWindow(hParent);
	*/

	return hResult;
}

HRESULT CBaseStereoRenderer::SetATIBltEye( DWORD command, DWORD param )
{
	if (!m_pATICommSurface)
		return E_FAIL;

	HRESULT hResult = S_OK;
	//Lock the fourCC surface and request stereo using the returned communication surface
	D3DLOCKED_RECT          lockedRect;
	NSCALL(m_pATICommSurface->LockRect(&lockedRect, 0, 0));
	if (FAILED(hResult)  && USE_ATI_PRESENTER)
	{
		m_pATICommSurface = 0;
		{
			SetUMEvent UMEvent(this, reCreatingAQBS);
			NSCALL(m_Direct3DDevice.CreateOffscreenPlainSurface( 10, 10, (D3DFORMAT)FOURCC_AQBS, D3DPOOL_DEFAULT, &m_pATICommSurface, NULL));
		}
		if (!m_pATICommSurface)	
			return E_FAIL;
		NSCALL(m_pATICommSurface->LockRect(&lockedRect, 0, 0));
	}
	ATIDX9STEREOCOMMPACKET* pCommPacket;
	pCommPacket = (ATIDX9STEREOCOMMPACKET *)(lockedRect.pBits);

	// Indicate to driver that this is a valid packet by setting the signature to 'STER'
	pCommPacket->dwSignature = 'STER';

	// Tell the driver to enable stereo (ie allocate a quad buffer) when in full screen mode
	pCommPacket->stereoCommand = (ATIDX9STEREOCOMMAND)command;
	pCommPacket->dwInBufferSize = sizeof(param);
	pCommPacket->pInBuffer = (BYTE*)&param;

	// Tell the driver where to put the result code of the command
	HRESULT hDrvResult = E_FAIL;
	pCommPacket->pResult = &hDrvResult;
	// Send the command to the driver
	NSCALL(m_pATICommSurface->UnlockRect());
	return hDrvResult;
}

void CBaseStereoRenderer::InitATIShutterMode()
{
	HRESULT hResult = S_OK;
	m_DeviceMode = DEVICE_MODE_AUTO;
	{
		RouterTypeHookCallGuard<ProxyDevice9> RT(&m_Direct3DDevice);
		if (USE_ATI_PRESENTER)
		{
			//Create AQBS surface to be used to communicate with the driver
			SetUMEvent UMEvent(this, reCreatingAQBS);
			NSCALL(m_Direct3DDevice.CreateOffscreenPlainSurface( 10, 10, (D3DFORMAT)FOURCC_AQBS, D3DPOOL_DEFAULT, &m_pATICommSurface, NULL));
			if(FAILED(hResult))
				m_bUpdateAQBSDriver = true;
		}
		else // emulate
		{
			SetUMEvent UMEvent(this, reCreatingAQBS);
			NSCALL( m_Direct3DDevice.CreateOffscreenPlainSurface( 10, 10, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &m_pATICommSurface, NULL ) );
		}
	}
	GetBaseSC()->ModifyPresentParameters();
	m_ATILineOffset = 0;

	if(SUCCEEDED(hResult))
	{
		HRESULT hResult = S_OK;
		//Lock the fourCC surface and request stereo using the returned communication surface
		D3DLOCKED_RECT          lockedRect;
		NSCALL(m_pATICommSurface->LockRect(&lockedRect, 0, 0));
		ATIDX9STEREOCOMMPACKET* pCommPacket;
		pCommPacket = (ATIDX9STEREOCOMMPACKET *)(lockedRect.pBits);

		// Indicate to driver that this is a valid packet by setting the signature to 'STER'
		pCommPacket->dwSignature = 'STER';

		// Tell the driver to enable stereo (ie allocate a quad buffer) when in full screen mode
		pCommPacket->stereoCommand = ATI_STEREO_ENABLESTEREO;

		// Tell the driver where to put the result code of the command
		HRESULT hDrvResult = E_FAIL;
		pCommPacket->pResult = &hDrvResult;
		// Send the command to the driver
		NSCALL(m_pATICommSurface->UnlockRect());
		//--- Do not use macro here! It check gData.ShutterMode, not gInfo.ShutterMode ---
		if( FAILED(hDrvResult) && _tcscmp(gInfo.Vendor, _T("ATI")) == 0 && 
			(gInfo.ShutterMode == SHUTTER_MODE_ATIQB || gInfo.ShutterMode == SHUTTER_MODE_AUTO) )
		{
			m_bUnsupported3DDevice = true;
			DEBUG_MESSAGE(_T("Stereo driver command EnableStereo Failed\n"));
		}
	}

	CBaseSwapChain* pSwapChain = GetBaseSC();
	D3DPRESENT_PARAMETERS* pPresParam = &pSwapChain->m_PresentationParameters[0];
	D3DDISPLAYMODEEX* pDMode = pSwapChain->m_pFullscreenDisplayMode;

	if (USE_ATI_PRESENTER)
	{
		if (m_pATICommSurface)
		{
			ATIDX9STEREOVERSION ver;
			ZeroMemory((void*)&ver, sizeof(ATIDX9STEREOVERSION));
			ver.dwSize = sizeof(ATIDX9STEREOVERSION);
			if (SUCCEEDED(SendStereoCommand(ATI_STEREO_GETVERSIONDATA, (BYTE*)&ver, sizeof(ATIDX9STEREOVERSION), 0, 0)))
			{
				DEBUG_MESSAGE(_T("ATI DX9 Stereo Version from header = %d.%d\n"), ATI_STEREO_VERSION_MAJOR, ATI_STEREO_VERSION_MINOR);
				DEBUG_MESSAGE(_T("ATI DX9 Stereo Version = %d.%d\n"), ver.dwVersionMajor, ver.dwVersionMinor);
				if(ATI_STEREO_VERSION_MAJOR > ver.dwVersionMajor || 
					ATI_STEREO_VERSION_MAJOR == ver.dwVersionMajor && ATI_STEREO_VERSION_MINOR > ver.dwVersionMinor)
					m_bUpdateAQBSDriver = true;
			}
		}
		m_pATICommSurface = 0;
		{
			SetUMEvent UMEvent(this, reResetingDevice);
			ReleaseStereoParamsTexture(); // D3DPOOL_DEFAULT must be released before Reset
			NSCALL_TRACE1(m_Direct3DDevice.Reset(pPresParam), DEBUG_MESSAGE(_T("Reset(pPresParam = %s)"),
						  GetPresentationParametersString(pPresParam)));
		}
		{
			SetUMEvent UMEvent(this, reCreatingAQBS);
			NSCALL(m_Direct3DDevice.CreateOffscreenPlainSurface( 10, 10, (D3DFORMAT)FOURCC_AQBS, D3DPOOL_DEFAULT, &m_pATICommSurface, NULL));
		}
	}

	SetupUMPresenter();
}

bool CBaseStereoRenderer::GetATILineOffset()
{
	_ASSERT(m_pATICommSurface);
	m_ATILineOffset = 0;
	D3DLOCKED_RECT          lockedRect;
	ATIDX9STEREOCOMMPACKET* pCommPacket;
	HRESULT					hResult = S_OK;
	//Lock the fourcc surface and set the comm. Packet to retrieve the line offset
	NSCALL( m_pATICommSurface->LockRect(&lockedRect, 0, 0) );
	pCommPacket = (ATIDX9STEREOCOMMPACKET *)(lockedRect.pBits);
	pCommPacket->dwSignature = 'STER';

	// Tell the driver to return the line offset
	pCommPacket->stereoCommand = ATI_STEREO_GETLINEOFFSET;

	HRESULT hDrvResult = E_FAIL;
	pCommPacket->pResult = &hDrvResult;

	// Tell the driver to write line offset to class variable m_ATILineOffset
	pCommPacket->dwOutBufferSize = sizeof(m_ATILineOffset);
	pCommPacket->pOutBuffer = (BYTE *)&m_ATILineOffset;

	// On UnlockRect, the driver will write the line offset to m_ATILineOffset
	NSCALL( m_pATICommSurface->UnlockRect() );
	return m_ATILineOffset != 0;
}

bool CBaseStereoRenderer::IsCurrentThreadMain()
{
	bool bResult = (m_MainThreadId == GetCurrentThreadId());
	return bResult;
}

void CBaseStereoRenderer::FindBestFullScreenResolution(UINT &Width, UINT &Height, D3DFORMAT Format)
{
	D3DDISPLAYMODE mode;
	DWORD mc = m_pDirect3D->GetAdapterModeCount(m_nAdapter[0], Format);
	for (DWORD i = 0; i < mc; i++)
	{
		m_pDirect3D->EnumAdapterModes(m_nAdapter[0], Format, i, &mode);
		if (Height == mode.Height && Width == mode.Width)
			return;
	}
	for (DWORD i = 0; i < mc; i++)
	{
		m_pDirect3D->EnumAdapterModes(m_nAdapter[0], Format, i, &mode);
		if (Height <= mode.Height && Width <= mode.Width)
		{
			Width = mode.Width;
			Height = mode.Height;
			return;
		}
	}
	m_pDirect3D->GetAdapterDisplayMode(m_nAdapter[0], &mode);
	Width = mode.Width;
	Height = mode.Height;
}

D3DFORMAT CBaseStereoRenderer::GetDisplayFormatFromBackBufferFormat(D3DFORMAT backBufferFormat)
{
	switch(backBufferFormat)
	{
		case D3DFMT_A2R10G10B10:
			return D3DFMT_A2R10G10B10;
		case D3DFMT_A1R5G5B5:
		case D3DFMT_X1R5G5B5:
			return D3DFMT_X1R5G5B5;
		case D3DFMT_R5G6B5:
			return D3DFMT_R5G6B5;
	}
	return D3DFMT_X8R8G8B8;
}

void CBaseStereoRenderer::FindNativeFullScreenResolution()
{
	if(gInfo.DisplayNativeWidth == 0 || gInfo.DisplayNativeHeight == 0)
	{
		DEVMODE mode;
		EnumDisplaySettings(NULL, ENUM_REGISTRY_SETTINGS, &mode);
		gInfo.DisplayNativeWidth = mode.dmPelsWidth;
		gInfo.DisplayNativeHeight = mode.dmPelsHeight;
		gInfo.DisplayNativeFrequency.Numerator = mode.dmDisplayFrequency;
		gInfo.DisplayNativeFrequency.Denominator = 1;
	}
}

void CBaseStereoRenderer::InitVariables()
{
	// init variables
	m_VSPipelineData.Init(0);
	m_PSPipelineData.Init(0xFFFFFFFF);

	m_bShowMouse = false;
	m_MouseXHotSpot = 0;
	m_MouseYHotSpot = 0;

	m_pModifiedVertexStreamZeroData = NULL;
	m_ModifiedVertexStreamZeroSize = 0;
	m_RHWEmulData = NULL;

	m_DumpType = dtNone;

	// OSD
	m_pDynRender = 0;
	m_pFont = NULL;
	m_nOSDShowTimeLeft.QuadPart = 0;
	m_nWriteSettingsAfter.QuadPart = 0;
	m_FlushTime.QuadPart = 0;
	m_PresenterFlushTime = 1;
	// Only in Draw
	m_Pipeline = Fixed; 
	m_bRenderInStereo = FALSE;
	m_CurrentMeshMultiplier = 1.0f;
	m_CurrentConvergenceShift = 0.0f;
	m_bRenderInStereoPS = FALSE;
	m_CurrentPSMultiplier = 1.0f;
	m_CurrentPSConvergenceShift = 0.0f;
	m_CurrentView = VIEWINDEX_MONO;	
	m_pUpdateRTs = m_dwColorWriteEnable;
	m_UpdateDS = true;

	// states
	ZeroMemory(&m_ScissorRect, sizeof(RECT));
	ZeroMemory(&m_ViewPort, sizeof(D3DVIEWPORT9));
	ZeroMemory(&m_PrimaryRTSize, sizeof(SIZE));
	m_bFullSizeViewport = FALSE;
	m_bFullSizeScissorRect = FALSE;
	ZeroMemory(m_VertexShaderRegister, sizeof(m_VertexShaderRegister));
	m_dwColorWriteEnable[0] = 0xF;
	m_dwColorWriteEnable[1] = 0xF;
	m_dwColorWriteEnable[2] = 0xF;
	m_dwColorWriteEnable[3] = 0xF;
	m_dwZWriteEnable = TRUE;
	m_dwStencilEnable = TRUE;
	m_dwScissorTestEnable = FALSE;
	m_dwStreamRHWVertices = 0;
	m_dwOffsetRHWVertices = 0;
	m_nSavedOffsetInBytes = 0;
	m_nSavedStride = 0;
	m_bInverseZ = FALSE; 
	m_bOnlyProjectionMatrix = FALSE;
	m_ZNear = 0.0f;
	m_ZFar = 0.0f;
	m_FixedZNear = 0.0f;
	m_FixedZFar = 0.0f;
	m_ShaderZNear = 0.0f;
	m_ShaderZFar = 0.0f;
	D3DXMatrixIdentity(&m_WorldMatrix);
	D3DXMatrixIdentity(&m_ViewMatrix);
	D3DXMatrixIdentity(&m_ProjectionMatrix);
	D3DXMatrixIdentity(&m_VSProjectionMatrix);
	D3DXMatrixIdentity(&m_TransformAfterViewportVSMono);
	D3DXMatrixIdentity(&m_OldShaderMatrix);

	// cached values
	m_bRenderScene = false;
	m_bInStateBlock = false;
	m_bPerspectiveMatrix = FALSE;
	m_bPerspectiveVSMatrix = FALSE;
	m_bRHWVertexDeclaration = FALSE;
	m_bOldShaderPerspectiveMatrix = FALSE;
	m_bOldShaderInverseZ = FALSE;
	m_bIsWorldMatrixIdenty = TRUE; 
	m_bIsViewMatrixIdenty = TRUE; 
	ZeroMemory(&m_OldViewPort, sizeof(D3DVIEWPORT9));

	//statistic counters
	m_fCurSessionTime = 0.0f;
	m_fTotalMonoTime = 0.0f;
	m_fTotalStereoTime = 0.0f;
	m_nMonoSessionsCounter = 0;
	m_nStereoSessionsCounter = 0;
	m_fMaxMonoTime = 0.0f;
	m_fMaxStereoTime = 0.0f;
#ifndef FINAL_RELEASE
	m_nRenderTargetsMonoSize = 0;
	m_nRenderTargetsStereoSize = 0;
	m_nRenderTargetTexturesMonoSize = 0;
	m_nRenderTargetTexturesStereoSize = 0;
	m_nDepthStencilSurfacesMonoSize = 0;
	m_nDepthStencilSurfacesStereoSize = 0;
	m_nDepthStencilTexturesMonoSize = 0;
	m_nDepthStencilTexturesStereoSize = 0;
	m_nTexturesDefaultSize = 0;
	m_nCubeTexturesDefaultSize = 0;
	m_nRenderTargetCubeTexturesMonoSize = 0;
	m_nRenderTargetCubeTexturesStereoSize = 0;
	m_nDepthStencilCubeTexturesMonoSize = 0;
	m_nDepthStencilCubeTexturesStereoSize = 0;
	m_nVolumeTexturesDefaultSize = 0;
#endif	
	m_nRenderTargetCounter = 0;
	m_nDepthStencilSurfaceCounter = 0;
	m_nTextureDefaultCounter = 0;
	m_nRenderTargetTextureCounter = 0;
	m_nRenderTargetR32FTextureCounter = 0;
	m_nRenderTargetSquareTextureCounter = 0;
	m_nRenderTargetLessThanBBTextureCounter = 0;
	m_nDepthStencilTextureCounter = 0;
	m_nCubeTextureDefaultCounter = 0;
	m_nRenderTargetCubeTextureCounter = 0;
	m_nDepthStencilCubeTextureCounter = 0;
	m_nVolumeTextureDefaultCounter = 0;
	m_nFlushCounter = 0;
	m_nGetDataCounter = 0;

#ifndef FINAL_RELEASE
	m_nTexSynchTime.QuadPart = 0;
	m_nTexSynchRTTime.QuadPart = 0;
	m_nTexSynchCounter = 0;
	m_nTexSynchRTCounter = 0;
	m_nTexSynchCachedCounter = 0;
	m_nWeaponDrawCounter = 0;
#endif	

	m_bOutOfVideoMemory = false;
	m_bDebugWarningMonoDepthStencil = FALSE;
	m_bDebugWarningEmulateFullscreenMode = FALSE;

	m_nEventCounter = 0;
	m_nDrawCounter = 0;
	m_nDrawMonoCounter = 0;
	m_nDrawVSCounter = 0;
	m_nDrawRHWCounter = 0;
	m_nDrawFixedCounter = 0;
	m_nDrawHeavyCounter = 0;

	///////////////////

	m_ViewPort.MinZ = 0.0f;
	m_ViewPort.MaxZ = 1.0f;
}

HRESULT CBaseStereoRenderer::StartEngine() 
{
	INSIDE;
	HRESULT hResult = S_OK;
	DEBUG_TRACE1(_T("StartEngine()\n"));

	RouterTypeHookCallGuard<ProxyDirect9> RTdir(GetProxyDirect3D9());
	RouterTypeHookCallGuard<ProxyDevice9> RTdev(&m_Direct3DDevice);

	InitVariables();

	UpdateProjectionMatrix();

	NSCALL(GetMonoSC()->InitializeEx(0));

	//--- adjust scale for ZPS movement ---
	gKbdHook.setBackBufferWidth(GetBaseSC()->m_BackBufferSizeBeforeScaling.cx);
	//m_KbdHook.setSwapChain(GetBaseSC());

	NSCALL(StartEngineMode());
	if (!m_pPresenterThread)
		m_pOutputMethod->Initialize(m_Direct3DDevice, GET_DEBUG_VALUE(gInfo.TwoPassRendering) != 0);
	if (m_pPresenter)
	{
		m_pPresenter->SetAction(ptInitialize);
		m_pPresenter->WaitForActionDone();
	}

	//--- initialize laser sight ---
	NSCALL(m_LaserSight.Initialize(m_Direct3DDevice));

	// --- initialize wizard
#ifndef DISABLE_WIZARD
	NSCALL(m_Wizard.Initialize(m_Direct3DDevice));
#endif

#ifndef DISABLE_HOTKEY_OSD
	// --- initialize Hot Key OSD
	NSCALL( m_HotKeyOSD.Initialize( m_Direct3DDevice ) );
#endif

	GetBaseSC()->PostInitialize();

	// init font
	QueryPerformanceCounter(&m_nGlobalLastFrameTime);
	m_bStereoModeChanged = false;
	m_nWriteSettingsAfter.QuadPart = 0;
	NSCALL(m_Direct3DDevice.CreateStateBlock(D3DSBT_ALL, &m_pSavedState));

	if (gInfo.ExtractDepthBuffer || GINFO_DEBUG)
	{
		NSCALL(m_Direct3DDevice.CreatePixelShader((DWORD*)g_ps20_shaderViewDepth, &m_pViewShader));
		NSCALL(m_Direct3DDevice.CreatePixelShader((DWORD*)g_ps20_shaderSimple, &m_pCopyShader));

		m_Direct3DDevice.SetTexture(0, 0);
		m_Direct3DDevice.SetTexture(1, 0);
		m_Direct3DDevice.SetTexture(2, 0);
		m_Direct3DDevice.SetTexture(3, 0);
		m_Direct3DDevice.SetTexture(4, 0);
		m_Direct3DDevice.SetTexture(5, 0);
		m_Direct3DDevice.SetTexture(6, 0);
		m_Direct3DDevice.SetTexture(7, 0);
		m_Direct3DDevice.SetRenderState(D3DRS_ZWRITEENABLE,        FALSE);
		m_Direct3DDevice.SetRenderState(D3DRS_ZENABLE,             FALSE);

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
		m_Direct3DDevice.SetPixelShader( m_pCopyShader );
		NSCALL(m_Direct3DDevice.CreateStateBlock(D3DSBT_ALL, &m_pCopyState));

		NSCALL(m_pSavedState->Apply());
	}
	//--- create font states ---
	{
		NSCALL(m_Direct3DDevice.CreateVertexShader((DWORD*)g_vs20_VSFont, &m_pVSFont));
		NSCALL(m_Direct3DDevice.CreatePixelShader((DWORD*)g_ps20_PSFont, &m_pPSFont));
		{
			m_Direct3DDevice.SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
			m_Direct3DDevice.SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
			m_Direct3DDevice.SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
			m_Direct3DDevice.SetRenderState(D3DRS_ZWRITEENABLE,        FALSE);
			m_Direct3DDevice.SetRenderState(D3DRS_ZENABLE,             FALSE);
			m_Direct3DDevice.SetRenderState(D3DRS_WRAP0,              0);
			m_Direct3DDevice.SetRenderState(D3DRS_ALPHABLENDENABLE,   TRUE);
			m_Direct3DDevice.SetRenderState(D3DRS_DESTBLEND,			D3DBLEND_INVSRCALPHA);
			m_Direct3DDevice.SetRenderState(D3DRS_SRCBLEND,			D3DBLEND_SRCALPHA);
			m_Direct3DDevice.SetRenderState(D3DRS_ALPHATESTENABLE,    FALSE);
			m_Direct3DDevice.SetRenderState(D3DRS_FOGENABLE,          FALSE);
			m_Direct3DDevice.SetRenderState(D3DRS_FILLMODE,           D3DFILL_SOLID);
			m_Direct3DDevice.SetRenderState(D3DRS_CULLMODE,           D3DCULL_NONE);
			m_Direct3DDevice.SetRenderState(D3DRS_LIGHTING,			FALSE);
			m_Direct3DDevice.SetRenderState(D3DRS_STENCILENABLE,		FALSE);
			m_Direct3DDevice.SetRenderState(D3DRS_CLIPPING,			FALSE);
			m_Direct3DDevice.SetRenderState(D3DRS_CLIPPLANEENABLE,	FALSE);
			m_Direct3DDevice.SetRenderState(D3DRS_SRGBWRITEENABLE,	FALSE);
			m_Direct3DDevice.SetRenderState(D3DRS_DIFFUSEMATERIALSOURCE,  D3DMCS_COLOR1);
			m_Direct3DDevice.SetRenderState(D3DRS_AMBIENTMATERIALSOURCE,  D3DMCS_COLOR1);
			m_Direct3DDevice.SetRenderState(D3DRS_SPECULARMATERIALSOURCE, D3DMCS_MATERIAL);
			m_Direct3DDevice.SetRenderState(D3DRS_EMISSIVEMATERIALSOURCE, D3DMCS_MATERIAL);
			m_Direct3DDevice.SetRenderState(D3DRS_NORMALIZENORMALS,       TRUE);

			m_Direct3DDevice.SetPixelShader( m_pPSFont );
			m_Direct3DDevice.SetVertexShader( m_pVSFont );
			NSCALL(m_Direct3DDevice.CreateStateBlock(D3DSBT_ALL, &m_pTextState));
			DEBUG_MESSAGE(_T("Font initialized\n"));
		}
		NSCALL(m_pSavedState->Apply());
		m_pFont = NULL;
		m_pDynRender = NULL;
	}

	InitPerfHUD();

	//--- re-hook IDirect3DStateBlock9->Apply() ---
	if(m_pSavedState)
	{
		void** p = *(void***)m_pSavedState.p;
		UniqueHookCode(this, p[5], Proxy_StateBlock_Apply, (PVOID*)&Original_StateBlock_Apply);
#ifndef FINAL_RELEASE
		UniqueHookCode(this, p[4], Proxy_StateBlock_Capture, (PVOID*)&Original_StateBlock_Capture);
#endif // FINAL_RELEASE
	}

	HOOK_GET_DEVICE(&GetBaseSC()->m_pPrimaryBackBuffer.p, Proxy_Surface_GetDevice, Original_Surface_GetDevice);
	CComPtr<IDirect3DSurface9> leftSurface;
	GetBaseSC()->m_pLeftMethodTexture->GetSurfaceLevel(0, &leftSurface);
	HOOK_GET_DEVICE(&leftSurface.p, Proxy_TextureSurface_GetDevice, Original_TextureSurface_GetDevice);
	HOOK_GET_DEVICE(&GetBaseSC()->m_pLeftMethodTexture.p, Proxy_Texture_GetDevice, Original_Texture_GetDevice);

	//HOOK_RELEASE(m_pPrimaryBackBuffer.p, Proxy_Surface_Release, Original_Surface_Release);
	//HOOK_RELEASE(m_pLeftMethodRenderTargetTexture.p, Proxy_Texture_Release, Original_Texture_Release);
	//HOOK_RELEASE(m_pPSFont.p, Proxy_PixelShader_Release, Original_PixelShader_Release);
	//HOOK_RELEASE(m_pVSFont.p, Proxy_VertexShader_Release, Original_VertexShader_Release);

	InitViewport(&m_ViewPort, 0, 0, GetBaseSC()->m_BackBufferSizeBeforeScaling.cx, GetBaseSC()->m_BackBufferSizeBeforeScaling.cy);
	SetRect(&m_ScissorRect, 0, 0, GetBaseSC()->m_BackBufferSizeBeforeScaling.cx, GetBaseSC()->m_BackBufferSizeBeforeScaling.cy);
	m_PrimaryRTSize = GetBaseSC()->m_BackBufferSizeBeforeScaling;
	m_bFullSizeViewport = TRUE;
	m_bFullSizeScissorRect = TRUE;

	if (0) // only for debugging reference counting
	{
		HOOK_ADDREF(GetBaseSC()->m_pLeftMethodTexture.p, Proxy_Texture_AddRef, Original_Texture_AddRef);
		CComPtr<IDirect3DSurface9> pLeftSurface;
		GetBaseSC()->m_pLeftMethodTexture->GetSurfaceLevel(0, &pLeftSurface);
		HOOK_ADDREF(pLeftSurface.p, Proxy_TexSurf_AddRef, Original_TexSurf_AddRef);
		HOOK_ADDREF(m_DepthStencil.m_pMainSurf, Proxy_Surface_AddRef, Original_Surface_AddRef);
		HOOK_ADDREF(m_pPSFont.p, Proxy_PixelShader_AddRef, Original_PixelShader_AddRef);
		HOOK_ADDREF(m_pVSFont.p, Proxy_VertexShader_AddRef, Original_VertexShader_AddRef);
	}

	//--- use multi hook instead ---
	HOOK_RELEASE(m_DepthStencil.m_pMainSurf, Proxy_Surface_Release, Original_Surface_Release);
	//HOOK_RELEASE(m_DepthStencil.m_pMainSurf, Proxy_Surface_And_SwapChain_Release, Original_Surface_And_SwapChain_Release);

	if (0) // only for debugging reference counting
	{
		HOOK_RELEASE(GetBaseSC()->m_pLeftMethodTexture.p, Proxy_Texture_Release, Original_Texture_Release);
		CComPtr<IDirect3DSurface9> pLeftSurface;
		GetBaseSC()->m_pLeftMethodTexture->GetSurfaceLevel(0, &pLeftSurface);
		HOOK_RELEASE(pLeftSurface.p, Proxy_TexSurf_Release, Original_TexSurf_Release);
		HOOK_RELEASE(m_pPSFont.p, Proxy_PixelShader_Release, Original_PixelShader_Release);
		HOOK_RELEASE(m_pVSFont.p, Proxy_VertexShader_Release, Original_VertexShader_Release);
	}

	NSCALL(m_Direct3DDevice.SetDepthStencilSurface(m_DepthStencil.m_pMainSurf));
	if (m_RenderTarget[1].m_pRightSurf)
	{
		NSCALL(m_Direct3DDevice.SetRenderTarget(0, m_RenderTarget[1].m_pRightSurf));
		NSCALL(m_Direct3DDevice.Clear(0, NULL, D3DCLEAR_TARGET, 0xFF000000, 0.0f, 0)); // Clear buffers
	}
	NSCALL(m_Direct3DDevice.SetRenderTarget(0, m_RenderTarget[0].m_pMainSurf));
	NSCALL(m_Direct3DDevice.Clear(0, NULL, D3DCLEAR_TARGET, 0xFF000000, 0.0f, 0)); // Clear buffers
	NSCALL(m_Direct3DDevice.SetViewport(&m_ViewPort));
	NSCALL(m_Direct3DDevice.SetScissorRect(&m_ScissorRect));

	// Mouse Lock Communication
	m_hMapFile = OpenGlobalFileMapping("iZ3DMouseLocked", TRUE);
	//if (!m_hMapFile)	
	//	m_hMapFile = CreateGlobalFileMapping("iZ3DMouseLocked", 1);
	if (m_hMapFile)
	{
		m_pVar = (DWORD*) MapViewOfFile(m_hMapFile, FILE_MAP_ALL_ACCESS, 0, 0, 1); 
		if (!m_pVar)
			CloseHandle(m_hMapFile);
	}
	else
		m_pVar = NULL;

	if(IS_SHUTTER_OUTPUT && !USE_ATI_PRESENTER
#ifndef FINAL_RELEASE
		|| gInfo.WideRenderTarget
#endif
		)
	{
		NSCALL(m_Direct3DDevice.CreateQuery(D3DQUERYTYPE_EVENT, &m_pFlushQuery)); 

		LARGE_INTEGER time;
		QueryPerformanceCounter(&time);
		SetNextFlushTime(time);
		if (gInfo.PresenterFlushTime < 0.0f)
			m_PresenterFlushTime = 1;
		else if (gInfo.PresenterFlushTime == 0.0f)
			m_PresenterFlushTime = 50;
		else
			m_PresenterFlushTime = gInfo.PresenterFlushTime;
	}

	if(USE_IZ3DKMSERVICE_PRESENTER && IsFullscreen())
		g_KMShutter.StartShutter();

	if (m_pOutputMethod->GetCaps() & ocHardwareMouseCursorNotSupported)
	{
		CURSORINFO info;
		info.cbSize = sizeof(CURSORINFO);
		GetCursorInfo(&info);

		m_bShowGDIMouse = (info.flags == CURSOR_SHOWING);
		m_hGDIMouseCursor = info.hCursor;
		//if (m_bShowGDIMouse)
		//	::ShowCursor(FALSE);

		if (info.hCursor != NULL)
		{
			ICONINFO iconinfo;
			GetIconInfo(info.hCursor, &iconinfo);
			m_MouseXHotSpot = iconinfo.xHotspot;
			m_MouseYHotSpot = iconinfo.yHotspot;	
			D3DPOOL TexPool = m_Direct3DDevice.getExMode() != EXMODE_NONE ? D3DPOOL_DEFAULT : D3DPOOL_MANAGED;
			HRESULT hr = m_Direct3DDevice.CreateTexture(32, 32, 1, 0, D3DFMT_A8R8G8B8, TexPool,
				&m_pMouseCursorTexture, NULL);
			m_pMouseCursorTexture->GetSurfaceLevel(0, &m_pMouseCursor);
			if (iconinfo.hbmColor != NULL)
				UpdateCursorTexture(iconinfo.hbmColor);
		}
	}

#ifndef FINAL_RELEASE
	m_deviceState = CDeviceState();
#endif

	GetBaseSC()->CheckSecondWindowPosition();
	CheckEngine();
	
	m_bEngineStarted = TRUE;

	return hResult;
}

void CBaseStereoRenderer::UpdateCursorTexture( HBITMAP hCursorImage )
{
	_ASSERT(m_pMouseCursor);
	HRESULT hr;
	DIBSECTION bmp;
	int nBytes = ::GetObject( hCursorImage, sizeof( DIBSECTION ), &bmp );
	if (nBytes != 0)
	{
		int width = bmp.dsBm.bmWidth;
		int height = bmp.dsBm.bmHeight;
		int bpp = bmp.dsBm.bmBitsPixel;
		int pitch = (((width*bpp)+31)/32)*4;

		if (bpp == 32)
		{
			D3DLOCKED_RECT lockedRect;
			hr = m_pMouseCursor->LockRect(&lockedRect, NULL, 0);
			if (SUCCEEDED(hr))
			{
				void* pBits;
				if( nBytes == sizeof( BITMAP ) )
				{
					pBits = new char[pitch * height];
					GetBitmapBits(hCursorImage, pitch * height, pBits);
				}
				else
				{
					ATLASSERT( nBytes == sizeof( DIBSECTION ) );
					pBits = (PBYTE)bmp.dsBm.bmBits;
				}

				PBYTE pSrcBits = (PBYTE)pBits;
				PBYTE pDstBits = (PBYTE)lockedRect.pBits;
				for(int y = 0; y < height; y++)
				{
					memcpy(pDstBits, pSrcBits, width * 4);
					pDstBits += lockedRect.Pitch;
					pSrcBits += pitch;
				}

				if( nBytes == sizeof( BITMAP ) )
					delete [] pBits;

				m_pMouseCursor->UnlockRect();
			}
		}
	}
}

void CBaseStereoRenderer::CreateFont()
{
	m_bStereoModeChanged = true;
	m_pDynRender = DNew acGraphics::CDynRender_dx9(m_Direct3DDevice);		
	m_pFont = DNew CFont();
	TCHAR fontPath[MAX_PATH];
	if (iz3d::resources::GetAllUsersDirectory( fontPath ))
	{
		PathAppend(fontPath, _T("Font\\OSDFont.fnt"));
		m_pFont->Init(fontPath, m_pDynRender);
	}
}

HRESULT CBaseStereoRenderer::StopEngine( ) 
{
	INSIDE;
	RouterTypeHookCallGuard<ProxyDirect9> RTdir(GetProxyDirect3D9());
	RouterTypeHookCallGuard<ProxyDevice9> RTdev(&m_Direct3DDevice);
	
	HRESULT hResult = S_OK;
	DEBUG_TRACE1(_T("StopEngine()\n"));
	m_bEngineStarted = FALSE;

	if (m_pVar)
	{
		*m_pVar = 0;
		UnmapViewOfFile(m_pVar);
		CloseHandle(m_hMapFile);
	}

	StopEngineMode();
	gInfo.SpanModeOn = FALSE;
	m_pSavedState.Release();
	m_VSPipelineData.Clear();
	m_PSPipelineData.Clear();

	if (m_Direct3DDevice)
	{
		if (GetBaseSC()->m_pPrimaryBackBuffer)
		{
			FreeStereoObject(GetBaseSC()->m_pPrimaryBackBuffer);
			m_Direct3DDevice.SetRenderTarget( 0, GetBaseSC()->m_pPrimaryBackBuffer);
			m_Direct3DDevice.SetDepthStencilSurface(GetBaseSC()->m_pPrimaryDepthStencil);
			m_Direct3DDevice.SetViewport(&m_ViewPort);
			m_Direct3DDevice.SetScissorRect(&m_ScissorRect);
		}
		if (GetBaseSC()->m_pPrimaryDepthStencil)
			FreeStereoObject(GetBaseSC()->m_pPrimaryDepthStencil);
	}

	if (m_pPresenter)
	{
		m_pPresenter->SetAction(ptClear);
		m_pPresenter->WaitForActionDone();
	}
	if (!m_pPresenterThread && m_pOutputMethod)
		m_pOutputMethod->Clear();
	m_pFlushQuery.Release();

	m_LaserSight.Clear();
#ifndef DISABLE_WIZARD
	m_Wizard.Clear();
#endif
	m_HotKeyOSD.Clear();
	//--- unhook Original_Surface_Release before GetMonoSC()->Clear(), madchook pNextHook check ---
	UniqueUnhookCode(this, (PVOID*)&Original_Surface_Release);	//--- use multiple hook instead ---
	//UniqueUnhookCode(this, (PVOID*)&Original_Surface_And_SwapChain_Release);

	ReleasePerfHUD();
	//_ASSERT(m_SwapChains.size() == 1);
	GetMonoSC()->Clear();
	//--- release our internal IDirect3DSwapChain9 reference, but keep wrapper ---
	GetMonoSC()->m_SwapChain.Release();
	
	//--- we need to collect statistic again after alt-tab ---
	if (m_pUMDeviceWrapper)
		m_pUMDeviceWrapper->KillPresenterThread();

	m_pViewShader.Release();
	m_pCopyShader.Release();
	m_pCopyState.Release();

	m_pVSFont.Release();
	m_pPSFont.Release();
	m_pTextState.Release();	
	SAFE_DELETE(m_pFont);
	SAFE_DELETE(m_pDynRender);
	m_pLine.Release();
	for (DWORD i = 0; i < m_RenderTarget.size(); i++)
		m_RenderTarget[i].Clear();
	m_DepthStencil.Clear();
	m_pLockVBData.Release();
	m_pSavedVertexBuffer.Release();
	m_pModifiedVertexBuffer.Release();
	if (m_pModifiedVertexStreamZeroData)
		delete [] m_pModifiedVertexStreamZeroData;
	m_pModifiedVertexStreamZeroData = NULL;
	m_pMouseCursor.Release();
	m_pMouseCursorTexture.Release();

#ifndef FINAL_RELEASE
	m_deviceState.Clear();
	m_tempDeviceState.Clear();
	m_StateBlocks.clear();
#endif

	WriteInputData(&m_Input);
	WriteStatistic();

#ifndef FINAL_RELEASE
	DEBUG_MESSAGE(_T("\n------------- Counters -------------\n"));
	DEBUG_MESSAGE(_T("Textures:\t\t\t%d (%.2f Mb)\n"), m_nTextureCounter, m_nTexturesSize / 1024.0f / 1024);
	DEBUG_MESSAGE(_T("Textures Default:\t\t%d (%.2f Mb)\n"), m_nTextureDefaultCounter, m_nTexturesDefaultSize / 1024.0f / 1024);
	DEBUG_MESSAGE(_T("RenderTarget textures:\t\t%d (%.2f Mb + %.2f Mb)\n"), m_nRenderTargetTextureCounter, m_nRenderTargetTexturesMonoSize / 1024.0f / 1024, m_nRenderTargetTexturesStereoSize / 1024.0f / 1024);
	DEBUG_MESSAGE(_T("RenderTarget R32F textures:\t%d\n"), m_nRenderTargetR32FTextureCounter);	
	DEBUG_MESSAGE(_T("RenderTarget square textures:\t%d\n"), m_nRenderTargetSquareTextureCounter);	
	DEBUG_MESSAGE(_T("RenderTarget lest than BB textures:\t%d\n"), m_nRenderTargetLessThanBBTextureCounter);	
	DEBUG_MESSAGE(_T("DepthStencil textures:\t\t%d (%.2f Mb + %.2f Mb)\n"), m_nDepthStencilTextureCounter, m_nDepthStencilTexturesMonoSize / 1024.0f / 1024, m_nDepthStencilTexturesStereoSize / 1024.0f / 1024);
	DEBUG_MESSAGE(_T("CubeTextures:\t\t\t%d (%.2f Mb)\n"), m_nCubeTextureCounter, m_nCubeTexturesSize / 1024.0f / 1024);
	DEBUG_MESSAGE(_T("CubeTextures Default:\t\t%d (%.2f Mb)\n"), m_nCubeTextureDefaultCounter, m_nCubeTexturesDefaultSize / 1024.0f / 1024);
	DEBUG_MESSAGE(_T("VolumeTextures:\t\t\t%d (%.2f Mb)\n"), m_nVolumeTextureCounter, m_nVolumeTexturesSize / 1024.0f / 1024);
	DEBUG_MESSAGE(_T("VolumeTextures Default:\t\t%d (%.2f Mb)\n"), m_nVolumeTextureDefaultCounter, m_nVolumeTexturesDefaultSize / 1024.0f / 1024);
	DEBUG_MESSAGE(_T("RenderTarget cubetextures:\t%d (%.2f Mb + %.2f Mb)\n"), m_nRenderTargetCubeTextureCounter, m_nRenderTargetCubeTexturesMonoSize / 1024.0f / 1024, m_nRenderTargetCubeTexturesStereoSize / 1024.0f / 1024);
	DEBUG_MESSAGE(_T("DepthStencil cubetextures:\t%d (%.2f Mb + %.2f Mb)\n"), m_nDepthStencilCubeTextureCounter, m_nDepthStencilCubeTexturesMonoSize / 1024.0f / 1024, m_nDepthStencilCubeTexturesStereoSize / 1024.0f / 1024);
	DEBUG_MESSAGE(_T("RenderTarget surfaces:\t\t%d (%.2f Mb + %.2f Mb)\n"), m_nRenderTargetCounter, m_nRenderTargetsMonoSize / 1024.0f / 1024, m_nRenderTargetsStereoSize / 1024.0f / 1024);
	DEBUG_MESSAGE(_T("DepthStencil surfaces:\t\t%d (%.2f Mb + %.2f Mb)\n\n"), m_nDepthStencilSurfaceCounter, m_nDepthStencilSurfacesMonoSize / 1024.0f / 1024, m_nDepthStencilSurfacesStereoSize / 1024.0f / 1024);
#endif

	return hResult;
}

void CBaseStereoRenderer::HookDevice()
{
	if(!g_pRendererList.IsWrapperInList(this))
		return;
	//--- Hooks for PrepareViewStage() optimization ---
#define HOOKINTERFACEENTRY(x) m_Direct3DDevice.HookInterfaceEntry(this, _ ##x ##_, Proxy_ ##x)
	HOOKINTERFACEENTRY(SetVertexDeclaration);
	HOOKINTERFACEENTRY(SetFVF);
	HOOKINTERFACEENTRY(SetVertexShader);
	HOOKINTERFACEENTRY(GetVertexShader);
	HOOKINTERFACEENTRY(SetPixelShader);
	HOOKINTERFACEENTRY(SetStreamSource);
	HOOKINTERFACEENTRY(SetTexture);
	HOOKINTERFACEENTRY(SetRenderState);

	HOOKINTERFACEENTRY(SetVertexShaderConstantF);
	HOOKINTERFACEENTRY(GetVertexShaderConstantF);
	HOOKINTERFACEENTRY(SetTransform);
	HOOKINTERFACEENTRY(MultiplyTransform);
	HOOKINTERFACEENTRY(SetViewport);
	HOOKINTERFACEENTRY(SetScissorRect);
#undef HOOKINTERFACEENTRY
	
	if(gInfo.RouterType == ROUTER_TYPE_HOOK)
		HookIDirect3DDevice9All();

	m_Direct3DDevice.refreshMutablePointers();
}

void CBaseStereoRenderer::UnhookDevice()
{
	if(!g_pRendererList.IsWrapperInList(this))
		return;
#define UNHOOKINTERFACEENTRY(x) m_Direct3DDevice.UnhookInterfaceEntry(this, _ ##x ##_)
	UNHOOKINTERFACEENTRY(SetVertexDeclaration);
	UNHOOKINTERFACEENTRY(SetFVF);
	UNHOOKINTERFACEENTRY(SetVertexShader);
	UNHOOKINTERFACEENTRY(GetVertexShader);
	UNHOOKINTERFACEENTRY(SetPixelShader);
	UNHOOKINTERFACEENTRY(SetStreamSource);
	UNHOOKINTERFACEENTRY(SetTexture);
	UNHOOKINTERFACEENTRY(SetRenderState);

	UNHOOKINTERFACEENTRY(SetVertexShaderConstantF);
	UNHOOKINTERFACEENTRY(GetVertexShaderConstantF);
	UNHOOKINTERFACEENTRY(SetTransform);
	UNHOOKINTERFACEENTRY(MultiplyTransform);
	UNHOOKINTERFACEENTRY(SetViewport);
	UNHOOKINTERFACEENTRY(SetScissorRect);
#undef UNHOOKINTERFACEENTRY
	
	UniqueUnhookCode(this, (PVOID*)&Original_VertexBuffer_Lock);
	UniqueUnhookCode(this, (PVOID*)&Original_VertexBuffer_Unlock);
	UniqueUnhookCode(this, (PVOID*)&Original_DriverVertexBuffer_Lock);
	UniqueUnhookCode(this, (PVOID*)&Original_DriverVertexBuffer_Unlock);

	if(gInfo.RouterType == ROUTER_TYPE_HOOK)
		UnHookIDirect3DDevice9All();

	m_Direct3DDevice.refreshMutablePointers();
}

void CheckTime();

HRESULT CBaseStereoRenderer::CheckEngine() 
{
	HRESULT hResult = S_OK;

//	CheckTime();

	DEBUG_TRACE3(_T("m_nDrawCounter = %d (mono %d, VS %d, fixed %d, RHW %d, Heavy %d)\n"), m_nDrawCounter, m_nDrawMonoCounter, m_nDrawVSCounter, m_nDrawFixedCounter, m_nDrawRHWCounter, m_nDrawHeavyCounter);
	m_nEventCounter = 0;
	m_nDrawCounter = 0;
	m_nDrawMonoCounter = 0;
	m_nDrawVSCounter = 0;
	m_nDrawRHWCounter = 0;
	m_nDrawFixedCounter = 0;
	m_nDrawHeavyCounter = 0;
#ifndef FINAL_RELEASE
	DEBUG_TRACE3(_T("m_nTexSynchCounter = %d\n"), m_nTexSynchCounter);
	DEBUG_TRACE3(_T("m_nTexSynchRTCounter = %d\n"), m_nTexSynchRTCounter);	
	DEBUG_TRACE3(_T("m_nTexSynchCachedCounter = %d\n"), m_nTexSynchCachedCounter);
	m_nTexSynchCounter = 0;
	m_nTexSynchRTCounter = 0;
	m_nTexSynchCachedCounter = 0;
	m_nTexSynchTime.QuadPart = 0;
	m_nTexSynchRTTime.QuadPart = 0;	
	m_nWeaponDrawCounter = 0;

	if (GINFO_DEBUG) 
	{
		if (IsKeyPressed(VK_F11))
			m_bDebugUseSimplePresenter = !m_bDebugUseSimplePresenter;
		if (IsKeyPressed('1'))
			m_bDebugUseDrawPassSplitting = !m_bDebugUseDrawPassSplitting;
		if (IsKeyPressed(VK_OEM_PLUS))
			m_bDebugDoNotRenderMono = !m_bDebugDoNotRenderMono;	
		if (IsKeyPressed(VK_NUMPAD7))
			m_bDebugSkipUpdateProjectionMatrix = !m_bDebugSkipUpdateProjectionMatrix;
		if (IsKeyPressed(VK_NUMPAD8))
			m_bDebugSkipModifyRHW = !m_bDebugSkipModifyRHW;
		if (IsKeyPressed(VK_NUMPAD9))
			m_bDebugSkipSetProjection = !m_bDebugSkipSetProjection;
		if (IsKeyPressed(VK_NUMPAD4))
			m_bDebugRenderInMono =	!m_bDebugRenderInMono;
		if (IsKeyPressed(VK_NUMPAD5))
			m_bDebugSkipSetStereoTextures =	!m_bDebugSkipSetStereoTextures;
		if (IsKeyPressed(VK_NUMPAD6))
			m_bDebugSkipCheckCRC =	!m_bDebugSkipCheckCRC;
		if (IsKeyPressed(VK_NUMPAD1))
			m_bDebugSkipShaderMatrixAnalyzing =	!m_bDebugSkipShaderMatrixAnalyzing;
		if (IsKeyPressed(VK_NUMPAD2))
			m_bDebugSkipVDAnalyzing = !m_bDebugSkipVDAnalyzing;
		if (IsKeyPressed('0'))
			m_bDebugSkipTextureUsingCheck = !m_bDebugSkipTextureUsingCheck;
		if (IsKeyPressed(VK_NUMPAD3))
			m_bDebugSkipSetStereoRenderTarget =	!m_bDebugSkipSetStereoRenderTarget;
		if (IsKeyPressed(VK_NUMPAD0))	
			m_bDebugSkipSynchronizeTextures = !m_bDebugSkipSynchronizeTextures;
		if (IsKeyPressed(VK_DECIMAL))
			m_bDebugSkipSynchronizeDepthTextures = !m_bDebugSkipSynchronizeDepthTextures;
		if (IsKeyPressed(VK_OEM_5))
			m_bDebugRenderInOneRT =	!m_bDebugRenderInOneRT;
		if (IsKeyPressed('P'))
			g_Profiler.clear();

		if(m_DumpType >= dtCommands  || 
			(m_DumpType == dtTrace && IsKeyDown(VK_PAUSE) && IsKeyDown(VK_SHIFT)))
		{
			Beep(200, 300);
			if (m_DumpType >= dtOnlyRT)
				zlog::SetupSeverity(m_nOldTrace);
			else
			{
				m_nOldTrace = zlog::SV_MESSAGE;
				zlog::SetupSeverity(zlog::SV_MESSAGE);
			}
			DumpManager::Get().WaitWhenFinish();
			m_DumpType = dtNone;
			float min = m_nGlobalLastFrameTime.QuadPart / 60.0f / m_nFreq.QuadPart;
			DEBUG_MESSAGE(_T("Dump time: %f min.\n"), min);
		}
		else
		{
			bool bStartDump = false;
			if (GINFO_DUMP_ON && IsKeyDown(VK_F11) && (IsKeyDown(VK_LMENU) || IsKeyDown(VK_RMENU))) //Alt + F11
			{
				m_DumpType = dtCommands;
				bStartDump = true;

				m_deviceState.Dump(TRUE);
				m_deviceState.DumpBaseSurfaces(m_Direct3DDevice);
			}			
			else if (IsKeyDown(VK_SCROLL) && !IsKeyDown(VK_SHIFT)) // ScrollLock
			{
				if (IsKeyDown(VK_LMENU) || IsKeyDown(VK_RMENU)) //Alt
					m_DumpType = dtOnlyRT;
				else
					m_DumpType = dtFull;
				bStartDump = true;

				gData.DumpIndex++;
				DEBUG_MESSAGE(_T("Dump index: %d\n"), gData.DumpIndex);
				//--- Create directories for debug images   ---
				m_DumpDirectory[0] = '\0';
				m_MethodDirectory[0] = '\0';
				if (gData.DumpIndex == 1)
				{
					_stprintf_s(m_DumpDirectory, L"%s\\%s", DUMPDIR, L"Draw History");
					_stprintf_s(m_MethodDirectory, L"%s\\%s", DUMPDIR, L"Method");
				}
				else
				{
					_stprintf_s(m_DumpDirectory, L"%s\\%s-%d", DUMPDIR, L"Draw History", gData.DumpIndex);
					_stprintf_s(m_MethodDirectory, L"%s\\%s-%d", DUMPDIR, L"Method", gData.DumpIndex);
				}
				CreateDirectory(m_DumpDirectory, 0);
				DeleteAllFiles(m_DumpDirectory);
				CreateDirectory(m_MethodDirectory, 0);
				DeleteAllFiles(m_MethodDirectory);
			}
			else if ((m_DumpType == dtNone) && IsKeyDown(VK_PAUSE) && !IsKeyDown(VK_SHIFT))
			{
				m_DumpType = dtTrace;
				bStartDump = true;
			}

			if (bStartDump) 
			{
				m_nOldTrace = zlog::GetSeverity();
				zlog::SetupSeverity(zlog::SV_FLOOD);
				Beep(1000, 100);
				DEBUG_MESSAGE(_T("Start Tracing (%s)\n"), m_Input.StereoActive ? _T("Stereo") : _T("Mono"));

				// for log
				UpdateProjectionMatrix();
			}
		}
	}
#endif
	return hResult;
}

CBaseStereoRenderer::~CBaseStereoRenderer()
{
	INSIDE;
	Wiz3D_UnregisterActiveRenderer(this);
	gKbdHook.clear();
	
	{
		RouterTypeHookCallGuard<ProxyDevice9> RT(&m_Direct3DDevice);

		m_pATICommSurface = 0;
		ReleaseStereoParamsTexture();
		_aligned_free(m_VertexShaderRegister);
		m_VertexShaderRegister = NULL;

		m_VertexShaderConverter.Clear();
		m_PixelShaderConverter.Clear();
	}

	UniqueUnhookCode(this, (PVOID*)&Original_StateBlock_Apply);
#ifndef FINAL_RELEASE
	UniqueUnhookCode(this, (PVOID*)&Original_StateBlock_Capture);
#endif // FINAL_RELEASE

	//-- clear one-time created hook ---
	UNHOOK_GET_DEVICE(Original_TextureSurface_GetDevice);		
	UNHOOK_GET_DEVICE(Original_Surface_GetDevice);				
	UNHOOK_GET_DEVICE(Original_Texture_GetDevice);

	if (gInfo.RHWModificationMethod == 1)
	{
		UniqueUnhookCode(this, (PVOID*)&Original_VertexBuffer_Lock);	
		UniqueUnhookCode(this, (PVOID*)&Original_VertexBuffer_Unlock);

		UniqueUnhookCode(this, (PVOID*)&Original_DriverVertexBuffer_Lock);	
		UniqueUnhookCode(this, (PVOID*)&Original_DriverVertexBuffer_Unlock);
	}

	if (m_pOutputMethod)
	{
		delete m_pOutputMethod;
		m_pOutputMethod = NULL;
	}

	UnhookDevice();

	if (m_Direct3DDevice)
	{
		for (DWORD i = 0 ; i < m_PSPipelineData.m_MaxTextures; i++) // HACK: Fix memory leak on Nvidia
				m_Direct3DDevice.SetTexture(i, NULL);	
		for (DWORD i = 0 ; i < m_VSPipelineData.m_MaxTextures; i++) // HACK: Fix memory leak on Nvidia
			m_Direct3DDevice.SetTexture(i + D3DDMAPSAMPLER, NULL);
		if (m_bRHWVertexDeclaration)
			m_Direct3DDevice.SetStreamSource(m_dwStreamRHWVertices, NULL, 0, 0);
	}

	if (USE_MULTI_THREAD_PRESENTER || USE_MULTI_DEVICE_PRESENTER)
	{
		//--- Warning! Device will be released at this point, but m_SwapChain.Release() ---
		//--- in CMonoSwapChain::~CMonoSwapChain() will be crashed. SwapChain need alive IDirect3DDevice, ---
		//--- but m_SwapChain.Release() do not decrease reference count ---
		//--- because of this  we must release SwapChain first. ---

		GetBaseSC()->ReleaseSC(); // release before multi thread presenter
		m_Direct3DDevice.clear();
		SAFE_DELETE( m_pPresenterThread );
	}

	DEBUG_MESSAGE(_T(__FUNCTION__) _T("released\n"));
}

void CBaseStereoRenderer::UpdateStatisticData()
{
	if (m_fCurSessionTime > 0.125f)
	{
		DEBUG_MESSAGE(_T("\n------------- Session End -------------\n"));
		if (m_Input.StereoActive != 0)
		{
			DEBUG_MESSAGE(_T("Stereo:		\t%fm\n\n"),	m_fCurSessionTime);
			m_fTotalStereoTime += m_fCurSessionTime;
			m_fMaxStereoTime = std::max(m_fMaxStereoTime, m_fCurSessionTime);
			m_nStereoSessionsCounter++;
		}
		else
		{
			DEBUG_MESSAGE(_T("Mono:		\t%fm\n\n"),		m_fCurSessionTime);
			m_fTotalMonoTime += m_fCurSessionTime;
			m_fMaxMonoTime = std::max(m_fMaxMonoTime, m_fCurSessionTime);
			m_nMonoSessionsCounter++;
		}

#ifndef FINAL_RELEASE
		//------------------------------------- Write UM call information -----------------------------------
		DEBUG_MESSAGE(L"-------------------------------- Profiler data ----------------------------------------\n");
		std::vector<RecordData>  timeData;

		for(CallProfiler::iterator it = g_Profiler.begin(); it != g_Profiler.end(); ++it)
		{
			RecordData rd(it->first.c_str(), it->second);
			timeData.push_back(rd);
		}

		sort(timeData.begin(), timeData.end(), SortRecordData());
		DEBUG_MESSAGE(L"Profiler UM functions count: %d\n", timeData.size());

		LARGE_INTEGER freq;
		QueryPerformanceFrequency(&freq);
		int i = 0;
		for(std::vector<RecordData>::iterator it = timeData.begin(); it != timeData.end(); ++it)
		{
			float totalTime = (1000.f * it->second.totalTime / freq.QuadPart);
			float averTime = totalTime / it->second.callCount; 
			DEBUG_MESSAGE(_T("%35s: aver. time = %3.6f, total time = %8.6f\n"), it->first, averTime, totalTime);
			i++;
			if (i == 20)
				break;
		}
		g_Profiler.clear();
#endif
	}
	m_fCurSessionTime = 0.0f;
}

void CBaseStereoRenderer::DrawMouseCursor( CBaseSwapChain* pSwapChain )
{
	INSIDE;
	if (!(m_pOutputMethod->GetCaps() & ocHardwareMouseCursorNotSupported) || 
		!m_pMouseCursorTexture)
		return;

	CURSORINFO info;
	info.cbSize = sizeof(CURSORINFO);
	GetCursorInfo(&info);

	if (!m_bShowMouse && info.flags == CURSOR_SHOWING)
	{
		m_bShowGDIMouse = TRUE;
		//::ShowCursor(FALSE);

		if (m_hGDIMouseCursor != info.hCursor)
		{
			m_hGDIMouseCursor = info.hCursor;

			ICONINFO iconinfo;
			GetIconInfo(info.hCursor, &iconinfo);
			m_MouseXHotSpot = iconinfo.xHotspot;
			m_MouseYHotSpot = iconinfo.yHotspot;	
			if (iconinfo.hbmColor != NULL)
				UpdateCursorTexture(iconinfo.hbmColor);			
		}
	}

	if (!(m_bShowMouse || m_bShowGDIMouse))
		return;

	POINT p = info.ptScreenPos;
	ScreenToClient(pSwapChain->GetAppWindow(), &p); 

	if (p.x < 0 || p.x >= (LONG)m_ViewPort.Width ||
		p.y < 0 || p.y >= (LONG)m_ViewPort.Height)
		return;

	HRESULT hResult = S_OK;
	NSCALL(m_pTextState->Apply());
	D3DXMATRIX mtx;
	// Setup orthogonal view
	// Origin is in lower-left corner
	D3DXMatrixOrthoOffCenterLH(&mtx, 0, (float)m_ViewPort.Width, (float)m_ViewPort.Height, 0, 1.0f, 1e+7f);
	D3DXMATRIX mtx2;
	D3DXMatrixTranspose(&mtx2, &mtx);
	m_pDynRender->SetVertexShaderConstantF(0, &mtx2._11, 4 );

	D3DSURFACE_DESC desc;
	m_pMouseCursor->GetDesc(&desc);

	m_Direct3DDevice.SetTexture(0, m_pMouseCursorTexture);

	DWORD rt[4] = { true, false, false, false };
	bool bStereoOn = RenderToRight() && RenderInStereo();
	if (bStereoOn)
	{
		m_pUpdateRTs = rt;
		m_UpdateDS = false;
	}
	for (ULONG nViewIndex = (bStereoOn ? VIEWINDEX_RIGHT : VIEWINDEX_LEFT); 
		nViewIndex >= VIEWINDEX_LEFT; nViewIndex--)
	{
		if (bStereoOn)
		{
			if(nViewIndex == VIEWINDEX_RIGHT)
				SetStereoRenderTarget<VIEWINDEX_RIGHT>();
			else
				SetStereoRenderTarget<VIEWINDEX_LEFT>();
		}

		m_pDynRender->Begin( acGraphics::RENDER_QUAD_LIST );

		float rScaleW = 1.0f / desc.Width;
		float rScaleH = 1.0f / desc.Height;

		float u = 0.5f * rScaleW;
		float v = 0.5f * rScaleH;
		float u2 = u + 1.0f;
		float v2 = v + 1.0f;

		float w = float(desc.Width);
		float h = float(desc.Height);
		float x = float(p.x - m_MouseXHotSpot);
		float y = float(p.y - m_MouseYHotSpot);
		float z = 1.0f;

		m_pDynRender->VtxData(0);
		m_pDynRender->VtxTexCoord(u, v);
		m_pDynRender->VtxPos(x, y, z);
		m_pDynRender->VtxTexCoord(u2, v);
		m_pDynRender->VtxPos(x+w, y, z);
		m_pDynRender->VtxTexCoord(u2, v2);
		m_pDynRender->VtxPos(x+w, y+h, z);
		m_pDynRender->VtxTexCoord(u, v2);
		m_pDynRender->VtxPos(x, y+h, z);

		m_pDynRender->End();
	}

	m_Direct3DDevice.SetTexture(0, NULL);
}

void CBaseStereoRenderer::DrawLogo( CBaseSwapChain* pSwapChain )
{
	INSIDE;
	IDirect3DTexture9* pLogo = m_pOutputMethod->GetLogo();
	if (!pLogo)
		return;

	HRESULT hResult = S_OK;
	NSCALL(m_pTextState->Apply());
	D3DXMATRIX mtx;
	// Setup orthogonal view
	// Origin is in lower-left corner
	D3DXMatrixOrthoOffCenterLH(&mtx, 0, (float)m_ViewPort.Width, (float)m_ViewPort.Height, 0, 1.0f, 1e+7f);
	D3DXMATRIX mtx2;
	D3DXMatrixTranspose(&mtx2, &mtx);
	m_pDynRender->SetVertexShaderConstantF(0, &mtx2._11, 4 );

	D3DSURFACE_DESC desc;
	pLogo->GetLevelDesc(0, &desc);

	m_Direct3DDevice.SetTexture(0, pLogo);

	DWORD rt[4] = { true, false, false, false };
	bool bStereoOn = RenderToRight() && RenderInStereo();
	if (bStereoOn)
	{
		m_pUpdateRTs = rt;
		m_UpdateDS = false;
	}
	for (ULONG nViewIndex = (bStereoOn ? VIEWINDEX_RIGHT : VIEWINDEX_LEFT); 
		nViewIndex >= VIEWINDEX_LEFT; nViewIndex--)
	{
		if (bStereoOn)
		{
			if(nViewIndex == VIEWINDEX_RIGHT)
				SetStereoRenderTarget<VIEWINDEX_RIGHT>();
			else
				SetStereoRenderTarget<VIEWINDEX_LEFT>();
		}

		m_pDynRender->Begin( acGraphics::RENDER_QUAD_LIST );

		float rScaleW = 1.0f / desc.Width;
		float rScaleH = 1.0f / desc.Height;

		float u = 0.5f * rScaleW;
		float v = 0.5f * rScaleH;
		float u2 = u + 1.0f;
		float v2 = v + 1.0f;

		float w = float(desc.Width);
		float h = float(desc.Height);
		float x = float(m_ViewPort.Width - desc.Width - 30);
		float y = float(30);
		float z = 1.0f;

		m_pDynRender->VtxData(0);
		m_pDynRender->VtxTexCoord(u, v);
		m_pDynRender->VtxPos(x, y, z);
		m_pDynRender->VtxTexCoord(u2, v);
		m_pDynRender->VtxPos(x+w, y, z);
		m_pDynRender->VtxTexCoord(u2, v2);
		m_pDynRender->VtxPos(x+w, y+h, z);
		m_pDynRender->VtxTexCoord(u, v2);
		m_pDynRender->VtxPos(x, y+h, z);

		m_pDynRender->End();
	}

	m_Direct3DDevice.SetTexture(0, NULL);
}

// draws overlays and external UI
void CBaseStereoRenderer::DrawInfo(CBaseSwapChain* pSwapChain)
{
	INSIDE;
	if (!m_pFont && (gInfo.ShowFPS || gInfo.ShowOSD || GET_DEBUG_VALUE(gInfo.ShowDebugInfo) || 
		m_pOutputMethod->GetLogo() || m_bShowMouse || m_bShowGDIMouse))
		CreateFont();
	
	if(!m_pFont)
		return;

	pSwapChain->PrepareFPSMessage();

	std::wostringstream szCameraInfo;
	if(gInfo.ShowOSD && m_nOSDShowTimeLeft.QuadPart > 0)
	{
		szCameraInfo << g_LocalData.GetText(LT_Preset) << L" " << m_Input.ActivePreset + 1 << L"\n";
		//--- show ZPS in LEFT handed coordinate system ---
		if(m_Input.GetActivePreset()->One_div_ZPS == 0.f)
			szCameraInfo << g_LocalData.GetText(LT_ConvergenceINF) << L"\n";
		else
			szCameraInfo << g_LocalData.GetText(LT_Convergence) << L" = " << std::fixed << std::setprecision(4) << -m_Input.GetActivePreset()->One_div_ZPS << L"\n";

		//--- show Separation as % ---
		bool separationValid = true;
		if(m_Input.GetActivePreset()->AutoFocusEnable)
		{
			if(!pSwapChain->m_ShiftFinder.IsCurrentShiftValid() || 
				(pSwapChain->m_ShiftFinder.IsShiftToHoldValid() && pSwapChain->m_nPresentCounter == pSwapChain->m_ShiftFinder.GetShiftInValidationFrame()))
				separationValid = false;
		}

		szCameraInfo << g_LocalData.GetText(LT_Separation) << L" = " << std::fixed << std::setprecision(2) << 
			SEPARATION_TO_PERCENT(m_Input.GetActivePreset()->StereoBase) << L"\n";
		szCameraInfo << g_LocalData.GetText(LT_AutoFocus) << L" " <<
			(m_Input.GetActivePreset()->AutoFocusEnable ? g_LocalData.GetText(LT_ON) : g_LocalData.GetText(LT_OFF));
		if (!separationValid)
			szCameraInfo << L" " << g_LocalData.GetText(LT_tooMuchSeparation);
		szCameraInfo << L"\n";

		if (m_Input.SwapEyes)
			szCameraInfo << g_LocalData.GetText(LT_SwapLR) << L"\n";

		//--- show trial mode  ---
		int DaysLeft = m_pOutputMethod->GetTrialDaysLeft();
		if (DaysLeft != -1)
		{
			szCameraInfo << g_LocalData.GetText(LT_DaysLeft) << L" " << DaysLeft << L"\n";
		}
	}

#ifndef FINAL_RELEASE
	CHAR szDebugInfo[2048];
	if(gInfo.ShowDebugInfo)
	{
		switch(m_Input.DebugInfoPage)
		{
		case 1:
			{
				DWORD TotalMono = m_nRenderTargetsMonoSize + m_nRenderTargetTexturesMonoSize + 
					m_nDepthStencilSurfacesMonoSize + m_nDepthStencilTexturesMonoSize + 
					m_nTexturesSize + m_nTexturesDefaultSize + m_nCubeTexturesSize + m_nCubeTexturesDefaultSize + 
					m_nRenderTargetCubeTexturesMonoSize + m_nDepthStencilCubeTexturesMonoSize;
				DWORD TotalStereo = m_nRenderTargetsStereoSize + m_nRenderTargetTexturesStereoSize + 
					m_nDepthStencilSurfacesStereoSize + m_nDepthStencilTexturesStereoSize + 
					m_nRenderTargetCubeTexturesStereoSize + m_nDepthStencilCubeTexturesStereoSize;
				_snprintf_s(szDebugInfo, _TRUNCATE, "Stereo mode: %s\nOriginal mode: %s\nDriver mode: %s\n"
					"DrawCount = %d (mono %d, VS %d, fixed %d, RHW %d, Heavy %d)\nScreensLagTime = %6.2fms\n"
					"TexSynchCount = %d\nTexSynchRTCount = %d\nTexSynchCachedCount = %d\n"
					"VS Count = %d (modified %d)\nPS Count = %d (modified %d)\n"
					"ShaderAnalysingTime = %.3fs\nCRCCalculatingTime = %.3fs\n"
					"RT Surfaces = %d (%.2f Mb + %.2f Mb)\nRT Textures = %d (%.2f Mb + %.2f Mb)\n"
					"RT R32F Textures = %d\nRT Square Textures = %d\nRT Less Than BB Textures = %d\n"
					"DS Surfaces = %d (%.2f Mb + %.2f Mb)\nDS Textures = %d (%.2f Mb + %.2f Mb)\n"
					"Textures = %d (%.2f Mb)\nTextures Default = %d (%.2f Mb)\n"
					"Cube Textures = %d (%.2f Mb)\nCube Textures Default = %d (%.2f Mb)\n"
					"Cube RT Textures = %d (%.2f Mb + %.2f Mb)\nCube DS Textures = %d (%.2f Mb + %.2f Mb)\n"
					"Volume Textures = %d (%.2f Mb)\nVolume Textures Default = %d (%.2f Mb)\n"
					"Total Memory For Textures = %.2f Mb + %.2f Mb (+%.2f%%)",  
					(gInfo.WideRenderTarget ? "Wide" : "Non-Wide"), 
					(GetMonoSC()->m_OriginalPresentationParameters.Windowed ? "Windowed" : "Fullscreen"), 
					(GetBaseSC()->m_PresentationParameters[0].Windowed ? "Windowed" :  (GetBaseSC()->m_bScalingEnabled ? "Fullscreen scaled" : "Fullscreen")),
					m_nDrawCounter, m_nDrawMonoCounter, m_nDrawVSCounter, m_nDrawFixedCounter, m_nDrawRHWCounter, m_nDrawHeavyCounter,
					1000.0 * pSwapChain->m_nScreensLagTime.QuadPart / m_nFreq.QuadPart, 
					m_nTexSynchCounter, m_nTexSynchRTCounter, m_nTexSynchCachedCounter, 
					DX9VertexShaderConverter::m_GlobalCounter, DX9VertexShaderConverter::m_ModifiedCounter,
					DX9PixelShaderConverter::m_GlobalCounter, DX9PixelShaderConverter::m_ModifiedCounter,
					1.0 * m_nShaderAnalysingTime.QuadPart / m_nFreq.QuadPart, 1.0 * m_nCRCCalculatingTime.QuadPart / m_nFreq.QuadPart, 
					m_nRenderTargetCounter, m_nRenderTargetsMonoSize / 1024.0f / 1024, m_nRenderTargetsStereoSize / 1024.0f / 1024, 
					m_nRenderTargetTextureCounter, m_nRenderTargetTexturesMonoSize / 1024.0f / 1024, m_nRenderTargetTexturesStereoSize / 1024.0f / 1024, 
					m_nRenderTargetR32FTextureCounter, m_nRenderTargetSquareTextureCounter, m_nRenderTargetLessThanBBTextureCounter,
					m_nDepthStencilSurfaceCounter, m_nDepthStencilSurfacesMonoSize / 1024.0f / 1024, m_nDepthStencilSurfacesStereoSize / 1024.0f / 1024, 
					m_nDepthStencilTextureCounter, m_nDepthStencilTexturesMonoSize / 1024.0f / 1024, m_nDepthStencilTexturesStereoSize / 1024.0f / 1024,
					m_nTextureCounter, m_nTexturesSize / 1024.0f / 1024, 
					m_nTextureDefaultCounter, m_nTexturesDefaultSize / 1024.0f / 1024, 
					m_nCubeTextureCounter, m_nCubeTexturesSize / 1024.0f / 1024, 
					m_nCubeTextureDefaultCounter, m_nCubeTexturesDefaultSize / 1024.0f / 1024, 
					m_nRenderTargetCubeTextureCounter, m_nRenderTargetCubeTexturesMonoSize / 1024.0f / 1024, m_nRenderTargetCubeTexturesStereoSize / 1024.0f / 1024, 
					m_nDepthStencilCubeTextureCounter, m_nDepthStencilCubeTexturesMonoSize / 1024.0f / 1024, m_nDepthStencilCubeTexturesStereoSize / 1024.0f / 1024,
					m_nVolumeTextureCounter, m_nVolumeTexturesSize / 1024.0f / 1024, 
					m_nVolumeTextureDefaultCounter, m_nVolumeTexturesDefaultSize / 1024.0f / 1024, 
					TotalMono / 1024.0f / 1024, TotalStereo / 1024.0f / 1024, (TotalMono == 0.0f ? 0.0f : 100.0f * TotalStereo / TotalMono));
				if (m_bDebugWarningMonoDepthStencil)
					strcat_s(szDebugInfo, sizeof(szDebugInfo), "\nWarning: used mono DS with stereo RT");
				if (m_bDebugWarningCantCreateWideSurface)
					strcat_s(szDebugInfo, sizeof(szDebugInfo), "\nWarning: can't create wide surface");
				if (m_bOutOfVideoMemory)
					strcat_s(szDebugInfo, sizeof(szDebugInfo), "\nWarning: out of video memory");
				if (m_bUseMultipleSwapChains)
					strcat_s(szDebugInfo, sizeof(szDebugInfo), "\nWarning: use multiple SwapChain's");
				if (m_bDebugWarningEmulateFullscreenMode)
					strcat_s(szDebugInfo, sizeof(szDebugInfo), "\nWarning: emulate fullscreen mode");
			}
			break;
		case 2:
			_snprintf_s(szDebugInfo, _TRUNCATE, "Skip UpdateProjectionMatrix = %d\nSkip modify RHW = %d\n"
				"Skip fixed projection matrix analyzing= %d\nRender in mono = %d\n"
				"Skip SetStereoTextures = %d\nSkip check CRC = %d\n"
				"Skip Skip shader matrix analyzing = %d\nSkip VD analyzing = %d\n"
				"Skip SetStereoRenderTarget = %d\nSkip SynchronizeTextures = %d\n"
				"Skip SynchronizeDepthTextures = %d\nRender in one RT = %d\n"
				"Skip texture using check = %d\nUse simple presenter = %d\n"
				"Use heavy draw pass splitting = %d\n",
				m_bDebugSkipUpdateProjectionMatrix, m_bDebugSkipModifyRHW, 
				m_bDebugSkipSetProjection, m_bDebugRenderInMono, 
				m_bDebugSkipSetStereoTextures, m_bDebugSkipCheckCRC, 
				m_bDebugSkipShaderMatrixAnalyzing, m_bDebugSkipVDAnalyzing,
				m_bDebugSkipSetStereoRenderTarget, m_bDebugSkipSynchronizeTextures, 
				m_bDebugSkipSynchronizeDepthTextures, m_bDebugRenderInOneRT,
				m_bDebugSkipTextureUsingCheck,	m_bDebugUseSimplePresenter,
				m_bDebugUseDrawPassSplitting);
			break;
		case 3:
			{
				static int CurrentShift = 0;
				static int ShiftToHold  = 0;
				static UINT CurrentCorrelation = 0;
				//--- because of thread can work right now ! ---
				if(pSwapChain->m_ShiftFinder.IsCalculationComplete())
				{
					CurrentShift = pSwapChain->m_ShiftFinder.GetCurrentShift();
					ShiftToHold  = pSwapChain->m_ShiftFinder.GetShiftToHold();
					CurrentCorrelation = pSwapChain->m_ShiftFinder.GetCurrentCorrelation();
				}
				_snprintf_s(szDebugInfo, _TRUNCATE, 
					"TexSynch frame time = %.4f%%\nTexSynchRT frame time = %.4f%%\n"
					"Current shift = %d\nShift to hold = %d\nCurrentCorrelation = %u\n"
					"DeviceMultithreaded = %s\n"
					"OutputSubMode = %i %S\nIsCurrentGammaIdentity = %s\n"
					"Custom multiplier = %.4f\nCustom convergence shift = %.4f\n"
					"Custom ZNear = %.4f\nWeapon Draw Calls = %d\n"
					"Min Depth = %.4f\nMax Depth = %.4f\n"
					"MiniHistogram = %.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f\n",
					100.0f * m_nTexSynchTime.QuadPart / pSwapChain->m_nFrameTimeDelta.QuadPart, 
					100.0f * m_nTexSynchRTTime.QuadPart / pSwapChain->m_nFrameTimeDelta.QuadPart,
					CurrentShift, ShiftToHold, CurrentCorrelation,
					pSwapChain->m_ShiftFinder.IsDeviceMultithreaded() ? "true" : "false", 
					m_pOutputMethod->GetSubMode(), m_pOutputMethod->GetSubModeName(),
					pSwapChain->m_CurrentRAMPisIdentity ? "true" : "false",
					m_Input.Multiplier, m_Input.ConvergenceShift, m_Input.ZNear, m_nWeaponDrawCounter,
					pSwapChain->m_fMinDepthValue, pSwapChain->m_fMaxDepthValue, 
					pSwapChain->m_MiniHistogram[0], pSwapChain->m_MiniHistogram[1],
					pSwapChain->m_MiniHistogram[2], pSwapChain->m_MiniHistogram[3],
					pSwapChain->m_MiniHistogram[4], pSwapChain->m_MiniHistogram[5],
					pSwapChain->m_MiniHistogram[6], pSwapChain->m_MiniHistogram[7] );
				break;
			}
		case 4:
			{
				std::vector<RecordData>  timeData;

				for(CallProfiler::iterator it = g_Profiler.begin(); it != g_Profiler.end(); ++it)
				{
					RecordData rd(it->first.c_str(), it->second);
					timeData.push_back(rd);
				}

				sort(timeData.begin(), timeData.end(), SortbyMaxRecordData());

				LARGE_INTEGER freq;
				QueryPerformanceFrequency(&freq);
				int i = 0;
				strcpy(szDebugInfo, "UM Presenter CS lock time:\n");
				char line[MAX_PATH];
				for(std::vector<RecordData>::iterator it = timeData.begin(); it != timeData.end(); ++it)
				{
					float totalTime = (1000.f * it->second.totalTime / freq.QuadPart);
					float averTime = totalTime / it->second.callCount; 
					float maxTime = (1000.f * it->second.maxTime / freq.QuadPart);
					_snprintf_s(line, _TRUNCATE, "%S: aver. time = %3.6f,\n        total time = %8.6f\n        max time = %8.6f\n",
						it->first, averTime, totalTime, maxTime);
					strcat_s(szDebugInfo, line);
					i++;
					if (i == 10)
						break;
				}

				break;
			}
		default:
			strcpy_s(szDebugInfo, "");
			break;
		}
	}
#endif

	HRESULT hResult = S_OK;

	NSCALL(m_pTextState->Apply());
	m_pFont->PreparePixelPerfectOutput(m_ViewPort);

	DWORD rt[4] = { true, false, false, false };
	bool bStereoOn = RenderToRight() && RenderInStereo();
	if (bStereoOn)
	{
		m_pUpdateRTs = rt;
		m_UpdateDS = false;
	}
	for (ULONG nViewIndex = (bStereoOn ? VIEWINDEX_RIGHT : VIEWINDEX_LEFT); 
		nViewIndex >= VIEWINDEX_LEFT; nViewIndex--)
	{
		if (bStereoOn)
		{
			if(nViewIndex == VIEWINDEX_RIGHT)
				SetStereoRenderTarget<VIEWINDEX_RIGHT>();
			else
				SetStereoRenderTarget<VIEWINDEX_LEFT>();
		}
		float x = 10;
		if (m_Input.StereoActive && gInfo.FrustumAdjustMode)
		{
			switch (gInfo.SeparationMode)
			{
			case 0:
				x += gInfo.BlackAreaWidth * pSwapChain->m_BackBufferSize.cx;
				break;
			case 1:
				x += 2 * gInfo.BlackAreaWidth * pSwapChain->m_BackBufferSize.cx;
				break;
			default:
				break;
			}
		}
		float y = 10;
		if (gInfo.FixNone16x9TextPosition)
			y += std::max(0.0f, (pSwapChain->m_BackBufferSize.cy - (pSwapChain->m_BackBufferSize.cx / (16.0f / 9))) / 2);

		if (gInfo.ShowFPS)
		{
			std::wstring s = pSwapChain->m_szFPS.str();
			y += m_pFont->DrawML(x, y, 1.0f, s.c_str(), 0, 0) * m_pFont->GetHeight();
		};

		if (m_bCantFindStereoDevice)
		{
			m_pFont->Draw(x, y, 1.0f, g_LocalData.GetText(LT_CantFindStereoDevice).c_str(), 0, 0);
			y += m_pFont->GetHeight();
		} 
		else if (m_bUsingMonoOutput)
		{
			m_pFont->Draw(x, y, 1.0f, g_LocalData.GetText(LT_CantLoadOutputDLL).c_str(), 0, 0);
			y += m_pFont->GetHeight();
		};
		
		if(m_bUpdateAQBSDriver)
		{
			m_pFont->Draw(x, y, 1.0f, g_LocalData.GetText(LT_UpdateAMDDriver).c_str(), 0, 0);
			y += m_pFont->GetHeight();
		}

		if(m_bUnsupported3DDevice)
		{
			m_pFont->Draw(x, y, 1.0f, g_LocalData.GetText(LT_NotSupported3DDevice).c_str(), 0, 0);
			y += m_pFont->GetHeight();
		}


		if(gInfo.ShowOSD && m_nOSDShowTimeLeft.QuadPart > 0)
		{
			std::wstring s = szCameraInfo.str();
			y += m_pFont->DrawML(x, y, 1.0f, s.c_str(), 0, 0) * m_pFont->GetHeight();
		};

#ifndef FINAL_RELEASE
		if(gInfo.ShowDebugInfo && m_Input.DebugInfoPage)
		{
			m_pFont->DrawML(x, y, 1.0f, szDebugInfo, 0, 0);
		}
#endif
	}

	NSCALL(m_pTextState->Apply());
	m_pFont->PreparePixelPerfectOutput(m_ViewPort);

	for (ULONG nViewIndex = (bStereoOn ? VIEWINDEX_RIGHT : VIEWINDEX_LEFT); 
		nViewIndex >= VIEWINDEX_LEFT; nViewIndex--)
	{
		if (bStereoOn)
		{
			if(nViewIndex == VIEWINDEX_RIGHT)
				SetStereoRenderTarget<VIEWINDEX_RIGHT>();
			else
				SetStereoRenderTarget<VIEWINDEX_LEFT>();
		}
#ifndef FINAL_RELEASE
		if(gInfo.ShowDebugInfo && m_Input.DebugInfoPage)
		{
			if (m_Input.DebugInfoPage == 1 )
				DrawGraph(pSwapChain);
#ifndef WIN64
			else
				DrawPerfHUDStatistics( pSwapChain );
#endif WIN64
		}
#endif
	}
}

// IUnknown
#pragma optimize( "y", off ) // for Steam Community
ULONG CBaseStereoRenderer::Release()
{
	//DEBUG_TRACE3(_T("CBaseStereoRenderer::Release(), %d\n"), m_refCount);
	ULONG ret = InterlockedDecrement(& m_refCount);
	if( ret == 0 )
	{
		if(m_bEngineStarted)
			StopEngine();
		delete this;
	}
	return ret;
}
#pragma optimize( "y", on )

HRESULT CBaseStereoRenderer::SendStereoCommand( ATIDX9STEREOCOMMAND stereoCommand, BYTE* pOutBuffer, DWORD dwOutBufferSize, BYTE* pInBuffer , DWORD dwInBufferSize )
{
	if (!m_pATICommSurface)
		return E_FAIL;

	HRESULT hr;
	ATIDX9STEREOCOMMPACKET *pCommPacket;
	D3DLOCKED_RECT lockedRect;

	m_pATICommSurface->LockRect( &lockedRect, 0, 0 );

	pCommPacket = (ATIDX9STEREOCOMMPACKET*)(lockedRect.pBits);

	pCommPacket->dwSignature = 'STER';
	pCommPacket->pResult = &hr;
	pCommPacket->stereoCommand = stereoCommand;

	pCommPacket->pOutBuffer = pOutBuffer;
	pCommPacket->dwOutBufferSize = dwOutBufferSize;

	pCommPacket->pInBuffer = pInBuffer;
	pCommPacket->dwInBufferSize = dwInBufferSize;
	m_pATICommSurface->UnlockRect();
	return hr;
}

//-------------------------------------------------------------------------------------------------------
// HelixMod-compatible stereo parameters texture
//-------------------------------------------------------------------------------------------------------
//
// HelixMod fixes (and 3DMigoto/Geo-11) embed instructions like
//
//     def c247, 0, 0, 0.0625, 0
//     dcl_2d s0
//     ...
//     texldl r24, c247.z, s0   ; sample (separation, convergence, ?, ?)
//     add r24.y, r1.w, -r24.y   ; W - convergence
//     mul r24.x, r24.x, r24.y   ; separation * (W - convergence)
//     add r0.x, r0.x, r24.x    ; X += offset
//
// They expect a small 2D texture bound to VS sampler 0 (= D3DVERTEXTEXTURESAMPLER0
// at runtime) and PS sampler 13. The texel at U=0, V=1/16 holds
// (separation, convergence, ...) for the eye currently being rendered.
//
// Historically NVIDIA's stereo driver populated this texture; on AMD/Intel/etc
// it never existed, which is why HelixMod fixes only worked on NVIDIA.
// wiz3D mimics the texture in user code so HelixMod fixes function on any
// vendor's hardware - we never call NVAPI for this and it does not depend
// on any vendor-specific runtime.
//
// Format: 1x16 A32B32G32R32F. We only ever write to row 1 (matching c247.z = 1/16);
// other rows stay zeroed and are never sampled by HelixMod fixes.

void CBaseStereoRenderer::EnsureStereoParamsTexture()
{
	if (m_pStereoParamsTexture)
		return;

	// D3DPOOL_DEFAULT is required because vertex textures (sampled from VS via
	// D3DVERTEXTEXTURESAMPLER0..3) cannot live in MANAGED. D3DUSAGE_DYNAMIC
	// lets us LockRect this DEFAULT-pool texture to refresh per-eye without
	// the slow stall path (and without it, LockRect on a non-dynamic
	// DEFAULT-pool texture fails outright).
	HRESULT hr = m_Direct3DDevice.CreateTexture(
		1, 16, 1, D3DUSAGE_DYNAMIC, D3DFMT_A32B32G32R32F, D3DPOOL_DEFAULT,
		&m_pStereoParamsTexture, NULL);
	if (FAILED(hr))
	{
		// Some drivers don't support A32B32G32R32F as a vertex texture format.
		// Caller will silently no-op; HelixMod fixes won't activate but the
		// game won't break either.
		DEBUG_MESSAGE(_T("[StereoParamsTex] CreateTexture failed: 0x%08x\n"), hr);
		m_pStereoParamsTexture = nullptr;
	}
}

void CBaseStereoRenderer::UpdateStereoParamsTexture(VIEWINDEX view)
{
	EnsureStereoParamsTexture();
	if (!m_pStereoParamsTexture)
		return;

	// Pull current stereo settings from the active preset. Magnitudes match
	// what wiz3D's existing matrix-offset path uses for its own per-eye math
	// (BaseStereoRenderer-inl.h:181-184), so HelixMod-fixed shaders and
	// wiz3D-analyzed shaders observe the same separation/convergence values.
	const float stereoBase = m_Input.GetActivePreset()->StereoBase * m_CurrentMeshMultiplier;
	const float oneDivZps  = m_Input.GetActivePreset()->One_div_ZPS + m_CurrentConvergenceShift;
	const float convergence = (oneDivZps != 0.0f) ? (1.0f / oneDivZps) : 0.0f;
	const float eyeSign = (view == VIEWINDEX_LEFT) ? -1.0f : +1.0f;
	const float separation = stereoBase * eyeSign;

	D3DLOCKED_RECT lr = {};
	HRESULT hr = m_pStereoParamsTexture->LockRect(0, &lr, NULL, D3DLOCK_DISCARD);
	if (FAILED(hr))
	{
		DEBUG_MESSAGE(_T("[StereoParamsTex] LockRect failed: 0x%08x\n"), hr);
		return;
	}

	BYTE* base = static_cast<BYTE*>(lr.pBits);
	memset(base, 0, lr.Pitch * 16); // zero rows we don't use

	// Row 1 (Y=1/16 = 0.0625) holds the eye's params, matching the canonical
	// HelixMod texldl coordinate.
	float* row1 = reinterpret_cast<float*>(base + lr.Pitch * 1);
	row1[0] = separation;     // .r -> r24.x in fixed shader
	row1[1] = convergence;    // .g -> r24.y in fixed shader
	row1[2] = eyeSign;        // .b -> some fixes inspect this
	row1[3] = 1.0f;           // .a -> "stereo active" flag

	m_pStereoParamsTexture->UnlockRect(0);
}

void CBaseStereoRenderer::BindStereoParamsSamplers()
{
	if (!m_pStereoParamsTexture)
		return;

	// VS sampler 0 in shader source code (`dcl_2d s0`) maps at runtime to the
	// D3DVERTEXTEXTURESAMPLER0 texture stage. PS sampler 13 is just stage 13.
	// Force point sampling so the texel is read exactly without filter blending.
	m_Direct3DDevice.SetTexture(D3DVERTEXTEXTURESAMPLER0, m_pStereoParamsTexture);
	m_Direct3DDevice.SetSamplerState(D3DVERTEXTEXTURESAMPLER0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
	m_Direct3DDevice.SetSamplerState(D3DVERTEXTEXTURESAMPLER0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
	m_Direct3DDevice.SetSamplerState(D3DVERTEXTEXTURESAMPLER0, D3DSAMP_MIPFILTER, D3DTEXF_POINT);

	m_Direct3DDevice.SetTexture(13, m_pStereoParamsTexture);
	m_Direct3DDevice.SetSamplerState(13, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
	m_Direct3DDevice.SetSamplerState(13, D3DSAMP_MINFILTER, D3DTEXF_POINT);
	m_Direct3DDevice.SetSamplerState(13, D3DSAMP_MIPFILTER, D3DTEXF_POINT);
}

void CBaseStereoRenderer::ReleaseStereoParamsTexture()
{
	// Unbind from sampler stages first so the device drops its reference,
	// otherwise the texture's refcount can survive past Reset.
	if (m_Direct3DDevice)
	{
		m_Direct3DDevice.SetTexture(D3DVERTEXTEXTURESAMPLER0, NULL);
		m_Direct3DDevice.SetTexture(13, NULL);
	}
	m_pStereoParamsTexture = nullptr;
}

//-------------------------------------------------------------------------------------------------------
/*
static bool bTimeStopped = false;
static LARGE_INTEGER liSavedTime; 
static DWORD tgtSavedTime; 
static DWORD gtcSavedTime; 
static ULONGLONG gtc64SavedTime; 

BOOL      (WINAPI *Original_QueryPerformanceCounter)( __out LARGE_INTEGER *lpPerformanceCount );
DWORD     (WINAPI *Original_timeGetTime)(void);
DWORD     (WINAPI *Original_GetTickCount)( VOID );
ULONGLONG (WINAPI *Original_GetTickCount64)( VOID );

BOOL WINAPI Hooked_QueryPerformanceCounter( __out LARGE_INTEGER *lpPerformanceCount )
{
	if(bTimeStopped)
	{
		*lpPerformanceCount = liSavedTime;
		return true;
	}
	return Original_QueryPerformanceCounter(lpPerformanceCount);
}

DWORD WINAPI Hooked_timeGetTime( void )
{
	if(bTimeStopped)
	{
		return tgtSavedTime;
	}
	return Original_timeGetTime();
}

DWORD WINAPI Hooked_GetTickCount( VOID )
{
	if(bTimeStopped)
	{
		return gtcSavedTime;
	}
	return Original_GetTickCount();
}

ULONGLONG WINAPI Hooked_GetTickCount64( VOID )
{
	if(bTimeStopped)
	{
		return gtc64SavedTime;
	}
	return Original_GetTickCount64();
}

void CheckTime()
{
	if(IsKeyPressed(VK_OEM_3))  // '~'
	{
		if(bTimeStopped == false)
		{
			QueryPerformanceCounter(&liSavedTime);
			//((__int64&)liSavedTime)++;
			tgtSavedTime = timeGetTime();// + 1/;
			gtcSavedTime = GetTickCount();// + 1; 
			gtc64SavedTime = GetTickCount64();
			//((__int64&)gtc64SavedTime)++;

			HookCode(QueryPerformanceCounter, Hooked_QueryPerformanceCounter, (PVOID*)&Original_QueryPerformanceCounter, HOOKING_FLAG);
			HookCode(timeGetTime, Hooked_timeGetTime, (PVOID*)&Original_timeGetTime, HOOKING_FLAG);
			HookCode(GetTickCount, Hooked_GetTickCount, (PVOID*)&Original_GetTickCount, HOOKING_FLAG);
			HookCode(GetTickCount64, Hooked_GetTickCount64, (PVOID*)&Original_GetTickCount64, HOOKING_FLAG);
			bTimeStopped = true;
			Beep(3000, 100);
			Sleep(100);
		}
		else
		{
			UnhookCode((PVOID*)&Original_QueryPerformanceCounter);
			UnhookCode((PVOID*)&Original_timeGetTime);
			UnhookCode((PVOID*)&Original_GetTickCount);
			UnhookCode((PVOID*)&Original_GetTickCount64);
			bTimeStopped = false;
			Beep(1000, 100);
		}
	}
}
*/