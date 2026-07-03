#include <DXUT.h>
#include <DXUTmisc.h>
#include <SDKmisc.h>
#include <SDKmesh.h>
#include <D3DX9Mesh.h>
#include <shlwapi.h>
#include "../Shared/ProductNames.h"
#include "../S3DAPI/StereoAPI.h"

//--------------------------------------------------------------------------------------
// D3D9 data
//--------------------------------------------------------------------------------------
namespace d3d9 
{
	ID3DXEffect*                        g_pEffect = NULL;
	ID3DXMesh*                          g_pMesh = NULL;
	ID3DXFont*							g_pFont = NULL;
	D3DMATERIAL9*						g_pMeshMaterials = NULL;
	DWORD								g_dwNumMeshMaterials = NULL;
	D3DXHANDLE							g_pTechnique = NULL;
	D3DXHANDLE                          g_pWorldVariable = NULL;
	D3DXHANDLE                          g_pViewVariable = NULL;
	D3DXHANDLE                          g_pProjectionVariable = NULL;
	D3DXHANDLE                          g_pEyeVariable = NULL;
	D3DXHANDLE                          g_pLightPosVariable = NULL;
	D3DXHANDLE                          g_pDiffuseVariable = NULL;
	D3DXHANDLE                          g_pSpecularVariable = NULL;

	bool	CALLBACK IsDeviceAcceptable( D3DCAPS9* pCaps, D3DFORMAT AdapterFormat, D3DFORMAT BackBufferFormat, bool bWindowed, void* pUserContext );
	HRESULT CALLBACK OnCreateDevice( IDirect3DDevice9* pd3dDevice, const D3DSURFACE_DESC* pBackBufferSurfaceDesc, void* pUserContext );
	HRESULT CALLBACK OnResetDevice( IDirect3DDevice9* pd3dDevice, const D3DSURFACE_DESC* pBackBufferSurfaceDesc, void* pUserContext );
	void	CALLBACK OnFrameRender( IDirect3DDevice9* pd3dDevice, double fTime, float fElapsedTime, void* pUserContext );
	void	CALLBACK OnLostDevice( void* pUserContext );
	void	CALLBACK OnDestroyDevice( void* pUserContext );
};

//--------------------------------------------------------------------------------------
// D3D10 data
//--------------------------------------------------------------------------------------
namespace d3d10 
{
	CDXUTSDKMesh                        g_Mesh;
	ID3DX10Font*						g_pFont = NULL;
	ID3DX10Sprite*						g_pFontSprite = NULL;
	ID3D10Effect*                       g_pEffect = NULL;
	ID3D10InputLayout*                  g_pVertexLayout = NULL;
	ID3D10EffectTechnique*              g_pTechnique = NULL;
	ID3D10EffectMatrixVariable*         g_pWorldVariable = NULL;
	ID3D10EffectMatrixVariable*         g_pViewVariable = NULL;
	ID3D10EffectMatrixVariable*         g_pProjectionVariable = NULL;
	ID3D10EffectVectorVariable*         g_pEyeVariable = NULL;
	ID3D10EffectVectorVariable*         g_pLightPosVariable = NULL;
	ID3D10EffectVectorVariable*			g_pDiffuseVariable = NULL;
	ID3D10EffectVectorVariable*			g_pSpecularVariable = NULL;

	bool	CALLBACK IsDeviceAcceptable( UINT Adapter, UINT Output, D3D10_DRIVER_TYPE DeviceType, DXGI_FORMAT BufferFormat, bool bWindowed, void* pUserContext );
	HRESULT CALLBACK OnCreateDevice( ID3D10Device* pd3dDevice, const DXGI_SURFACE_DESC* pBufferSurfaceDesc, void* pUserContext );
	HRESULT CALLBACK OnResizedSwapChain( ID3D10Device* pd3dDevice, IDXGISwapChain* pSwapChain, const DXGI_SURFACE_DESC* pBufferSurfaceDesc, void* pUserContext );
	void	CALLBACK OnReleasingSwapChain( void* pUserContext );
	void	CALLBACK OnDestroyDevice( void* pUserContext );
	void	CALLBACK OnFrameRender( ID3D10Device* pd3dDevice, double fTime, float fElapsedTime, void* pUserContext );
};

//--------------------------------------------------------------------------------------
// Shared
//--------------------------------------------------------------------------------------

DXUTDeviceSettings	g_AppDeviceSettings;
IStereoAPI*			g_StereoAPI = 0;

D3DXMATRIX			g_World;
D3DXMATRIX			g_View;
D3DXMATRIX			g_Projection;

D3DXVECTOR3			g_Eye( 0.0, 0.0, -10.0 );
D3DXVECTOR3			g_At( 0.0, 0.0, 0.0 );
D3DXVECTOR3			g_Up( 0.0, 1.0, 0.0 );

D3DXVECTOR3			g_LightPos( 0.0f, 10.0f, -5.0f );
D3DXCOLOR  			g_ClearColor( 0.0f, 0.05f, 0.1f, 1.0f );

