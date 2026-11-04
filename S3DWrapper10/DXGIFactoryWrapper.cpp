#include "StdAfx.h"
#include "DXGISwapChainWrapper.h"
#include "D3DDeviceWrapper.h"
#include <madCHook.h>
#include <d3d9types.h>  // D3DCOLORVALUE for the DXGI_RGBA shim
#ifndef _DXGI_RGBA_DEFINED
#define _DXGI_RGBA_DEFINED
typedef D3DCOLORVALUE DXGI_RGBA;  // bundled lib/d3d10 dxgitype.h is missing this
#endif
#include <dxgi1_2.h>   // IDXGIFactory2 / IDXGISwapChain1 for Factory2 hooks
#include "../OutputMethods/OutputLib/OutputData.h"
#include "../S3DAPI/ScalingAgent.h"
#include "../S3DAPI/ShutterAPI.h"
#include "DXGIFactoryWrapper.h"
#include "presenter.h"
#include "proxy_factory.h"      // Option B Stage 4b.8 factory hook

void UnhookCreateD3DDevice();

// ---------------------------------------------------------------------------
// Diagnostic: append to wiz3D_proxy.log next to the game exe so the wrapper
// shares one log with the proxy DLLs. The existing DEBUG_TRACE/DEBUG_MESSAGE
// plumbing goes through ZLOg and isn't visible in wiz3D_proxy.log.
// Keep this minimal — Win32 FILE* I/O only, no CRT singletons that could
// interact with loader lock.
// ---------------------------------------------------------------------------
static void WrapperLog(const char* fmt, ...)
{
    static FILE* fp = NULL;
    if (!fp)
    {
        WCHAR dir[MAX_PATH];
        GetModuleFileNameW(NULL, dir, MAX_PATH);
        WCHAR* pSlash = wcsrchr(dir, L'\\');
        if (pSlash) *(pSlash + 1) = L'\0';
        lstrcatW(dir, L"wiz3D_Logs");
        CreateDirectoryW(dir, NULL);
        lstrcatW(dir, L"\\wiz3D_proxy.log");
        fp = _wfopen(dir, L"a");  // append: shared with proxy DLLs
        if (fp) fputs("\n--- wrapper CreateSwapChain hook fired ---\n", fp);
    }
    if (!fp) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(fp, fmt, ap);
    va_end(ap);
    fflush(fp);
}

HRESULT (STDMETHODCALLTYPE *Original_CreateSwapChain)( IDXGIFactory* This, IUnknown *pDevice, DXGI_SWAP_CHAIN_DESC *pDesc, IDXGISwapChain **ppSwapChain );

void FindNativeFullScreenResolution()
{
	if(gInfo.DisplayNativeWidth == 0 || gInfo.DisplayNativeHeight == 0)
	{
		DEVMODE mode;
		EnumDisplaySettings(NULL, ENUM_REGISTRY_SETTINGS, &mode);	// FIXME! May return wrong data if the monitor was replaced with the different one!
		gInfo.DisplayNativeWidth = mode.dmPelsWidth;
		gInfo.DisplayNativeHeight = mode.dmPelsHeight;
		gInfo.DisplayNativeFrequency.Numerator = mode.dmDisplayFrequency;
		gInfo.DisplayNativeFrequency.Denominator = 1;
	}
}

bool InitAQBS( IDXGIDevice* pDXGIDevice )
{	
	bool bResult = false;

	if( !D3DDeviceWrapper::IsAmdStereoInited( pDXGIDevice ) )
	{
		D3DDeviceWrapper::CloseAMDStereoInterface();

		CComQIPtr<ID3D11Device>	pD3D11Device = pDXGIDevice;		

		if ( pD3D11Device )
			bResult =  D3DDeviceWrapper::OpenAMDStereoInterface( pDXGIDevice, pD3D11Device );
		else 
		{
			CComQIPtr<ID3D10Device>	pD3D10Device = pDXGIDevice;
			if (pD3D10Device)
				bResult = D3DDeviceWrapper::OpenAMDStereoInterface( pDXGIDevice, pD3D10Device );
		}
	}
	else
	{
		bResult = true;
	}

	return bResult;
}

