//

#include "stdafx.h"
#include "AdapterFunctions.h"
#include "D3DDeviceWrapper.h"
#include "DeviceWrapperRegistry.h"
#include "GlobalData.h"
#include <cstdarg>
#include <cstdio>

// FormatGUID's IID name table references the higher-version D3D11 and DXGI
// interfaces (Device3, Context3, SwapChain4, etc.). Pull the SDK headers in
// after the same DXGI_RGBA shim Device11Proxy.h uses — the bundled
// lib/d3d9/Include header chain is renamed to .legacy_unused but the shim
// stays defensive in case anyone reintroduces it.
#include <d3d9types.h>
#ifndef _DXGI_RGBA_DEFINED
#define _DXGI_RGBA_DEFINED
typedef D3DCOLORVALUE DXGI_RGBA;
#endif
#include <d3d11_3.h>
#include <dxgi1_5.h>

// ---------------------------------------------------------------------------
// Diagnostic: append to wiz3D_proxy.log next to the game exe so the wrapper's
// DDI path is visible in the same file as the proxy logs. ZLOg/DEBUG_MESSAGE
// don't write there. Crashes during wrapped DDI CreateDevice need this
// visibility to diagnose. Win32 FILE* I/O only — keeps this minimal.
// ---------------------------------------------------------------------------
void DDILog(const char* fmt, ...)
{
	static FILE* fp = NULL;
	if (!fp)
	{
		WCHAR dir[MAX_PATH];
		GetModuleFileNameW(NULL, dir, MAX_PATH);
		WCHAR* pSlash = wcsrchr(dir, L'\\');
		if (pSlash) *(pSlash + 1) = L'\0';
		lstrcatW(dir, L"wiz3D_proxy.log");
		fp = _wfopen(dir, L"a");
		if (fp) fputs("\n--- wrapper DDI hook fired ---\n", fp);
	}
	if (!fp) return;
	va_list ap;
	va_start(ap, fmt);
	vfprintf(fp, fmt, ap);
	va_end(ap);
	fflush(fp);
}

// ---------------------------------------------------------------------------
// Per-frame trace appender — emits into wiz3D_proxy.log (via DDILog) when
// gInfo.VerboseFrameTrace > 0. Counts down to 0 at each FrameTraceTickFrame()
// call (one per Present completion) and then stops. Previously wrote a
// separate wiz3D_frame_trace.log file; merged into the proxy log so testers
// only need to send a single file back.
// ---------------------------------------------------------------------------
int g_FrameTraceRemaining = -1;  // -1 = uninitialized, 0 = done, >0 = active
int g_FrameTraceLastFrameDraws = 0;

void FrameTrace(const char* fmt, ...)
{
	if (g_FrameTraceRemaining <= 0) return;
	va_list ap;
	va_start(ap, fmt);
	// Reuse DDILog's open-on-first-call FILE* — same wiz3D_proxy.log next to
	// the game exe. Allocate enough headroom for the longest formatted trace
	// line (OMSet with proxy/sibling info is the widest, ~200 chars).
	char buf[512];
	int n = _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
	va_end(ap);
	if (n > 0) DDILog("%s", buf);
}

void FrameTraceTickFrame()
{
	// Two states tracked here:
	//   g_FrameTracePresentsSeen — total Presents observed since wrapper init.
	//     Increments every frame regardless of trace state. Used by the
	//     FrameTraceStartFrame gate so a tester can skip past loading screens.
	//   g_FrameTraceRemaining — countdown of frames left to capture.
	//     -1 before the start gate fires, >0 while capturing, 0 when complete.
	static int s_presentsSeen = 0;
	++s_presentsSeen;

	if (g_FrameTraceRemaining < 0)
	{
		// Gate on FrameTraceStartFrame so loading screens don't burn our trace
		// budget. 0 (default) starts at the first Present; >0 waits until that
		// Present count is reached.
		int startAt = (int)gInfo.FrameTraceStartFrame;
		if (startAt > 0 && s_presentsSeen < startAt) return;
		// Draw-count gate: waits for a frame that actually rendered something,
		// so the capture lands on gameplay without guessing a frame number.
		if (gInfo.FrameTraceMinDraws > 0 &&
		    g_FrameTraceLastFrameDraws < (int)gInfo.FrameTraceMinDraws) return;

		g_FrameTraceRemaining = (int)gInfo.VerboseFrameTrace;
		if (g_FrameTraceRemaining > 0)
			FrameTrace("=== frame trace begin: capturing %d frames at Present #%d (StartFrame=%d) ===\n",
			           g_FrameTraceRemaining, s_presentsSeen, startAt);
	}
	if (g_FrameTraceRemaining > 0)
	{
		--g_FrameTraceRemaining;
		if (g_FrameTraceRemaining == 0)
			FrameTrace("=== frame trace complete (auto-disabled) ===\n");
	}
}