LRESULT CALLBACK MsgProc( HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, bool* pbNoFurtherProcessing, void* pUserContext );
void	CALLBACK OnKeyboard( UINT nChar, bool bKeyDown, bool bAltDown, void* pUserContext );
void	CALLBACK OnFrameMove( double fTime, float fElapsedTime, void* pUserContext );
bool	CALLBACK ModifyDeviceSettings( DXUTDeviceSettings* pDeviceSettings, void* pUserContext );

void BeginStereo()
{
	if (g_StereoAPI) {
		g_StereoAPI->BeginStereoBlock();
	}
}

void EndStereo()
{
	if (g_StereoAPI) {
		g_StereoAPI->EndStereoBlock();
	}
}

HRESULT InitializeDX9(BOOL Windowed, DWORD Width, DWORD Height, DWORD RefreshRate, BOOL vsync)
{
	// Check for hardware T&L
	DWORD Flags = 0;
	D3DCAPS9 D3DCaps;

	DXUTGetD3D9Object()->GetDeviceCaps(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, &D3DCaps);
	if (D3DCaps.DevCaps & D3DDEVCAPS_HWTRANSFORMANDLIGHT)
	{
		Flags = D3DCREATE_HARDWARE_VERTEXPROCESSING;
		if (D3DCaps.DevCaps & D3DDEVCAPS_PUREDEVICE) {
			Flags |= D3DCREATE_PUREDEVICE;
		}
	}
	else {
		Flags = D3DCREATE_SOFTWARE_VERTEXPROCESSING;
	}

	// Fill up device settings
	g_AppDeviceSettings.d3d9.AdapterOrdinal                = D3DADAPTER_DEFAULT;
	g_AppDeviceSettings.d3d9.DeviceType                    = D3DDEVTYPE_HAL;
	g_AppDeviceSettings.d3d9.BehaviorFlags                 = Flags;
	g_AppDeviceSettings.d3d9.pp.BackBufferWidth            = Width;
	g_AppDeviceSettings.d3d9.pp.BackBufferHeight           = Height;
	g_AppDeviceSettings.d3d9.pp.BackBufferFormat           = D3DFMT_X8R8G8B8;
	g_AppDeviceSettings.d3d9.pp.BackBufferCount            = 2;
	g_AppDeviceSettings.d3d9.pp.MultiSampleType            = D3DMULTISAMPLE_NONE;
	g_AppDeviceSettings.d3d9.pp.SwapEffect                 = D3DSWAPEFFECT_DISCARD;
	g_AppDeviceSettings.d3d9.pp.hDeviceWindow              = DXUTGetHWND();
	g_AppDeviceSettings.d3d9.pp.Windowed                   = Windowed;
	g_AppDeviceSettings.d3d9.pp.EnableAutoDepthStencil     = TRUE;
	g_AppDeviceSettings.d3d9.pp.AutoDepthStencilFormat     = D3DFMT_D24S8;
	g_AppDeviceSettings.d3d9.pp.Flags                      = 0;
	g_AppDeviceSettings.d3d9.pp.FullScreen_RefreshRateInHz = Windowed ? 0 : RefreshRate;
	g_AppDeviceSettings.d3d9.pp.PresentationInterval       = vsync ? D3DPRESENT_INTERVAL_ONE : D3DPRESENT_INTERVAL_IMMEDIATE;
		
	// Resize window
	// Further back buffer size will be retrieved from window client rect
	if (Windowed)
	{
		RECT  rcClient, rcWindow;
		POINT ptDiff;
		GetClientRect(DXUTGetHWND(), &rcClient);
		GetWindowRect(DXUTGetHWND(), &rcWindow);
		ptDiff.x = (rcWindow.right - rcWindow.left) - rcClient.right;
		ptDiff.y = (rcWindow.bottom - rcWindow.top) - rcClient.bottom;

		MoveWindow( DXUTGetHWND(),
					100,
					100,
					Width  + ptDiff.x,
					Height + ptDiff.y,
					TRUE );
	}

	return DXUTCreateDeviceFromSettings(&g_AppDeviceSettings);
}

HMONITOR GetPrimaryMonitorHandle()
{
	const POINT ptZero = { 0, 0 };
	return MonitorFromPoint(ptZero, MONITOR_DEFAULTTOPRIMARY);
}

HRESULT FindPrimaryMonitor(DXUTD3D10DeviceSettings& d3d10)
{
	HMONITOR hPrimaryMonitor = GetPrimaryMonitorHandle();

	// Search for this monitor in our enumeration hierarchy.
	CD3D10Enumeration* pd3dEnum = DXUTGetD3D10Enumeration();
	CGrowableArray <CD3D10EnumAdapterInfo*>* pAdapterList = pd3dEnum->GetAdapterInfoList();
	for( int iAdapter = 0; iAdapter < pAdapterList->GetSize(); ++iAdapter )
	{
		CD3D10EnumAdapterInfo* pAdapterInfo = pAdapterList->GetAt( iAdapter );
		for( int o = 0; o < pAdapterInfo->outputInfoList.GetSize(); ++o )
		{
			CD3D10EnumOutputInfo* pOutputInfo = pAdapterInfo->outputInfoList.GetAt( o );
			DXGI_OUTPUT_DESC Desc;
			pOutputInfo->m_pOutput->GetDesc( &Desc );

			if( hPrimaryMonitor == Desc.Monitor )
			{
				d3d10.AdapterOrdinal = pAdapterInfo->AdapterOrdinal;
				d3d10.Output = pOutputInfo->Output;
				return S_OK;
			}
		}
	}
	return E_FAIL;
}