STDMETHODIMP CreateSwapChain( IDXGIFactory* This, IUnknown *pDevice, DXGI_SWAP_CHAIN_DESC *pDesc, IDXGISwapChain **ppSwapChain )
{
	DEBUG_TRACE1(_T("CreateSwapChain(): ENTER\n"));
	WrapperLog("\nCreateSwapChain ENTER\n");
	WrapperLog("  pDesc->BufferDesc.Width=%u Height=%u\n", pDesc ? pDesc->BufferDesc.Width : 0, pDesc ? pDesc->BufferDesc.Height : 0);
	WrapperLog("  pDesc->Windowed=%d (game-requested)\n", pDesc ? pDesc->Windowed : -1);
	WrapperLog("  gInfo.DisableFullscreenModeEmulation=%d\n", (int)gInfo.DisableFullscreenModeEmulation);
	WrapperLog("  gInfo.DisplayScalingMode=%d, DisplayNativeW=%u H=%u\n",
		gInfo.DisplayScalingMode, gInfo.DisplayNativeWidth, gInfo.DisplayNativeHeight);
	WrapperLog("  IsInternalCall()=%d, gInfo.UseMonoDeviceWrapper=%d\n",
		(int)IsInternalCall(), (int)gInfo.UseMonoDeviceWrapper);
	_ASSERT( pDesc );
	
	HRESULT hResult;
	DXGI_SWAP_CHAIN_DESC *pOriginalSCDesc = pDesc;	// Keep pointer to the original (we'll provide that for GetDesc())

	if (IsInternalCall())
	{
		DEBUG_MESSAGE(_T("Creating swap chain(internal)"));
		g_SwapChainMode = scCreating;
		NSCALL_TRACE1(Original_CreateSwapChain(This, pDevice, pDesc, ppSwapChain), 
			DEBUG_MESSAGE(_T("IDXGIFactory::CreateSwapChain(pDevice = %p, pDesc = (%s), ppSwapChain = %p)"), 
			pDevice, GetDXGISwapChainDescString(pDesc), ppSwapChain));
		g_SwapChainMode = scNormal;
		return hResult;
	}

	CComQIPtr<IDXGIDevice>	pDXGIDevice		= pDevice;
	DXGI_SWAP_CHAIN_DESC	SCDesc			= *pDesc; // Better to keep local copy and don't mess application variable
	CComQIPtr<IDXGIDevice1>	pDXGIDevice1	= pDevice;
	
	DEBUG_MESSAGE(_T("Creating swap chain"));
	if( USE_IZ3DKMSERVICE_PRESENTER )
	{
		// Check for KM shutter service installed
		if( TRUE != g_KMShutter.InitShutter() )
		{
			// Force UM presenter
			gData.ShutterMode = SHUTTER_MODE_UMHOOK_D3D10;
		}
		else
		{
			// Reduce frames-ahead count
			if( pDXGIDevice1 )
				pDXGIDevice1->SetMaximumFrameLatency( 1 );
		}
	}

	if( USE_ATI_PRESENTER )
	{
		bool bResult = InitAQBS(pDXGIDevice);
		if( !bResult )
		{
			if( gInfo.ShutterMode == SHUTTER_MODE_AUTO ) // switch to KM service or UM presenter, if auto mode selected  
			{
				D3DDeviceWrapper::ResetAQBSStates();
				gData.ShutterMode = SHUTTER_MODE_UMHOOK_D3D10;
			}
			else
				gData.ShutterMode = SHUTTER_MODE_DISABLED;
		}
	}

	g_SwapChainMode = scCreating;
	BOOL Fullscreen = !SCDesc.Windowed;
	FindNativeFullScreenResolution();
	SIZE BackBufferSize = {gInfo.DisplayNativeWidth, gInfo.DisplayNativeHeight};
	SIZE BackBufferSizeBeforeScaling = {SCDesc.BufferDesc.Width, SCDesc.BufferDesc.Height};

	ScalingHookPtrT ScalingHook;
	// DX9 already gates this on gInfo.DisableFullscreenModeEmulation (BaseSwapChain.cpp:437);
	// DX10 was missing the gate, which forced every fullscreen swap chain to windowed and
	// produced sub-window rendering on Bioshock 2007 DX10 / Far Cry 2 DX10. SR weave / SBS /
	// Anaglyph don't need windowed emulation — only shutter outputs do.
	WrapperLog("  Fullscreen=%d (computed from !SCDesc.Windowed)\n", Fullscreen);
	if (Fullscreen && !gInfo.DisableFullscreenModeEmulation) // force only when emulation is enabled (e.g. shutter outputs)
	{
		WrapperLog("  -> ENTERING windowed-forcing block (will set SCDesc.Windowed=true)\n");
		SCDesc.Windowed = true;

		// create swap chain in native resolution to perform scaling, after resize it and setup hooks
		if (gInfo.DisplayScalingMode != 0)
		{
			// Need to lie application about actual resolution while swap chain is creating
			ScalingHook = ScalingAgent::Instance()->InitializeHook( TDisplayScalingMode(gInfo.DisplayScalingMode), 
																	BackBufferSizeBeforeScaling, 
																	BackBufferSize );

			SCDesc.BufferDesc.Width = gInfo.DisplayNativeWidth;
			SCDesc.BufferDesc.Height = gInfo.DisplayNativeHeight;
			SCDesc.BufferDesc.RefreshRate = gInfo.DisplayNativeFrequency;
		}
	}

	if (gInfo.DisplayScalingMode != 0)
	{
		// disable flags which may distort or break scaling
		SCDesc.BufferDesc.Scaling = DXGI_MODE_SCALING_UNSPECIFIED;
		//SCDesc.Flags &= !DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
		//ScalingHook::IgnoreModeSwitch(true);
	}

	if( IS_SHUTTER_OUTPUT )
	{
		DEBUG_TRACE1(_T("FindShutterClosestMatchingMode <source>: ( pDesc = (%s) )\n"), GetDXGISwapChainDescString(&SCDesc) );
		D3DDeviceWrapper::FindShutterClosestMatchingMode( pDXGIDevice, &SCDesc.BufferDesc );
		DEBUG_TRACE1(_T("FindShutterClosestMatchingMode <modified>: ( pDesc = (%s) )\n"), GetDXGISwapChainDescString(&SCDesc) );

		if( USE_IZ3DKMSERVICE_PRESENTER )
		{
			// Force quadbuffering for KM shutter service (TODO: discuss that)
			if( !pDesc->Windowed )
			{
				// FIXME!
				SCDesc.BufferCount	= 2; // KM_SHUTTER_SERVICE_BUFFER_COUNT;
				SCDesc.SwapEffect	= DXGI_SWAP_EFFECT_SEQUENTIAL;
				SCDesc.Windowed		= pDesc->Windowed;					// don't force windowed mode
			}
			// N.B. Multisampling will not work with DXGI_SWAP_EFFECT_SEQUENTIAL feature
			_ASSERT( SCDesc.SampleDesc.Count	== 1 );
			_ASSERT( SCDesc.SampleDesc.Quality	== 0 );
		}

		if( USE_UM_PRESENTER )
		{
			CPresenterX::Create( SCDesc.BufferDesc.RefreshRate );
			//SCDesc.OutputWindow = CPresenterX::Get()->GetPresenterWindow();
		}
	}

	WrapperLog("  Final SCDesc.Windowed=%d before Original_CreateSwapChain (Width=%u, Height=%u)\n",
		SCDesc.Windowed, SCDesc.BufferDesc.Width, SCDesc.BufferDesc.Height);
	const bool emulatedFullscreen = Fullscreen && SCDesc.Windowed;
	NSCALL_TRACE1(Original_CreateSwapChain(This, pDevice, &SCDesc, ppSwapChain),
		DEBUG_MESSAGE(_T("IDXGIFactory::CreateSwapChain(pDevice = %p, pDesc = (%s), ppSwapChain = %p)"),
		pDevice, GetDXGISwapChainDescString(&SCDesc), ppSwapChain));
	WrapperLog("  Original_CreateSwapChain returned 0x%08lX\n", hResult);

	if( Fullscreen )
		SCDesc.Windowed = false;

	if( g_pLastD3DDevice && SUCCEEDED(hResult) )
	{
		IDXGISwapChain *pRealSwapChain = *ppSwapChain;
		D3DSwapChain* p = NULL;
		g_pLastD3DDevice->InitializeDXGIDevice( pDXGIDevice );
		p = g_pLastD3DDevice->MapDXGISwapChain(This, pRealSwapChain);	
		p->m_BackBufferSizeBeforeScaling = BackBufferSize;

		if( USE_IZ3DKMSERVICE_PRESENTER )
		{
			p->m_bUseSimplePresenter	= false;			// turn off simple presentation if KM shutter is available
			p->m_bUseKMShutterService	= true;
		}

		CComPtr<IWrapper> pWrapper;
		NSCALL(DXGISwapChainWrapper::CreateInstance(&pWrapper));
		if(pWrapper)
		{
			NSCALL(pWrapper->Init((IUnknown*)pRealSwapChain));
			(*(IDXGISwapChain**)ppSwapChain)->Release();
			IDXGISwapChain *pSwapChain = NULL;
			pWrapper.QueryInterface(&pSwapChain);
			*ppSwapChain = pSwapChain;
			DXGISwapChainWrapper* pSwapChainWrapper = static_cast<DXGISwapChainWrapper*>(pWrapper.p);
			pSwapChainWrapper->setD3DSwapChain(pDXGIDevice, p);
			pSwapChainWrapper->setOriginalSwapChainDesc( pOriginalSCDesc ); 
			DeviceModes deviceMode = p ? p->m_pD3DDeviceWrapper->m_DeviceMode : (DeviceModes)gInfo.DeviceMode;
			if (deviceMode == DEVICE_MODE_FORCEFULLSCREEN)
				Fullscreen = true;
			else if (deviceMode == DEVICE_MODE_FORCEWINDOWED)
				Fullscreen = false;

			if (CPresenterX::Get())
			{
				SetCurrentSwapChain setSwapChain(pRealSwapChain);
				CPresenterX::Get()->Init(g_pLastD3DDevice);
			}
			if (Fullscreen) // FIXME! What about KM Shutter + Scaling enabled?
			{
				g_SwapChainMode = scNormal;
				if( !USE_IZ3DKMSERVICE_PRESENTER )		// SwapChain is fullscreen already for KM Shutter
				{
					if (deviceMode == DEVICE_MODE_FORCEFULLSCREEN)
						pSwapChainWrapper->ResizeBuffers(SCDesc.BufferCount, gInfo.DisplayNativeWidth, gInfo.DisplayNativeHeight, SCDesc.BufferDesc.Format, 0);

					HRESULT sfsHr = pSwapChain->SetFullscreenState( TRUE, NULL );
					WrapperLog("  SetFullscreenState(TRUE) returned 0x%08lX\n", sfsHr);

					// Borderless-fullscreen fallback. Only fires when SetFullscreenState
					// actually failed to put us in exclusive fullscreen.
					BOOL inFullscreen = FALSE;
					pSwapChain->GetFullscreenState(&inFullscreen, NULL);
					WrapperLog("  GetFullscreenState reports inFullscreen=%d\n", inFullscreen);
					HWND hwndOut = pOriginalSCDesc ? pOriginalSCDesc->OutputWindow : NULL;
					if (!inFullscreen && hwndOut)
					{
						HMONITOR hMon = MonitorFromWindow(hwndOut, MONITOR_DEFAULTTONEAREST);
						MONITORINFO mi = { sizeof(mi) };
						if (GetMonitorInfo(hMon, &mi))
						{
							const LONG monW = mi.rcMonitor.right  - mi.rcMonitor.left;
							const LONG monH = mi.rcMonitor.bottom - mi.rcMonitor.top;
							DEBUG_MESSAGE(_T("SetFullscreenState did not enter fullscreen — applying borderless-fullscreen fallback (HWND %dx%d, buffer→%dx%d)\n"), monW, monH, monW, monH);
							pSwapChain->SetFullscreenState(FALSE, NULL);
							LONG style = GetWindowLong(hwndOut, GWL_STYLE);
							SetWindowLong(hwndOut, GWL_STYLE,
								(style & ~(WS_OVERLAPPEDWINDOW | WS_CAPTION | WS_THICKFRAME | WS_DLGFRAME)) | WS_POPUP);
							SetWindowPos(hwndOut, HWND_TOP,
								mi.rcMonitor.left, mi.rcMonitor.top, monW, monH,
								SWP_FRAMECHANGED | SWP_SHOWWINDOW);
							pSwapChainWrapper->ResizeBuffers(SCDesc.BufferCount, monW, monH, SCDesc.BufferDesc.Format, 0);
						}
					}
				}
				// restore requested resolution
				if (gInfo.DisplayScalingMode != 0 && (BackBufferSizeBeforeScaling.cx != gInfo.DisplayNativeWidth || BackBufferSizeBeforeScaling.cy != gInfo.DisplayNativeHeight))
				{
					SCDesc.BufferDesc.Width  = BackBufferSizeBeforeScaling.cx;
					SCDesc.BufferDesc.Height = BackBufferSizeBeforeScaling.cy;
					pSwapChainWrapper->ResizeBuffers(SCDesc.BufferCount, SCDesc.BufferDesc.Width, SCDesc.BufferDesc.Height, SCDesc.BufferDesc.Format, 0); 
					pSwapChainWrapper->ResizeTarget(&SCDesc.BufferDesc);
				}
				if (CPresenterX::Get())
				{
					DXGI_SWAP_CHAIN_DESC	desc;
					pSwapChain->GetDesc(&desc);
					CPresenterX::Get()->ReInit( desc.BufferDesc.RefreshRate );
				}
			}
		}
		g_pLastD3DDevice = NULL;
	}
	ScalingHook.reset();
	g_SwapChainMode = scNormal;

	// Stage 4b.8 follow-up: Option B factory hook. The legacy g_pLastD3DDevice
	// path above only fires when DDI hooks created a D3DDeviceWrapper. With
	// UseCOMWrap=1 (default on Win11) the DDI hooks are skipped, so games
	// taking the two-call path (D3D11CreateDevice + factory->CreateSwapChain
	// — Dragon Age II, Max Payne 3, Batman Arkham Origins) get unwrapped
	// swap chains and skip the 4b.8 replay sweep + 4d composite. Detect our
	// Device11Proxy via the private IID and wrap the swap chain post-hoc.
	// Same gates as the D3D11CreateDeviceAndSwapChain export path: the
	// wiz3D_WrapSwapChain helper checks UseCOMWrapSwapChain internally.
	//
	// pDevice can be either our Device11Proxy or our DXGIDeviceProxy (the
	// runtime usually hands the latter to factory->CreateSwapChain). QI for
	// ID3D11Device first to normalise: DXGIDeviceProxy::QI routes that to
	// the parent Device11Proxy, and Device11Proxy::QI returns itself. Then
	// QI for the private IID to confirm it's actually ours (not a real
	// ID3D11Device, which would mean Option B isn't active for this device).
	if (SUCCEEDED(hResult) && ppSwapChain && *ppSwapChain && pDevice)
	{
		// DX11 path: route via ID3D11Device + private IID.
		ID3D11Device* asD3D11 = NULL;
		HRESULT qiHr = pDevice->QueryInterface(__uuidof(ID3D11Device),
		                                        reinterpret_cast<void**>(&asD3D11));
		if (SUCCEEDED(qiHr) && asD3D11)
		{
			IUnknown* probe = NULL;
			HRESULT pHr = asD3D11->QueryInterface(IID_wiz3D_Device11Proxy,
			                                       reinterpret_cast<void**>(&probe));
			if (SUCCEEDED(pHr) && probe)
			{
				probe->Release();
				WrapperLog("  Option B: pDevice routed to wiz3D Device11Proxy=%p — invoking wiz3D_WrapSwapChain\n", probe);
				void* scOut = static_cast<void*>(*ppSwapChain);
				wiz3D_WrapSwapChain(&scOut, probe);
				*ppSwapChain = static_cast<IDXGISwapChain*>(scOut);
				WrapperLog("  Option B: wrapped *ppSwapChain=%p\n", *ppSwapChain);
				if (emulatedFullscreen)
					wiz3D_EmulateFullscreen(*ppSwapChain);
			}
			asD3D11->Release();
		}
		// DX10 path: same idea but via ID3D10Device + IID_wiz3D_Device10Proxy.
		// Only fires when the DX11 probe didn't match (DX10 games never
		// implement ID3D11Device on their device, so the QI above would
		// have failed with E_NOINTERFACE before getting here).
		ID3D10Device* asD3D10 = NULL;
		HRESULT qi10 = pDevice->QueryInterface(__uuidof(ID3D10Device),
		                                        reinterpret_cast<void**>(&asD3D10));
		if (SUCCEEDED(qi10) && asD3D10)
		{
			IUnknown* probe10 = NULL;
			HRESULT pHr10 = asD3D10->QueryInterface(IID_wiz3D_Device10Proxy,
			                                         reinterpret_cast<void**>(&probe10));
			if (SUCCEEDED(pHr10) && probe10)
			{
				probe10->Release();
				WrapperLog("  Option B: pDevice routed to wiz3D Device10Proxy=%p — invoking wiz3D_WrapD3D10SwapChain\n", probe10);
				void* scOut = static_cast<void*>(*ppSwapChain);
				wiz3D_WrapD3D10SwapChain(&scOut, probe10);
				*ppSwapChain = static_cast<IDXGISwapChain*>(scOut);
				WrapperLog("  Option B: wrapped *ppSwapChain=%p (DX10)\n", *ppSwapChain);
				if (emulatedFullscreen)
					wiz3D_EmulateFullscreen10(*ppSwapChain);
			}
			asD3D10->Release();
		}
	}

	DEBUG_TRACE1(_T("CreateSwapChain(): EXIT\n"));
	return hResult;
}