// IID → name lookup for diagnostic logs. Recognises the high-traffic D3D11
// and DXGI interfaces seen during proxy-stack QI fall-throughs; unknowns
// fall back to {xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx} so the GUID is still
// recoverable by hand-grep against d3d11.h / dxgi*.h.
void FormatGUID(REFIID riid, char* buf, size_t bufLen)
{
	struct Known { const IID* iid; const char* name; };
	static const Known table[] = {
		{ &__uuidof(IUnknown),              "IUnknown" },
		// DXGI core
		{ &__uuidof(IDXGIObject),           "IDXGIObject" },
		{ &__uuidof(IDXGIDeviceSubObject),  "IDXGIDeviceSubObject" },
		{ &__uuidof(IDXGIResource),         "IDXGIResource" },
		{ &__uuidof(IDXGIResource1),        "IDXGIResource1" },
		{ &__uuidof(IDXGISurface),          "IDXGISurface" },
		{ &__uuidof(IDXGISurface1),         "IDXGISurface1" },
		{ &__uuidof(IDXGISurface2),         "IDXGISurface2" },
		{ &__uuidof(IDXGIKeyedMutex),       "IDXGIKeyedMutex" },
		// DXGI device
		{ &__uuidof(IDXGIDevice),           "IDXGIDevice" },
		{ &__uuidof(IDXGIDevice1),          "IDXGIDevice1" },
		{ &__uuidof(IDXGIDevice2),          "IDXGIDevice2" },
		{ &__uuidof(IDXGIDevice3),          "IDXGIDevice3" },
		// DXGI swap chain
		{ &__uuidof(IDXGISwapChain),        "IDXGISwapChain" },
		{ &__uuidof(IDXGISwapChain1),       "IDXGISwapChain1" },
		{ &__uuidof(IDXGISwapChain2),       "IDXGISwapChain2" },
		{ &__uuidof(IDXGISwapChain3),       "IDXGISwapChain3" },
		{ &__uuidof(IDXGISwapChain4),       "IDXGISwapChain4" },
		// D3D11 device / context
		{ &__uuidof(ID3D11Device),          "ID3D11Device" },
		{ &__uuidof(ID3D11Device1),         "ID3D11Device1" },
		{ &__uuidof(ID3D11Device2),         "ID3D11Device2" },
		{ &__uuidof(ID3D11Device3),         "ID3D11Device3" },
		{ &__uuidof(ID3D11DeviceContext),   "ID3D11DeviceContext" },
		{ &__uuidof(ID3D11DeviceContext1),  "ID3D11DeviceContext1" },
		{ &__uuidof(ID3D11DeviceContext2),  "ID3D11DeviceContext2" },
		{ &__uuidof(ID3D11DeviceContext3),  "ID3D11DeviceContext3" },
		// D3D11 resources / views
		{ &__uuidof(ID3D11Resource),        "ID3D11Resource" },
		{ &__uuidof(ID3D11Texture2D),       "ID3D11Texture2D" },
		{ &__uuidof(ID3D11Buffer),          "ID3D11Buffer" },
		{ &__uuidof(ID3D11RenderTargetView),"ID3D11RenderTargetView" },
		{ &__uuidof(ID3D11ShaderResourceView),"ID3D11ShaderResourceView" },
		{ &__uuidof(ID3D11DepthStencilView),"ID3D11DepthStencilView" },
		{ &__uuidof(ID3D11UnorderedAccessView),"ID3D11UnorderedAccessView" },
	};
	for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); ++i)
	{
		if (IsEqualGUID(riid, *table[i].iid))
		{
			_snprintf_s(buf, bufLen, _TRUNCATE, "%s", table[i].name);
			return;
		}
	}
	_snprintf_s(buf, bufLen, _TRUNCATE,
		"{%08lX-%04hX-%04hX-%02X%02X-%02X%02X%02X%02X%02X%02X}",
		(unsigned long)riid.Data1, (unsigned short)riid.Data2, (unsigned short)riid.Data3,
		riid.Data4[0], riid.Data4[1], riid.Data4[2], riid.Data4[3],
		riid.Data4[4], riid.Data4[5], riid.Data4[6], riid.Data4[7]);
}

static union{
	D3D10DDI_ADAPTERFUNCS	OriginalAdapterFuncs;
	D3D10_2DDI_ADAPTERFUNCS	OriginalAdapterFuncs2;
};

#define GET_ORIG()		OriginalAdapterFuncs
#define GET_ORIG2()		OriginalAdapterFuncs2