HRESULT InitializeDX10(BOOL Windowed, DWORD Width, DWORD Height, DWORD RefreshRate, BOOL vsync)
{
	HRESULT hr;

	g_AppDeviceSettings.ver                          = DXUT_D3D10_DEVICE;
	g_AppDeviceSettings.d3d10.AdapterOrdinal         = 0;
	g_AppDeviceSettings.d3d10.DriverType             = D3D10_DRIVER_TYPE_HARDWARE;
	g_AppDeviceSettings.d3d10.Output                 = 0;
	g_AppDeviceSettings.d3d10.CreateFlags            = 0;
	g_AppDeviceSettings.d3d10.SyncInterval           = vsync ? 1 : 0;
	g_AppDeviceSettings.d3d10.PresentFlags           = 0;
	g_AppDeviceSettings.d3d10.AutoCreateDepthStencil = TRUE;
	g_AppDeviceSettings.d3d10.AutoDepthStencilFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;

	FindPrimaryMonitor(g_AppDeviceSettings.d3d10);

	if (Windowed)
	{
		DXGI_RATIONAL RefreshRateRational                        = { 0, 1 };
		g_AppDeviceSettings.d3d10.sd.BufferDesc.Width            = Width;
		g_AppDeviceSettings.d3d10.sd.BufferDesc.Height           = Height;
		g_AppDeviceSettings.d3d10.sd.BufferDesc.RefreshRate      = RefreshRateRational;
		g_AppDeviceSettings.d3d10.sd.BufferDesc.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
		g_AppDeviceSettings.d3d10.sd.BufferDesc.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED;
		g_AppDeviceSettings.d3d10.sd.BufferDesc.Scaling          = DXGI_MODE_SCALING_UNSPECIFIED;
	}
	else
	{
		IDXGIFactory* Factory = DXUTGetDXGIFactory();
		if (!Factory) {
			return E_FAIL;
		}

		IDXGIAdapter* Adapter;
		V_RETURN( Factory->EnumAdapters(g_AppDeviceSettings.d3d10.AdapterOrdinal, &Adapter) );

		IDXGIOutput* Output;
		V_RETURN( Adapter->EnumOutputs(g_AppDeviceSettings.d3d10.Output, &Output) );

		// Find closest matching mode;
		DXGI_MODE_DESC ClosestModeToMatch;
		DXGI_MODE_DESC ClosestMatchingMode;
		{
			DXGI_RATIONAL RefreshRateRational   = { RefreshRate, 1 };
			ClosestModeToMatch.Width            = Width;
			ClosestModeToMatch.Height           = Height;
			ClosestModeToMatch.RefreshRate      = RefreshRateRational;
			ClosestModeToMatch.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
			ClosestModeToMatch.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED;
			ClosestModeToMatch.Scaling          = DXGI_MODE_SCALING_UNSPECIFIED;
			
			V_RETURN( Output->FindClosestMatchingMode(&ClosestModeToMatch, &ClosestMatchingMode, NULL) );
		}

		g_AppDeviceSettings.d3d10.sd.BufferDesc  = ClosestMatchingMode;
	}
	DXGI_SAMPLE_DESC SampleDesc                  = { 0, 0 };
	g_AppDeviceSettings.d3d10.sd.SampleDesc      = SampleDesc;
	g_AppDeviceSettings.d3d10.sd.BufferUsage     = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	g_AppDeviceSettings.d3d10.sd.BufferCount     = 1;
	g_AppDeviceSettings.d3d10.sd.OutputWindow    = DXUTGetHWND();
	g_AppDeviceSettings.d3d10.sd.Windowed        = Windowed;
	g_AppDeviceSettings.d3d10.sd.SwapEffect      = DXGI_SWAP_EFFECT_DISCARD;
	g_AppDeviceSettings.d3d10.sd.Flags           = 0;

	return DXUTCreateDeviceFromSettings(&g_AppDeviceSettings);
}