static void HookFactory2SwapChainSlots(void* pFactory);   // slots 15, 16 and 24, defined beside their hooks below

// FC3 2026-10-18 log showed the game creating 249 DXGI factories during one
// level transition. Every factory shares the dxgi.dll vtable, so re-running
// HookCode on the same function address 249*4 times was pure waste and may
// have contributed to the Dunia 2 level-load hang (madCHook internal state
// swelling + per-call page-protect dances). Cache the function addresses
// we've already hooked and skip the rest.
#include <unordered_set>
static std::unordered_set<void*> s_hookedCreateSwapChainAddrs;
static std::unordered_set<void*> s_hookedCreateSCForHwndAddrs;
static std::unordered_set<void*> s_hookedCreateSCForCWAddrs;
static std::unordered_set<void*> s_hookedCreateSCForCompAddrs;
static CRITICAL_SECTION           s_hookedAddrsLock;
static volatile LONG              s_hookedAddrsInit = 0;
static void EnsureHookCacheInit()
{
	if (InterlockedCompareExchange(&s_hookedAddrsInit, 1, 0) == 0)
		InitializeCriticalSection(&s_hookedAddrsLock);
}

// One place to install the swap-chain hooks, so the CreateDXGIFactory entry
// points can't disagree about which factory IIDs deserve them.
static void HookFactorySwapChainSlot(void* pFactory)
{
	if (!pFactory) return;
	void** vt = *(void***)pFactory;
	EnsureHookCacheInit();
	bool install = false;
	{
		EnterCriticalSection(&s_hookedAddrsLock);
		install = s_hookedCreateSwapChainAddrs.insert(vt[10]).second;
		LeaveCriticalSection(&s_hookedAddrsLock);
	}
	if (install)
		HookCode(vt[10], CreateSwapChain, (PVOID*)&Original_CreateSwapChain, HOOKING_FLAG);
	HookFactory2SwapChainSlots(pFactory);
}