/*ID3D10Device *GetD3DDevicePtr(D3D10DDI_HRTDEVICE hRTDevice)
{
	UINT_PTR Offset;
	// some magic will happen here
#ifndef WIN64
	Offset = 0x24;
#else
	Offset = 0x40;
#endif
	void* pD3DDev = (void*)((UINT_PTR)hRTDevice.handle - Offset);
	__try
	{
		if (!IsBadReadPtr(pD3DDev, sizeof(void*)))
		{
			void** p = *(void***)pD3DDev;	
			if (!IsBadReadPtr(p, sizeof(void*)))
			{
				UINT_PTR firstMethod = (UINT_PTR)p[0];
				// only if release, should be in d3d10core.dll
				if (CheckCodeInDll(_T("d3d10core.dll"), firstMethod) || 
					CheckCodeInDll(_T("d3d10_1core.dll"), firstMethod))
					return (ID3D10Device*) pD3DDev;
			}

// Dump array of function pointers (for diagnostics)
static void LogFuncPointerArray(void* pBase, size_t count, const char* label)
{
	if (!pBase)
	{
		DEBUG_MESSAGE(_T("%S: NULL\n"), label);
		return;
	}
	DEBUG_MESSAGE(_T("%S: base=%p count=%u\n"), label, pBase, (unsigned)count);
	void** slots = (void**)pBase;
	for (size_t i = 0; i < count; i++)
	{
		void* v = NULL;
		__try { v = slots[i]; } __except (EXCEPTION_EXECUTE_HANDLER) { v = NULL; }
		if (!v)
		{
			DEBUG_MESSAGE(_T("  [%03u] NULL\n"), (unsigned)i);
			continue;
		}
		HMODULE hMod = NULL;
		BOOL hasMod = GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
										 GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
										 (LPCSTR)v, &hMod);
		if (hasMod)
		{
			WCHAR modName[MAX_PATH];
			GetModuleFileNameW(hMod, modName, MAX_PATH);
			WCHAR* pSlash = wcsrchr(modName, L'\\');
			DEBUG_MESSAGE(_T("  [%03u] %p -> %ls\n"), (unsigned)i, v, pSlash ? pSlash + 1 : modName);
		}
		else
		{
			DEBUG_MESSAGE(_T("  [%03u] %p -> (no module)\n"), (unsigned)i, v);
		}
	}
}

// Detect whether the device function table provided by the runtime contains
// additional (unhandled) function pointers beyond the compiled
// D3D11DDI_DEVICEFUNCS layout. If so, wrapping the device would leave
// some driver functions unhooked while the wrapper still offsets the
// device handle — this causes the driver to dereference invalid memory.
bool HasExtraDeviceFuncs(void* pDeviceFuncsBase, size_t compiledSize)
{
	if (!pDeviceFuncsBase) return false;
	BYTE* base = (BYTE*)pDeviceFuncsBase;
	const size_t slotsToScan = 64;
	BYTE* scan = base + compiledSize;
	for (size_t i = 0; i < slotsToScan; i++)
	{
		void* val = NULL;
		__try { val = *(void**)(scan + i * sizeof(void*)); }
		__except (EXCEPTION_EXECUTE_HANDLER) { val = NULL; }

		if (!val) continue;

		HMODULE hMod = NULL;
		if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
							  GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
							  (LPCSTR)val, &hMod))
		{
			DEBUG_MESSAGE(_T("Detected extra device-func pointer @ %p -> module present\n"), val);
			return true;
		}
	}
	return false;
}


		}

		//try debug
#ifndef WIN64
		Offset = 0x11D0;
#else
		Offset = 0x1B60;
#endif
		pD3DDev = (void*)((UINT_PTR)hRTDevice.handle - Offset);
		if (!IsBadReadPtr(pD3DDev, sizeof(void*)))
		{
			void** p = *(void***)pD3DDev;	
			if (!IsBadReadPtr(p, sizeof(void*)))
			{
				UINT_PTR firstMethod = (UINT_PTR)p[0];
				if (CheckCodeInDll(_T("d3d10sdklayers.dll"), firstMethod))
					return (ID3D10Device*) pD3DDev;
			}
		}

		//try debug
#ifndef WIN64
		Offset = 0x1190;
#else
		Offset = 0x1B20;
#endif
		pD3DDev = (void*)((UINT_PTR)hRTDevice.handle - Offset);
		if (!IsBadReadPtr(pD3DDev, sizeof(void*)))
		{
			void** p = *(void***)pD3DDev;	
			if (!IsBadReadPtr(p, sizeof(void*)))
			{
				UINT_PTR firstMethod = (UINT_PTR)p[0];
				if (CheckCodeInDll(_T("d3d10sdklayers.dll"), firstMethod))
					return (ID3D10Device*) pD3DDev;
			}
		}
	}
	__except(EXCEPTION_EXECUTE_HANDLER)
	{
	}
	DEBUG_MESSAGE(_T("Can't get D3D10 Device address!\n"));
	return NULL;
}*/

#define D3D10_0_w_DDI_MINOR_VERSION 3	// Vista SP2 with platform update
#define D3D10_1_w_DDI_MINOR_VERSION 4	// Vista SP2 with platform update
#define D3D11_0_w_DDI_MINOR_VERSION 9

TD3DVersion GetD3DVersionFromInterface( UINT Interface )
{
	int DDIMajorVersion = Interface >> 16;
	int DDIMinorVersion = Interface & 0xFFFF;
	if ( DDIMajorVersion == D3D10_DDI_MAJOR_VERSION )
	{
		switch(DDIMinorVersion)
		{
		case D3D10_0_DDI_MINOR_VERSION:
		case D3D10_0_w_DDI_MINOR_VERSION:
		case D3D10_0_x_DDI_MINOR_VERSION:
		case D3D10_0_7_DDI_MINOR_VERSION:
			return TD3DVersion_10_0;

		case D3D10_1_DDI_MINOR_VERSION:
		case D3D10_1_w_DDI_MINOR_VERSION:
		case D3D10_1_x_DDI_MINOR_VERSION:
		case D3D10_1_7_DDI_MINOR_VERSION:
			return TD3DVersion_10_1;

		case D3D10on9_DDI_MINOR_VERSION:
			return TD3DVersion_Unknown;
		}
	}
	else if ( DDIMajorVersion == D3D11_DDI_MAJOR_VERSION )
	{
		// D3D11.1+ DDI (Windows 8+) uses minor versions beyond those defined in
		// the original iZ3D SDK headers. Treat all D3D11.x as 11_0 — the base
		// device function table layout is identical and the extra 11.1+ slots
		// simply pass through unhooked.
		return TD3DVersion_11_0;
	}
	return TD3DVersion_Unknown;
}