//--------------------------------------------------------------------------------------
// Entry point to the program. Initializes everything and goes into a message processing 
// loop. Idle time is used to render the scene.
//--------------------------------------------------------------------------------------
INT WINAPI wWinMain( HINSTANCE, HINSTANCE, LPWSTR cmdLine, int )
{
	// Enable run-time memory check for debug builds.
#ifdef _DEBUG
	_CrtSetDbgFlag( _CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF );
#endif

	// Remove the working directory from the search path to work around the DLL preloading vulnerability
	HMODULE hKernel = GetModuleHandle(L"Kernel32.dll");
	if (hKernel)
	{
		typedef BOOL (WINAPI *SetDllDirectoryWType)(LPCWSTR lpPathName);
		SetDllDirectoryWType pfnSetDllDirectory = (SetDllDirectoryWType)GetProcAddress(hKernel, "SetDllDirectoryW");
		if (pfnSetDllDirectory)
			pfnSetDllDirectory(L"");

		typedef BOOL (WINAPI *SetSearchPathModeType)(DWORD Flags);
		SetSearchPathModeType pfnSetSearchPathMode = (SetSearchPathModeType)GetProcAddress(hKernel, "SetSearchPathMode");
		if (pfnSetSearchPathMode)
			pfnSetSearchPathMode(BASE_SEARCH_PATH_ENABLE_SAFE_SEARCHMODE);
	}
	
	// Setup AppData as search path
	TCHAR szPath[ MAX_PATH ];
	if( SUCCEEDED(SHGetFolderPath(NULL, CSIDL_COMMON_APPDATA, NULL, 0, szPath )) ) 
	{
		PathAppend( szPath, TEXT(PRODUCT_NAME));
		PathAppend( szPath, TEXT("DTest"));
	}	
	SetCurrentDirectory(szPath);

	// Set DXUT callbacks
	DXUTSetCallbackD3D9DeviceAcceptable( d3d9::IsDeviceAcceptable );
	DXUTSetCallbackD3D9DeviceCreated( d3d9::OnCreateDevice );
	DXUTSetCallbackD3D9DeviceReset( d3d9::OnResetDevice );
	DXUTSetCallbackD3D9FrameRender( d3d9::OnFrameRender );
	DXUTSetCallbackD3D9DeviceLost( d3d9::OnLostDevice );
	DXUTSetCallbackD3D9DeviceDestroyed( d3d9::OnDestroyDevice );

	DXUTSetCallbackD3D10DeviceAcceptable( d3d10::IsDeviceAcceptable );
	DXUTSetCallbackD3D10DeviceCreated( d3d10::OnCreateDevice );
	DXUTSetCallbackD3D10SwapChainResized( d3d10::OnResizedSwapChain );
	DXUTSetCallbackD3D10SwapChainReleasing( d3d10::OnReleasingSwapChain );
	DXUTSetCallbackD3D10DeviceDestroyed( d3d10::OnDestroyDevice );
	DXUTSetCallbackD3D10FrameRender( d3d10::OnFrameRender );

	DXUTSetCallbackMsgProc( MsgProc );
	DXUTSetCallbackKeyboard( OnKeyboard );
	DXUTSetCallbackFrameMove( OnFrameMove );
	DXUTSetCallbackDeviceChanging( ModifyDeviceSettings );

#ifdef _DEBUG
	bool MBoxOnErrors = true;
#else
	bool MBoxOnErrors = false;
#endif
	DXUTInit( true, MBoxOnErrors, NULL );        
	DXUTSetCursorSettings( true, true );
	DXUTCreateWindow( L"Dynamic Test" );

	// Parse command line & retrieve settings
	DXUTDeviceVersion   DeviceVersion = DXUT_D3D10_DEVICE;
	BOOL                Windowed      = TRUE;
	DWORD               Width         = 800;
	DWORD               Height        = 600;
	DWORD				RefreshRate   = 60;
	BOOL				VSync		  = TRUE;
	BOOL				StereoAPI     = FALSE;
	{
		LPWSTR*	szArgList;
		int		argCount;
 
		szArgList = CommandLineToArgvW(GetCommandLine(), &argCount);
		if (szArgList)
		{
			for (int i = 1; i<argCount; ++i)
			{
				if (StrCmp(szArgList[i], L"-dx9") == 0) {
					DeviceVersion = DXUT_D3D9_DEVICE;
				}
				else if (StrCmp(szArgList[i], L"-dx10") == 0) {
					DeviceVersion = DXUT_D3D10_DEVICE;
				}
				else if (StrCmp(szArgList[i], L"-stereo_api") == 0) {
					StereoAPI = TRUE;
				}
				else if (StrCmp(szArgList[i], L"-fullscreen") == 0) 
				{
					Windowed = FALSE;

					DEVMODE CurrentMode;
					if ( EnumDisplaySettings(NULL, ENUM_CURRENT_SETTINGS, &CurrentMode) )
					{
						Width       = CurrentMode.dmPelsWidth;
						Height      = CurrentMode.dmPelsHeight;
						RefreshRate = CurrentMode.dmDisplayFrequency;
					}
				}
				else if (StrCmp(szArgList[i], L"-no_vsync") == 0) {
					VSync = FALSE;
				}
			}
		}

		LocalFree(szArgList);
	}

	// Try to create DX10 device
	g_AppDeviceSettings.ver = DeviceVersion;
	if (g_AppDeviceSettings.ver == DXUT_D3D10_DEVICE)
	{
		if ( FAILED( InitializeDX10(Windowed, Width, Height, RefreshRate, VSync) ) ) 
		{
			// Try to initialize DX9 as well
			g_AppDeviceSettings.ver = DXUT_D3D9_DEVICE;
		}
	}

	if (g_AppDeviceSettings.ver == DXUT_D3D9_DEVICE)
	{
		if ( FAILED( InitializeDX9(Windowed, Width, Height, RefreshRate, VSync) ) ) {
			return EXIT_FAILURE;
		}
	}
	
	if (StereoAPI)
	{
		if (HMODULE hMod = GetStereoLibraryHandle()) {
			CreateStereoAPI( hMod, &g_StereoAPI );
		}
	}

	DXUTMainLoop();
	SAFE_RELEASE(g_StereoAPI);
	return DXUTGetExitCode();
}

