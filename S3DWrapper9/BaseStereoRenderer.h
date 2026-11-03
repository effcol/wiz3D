/* 
* Project : iZ3D Stereo Driver
* Copyright (C) iZ3D Inc. 2002 - 2010
*/

#pragma once

#include "MonoRenderer.h"
#include "BaseSwapChain.h"
#include "States.h"
#include "DX9ShaderConverter.h"
#include "..\S3DAPI\GlobalData.h"
#include "GlobalData.h"
#include "..\S3DAPI\KeyboardHook.h"
#include "DX9AutoShift.h"
#include "DX9LaserSight.h"
#include "..\UILIB\Wizard.h"
#include "..\UILIB\HotKeyOSD.h"
#include "Presenter.h"
#include "Utils.h"
#include "OutputMethod_dx9.h"
#include "BaseStereoRenderer_InternalClasses.h"
#include "UMDDeviceWrapper.h"

#include "atiqbstereo.h"
class UMDDeviceWrapper;

class CBaseStereoRenderer;
namespace wiz3d { class OSDRenderer9; class LaserSightRenderer9; }

extern CBaseStereoRenderer* g_pCurrentStereoRenderer;

namespace DX9Output{
	class OutputMethod;
}

using namespace DX9Output;

#define  S3D_WINDOW_CLASS_NAME (_T(PRODUCT_NAME) _T(" Window Class"))

#define CUSTOMIZATION_NUMBER 777.0f

struct RHWHEADER { FLOAT x, y, z, rhw; };

enum RendererEvents { reNone, reCreatingDevice, reResetingDevice = reCreatingDevice, reCreatingDoubleBuffer, reCreatingAQBS };

namespace acGraphics
{
	class CDynRender;
	class CFont;
}

using namespace acGraphics;

struct RHWEmulData
{
	CComPtr<IDirect3DVertexDeclaration9>	pDecl;
	CComPtr<IDirect3DVertexShader9>			pVS;
	int										RHWEmulShaderIndex;
	
	RHWEmulData(int shaderIndex) { RHWEmulShaderIndex = shaderIndex; }
};

typedef std::pair<IDirect3DVertexDeclaration9* const, RHWEmulData>			RHWEmulPair;
#ifdef _DEBUG
typedef std::unordered_map	<IDirect3DVertexDeclaration9*, RHWEmulData>	RHWEmulMap;
#else
typedef boost::unordered_map<IDirect3DVertexDeclaration9*, RHWEmulData>	RHWEmulMap;
#endif