bool IsAcceptableD3DInterfaceVersion( UINT Interface )
{
	TD3DVersion ver = GetD3DVersionFromInterface( Interface );
	if (ver == TD3DVersion_10_0)
		return true;
	else if (ver == TD3DVersion_10_1)
		return true;
	else if (ver == TD3DVersion_11_0)
		return true;
	return false;
}

PFND3D10DDI_SETERROR_CB SetErrorCB_Orig = NULL;
void (APIENTRY  SetErrorCB)( D3D10DDI_HRTCORELAYER hRTCoreLayer, HRESULT hr )
{
	if (hr != DXGI_DDI_ERR_WASSTILLDRAWING)
	{
		HRESULT hrRemoved = D3DERR_DEVICEREMOVED;
#ifndef FINAL_RELEASE
		if (hr != hrRemoved) {
			DEBUG_MESSAGE(_T("Video Driver error occur, hr = %s(0x%X)\n"), DXGetErrorString(hr), hr);
		} else {
			DEBUG_MESSAGE(_T("Video Driver error occur, hr = D3DERR_DEVICEREMOVED\n"));
		}
		DEBUG_MESSAGE(_T("Previous command: %S\n"), CommandIDToString(Commands::Command::LastCommandId_));
#endif
		_ASSERT(hr != hrRemoved);
	}
	SetErrorCB_Orig(hRTCoreLayer, hr);
}

SIZE_T (APIENTRY  CalcPrivateDeviceSize)( D3D10DDI_HADAPTER  hAdapter, CONST D3D10DDIARG_CALCPRIVATEDEVICESIZE*  pData )
{
	// A2: wrapper lives on heap and is found via side-table lookup, not at
	// offset 0 of the runtime's private buffer. So we add zero bytes here
	// and the runtime allocates exactly what the driver requested.
	SIZE_T origSize = GET_ORIG().pfnCalcPrivateDeviceSize(hAdapter, pData);
	int DDIMajorVersion = pData ? (pData->Interface >> 16) : 0;
	int DDIMinorVersion = pData ? (pData->Interface & 0xFFFF) : 0;
	DDILog("CalcPrivateDeviceSize: DDI=%d.%d, origSize=%zu (no handle-shift)\n",
		DDIMajorVersion, DDIMinorVersion, origSize);
	return origSize;
}