//--------------------------------------------------------------------------------------
// Called right before creating a D3D9 or D3D10 device, allowing the app to modify the device settings as needed
//--------------------------------------------------------------------------------------
bool CALLBACK ModifyDeviceSettings( DXUTDeviceSettings* pDeviceSettings, void* pUserContext )
{
	if ( DXUT_D3D9_DEVICE == pDeviceSettings->ver )
	{
		// DXUT gives invalid settings for DX9, use own
		pDeviceSettings->d3d9 = g_AppDeviceSettings.d3d9;

		// Resice back buffer
		if (g_AppDeviceSettings.d3d9.pp.Windowed)
		{
			RECT clientRect;
			GetClientRect(DXUTGetHWND(), &clientRect);
			pDeviceSettings->d3d9.pp.BackBufferWidth  = clientRect.right - clientRect.left;
			pDeviceSettings->d3d9.pp.BackBufferHeight = clientRect.bottom - clientRect.top;
		}

		// Correct aspect
		float fAspect = static_cast<float>(pDeviceSettings->d3d9.pp.BackBufferWidth) / static_cast<float>(pDeviceSettings->d3d9.pp.BackBufferHeight);
		D3DXMatrixPerspectiveFovLH( &g_Projection, D3DX_PI * 0.25f, fAspect, 0.5f, 100.0f );
	}
	else {
		g_AppDeviceSettings.d3d10 = pDeviceSettings->d3d10;
	}

	return true;
}

//--------------------------------------------------------------------------------------
// Handle updates to the scene.  This is called regardless of which D3D API is used
//--------------------------------------------------------------------------------------
void CALLBACK OnFrameMove( double fTime, float fElapsedTime, void* pUserContext )
{
	// Rotate cube around the origin
	D3DXMatrixRotationY( &g_World, (float)fTime );
}


//--------------------------------------------------------------------------------------
// Handle messages to the application
//--------------------------------------------------------------------------------------
LRESULT CALLBACK MsgProc( HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, bool* pbNoFurtherProcessing, void* pUserContext )
{
	return 0;
}


//--------------------------------------------------------------------------------------
// Handle key presses
//--------------------------------------------------------------------------------------
void CALLBACK OnKeyboard( UINT nChar, bool bKeyDown, bool bAltDown, void* pUserContext )
{
}

