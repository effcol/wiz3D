//

#include "stdafx.h"
#include "../../CommonUtils/CommonResourceFolders.h"
#include "..\S3DAPI\GlobalData.h"
#include "..\S3DAPI\ReadData.h"
#include "..\S3DAPI\ShutterAPI.h"
#include "..\Shared\OutputModeNames.h"
#include "D3DDeviceWrapper.h"
#include "Commands\CommandSet.h"
#include "DXGISwapChainWrapper.h"
#include "StereoResourceWrapper.h"
#include "../OutputMethods/OutputLib/MonoOutput_dx10.h"
#include "Presenter.h"

#include "DepthStencilViewWrapper.h"
#include "RenderTargetViewWrapper.h"
#include "ShaderResourceViewWrapper.h"
#include "UnorderedAccessViewWrapper.h"
#include "AdapterFunctions.h"

#include "MonoCommandBuffer.h"
#include "StereoCommandBuffer.h"

#include "acgfx_dynrender_dx10um.h"
#include "../Font/acgfx_font.h"
#include "../DX10SharedLibrary/WizardDrawer.h"
#include "../uilib/WizardSCData.h"

#include "./QbsHeaders/amddxextapi.h"
#include "./QbsHeaders/AmdDxExtQbStereoApi.h"

#include "../CommonUtils/System.h"
#include "madCHook.h"

#if defined _WIN64
	const TCHAR*	g_sAQBSDll				= _T("atidxx64.dll");
#else
	const TCHAR*	g_sAQBSDll				= _T("atidxx32.dll");
#endif

using namespace DX10Output;

TLS D3DDeviceWrapper*	g_pLastD3DDevice = NULL;

HMODULE						D3DDeviceWrapper::m_hAQBSModule				= NULL;
IAmdDxExt*					D3DDeviceWrapper::m_pAmdExt					= NULL;
IAmdDxExtQuadBufferStereo*	D3DDeviceWrapper::m_pAmdStereo				= NULL;
IDXGIDevice*				D3DDeviceWrapper::m_pAQBSDXGIDevice			= NULL;

bool D3DDeviceWrapper::m_bUpdateAQBSDriver		= false;
bool D3DDeviceWrapper::m_bUnsupported3DDevice	= false;

#ifndef FINAL_RELEASE

BOOL (WINAPI *origTerminateProcess)(HANDLE hProcess,UINT uExitCode);
BOOL WINAPI myTerminateProcess(HANDLE hProcess,UINT uExitCode)
{
	DEBUG_TRACE1(L"TerminateProcess hook!\n");

	// call destructors & cleanup for games which directly call TerminateProcess
	//   e.g. metro2033	
	if (hProcess == GetCurrentProcess()) _cexit();

	return origTerminateProcess(hProcess,uExitCode);
}

#endif // FINAL_RELEASE

D3DDeviceWrapper::D3DDeviceWrapper()
:	m_FrameIndex(0), m_pTempSwapChain(NULL), m_pLastSwapChain(NULL), m_pDXGIDevice(NULL)
,   m_bFirstSwapChainCreated(false)
,	m_bUsingMonoOutput      (false)
,   m_pOutputMethod         (NULL)
,	m_MonoBuffer			(new MonoCommandBuffer)
,	m_StereoBuffer			(new StereoCommandBuffer)
,   m_bIsLastRTStereo       (false)
,	m_MainThreadId			( 0 )
,	m_pFont					( NULL )
,	m_pDynRender			( NULL )
,	m_bStereoModeChanged	( true )
,	m_bCantFindStereoDevice	( false )
,	m_pECBuffer				( 0 )
,	m_nFlushCounter			( 0 )
,	m_nGetDataCounter		( 0 )
,	m_PresenterFlushTime	( 1 )
,	m_bFlushCommands		( false )
#ifndef FINAL_RELEASE
,	m_DumpType              (dtNone)
,   m_bDebugUseSimplePresenter(false)
#endif
{
	DEBUG_TRACE1(_T("Created device\n"));
#ifndef FINAL_RELEASE
	m_MonoCommandsCount = 0;
	m_StereoCommandsCount = 0;
	m_SetRenderTargetCount = 0;
	m_DrawCount = 0;
	m_StereoDrawCount = 0;
	m_StereoDrawAutoCount = 0;
	m_StereoDrawIndexedCount = 0;
	m_StereoDrawInstancedCount = 0;
	m_StereoDrawIndirectCount = 0;
	m_StereoBuffersCount = 0;
	m_CBCount = 0;
	m_CBSubIndexCount = 0;
#endif
	m_MaxBufferSize = 0;
	m_FlushTime.QuadPart = 0;

	if (gData.VendorId == 0)
	{
		WE_CALL;
		CComPtr<IDXGIFactory> pFactory;
		CreateDXGIFactory(__uuidof(IDXGIFactory), (void**)(&pFactory));
		if (pFactory)
		{
			CComPtr<IDXGIAdapter> pAdapter;
			if (SUCCEEDED(pFactory->EnumAdapters(0, &pAdapter)) && pAdapter)
			{
				DXGI_ADAPTER_DESC desc;
				pAdapter->GetDesc(&desc);
				gData.VendorId = desc.VendorId;
			}
		}
	}

	ReadCurrentProfile(gData.VendorId);

	m_StereoBuffer->Init( this );
	m_MonoBuffer->Init( this );
	m_MonoBuffer->Begin();

	UpdateProjectionMatrix();

	//--- read camera setting and initialize keyboard read thread ---
	m_Input = gInfo.Input;
	m_Input.StereoActive |= gInfo.EnableStereo;
	gKbdHook.initialize(&m_Input);
	m_DeviceMode = (DeviceModes)gInfo.DeviceMode;
	m_bTwoWindows = false;

	m_MainThreadId = GetCurrentThreadId();

	m_nOSDShowTimeLeft.QuadPart			= 0;
	m_nWriteSettingsAfter.QuadPart		= 0;
	m_nGlobalFrameTimeDelta.QuadPart	= 0;
	m_nGlobalLastFrameTime.QuadPart		= 0;	
	QueryPerformanceFrequency( &m_nFreq );
	m_nScreenShotCounter				= 0;
	m_ScreenShotButton.selectButton( gInfo.HotKeyList[ SCREENSHOT ].code );

	m_tViewPort.TopLeftX = 0.0f;
	m_tViewPort.TopLeftY = 0.0f;
	m_tViewPort.MinDepth = 0.0f;
	m_tViewPort.MaxDepth = 1.0f;
	
	//--- vendor specific setup ---
	{
		// Buffering setup
		if ( gInfo.CommandBuffering ) 
		{
			DEBUG_MESSAGE(_T("Advanced command buffering enabled\n"));
			m_BufferingOnlyBuffers	= true;
			m_BufferingOnlyCB		= true;
			m_BufferingMaxSize		= 8192;
			m_BufferingMinSizeUSU	= 256;
			m_BufferingMaxSizeUSU	= 1 << 23; // ~8mb

			m_ConstantBufferAllocator.reallocate(m_BufferingMinSizeUSU, m_BufferingMaxSizeUSU);
		}
		else
		{
			DEBUG_MESSAGE(_T("Advanced command buffering disabled\n"));
			m_BufferingOnlyBuffers	= false;
			m_BufferingOnlyCB		= false;
			m_BufferingMaxSize		= 0;
			m_BufferingMinSizeUSU	= 0;
			m_BufferingMaxSizeUSU	= 0;
		}
	}

#ifndef FINAL_RELEASE
	static int hooked = 0;
	if (!hooked)
	{
		hooked = 1;
		HookCode(TerminateProcess,myTerminateProcess,(void **)&origTerminateProcess,HOOKING_FLAG);
	}
#endif // FINAL_RELEASE
}

D3DDeviceWrapper::~D3DDeviceWrapper()
{
#ifndef DISABLE_WIZARD
	m_Wizard.Clear();
#endif
	m_HotKeyOSD.Uninitialize();
#ifndef FINAL_RELEASE
	m_StereoBuffer->DumpStatistics();
#endif
	WriteInputData(&m_Input);
	gKbdHook.clear();
	SAFE_DELETE( m_pECBuffer );
	CPresenterX::Delete();
	if (m_pOutputMethod)
	{
		m_pOutputMethod->Clear(this);
		delete m_pOutputMethod;
	}
	if (m_pTempSwapChain)
		delete m_pTempSwapChain;
	g_pLastD3DDevice = NULL;
	DEBUG_TRACE1(_T("Removed device\n"));

	m_LaserSight.Clear();
	SAFE_DELETE( m_pFont );
	SAFE_DELETE( m_pDynRender );

	g_KMShutter.StopShutter();
}

#ifdef SMALL_OBJECT_ALLOCATOR_STATISTICS
template<typename AllocatorType>
void PrintAllocationStatistics(std::basic_ostream<TCHAR>& os, const AllocatorType& alloc)
{
	for (size_t i = 0; i<alloc.num_block_allocators(); ++i) 
	{
		os << "\t\tblock_size: " << alloc.get_block_allocator(i).block_size() 
		   << "; num_allocations: " << alloc.get_block_allocator(i).num_allocations() 
		   << "; max_num_active_allocations: " << alloc.get_block_allocator(i).max_num_active_allocations() << std::endl;
	}

	os << "\t\tNum excess allocations: " << alloc.num_excess_allocations() << std::endl
	   << "\t\tMin allocation: " << alloc.min_allocation() << std::endl
	   << "\t\tMax allocation: " << alloc.max_allocation() << std::endl;
}

#endif // SMALL_OBJECT_ALLOCATOR_STATISTICS

#ifndef FINAL_RELEASE
void D3DDeviceWrapper::LogStatistics()
{
#ifdef SMALL_OBJECT_ALLOCATOR_STATISTICS
	std::basic_ostringstream<TCHAR> ss;
	ss << "Handle allocation statistics(size, allocations): \n";
	{
		ss << "\txxSetConstantBuffers:\n";
		PrintAllocationStatistics(ss, Commands::xxSetConstantBuffers::allocator);
		ss << "\txxSetShaderResources:\n";
		PrintAllocationStatistics(ss, Commands::xxSetShaderResources::allocator);
		ss << "\tIaSetVertexBuffers:\n";
		PrintAllocationStatistics(ss, Commands::IaSetVertexBuffers::allocator);
		ss << "\txxSetSamplers:\n";
		PrintAllocationStatistics(ss, Commands::xxSetSamplers::allocator);
		ss << "\txxSetRenderTargets:\n";
		PrintAllocationStatistics(ss, Commands::xxSetRenderTargets::allocator);
		ss << "\tSetViewports:\n";
		PrintAllocationStatistics(ss, Commands::SetViewports::viewportAllocator);
		ss << "\tSetScissorRects:\n";
		PrintAllocationStatistics(ss, Commands::SetScissorRects::rectAllocator);
		ss << "\tSoSetTargets:\n";
		PrintAllocationStatistics(ss, Commands::SoSetTargets::handleAllocator);
	}
	ZLOG_MESSAGE(zlog::SV_NOTICE, ss.str().c_str());
#endif // SMALL_OBJECT_ALLOCATOR_STATISTICS
}
#endif // FINAL_RELEASE

UINT D3DDeviceWrapper::AQBSGetLineOffset( D3DSwapChain* pSwapChain_ )
{
	UINT nOffset = 0;	
	if ( m_pAmdStereo && pSwapChain_ )
		nOffset = m_pAmdStereo->GetLineOffset( pSwapChain_->m_pSwapChain );
	
	return nOffset;
}

HRESULT	D3DDeviceWrapper::FindShutterClosestMatchingMode( IDXGIDevice *pDXGIDevice, DXGI_MODE_DESC* pMode_, IDXGIOutput* pDXGIOutput )
{
	WE_CALL;

	_ASSERT( pMode_ );

	HRESULT			hResult			= E_FAIL;
	CComPtr<IDXGIOutput>	pDeviceDXGIOutput;

	if (!pDXGIOutput && pDXGIDevice)
	{
		CComPtr<IDXGIAdapter> pDXGIAdapter;
		if (SUCCEEDED(pDXGIDevice->GetAdapter(&pDXGIAdapter)))
		{
			pDXGIAdapter->EnumOutputs(0, &pDeviceDXGIOutput);
			pDXGIOutput = pDeviceDXGIOutput;
		}
	}

	if ( pDXGIOutput )
	{
		DXGI_MODE_DESC modeDesc = *pMode_;
		modeDesc.RefreshRate.Numerator				= 120;
		modeDesc.RefreshRate.Denominator			= 1;
		modeDesc.ScanlineOrdering					= DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED;

		if ( g_pLastD3DDevice )
		{
			DX10Output::OutputMethod* pOutputMethod = g_pLastD3DDevice->GetOutputMethod();
			if ( pOutputMethod )
				modeDesc.RefreshRate.Numerator	= pOutputMethod->GetPreferredRefreshRate();					
		}

		//////////////////////////////////////////////////////////////////////////
		//	we are searching appropriate display mode in the following order:
		//	ScanlineOrdering 
		//	Scaling 
		//	Format 
		//	Resolution 
		//	RefreshRate

		DXGI_MODE_DESC prev = modeDesc;
		memset(&prev, 0, sizeof(prev));
		while ( memcmp(&prev, pMode_, sizeof(prev)) != 0 && SUCCEEDED( pDXGIOutput->FindClosestMatchingMode( &modeDesc, pMode_, pDXGIDevice )) )
		{
			if (modeDesc.ScanlineOrdering != pMode_->ScanlineOrdering)
				modeDesc.ScanlineOrdering = pMode_->ScanlineOrdering;
			else if (modeDesc.Scaling != pMode_->Scaling)
				modeDesc.Scaling = pMode_->Scaling;
			else if (modeDesc.Format != pMode_->Format)
				modeDesc.Format = pMode_->Format;
			else if (modeDesc.Width	!= pMode_->Width || modeDesc.Height != pMode_->Height)
			{
				modeDesc.Width	= pMode_->Width;
				modeDesc.Height = pMode_->Height;
			}
			prev = *pMode_;
		}

		hResult = S_OK;
		//////////////////////////////////////////////////////////////////////////
	}

	return hResult;
}