HRESULT (APIENTRY  CreateDevice)( D3D10DDI_HADAPTER  hAdapter, D3D10DDIARG_CREATEDEVICE*  pCreateData)
{
	if ( IsInternalCall() )
	{
		DDILog("CreateDevice: IsInternalCall — passthrough\n");
		HRESULT hResult = S_OK;
		NSCALL( GET_ORIG().pfnCreateDevice( hAdapter, pCreateData ) );
		return hResult;
	}

	int DDIMajorVersion = pCreateData->Interface >> 16;
	int DDIMinorVersion = pCreateData->Interface & 0xFFFF;
	DEBUG_MESSAGE(_T("DDI Interface %d.%d\n"), DDIMajorVersion, DDIMinorVersion);
	int DriverMajorVersion = pCreateData->Version >> 16;
	int DriverMinorVersion = pCreateData->Version & 0xFFFF;
	DEBUG_MESSAGE(_T("Driver Version %d.%d\n"), DriverMajorVersion, DriverMinorVersion);
	DDILog("CreateDevice: DDI=%d.%d, Driver=%d.%d, hDrvDevice=%p, pDeviceFuncs=%p, p10_1DeviceFuncs=%p, p11DeviceFuncs=%p\n",
		DDIMajorVersion, DDIMinorVersion, DriverMajorVersion, DriverMinorVersion,
		pCreateData->hDrvDevice.pDrvPrivate, pCreateData->pDeviceFuncs,
		pCreateData->p10_1DeviceFuncs, pCreateData->p11DeviceFuncs);

	HRESULT hResult;
	if (!IsAcceptableD3DInterfaceVersion(pCreateData->Interface))
	{
		DEBUG_MESSAGE(_T("Warning: Unsupported version\n"));
		DDILog("CreateDevice: Unsupported DDI version %d.%d — passthrough (no wrapping)\n",
			DDIMajorVersion, DDIMinorVersion);
		NSCALL(GET_ORIG().pfnCreateDevice(hAdapter, pCreateData));
		return hResult;
	}

	D3D10DDI_HDEVICE  hOrigDevice = pCreateData->hDrvDevice;

	// A2: No more handle-shift. Wrapper lives on the heap and is looked up
	// via the device-wrapper registry. The runtime's hDevice is passed
	// through to the driver unmodified, so unwrapped DDI slots (functions
	// that exist in modern Win11 D3D11.10+ struct layouts but not in our
	// compiled D3D11.0) work correctly because the driver finds its data
	// where it wrote it. The HasExtraDeviceFuncs diagnostic is kept for
	// visibility but no longer triggers a bail-out.
	if (pCreateData->p11DeviceFuncs &&
		HasExtraDeviceFuncs(pCreateData->p11DeviceFuncs, sizeof(D3D11DDI_DEVICEFUNCS)))
	{
		DDILog("CreateDevice: p11DeviceFuncs has extra fields beyond compiled layout (safe under A2)\n");
	}
	if (pCreateData->p10_1DeviceFuncs &&
		HasExtraDeviceFuncs(pCreateData->p10_1DeviceFuncs, sizeof(D3D10_1DDI_DEVICEFUNCS)))
	{
		DDILog("CreateDevice: p10_1DeviceFuncs has extra fields beyond compiled layout (safe under A2)\n");
	}
	if (pCreateData->pDeviceFuncs &&
		HasExtraDeviceFuncs(pCreateData->pDeviceFuncs, sizeof(D3D10DDI_DEVICEFUNCS)))
	{
		DDILog("CreateDevice: pDeviceFuncs has extra fields beyond compiled layout (safe under A2)\n");
	}

	NSCALL(GET_ORIG().pfnCreateDevice(hAdapter, pCreateData));
	if (SUCCEEDED(hResult))
	{
		if(gInfo.UseMonoDeviceWrapper)
		{
			D3DMonoDeviceWrapper* pWrapper = new D3DMonoDeviceWrapper();
			iZ3D::RegisterDeviceWrapper(hOrigDevice.pDrvPrivate, pWrapper);
			g_pLastD3DDevice = (D3DDeviceWrapper*)pWrapper;

			pWrapper->hDevice = pCreateData->hDrvDevice;
			pWrapper->SetInterfaceVersion( pCreateData->Interface );
			TD3DVersion ver = pWrapper->GetD3DVersion();		
			if ( ver == TD3DVersion_10_0 )
			{
				DEBUG_MESSAGE(_T("D3D 10.0\n"));
				D3D10DDI_DEVICEFUNCS *pDeviceFuncs = pCreateData->pDeviceFuncs;
				pWrapper->HookDeviceFuncs(pDeviceFuncs);
			}
			else if ( ver == TD3DVersion_10_1 )
			{
				DEBUG_MESSAGE(_T("D3D 10.1\n"));
				D3D10_1DDI_DEVICEFUNCS *pDeviceFuncs = pCreateData->p10_1DeviceFuncs;
				pWrapper->HookDeviceFuncs(pDeviceFuncs);
			}
			else if ( ver == TD3DVersion_11_0 )
			{
				DEBUG_MESSAGE(_T("D3D 11.0\n"));
				D3D11DDI_DEVICEFUNCS *pDeviceFuncs = pCreateData->p11DeviceFuncs;
				pWrapper->HookDeviceFuncs(pDeviceFuncs);
			}
			else 
			{
				// Try rebuild project
				_ASSERT(FALSE);
			}
			if (!IS_DXGI1_1_BASE_FUNCTIONS(pCreateData->Interface, pCreateData->Version))
			{
				DEBUG_MESSAGE(_T("DXGI 1.0\n"));
				DXGI_DDI_BASE_FUNCTIONS  *pDXGIDDIBaseFunctions = pCreateData->DXGIBaseDDI.pDXGIDDIBaseFunctions;
				pWrapper->HookDXGIFuncs(pDXGIDDIBaseFunctions);			
			}
			else
			{
				DEBUG_MESSAGE(_T("DXGI 1.1\n"));
				DXGI1_1_DDI_BASE_FUNCTIONS  *pDXGIDDIBaseFunctions = pCreateData->DXGIBaseDDI.pDXGIDDIBaseFunctions2;
				pWrapper->HookDXGIFuncs(pDXGIDDIBaseFunctions);	
			}
		}
		else
		{
			D3DDeviceWrapper* pWrapper = new D3DDeviceWrapper();
			iZ3D::RegisterDeviceWrapper(hOrigDevice.pDrvPrivate, pWrapper);
			g_pLastD3DDevice = pWrapper;

			pWrapper->hDevice = pCreateData->hDrvDevice;
			pWrapper->dwCreationFlags = pCreateData->Flags;
			pWrapper->SetInterfaceVersion( pCreateData->Interface );
			TD3DVersion ver = pWrapper->GetD3DVersion();
			DDILog("CreateDevice (stereo branch): D3DVersion=%d (10_0=1,10_1=2,11_0=3), Flags=0x%X\n",
				(int)ver, pCreateData->Flags);
			if ( ver == TD3DVersion_10_0 )
			{
				DEBUG_MESSAGE(_T("D3D 10.0\n"));
				DDILog("CreateDevice: D3D 10.0 — calling HookDeviceFuncs\n");
				D3D10DDI_DEVICEFUNCS *pDeviceFuncs = pCreateData->pDeviceFuncs;
				pWrapper->HookDeviceFuncs(pDeviceFuncs);
			}
			else if ( ver == TD3DVersion_10_1 )
			{
				DEBUG_MESSAGE(_T("D3D 10.1\n"));
				DDILog("CreateDevice: D3D 10.1 — calling HookDeviceFuncs\n");
				D3D10_1DDI_DEVICEFUNCS *pDeviceFuncs = pCreateData->p10_1DeviceFuncs;
				pWrapper->HookDeviceFuncs(pDeviceFuncs);
			}
			else if ( ver == TD3DVersion_11_0 )
			{
				DEBUG_MESSAGE(_T("D3D 11.0\n"));
				DDILog("CreateDevice: D3D 11.0 — calling HookDeviceFuncs\n");
				D3D11DDI_DEVICEFUNCS *pDeviceFuncs = pCreateData->p11DeviceFuncs;
				pWrapper->HookDeviceFuncs(pDeviceFuncs);
			}
			else
			{
				DDILog("CreateDevice: ERROR unknown D3DVersion=%d, Interface=0x%X — wrapper will not hook funcs\n",
					(int)ver, pCreateData->Interface);
				// Try rebuild project
				_ASSERT(FALSE);
			}
			if (!IS_DXGI1_1_BASE_FUNCTIONS(pCreateData->Interface, pCreateData->Version))
			{
				DEBUG_MESSAGE(_T("DXGI 1.0\n"));
				DDILog("CreateDevice: DXGI 1.0 — calling HookDXGIFuncs\n");
				DXGI_DDI_BASE_FUNCTIONS  *pDXGIDDIBaseFunctions = pCreateData->DXGIBaseDDI.pDXGIDDIBaseFunctions;
				pWrapper->HookDXGIFuncs(pDXGIDDIBaseFunctions);
			}
			else
			{
				DEBUG_MESSAGE(_T("DXGI 1.1\n"));
				DDILog("CreateDevice: DXGI 1.1 — calling HookDXGIFuncs\n");
				DXGI1_1_DDI_BASE_FUNCTIONS  *pDXGIDDIBaseFunctions = pCreateData->DXGIBaseDDI.pDXGIDDIBaseFunctions2;
				pWrapper->HookDXGIFuncs(pDXGIDDIBaseFunctions);
			}
#ifndef FINAL_RELEASE
			pWrapper->m_ResourceManager.OpenFile(hOrigDevice);
#endif
			DDILog("CreateDevice: calling pWrapper->CreateOutput()\n");
			pWrapper->CreateOutput();
			DDILog("CreateDevice: CreateOutput returned, m_pOutputMethod=%p\n",
				(void*)pWrapper->m_pOutputMethod);
		}
#ifndef FINAL_RELEASE
		D3D10DDI_CORELAYER_DEVICECALLBACKS  *pD3D10UMFunctions = (D3D10DDI_CORELAYER_DEVICECALLBACKS*)pCreateData->pUMCallbacks;
		SetErrorCB_Orig = pD3D10UMFunctions->pfnSetErrorCb;
		pD3D10UMFunctions->pfnSetErrorCb = SetErrorCB;
#endif
	}
	
	pCreateData->hDrvDevice = hOrigDevice;
	return hResult;
}