STDMETHODIMP Hooked_CreateDXGIFactory(REFIID riid, void **ppFactory, HMODULE hCallingModule)
{
	if (IsInternalCall() || gInfo.UseMonoDeviceWrapper)
		return CreateDXGIFactory(riid, ppFactory);

#ifndef FINAL_RELEASE
	if (hCallingModule)
	{
		TCHAR moduleName[MAX_PATH];
		GetModuleFileName(hCallingModule, moduleName, sizeof(moduleName));
		DEBUG_MESSAGE(_T("Calling Module: %s\n"), moduleName);
	}
#endif

	HRESULT hResult = CreateDXGIFactory(riid, ppFactory);
	// Slot 10 is CreateSwapChain on IDXGIFactory and every interface derived from it, so it is hooked whatever IID was
	// asked for: CreateDXGIFactory1 may be called with IID_IDXGIFactory.
	if (SUCCEEDED(hResult) && ppFactory && *ppFactory)
		HookFactorySwapChainSlot(*ppFactory);
	return hResult;
}

typedef HRESULT (STDMETHODCALLTYPE *PFNCREATEDXGIFACTORY)(REFIID riid, void **ppFactory);

STDMETHODIMP Hooked_CreateDXGIFactory1(REFIID riid, void **ppFactory, HMODULE hCallingModule)
{
	HMODULE hDXGI = GetModuleHandle(_T("dxgi.dll"));
	PFNCREATEDXGIFACTORY p = (PFNCREATEDXGIFACTORY)GetProcAddress(hDXGI, "CreateDXGIFactory1");

	if (IsInternalCall() || gInfo.UseMonoDeviceWrapper)
		return p(riid, ppFactory);

#ifndef FINAL_RELEASE
	if (hCallingModule)
	{
		TCHAR moduleName[MAX_PATH];
		GetModuleFileName(hCallingModule, moduleName, sizeof(moduleName));
		DEBUG_MESSAGE(_T("Calling Module: %s\n"), moduleName);
	}
#endif

	HRESULT hResult = p(riid, ppFactory);
	if (SUCCEEDED(hResult) && ppFactory && *ppFactory)
		HookFactorySwapChainSlot(*ppFactory);
	return hResult;
}