namespace d3d9 {

//--------------------------------------------------------------------------------------
// Rejects any D3D9 devices that aren't acceptable to the app by returning false
//--------------------------------------------------------------------------------------
bool CALLBACK IsDeviceAcceptable( D3DCAPS9* pCaps, D3DFORMAT AdapterFormat, D3DFORMAT BackBufferFormat, bool bWindowed, void* pUserContext )
{
	// No fallback defined by this app, so reject any device that doesn't support at least ps2.0
	if( pCaps->PixelShaderVersion < D3DPS_VERSION(2, 0) ) {
		return false;
	}

	// Skip backbuffer formats that don't support alpha blending
	IDirect3D9* pD3D = DXUTGetD3D9Object();
	if( FAILED( pD3D->CheckDeviceFormat( pCaps->AdapterOrdinal, 
										 pCaps->DeviceType,
										 AdapterFormat, 
										 D3DUSAGE_QUERY_POSTPIXELSHADER_BLENDING,
										 D3DRTYPE_TEXTURE, 
										 BackBufferFormat ) ) )
	{
		return false;
	}

	return true;
}

//--------------------------------------------------------------------------------------
// Create any D3D9 resources that will live through a device reset (D3DPOOL_MANAGED)
// and aren't tied to the back buffer size 
//--------------------------------------------------------------------------------------
HRESULT CALLBACK OnCreateDevice( IDirect3DDevice9* pd3dDevice, const D3DSURFACE_DESC* pBackBufferSurfaceDesc, void* pUserContext )
{
	HRESULT hr;

	// Load the mesh
	ID3DXMesh*   pMesh = 0;
	ID3DXBuffer* pD3DXMtrlBuffer = 0;
	V_RETURN( D3DXLoadMeshFromX( L"data\\models\\logo\\logo.x", 
								 D3DXMESH_MANAGED, 
								 pd3dDevice, 
								 NULL, 
								 &pD3DXMtrlBuffer, 
								 NULL, 
								 &g_dwNumMeshMaterials, 
								 &g_pMesh ) );
	
	// Copy materials
	D3DXMATERIAL* pD3DXMaterials = (D3DXMATERIAL*)pD3DXMtrlBuffer->GetBufferPointer();
	if (pD3DXMaterials)
	{
		g_pMeshMaterials = new D3DMATERIAL9[g_dwNumMeshMaterials];
		for (DWORD i = 0; i<g_dwNumMeshMaterials; ++i)
		{
			g_pMeshMaterials[i] = pD3DXMaterials[i].MatD3D;
		}
	}
	SAFE_RELEASE(pD3DXMtrlBuffer);

	// Load effect
	LPD3DXBUFFER pErrors;
	DWORD dwShaderFlags = D3DXFX_NOT_CLONEABLE | D3DXSHADER_NO_PRESHADER | D3DXFX_LARGEADDRESSAWARE;
	hr = D3DXCreateEffectFromFile( pd3dDevice, 
								   L"data\\shaders\\DTestDX9.fx", 
								   NULL, 
								   NULL, 
								   dwShaderFlags, 
								   NULL, 
								   &g_pEffect, 
								   &pErrors );

	if ( !SUCCEEDED(hr) ) 
	{
		if (pErrors) {
			DXUTOutputDebugStringA( (char*)pErrors->GetBufferPointer() );
		}
		else {
			DXUTOutputDebugStringW( L"Can't create effect.\n" );
		}
		return hr;
	}

	// Obtain the technique
	g_pTechnique			= g_pEffect->GetTechniqueByName( "Render" );
	g_pWorldVariable		= g_pEffect->GetParameterByName( NULL, "World" );
	g_pViewVariable			= g_pEffect->GetParameterByName( NULL, "View" );
	g_pProjectionVariable	= g_pEffect->GetParameterByName( NULL, "Projection" );
	g_pEyeVariable			= g_pEffect->GetParameterByName( NULL, "Eye" );
	g_pDiffuseVariable		= g_pEffect->GetParameterByName( NULL, "Diffuse" );
	g_pSpecularVariable		= g_pEffect->GetParameterByName( NULL, "Specular" );
	g_pLightPosVariable		= g_pEffect->GetParameterByName( NULL, "LightPos" );
	
	// Create font
	V_RETURN( D3DXCreateFont( pd3dDevice, 
							  36, 
							  0, 
							  FW_BOLD, 
							  1, 
							  FALSE, 
							  DEFAULT_CHARSET, 
							  OUT_DEFAULT_PRECIS, 
							  DEFAULT_QUALITY, 
							  DEFAULT_PITCH | FF_DONTCARE, 
							  L"Arial", 
							  &g_pFont ) );

	// Initialize the world matrices
	D3DXMatrixIdentity( &g_World );

	// Initialize the view matrix
	D3DXMatrixLookAtLH( &g_View, &g_Eye, &g_At, &g_Up );

	return S_OK;
}

//--------------------------------------------------------------------------------------
// Create any D3D9 resources that won't live through a device reset (D3DPOOL_DEFAULT) 
// or that are tied to the back buffer size 
//--------------------------------------------------------------------------------------
HRESULT CALLBACK OnResetDevice( IDirect3DDevice9* pd3dDevice, const D3DSURFACE_DESC* pBackBufferSurfaceDesc, void* pUserContext )
{
	HRESULT hr;

	if (g_pEffect) {
		V_RETURN( g_pEffect->OnResetDevice() );
	}
	
	g_AppDeviceSettings.d3d9.pp.BackBufferWidth    = pBackBufferSurfaceDesc->Width;
	g_AppDeviceSettings.d3d9.pp.BackBufferHeight   = pBackBufferSurfaceDesc->Height;
	g_AppDeviceSettings.d3d9.pp.BackBufferFormat   = pBackBufferSurfaceDesc->Format;
	g_AppDeviceSettings.d3d9.pp.MultiSampleType    = pBackBufferSurfaceDesc->MultiSampleType;
	g_AppDeviceSettings.d3d9.pp.MultiSampleQuality = pBackBufferSurfaceDesc->MultiSampleQuality;

	return S_OK;
}


//--------------------------------------------------------------------------------------
// Render the scene using the D3D9 device
//--------------------------------------------------------------------------------------
void CALLBACK OnFrameRender( IDirect3DDevice9* pd3dDevice, double fTime, float fElapsedTime, void* pUserContext )
{
	HRESULT hr;
	
	// Clear the render target and the zbuffer
	D3DXCOLOR ClearColorGamma;
	ClearColorGamma.r = powf(g_ClearColor.r, 1.0f / 2.2f);
	ClearColorGamma.g = powf(g_ClearColor.g, 1.0f / 2.2f);
	ClearColorGamma.b = powf(g_ClearColor.b, 1.0f / 2.2f);
	ClearColorGamma.a = 1.0f;
	V( pd3dDevice->Clear( 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, ClearColorGamma, 1.0f, 0 ) );

	// Render the scene
	if (g_StereoAPI) {
		g_StereoAPI->BeginStereoBlock();
	}

	if( SUCCEEDED( pd3dDevice->BeginScene() ) )
	{
		// Update the effect's variables. 
		V( g_pEffect->SetMatrix( g_pWorldVariable,		&g_World ) );
		V( g_pEffect->SetMatrix( g_pViewVariable,		&g_View ) );
		V( g_pEffect->SetMatrix( g_pProjectionVariable, &g_Projection ) );

		V( g_pEffect->SetValue( g_pEyeVariable,			&g_Eye,			sizeof(D3DXVECTOR3) ) );
		V( g_pEffect->SetValue( g_pLightPosVariable,	&g_LightPos,	sizeof(D3DXVECTOR3) ) );

		V( g_pEffect->SetTechnique( g_pTechnique ) );

		BeginStereo();
		{
			// Apply the technique contained in the effect and render the mesh
			UINT cPasses;
			V( g_pEffect->Begin( &cPasses, 0 ) );
			{
				for(DWORD iPass = 0; iPass < cPasses; ++iPass)
				{
					for (DWORD iMat = 0; iMat < g_dwNumMeshMaterials; ++iMat)
					{
						V( g_pEffect->SetValue( g_pDiffuseVariable,  &g_pMeshMaterials[iMat].Diffuse,  sizeof(D3DCOLORVALUE) ) );
						V( g_pEffect->SetValue( g_pSpecularVariable, &g_pMeshMaterials[iMat].Specular, sizeof(D3DCOLORVALUE) ) );
						V( g_pEffect->BeginPass( iPass ) );
						V( g_pMesh->DrawSubset(iMat) );
						V( g_pEffect->EndPass() );
					}
				}
			}
			V( g_pEffect->End() );
		
			if (g_StereoAPI)
			{
				// just to illustrate that you can make subblocks
				g_StereoAPI->BeginMonoBlock();
				{
					RECT rect;
					rect.left   = g_AppDeviceSettings.d3d9.pp.BackBufferWidth / 2 - 125;
					rect.right  = rect.left + 250;
					rect.top    = 50;
					rect.bottom = rect.top + 40;

					g_pFont->DrawText( NULL, _T("Text is MONO"), -1, &rect, DT_CENTER, D3DCOLOR_RGBA(255,255,255,255) );
				}
				g_StereoAPI->EndMonoBlock();
			}
		}
		EndStereo();

		V( pd3dDevice->EndScene() );
	}

	if (g_StereoAPI) {
		g_StereoAPI->EndStereoBlock();
	}
}


//--------------------------------------------------------------------------------------
// Release D3D9 resources created in the OnResetDevice callback 
//--------------------------------------------------------------------------------------
void CALLBACK OnLostDevice( void* pUserContext )
{
	if (g_pEffect) {
		g_pEffect->OnLostDevice();
	}
}


//--------------------------------------------------------------------------------------
// Release D3D9 resources created in the OnCreateDevice callback 
//--------------------------------------------------------------------------------------
void CALLBACK OnDestroyDevice( void* pUserContext )
{
	SAFE_RELEASE( g_pEffect );
	SAFE_RELEASE( g_pMesh );
	SAFE_RELEASE( g_pFont );
	SAFE_DELETE_ARRAY( g_pMeshMaterials );
}

} // namespace d3d9