HRESULT (APIENTRY  CloseAdapter)( D3D10DDI_HADAPTER  hAdapter )
{
	HRESULT hResult = GET_ORIG().pfnCloseAdapter(hAdapter);
	return hResult;
}

TCHAR* EnumToString(TD3DVersion ver)
{
	switch ( ver )
	{
	case TD3DVersion_10_0:
		return _T("D3D 10.0");
	case TD3DVersion_10_1:
		return _T("D3D 10.1");
	case TD3DVersion_11_0:
		return _T("D3D 11.0");
	default:
		return _T("Unknown");
	}
}

HRESULT (APIENTRY  GetSupportedVersions)( D3D10DDI_HADAPTER  hAdapter, UINT32* puEntries, UINT64* pSupportedDDIInterfaceVersions)
{
	HRESULT hResult = GET_ORIG2().pfnGetSupportedVersions(hAdapter, puEntries, pSupportedDDIInterfaceVersions);
	DDILog("GetSupportedVersions: hr=0x%08lX, count=%u (before filtering)\n",
		hResult, puEntries ? *puEntries : 0);
	if (puEntries && pSupportedDDIInterfaceVersions)
	{
		for (UINT32 i = 0; i < *puEntries && i < 32; i++)
		{
			UINT iface = (UINT)(pSupportedDDIInterfaceVersions[i] >> 32);
			DDILog("  [%u] DDI=%d.%d (0x%08X)\n", i, iface >> 16, iface & 0xFFFF, iface);
		}
	}
	if (SUCCEEDED(hResult) && pSupportedDDIInterfaceVersions)
	{
		// Filter out D3D11.1+ DDI versions that use a larger D3D11_1DDI_DEVICEFUNCS
		// struct than our compiled D3D11DDI_DEVICEFUNCS (Win7 D3D11.0 layout).
		// Without this, d3d11.dll negotiates D3D11.1+ DDI, the extra function slots
		// beyond our struct are left pointing at the original driver, but the device
		// handle has been offset by the wrapper — causing the driver to dereference
		// garbage and crash.
		UINT32 dst = 0;
		for (UINT32 i = 0; i < *puEntries; i++)
		{
			UINT Interface = (UINT)(pSupportedDDIInterfaceVersions[i] >> 32);
			int DDIMajorVersion = Interface >> 16;
			int DDIMinorVersion = Interface & 0xFFFF;

			if (DDIMajorVersion == D3D11_DDI_MAJOR_VERSION && DDIMinorVersion > D3D11_0_7_DDI_MINOR_VERSION)
			{
				DEBUG_MESSAGE(_T("\tFiltered out D3D11.1+ DDI version %d.%d (unsupported struct layout)\n"),
					DDIMajorVersion, DDIMinorVersion);
				continue;
			}

			pSupportedDDIInterfaceVersions[dst++] = pSupportedDDIInterfaceVersions[i];
		}
		*puEntries = dst;
		DDILog("GetSupportedVersions: count=%u (after filtering)\n", *puEntries);
		for (UINT32 i = 0; i < *puEntries && i < 32; i++)
		{
			UINT iface = (UINT)(pSupportedDDIInterfaceVersions[i] >> 32);
			DDILog("  [%u] DDI=%d.%d (0x%08X)\n", i, iface >> 16, iface & 0xFFFF, iface);
		}

		DEBUG_MESSAGE(_T("\tDriver support version (after filtering):\n"));
		for(UINT32 i = 0; i < *puEntries; i++)
		{
			int MajorVersion = (pSupportedDDIInterfaceVersions[i] >> 48) & 0xFFFF;
			int MinorVersion = (pSupportedDDIInterfaceVersions[i] >> 32) & 0xFFFF;
			int BuildVersion = (pSupportedDDIInterfaceVersions[i] >> 16) & 0xFFFF;
			TD3DVersion ver = GetD3DVersionFromInterface( pSupportedDDIInterfaceVersions[i] >> 32 );
			DEBUG_MESSAGE(_T("\t\tD3D Interface %d.%d.%d (%s)\n"), MajorVersion, MinorVersion, BuildVersion, EnumToString(ver));
		}
	}
	return hResult;
}