// ---------------------------------------------------------------------------
// Factory2 swap-chain hooks. CreateDXGIFactory2 returns an IDXGIFactory2 which
// exposes three NEW swap-chain entries on top of the legacy slot-10 path:
//
//   slot 15: CreateSwapChainForHwnd
//   slot 16: CreateSwapChainForCoreWindow
//   slot 24: CreateSwapChainForComposition
//
// DX10/11 games that use D3D11CreateDevice + CreateDXGIFactory2 land their
// gameplay swap chain through slot 15 (Hwnd variant) on Win8+, bypassing the
// legacy slot 10 hook entirely. Without these hooks the COM wrap can't see
// the swap chain at all and the game runs mono (FC2 / JC2 / De Blob / Lost
// Planet on the DX10 path were the original blockers).
// ---------------------------------------------------------------------------

HRESULT (STDMETHODCALLTYPE *Original_CreateSwapChainForHwnd)(
    IDXGIFactory2* This, IUnknown* pDevice, HWND hWnd,
    const DXGI_SWAP_CHAIN_DESC1* pDesc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullscreenDesc,
    IDXGIOutput* pRestrictToOutput,
    IDXGISwapChain1** ppSwapChain) = NULL;

HRESULT (STDMETHODCALLTYPE *Original_CreateSwapChainForCoreWindow)(
    IDXGIFactory2* This, IUnknown* pDevice, IUnknown* pWindow,
    const DXGI_SWAP_CHAIN_DESC1* pDesc,
    IDXGIOutput* pRestrictToOutput,
    IDXGISwapChain1** ppSwapChain) = NULL;