namespace d3d10 {

//--------------------------------------------------------------------------------------
// Reject any D3D10 devices that aren't acceptable by returning false
//--------------------------------------------------------------------------------------
bool CALLBACK IsDeviceAcceptable( UINT Adapter, UINT Output, D3D10_DRIVER_TYPE DeviceType, DXGI_FORMAT BufferFormat, bool bWindowed, void* pUserContext )
{
	return true;
}

//--------------------------------------------------------------------------------------
// Create any D3D10 resources that aren't dependant on the back buffer
//--------------------------------------------------------------------------------------
HRESULT CALLBACK OnCreateDevice( ID3D10Device* pd3dDevice, const DXGI_SURFACE_DESC* pBufferSurfaceDesc, void* pUserContext )
{
	HRESULT hr;

	// Find the D3DX effect file
	DWORD dwShaderFlags = D3D10_SHADER_ENABLE_STRICTNESS;
#ifdef _DEBUG
	dwShaderFlags |= D3D10_SHADER_DEBUG;
#endif
	ID3D10Blob* pErrors = 0;
	hr = D3DX10CreateEffectFromFile( L"data\\shaders\\DTestDX10.fx", 
									 NULL, 
									 NULL, 
									 "fx_4_0", 
									 dwShaderFlags, 
									 0, 
									 pd3dDevice, 
									 NULL, 
									 NULL, 
									 &g_pEffect, 
									 &pErrors, 
									 NULL );
	if ( !SUCCEEDED(hr) ) 
	{
		if (pErrors) {
			DXUTOutputDebugStringA( (char*)pErrors->GetBufferPointer() );
		}
		else {
			DXUTOutputDebugStringW( L"Can't create effect.\n" );
		}
		return hr;
	}

	// Obtain the technique
	g_pTechnique			= g_pEffect->GetTechniqueByName( "Render" );
	g_pWorldVariable		= g_pEffect->GetVariableByName( "World" )->AsMatrix();
	g_pViewVariable			= g_pEffect->GetVariableByName( "View" )->AsMatrix();
	g_pProjectionVariable	= g_pEffect->GetVariableByName( "Projection" )->AsMatrix();
	g_pEyeVariable			= g_pEffect->GetVariableByName( "Eye" )->AsVector();
	g_pDiffuseVariable		= g_pEffect->GetVariableByName( "Diffuse" )->AsVector();
	g_pSpecularVariable		= g_pEffect->GetVariableByName( "Specular" )->AsVector();
	g_pLightPosVariable		= g_pEffect->GetVariableByName( "LightPos" )->AsVector();

	// Define the input layout
	const D3D10_INPUT_ELEMENT_DESC layout[] =
	{
		{ "POSITION",	0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,	D3D10_INPUT_PER_VERTEX_DATA, 0 },
		{ "NORMAL",		0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12,	D3D10_INPUT_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD",	0, DXGI_FORMAT_R32G32_FLOAT,	0, 24,	D3D10_INPUT_PER_VERTEX_DATA, 0 },
	};
	UINT numElements = sizeof( layout ) / sizeof( layout[0] );

	// Create the input layout
	D3D10_PASS_DESC PassDesc;
	g_pTechnique->GetPassByIndex(0)->GetDesc( &PassDesc );
	V_RETURN( pd3dDevice->CreateInputLayout( layout, 
											 numElements, 
											 PassDesc.pIAInputSignature,
											 PassDesc.IAInputSignatureSize, 
											 &g_pVertexLayout ) );

	// Set the input layout
	pd3dDevice->IASetInputLayout( g_pVertexLayout );

	// Load the mesh
	V_RETURN( g_Mesh.Create( pd3dDevice, L"data\\models\\logo\\logo.sdkmesh", true ) );
		
	// Create font
	V_RETURN( D3DX10CreateSprite(pd3dDevice, 0, &g_pFontSprite) );
	V_RETURN( D3DX10CreateFont( pd3dDevice, 
								36, 
								0, 
								FW_BOLD, 
								1, 
								FALSE, 
								DEFAULT_CHARSET, 
								OUT_DEFAULT_PRECIS, 
								DEFAULT_QUALITY, 
								DEFAULT_PITCH | FF_DONTCARE, 
								L"Arial", 
								&g_pFont ) );

	// Initialize the world matrices
	D3DXMatrixIdentity( &g_World );

	// Initialize the view matrix
	D3DXMatrixLookAtLH( &g_View, &g_Eye, &g_At, &g_Up );

	g_pEyeVariable->SetFloatVector( (float*)&g_Eye );
	g_pViewVariable->SetMatrix( (float*)&g_View );

	// Initialize light
	g_pLightPosVariable->SetFloatVector( (float*)&g_LightPos);

	return S_OK;
}


//--------------------------------------------------------------------------------------
// Create any D3D10 resources that depend on the back buffer
//--------------------------------------------------------------------------------------
HRESULT CALLBACK OnResizedSwapChain( ID3D10Device* pd3dDevice, IDXGISwapChain* pSwapChain, const DXGI_SURFACE_DESC* pBufferSurfaceDesc, void* pUserContext )
{
	float fAspect = static_cast<float>(pBufferSurfaceDesc->Width) / static_cast<float>(pBufferSurfaceDesc->Height);
	D3DXMatrixPerspectiveFovLH( &g_Projection, D3DX_PI * 0.25f, fAspect, 0.5f, 100.0f );
	return S_OK;
}


//--------------------------------------------------------------------------------------
// Render the scene using the D3D10 device
//--------------------------------------------------------------------------------------
void CALLBACK OnFrameRender( ID3D10Device* pd3dDevice, double fTime, float fElapsedTime, void* pUserContext )
{
	// Clear the back buffer
	ID3D10RenderTargetView* pRTV = DXUTGetD3D10RenderTargetView();
	pd3dDevice->ClearRenderTargetView( pRTV, g_ClearColor );

	// Clear the depth stencil
	ID3D10DepthStencilView* pDSV = DXUTGetD3D10DepthStencilView();
	pd3dDevice->ClearDepthStencilView( pDSV, D3D10_CLEAR_DEPTH, 1.0, 0 );

	// Update matrices
	g_pWorldVariable->SetMatrix( (FLOAT*)&g_World );
	g_pViewVariable->SetMatrix( (FLOAT*)&g_View );
	g_pProjectionVariable->SetMatrix( (FLOAT*)&g_Projection );

	// Render 
	BeginStereo();
	{
		g_Mesh.Render( pd3dDevice, g_pTechnique, 0, 0, 0, g_pDiffuseVariable, g_pSpecularVariable );

		if (g_StereoAPI)
		{
			// just to illustrate that you can make subblocks
			g_StereoAPI->BeginMonoBlock();
			{
				RECT rect;
				rect.left   = g_AppDeviceSettings.d3d10.sd.BufferDesc.Width / 2 - 125;
				rect.right  = rect.left + 250;
				rect.top    = 50;
				rect.bottom = rect.top + 40;

				g_pFontSprite->Begin(D3DX10_SPRITE_SAVE_STATE);
				g_pFont->DrawText( g_pFontSprite, _T("Text is MONO"), -1, &rect, DT_CENTER, D3DCOLOR_RGBA(255,255,255,255) );
				g_pFontSprite->Flush();
				g_pFontSprite->End();

				// Restore states broken by font
				pd3dDevice->OMSetBlendState(NULL, NULL, 0xffffffff);
				pd3dDevice->OMSetDepthStencilState(NULL, 0);
			}
			g_StereoAPI->EndMonoBlock();
		}
	}
	EndStereo();

}


//--------------------------------------------------------------------------------------
// Release D3D10 resources created in OnResizedSwapChain 
//--------------------------------------------------------------------------------------
void CALLBACK OnReleasingSwapChain( void* pUserContext )
{
}


//--------------------------------------------------------------------------------------
// Release D3D10 resources created in OnCreateDevice 
//--------------------------------------------------------------------------------------
void CALLBACK OnDestroyDevice( void* pUserContext )
{
	SAFE_RELEASE( g_pVertexLayout );
	SAFE_RELEASE( g_pFont );
	SAFE_RELEASE( g_pFontSprite );
	SAFE_RELEASE( g_pEffect );
	g_Mesh.Destroy();
}

} // namespace d3d10