HRESULT (APIENTRY  GetCaps)( D3D10DDI_HADAPTER  hAdapter, CONST D3D10_2DDIARG_GETCAPS* pCaps)
{
	HRESULT hResult = GET_ORIG2().pfnGetCaps(hAdapter, pCaps);
	switch (pCaps->Type)
	{
	case D3D11DDICAPS_THREADING:
		{
			_ASSERT(pCaps->DataSize == sizeof(D3D11DDI_THREADING_CAPS));
			D3D11DDI_THREADING_CAPS*  pData = (D3D11DDI_THREADING_CAPS*)pCaps->pData;
			if (pData->Caps & D3D11DDICAPS_FREETHREADED)
			{
				DEBUG_MESSAGE(_T("\tDriver support free threading\n"));
			}
			if (pData->Caps & D3D11DDICAPS_COMMANDLISTS )
			{
				DEBUG_MESSAGE(_T("\tDriver support command list\n"));
				pData->Caps &= ~D3D11DDICAPS_COMMANDLISTS;
			}
			else
			{
				DEBUG_MESSAGE(_T("\tDriver NOT support command list\n"));
			}
		}
		break;
	case D3D11DDICAPS_SHADER:
		{
			_ASSERT(pCaps->DataSize == sizeof(D3D11DDI_SHADER_CAPS));
			D3D11DDI_SHADER_CAPS*  pData = (D3D11DDI_SHADER_CAPS*)pCaps->pData;
		}
		break;
	case D3D11DDICAPS_3DPIPELINESUPPORT:
		{
			_ASSERT(pCaps->DataSize == sizeof(D3D11DDI_3DPIPELINESUPPORT_CAPS));
			D3D11DDI_3DPIPELINESUPPORT_CAPS*  pData = (D3D11DDI_3DPIPELINESUPPORT_CAPS*)pCaps->pData;
			if (pData->Caps & D3D11DDI_ENCODE_3DPIPELINESUPPORT_CAP(D3D11DDI_3DPIPELINELEVEL_10_0))
			{
				DEBUG_MESSAGE(_T("\tD3D11DDI_3DPIPELINELEVEL_10_0\n"));
			}
			if (pData->Caps & D3D11DDI_ENCODE_3DPIPELINESUPPORT_CAP(D3D11DDI_3DPIPELINELEVEL_10_1))
			{
				DEBUG_MESSAGE(_T("\tD3D11DDI_3DPIPELINELEVEL_10_1\n"));
			}
			if (pData->Caps & D3D11DDI_ENCODE_3DPIPELINESUPPORT_CAP(D3D11DDI_3DPIPELINELEVEL_11_0))
			{
				DEBUG_MESSAGE(_T("\tD3D11DDI_3DPIPELINELEVEL_11_0\n"));
			}
		}
		break;
	}
	return hResult;
}

#define D3D10_VISTA_MODULE_NAME	_T("DriverD3D10VistaModule")
#define D3D10_WIN7_MODULE_NAME	_T("DriverD3D10Win7Module")

void GetDriverModuleName(LPTSTR ModuleName, LPCTSTR Key)
{
	ModuleName[0] = '\0';
#ifndef WIN64
	LPCTSTR key = _T("SOFTWARE\\iZ3D\\iZ3D Driver\\Win32");
#else
	LPCTSTR key = _T("SOFTWARE\\iZ3D\\iZ3D Driver\\Win64");
#endif
	HKEY hDriver;
	if (RegOpenKeyEx(HKEY_CURRENT_USER, key, 0, KEY_READ, &hDriver) == ERROR_SUCCESS)
	{
		DWORD Size = MAX_PATH * sizeof(TCHAR);
		if (RegQueryValueEx(hDriver, Key, NULL, NULL, (BYTE*)ModuleName, &Size))
			Size = 0;
		ModuleName[Size / sizeof(TCHAR)] = 0;
		RegCloseKey(hDriver);
	}
}