HRESULT (STDMETHODCALLTYPE *Original_CreateSwapChainForComposition)(
    IDXGIFactory2* This, IUnknown* pDevice,
    const DXGI_SWAP_CHAIN_DESC1* pDesc,
    IDXGIOutput* pRestrictToOutput,
    IDXGISwapChain1** ppSwapChain) = NULL;

namespace wiz3d { bool Factory2SwapChainsOn(); }   // FeatureFactory2SwapChains.cpp

// A game that called CreateDXGIFactory2 has these swap chains wrapped; one whose factories come from CreateDXGIFactory
// or CreateDXGIFactory1 only under its profile's <Factory2SwapChains> (Mass Effect LE), else they pass through.
static bool s_factory2Entry = false;
static bool WrapFactory2SwapChains() { return s_factory2Entry || wiz3d::Factory2SwapChainsOn(); }

// Shared device-probe + wrap path. Pulled out of the slot-10 CreateSwapChain
// body so the new Factory2 hooks don't redundantly duplicate ~40 lines each.
// On entry *ppSC is the real swap chain from the runtime. On exit it points
// to our proxy if pDevice resolves to a wiz3D Device10/11 proxy, otherwise
// unchanged.
static void TryWrapSwapChainOptionB(const char* siteTag, IUnknown* pDevice,
                                    IDXGISwapChain** ppSC)
{
    if (!ppSC || !*ppSC || !pDevice) return;

    // DX11 path
    ID3D11Device* asD3D11 = NULL;
    HRESULT qiHr = pDevice->QueryInterface(__uuidof(ID3D11Device),
                                           reinterpret_cast<void**>(&asD3D11));
    if (SUCCEEDED(qiHr) && asD3D11)
    {
        IUnknown* probe = NULL;
        HRESULT pHr = asD3D11->QueryInterface(IID_wiz3D_Device11Proxy,
                                              reinterpret_cast<void**>(&probe));
        if (SUCCEEDED(pHr) && probe)
        {
            probe->Release();
            WrapperLog("  Option B[%s]: routed via Device11Proxy=%p\n", siteTag, probe);
            void* scOut = static_cast<void*>(*ppSC);
            wiz3D_WrapSwapChain(&scOut, probe);
            *ppSC = static_cast<IDXGISwapChain*>(scOut);
            asD3D11->Release();
            return;
        }
        asD3D11->Release();
    }
    // DX10 path
    ID3D10Device* asD3D10 = NULL;
    HRESULT qi10 = pDevice->QueryInterface(__uuidof(ID3D10Device),
                                           reinterpret_cast<void**>(&asD3D10));
    if (SUCCEEDED(qi10) && asD3D10)
    {
        IUnknown* probe10 = NULL;
        HRESULT pHr10 = asD3D10->QueryInterface(IID_wiz3D_Device10Proxy,
                                                 reinterpret_cast<void**>(&probe10));
        if (SUCCEEDED(pHr10) && probe10)
        {
            probe10->Release();
            WrapperLog("  Option B[%s]: routed via Device10Proxy=%p\n", siteTag, probe10);
            void* scOut = static_cast<void*>(*ppSC);
            wiz3D_WrapD3D10SwapChain(&scOut, probe10);
            *ppSC = static_cast<IDXGISwapChain*>(scOut);
        }
        asD3D10->Release();
    }
}