template <typename T>
bool D3DDeviceWrapper::OpenAMDStereoInterfaceX(  IDXGIDevice* pDXGIDevice, T* pDevice_, const char* pFncName  )
{
	HRESULT hRes	= S_OK;

	m_pAQBSDXGIDevice = pDXGIDevice;	
	m_hAQBSModule = LoadLibrary( g_sAQBSDll );

	if ( !m_hAQBSModule )
	{
		const TCHAR* szError = GetErrorMessage( GetLastError(), FormatStr( _T("LoadLibrary( \"%s\" )"), g_sAQBSDll ) );
		DEBUG_TRACE1(_T("D3DDeviceWrapper::OpenAMDStereoInterface() error: %s.\n"), szError );
		return false;
	}

	typedef HRESULT (__cdecl *PFNAmdDxExtCreateX)(T* pDevice, IAmdDxExt** ppExt);
	PFNAmdDxExtCreateX pfnAmdDxExtCreate = NULL;
	pfnAmdDxExtCreate = reinterpret_cast< PFNAmdDxExtCreateX >( GetProcAddress( m_hAQBSModule, pFncName ));

	if ( !pfnAmdDxExtCreate )
	{
		DEBUG_TRACE1( _T("D3DDeviceWrapper::OpenAMDStereoInterface() error: failed GetProcAddress( \"AmdDxExtCreate\" ) \n") );
		FreeLibrary(m_hAQBSModule);
		m_hAQBSModule = NULL;
		return false;
	}
	
	hRes = pfnAmdDxExtCreate( pDevice_, &m_pAmdExt );

	if ( hRes != S_OK )
	{
		m_bUpdateAQBSDriver = true;
		DEBUG_TRACE1(_T("D3DDeviceWrapper::OpenAMDStereoInterface() error: can't create amd extension.\n") );
		FreeLibrary(m_hAQBSModule);
		m_hAQBSModule = NULL;
		return false;
	}

	AmdDxExtVersion tExtVersion;
	hRes = m_pAmdExt->GetVersion( &tExtVersion );

	if ( hRes == S_OK )	
		DEBUG_TRACE1( _T("D3DDeviceWrapper::OpenAMDStereoInterface() AMD extension version: %d.%d\n"), tExtVersion.majorVersion, tExtVersion.minorVersion );

	m_pAmdStereo = static_cast<IAmdDxExtQuadBufferStereo*>( m_pAmdExt->GetExtInterface( AmdDxExtQuadBufferStereoID ) );

	if ( !m_pAmdStereo )
	{
		m_bUpdateAQBSDriver = true;
		DEBUG_TRACE1(_T("D3DDeviceWrapper::OpenAMDStereoInterface() error: can't create amd stereo. Probably device ID not supported by videordiver.\n") );
		SAFE_FINAL_RELEASE(m_pAmdExt);
		FreeLibrary(m_hAQBSModule);
		m_hAQBSModule = NULL;
		return false;
	}

	if ( FAILED( m_pAmdStereo->EnableQuadBufferStereo( TRUE ) ) )
	{
		m_bUnsupported3DDevice = true;
		DEBUG_TRACE1(_T("D3DDeviceWrapper::OpenAMDStereoInterface() error: EnableQuadBufferStereo failed.\n") );
		return false;
	}

	DEBUG_TRACE1(_T("D3DDeviceWrapper::OpenAMDStereoInterface(): amd stereo interface successfully initialized.\n") );

	return true;
}

bool D3DDeviceWrapper::OpenAMDStereoInterface(  IDXGIDevice* pDXGIDevice, ID3D10Device* pDevice_ )
{
	return OpenAMDStereoInterfaceX(pDXGIDevice, pDevice_, "AmdDxExtCreate");
}

bool D3DDeviceWrapper::OpenAMDStereoInterface(  IDXGIDevice* pDXGIDevice, ID3D11Device* pDevice_ )
{
	return OpenAMDStereoInterfaceX(pDXGIDevice, pDevice_, "AmdDxExtCreate11");
}

void D3DDeviceWrapper::CloseAMDStereoInterface()
{
	m_pAQBSDXGIDevice = NULL;

	SAFE_FINAL_RELEASE(m_pAmdStereo);
	SAFE_FINAL_RELEASE(m_pAmdExt);

	if ( m_hAQBSModule )
	{
		FreeLibrary( m_hAQBSModule );
		m_hAQBSModule = NULL;
	}

	DEBUG_TRACE1(_T("D3DDeviceWrapper::CloseAMDStereoInterface().\n") );
}

void D3DDeviceWrapper::CreateD3DSwapChain()
{
	_ASSERT(g_SwapChainMode == scCreating);
	DEBUG_TRACE1(_T("Add app backbuffer (creating)\n"));
	g_pLastD3DDevice = this;
	if (!m_pTempSwapChain) // if create several bb before call MapDXGISwapChain
		m_pTempSwapChain = DNew D3DSwapChain(this);
	InitializeSwapChainRelatedData();
}

bool D3DDeviceWrapper::IsCurrentThreadMain()
{
	bool bResult = (m_MainThreadId == GetCurrentThreadId());
	return bResult;
}

void D3DDeviceWrapper::UpdateCBMatrices( bool bNewMode )
{
	m_bIsLastRTStereo = bNewMode;

	Commands::GsSetConstantBuffers* gsSetConstantBuffers = (Commands::GsSetConstantBuffers*)m_DeviceState.GS.m_ConstantBuffers.get();
	if (gsSetConstantBuffers->IsUsedStereoResources(this))
		gsSetConstantBuffers->Execute(this);
	Commands::DsSetConstantBuffers11_0* dsSetConstantBuffers = (Commands::DsSetConstantBuffers11_0*)m_DeviceState.DS.m_ConstantBuffers.get();
	if (dsSetConstantBuffers && dsSetConstantBuffers->IsUsedStereoResources(this))
		dsSetConstantBuffers->Execute(this);
	Commands::VsSetConstantBuffers* vsSetConstantBuffers = (Commands::VsSetConstantBuffers*)m_DeviceState.VS.m_ConstantBuffers.get();
	if (vsSetConstantBuffers->IsUsedStereoResources(this))
		vsSetConstantBuffers->Execute(this);
}

void D3DDeviceWrapper::StereoModeChanged( bool bNewMode )
{
	if (bNewMode)
		Commands::Command::CurrentView_ = VIEWINDEX_MONO;
	else
		Commands::Command::CurrentView_ = VIEWINDEX_LEFT;
	UpdateCBMatrices(bNewMode);
}

void D3DDeviceWrapper::CheckEngine( )
{
#ifndef FINAL_RELEASE
	DEBUG_TRACE2(_T("Stereo %d, Mono %d, Ratio %f\n"), m_StereoCommandsCount, m_MonoCommandsCount, 
		m_MonoCommandsCount > 0 ? 1.0f * m_StereoCommandsCount / m_MonoCommandsCount : 1.0f);
	DEBUG_TRACE2(_T("Draw %d (StereoDraw %d, Auto %d, Indexed %d, Instanced %d, Indirect %d)\n"),
		m_DrawCount, m_StereoDrawCount, m_StereoDrawAutoCount, m_StereoDrawIndexedCount, 
		m_StereoDrawInstancedCount, m_StereoDrawIndirectCount);
	DEBUG_TRACE2(_T("StereoBuffer %d, Average %f, SetRenderTarget %d\n"),  
		m_StereoBuffersCount, 1.0f * m_DrawCount / std::max(m_StereoBuffersCount, 1), m_SetRenderTargetCount);

	if(m_DumpType >= dtCommands || 
		(m_DumpType == dtTrace && IsKeyDown(VK_PAUSE) && IsKeyDown(VK_SHIFT)))
	{
		Beep(200, 300);
		if (m_DumpType >= dtCommands)
			zlog::SetupSeverity(m_nOldTrace);
		else
		{
			m_nOldTrace = zlog::SV_NOTICE;
			zlog::SetupSeverity(zlog::SV_NOTICE);
		}
		LogStatistics();
		m_StereoBuffer->DumpStatistics();
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
			if (gData.DumpIndex == 1)
				_stprintf_s(m_DumpDirectory, L"%s\\%s", gData.DumpDirectory, L"Draw History");
			else
				_stprintf_s(m_DumpDirectory, L"%s\\%s-%d", gData.DumpDirectory, L"Draw History", gData.DumpIndex);
			CreateDirectory(m_DumpDirectory, 0);
			DeleteAllFiles(m_DumpDirectory);
			m_StereoBuffer->ClearStatistics();
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
			DEBUG_MESSAGE(_T("Start Tracing\n"));

			CriticalSectionLocker locker(m_CSCB);
			WriteStreamer::Comment("### device state begin ###" );
			m_DeviceState.WriteToFile( this );
			WriteStreamer::Comment("### device state end ###" );
		}
	}

	m_MonoCommandsCount = 0;
	m_StereoCommandsCount = 0;
	m_SetRenderTargetCount = 0;
	m_DrawCount = 0;
	m_StereoDrawCount = 0;
	m_StereoDrawAutoCount = 0;
	m_StereoDrawIndexedCount = 0;
	m_StereoDrawInstancedCount = 0;
	m_StereoDrawIndirectCount = 0;
	m_StereoBuffersCount = 0;
	m_CBCount = 0;
	m_CBSubIndexCount = 0;
#endif
	m_MaxBufferSize = 0;
	m_FrameIndex++;
}

void D3DDeviceWrapper::InitFont()
{
	SAFE_DELETE( m_pFont );

	m_pFont	= DNew acGraphics::CFont();
	m_pFont->SetTexelOffset(0.0f);

	TCHAR fontPath[MAX_PATH];
	if (iz3d::resources::GetAllUsersDirectory( fontPath ))
	{
		PathAppend(fontPath, _T("Font\\OSDFont.fnt"));
		m_pFont->Init( fontPath, m_pDynRender );
	}	
}

void D3DDeviceWrapper::DrawOSD( D3DSwapChain* pSwapChain_, float fFrameTimeDelta )
{
	D3DSwapChain* pSwapChain = GetD3DSwapChain();
	SwapChainResources* pRes = pSwapChain->GetCurrentPrimaryResource();
	if(IsStereoActive())
	{
		ShiftFinder10* pShift = pSwapChain->GetShiftFinder();
		if(pShift->IsInitializedSuccessfully())
		{
			//--- find new shift ---
			if((m_Input.GetActivePreset()->AutoFocusEnable || m_Input.LaserSightEnable))
			{
				pShift->FindShift(pRes->GetMethodResourceLeft(), pRes->GetMethodResourceRight(), m_FrameIndex);
			}

			//--- draw laser sight ---
			if(m_Input.LaserSightEnable)
				m_LaserSight.Draw(pSwapChain, fFrameTimeDelta);
		}

		/************************************************************************/
		// draw clipping quads
		if( gInfo.FrustumAdjustMode )
		{
			for (int nViewIndex = (m_Input.StereoActive ? VIEWINDEX_RIGHT : VIEWINDEX_LEFT); 
				nViewIndex >= VIEWINDEX_LEFT; nViewIndex--)
			{
				pRes->SetStereoRenderTarget(nViewIndex);

				float fWidth			=	( float )pSwapChain_->m_Description.BufferDesc.Width;
				float fHeight			=	( float )pSwapChain_->m_Description.BufferDesc.Height;
				float fClippingWidth	=	gInfo.BlackAreaWidth * fWidth;

				m_pDynRender->Begin( acGraphics::RENDER_QUAD_CLIPPING );

				// left clipping quad
				m_pDynRender->VtxPos( 0.0f, 0.0f, 1.0f );
				m_pDynRender->VtxPos( fClippingWidth, -fHeight, 1.0f );
				m_pDynRender->VtxPos( 0.0f, -fHeight, 1.0f );
				m_pDynRender->VtxPos( 0.0f, 0.0f, 1.0f );
				m_pDynRender->VtxPos( fClippingWidth, 0.0f, 1.0f );
				m_pDynRender->VtxPos( fClippingWidth, -fHeight, 1.0f );

				// right clipping quad
				m_pDynRender->VtxPos( fWidth - fClippingWidth, 0.0f, 1.0f );
				m_pDynRender->VtxPos( fWidth, -fHeight, 1.0f );
				m_pDynRender->VtxPos( fWidth - fClippingWidth, -fHeight, 1.0f );
				m_pDynRender->VtxPos( fWidth - fClippingWidth, 0.0f, 1.0f );
				m_pDynRender->VtxPos( fWidth, 0.0f, 1.0f );
				m_pDynRender->VtxPos( fWidth, -fHeight, 1.0f );

				m_pDynRender->End();
			}
		}
	}


#ifndef DISABLE_WIZARD
	// 0.4.2: match the DX9 strip (see S3DWrapper9/BaseSwapChain.cpp
	// around DrawOSD). WizardData::RenderDX10 and HotKeyOSD::Render
	// are vestigial iZ3D-era paths whose on-screen output is either
	// uninformative (Wizard, assets never ship) or duplicates the
	// modern DX11 OSDRenderer (HotKeyOSD vs Slot::HotkeyHelp).
	// ProcessKeys stays so Shift+Numpad* hotkey chain doesn't fall off.
	if (gInfo.ShowOSD)
	{
		pSwapChain->GetWizardData()->SetCurSC( pRes );
		pSwapChain->GetWizardData()->ProcessKeys();
	}
#endif
}

void D3DDeviceWrapper::DrawLogo( D3DSwapChain* pSwapChain_ )
{	
	if ( !m_pOutputMethod->IsLogoPresent() )
		return;

	SwapChainResources* pRes = pSwapChain_->GetCurrentPrimaryResource();
	bool bStereoEnabled = IsStereoActive();

	acGraphics::CDynTexture logo;
	static acGraphics::TD3D10UMShaderResourceView tUMTexture;
	logo.m_pDxTex = &tUMTexture;
	tUMTexture.m_hResView	= m_pOutputMethod->GetLogoResourceView().pDrvPrivate;
	tUMTexture.m_hRes		= NULL;

	m_pDynRender->SetTexture( logo );

	for ( int nViewIndex = ( bStereoEnabled ? VIEWINDEX_RIGHT : VIEWINDEX_LEFT); 
		nViewIndex >= VIEWINDEX_LEFT; nViewIndex-- )
	{
		if ( bStereoEnabled )
			pRes->SetStereoRenderTarget(nViewIndex);

		m_pDynRender->Begin( acGraphics::RENDER_QUAD_LIST	 );

		float fU1 = 0.0f;
		float fV1 = 0.0f;
		float fU2 = 1.0f;
		float fV2 = 1.0f;		

		float fBorderOffset		= 30.0f;
		float fViewPortWidth	= ( float ) pSwapChain_->m_Description.BufferDesc.Width;		
		float fWidth			= ( float ) m_pOutputMethod->GetLogoWidth();
		float fHeight			= ( float ) m_pOutputMethod->GetLogoHeight();
		float fLeft				= fViewPortWidth - fWidth - fBorderOffset;
		float fRight			= fLeft + fWidth;
		float fTop				= -fBorderOffset;
		float fBottom			= fTop - fHeight;
		float fZ				= 1.0f;

		m_pDynRender->VtxData( 0 );
		m_pDynRender->VtxTexCoord( fU1, fV1 );
		m_pDynRender->VtxPos( fLeft, fTop, fZ );
		m_pDynRender->VtxTexCoord( fU2, fV1 );
		m_pDynRender->VtxPos( fRight, fTop, fZ );
		m_pDynRender->VtxTexCoord( fU2, fV2);
		m_pDynRender->VtxPos( fRight, fBottom, fZ );		
		m_pDynRender->VtxTexCoord( fU1, fV2 );
		m_pDynRender->VtxPos( fLeft, fBottom, fZ );

		m_pDynRender->End();
	}
}