S3DWRAPPER10_API HRESULT WINAPI OpenAdapter10(D3D10DDIARG_OPENADAPTER* pOpenData )
{
	HRESULT hResult = S_OK;
	TCHAR moduleName[MAX_PATH];
	GetDriverModuleName(moduleName, D3D10_VISTA_MODULE_NAME);
	TCHAR szBuffer[MAX_PATH];
	UINT nSize = GetSystemDirectory(szBuffer, MAX_PATH - 1);
	TCHAR szFileName[MAX_PATH];
	_stprintf_s(szFileName, _T("%s\\%s"), szBuffer, moduleName);
	//MAGIC OPTIMIZATION
	HMODULE hD3D10Lib = GetModuleHandle(moduleName);
	if (!hD3D10Lib)
		hD3D10Lib = LoadLibraryEx(szFileName, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
	if (!hD3D10Lib)
		return E_FAIL;
	PFND3D10DDI_OPENADAPTER pOpenAdapter10 = NULL;
	pOpenAdapter10 = (PFND3D10DDI_OPENADAPTER)GetProcAddress(hD3D10Lib, "OpenAdapter10");
	if (!pOpenAdapter10)
	{
		DEBUG_TRACE1(_T("GetProcAddress(OpenAdapter10) FAILED\n"));
		return E_FAIL;
	}
	hResult = pOpenAdapter10(pOpenData);
	if (SUCCEEDED(hResult))
	{
		memcpy(&OriginalAdapterFuncs, pOpenData->pAdapterFuncs, sizeof D3D10DDI_ADAPTERFUNCS);
		if (gInfo.UseCOMWrap)
		{
			// Option B (Stage 2): leave the adapter-funcs table unmodified.
			// Game's call chain runs through the d3d11.dll-proxy COM wrappers
			// instead, avoiding the Win11 D3D11.10 DDI mismatch that broke
			// the legacy hook path. See GlobalData.h::UseCOMWrap.
			DDILog("OpenAdapter10: UseCOMWrap=1 -- SKIPPING DDI hook install\n");
		}
		else
		{
#define SET_FUNC(x) pOpenData->pAdapterFuncs->pfn##x = ##x;
			SET_FUNC(CalcPrivateDeviceSize);
			SET_FUNC(CreateDevice);
			SET_FUNC(CloseAdapter);
#undef SET_FUNC
		}
	}
	DEBUG_TRACE1(_T("OpenAdapter10(pOpenData) result=0x%X\n"), hResult );
	return hResult;
}

S3DWRAPPER10_API HRESULT WINAPI OpenAdapter10_2(D3D10DDIARG_OPENADAPTER* pOpenData )
{
	HRESULT hResult = S_OK;
	TCHAR moduleName[MAX_PATH];
	GetDriverModuleName(moduleName, D3D10_WIN7_MODULE_NAME);
	TCHAR szBuffer[MAX_PATH];
	UINT nSize = GetSystemDirectory(szBuffer, MAX_PATH - 1);
	TCHAR szFileName[MAX_PATH];
	_stprintf_s(szFileName, _T("%s\\%s"), szBuffer, moduleName);
	//MAGIC OPTIMIZATION
	HMODULE hD3D10Lib = GetModuleHandle(moduleName);
	if (!hD3D10Lib)
		hD3D10Lib = LoadLibraryEx(szFileName, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
	if (!hD3D10Lib)
		return E_FAIL;
	PFND3D10DDI_OPENADAPTER pOpenAdapter10 = NULL;
	pOpenAdapter10 = (PFND3D10DDI_OPENADAPTER)GetProcAddress(hD3D10Lib, "OpenAdapter10_2");
	if (!pOpenAdapter10)
	{
		DEBUG_TRACE1(_T("GetProcAddress(OpenAdapter10_2) FAILED\n"));
		return E_FAIL;
	}
	hResult = pOpenAdapter10(pOpenData);
	if (SUCCEEDED(hResult))
	{
		memcpy(&OriginalAdapterFuncs2, pOpenData->pAdapterFuncs_2, sizeof D3D10_2DDI_ADAPTERFUNCS);
		if (gInfo.UseCOMWrap)
		{
			// Option B (Stage 2): see OpenAdapter10's matching guard.
			DDILog("OpenAdapter10_2: UseCOMWrap=1 -- SKIPPING DDI hook install\n");
		}
		else
		{
#define SET_FUNC(x) pOpenData->pAdapterFuncs_2->pfn##x = ##x;
			SET_FUNC(CalcPrivateDeviceSize);
			SET_FUNC(CreateDevice);
			SET_FUNC(CloseAdapter);
			SET_FUNC(GetSupportedVersions);
			SET_FUNC(GetCaps);
#undef SET_FUNC
		}
        // Diagnostics: dump first several p11DeviceFuncs pointers if present (not commented out rogionally, temp fix to get DX10 working)
		//if (pOpenData->p11DeviceFuncs)
		//{
			//DEBUG_MESSAGE(_T("OpenAdapter10_2: p11DeviceFuncs=%p\n"), pOpenData->p11DeviceFuncs);
			//LogFuncPointerArray(pOpenData->p11DeviceFuncs, 32, "p11DeviceFuncs (pre-hook)");
		//}
	}
	DEBUG_TRACE1(_T("OpenAdapter10_2(pOpenData) result=0x%X\n"), hResult );
	return hResult;
}