STDMETHODIMP CreateSwapChainForHwnd(IDXGIFactory2* This, IUnknown* pDevice, HWND hWnd,
    const DXGI_SWAP_CHAIN_DESC1* pDesc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullscreenDesc,
    IDXGIOutput* pRestrictToOutput,
    IDXGISwapChain1** ppSwapChain)
{
    if (!WrapFactory2SwapChains())
        return Original_CreateSwapChainForHwnd(This, pDevice, hWnd, pDesc, pFullscreenDesc, pRestrictToOutput, ppSwapChain);
    // Forced-windowed parity with the slot-10 CreateSwapChain path: when
    // DisableFullscreenModeEmulation=0 (iZ3D legacy / shutter outputs need
    // windowed) and the game asked for fullscreen (non-NULL pFullscreenDesc
    // with Windowed=FALSE), pass NULL as pFullscreenDesc to force windowed.
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFSOverride = pFullscreenDesc;
    const bool wasFullscreen = pFullscreenDesc && !pFullscreenDesc->Windowed;
    const bool forceWindowed = wasFullscreen && !gInfo.DisableFullscreenModeEmulation;
    if (forceWindowed)
    {
        WrapperLog("  CreateSwapChainForHwnd: forcing windowed (DisableFullscreenModeEmulation=0)\n");
        pFSOverride = nullptr;
    }
    WrapperLog("\nCreateSwapChainForHwnd ENTER (Width=%u, Height=%u, Format=%d, FS=%d)\n",
        pDesc ? pDesc->Width : 0, pDesc ? pDesc->Height : 0,
        pDesc ? (int)pDesc->Format : -1, (int)wasFullscreen);
    HRESULT hr = Original_CreateSwapChainForHwnd(This, pDevice, hWnd, pDesc,
        pFSOverride, pRestrictToOutput, ppSwapChain);
    WrapperLog("  Original_CreateSwapChainForHwnd returned 0x%08lX\n", hr);
    if (SUCCEEDED(hr) && ppSwapChain && *ppSwapChain)
    {
        // IDXGISwapChain1 inherits from IDXGISwapChain — same pointer
        // value, just a typed view. Wrap via the IDXGISwapChain* alias and
        // re-tag back to IDXGISwapChain1* on return.
        IDXGISwapChain* sc = static_cast<IDXGISwapChain*>(*ppSwapChain);
        TryWrapSwapChainOptionB("F2.Hwnd", pDevice, &sc);
        if (forceWindowed)
        {
            // Both wrappers get a tag attempt; each helper's own QI gates
            // whether it fires (SwapChain11Proxy vs SwapChain10Proxy IID),
            // so at most one actually tags the chain. No-op otherwise.
            wiz3D_EmulateFullscreen(sc);
            wiz3D_EmulateFullscreen10(sc);
        }
        *ppSwapChain = static_cast<IDXGISwapChain1*>(sc);
    }
    return hr;
}