void D3DDeviceWrapper::DrawInfo( D3DSwapChain* pSwapChain_ )
{	
	if ( !m_pFont && (gInfo.FrustumAdjustMode != 0 || gInfo.ShowFPS || gInfo.ShowOSD || GET_DEBUG_VALUE(gInfo.ShowDebugInfo)) )
		InitFont();

	if(!m_pFont)
		return;

	SwapChainResources* pRes = pSwapChain_->GetCurrentPrimaryResource();

	D3D10_VIEWPORT	tVP;
	ZeroMemory(&tVP, sizeof( D3D10_VIEWPORT ) );
	tVP.Width	= pSwapChain_->m_Description.BufferDesc.Width;
	tVP.Height	= pSwapChain_->m_Description.BufferDesc.Height;
	m_pFont->PreparePixelPerfectOutput( tVP );
	
	pSwapChain_->PrepareFPSMessage( this );

	std::wostringstream szCameraInfo;
	if( gInfo.ShowOSD && m_nOSDShowTimeLeft.QuadPart > 0 )
	{
		szCameraInfo << g_LocalData.GetText(LT_Preset) << L" " << m_Input.ActivePreset + 1 << L"\n";
		if( m_Input.GetActivePreset()->One_div_ZPS == 0.f )
			szCameraInfo << g_LocalData.GetText(LT_ConvergenceINF) << L"\n";
		else
			szCameraInfo << g_LocalData.GetText(LT_Convergence) << L" = " << std::fixed << std::setprecision(4) << -m_Input.GetActivePreset()->One_div_ZPS << L"\n";

		//--- show Separation as % ---
		bool separationValid = true;	
		if(m_Input.GetActivePreset()->AutoFocusEnable)
		{
			if(!pSwapChain_->GetShiftFinder()->IsCurrentShiftValid() || 
				(pSwapChain_->GetShiftFinder()->IsShiftToHoldValid() && m_FrameIndex == pSwapChain_->GetShiftFinder()->GetShiftInValidationFrame()))
				separationValid = false;
		}

		szCameraInfo << g_LocalData.GetText(LT_Separation) << L" = " << std::fixed << std::setprecision(2) << 
			SEPARATION_TO_PERCENT(m_Input.GetActivePreset()->StereoBase) << L"%\n" <<
			g_LocalData.GetText(LT_AutoFocus) << L" " <<
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
	CHAR szDebugInfo[1024];
	if(gInfo.ShowDebugInfo)
	{
		DXGI_SWAP_CHAIN_DESC primaryDesc, secondaryDesc;
		switch(m_Input.DebugInfoPage)
		{
		case 1:
			{
				pSwapChain_->m_pSwapChain->GetDesc(&primaryDesc);
				if (m_bTwoWindows)
					pSwapChain_->m_pSecondSwapChain->GetDesc(&secondaryDesc);
				_snprintf_s(szDebugInfo, _TRUNCATE, "Command buffering: %s\n"
					"Original mode: %s\nDriver mode: %s%s\n"
					"DrawCount = %d (StereoDraw %d, Auto %d, Indexed %d, Instanced %d, Indirect %d)\n"
					"MonoCommandsCount = %d\nStereoCommandsCount = %d\n"
					"SetRenderTargetCount = %d\nMaxBufferSize=%d\n"
					"StereoBuffersCount = %d\nCBCount = %d\n"
					"FlushCount = %d",
					(gInfo.CommandBuffering ? "Enabled" : "Disabled"),
					(pSwapChain_->m_Description.Windowed ? "Windowed" : "Fullscreen"), 
					(primaryDesc.Windowed ? "Windowed" : "Fullscreen"), 
					m_bTwoWindows ? (secondaryDesc.Windowed ? ", Windowed" : ", Fullscreen") : "",
					m_DrawCount, m_StereoDrawCount, m_StereoDrawAutoCount, m_StereoDrawIndexedCount, 
					m_StereoDrawInstancedCount, m_StereoDrawIndirectCount,
					m_MonoCommandsCount, m_StereoCommandsCount, 
					m_MaxBufferSize, m_SetRenderTargetCount,
					m_StereoBuffersCount, m_CBCount,
					m_nFlushCounter);
			}
			break;
		case 2:			
			_snprintf_s(szDebugInfo, _TRUNCATE, "Primary buffers count = %d\n"
				"Current buffer = %p",
				pSwapChain_->m_ResourcesPrimary.size(), 
				pRes->GetBackBufferPrimaryWrap());
			break;
		case 3:
			DXGI_SWAP_CHAIN_DESC desc;
			ZeroMemory(&desc, sizeof(DXGI_SWAP_CHAIN_DESC));
			pSwapChain_->m_pSwapChain->GetDesc(&desc);
			_snprintf_s(szDebugInfo, _TRUNCATE, "Windowed = %d\n"
				"Refresh rate = %d/%d",
				desc.Windowed, 
				desc.BufferDesc.RefreshRate.Numerator, 
				desc.BufferDesc.RefreshRate.Denominator);
			break;
		case 4:
			strcpy_s(szDebugInfo, "");
			break;
		default:
			strcpy_s(szDebugInfo, "");
			break;
		}
	}
#endif

	for (int nViewIndex = (m_Input.StereoActive ? VIEWINDEX_RIGHT : VIEWINDEX_LEFT); 
		nViewIndex >= VIEWINDEX_LEFT; nViewIndex--)
	{
		pRes->SetStereoRenderTarget(nViewIndex);
		float x = 10;
		if (m_Input.StereoActive && gInfo.FrustumAdjustMode)
		{
			switch (gInfo.SeparationMode)
			{
			case 0:
				x += gInfo.BlackAreaWidth * pSwapChain_->m_BackBufferSize.cx;
				break;
			case 1:
				x += 2 * gInfo.BlackAreaWidth * pSwapChain_->m_BackBufferSize.cx;
				break;
			default:
				break;
			}
		}
		float y = 10;
		if (gInfo.FixNone16x9TextPosition)
			y += std::max(0.0f, (pSwapChain_->m_BackBufferSize.cy - (pSwapChain_->m_BackBufferSize.cx / (16.0f / 9))) / 2);

		if (gInfo.ShowFPS)
		{
			std::wstring s = pSwapChain_->m_szFPS.str();
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
			m_pFont->DrawML(x, y, 1.0f, szDebugInfo, 0, 0);
#endif
	}
}

void D3DDeviceWrapper::UpdateFrameStatistic( D3DSwapChain* pSwapChain_ )
{
	LARGE_INTEGER nCurrentTime;
	QueryPerformanceCounter(&nCurrentTime);
	pSwapChain_->m_nFrameTimeDelta.QuadPart = nCurrentTime.QuadPart - pSwapChain_->m_nLastFrameTime.QuadPart;
	pSwapChain_->m_nLastFrameTime = nCurrentTime;
	// time of fps drop show
	pSwapChain_->m_nFPSDropShowTimeLeft.QuadPart = std::max<LONGLONG>(pSwapChain_->m_nFPSDropShowTimeLeft.QuadPart - pSwapChain_->m_nFrameTimeDelta.QuadPart, 0);

	m_nGlobalFrameTimeDelta.QuadPart = nCurrentTime.QuadPart - m_nGlobalLastFrameTime.QuadPart;
	m_nGlobalLastFrameTime = nCurrentTime;
	// time of showing time left
	m_nOSDShowTimeLeft.QuadPart		= std::max<LONGLONG>( m_nOSDShowTimeLeft.QuadPart - m_nGlobalFrameTimeDelta.QuadPart, 0 );
}

void D3DDeviceWrapper::RestoreStates()
{
	m_DeviceState.m_OMRenderTargets->Execute( this );
	m_DeviceState.m_OMDepthStencilState->Execute( this );
	m_DeviceState.m_OMBlendState->Execute( this );

	m_DeviceState.m_IaTopology->Execute( this );
	m_DeviceState.m_IaInputLayout->Execute( this );
	m_DeviceState.m_IaVertexBuffers->Execute( this );

	m_DeviceState.m_RSState->Execute( this );
	//m_DeviceState.m_RSScissorRects->Execute( this );
	m_DeviceState.m_RSViewports->Execute( this );

	m_DeviceState.VS.m_Shader->Execute( this );
	m_DeviceState.VS.m_ConstantBuffers->Execute( this );

	m_DeviceState.PS.m_Shader->Execute( this );
	m_DeviceState.PS.m_ConstantBuffers->Execute( this );
	m_DeviceState.PS.m_ShaderResources->Execute( this );
	m_DeviceState.PS.m_Samplers->Execute( this );

	m_DeviceState.GS.m_Shader->Execute( this );
}

void D3DDeviceWrapper::DoPresent(  )
{
	g_pLastD3DDevice = this;
	D3DSwapChain* pSwapChain = GetD3DSwapChain( );
	if (!pSwapChain)
		return;

	SwapChainResources* pRes = pSwapChain->GetCurrentPrimaryResource();
	if ( !pRes->IsInitialized() )
		return;

	CriticalSectionLocker locker( m_CSUMCall );					// Check for performance penalty
	WE_CALL;
	DEBUG_TRACE3(_T("PresentInt\n"));

	if (!pSwapChain->m_bLeftEyeActive)
	{
		if (!IS_SHUTTER_OUTPUT)
			return;
		if( USE_IZ3DKMSERVICE_PRESENTER )
		{
			// do nothing, we've already called Output() for both eyes earlier in D3DSwapChain::PresentData() 
			return;
		}
		else
			m_pOutputMethod->Output( pSwapChain->m_bLeftEyeActive, pSwapChain, NULL );
		RestoreStates();
		return;
	}

	UpdateFrameStatistic( pSwapChain );
	float fFrameTimeDelta = float(pSwapChain->m_nFrameTimeDelta.QuadPart) / float(m_nFreq.QuadPart);

	//--- optimization ---
	//--- do not check m_AutoFocus.IsInitializedSuccessfully() because enough ---
	//--- checking of m_AutoFocus.IsCurrentShiftValid() flag in this case     ---
	//--- we can use valid shift value regardless input changing ---
	ShiftFinder10* pShift = pSwapChain->GetShiftFinder();
	if(m_Input.StereoActive && pShift->IsCalculationComplete() && pShift->IsCurrentShiftValid())
	{
		//--- we can use m_AutoFocus data here ---
		if(m_Input.LaserSightEnable)
			pSwapChain->GetLaserSightData()->SetShift(pShift->GetCurrentShift(), gInfo.SeparationMode);
	}

	gKbdHook.m_Access.Enter();
	DataInput updatedInput = gInfo.Input;
	gKbdHook.m_Access.Leave();
	if (m_Input.IsChanged( updatedInput	))
	{
		m_nWriteSettingsAfter.QuadPart	= 5 * m_nFreq.QuadPart; // 5 sec
		m_nOSDShowTimeLeft.QuadPart		= 2 * m_nFreq.QuadPart;	// 2 sec		

		//--- first, we must render new views, then calculate shift ---
		pShift->SetShiftToHoldInvalid(m_FrameIndex);
	}
	else
	{
		//--- optimization ---
		//--- do not check m_AutoFocus.IsInitializedSuccessfully() because enough ---
		//--- checking of m_AutoFocus.IsCurrentShiftValid() flag in this case     ---
		if(m_Input.StereoActive && pShift->IsCalculationComplete() && pShift->IsCurrentShiftValid())
		{
			if(m_Input.GetActivePreset()->AutoFocusEnable)
			{
				gKbdHook.m_Access.Enter(); // one thread is beginning
				m_AutoFocus.UpdateZPS(pShift, gInfo.Input.GetActivePreset());
				updatedInput = gInfo.Input;
				gKbdHook.m_Access.Leave(); 
			}
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

	// scale before drawing onto scaled surface
	pSwapChain->DoScaling();

	// draw things
	{		
		m_tViewPort.Width	= (float)pSwapChain->m_Description.BufferDesc.Width;
		m_tViewPort.Height	= (float)pSwapChain->m_Description.BufferDesc.Height;
		UINT nClearViewPorts = Commands::GetClearCount( 1, m_DeviceState.m_NumViewports );
		OriginalDeviceFuncs.pfnSetViewports( hDevice, 1, nClearViewPorts, &m_tViewPort );

		if (gInfo.MakeScreenshot)
		{
			pSwapChain->WriteJPSScreenshot(false);
			gInfo.MakeScreenshot = FALSE; // single-thread only
			if (gInfo.MakeScreenshotCallback) gInfo.MakeScreenshotCallback();
		}

		DrawOSD( pSwapChain, fFrameTimeDelta );

		if( m_ScreenShotButton.isButtonWasPressed() )
		{
			bool bApplyGammaCorrection = !IsKeyDown( VK_CONTROL );
			pSwapChain->WriteJPSScreenshot( bApplyGammaCorrection );
		}

		//font draw
		DrawInfo( pSwapChain );

		DrawLogo( pSwapChain );

		pSwapChain->PresentData();

		RestoreStates();
	}

	if (USE_UM_PRESENTER)
	{
		LARGE_INTEGER time;
		QueryPerformanceCounter(&time);
		SetNextFlushTime(time);
	}

	pSwapChain->CheckSecondWindowPosition();

	// update camera data
	m_bStereoModeChanged = ( m_Input.StereoActive != updatedInput.StereoActive );

	bool bUpdateProjectionMatrix = false;
	if (updatedInput.StereoActive && (!m_Input.StereoActive || m_Input.IsStereoParamChanged( updatedInput )))
		bUpdateProjectionMatrix = true;

	m_Input = updatedInput;
	gInfo.ShowFPS = m_Input.ShowFPS;

	if (m_bStereoModeChanged)
	{
		StereoModeChanged( updatedInput.StereoActive );
		m_pOutputMethod->StereoModeChanged( updatedInput.StereoActive );
	}

	if (bUpdateProjectionMatrix)
		UpdateProjectionMatrix();
}

bool D3DDeviceWrapper::CheckFlush( )
{
	if (gInfo.PresenterSleepTime != -2)
		return false;

	if (!IsStereoActive() || !m_pLastSwapChain || m_pLastSwapChain->m_Description.Windowed)
		return false;

	LARGE_INTEGER time;
	QueryPerformanceCounter(&time);
	if (time.QuadPart >= m_FlushTime.QuadPart)
		return true;
	return false;
}

void D3DDeviceWrapper::FlushCommands( )
{
	CriticalSectionLocker locker(m_CSUMCall);
	LARGE_INTEGER time;
	CallFlush();
	QueryPerformanceCounter(&time);
	SetNextFlushTime(time);
}

void D3DDeviceWrapper::UpdateSecondSwapChain( D3D10DDI_HRESOURCE hResource )
{
	D3DSwapChain* pSwapChain = GetD3DSwapChain( );
	if (!pSwapChain)
		return;

	SwapChainResources* pRes = pSwapChain->GetCurrentPrimaryResource();
	if ( pRes == NULL || !pRes->GetBackBufferSecondary().pDrvPrivate )
		return;

	CriticalSectionLocker locker( m_CSUMCall );
	DEBUG_TRACE3(_T("UpdateSecondSwapChain\n"));
	ResourceWrapper* pBackBuffer;
	InitWrapper(hResource, pBackBuffer);

	if (!pRes->IsAAOn() || m_pOutputMethod->RenderDirectlyToBB())
		OriginalDeviceFuncs.pfnResourceCopy( hDevice, pBackBuffer->GetHandle(), pRes->GetBackBufferSecondary() );
	else
		OriginalDeviceFuncs.pfnResourceResolveSubresource( hDevice, pBackBuffer->GetHandle(), 0,
			pRes->GetBackBufferSecondary(), 0, pSwapChain->m_Description.BufferDesc.Format);
}

typedef OutputMethod* (CALLBACK* LPFNCREATEOUTPUT)(DWORD, DWORD);

void D3DDeviceWrapper::CreateOutput()
{
	DDILog("CreateOutput: gInfo.hOutputDLL=%p, gInfo.OutputMode=%d, gInfo.OutputSpanMode=%d\n",
		(void*)gInfo.hOutputDLL, (int)gInfo.OutputMode, (int)gInfo.OutputSpanMode);
	LPFNCREATEOUTPUT lpfnCreateOutput;	// Function pointer
	if (gInfo.hOutputDLL != NULL)
	{
		lpfnCreateOutput = (LPFNCREATEOUTPUT)GetProcAddress(gInfo.hOutputDLL,
			"CreateOutputDX10");
		DDILog("CreateOutput: GetProcAddress(CreateOutputDX10)=%p\n", (void*)lpfnCreateOutput);
		if (!lpfnCreateOutput)
		{
			DDILog("CreateOutput: CreateOutputDX10 export missing — freeing DLL\n");
			FreeLibrary(gInfo.hOutputDLL);
			gInfo.hOutputDLL = NULL;
		}
		else
		{
			// 0.4.3 fix: pass the per-DLL submode index (0..N inside the
			// loaded Output DLL), not the global OUTMODE_* id. Same bug as
			// S3DWrapper9/BaseStereoRenderer::CreateOutput. gInfo.OutputMode
			// stays as the global id so SwapChain11Proxy::SelectedOutputMode
			// and OSD routing elsewhere keep working.
			const DWORD dllSubmode = static_cast<DWORD>(
				wiz3d::OutputModeToLegacySubmode(static_cast<int>(gInfo.OutputMode)));
			DDILog("CreateOutput: dllSubmode=%d (from global OutputMode=%d)\n",
				(int)dllSubmode, (int)gInfo.OutputMode);
			m_pOutputMethod = lpfnCreateOutput(dllSubmode, gInfo.OutputSpanMode);
			DDILog("CreateOutput: CreateOutputDX10 returned m_pOutputMethod=%p\n",
				(void*)m_pOutputMethod);
		}
	}
	else
	{
		DDILog("CreateOutput: hOutputDLL is NULL — output DLL never loaded\n");
	}
	if (m_pOutputMethod == NULL)
	{
		DEBUG_MESSAGE(_T("Failed to load output module. Mono output selected.\n"));
		DDILog("CreateOutput: FALLBACK to MonoOutput (output module not loaded)\n");
		m_bUsingMonoOutput = true;
		m_pOutputMethod = DNew MonoOutput(gInfo.SeparationMode != 2 ? 0 : 1, gInfo.OutputSpanMode);
		gInfo.OutputCaps = 0;
		gData.ShutterMode = SHUTTER_MODE_DISABLED;
	}

	IDXGIFactory * pDXGIFactory = NULL;
	bool bFindAdapter			= false;

	HRESULT hrFactory = CreateDXGIFactory(__uuidof(IDXGIFactory), (void**)&pDXGIFactory);
	DDILog("CreateOutput: CreateDXGIFactory hr=0x%08X, pFactory=%p\n", hrFactory, (void*)pDXGIFactory);
	if( SUCCEEDED( hrFactory ) )
	{
		bFindAdapter = m_pOutputMethod->FindAdapter( pDXGIFactory );
		DDILog("CreateOutput: FindAdapter returned %d\n", (int)bFindAdapter);
		SAFE_RELEASE( pDXGIFactory );
	}

	if ( !bFindAdapter )
	{
		DEBUG_MESSAGE(_T("Not found output adapter. Mono output selected.\n"));
		DDILog("CreateOutput: FALLBACK to MonoOutput (FindAdapter failed)\n");
		delete m_pOutputMethod;
		m_bCantFindStereoDevice = true;
		gInfo.OutputCaps = 0;
		gData.ShutterMode = SHUTTER_MODE_DISABLED;
		m_pOutputMethod = DNew MonoOutput(gInfo.SeparationMode != 2 ? 0 : 1, gInfo.OutputSpanMode);
	}
	DDILog("CreateOutput: DONE, m_pOutputMethod=%p, m_bUsingMonoOutput=%d, m_bCantFindStereoDevice=%d\n",
		(void*)m_pOutputMethod, (int)m_bUsingMonoOutput, (int)m_bCantFindStereoDevice);
}

// swapchain should be created before first back buffer
void D3DDeviceWrapper::InitializeSwapChainRelatedData()
{
	if (m_bFirstSwapChainCreated)
		return;
	m_bTwoWindows = m_pOutputMethod->GetOutputChainsNumber() > 1;
	if (m_bTwoWindows)
	{
		switch(gInfo.MultiWindowsMode)
		{
		case MULTI_WINDOWS_MODE_APPLICATION:
			m_bTwoWindows = false;
			break;
		case MULTI_WINDOWS_MODE_MULTIHEAD:
			m_DeviceMode = DEVICE_MODE_FORCEFULLSCREEN;
			break;
		case MULTI_WINDOWS_MODE_SWAPCHAINS:
		case MULTI_WINDOWS_MODE_SIDEBYSIDEWINDOWED:
			m_DeviceMode = DEVICE_MODE_FORCEWINDOWED;
			break;
		}
	}

	m_pDynRender = DNew acGraphics::CDynRender_dx10UM( this );
	m_pOutputMethod->Initialize(this);
	m_bFirstSwapChainCreated = true;
	StereoModeChanged(m_Input.StereoActive);
	m_pOutputMethod->StereoModeChanged( m_Input.StereoActive != 0 );
	m_LaserSight.Initialize(m_pDynRender);

	if(IS_SHUTTER_OUTPUT && !USE_ATI_PRESENTER)
	{
		LARGE_INTEGER time;
		QueryPerformanceCounter(&time);
		SetNextFlushTime(time);
		if (gInfo.PresenterFlushTime < 0)
			m_PresenterFlushTime = 1;
		else if (gInfo.PresenterFlushTime == 0)
			m_PresenterFlushTime = 50;
		else
			m_PresenterFlushTime = gInfo.PresenterFlushTime;
	}

#ifndef DISABLE_WIZARD
	//////////////////////////////////////////////////////////////////////////
	// Wizard initializing
	auto WizDrawer = new WizardDrawer( m_pDynRender );
	m_Wizard.Initialize( WizDrawer );
	m_HotKeyOSD.Initialize( WizDrawer );
#endif

	m_pECBuffer = new iz3d::ExternalConstantBuffer( this, 8 );
}

void D3DDeviceWrapper::SetNextFlushTime( LARGE_INTEGER &time )
{
	m_FlushTime.QuadPart = time.QuadPart + (LONGLONG)(m_nFreq.QuadPart * m_PresenterFlushTime / 1000);
}

void D3DDeviceWrapper::InitializeDXGIDevice( IDXGIDevice* pDevice_ )
{
	m_pDXGIDevice = pDevice_;
}

D3DSwapChain* D3DDeviceWrapper::MapDXGISwapChain( IDXGIFactory* pFactory, IDXGISwapChain* pSwapChain )
{
	if (!m_pTempSwapChain)
		return NULL;

	D3DSwapChain* pD3DSwapChain = m_pTempSwapChain;
	pD3DSwapChain->m_pDXGIFactory = pFactory;
	pD3DSwapChain->SetDXGISwapChain(pSwapChain);
	gKbdHook.setBackBufferWidth(pD3DSwapChain->m_BackBufferSize.cx);

	_ASSERT(m_SwapChainsMap.find(pSwapChain) == m_SwapChainsMap.end());
	m_SwapChainsMap.insert(pSwapChain, pD3DSwapChain);
	pD3DSwapChain->InitializeSwapChains();
	m_pLastSwapChain = m_pTempSwapChain;
	m_pTempSwapChain = NULL;
	UpdateProjectionMatrix();
	return pD3DSwapChain;
}

D3DSwapChain* D3DDeviceWrapper::GetD3DSwapChain( )
{
	D3DSwapChain* pSwapChain;
	SwapChainsMap::iterator it = m_SwapChainsMap.find(g_pCurrentSwapChain);
	if( it != m_SwapChainsMap.end() )
	{
		pSwapChain = it->second;
		m_pLastSwapChain = pSwapChain;
	}
	else
		pSwapChain = NULL;
	return pSwapChain;
}

void D3DDeviceWrapper::SetSecondBackBuffer( D3D10DDI_HRESOURCE hResource )
{
	DEBUG_TRACE1(_T("Created our backbuffer - %p\n"), hResource.pDrvPrivate);
}

bool D3DDeviceWrapper::RemoveAppBackBuffer( SwapChainsMap::iterator it, D3DSwapChain* pSwapChain, D3D10DDI_HRESOURCE &hResource )
{
	PrimaryResourcesMap::iterator i = pSwapChain->m_ResourcesPrimary.find(hResource);
	if (i != pSwapChain->m_ResourcesPrimary.end())
	{
		DEBUG_TRACE1(_T("Remove app backbuffer - %p\n"), hResource.pDrvPrivate);
		if (g_SwapChainMode == scDestroing)
		{
			if (pSwapChain == m_pLastSwapChain)
				m_pLastSwapChain = NULL;
			if (it != m_SwapChainsMap.end())
				m_SwapChainsMap.erase(it);
		}
		else
		{
			pSwapChain->Clear(&i->second);
			bool bResetIterator = (pSwapChain->m_LatestResourcePrimary == i);
			pSwapChain->m_ResourcesPrimary.erase(i);
			if (bResetIterator)

			{
				if( CPresenterX::Get() )
					CPresenterX::Get()->SetActionAndWait(ptNone);
				pSwapChain->m_LatestResourcePrimary = pSwapChain->m_ResourcesPrimary.end();
			}
		}
		return true;
	}
	return false;
}

void D3DDeviceWrapper::RemoveSwapChain( D3D10DDI_HRESOURCE hResource, bool bPrimary )
{
	if (!bPrimary)
	{
		DEBUG_TRACE1(_T("Removed secondary backbuffer - %p\n"), hResource.pDrvPrivate);
		return;
	}

	SwapChainsMap::iterator it = m_SwapChainsMap.end();
	if (m_pTempSwapChain && RemoveAppBackBuffer(it, m_pTempSwapChain, hResource))
		return;

	it = m_SwapChainsMap.begin();
	while( it != m_SwapChainsMap.end() )
	{
		D3DSwapChain* pSwapChain = it->second;
		if (RemoveAppBackBuffer(it, pSwapChain, hResource))
			return;
		it++;
	}
	DEBUG_TRACE1(_T("Something miss (primary or secondary) - %p\n"), hResource.pDrvPrivate);
}

void D3DDeviceWrapper::ProcessCB()
{
	CriticalSectionLocker locker(m_CSCB);

	CommandBuffer* cb = m_StereoBuffer->Processing() ? (CommandBuffer*)m_StereoBuffer.get() : (CommandBuffer*)m_MonoBuffer.get();

	// Diagnostic: which buffer is flushing this round, and what stereo gating
	// would say right now? Rate-limited to first 10 calls + every 200th after,
	// so a long session doesn't bloat the log.
	{
		static int s_processCBCount = 0;
		bool wantStereo = UseStereoCommandBuffer();
		bool isStereoBuffer = (cb == (CommandBuffer*)m_StereoBuffer.get());
		if (s_processCBCount == 0)
		{
			// One-shot profile diagnostic: dump the profile S3DAPI matched.
			// Per-source flags distinguish where the match came from:
			//   * base=1     → matched in BaseProfile.xml (iZ3D-shipped game list)
			//   * community=1→ matched in CommunityProfile.xml (only checked
			//                  if Base didn't match — community fixes go here)
			//   * user=1     → matched in UserProfile.xml (always checked,
			//                  user-local overrides; can layer on top of Base)
			// When all three are 0, ProfileName holds the exe-name fallback
			// (Bioshock.exe → "bioshock"), which would be confusing to log
			// as a real match — so we show ProfileName='' in that case.
			bool anyMatched = gInfo.bMatchedInBase || gInfo.bMatchedInCommunity || gInfo.bMatchedInUser;
			DDILog("ProfileLoad: ProfileName='%ls' base=%d community=%d user=%d AppName='%ls' AppFile='%ls'\n",
				anyMatched ? gInfo.ProfileName : L"",
				(int)gInfo.bMatchedInBase,
				(int)gInfo.bMatchedInCommunity,
				(int)gInfo.bMatchedInUser,
				gInfo.ApplicationName,
				gInfo.ApplicationFileName);
		}
		if (s_processCBCount < 10 || (s_processCBCount % 200) == 0)
		{
			DDILog("ProcessCB[%d]: flush=%s wouldFinalizeAs=%s IsStereoActive=%d IsRTStereo=%d IsDSStereo=%d IsUAVStereo=%d\n",
				s_processCBCount,
				isStereoBuffer ? "Stereo" : "Mono",
				wantStereo ? "Stereo" : "Mono",
				(int)IsStereoActive(),
				(int)m_DeviceState.m_IsRTStereo,
				(int)m_DeviceState.m_IsDSStereo,
				(int)m_DeviceState.m_IsUAVStereo);
		}
		++s_processCBCount;
	}

	cb->FlushAll();
	if ( cb->WantFinalize() )
	{
		cb->Finalize();
		if ( UseStereoCommandBuffer() ) {
			m_StereoBuffer->Begin();
		}
		else {
			m_MonoBuffer->Begin();
		}
	}
	else
		cb->Begin();
}

bool D3DDeviceWrapper::UseStereoCommandBuffer() const
{
	return IsStereoActive() && (m_DeviceState.m_IsRTStereo || m_DeviceState.m_IsDSStereo || m_DeviceState.m_IsUAVStereo);
}

void D3DDeviceWrapper::UpdateProjectionMatrix( ) 
{
	DEBUG_TRACE3(_T("\nUpdateProjection beginning\n"));

	float cameraShift = m_Input.GetActivePreset()->StereoBase * 0.5f;
	float One_div_ZPS = m_Input.GetActivePreset()->One_div_ZPS;
	float aspectRatio;
	if (!m_SwapChainsMap.empty())
	{
		SwapChainsMap::const_reverse_iterator it = m_SwapChainsMap.rbegin();
		const SIZE& size = (*it).second->m_BackBufferSize;
		aspectRatio = (float)size.cx / (float)size.cy;
	}
	else
		aspectRatio = 1680.0f / 1050.0f;
	float yScale = 1.0f/tan((float)DEFAULT_FOV/2);
	D3DXMATRIXA16 projMatrix(
		yScale / aspectRatio,	0.0f,	0.0f,															0.0f, 
		0.0f,					yScale,	0.0f,															0.0f, 
		0.0f,					0.0f,	DEFAULT_ZFAR / (DEFAULT_ZFAR - DEFAULT_ZNEAR),					1.0f,
		0.0f,					0.0f,	-DEFAULT_ZNEAR * DEFAULT_ZFAR / (DEFAULT_ZFAR - DEFAULT_ZNEAR),	0.0f
		);
	//D3DXMatrixPerspectiveFovLH(&m_ProjectionMatrix, DEFAULT_FOV, aspectRatio,
	//	DEFAULT_ZNEAR, DEFAULT_ZFAR );
	D3DXMATRIXA16 invProjection;
	D3DXMatrixInverse(&invProjection, NULL, &projMatrix);

	float A, B;
	float scale;
	//if (FALSE)
	//{
	//	m_OldMinZ = m_ViewPort.MinZ;
	//	m_OldMaxZ = m_ViewPort.MaxZ;
	//	DEBUG_TRACE3(_T("MinZ %f, MaxZ %f:\n"), m_ViewPort.MinZ, m_ViewPort.MaxZ); 
	//	if (m_OldMinZ != 0.0f)
	//	{
	//		DEBUG_TRACE3(_T("Warning: MinZ != 0.0\n")); 
	//	}
	//	scale = m_OldMaxZ /*- m_OldMinZ*/;
	//	scale = scale > 0.0f ? (1.0f / scale) : (1.0f / 1e-10f);
	//}
	//else
	{
		scale = 1.0f;
	}

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
	CalculateMatrix(VIEWINDEX_LEFT, A, B, &projMatrix, &invProjection);

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
	CalculateMatrix(VIEWINDEX_RIGHT, A, B, &projMatrix, &invProjection);

	DEBUG_TRACE3(_T("End of UpdateProjectionMatrix\n"));
}

void D3DDeviceWrapper::CalculateMatrix( VIEWINDEX viewIndex, float A, float B, const D3DXMATRIXA16* pProjMatrix, const D3DXMATRIXA16* pInvProjection )
{
	D3DXMATRIX &transformationMatrix = GetTransformationMatrix(viewIndex);

	//--- stereo matrix ---
	D3DXMATRIXA16 StereoProjectionMatrix;
	StereoProjectionMatrix._11 = pProjMatrix->_11;
	StereoProjectionMatrix._12 = pProjMatrix->_12;
	StereoProjectionMatrix._13 = pProjMatrix->_13;
	StereoProjectionMatrix._14 = pProjMatrix->_14;
	StereoProjectionMatrix._21 = pProjMatrix->_21;
	StereoProjectionMatrix._22 = pProjMatrix->_22;
	StereoProjectionMatrix._23 = pProjMatrix->_23;
	StereoProjectionMatrix._24 = pProjMatrix->_24;
	StereoProjectionMatrix._31 = A * pProjMatrix->_11 + pProjMatrix->_31;
	StereoProjectionMatrix._32 = A * pProjMatrix->_12 + pProjMatrix->_32;
	StereoProjectionMatrix._33 = A * pProjMatrix->_13 + pProjMatrix->_33;
	StereoProjectionMatrix._34 = A * pProjMatrix->_14 + pProjMatrix->_34;
	StereoProjectionMatrix._41 = B * pProjMatrix->_11 + pProjMatrix->_41;
	StereoProjectionMatrix._42 = B * pProjMatrix->_12 + pProjMatrix->_42;
	StereoProjectionMatrix._43 = B * pProjMatrix->_13 + pProjMatrix->_43;
	StereoProjectionMatrix._44 = B * pProjMatrix->_14 + pProjMatrix->_44;

	// for shader skew matrix
	D3DXMatrixMultiply(&transformationMatrix, pInvProjection, &StereoProjectionMatrix);
}

const ShaderWrapper* GetShaderWrapper( const ShaderPipelineStates& pipeline )
{
	const Commands::xxSetShader* pShader = static_cast<const Commands::xxSetShader*>( pipeline.m_Shader.get() );
	ShaderWrapper* pShaderWrapper = NULL;
	if ( pShader )
		InitWrapper( pShader->hShader_, pShaderWrapper );
	return pShaderWrapper;
}

#ifndef FINAL_RELEASE

void D3DDeviceWrapper::DumpRT(bool b2RT, bool bStereo, DumpView view)
{
	TCHAR szFileName[MAX_PATH];
	TCHAR resourceName[128];
	TCHAR additionalString[128] = _T("");
	TCHAR temp[128];
	TCHAR EIDText[20];
	EIDText[0] = '\0';
	if (gInfo.EID != 0)
		_stprintf_s(EIDText, L"[%.4I64d].", gInfo.EID);

	const ShaderWrapper* VS = GetShaderWrapper( m_DeviceStateAtExecute.VS );
	const ShaderWrapper* GS = GetShaderWrapper( m_DeviceStateAtExecute.GS );
	const ShaderWrapper* PS = GetShaderWrapper( m_DeviceStateAtExecute.PS );
	const ShaderWrapper* HS = GetD3DVersion() >= TD3DVersion_11_0 ? GetShaderWrapper( m_DeviceStateAtExecute.HS ) : NULL;
	const ShaderWrapper* DS = GetD3DVersion() >= TD3DVersion_11_0 ? GetShaderWrapper( m_DeviceStateAtExecute.DS ) : NULL;
	if (VS != NULL)
	{
		_stprintf_s(temp, L"VS #%d (CRC 0x%X) ", 
			VS->m_ShaderIndex, VS->m_CRC32);
		_tcscat(additionalString, temp);
	}
	if (HS != NULL)
	{
		_stprintf_s(temp, L"HS #%d (CRC 0x%X)" ,
			HS->m_ShaderIndex, HS->m_CRC32);
		_tcscat(additionalString, temp);
	}
	if (DS != NULL)
	{
		_stprintf_s(temp, L"DS #%d (CRC 0x%X)" ,
			DS->m_ShaderIndex, DS->m_CRC32);
		_tcscat(additionalString, temp);
	}
	if (GS != NULL)
	{
		_stprintf_s(temp, L"GS #%d (CRC 0x%X) ", 
			GS->m_ShaderIndex, GS->m_CRC32);
		_tcscat(additionalString, temp);
	}
	if (PS != NULL)
	{
		_stprintf_s(temp, L"PS #%d (CRC 0x%X) ", 
			PS->m_ShaderIndex, PS->m_CRC32);
		_tcscat(additionalString, temp);
	}
	size_t len = _tcslen(additionalString);
	if (len > 0)
		additionalString[len - 1] = '\0'; // remove last space

	Commands::SetRenderTargets* pRTs = (Commands::SetRenderTargets*)m_DeviceStateAtExecute.m_OMRenderTargets.get();
	for (UINT i = 0 ; i < pRTs->NumValues_; i++)
	{
		RenderTargetViewWrapper* pRTV;
		InitWrapper(pRTs->Values_[i], pRTV);
		if (pRTV)
		{
			ResourceWrapper* pRT = pRTV->GetResourceWrapper();
			int resID = m_ResourceManager.GetID(pRT->GetHandle());
			if (view == dvBoth)
			{
				_stprintf_s(szFileName, L"%s\\%s%.4d.RT%d.", 
					m_DumpDirectory, EIDText, m_CBCount, i);
			}
			else
			{
				_stprintf_s(szFileName, L"%s\\%s%.4d.%s.%.4d.RT%d.", 
					m_DumpDirectory, EIDText, m_CBCount, 
					view == dvLeft ? "L" : "R", m_CBSubIndexCount, i);
			}
			_stprintf_s(resourceName, L"%s%d(%s)", pRT->GetResourceName(), resID, additionalString);
			pRT->DumpToFile(this, szFileName, resourceName, b2RT, bStereo, view);
		}
	}
	if (GetD3DVersion() >= TD3DVersion_11_0)
	{
		Commands::SetRenderTargets11_0* pRT11s = (Commands::SetRenderTargets11_0*)m_DeviceStateAtExecute.m_OMRenderTargets.get();
		for (UINT i = 0 ; i < pRT11s->NumUAVs_; i++)
		{
			UnorderedAccessViewWrapper* pUAV = 0;
			InitWrapper(pRT11s->phUnorderedAccessView_[i], pUAV);
			if (pUAV)
			{
				ResourceWrapper* pRes = pUAV->GetResourceWrapper();
				if (!pRes->IsTexture()) {
					continue;
				}

				int resID = m_ResourceManager.GetID(pRes->GetHandle());
				if (view == dvBoth)
				{
					_stprintf_s(szFileName, L"%s\\%s%.4d.UAV%d.", 
						m_DumpDirectory, EIDText, m_CBCount, i + pRT11s->UAVIndex_);
				}
				else
				{
					_stprintf_s(szFileName, L"%s\\%s%.4d.%s.%.4d.UAV%d.", 
						m_DumpDirectory, EIDText, m_CBCount, 
						view == dvLeft ? "L" : "R", m_CBSubIndexCount, i + pRT11s->UAVIndex_);
				}
				_stprintf_s(resourceName, L"%s%d(%s)", pRes->GetResourceName(), resID, additionalString);
				pRes->DumpToFile(this, szFileName, resourceName, b2RT, bStereo, view);
			}
		}
	}
}

void D3DDeviceWrapper::DumpUAVs(bool b2RT, bool bStereo, DumpView view)
{
	if (!m_DeviceStateAtExecute.m_CsUnorderedAccessViews)
		return;

	TCHAR szFileName[MAX_PATH];
	TCHAR resourceName[128];
	TCHAR additionalString[128] = _T("");
	TCHAR temp[128];
	TCHAR EIDText[20];
	EIDText[0] = '\0';
	if (gInfo.EID != 0)
		_stprintf_s(EIDText, L"[%.4I64d].", gInfo.EID);

	const ShaderWrapper* CS = GetD3DVersion() >= TD3DVersion_11_0 ? GetShaderWrapper( m_DeviceStateAtExecute.CS ) : NULL;
	if (CS != NULL)
	{
		_stprintf_s(temp, L"CS #%d (CRC 0x%X) ", 
			CS->m_ShaderIndex, CS->m_CRC32);
		_tcscat(additionalString, temp);
	}
	size_t len = _tcslen(additionalString);
	if (len > 0)
		additionalString[len - 1] = '\0'; // remove last space

	Commands::CsSetUnorderedAccessViews11_0* pUAVs = (Commands::CsSetUnorderedAccessViews11_0*)m_DeviceStateAtExecute.m_CsUnorderedAccessViews.get();
	for (UINT i = 0 ; i < pUAVs->NumViews_; i++)
	{
		UnorderedAccessViewWrapper* pUAV = 0;
		InitWrapper(pUAVs->phUnorderedAccessView_[i], pUAV);
		if (pUAV)
		{
			ResourceWrapper* pRes = pUAV->GetResourceWrapper();
			if (!pRes->IsTexture()) {
				continue;
			}
			
			int resID = m_ResourceManager.GetID(pRes->GetHandle());
			if (view == dvBoth)
			{
				_stprintf_s(szFileName, L"%s\\%s%.4d.UAV%d.", 
					m_DumpDirectory, EIDText, m_CBCount, i);
			}
			else
			{
				_stprintf_s(szFileName, L"%s\\%s%.4d.%s.%.4d.UAV%d.", 
					m_DumpDirectory, EIDText, m_CBCount, 
					view == dvLeft ? "L" : "R", m_CBSubIndexCount, i);
			}
			_stprintf_s(resourceName, L"%s%d(%s)", pRes->GetResourceName(), resID, additionalString);
			pRes->DumpToFile(this, szFileName, resourceName, b2RT, bStereo, view);
		}
	}
}

void D3DDeviceWrapper::DumpTextures(bool b2RT, DumpView view)
{
	if( !(GINFO_DUMP_ON && m_DumpType == dtFull) )
		return;
	DumpPipelineTextures(m_DeviceStateAtExecute.PS, _T("PS"), b2RT, view);
	DumpPipelineTextures(m_DeviceStateAtExecute.VS, _T("VS"), b2RT, view);
	DumpPipelineTextures(m_DeviceStateAtExecute.GS, _T("GS"), b2RT, view);
	if (GetD3DVersion() >= TD3DVersion_11_0)
	{
		DumpPipelineTextures(m_DeviceStateAtExecute.HS, _T("HS"), b2RT, view);
		DumpPipelineTextures(m_DeviceStateAtExecute.DS, _T("DS"), b2RT, view);
	}
}

void D3DDeviceWrapper::DumpCSTextures(bool b2RT, DumpView view)
{
	if( !(GINFO_DUMP_ON && m_DumpType == dtFull) )
		return;
	if (GetD3DVersion() >= TD3DVersion_11_0)
		DumpPipelineTextures(m_DeviceStateAtExecute.CS, _T("CS"), b2RT, view);
}

void D3DDeviceWrapper::DumpPipelineTextures( const ShaderPipelineStates &pipeline, const TCHAR* prefix, bool b2RT, DumpView view )
{
	Commands::xxSetShader* pShader = (Commands::xxSetShader*)pipeline.m_Shader.get();
	if (pShader && pShader->hShader_.pDrvPrivate)
	{
		ShaderWrapper* pShaderWrapper = (ShaderWrapper*)pShader->hShader_.pDrvPrivate;
		Commands::xxSetShaderResources* shaderResoures = (Commands::xxSetShaderResources*)pipeline.m_ShaderResources.get();
		if (shaderResoures)
			DumpPipelineTextures(shaderResoures, pShaderWrapper->m_UserResourcesMask, prefix, b2RT, view);
	}
}

void D3DDeviceWrapper::DumpPipelineTextures(const Commands::xxSetShaderResources* xxRes, const std::bitset<128>& mask, const TCHAR* prefix, bool bRT, DumpView view)
{
	TCHAR szFileName[MAX_PATH];
	TCHAR resourceName[128];
	TCHAR EIDText[20];
	EIDText[0] = '\0';
	if (gInfo.EID != 0)
		_stprintf_s(EIDText, L"[%.4I64d].", gInfo.EID);
	for(UINT i=0; i< xxRes->NumValues_; i++)
		if(xxRes->Values_[i].pDrvPrivate && mask.test(i))
		{
			ShaderResourceViewWrapper* pWrp;
			InitWrapper(xxRes->Values_[i], pWrp);
			ResourceWrapper* pResWrp = pWrp->GetResourceWrapper();
			int resID = m_ResourceManager.GetID(pResWrp->GetHandle());
			bool bStereoState = pResWrp->IsStereoState();
			bool bCurrentStereoState = bRT && bStereoState;
			if (view == dvBoth)
			{
				_stprintf_s(szFileName, L"%s\\%s%.4d.TX.%s.%02d.", 
					m_DumpDirectory, EIDText, m_CBCount, prefix, i);
			}
			else
			{
				_stprintf_s(szFileName, L"%s\\%s%.4d.%s.%.4d.TX.%s.%02d.", 
					m_DumpDirectory, EIDText, m_CBCount, 
					view == dvLeft ? "L" : "R", m_CBSubIndexCount, prefix, i);
			}
			_stprintf_s(resourceName, L"%s%d", pResWrp->GetResourceName(), resID);
			DumpView curView = dvBoth;
			if (view != dvBoth)
				curView = (view == dvLeft) || !bCurrentStereoState ? dvLeft : dvRight;
			pResWrp->DumpToFile(this, szFileName, resourceName, bStereoState, bCurrentStereoState, curView);
		}
}

#endif // FINAL_RELEASE

void D3DDeviceWrapper::WrapCommand( Commands::CreateResource* pCmd_ )
{
#if ENABLE_BUFFERING
	WrapCreateResource(pCmd_->hResource_);
#endif // ENABLE_BUFFERING
	// process CBs
	AddCommand(pCmd_);
}

void D3DDeviceWrapper::WrapCommand( Commands::CreateResource11_0* pCmd_ )
{
#if ENABLE_BUFFERING
	WrapCreateResource(pCmd_->hResource_);
#endif // ENABLE_BUFFERING
	// process CBs
	AddCommand(pCmd_);
}

void D3DDeviceWrapper::WrapCreateResource( D3D10DDI_HRESOURCE hResource )
{
	// ignore textures && large resources
	ResourceWrapper* pRes;
	InitWrapper(hResource, pRes);

	D3D10DDIARG_CREATERESOURCE &CreateResource = pRes->m_CreateResource;

	bool   bBuffer      = (CreateResource.ResourceDimension == D3D10DDIRESOURCE_BUFFER) || (CreateResource.ResourceDimension == D3D11DDIRESOURCE_BUFFEREX);
	bool   bCB          = (CreateResource.BindFlags == D3D10_DDI_BIND_CONSTANT_BUFFER);
	bool   bImmutable   = (CreateResource.Usage == D3D10_DDI_USAGE_IMMUTABLE);
	size_t resourceSize = pRes->Size(0);
	
	pRes->m_EnableCaching = (bBuffer || !m_BufferingOnlyBuffers) && (bCB || !m_BufferingOnlyCB) && !bImmutable && (resourceSize <= m_BufferingMaxSize);
	if (pRes->m_EnableCaching)
	{
		// NOTE: CreateResource.pInitialDataUP->pSysMem can be NULL, but we handle it in MakeInternalDataCopy

		// Create command for buffer restoration
		if (bCB)
		{
			pRes->m_pRestoreCmd.reset(
				new Commands::DefaultConstantBufferUpdateSubresourceUP( hResource, 
				0, 
				0, 
				CreateResource.pInitialDataUP ? CreateResource.pInitialDataUP->pSysMem : 0,
				0,
				0 ) );
		}
		else
		{
			pRes->m_pRestoreCmd.reset(
				new Commands::ResourceUpdateSubresourceUP( hResource, 
				0, 
				0, 
				CreateResource.pInitialDataUP ? CreateResource.pInitialDataUP->pSysMem : 0,
				0,
				0 ) );
		}

		if ( !pRes->m_pRestoreCmd->MakeInternalDataCopy(this) ) {
			pRes->m_pRestoreCmd.reset();
		}
	}

#ifndef FINAL_RELEASE
	const char* status = "";
	if (pRes->m_EnableCaching) {
		status = pRes->m_pRestoreCmd ? "ENABLED" : "FAILED";
	}
	else {
		status = "DISABLED";
	}

	DEBUG_TRACE3( _T("Resource buffering status 0x%p(buffer = %d, cb = %d, immutable = %d, size = %d): %S"), pRes->GetHandle().pDrvPrivate, bBuffer, bCB, bImmutable, resourceSize, status  );
#endif
}

void D3DDeviceWrapper::WrapCommand( Commands::xxUpdateSubresourceUP* pCmd_ )
{
	Commands::TCmdPtr guard(pCmd_);
	AddCommand(pCmd_);

#if ENABLE_BUFFERING_USU
	// Update resource restoration command
	ResourceWrapper* pRes;
	InitWrapper(pCmd_->hDstResource_, pRes);

	if (pRes->m_EnableCaching)
	{
		if (pRes->m_pRestoreCmd) 
		{
			if (pCmd_->pDstBox_) 
			{
				Commands::xxUpdateSubresourceUP& cmd = static_cast<Commands::xxUpdateSubresourceUP&>(*pRes->m_pRestoreCmd);
				if ( !cmd.Merge(this, *pCmd_) ) 
				{
					pRes->m_pRestoreCmd.reset();
					DEBUG_TRACE3( _T("Can't merge resource restoration commands, resource restoration command erased") );
				}
				else if ( !pCmd_->MakeInternalDataCopy(this) ) 
				{
					pRes->m_pRestoreCmd.reset();
					DEBUG_TRACE3( _T("Can't allocate memory for restoration restore command, resource restoration command erased") );
				}
			}
			else if ( pCmd_->MakeInternalDataCopy(this) ) {
				pRes->m_pRestoreCmd = pCmd_;
			}
		}
		else if ( pCmd_->MakeInternalDataCopy(this) ) {
			pRes->m_pRestoreCmd = pCmd_;
		}
	}
	else {
		pRes->m_pRestoreCmd.reset();
	}
#endif // ENABLE_BUFFERING_USU
}

void D3DDeviceWrapper::WrapCommand( Commands::xxMap* pCmd_ )
{
	Commands::TCmdPtr guard(pCmd_);
	
#if ENABLE_BUFFERING_MAP
	ResourceWrapper* pWrp;
	InitWrapper(pCmd_->hResource_, pWrp);
	if (pWrp->m_EnableCaching && pWrp->m_pRestoreCmd) {
		 pCmd_->CreateUpdateCommand(this, pWrp->m_pRestoreCmd.get()) ;
	}
#endif
	AddCommand(pCmd_);
#if ENABLE_BUFFERING_MAP
	// Remove resource restoration command, we can't restore mapped resources
	pWrp->m_pCurrentMapCmd = pCmd_;
#endif
}

void D3DDeviceWrapper::WrapCommand( Commands::xxUnmap* pCmd_ )
{
	Commands::TCmdPtr guard(pCmd_);
	AddCommand(pCmd_);

#if ENABLE_BUFFERING
	// Remove resource restoration command, we can't restore mapped resources
	ResourceWrapper* pWrp;
	InitWrapper(pCmd_->hResource_, pWrp);
	pWrp->m_pRestoreCmd.reset();
#if ENABLE_BUFFERING_MAP
	// Update resource restoration command
	if (pWrp->m_EnableCaching && pWrp->m_pCurrentMapCmd) {
		pWrp->m_pRestoreCmd = pWrp->m_pCurrentMapCmd->UpdateCommand_;
	}
	pWrp->m_pCurrentMapCmd.reset();
#endif // ENABLE_BUFFERING_MAP
#endif // ENABLE_BUFFERING
}

void D3DDeviceWrapper::HookDeviceFuncs( struct D3D10DDI_DEVICEFUNCS * pDeviceFuncs )
{
	memcpy(&OriginalDeviceFuncs, pDeviceFuncs, sizeof(D3D10DDI_DEVICEFUNCS) );
	ZeroMemory(pDeviceFuncs, sizeof(*pDeviceFuncs));
#define SET_FUNC(x) pDeviceFuncs->pfn##x = ::##x;
	// Order of functions is in decreasing order of priority ( as far as performance is concerned ).
	// !!! BEGIN HIGH-FREQUENCY !!!
	SET_FUNC(DefaultConstantBufferUpdateSubresourceUP);
	SET_FUNC(VsSetConstantBuffers);
	SET_FUNC(PsSetShaderResources);
	SET_FUNC(PsSetShader);
	SET_FUNC(PsSetSamplers);
	SET_FUNC(VsSetShader);
	SET_FUNC(DrawIndexed);
	SET_FUNC(Draw);
	SET_FUNC(DynamicIABufferMapNoOverwrite);
	SET_FUNC(DynamicIABufferUnmap);
	SET_FUNC(DynamicConstantBufferMapDiscard);
	SET_FUNC(DynamicIABufferMapDiscard);
	SET_FUNC(DynamicConstantBufferUnmap);
	SET_FUNC(PsSetConstantBuffers);
	SET_FUNC(IaSetInputLayout);
	SET_FUNC(IaSetVertexBuffers);
	SET_FUNC(IaSetIndexBuffer);
	// !!! END HIGH-FREQUENCY !!!

	// Order of functions is in decreasing order of priority ( as far as performance is concerned ).
	// !!! BEGIN MIDDLE-FREQUENCY !!!
	SET_FUNC(DrawIndexedInstanced);
	SET_FUNC(DrawInstanced);
	SET_FUNC(DynamicResourceMapDiscard);
	SET_FUNC(DynamicResourceUnmap);
	SET_FUNC(GsSetConstantBuffers);
	SET_FUNC(GsSetShader);
	SET_FUNC(IaSetTopology);
	SET_FUNC(StagingResourceMap);
	SET_FUNC(StagingResourceUnmap);
	SET_FUNC(VsSetShaderResources);
	SET_FUNC(VsSetSamplers);
	SET_FUNC(GsSetShaderResources);
	SET_FUNC(GsSetSamplers);
	SET_FUNC(SetRenderTargets);
	SET_FUNC(ShaderResourceViewReadAfterWriteHazard);
	SET_FUNC(ResourceReadAfterWriteHazard);
	SET_FUNC(SetBlendState);
	SET_FUNC(SetDepthStencilState);
	SET_FUNC(SetRasterizerState);
	SET_FUNC(QueryEnd);
	SET_FUNC(QueryBegin);
	SET_FUNC(ResourceCopyRegion);
	SET_FUNC(ResourceUpdateSubresourceUP);
	SET_FUNC(SoSetTargets);
	SET_FUNC(DrawAuto);
	SET_FUNC(SetViewports);
	SET_FUNC(SetScissorRects);
	SET_FUNC(ClearRenderTargetView);
	SET_FUNC(ClearDepthStencilView);
	SET_FUNC(SetPredication);
	SET_FUNC(QueryGetData);
	SET_FUNC(Flush);
	SET_FUNC(GenMips);
	SET_FUNC(ResourceCopy);
	SET_FUNC(ResourceResolveSubresource);
	// !!! END MIDDLE-FREQUENCY !!!

	// Infrequent paths:
	SET_FUNC(ResourceMap);
	SET_FUNC(ResourceUnmap);
	SET_FUNC(ResourceIsStagingBusy);
	SET_FUNC(RelocateDeviceFuncs);
	SET_FUNC(CalcPrivateResourceSize);
	SET_FUNC(CalcPrivateOpenedResourceSize);
	SET_FUNC(CreateResource);
	SET_FUNC(OpenResource);
	SET_FUNC(DestroyResource);
	SET_FUNC(CalcPrivateShaderResourceViewSize);
	SET_FUNC(CreateShaderResourceView);
	SET_FUNC(DestroyShaderResourceView);
	SET_FUNC(CalcPrivateRenderTargetViewSize);
	SET_FUNC(CreateRenderTargetView);
	SET_FUNC(DestroyRenderTargetView);
	SET_FUNC(CalcPrivateDepthStencilViewSize);
	SET_FUNC(CreateDepthStencilView);
	SET_FUNC(DestroyDepthStencilView);
	SET_FUNC(CalcPrivateElementLayoutSize);
	SET_FUNC(CreateElementLayout);
	SET_FUNC(DestroyElementLayout);
	SET_FUNC(CalcPrivateBlendStateSize);
	SET_FUNC(CreateBlendState);
	SET_FUNC(DestroyBlendState);
	SET_FUNC(CalcPrivateDepthStencilStateSize);
	SET_FUNC(CreateDepthStencilState);
	SET_FUNC(DestroyDepthStencilState);
	SET_FUNC(CalcPrivateRasterizerStateSize);
	SET_FUNC(CreateRasterizerState);
	SET_FUNC(DestroyRasterizerState);
	SET_FUNC(CalcPrivateShaderSize);
	SET_FUNC(CreateVertexShader);
	SET_FUNC(CreateGeometryShader);
	SET_FUNC(CreatePixelShader);
	SET_FUNC(CalcPrivateGeometryShaderWithStreamOutput);
	SET_FUNC(CreateGeometryShaderWithStreamOutput);
	SET_FUNC(DestroyShader);
	SET_FUNC(CalcPrivateSamplerSize);
	SET_FUNC(CreateSampler);
	SET_FUNC(DestroySampler);
	SET_FUNC(CalcPrivateQuerySize);
	SET_FUNC(CreateQuery);
	SET_FUNC(DestroyQuery);

	SET_FUNC(CheckFormatSupport);
	SET_FUNC(CheckMultisampleQualityLevels);
	SET_FUNC(CheckCounterInfo);
	SET_FUNC(CheckCounter);

	SET_FUNC(DestroyDevice);
	SET_FUNC(SetTextFilterSize);
#undef SET_FUNC
	CheckFunctions(&OriginalDeviceFuncs, pDeviceFuncs);
}

void D3DDeviceWrapper::HookDeviceFuncs( struct D3D10_1DDI_DEVICEFUNCS * pDeviceFuncs )
{
	memcpy(&OriginalDeviceFuncs10_1, pDeviceFuncs, sizeof(D3D10_1DDI_DEVICEFUNCS) );
	ZeroMemory(pDeviceFuncs, sizeof(*pDeviceFuncs));
#define SET_FUNC(x) pDeviceFuncs->pfn##x = ::##x;
#define SET_FUNC_10_1(x) pDeviceFuncs->pfn##x = ::##x ##10_1;
	// Order of functions is in decreasing order of priority ( as far as performance is concerned ).
	// !!! BEGIN HIGH-FREQUENCY !!!
	SET_FUNC(DefaultConstantBufferUpdateSubresourceUP);
	SET_FUNC(VsSetConstantBuffers);
	SET_FUNC(PsSetShaderResources);
	SET_FUNC(PsSetShader);
	SET_FUNC(PsSetSamplers);
	SET_FUNC(VsSetShader);
	SET_FUNC(DrawIndexed);
	SET_FUNC(Draw);
	SET_FUNC(DynamicIABufferMapNoOverwrite);
	SET_FUNC(DynamicIABufferUnmap);
	SET_FUNC(DynamicConstantBufferMapDiscard);
	SET_FUNC(DynamicIABufferMapDiscard);
	SET_FUNC(DynamicConstantBufferUnmap);
	SET_FUNC(PsSetConstantBuffers);
	SET_FUNC(IaSetInputLayout);
	SET_FUNC(IaSetVertexBuffers);
	SET_FUNC(IaSetIndexBuffer);
	// !!! END HIGH-FREQUENCY !!!

	// Order of functions is in decreasing order of priority ( as far as performance is concerned ).
	// !!! BEGIN MIDDLE-FREQUENCY !!!
	SET_FUNC(DrawIndexedInstanced);
	SET_FUNC(DrawInstanced);
	SET_FUNC(DynamicResourceMapDiscard);
	SET_FUNC(DynamicResourceUnmap);
	SET_FUNC(GsSetConstantBuffers);
	SET_FUNC(GsSetShader);
	SET_FUNC(IaSetTopology);
	SET_FUNC(StagingResourceMap);
	SET_FUNC(StagingResourceUnmap);
	SET_FUNC(VsSetShaderResources);
	SET_FUNC(VsSetSamplers);
	SET_FUNC(GsSetShaderResources);
	SET_FUNC(GsSetSamplers);
	SET_FUNC(SetRenderTargets);
	SET_FUNC(ShaderResourceViewReadAfterWriteHazard);
	SET_FUNC(ResourceReadAfterWriteHazard);
	SET_FUNC(SetBlendState);
	SET_FUNC(SetDepthStencilState);
	SET_FUNC(SetRasterizerState);
	SET_FUNC(QueryEnd);
	SET_FUNC(QueryBegin);
	SET_FUNC(ResourceCopyRegion);
	SET_FUNC(ResourceUpdateSubresourceUP);
	SET_FUNC(SoSetTargets);
	SET_FUNC(DrawAuto);
	SET_FUNC(SetViewports);
	SET_FUNC(SetScissorRects);
	SET_FUNC(ClearRenderTargetView);
	SET_FUNC(ClearDepthStencilView);
	SET_FUNC(SetPredication);
	SET_FUNC(QueryGetData);
	SET_FUNC(Flush);
	SET_FUNC(GenMips);
	SET_FUNC(ResourceCopy);
	SET_FUNC(ResourceResolveSubresource);
	// !!! END MIDDLE-FREQUENCY !!!

	// Infrequent paths:
	SET_FUNC(ResourceMap);
	SET_FUNC(ResourceUnmap);
	SET_FUNC(ResourceIsStagingBusy);
	SET_FUNC_10_1(RelocateDeviceFuncs);
	SET_FUNC(CalcPrivateResourceSize);
	SET_FUNC(CalcPrivateOpenedResourceSize);
	SET_FUNC(CreateResource);
	SET_FUNC(OpenResource);
	SET_FUNC(DestroyResource);
	SET_FUNC_10_1(CalcPrivateShaderResourceViewSize);
	SET_FUNC_10_1(CreateShaderResourceView);
	SET_FUNC(DestroyShaderResourceView);
	SET_FUNC(CalcPrivateRenderTargetViewSize);
	SET_FUNC(CreateRenderTargetView);
	SET_FUNC(DestroyRenderTargetView);
	SET_FUNC(CalcPrivateDepthStencilViewSize);
	SET_FUNC(CreateDepthStencilView);
	SET_FUNC(DestroyDepthStencilView);
	SET_FUNC(CalcPrivateElementLayoutSize);
	SET_FUNC(CreateElementLayout);
	SET_FUNC(DestroyElementLayout);
	SET_FUNC_10_1(CalcPrivateBlendStateSize);
	SET_FUNC_10_1(CreateBlendState);
	SET_FUNC(DestroyBlendState);
	SET_FUNC(CalcPrivateDepthStencilStateSize);
	SET_FUNC(CreateDepthStencilState);
	SET_FUNC(DestroyDepthStencilState);
	SET_FUNC(CalcPrivateRasterizerStateSize);
	SET_FUNC(CreateRasterizerState);
	SET_FUNC(DestroyRasterizerState);
	SET_FUNC(CalcPrivateShaderSize);
	SET_FUNC(CreateVertexShader);
	SET_FUNC(CreateGeometryShader);
	SET_FUNC(CreatePixelShader);
	SET_FUNC(CalcPrivateGeometryShaderWithStreamOutput);
	SET_FUNC(CreateGeometryShaderWithStreamOutput);
	SET_FUNC(DestroyShader);
	SET_FUNC(CalcPrivateSamplerSize);
	SET_FUNC(CreateSampler);
	SET_FUNC(DestroySampler);
	SET_FUNC(CalcPrivateQuerySize);
	SET_FUNC(CreateQuery);
	SET_FUNC(DestroyQuery);

	SET_FUNC(CheckFormatSupport);
	SET_FUNC(CheckMultisampleQualityLevels);
	SET_FUNC(CheckCounterInfo);
	SET_FUNC(CheckCounter);

	SET_FUNC(DestroyDevice);
	SET_FUNC(SetTextFilterSize);

	// Start additional 10.1 entries:
	SET_FUNC_10_1(ResourceConvert);
	SET_FUNC_10_1(ResourceConvertRegion);
#undef SET_FUNC_10_1
#undef SET_FUNC
	CheckFunctions(&OriginalDeviceFuncs10_1, pDeviceFuncs);
}

void D3DDeviceWrapper::HookDeviceFuncs( struct D3D11DDI_DEVICEFUNCS * pDeviceFuncs )
{
    memcpy(&OriginalDeviceFuncs11, pDeviceFuncs, sizeof(D3D11DDI_DEVICEFUNCS) );

	// Diagnostic: log entry and negotiated interface information
	DEBUG_MESSAGE(_T("HookDeviceFuncs: Enter (pDeviceFuncs=%p)\n"), pDeviceFuncs);

	// A2: extra slots beyond compiled size are SAFE to leave alone. The
	// runtime calls them with the unshifted hDevice (under A2 the wrapper
	// is a separate heap allocation, so we don't shift). Driver finds its
	// data at the unshifted address. We only zero+rewrite the slots we
	// actually wrap; extra slots retain the original driver pointers.
	if (HasExtraDeviceFuncs(pDeviceFuncs, sizeof(D3D11DDI_DEVICEFUNCS)))
	{
		DEBUG_MESSAGE(_T("HookDeviceFuncs: extra device-func slots present (safe under A2 side-table)\n"));
	}

	ZeroMemory(pDeviceFuncs, sizeof(*pDeviceFuncs));
#define SET_FUNC(x) pDeviceFuncs->pfn##x = ::##x;
#define SET_FUNC_10_1(x) pDeviceFuncs->pfn##x = ::##x ##10_1;
#define SET_FUNC_11(x) pDeviceFuncs->pfn##x = ::##x ##11_0;
	// Order of functions is in decreasing order of priority ( as far as performance is concerned ).
	// !!! BEGIN HIGH-FREQUENCY !!!
	SET_FUNC(DefaultConstantBufferUpdateSubresourceUP);
	SET_FUNC(VsSetConstantBuffers);
	SET_FUNC(PsSetShaderResources);
	SET_FUNC(PsSetShader);
	SET_FUNC(PsSetSamplers);
	SET_FUNC(VsSetShader);
	SET_FUNC(DrawIndexed);
	SET_FUNC(Draw);
	SET_FUNC(DynamicIABufferMapNoOverwrite);
	SET_FUNC(DynamicIABufferUnmap);
	SET_FUNC(DynamicConstantBufferMapDiscard);
	SET_FUNC(DynamicIABufferMapDiscard);
	SET_FUNC(DynamicConstantBufferUnmap);
	SET_FUNC(PsSetConstantBuffers);
	SET_FUNC(IaSetInputLayout);
	SET_FUNC(IaSetVertexBuffers);
	SET_FUNC(IaSetIndexBuffer);
	// !!! END HIGH-FREQUENCY !!!

	// Order of functions is in decreasing order of priority ( as far as performance is concerned ).
	// !!! BEGIN MIDDLE-FREQUENCY !!!
	SET_FUNC(DrawIndexedInstanced);
	SET_FUNC(DrawInstanced);
	SET_FUNC(DynamicResourceMapDiscard);
	SET_FUNC(DynamicResourceUnmap);
	SET_FUNC(GsSetConstantBuffers);
	SET_FUNC(GsSetShader);
	SET_FUNC(IaSetTopology);
	SET_FUNC(StagingResourceMap);
	SET_FUNC(StagingResourceUnmap);
	SET_FUNC(VsSetShaderResources);
	SET_FUNC(VsSetSamplers);
	SET_FUNC(GsSetShaderResources);
	SET_FUNC(GsSetSamplers);
	SET_FUNC_11(SetRenderTargets);
	SET_FUNC(ShaderResourceViewReadAfterWriteHazard);
	SET_FUNC(ResourceReadAfterWriteHazard);
	SET_FUNC(SetBlendState);
	SET_FUNC(SetDepthStencilState);
	SET_FUNC(SetRasterizerState);
	SET_FUNC(QueryEnd);
	SET_FUNC(QueryBegin);
	SET_FUNC(ResourceCopyRegion);
	SET_FUNC(ResourceUpdateSubresourceUP);
	SET_FUNC(SoSetTargets);
	SET_FUNC(DrawAuto);
	SET_FUNC(SetViewports);
	SET_FUNC(SetScissorRects);
	SET_FUNC(ClearRenderTargetView);
	SET_FUNC(ClearDepthStencilView);
	SET_FUNC(SetPredication);
	SET_FUNC(QueryGetData);
	SET_FUNC(Flush);
	SET_FUNC(GenMips);
	SET_FUNC(ResourceCopy);
	SET_FUNC(ResourceResolveSubresource);
	// !!! END MIDDLE-FREQUENCY !!!

	// Infrequent paths:
	SET_FUNC(ResourceMap);
	SET_FUNC(ResourceUnmap);
	SET_FUNC(ResourceIsStagingBusy);
	SET_FUNC_11(RelocateDeviceFuncs);
	SET_FUNC_11(CalcPrivateResourceSize);
	SET_FUNC(CalcPrivateOpenedResourceSize);
	SET_FUNC_11(CreateResource);
	SET_FUNC(OpenResource);
	SET_FUNC(DestroyResource);
	SET_FUNC_11(CalcPrivateShaderResourceViewSize);
	SET_FUNC_11(CreateShaderResourceView);
	SET_FUNC(DestroyShaderResourceView);
	SET_FUNC(CalcPrivateRenderTargetViewSize);
	SET_FUNC(CreateRenderTargetView);
	SET_FUNC(DestroyRenderTargetView);
	SET_FUNC_11(CalcPrivateDepthStencilViewSize);
	SET_FUNC_11(CreateDepthStencilView);
	SET_FUNC(DestroyDepthStencilView);
	SET_FUNC(CalcPrivateElementLayoutSize);
	SET_FUNC(CreateElementLayout);
	SET_FUNC(DestroyElementLayout);
	SET_FUNC_10_1(CalcPrivateBlendStateSize);
	SET_FUNC_10_1(CreateBlendState);
	SET_FUNC(DestroyBlendState);
	SET_FUNC(CalcPrivateDepthStencilStateSize);
	SET_FUNC(CreateDepthStencilState);
	SET_FUNC(DestroyDepthStencilState);
	SET_FUNC(CalcPrivateRasterizerStateSize);
	SET_FUNC(CreateRasterizerState);
	SET_FUNC(DestroyRasterizerState);
	SET_FUNC(CalcPrivateShaderSize);
	SET_FUNC(CreateVertexShader);
	SET_FUNC(CreateGeometryShader);
	SET_FUNC(CreatePixelShader);
	SET_FUNC_11(CalcPrivateGeometryShaderWithStreamOutput);
	SET_FUNC_11(CreateGeometryShaderWithStreamOutput);
	SET_FUNC(DestroyShader);
	SET_FUNC(CalcPrivateSamplerSize);
	SET_FUNC(CreateSampler);
	SET_FUNC(DestroySampler);
	SET_FUNC(CalcPrivateQuerySize);
	SET_FUNC(CreateQuery);
	SET_FUNC(DestroyQuery);

	SET_FUNC(CheckFormatSupport);
	SET_FUNC(CheckMultisampleQualityLevels);
	SET_FUNC(CheckCounterInfo);
	SET_FUNC(CheckCounter);

	SET_FUNC(DestroyDevice);
	SET_FUNC(SetTextFilterSize);

	// Start additional 10.1 entries:
	SET_FUNC_10_1(ResourceConvert);
	SET_FUNC_10_1(ResourceConvertRegion);

	// Start additional 11.0 entries:
	SET_FUNC_11(DrawIndexedInstancedIndirect);
	SET_FUNC_11(DrawInstancedIndirect);
	SET_FUNC_11(CommandListExecute); // Only required when supporting D3D11DDICAPS_COMMANDLISTS
	SET_FUNC_11(HsSetShaderResources);
	SET_FUNC_11(HsSetShader);
	SET_FUNC_11(HsSetSamplers);
	SET_FUNC_11(HsSetConstantBuffers);
	SET_FUNC_11(DsSetShaderResources);
	SET_FUNC_11(DsSetShader);
	SET_FUNC_11(DsSetSamplers);
	SET_FUNC_11(DsSetConstantBuffers);
	SET_FUNC_11(CreateHullShader);
	SET_FUNC_11(CreateDomainShader);
	SET_FUNC_11(CheckDeferredContextHandleSizes); // Only required when supporting D3D11DDICAPS_COMMANDLISTS
	SET_FUNC_11(CalcDeferredContextHandleSize); // Only required when supporting D3D11DDICAPS_COMMANDLISTS
	SET_FUNC_11(CalcPrivateDeferredContextSize); // Only required when supporting D3D11DDICAPS_COMMANDLISTS
	SET_FUNC_11(CreateDeferredContext); // Only required when supporting D3D11DDICAPS_COMMANDLISTS
	SET_FUNC_11(AbandonCommandList); // Only required when supporting D3D11DDICAPS_COMMANDLISTS
	SET_FUNC_11(CalcPrivateCommandListSize); // Only required when supporting D3D11DDICAPS_COMMANDLISTS
	SET_FUNC_11(CreateCommandList); // Only required when supporting D3D11DDICAPS_COMMANDLISTS
	SET_FUNC_11(DestroyCommandList); // Only required when supporting D3D11DDICAPS_COMMANDLISTS
	SET_FUNC_11(CalcPrivateTessellationShaderSize);
	SET_FUNC_11(PsSetShaderWithIfaces);
	SET_FUNC_11(VsSetShaderWithIfaces);
	SET_FUNC_11(GsSetShaderWithIfaces);
	SET_FUNC_11(HsSetShaderWithIfaces);
	SET_FUNC_11(DsSetShaderWithIfaces);
	SET_FUNC_11(CsSetShaderWithIfaces);
	SET_FUNC_11(CreateComputeShader);
	SET_FUNC_11(CsSetShader);
	SET_FUNC_11(CsSetShaderResources);
	SET_FUNC_11(CsSetSamplers);
	SET_FUNC_11(CsSetConstantBuffers);
	SET_FUNC_11(CalcPrivateUnorderedAccessViewSize);
	SET_FUNC_11(CreateUnorderedAccessView);
	SET_FUNC_11(DestroyUnorderedAccessView);
	SET_FUNC_11(ClearUnorderedAccessViewUint);
	SET_FUNC_11(ClearUnorderedAccessViewFloat);
	SET_FUNC_11(CsSetUnorderedAccessViews);
	SET_FUNC_11(Dispatch);
	SET_FUNC_11(DispatchIndirect);
	SET_FUNC_11(SetResourceMinLOD);
	SET_FUNC_11(CopyStructureCount);
#undef SET_FUNC_11
#undef SET_FUNC_10_1
#undef SET_FUNC
	CheckFunctions(&OriginalDeviceFuncs11, pDeviceFuncs);

	// Post-hook diagnostics: scan table for NULLs and pointers still pointing
	// to external modules (possible driver originals).
	{
		size_t slotCount = sizeof(D3D11DDI_DEVICEFUNCS) / sizeof(void*);
		void** slots = (void**)pDeviceFuncs;
		int nullCount = 0;
		int externalCount = 0;
		for (size_t i = 0; i < slotCount; i++)
		{
			void* v = NULL;
			__try { v = slots[i]; } __except(EXCEPTION_EXECUTE_HANDLER) { v = NULL; }
			if (!v) { nullCount++; continue; }
			HMODULE hMod = NULL;
			if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
								  GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
								  (LPCSTR)v, &hMod))
			{
				externalCount++;
				DEBUG_MESSAGE(_T("HookDeviceFuncs: slot[%u]=%p -> module present\n"), (unsigned)i, v);
			}
		}
		DEBUG_MESSAGE(_T("HookDeviceFuncs: post-hook slots=%u nulls=%d externals=%d\n"), (unsigned)slotCount, nullCount, externalCount);
	}
}

void D3DDeviceWrapper::HookDXGIFuncs( struct DXGI_DDI_BASE_FUNCTIONS  *pDXGIDDIBaseFunctions )
{
	memcpy(&OriginalDXGIDDIBaseFunctions, pDXGIDDIBaseFunctions, sizeof DXGI_DDI_BASE_FUNCTIONS);
	ZeroMemory(pDXGIDDIBaseFunctions, sizeof DXGI_DDI_BASE_FUNCTIONS);
#define SET_FUNC(x) if (OriginalDXGIDDIBaseFunctions.pfn##x) pDXGIDDIBaseFunctions->pfn##x = ::##x;
	SET_FUNC(Present);
	SET_FUNC(GetGammaCaps);
	SET_FUNC(SetDisplayMode);
	SET_FUNC(SetResourcePriority);
	SET_FUNC(QueryResourceResidency);
	SET_FUNC(RotateResourceIdentities);
	SET_FUNC(Blt);
#undef SET_FUNC
	CheckFunctions(&OriginalDXGIDDIBaseFunctions, pDXGIDDIBaseFunctions);
}

void D3DDeviceWrapper::HookDXGIFuncs( struct DXGI1_1_DDI_BASE_FUNCTIONS  *pDXGIDDIBaseFunctions )
{
	memcpy(&OriginalDXGIDDIBaseFunctions2, pDXGIDDIBaseFunctions, sizeof DXGI1_1_DDI_BASE_FUNCTIONS);
	ZeroMemory(pDXGIDDIBaseFunctions, sizeof DXGI1_1_DDI_BASE_FUNCTIONS);
#define SET_FUNC(x) if (OriginalDXGIDDIBaseFunctions2.pfn##x) pDXGIDDIBaseFunctions->pfn##x = ::##x;
	SET_FUNC(Present);
	SET_FUNC(GetGammaCaps);
	SET_FUNC(SetDisplayMode);
	SET_FUNC(SetResourcePriority);
	SET_FUNC(QueryResourceResidency);
	SET_FUNC(RotateResourceIdentities);
	SET_FUNC(Blt);
	SET_FUNC(ResolveSharedResource);
#undef SET_FUNC
	CheckFunctions(&OriginalDXGIDDIBaseFunctions2, pDXGIDDIBaseFunctions);
}

void D3DDeviceWrapper::SetInterfaceVersion ( UINT Interface )	
{
	m_nInterfaceVersion = Interface;

	m_tD3DVesion = GetD3DVersionFromInterface(Interface);
	_ASSERT( m_tD3DVesion != TD3DVersion_Unknown );

	m_DeviceState.Init(m_tD3DVesion);
#ifndef FINAL_RELEASE
	m_DeviceStateAtExecute.Init(m_tD3DVesion);
#endif
}

bool D3DDeviceWrapper::SkipPresenterThread()
{
	if (!USE_UM_PRESENTER)
		return false;
#ifndef FINAL_RELEASE
	if (m_bDebugUseSimplePresenter)
		return true;
	if (m_DumpType >= dtCommands)
		return false;
#endif
	//if (!m_Input.StereoActive)
	//	return true;
	return false;
}

template< typename T>
void D3DDeviceWrapper::AddCommand( T* pCmd_, bool bForceProcess_ )
{
#if defined(EXECUTE_IMMEDIATELY_G1) && defined(EXECUTE_IMMEDIATELY_G2) && defined(EXECUTE_IMMEDIATELY_G3)
	__debugbreak();
#endif
	_ASSERT( pCmd_ != NULL );
	m_CSCB.Enter();	

	boost::intrusive_ptr<T> guard(pCmd_); // make sure that command will be deleted if it nowhere stored
#ifndef FINAL_RELEASE
	Commands::Command::LastCommandId_ = pCmd_->CommandId;
	WriteStreamer::Get().SetCurrentRM(&m_ResourceManager);
#endif
	if ( m_StereoBuffer->Processing() )
	{
		m_StereoBuffer->AddCommand( pCmd_ );

		if ( bForceProcess_ ) {
			_ASSERT( m_StereoBuffer->HaveFinaleCommand() );
		}

		if ( m_StereoBuffer->WantProcess() ) {
			bForceProcess_ = true;
		}
	}
	else
	{
		m_MonoBuffer->AddCommand( pCmd_ );	

		if ( m_MonoBuffer->WantProcess() ) {
			bForceProcess_ = true;
		}
	}

#if 0 // DEBUG
	ProcessCB();
	m_CSUMCall.Enter();
	CallFlush();
	m_CSUMCall.Leave();
#else
	if ( bForceProcess_ )
	{
		ProcessCB();
		if (m_bFlushCommands)
			FlushCommands();
	}
#endif
	m_bFlushCommands = false;

	m_CSCB.Leave();
}

// Instantiate stuff
#define INST_ADD_COMMAND(Cmd) template void D3DDeviceWrapper::AddCommand<Commands::Cmd>(Commands::Cmd* pCmd_, bool bForceProcess_);

INST_ADD_COMMAND(PsSetConstantBuffers)
INST_ADD_COMMAND(DefaultConstantBufferUpdateSubresourceUP)
INST_ADD_COMMAND(VsSetConstantBuffers)
INST_ADD_COMMAND(PsSetShaderResources)
INST_ADD_COMMAND(PsSetShader)
INST_ADD_COMMAND(PsSetSamplers)
INST_ADD_COMMAND(VsSetShader)
INST_ADD_COMMAND(DrawIndexed)
INST_ADD_COMMAND(Draw)
INST_ADD_COMMAND(DynamicIABufferMapNoOverwrite)
INST_ADD_COMMAND(DynamicIABufferUnmap)
INST_ADD_COMMAND(DynamicConstantBufferMapDiscard)
INST_ADD_COMMAND(DynamicIABufferMapDiscard)
INST_ADD_COMMAND(DynamicConstantBufferUnmap)
INST_ADD_COMMAND(IaSetInputLayout)
INST_ADD_COMMAND(IaSetVertexBuffers)
INST_ADD_COMMAND(IaSetIndexBuffer)

INST_ADD_COMMAND(DrawIndexedInstanced)
INST_ADD_COMMAND(DrawInstanced)
INST_ADD_COMMAND(DynamicResourceMapDiscard)
INST_ADD_COMMAND(DynamicResourceUnmap)
INST_ADD_COMMAND(GsSetConstantBuffers)
INST_ADD_COMMAND(GsSetShader)
INST_ADD_COMMAND(IaSetTopology)
INST_ADD_COMMAND(StagingResourceMap)
INST_ADD_COMMAND(StagingResourceUnmap)
INST_ADD_COMMAND(VsSetShaderResources)
INST_ADD_COMMAND(VsSetSamplers)
INST_ADD_COMMAND(GsSetShaderResources)
INST_ADD_COMMAND(GsSetSamplers)
INST_ADD_COMMAND(SetRenderTargets)
INST_ADD_COMMAND(ShaderResourceViewReadAfterWriteHazard)
INST_ADD_COMMAND(ResourceReadAfterWriteHazard)
INST_ADD_COMMAND(SetBlendState)
INST_ADD_COMMAND(SetDepthStencilState)
INST_ADD_COMMAND(SetRasterizerState)
INST_ADD_COMMAND(QueryEnd)
INST_ADD_COMMAND(QueryBegin)
INST_ADD_COMMAND(ResourceCopyRegion)
INST_ADD_COMMAND(ResourceUpdateSubresourceUP)
INST_ADD_COMMAND(SoSetTargets)
INST_ADD_COMMAND(DrawAuto)
INST_ADD_COMMAND(SetViewports)
INST_ADD_COMMAND(SetScissorRects)
INST_ADD_COMMAND(ClearRenderTargetView)
INST_ADD_COMMAND(ClearDepthStencilView)
INST_ADD_COMMAND(SetPredication)
INST_ADD_COMMAND(QueryGetData)
INST_ADD_COMMAND(Flush)
INST_ADD_COMMAND(GenMips)
INST_ADD_COMMAND(ResourceCopy)
INST_ADD_COMMAND(ResourceResolveSubresource)

INST_ADD_COMMAND(ResourceMap)
INST_ADD_COMMAND(ResourceUnmap)
INST_ADD_COMMAND(ResourceIsStagingBusy)
INST_ADD_COMMAND(RelocateDeviceFuncs)
INST_ADD_COMMAND(CalcPrivateResourceSize)
INST_ADD_COMMAND(CalcPrivateOpenedResourceSize)
INST_ADD_COMMAND(CreateResource)
INST_ADD_COMMAND(OpenResource)
INST_ADD_COMMAND(DestroyResource)
INST_ADD_COMMAND(CalcPrivateShaderResourceViewSize)
INST_ADD_COMMAND(CreateShaderResourceView)
INST_ADD_COMMAND(DestroyShaderResourceView)
INST_ADD_COMMAND(CalcPrivateRenderTargetViewSize)
INST_ADD_COMMAND(CreateRenderTargetView)
INST_ADD_COMMAND(DestroyRenderTargetView)
INST_ADD_COMMAND(CalcPrivateDepthStencilViewSize)
INST_ADD_COMMAND(CreateDepthStencilView)
INST_ADD_COMMAND(DestroyDepthStencilView)
INST_ADD_COMMAND(CalcPrivateElementLayoutSize)
INST_ADD_COMMAND(CreateElementLayout)
INST_ADD_COMMAND(DestroyElementLayout)
INST_ADD_COMMAND(CalcPrivateBlendStateSize)
INST_ADD_COMMAND(CreateBlendState)
INST_ADD_COMMAND(DestroyBlendState)
INST_ADD_COMMAND(CalcPrivateDepthStencilStateSize)
INST_ADD_COMMAND(CreateDepthStencilState)
INST_ADD_COMMAND(DestroyDepthStencilState)
INST_ADD_COMMAND(CalcPrivateRasterizerStateSize)
INST_ADD_COMMAND(CreateRasterizerState)
INST_ADD_COMMAND(DestroyRasterizerState)
INST_ADD_COMMAND(CalcPrivateShaderSize)
INST_ADD_COMMAND(CreateVertexShader)
INST_ADD_COMMAND(CreateGeometryShader)
INST_ADD_COMMAND(CreatePixelShader)
INST_ADD_COMMAND(CalcPrivateGeometryShaderWithStreamOutput)
INST_ADD_COMMAND(CreateGeometryShaderWithStreamOutput)
INST_ADD_COMMAND(DestroyShader)
INST_ADD_COMMAND(CalcPrivateSamplerSize)
INST_ADD_COMMAND(CreateSampler)
INST_ADD_COMMAND(DestroySampler)
INST_ADD_COMMAND(CalcPrivateQuerySize)
INST_ADD_COMMAND(CreateQuery)
INST_ADD_COMMAND(DestroyQuery)

INST_ADD_COMMAND(CheckFormatSupport)
INST_ADD_COMMAND(CheckMultisampleQualityLevels)
INST_ADD_COMMAND(CheckCounterInfo)
INST_ADD_COMMAND(CheckCounter)

INST_ADD_COMMAND(DestroyDevice)
INST_ADD_COMMAND(SetTextFilterSize)

INST_ADD_COMMAND(RelocateDeviceFuncs10_1)
INST_ADD_COMMAND(CalcPrivateShaderResourceViewSize10_1)
INST_ADD_COMMAND(CreateShaderResourceView10_1)
INST_ADD_COMMAND(CalcPrivateBlendStateSize10_1)
INST_ADD_COMMAND(CreateBlendState10_1)
INST_ADD_COMMAND(ResourceConvert10_1)
INST_ADD_COMMAND(ResourceConvertRegion10_1)

INST_ADD_COMMAND(AbandonCommandList11_0)
INST_ADD_COMMAND(CalcDeferredContextHandleSize11_0)
INST_ADD_COMMAND(CalcPrivateCommandListSize11_0)
INST_ADD_COMMAND(CalcPrivateDeferredContextSize11_0)
INST_ADD_COMMAND(CalcPrivateDepthStencilViewSize11_0)
INST_ADD_COMMAND(CalcPrivateGeometryShaderWithStreamOutput11_0)
INST_ADD_COMMAND(CalcPrivateResourceSize11_0)
INST_ADD_COMMAND(CalcPrivateShaderResourceViewSize11_0)
INST_ADD_COMMAND(CalcPrivateTessellationShaderSize11_0)
INST_ADD_COMMAND(CalcPrivateUnorderedAccessViewSize11_0)
INST_ADD_COMMAND(CheckDeferredContextHandleSizes11_0)
INST_ADD_COMMAND(ClearUnorderedAccessViewFloat11_0)
INST_ADD_COMMAND(ClearUnorderedAccessViewUint11_0)
INST_ADD_COMMAND(CommandListExecute11_0)
INST_ADD_COMMAND(CopyStructureCount11_0)
INST_ADD_COMMAND(CreateCommandList11_0)
INST_ADD_COMMAND(CreateComputeShader11_0)
INST_ADD_COMMAND(CreateDeferredContext11_0)
INST_ADD_COMMAND(CreateDepthStencilView11_0)
INST_ADD_COMMAND(CreateDomainShader11_0)
INST_ADD_COMMAND(CreateGeometryShaderWithStreamOutput11_0)
INST_ADD_COMMAND(CreateHullShader11_0)
INST_ADD_COMMAND(CreateResource11_0)
INST_ADD_COMMAND(CreateShaderResourceView11_0)
INST_ADD_COMMAND(CreateUnorderedAccessView11_0)
INST_ADD_COMMAND(CsSetConstantBuffers11_0)
INST_ADD_COMMAND(CsSetSamplers11_0)
INST_ADD_COMMAND(CsSetShader11_0)
INST_ADD_COMMAND(CsSetShaderResources11_0)
INST_ADD_COMMAND(CsSetShaderWithIfaces11_0)
INST_ADD_COMMAND(CsSetUnorderedAccessViews11_0)
INST_ADD_COMMAND(DestroyCommandList11_0)
INST_ADD_COMMAND(DestroyUnorderedAccessView11_0)
INST_ADD_COMMAND(Dispatch11_0)
INST_ADD_COMMAND(DispatchIndirect11_0)
INST_ADD_COMMAND(DrawIndexedInstancedIndirect11_0)
INST_ADD_COMMAND(DrawInstancedIndirect11_0)
INST_ADD_COMMAND(DsSetConstantBuffers11_0)
INST_ADD_COMMAND(DsSetSamplers11_0)
INST_ADD_COMMAND(DsSetShader11_0)
INST_ADD_COMMAND(DsSetShaderResources11_0)
INST_ADD_COMMAND(DsSetShaderWithIfaces11_0)
INST_ADD_COMMAND(GsSetShaderWithIfaces11_0)
INST_ADD_COMMAND(HsSetConstantBuffers11_0)
INST_ADD_COMMAND(HsSetSamplers11_0)
INST_ADD_COMMAND(HsSetShader11_0)
INST_ADD_COMMAND(HsSetShaderResources11_0)
INST_ADD_COMMAND(HsSetShaderWithIfaces11_0)
INST_ADD_COMMAND(PsSetShaderWithIfaces11_0)
INST_ADD_COMMAND(RelocateDeviceFuncs11_0)
INST_ADD_COMMAND(SetRenderTargets11_0)
INST_ADD_COMMAND(SetResourceMinLOD11_0)
INST_ADD_COMMAND(VsSetShaderWithIfaces11_0)

INST_ADD_COMMAND(Present)
INST_ADD_COMMAND(GetGammaCaps)
INST_ADD_COMMAND(SetDisplayMode)
INST_ADD_COMMAND(SetResourcePriority)
INST_ADD_COMMAND(QueryResourceResidency)
INST_ADD_COMMAND(RotateResourceIdentities)
INST_ADD_COMMAND(Blt)
INST_ADD_COMMAND(ResolveSharedResource)

INST_ADD_COMMAND(ChangeDeviceView)
INST_ADD_COMMAND(ChangeShaderMatrixView)
INST_ADD_COMMAND(SetDestResourceType)

#undef INST_ADD_COMMAND