class DECLSPEC_UUID("88D955D4-E26F-4092-8023-44D4621A92DD")
CBaseStereoRenderer: 
	public CMonoRenderer
{
public:
	DWORD							m_BehaviorFlags;
	UINT                        	m_nAdapter[2];
	UINT							m_OrigD3DVersion;

	bool							m_bEngineStarted;	// false then device lost 
	bool							m_bDrawMonoImageOnSecondMonitor;
	DeviceModes						m_DeviceMode;
	bool							m_bTwoWindows;
	DWORD							m_NumberOfAdaptersInGroup;

	DWORD							m_dwNumSimultaneousRTs;
	std::vector<SurfaceData>		m_RenderTarget;		// Current
	SurfaceData						m_DepthStencil;		// Current

	OutputMethod*					m_pOutputMethod;

	AutoFocus						m_AutoFocus;
	DX9LaserSight					m_LaserSight;
	D3DGAMMARAMP					m_GlobalGammaRamp;	// always keep actual ramp for ScreenSaving 
#ifndef DISABLE_WIZARD
	uilib::Wizard					m_Wizard;
#endif
	iz3d::UI::HotKeyOSD				m_HotKeyOSD;
	CPresenter*						m_pPresenterThread;
	CBasePresenter*					m_pPresenter;
	UMDDeviceWrapper*				m_pUMDeviceWrapper;

	// mouse lock data
	HANDLE							m_hMapFile;
	DWORD*							m_pVar;

	BOOL 							m_bShowGDIMouse;
	HCURSOR							m_hGDIMouseCursor;
	BOOL 							m_bShowMouse;
	UINT							m_MouseXHotSpot;
	UINT							m_MouseYHotSpot;
	CComPtr<IDirect3DSurface9>		m_pMouseCursor;
	CComPtr<IDirect3DTexture9>		m_pMouseCursorTexture;

	// HelixMod-compatible stereo parameters texture. A 1x16 RGBA32F texture
	// bound to VS sampler 0 (D3DVERTEXTEXTURESAMPLER0) and PS sampler 13.
	// Updated per-eye with (separation, convergence, eye_sign, magic). Shaders
	// that have been authored or replaced to do their own stereo offset (e.g.
	// HelixMod fixes that contain `texldl r, c, s0`) read from this texture.
	// In D3DPOOL_DEFAULT, so released before device Reset and lazy-allocated
	// on next use.
	CComPtr<IDirect3DTexture9>		m_pStereoParamsTexture;
	void EnsureStereoParamsTexture();
	void UpdateStereoParamsTexture(VIEWINDEX view);
	void BindStereoParamsSamplers();
	void ReleaseStereoParamsTexture();

	CComPtr<IDirect3DQuery9>		m_pFlushQuery;
	LARGE_INTEGER					m_FlushTime;
	volatile DWORD					m_nFlushCounter;
	volatile DWORD					m_nGetDataCounter;
	FLOAT							m_PresenterFlushTime;

	CComPtr<IDirect3DPixelShader9>  m_pViewShader;
	CComPtr<IDirect3DPixelShader9>  m_pCopyShader;
	CComPtr<IDirect3DStateBlock9>   m_pCopyState;
	bool							m_bUseR32FDepthTextures;
	D3DFORMAT						m_DepthTexturesFormat16;
	D3DFORMAT						m_DepthTexturesFormat24;
	D3DFORMAT						GetDepthTextureFormat(D3DFORMAT fmt)
	{	return (fmt == D3DFMT_D16 || fmt == FOURCC_DF16) ? m_DepthTexturesFormat16 : m_DepthTexturesFormat24;	}

	CComPtr<IDirect3DSurface9>  	m_pATICommSurface;
	DWORD							m_ATILineOffset;

	ULONG							m_nScreenShotCounter;	

	ULONG							m_nRenderTargetCounter;
	ULONG							m_nRenderTargetTextureCounter;
	ULONG							m_nRenderTargetR32FTextureCounter;
	ULONG							m_nRenderTargetSquareTextureCounter;
	ULONG							m_nRenderTargetLessThanBBTextureCounter;
	ULONG							m_nDepthStencilSurfaceCounter;
	ULONG							m_nDepthStencilTextureCounter;
	ULONG							m_nTextureCounter;
	ULONG							m_nTextureDefaultCounter;
	ULONG							m_nRenderTargetCubeTextureCounter;
	ULONG							m_nDepthStencilCubeTextureCounter;
	ULONG							m_nCubeTextureCounter;
	ULONG							m_nCubeTextureDefaultCounter;
	ULONG							m_nVolumeTextureCounter;
	ULONG							m_nVolumeTextureDefaultCounter;

#ifndef FINAL_RELEASE
	UINT							m_nRenderTargetsMonoSize;
	UINT							m_nRenderTargetsStereoSize;
	UINT							m_nRenderTargetTexturesMonoSize;
	UINT							m_nRenderTargetTexturesStereoSize;
	UINT							m_nDepthStencilSurfacesMonoSize;
	UINT							m_nDepthStencilSurfacesStereoSize;
	UINT							m_nDepthStencilTexturesMonoSize;
	UINT							m_nDepthStencilTexturesStereoSize;
	UINT							m_nTexturesSize;
	UINT							m_nTexturesDefaultSize;
	UINT							m_nCubeTexturesSize;
	UINT							m_nCubeTexturesDefaultSize;
	UINT							m_nRenderTargetCubeTexturesMonoSize;
	UINT							m_nRenderTargetCubeTexturesStereoSize;
	UINT							m_nDepthStencilCubeTexturesMonoSize;
	UINT							m_nDepthStencilCubeTexturesStereoSize;
	UINT							m_nVolumeTexturesSize;
	UINT							m_nVolumeTexturesDefaultSize;
#endif

	bool							m_bDebugWarningCantCreateWideSurface;
	bool							m_bDebugWarningMonoDepthStencil;
	bool							m_bDebugDoNotRenderMono;
	bool							m_bDebugWarningEmulateFullscreenMode;

	RendererEvents					m_RendererEvents;

#ifndef FINAL_RELEASE
	typedef int (WINAPI *D3DPERF_BeginEvent_type)( D3DCOLOR col, LPCWSTR wszName );
	typedef int (WINAPI *D3DPERF_EndEvent_type)( void );
	D3DPERF_BeginEvent_type _D3DPERF_BeginEvent;
	D3DPERF_EndEvent_type   _D3DPERF_EndEvent;
	int WINAPI D3DPERF_BeginEvent( D3DCOLOR col, LPCWSTR wszName ) { return _D3DPERF_BeginEvent ? _D3DPERF_BeginEvent(col, wszName) : 0; }
	int WINAPI D3DPERF_EndEvent( void ) { return _D3DPERF_EndEvent ? _D3DPERF_EndEvent() : 0; }
#else
	int WINAPI D3DPERF_BeginEvent( D3DCOLOR col, LPCWSTR wszName ) { return 0; }
	int WINAPI D3DPERF_EndEvent( void ) { return 0; }
#endif

	bool		SkipPresenterThread		();

	void		SetNextFlushTime( LARGE_INTEGER &time );

#ifndef FINAL_RELEASE
	TCHAR							m_DumpDirectory[MAX_PATH];
	TCHAR							m_MethodDirectory[MAX_PATH];

	DWORD							m_nTexSynchCounter;
	DWORD							m_nTexSynchRTCounter;
	DWORD							m_nTexSynchCachedCounter;
	LARGE_INTEGER 					m_nShaderAnalysingTime;
	LARGE_INTEGER 					m_nCRCCalculatingTime;
	LARGE_INTEGER 					m_nTexSynchTime;
	LARGE_INTEGER 					m_nTexSynchRTTime;
	ULONG							m_nWeaponDrawCounter;

	bool							m_bDebugSkipUpdateProjectionMatrix;
	bool							m_bDebugSkipModifyRHW;
	bool							m_bDebugSkipSetProjection;
	bool							m_bDebugSkipSynchronizeTextures;
	bool							m_bDebugSkipSynchronizeDepthTextures;
	bool							m_bDebugSkipSetStereoRenderTarget;
	bool							m_bDebugRenderInMono;
	bool							m_bDebugSkipSetStereoTextures;
	bool							m_bDebugSkipCheckCRC;
	bool							m_bDebugSkipShaderMatrixAnalyzing;
	bool							m_bDebugSkipVDAnalyzing;
	bool							m_bDebugSkipTextureUsingCheck;
	bool							m_bDebugRenderInOneRT;
	bool							m_bDebugUseSimplePresenter;
	bool							m_bDebugUseDrawPassSplitting;

#define DRAW_HISTORY							m_DumpDirectory
#define METHOD									m_MethodDirectory
#define DEBUG_SKIP_UPDATE_PROJECTION_MATRIX		m_bDebugSkipUpdateProjectionMatrix
#define DEBUG_SKIP_MODIFY_RHW     				m_bDebugSkipModifyRHW
#define DEBUG_SKIP_SET_PROJECTION_MATRIX		m_bDebugSkipSetProjection
#define DEBUG_SKIP_SYNCHRONIZE_TEXTURES			m_bDebugSkipSynchronizeTextures
#define DEBUG_SKIP_SYNCHRONIZE_DEPTH_TEXTURES   m_bDebugSkipSynchronizeDepthTextures
#define DEBUG_SKIP_SET_STEREO_RENDERTARGET		m_bDebugSkipSetStereoRenderTarget
#define DEBUG_RENDER_IN_MONO					m_bDebugRenderInMono
#define DEBUG_SKIP_SET_STEREO_TEXTURES			m_bDebugSkipSetStereoTextures
#define DEBUG_SKIP_CRC_CHECK					m_bDebugSkipCheckCRC
#define DEBUG_SKIP_SHADER_MATRIX_ANALYZING		m_bDebugSkipShaderMatrixAnalyzing
#define DEBUG_SKIP_VD_ANALYZING					m_bDebugSkipVDAnalyzing
#define DEBUG_SKIP_TEXTURE_USING_CHECK			m_bDebugSkipTextureUsingCheck
#define DEBUG_RENDER_IN_ONE_RT					m_bDebugRenderInOneRT
#else
#define DRAW_HISTORY							_T("")
#define METHOD									_T("")
#define DEBUG_SKIP_UPDATE_PROJECTION_MATRIX		FALSE
#define DEBUG_SKIP_MODIFY_RHW     				FALSE
#define DEBUG_SKIP_SET_PROJECTION_MATRIX		FALSE
#define DEBUG_SKIP_SYNCHRONIZE_TEXTURES			FALSE
#define DEBUG_SKIP_SYNCHRONIZE_DEPTH_TEXTURES   FALSE
#define DEBUG_SKIP_SET_STEREO_RENDERTARGET		FALSE
#define DEBUG_RENDER_IN_MONO					FALSE
#define DEBUG_SKIP_GET_STEREO_TEXTURES			FALSE
#define DEBUG_SKIP_SET_STEREO_TEXTURES			FALSE
#define DEBUG_SKIP_CRC_CHECK					FALSE
#define DEBUG_SKIP_SHADER_MATRIX_ANALYZING		FALSE
#define DEBUG_SKIP_VD_ANALYZING					FALSE
#define DEBUG_SKIP_TEXTURE_USING_CHECK			FALSE
#define DEBUG_RENDER_IN_ONE_RT					FALSE	
#endif

	ULONG							m_nEventCounter;
	ULONG							m_nDrawCounter;
	ULONG							m_nDrawMonoCounter;
	ULONG							m_nDrawVSCounter;
	ULONG							m_nDrawRHWCounter;
	ULONG							m_nDrawFixedCounter;
	ULONG							m_nDrawHeavyCounter;

	float							m_fCurSessionTime;
	float							m_fTotalMonoTime;
	float							m_fTotalStereoTime;
	LONG							m_nMonoSessionsCounter;
	LONG							m_nStereoSessionsCounter;
	float							m_fMaxMonoTime;
	float							m_fMaxStereoTime;

	HRESULT			DumpSurfaces(TCHAR *comment, IDirect3DSurface9 *pLeft, IDirect3DSurface9 *pRight);
	template <typename T, typename Q>
	void			DumpPipelineTextures(ShaderPipelineData<T, Q>& m_PipelineData, BOOL bRenderTo2RT, const TCHAR* prefix);
	void            DumpPipelineTexture( const TexData& texData, BOOL bRenderTo2RT, const TCHAR* prefix,
		DWORD samplerIndex, const ResourceCRCMultiplier* pCRCMultiplier );
	void			DumpTexturesAndRT(TCHAR* drawName, BOOL bRenderTo2RT, BOOL bHeavy);
	void			DumpAllStuff( BOOL bRenderTo2RT, TCHAR* drawName );
	void			DumpRTSurfaces( BOOL bRenderTo2RT, TCHAR* drawName, TCHAR* szAdditionalData, BOOL bClear );
	void			DumpDSSurfaces( BOOL bRenderTo2RT, TCHAR* drawName, TCHAR* szAdditionalData );

	// only for method
	void			DumpSurface(TCHAR *pComment, IDirect3DSurface9 *pSurface);
	void			DumpTexture(TCHAR *pComment, IDirect3DBaseTexture9 *pTexture);
	HRESULT			DumpTextures(TCHAR *comment, IDirect3DBaseTexture9 *pLeft, IDirect3DBaseTexture9 *pRight);


	void			DrawPerfHUDStatistics	( class CBaseSwapChain* pSwapChain );
	void			InitPerfHUD				();
	void			ReleasePerfHUD			();

	void			SaveStereoSurfaceToFile(TCHAR* szFileName, IDirect3DSurface9* pLeftSurf, IDirect3DSurface9* pRightSurf);
	void            SaveStereoTextureToFile(TCHAR* szFileName, IDirect3DBaseTexture9* pLeftTex, IDirect3DBaseTexture9* pRightTex);
	HRESULT			CaptureAll( vbs *&vb );
	HRESULT			ApplyAll( vbs *&vb );

	const ResourceCRCMultiplier*	GetCRCMultiplier(IDirect3DResource9 *pInterface);
	const ResourceCRCMultiplier*	SetCRCMultiplier(IDirect3DResource9 *pInterface, const ResourceCRCMultiplier *pCRCMultiplier);

	bool							RenderToRight();
	template <VIEWINDEX view>
	inline void						SetStereoRenderTarget( )
	{
		if (view != VIEWINDEX_RIGHT)
			SetStereoRenderTargetLeft();
		else
			SetStereoRenderTargetRight();
	}	
	bool IsWeaponDrawCall() 
	{
		return m_ViewPort.MinZ <= 0.05f && m_ViewPort.MaxZ <= g_ProfileData.WeaponMaxZ;
	}
	void GetMinMaxValues( float& minValue, float& maxValue, DWORD* pHistogram, 
		IDirect3DTexture9* pLeftTexture, IDirect3DTexture9* pRightTexture, bool bSaveToFile );
	void SaveDepthMapToFile( TCHAR * fileName, D3DSURFACE_DESC* pDesc, D3DLOCKED_RECT &lockedRect );
	void							OneToOneRender( IDirect3DTexture9* pTex, CONST RECT* pTexRect, CONST SIZE* pOffset );
	UINT							GetShutterRefreshRate(UINT appRefreshRate);
	CONST TCHAR*					GetModeString();

	CBaseSwapChain* 				GetBaseSC(int i = 0) {
		return (CBaseSwapChain*)m_SwapChains[i];
	}
	bool							IsFullscreen() { return GetBaseSC()->m_PresentationParameters[0].Windowed != TRUE; }

protected:
	CBaseStereoRenderer*			GetBaseDevice() { return this; }; // stub

	CONST SIZE*	                	m_pCurOffset;

	bool							m_bNVPerfHUDInitOk;
	CComPtr<IDirect3DStateBlock9>   m_pSavedState;
	CComPtr<IDirect3DStateBlock9>   m_pTextState;
	CComPtr<IDirect3DVertexShader9> m_pVSFont;
	CComPtr<IDirect3DPixelShader9>  m_pPSFont;

	bool                        	m_bUsedStereoTextures;

	bool							m_bUsingMonoOutput;
	bool							m_bCantFindStereoDevice;
	bool							m_bUpdateAQBSDriver;
	bool							m_bUnsupported3DDevice;

	DX9VertexShaderConverter		m_VertexShaderConverter;
	DX9PixelShaderConverter			m_PixelShaderConverter;

	bool                        	m_CurrentRAMPisSetToDevice;
	bool                        	m_IdentityRAMPisSetToDevice;

	//--- statistics variables ---
	LARGE_INTEGER 					m_nGlobalFrameTimeDelta;
	LARGE_INTEGER 					m_nGlobalLastFrameTime;			//--- time when previous frame was completed ---
	LARGE_INTEGER					m_nFreq; 

	//--- draw FPS related variables ---
	CDynRender*						m_pDynRender;
	CFont*							m_pFont;
	// Modern OSD (replaces the dead m_pFont path which depends on
	// %ALLUSERSPROFILE%\Font\OSDFont.fnt not shipping on current builds).
	// Lazy-created on first DrawInfo call that has a non-null device.
	wiz3d::OSDRenderer9*			m_pOSD = nullptr;
	// Modern laser-sight crosshair (replaces the dead legacy path which
	// depends on %ALLUSERSPROFILE%\resources.zip not shipping either).
	wiz3d::LaserSightRenderer9*		m_pLaserSight = nullptr;
	CComPtr<ID3DXLine>				m_pLine;
	bool							m_bStereoModeChanged;

	//--- draw stereo-camera parameters related variables ---
	LARGE_INTEGER					m_nOSDShowTimeLeft;
	LARGE_INTEGER					m_nWriteSettingsAfter;
	
	//--- stereo pipeline branch variables ---
	DWORD							m_MainThreadId;
	DWORD							m_dwMaxStreams;				// DX caps

	FLOAT							m_StereoZNear;
	bool							m_bIsWorldMatrixIdenty; 
	bool							m_bIsViewMatrixIdenty; 
	D3DXMATRIX						m_WorldMatrix;
	D3DXMATRIX						m_ViewMatrix;
	FLOAT							m_FixedZNear;
	FLOAT							m_FixedZFar;
	D3DXMATRIX						m_ProjectionMatrix;
	D3DXMATRIX						m_VSProjectionMatrix;
	D3DXMATRIX						m_TransformAfterViewportVSMono;
	FLOAT							m_ShaderZNear;
	FLOAT							m_ShaderZFar;
	FLOAT*							m_VertexShaderRegister;
	D3DXMATRIX						m_PSMatrix;
	FLOAT							m_ZNear;
	FLOAT							m_ZFar;
	D3DXMATRIX						m_OldShaderMatrix;	// cached shader matrices 
	bool							m_bOldShaderPerspectiveMatrix; 
	bool							m_bOldShaderOnlyProjectionMatrix; 
	bool							m_bOldShaderInverseZ; 
	FLOAT							m_OldMinZ; 
	FLOAT							m_OldMaxZ; 
	bool                            m_bInverseZ; 
	bool                            m_bOnlyProjectionMatrix; 
	PIPELINE						m_Pipeline;
	bool                            m_bRenderInStereo; 
	bool                            m_bRenderInStereoPS; 
	DWORD*							m_pUpdateRTs;
	bool							m_UpdateDS;
	FLOAT							m_CurrentMeshMultiplier;
	FLOAT							m_CurrentConvergenceShift;
	FLOAT							m_CurrentPSMultiplier;
	FLOAT							m_CurrentPSConvergenceShift;

	std::vector<IDirect3DVertexBuffer9*>		m_pVB;
	std::vector<const ResourceCRCMultiplier*>	m_MeshCRCMultiplier;
	DWORD                           m_dwMaxTextureWidth;
	DWORD                           m_dwMaxTextureHeight;
	ShaderPipelineData<PS_PRIVATE_DATA, IDirect3DPixelShader9>	m_PSPipelineData;
	ShaderPipelineData<VS_PRIVATE_DATA, IDirect3DVertexShader9>	m_VSPipelineData;
	inline D3DXMATRIX&				GetVSMatrix(int i)	{ return (D3DXMATRIX&)m_VertexShaderRegister[m_VSPipelineData.m_CurrentShaderData.matricesData.matrix[i].matrixRegister * 4 + 0]; }

	// Transform Vertex Buffer
	bool							m_bRHWVertexDeclaration;
	WORD							m_dwStreamRHWVertices;
	WORD							m_dwOffsetRHWVertices;
	CComPtr<ILockVBData>			m_pLockVBData;
	CComPtr<IDirect3DVertexBuffer9>	m_pSavedVertexBuffer;
	CComPtr<IDirect3DVertexBuffer9>	m_pModifiedVertexBuffer;
	UINT							m_nSavedOffsetInBytes;
	UINT						    m_nSavedStride;
	BYTE*						    m_pModifiedVertexStreamZeroData;
	UINT							m_ModifiedVertexStreamZeroSize;
	RHWEmulMap						m_RHWEmulMap;
	RHWEmulPair*					m_RHWEmulData;
									
	VIEWINDEX                   	m_CurrentView;
	AsyncButton					    m_ScreenShotButton;
	STEREO_CAMERA               	m_StereoCamera;
	D3DVIEWPORT9                	m_OldViewPort;          //--- for optimized matrix update ---
	bool                        	m_bUpdatedSkewMatrices;
	bool                        	m_bPerspectiveMatrix;
	bool                        	m_bPerspectiveVSMatrix;

	DWORD							m_dwColorWriteEnable[4];
	DWORD							m_dwZWriteEnable;
	DWORD							m_dwStencilEnable;
	DWORD							m_dwScissorTestEnable;

	void							DetectDepthStencilTextureFormat();
#ifdef ZLOG_ALLOW_TRACING
	zlog::SEVERITY                 	m_nOldTrace;
#endif
	CONST TCHAR*	GetViewString( VIEWINDEX view );

	void			CheckRenderInStereo(RHWHEADER *p);
	const void*		TransformPrimitiveUPInit(UINT VertexCount, const void * pVertexStreamZeroData, UINT VertexStreamZeroStride);
	const void*		TransformPrimitiveUP(UINT VertexCount, const void * pVertexStreamZeroData, UINT VertexStreamZeroStride);
	HRESULT			TransformPrimitiveInit(UINT &StartVertex, UINT MinVertexIndex, UINT Count);
	HRESULT			TransformPrimitive(UINT &StartVertex);
	void			TransformVertices(RHWHEADER* pDest, const RHWHEADER* pSrc, UINT nCount, UINT nStride);
	void			RestoreVertices(void);

	void			DumpIndexedMeshesUP(UINT MinIndex, UINT NumIndexes, const void* pIndexData, D3DFORMAT IndexDataFormat, 
						UINT VertexCount, const void* pVertexStreamZeroData, UINT VertexStreamZeroStride);
	void			DumpMeshesUP(UINT VertexCount, const void* pVertexStreamZeroData, UINT VertexStreamZeroStride);
	void			DumpIndexedMeshes( INT BaseVertexIndex, UINT MinVertexIndex, UINT NumIndexes, UINT StartIndex, UINT VertexCount);
	void			DumpMeshes(UINT Start, UINT VertexCount);
	void			DrawInfo(CBaseSwapChain* pSwapChain);
	void			DrawGraph(CBaseSwapChain* pSwapChain);

	virtual void	WriteStatistic();
	void			GetStates(AllStates* states);
	void			PrintStates(AllStates* states);
	void			CheckStates(AllStates* states);

	void			CheckYouTubeTexture( UINT Width, UINT Height, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DTexture9* pTexture );
	void			UpdateYouTubeTexture(DWORD Stage);

	inline UINT		GetCorrectSwapChainIndex(UINT iSwapChain);
	void			HookDevice();
	void			UnhookDevice();

	void			InitVariables();
	HRESULT			StartEngine();

	void			UpdateCursorTexture( HBITMAP hCursorImage );
	void			CreateFont();
	HRESULT			CheckEngine();
	HRESULT			StopEngine();

	void			InitATIShutterMode();
	void			SetupUMPresenter();
	HRESULT			SetATIBltEye( DWORD command, DWORD param );
	bool			GetATILineOffset();
	HRESULT			SendStereoCommand(
		ATIDX9STEREOCOMMAND stereoCommand,
		BYTE* pOutBuffer,
		DWORD dwOutBufferSize, 
		BYTE* pInBuffer , 
		DWORD dwInBufferSize);


	void            UpdateProjectionMatrix(bool bOnlyFP = false);
	__forceinline void		CalculateMatrix( VIEWINDEX viewIndex, const D3DXMATRIX* pStereoProjMatrix, const D3DXMATRIX* pInvProjection );
	__forceinline void		CalculateFPMatrix( VIEWINDEX viewIndex, const D3DXMATRIX* pProjMatrix, bool bRightHanded );
	void			UpdateRHWMatrix();
	__forceinline void		CalculateRHWMatrix( VIEWINDEX viewIndex, const D3DXMATRIX* pViewPortMatrix, const D3DXMATRIX* pInvViewPortMatrix );
	bool			CheckVSMatrices();
	void			CheckVSMatricesTransposed( D3DXMATRIX &CurrentShaderMatrix );
	void			CheckVSMatricesNonTransposed( D3DXMATRIX &CurrentShaderMatrix );

	bool            PrepareViewStages(bool bFromMemory = false);
	void			BeforeDrawCall();
	void			AfterDrawCall();
	bool			CheckDrawPassSplitting( D3DPRIMITIVETYPE PrimitiveType, UINT PrimitiveCount );
	void 			AutoDetectStereoSettings();
	RHWEmulPair*	GetRHWEmulData(IDirect3DVertexDeclaration9* pDecl);
	HRESULT			GenerateRHWEmulVS(D3DVERTEXELEMENT9* pDecl, IDirect3DVertexShader9** pVS, int RHWEmulShaderIndex);
	char*			VertexElementToString( const D3DVERTEXELEMENT9* pDeclElem, UINT i, char* p );
	void			WriteRHWEmulVS(const char* shaderText, const char* asmText, int RHWEmulShaderIndex);

	bool			CheckRenderInStereoVSPipeline();
	bool			CheckRenderInStereoFixedPipeline();
	bool			CheckFixedPipelineMatrices();
	void			CheckRenderInStereoPSPipeline();
	void			CheckVSCRCMultiplier();
	void			CheckPSCRCMultiplier();
	// 0.4.3 <When TextureCRC="0x..."/> sub-filter on DX9: queries bound PS
	// textures (stages 0..15), FNV-1a-hashes each pointer, and returns true
	// if any match a listed CRC in the profile's m_WhenTextureCRCs vector.
	// Returns true when the vector is empty (unfiltered). Called from
	// CheckVSCRCMultiplier / CheckPSCRCMultiplier before applying the
	// profile's m_Multiplier / m_ConvergenceShift.
	bool			TextureMatchesWhenFilter(const class ShaderProfileData& p);
	void			CheckMeshMultiplier();
	void			CheckViewportZRange();

	bool            IsStereoRenderTargetSurface( UINT Width, UINT Height );
	bool            IsStereoDepthStencilSurface( UINT Width, UINT Height );
	bool            IsStereoDepthStencilTexture( UINT Width, UINT Height );
	bool            IsStereoRenderTargetTexture( D3DFORMAT Format, DWORD Usage, UINT Width, UINT Height );

	void			UpdateTexturesType(TextureType textureType);
	bool			IsStereoRender();
	void			UpdateStatisticData();

	DWORD			CalculateSurfaceCRC( D3DSURFACE_DESC* pDesc, IDirect3DSurface9* pSurface, CONST RECT* pSourceRect );
	DWORD			CalculateVBCRC( IDirect3DVertexBuffer9* pVB, D3DVERTEXBUFFER_DESC* pDesc );
	HRESULT			CRCMeshCheck(UINT StreamNumber, IDirect3DVertexBuffer9 * pStreamData);
	bool			NeedCalculateMeshCRC(DWORD Size, ResourcesCRCMultiplierMapBySize::const_iterator &iter);
	template <typename T, typename Q>
	HRESULT			CRCTextureCheck(ShaderPipelineData<T, Q>& pipelineData, DWORD Sampler, IDirect3DBaseTexture9 * pTexture);
	bool			NeedCalculateTextureCRC(DWORD Size, ResourcesCRCMultiplierMapBySize::const_iterator &iter);
	const ResourceCRCMultiplier* CRCTextureFromSurfaceCheck( IDirect3DSurface9 * pSrcSurf, CONST RECT* pSourceRect, IDirect3DBaseTexture9 * pDestTex );


	template <VIEWINDEX view>
	void            SetViewStages();
	template <VIEWINDEX view>
	HRESULT			ModifyVSMatricesWVP();
	template <VIEWINDEX view>
	HRESULT			ModifyVSMatricesP();
	HRESULT			SetMonoModifiedShaderConstants( );
	template <VIEWINDEX view>
	HRESULT			SetModifiedShaderConstants();
	template <VIEWINDEX view>
	void			SetStereoTextures();
	void			SetMonoViewStage();
	bool			ShadowFormat( D3DFORMAT Format );
	bool			NeedDumpMeshes( UINT PrimitiveCount, D3DPRIMITIVETYPE PrimitiveType );
	bool			IsSquareSize( UINT Width, UINT Height, BOOL CreateBigInStereo );
	bool			IsLessThanBB( UINT Width, UINT Height );
	void            GetViewportOffset(FLOAT &x, FLOAT &y); // RHW Offset
	void			FlushCommands( );

	virtual HRESULT StartEngineMode() = 0;
	virtual void    StopEngineMode() = 0;
	virtual void    GetStereoSurfRects(IDirect3DSurface9* pLeft, IDirect3DSurface9* &pRight, D3DFORMAT &format, RECT *pLeftRect, RECT *pRightRect) = 0;

	virtual	void	ViewStagesSynchronization() = 0;
	virtual bool    RenderToTextures() = 0;
	virtual bool    RenderInStereo() = 0;
	virtual bool    StereoDS() = 0;
	virtual void	SetStereoRenderTargetLeft() = 0;
	virtual void	SetStereoRenderTargetRight() = 0;  	

	// for wide only
	bool			CloneDepthSurfaces(IDirect3DTexture9* pDepthTexure, CComPtr<IDirect3DSurface9> &pLeft, CComPtr<IDirect3DSurface9> &pRight);
	virtual void	SynchronizeDepthBuffer( CBaseSwapChain* pSwapChain ) { }
	virtual bool	CopyDepthSurfaces(IDirect3DTexture9* pDepthTexure, CComPtr<IDirect3DSurface9> &pLeft, CComPtr<IDirect3DSurface9> &pRight) { return false; }

	void UpdateFrameStatistic( CBaseSwapChain* pSwapChain );
	void StereoModeChanged(bool bNewMode);
	HRESULT DoPresent( CBaseSwapChain* pSwapChain, HWND hDestWindowOverride, CONST RECT * pSourceRect, CONST RECT * pDestRect, 
		CONST RGNDATA * pDirtyRegion, DWORD dwFlags = 0, bool bExMode = false );

	void DrawMouseCursor( CBaseSwapChain* pSwapChain );
	void DrawLogo( CBaseSwapChain* pSwapChain );
	HRESULT DoReset( D3DPRESENT_PARAMETERS* pPresentationParameters, 
		D3DDISPLAYMODEEX * pFullscreenDisplayMode = NULL, bool bExMode = false );
	void	CreateOutput();
	virtual CMonoSwapChain* CreateSwapChainWrapper(UINT index) = 0;

	//for draw pass splitting
	HRESULT	DrawPrimitiveSplitted(D3DPRIMITIVETYPE PrimitiveType,UINT StartVertex,UINT PrimitiveCount);
	HRESULT	DrawIndexedPrimitiveSplitted(D3DPRIMITIVETYPE,INT BaseVertexIndex,UINT MinVertexIndex,UINT NumVertices,UINT startIndex,UINT primCount);
	HRESULT	DrawPrimitiveUPSplitted(D3DPRIMITIVETYPE PrimitiveType,UINT PrimitiveCount,CONST void* pVertexStreamZeroData,UINT VertexStreamZeroStride);
	HRESULT	DrawIndexedPrimitiveUPSplitted(D3DPRIMITIVETYPE PrimitiveType,UINT MinVertexIndex,UINT NumVertices,UINT PrimitiveCount,CONST void* pIndexData,D3DFORMAT IndexDataFormat,CONST void* pVertexStreamZeroData,UINT VertexStreamZeroStride);


public:
	CBaseStereoRenderer();
	virtual ~CBaseStereoRenderer();

	virtual HRESULT DoInitialize();
	void	SetDirect3DVersion(UINT D3DVersion) { m_OrigD3DVersion = D3DVersion; }

	//--- RouterType = ROUTER_TYPE_HOOK case functions ----
	void HookIDirect3DDevice9All();
	void UnHookIDirect3DDevice9All();

	bool IsCurrentThreadMain();
	void FindBestFullScreenResolution(UINT &Width, UINT &Height, D3DFORMAT Format);
	void FindNativeFullScreenResolution();
	D3DFORMAT GetDisplayFormatFromBackBufferFormat(D3DFORMAT backBufferFormat);	

	// Saved states
	D3DVIEWPORT9				m_ViewPort;
	RECT						m_ScissorRect;
	SIZE						m_PrimaryRTSize;
	bool						m_bFullSizeViewport;
	bool						m_bFullSizeScissorRect;
	DataInput					m_Input;

	/*** IUnknown methods ***/
	STDMETHOD_(ULONG,Release)(THIS);
	/*** IDirect3DDevice9 methods ***/
	STDMETHOD(Reset)(THIS_ D3DPRESENT_PARAMETERS* pPresentationParameters);
	STDMETHOD(Present)(THIS_ CONST RECT* pSourceRect,CONST RECT* pDestRect,HWND hDestWindowOverride,CONST RGNDATA* pDirtyRegion);
	STDMETHOD(TestCooperativeLevel)(void);

	STDMETHOD(GetBackBuffer)(THIS_ UINT iSwapChain,UINT iBackBuffer,D3DBACKBUFFER_TYPE Type,IDirect3DSurface9** ppBackBuffer);
	STDMETHOD_(void, SetGammaRamp)(THIS_ UINT iSwapChain,DWORD Flags,CONST D3DGAMMARAMP* pRamp);
	STDMETHOD_(void, GetGammaRamp)(THIS_ UINT iSwapChain,D3DGAMMARAMP* pRamp);
	STDMETHOD(CreateVertexBuffer)(THIS_ UINT Length,DWORD Usage,DWORD FVF,D3DPOOL Pool,IDirect3DVertexBuffer9** ppVertexBuffer,HANDLE* pSharedHandle);
	STDMETHOD(DrawPrimitive)(THIS_ D3DPRIMITIVETYPE PrimitiveType,UINT StartVertex,UINT PrimitiveCount);
	STDMETHOD(DrawIndexedPrimitive)(THIS_ D3DPRIMITIVETYPE,INT BaseVertexIndex,UINT MinVertexIndex,UINT NumVertices,UINT startIndex,UINT primCount);
	STDMETHOD(DrawPrimitiveUP)(THIS_ D3DPRIMITIVETYPE PrimitiveType,UINT PrimitiveCount,CONST void* pVertexStreamZeroData,UINT VertexStreamZeroStride);
	STDMETHOD(DrawIndexedPrimitiveUP)(THIS_ D3DPRIMITIVETYPE PrimitiveType,UINT MinVertexIndex,UINT NumVertices,UINT PrimitiveCount,CONST void* pIndexData,D3DFORMAT IndexDataFormat,CONST void* pVertexStreamZeroData,UINT VertexStreamZeroStride);
	STDMETHOD(CreateVertexShader)(THIS_ CONST DWORD* pFunction,IDirect3DVertexShader9** ppShader);
	STDMETHOD(DrawRectPatch)(THIS_ UINT Handle,CONST float* pNumSegs,CONST D3DRECTPATCH_INFO* pRectPatchInfo);
	STDMETHOD(DrawTriPatch)(THIS_ UINT Handle,CONST float* pNumSegs,CONST D3DTRIPATCH_INFO* pTriPatchInfo);
	STDMETHOD(CreatePixelShader)(THIS_ CONST DWORD* pFunction,IDirect3DPixelShader9** ppShader) ;

	STDMETHOD(SetCursorProperties)(THIS_ UINT XHotSpot,UINT YHotSpot,IDirect3DSurface9* pCursorBitmap) ;
	STDMETHOD_(void, SetCursorPosition)(THIS_ int X,int Y,DWORD Flags) ;
	STDMETHOD_(BOOL, ShowCursor)(THIS_ BOOL bShow) ;
	STDMETHOD(CreateAdditionalSwapChain)(THIS_ D3DPRESENT_PARAMETERS* pPresentationParameters,IDirect3DSwapChain9** pSwapChain) ;
	STDMETHOD_(UINT, GetNumberOfSwapChains)(THIS) ;
	STDMETHOD(CreateVolumeTexture)(THIS_ UINT Width,UINT Height,UINT Depth,UINT Levels,DWORD Usage,D3DFORMAT Format,D3DPOOL Pool,IDirect3DVolumeTexture9** ppVolumeTexture,HANDLE* pSharedHandle) ;
	STDMETHOD(SetTransform)(THIS_ D3DTRANSFORMSTATETYPE State,CONST D3DMATRIX* pMatrix) ;
	STDMETHOD(MultiplyTransform)(THIS_ D3DTRANSFORMSTATETYPE,CONST D3DMATRIX*) ;
	STDMETHOD(SetViewport)(THIS_ CONST D3DVIEWPORT9* pViewport) ;
	STDMETHOD(SetRenderState)(THIS_ D3DRENDERSTATETYPE State,DWORD Value) ;
	STDMETHOD(SetScissorRect)(THIS_ CONST RECT* pRect) ;
	STDMETHOD(SetVertexDeclaration)(THIS_ IDirect3DVertexDeclaration9* pDecl) ;
	STDMETHOD(SetFVF)(THIS_ DWORD FVF) ;
	STDMETHOD(SetVertexShader)(THIS_ IDirect3DVertexShader9* pShader) ;
	STDMETHOD(GetVertexShader)(THIS_ IDirect3DVertexShader9** ppShader) ;
	STDMETHOD(SetVertexShaderConstantF)(THIS_ UINT StartRegister,CONST float* pConstantData,UINT Vector4fCount) ;
	STDMETHOD(GetVertexShaderConstantF)(THIS_ UINT StartRegister,float* pConstantData,UINT Vector4fCount) ;
	STDMETHOD(SetStreamSource)(THIS_ UINT StreamNumber,IDirect3DVertexBuffer9* pStreamData,UINT OffsetInBytes,UINT Stride) ;
	STDMETHOD(SetPixelShader)(THIS_ IDirect3DPixelShader9* pShader) ;
	STDMETHOD(GetPixelShader)(THIS_ IDirect3DPixelShader9** ppShader) ;
	STDMETHOD(CreateQuery)(THIS_ D3DQUERYTYPE Type, IDirect3DQuery9** ppQuery);
	STDMETHOD(GetDisplayMode)(THIS_ UINT iSwapChain, D3DDISPLAYMODE* pMode);

	/*** IDirect3DDevice9Ex methods ***/
	STDMETHOD(PresentEx)(THIS_ CONST RECT* pSourceRect,CONST RECT* pDestRect,HWND hDestWindowOverride,CONST RGNDATA* pDirtyRegion,DWORD dwFlags);
	STDMETHOD(ResetEx)(THIS_ D3DPRESENT_PARAMETERS* pPresentationParameters,D3DDISPLAYMODEEX *pFullscreenDisplayMode);
	STDMETHOD(GetDisplayModeEx)(THIS_ UINT iSwapChain, D3DDISPLAYMODEEX* pMode, D3DDISPLAYROTATION* pRotation);

	//--- IDirect3DStateBlock9 hooking method ---
	STDMETHOD(StateBlock_Apply)(IDirect3DStateBlock9* pBlock);
	friend HRESULT __stdcall Proxy_StateBlock_Apply(IDirect3DStateBlock9* pBlock);

#ifndef FINAL_RELEASE
	STDMETHOD(StateBlock_Capture)(IDirect3DStateBlock9* pBlock);
	friend HRESULT __stdcall Proxy_StateBlock_Capture(IDirect3DStateBlock9* pBlock);
#endif // FINAL_RELEASE

	//--- GDI device RAMP ---
	STDMETHOD_(BOOL, SetDeviceGammaRamp)(THIS_ CONST D3DGAMMARAMP *pRamp);
	friend CPresenter;
	friend CPresenterSCData;
	friend CBaseSwapChain;
};

class SetUMEvent
{
public:
	SetUMEvent(CBaseStereoRenderer* pRenderer, RendererEvents eventType)
	{
		if(eventType == reCreatingDevice)
		{	
			g_pUMDeviceWrapper = NULL;
			g_pCurrentStereoRenderer = pRenderer;
		}
		pRenderer->m_RendererEvents = eventType;
		m_pRenderer = pRenderer;
	}

	~SetUMEvent()
	{
		if(m_pRenderer->m_RendererEvents == reCreatingDevice)
		{
			m_pRenderer->m_pUMDeviceWrapper = g_pUMDeviceWrapper;
			//--- just for safety ---
			g_pUMDeviceWrapper = NULL;
			//--- Disable user-mode wrapper creating for NULLREF and SW devices ---
			g_pCurrentStereoRenderer = NULL;	
		}
		m_pRenderer->m_RendererEvents = reNone;
	}

protected:
	CBaseStereoRenderer* m_pRenderer;
};

#include "BaseStereoRenderer-inl.h"
#include "BaseSwapChain-inl.h"