STDMETHODIMP CreateSwapChainForCoreWindow(IDXGIFactory2* This, IUnknown* pDevice,
    IUnknown* pWindow,
    const DXGI_SWAP_CHAIN_DESC1* pDesc,
    IDXGIOutput* pRestrictToOutput,
    IDXGISwapChain1** ppSwapChain)
{
    if (!WrapFactory2SwapChains())
        return Original_CreateSwapChainForCoreWindow(This, pDevice, pWindow, pDesc, pRestrictToOutput, ppSwapChain);
    WrapperLog("\nCreateSwapChainForCoreWindow ENTER\n");
    HRESULT hr = Original_CreateSwapChainForCoreWindow(This, pDevice, pWindow,
        pDesc, pRestrictToOutput, ppSwapChain);
    if (SUCCEEDED(hr) && ppSwapChain && *ppSwapChain)
    {
        IDXGISwapChain* sc = static_cast<IDXGISwapChain*>(*ppSwapChain);
        TryWrapSwapChainOptionB("F2.CoreWindow", pDevice, &sc);
        *ppSwapChain = static_cast<IDXGISwapChain1*>(sc);
    }
    return hr;
}

STDMETHODIMP CreateSwapChainForComposition(IDXGIFactory2* This, IUnknown* pDevice,
    const DXGI_SWAP_CHAIN_DESC1* pDesc,
    IDXGIOutput* pRestrictToOutput,
    IDXGISwapChain1** ppSwapChain)
{
    if (!WrapFactory2SwapChains())
        return Original_CreateSwapChainForComposition(This, pDevice, pDesc, pRestrictToOutput, ppSwapChain);
    WrapperLog("\nCreateSwapChainForComposition ENTER\n");
    HRESULT hr = Original_CreateSwapChainForComposition(This, pDevice, pDesc,
        pRestrictToOutput, ppSwapChain);
    if (SUCCEEDED(hr) && ppSwapChain && *ppSwapChain)
    {
        IDXGISwapChain* sc = static_cast<IDXGISwapChain*>(*ppSwapChain);
        TryWrapSwapChainOptionB("F2.Comp", pDevice, &sc);
        *ppSwapChain = static_cast<IDXGISwapChain1*>(sc);
    }
    return hr;
}

// Slots 15/16/24 exist on IDXGIFactory2 and later only, probed by QI whichever entry point made the factory.
static void HookFactory2SwapChainSlots(void* pFactory)
{
    IDXGIFactory2* asF2 = NULL;
    if (FAILED(static_cast<IUnknown*>(pFactory)->QueryInterface(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(&asF2))) ||
        !asF2)
        return;
    void** vt2 = *(void***)asF2;
    EnsureHookCacheInit();
    bool install15 = false, install16 = false, install24 = false;
    {
        EnterCriticalSection(&s_hookedAddrsLock);
        install15 = s_hookedCreateSCForHwndAddrs.insert(vt2[15]).second;
        install16 = s_hookedCreateSCForCWAddrs.insert(vt2[16]).second;
        install24 = s_hookedCreateSCForCompAddrs.insert(vt2[24]).second;
        LeaveCriticalSection(&s_hookedAddrsLock);
    }
    if (install15) HookCode(vt2[15], CreateSwapChainForHwnd,        (PVOID*)&Original_CreateSwapChainForHwnd,        HOOKING_FLAG);
    if (install16) HookCode(vt2[16], CreateSwapChainForCoreWindow,  (PVOID*)&Original_CreateSwapChainForCoreWindow,  HOOKING_FLAG);
    if (install24) HookCode(vt2[24], CreateSwapChainForComposition, (PVOID*)&Original_CreateSwapChainForComposition, HOOKING_FLAG);
    asF2->Release();
}

typedef HRESULT (STDMETHODCALLTYPE *PFNCREATEDXGIFACTORY2)(UINT Flags, REFIID riid, void **ppFactory);

STDMETHODIMP Hooked_CreateDXGIFactory2(UINT Flags, REFIID riid, void **ppFactory, HMODULE hCallingModule)
{
    HMODULE hDXGI = GetModuleHandle(_T("dxgi.dll"));
    PFNCREATEDXGIFACTORY2 pReal =
        (PFNCREATEDXGIFACTORY2)GetProcAddress(hDXGI, "CreateDXGIFactory2");
    if (!pReal) return E_FAIL;

    if (IsInternalCall() || gInfo.UseMonoDeviceWrapper)
        return pReal(Flags, riid, ppFactory);

#ifndef FINAL_RELEASE
    if (hCallingModule)
    {
        TCHAR moduleName[MAX_PATH];
        GetModuleFileName(hCallingModule, moduleName, sizeof(moduleName));
        DEBUG_MESSAGE(_T("Calling Module: %s\n"), moduleName);
    }
#endif

    HRESULT hResult = pReal(Flags, riid, ppFactory);
    if (FAILED(hResult) || !ppFactory || !*ppFactory)
        return hResult;

    // Slot 10, and 15/16/24 on an IDXGIFactory2; this entry's Factory2 swap chains are always wrapped.
    s_factory2Entry = true;
    HookFactorySwapChainSlot(*ppFactory);
    return hResult;
}