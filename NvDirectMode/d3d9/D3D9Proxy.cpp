/* NvDirectMode - IDirect3D9 proxy implementation */

#include "D3D9Proxy.h"
#include "Device9Proxy.h"
#include "proxy_factory.h"
#include "swapchain_helpers.h"
#include "log.h"

#pragma comment(lib, "dxguid.lib")  // for IID_IDirect3D9 / IID_IDirect3DDevice9

namespace NvDirectMode
{

void* CreateD3D9Proxy(void* realD3D9)
{
    if (!realD3D9) return nullptr;
    auto* p = new D3D9Proxy(static_cast<IDirect3D9*>(realD3D9), /*isEx=*/false);
    return static_cast<IDirect3D9*>(p);
}

void* CreateD3D9ExProxy(void* realD3D9Ex)
{
    if (!realD3D9Ex) return nullptr;
    auto* p = new D3D9Proxy(static_cast<IDirect3D9Ex*>(realD3D9Ex), /*isEx=*/true);
    return static_cast<IDirect3D9Ex*>(p);
}


D3D9Proxy::D3D9Proxy(IDirect3D9* real, bool isEx)
    : m_real(real)
    , m_realEx(isEx ? static_cast<IDirect3D9Ex*>(real) : nullptr)
    , m_isEx(isEx)
    , m_refs(1)
{
}

D3D9Proxy::~D3D9Proxy() = default;

HRESULT STDMETHODCALLTYPE D3D9Proxy::QueryInterface(REFIID riid, void** ppvObj)
{
    if (!ppvObj) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IDirect3D9)
    {
        *ppvObj = static_cast<IDirect3D9*>(this);
        AddRef();
        D9_TRACE_R("QI(IDirect3D9)", S_OK, "%s", "self");
        return S_OK;
    }
    if (riid == IID_IDirect3D9Ex)
    {
        if (!m_isEx) { *ppvObj = nullptr; D9_TRACE_R("QI(IDirect3D9Ex)", E_NOINTERFACE, "%s", "not-Ex"); return E_NOINTERFACE; }
        *ppvObj = static_cast<IDirect3D9Ex*>(this);
        AddRef();
        D9_TRACE_R("QI(IDirect3D9Ex)", S_OK, "%s", "self");
        return S_OK;
    }
    HRESULT hr = m_real->QueryInterface(riid, ppvObj);
    NVDM_TRACE_FIRST_N(4, "  D3D9Proxy::QI(unknown IID) hr=0x%08lX -- bypass risk\n", hr);
    D9_TRACE_R("QI(unknown)", hr, "%s", "passthrough");
    return hr;
}

ULONG STDMETHODCALLTYPE D3D9Proxy::AddRef()  { return InterlockedIncrement(&m_refs); }
ULONG STDMETHODCALLTYPE D3D9Proxy::Release()
{
    LONG r = InterlockedDecrement(&m_refs);
    if (r == 0)
    {
        if (m_real) { m_real->Release(); m_real = nullptr; }
        delete this;
    }
    return (ULONG)r;
}

// ---------------------------------------------------------------------------
// IDirect3D9 — passthrough except CreateDevice (wraps the returned device)
// ---------------------------------------------------------------------------
HRESULT D3D9Proxy::RegisterSoftwareDevice(void* p)                                                  { HRESULT hr = m_real->RegisterSoftwareDevice(p); D9_TRACE_R("RegisterSoftwareDevice", hr, "%p", p); return hr; }
UINT    D3D9Proxy::GetAdapterCount()                                                                { UINT n = m_real->GetAdapterCount(); D9_TRACE_R("GetAdapterCount", n, "%s", ""); return n; }
HRESULT D3D9Proxy::GetAdapterIdentifier(UINT A, DWORD F, D3DADAPTER_IDENTIFIER9* p)                 { HRESULT hr = m_real->GetAdapterIdentifier(A, F, p); D9_TRACE_R("GetAdapterIdentifier", hr, "A=%u F=0x%lX", A, F); return hr; }
UINT    D3D9Proxy::GetAdapterModeCount(UINT A, D3DFORMAT F)                                         { UINT n = m_real->GetAdapterModeCount(A, F); D9_TRACE_R("GetAdapterModeCount", n, "A=%u F=%d", A, (int)F); return n; }
HRESULT D3D9Proxy::EnumAdapterModes(UINT A, D3DFORMAT F, UINT M, D3DDISPLAYMODE* p)                 { HRESULT hr = m_real->EnumAdapterModes(A, F, M, p); D9_TRACE_R("EnumAdapterModes", hr, "A=%u F=%d M=%u", A, (int)F, M); return hr; }
HRESULT D3D9Proxy::GetAdapterDisplayMode(UINT A, D3DDISPLAYMODE* p)                                 { HRESULT hr = m_real->GetAdapterDisplayMode(A, p); D9_TRACE_R("GetAdapterDisplayMode", hr, "A=%u [%ux%u fmt=%d rr=%u]", A, p?p->Width:0, p?p->Height:0, p?(int)p->Format:-1, p?p->RefreshRate:0); return hr; }
HRESULT D3D9Proxy::CheckDeviceType(UINT A, D3DDEVTYPE D, D3DFORMAT AF, D3DFORMAT BF, BOOL W)        { HRESULT hr = m_real->CheckDeviceType(A, D, AF, BF, W); D9_TRACE_R("CheckDeviceType", hr, "A=%u DT=%d AF=%d BF=%d W=%d", A, (int)D, (int)AF, (int)BF, (int)W); return hr; }
HRESULT D3D9Proxy::CheckDeviceFormat(UINT A, D3DDEVTYPE D, D3DFORMAT AF, DWORD U, D3DRESOURCETYPE R, D3DFORMAT CF) { HRESULT hr = m_real->CheckDeviceFormat(A, D, AF, U, R, CF); D9_TRACE_R("CheckDeviceFormat", hr, "A=%u DT=%d AF=%d U=0x%lX RT=%d CF=%d", A, (int)D, (int)AF, U, (int)R, (int)CF); return hr; }
HRESULT D3D9Proxy::CheckDeviceMultiSampleType(UINT A, D3DDEVTYPE D, D3DFORMAT SF, BOOL W, D3DMULTISAMPLE_TYPE MT, DWORD* pQ) { HRESULT hr = m_real->CheckDeviceMultiSampleType(A, D, SF, W, MT, pQ); D9_TRACE_R("CheckDeviceMultiSampleType", hr, "A=%u DT=%d SF=%d W=%d MT=%d", A, (int)D, (int)SF, (int)W, (int)MT); return hr; }
HRESULT D3D9Proxy::CheckDepthStencilMatch(UINT A, D3DDEVTYPE D, D3DFORMAT AF, D3DFORMAT RTF, D3DFORMAT DSF) { HRESULT hr = m_real->CheckDepthStencilMatch(A, D, AF, RTF, DSF); D9_TRACE_R("CheckDepthStencilMatch", hr, "A=%u DT=%d AF=%d RTF=%d DSF=%d", A, (int)D, (int)AF, (int)RTF, (int)DSF); return hr; }
HRESULT D3D9Proxy::CheckDeviceFormatConversion(UINT A, D3DDEVTYPE D, D3DFORMAT SF, D3DFORMAT TF)    { HRESULT hr = m_real->CheckDeviceFormatConversion(A, D, SF, TF); D9_TRACE_R("CheckDeviceFormatConversion", hr, "A=%u DT=%d SF=%d TF=%d", A, (int)D, (int)SF, (int)TF); return hr; }
HRESULT D3D9Proxy::GetDeviceCaps(UINT A, D3DDEVTYPE D, D3DCAPS9* p)                                 { HRESULT hr = m_real->GetDeviceCaps(A, D, p); D9_TRACE_R("GetDeviceCaps", hr, "A=%u DT=%d [PS=%lx VS=%lx MaxTex=%u]", A, (int)D, p?(unsigned long)p->PixelShaderVersion:0, p?(unsigned long)p->VertexShaderVersion:0, p?p->MaxTextureWidth:0); return hr; }
HMONITOR D3D9Proxy::GetAdapterMonitor(UINT A)                                                       { HMONITOR h = m_real->GetAdapterMonitor(A); D9_TRACE_R("GetAdapterMonitor", (LONG_PTR)h, "A=%u", A); return h; }

HRESULT D3D9Proxy::CreateDevice(
    UINT Adapter,
    D3DDEVTYPE DeviceType,
    HWND hFocusWindow,
    DWORD BehaviorFlags,
    D3DPRESENT_PARAMETERS* pPresentationParameters,
    IDirect3DDevice9** ppReturnedDeviceInterface)
{
    if (!ppReturnedDeviceInterface) return D3DERR_INVALIDCALL;

    D3DPRESENT_PARAMETERS modified;
    D3DPRESENT_PARAMETERS* pParams = pPresentationParameters;
    UINT logicalW = 0, logicalH = 0;
    if (pParams)
    {
        modified = *pParams;
        ResolveAndDoubleSwapchainParams(&modified, hFocusWindow, &logicalW, &logicalH);
        pParams = &modified;
    }

    IDirect3DDevice9* realDevice = nullptr;
    HRESULT hr = m_real->CreateDevice(Adapter, DeviceType, hFocusWindow,
                                      BehaviorFlags, pParams,
                                      &realDevice);
    D9_TRACE_R("CreateDevice", hr, "A=%u DT=%d hWnd=%p BF=0x%lX BB=%ux%u fmt=%d SC=%u",
               Adapter, (int)DeviceType, (void*)hFocusWindow, BehaviorFlags,
               pParams?pParams->BackBufferWidth:0, pParams?pParams->BackBufferHeight:0,
               pParams?(int)pParams->BackBufferFormat:-1, pParams?pParams->BackBufferCount:0);
    if (FAILED(hr) || !realDevice)
    {
        *ppReturnedDeviceInterface = nullptr;
        return hr;
    }
    auto* proxy = new Device9Proxy(realDevice, /*isEx=*/false);
    if (logicalW > 0) proxy->SetLogicalBackBufferSize(logicalW, logicalH);
    proxy->StashBackBufferReference();
    *ppReturnedDeviceInterface = proxy;
    return hr;
}

// ---------------------------------------------------------------------------
// IDirect3D9Ex extras — only reachable when m_isEx (QI gates the cast)
// ---------------------------------------------------------------------------
UINT D3D9Proxy::GetAdapterModeCountEx(UINT A, CONST D3DDISPLAYMODEFILTER* pF)                              { UINT n = m_realEx->GetAdapterModeCountEx(A, pF); D9_TRACE_R("GetAdapterModeCountEx", n, "A=%u", A); return n; }
HRESULT D3D9Proxy::EnumAdapterModesEx(UINT A, CONST D3DDISPLAYMODEFILTER* pF, UINT M, D3DDISPLAYMODEEX* p) { HRESULT hr = m_realEx->EnumAdapterModesEx(A, pF, M, p); D9_TRACE_R("EnumAdapterModesEx", hr, "A=%u M=%u", A, M); return hr; }
HRESULT D3D9Proxy::GetAdapterDisplayModeEx(UINT A, D3DDISPLAYMODEEX* p, D3DDISPLAYROTATION* pR)            { HRESULT hr = m_realEx->GetAdapterDisplayModeEx(A, p, pR); D9_TRACE_R("GetAdapterDisplayModeEx", hr, "A=%u", A); return hr; }
HRESULT D3D9Proxy::GetAdapterLUID(UINT A, LUID* p)                                                         { HRESULT hr = m_realEx->GetAdapterLUID(A, p); D9_TRACE_R("GetAdapterLUID", hr, "A=%u", A); return hr; }

HRESULT D3D9Proxy::CreateDeviceEx(
    UINT Adapter,
    D3DDEVTYPE DeviceType,
    HWND hFocusWindow,
    DWORD BehaviorFlags,
    D3DPRESENT_PARAMETERS* pPresentationParameters,
    D3DDISPLAYMODEEX* pFullscreenDisplayMode,
    IDirect3DDevice9Ex** ppReturnedDeviceInterface)
{
    if (!ppReturnedDeviceInterface) return D3DERR_INVALIDCALL;

    D3DPRESENT_PARAMETERS modified;
    D3DPRESENT_PARAMETERS* pParams = pPresentationParameters;
    UINT logicalW = 0, logicalH = 0;
    if (pParams)
    {
        modified = *pParams;
        ResolveAndDoubleSwapchainParams(&modified, hFocusWindow, &logicalW, &logicalH);
        pParams = &modified;
    }

    IDirect3DDevice9Ex* realDevice = nullptr;
    HRESULT hr = m_realEx->CreateDeviceEx(Adapter, DeviceType, hFocusWindow,
                                          BehaviorFlags, pParams,
                                          pFullscreenDisplayMode, &realDevice);
    D9_TRACE_R("CreateDeviceEx", hr, "A=%u DT=%d hWnd=%p BF=0x%lX BB=%ux%u fmt=%d SC=%u",
               Adapter, (int)DeviceType, (void*)hFocusWindow, BehaviorFlags,
               pParams?pParams->BackBufferWidth:0, pParams?pParams->BackBufferHeight:0,
               pParams?(int)pParams->BackBufferFormat:-1, pParams?pParams->BackBufferCount:0);
    if (FAILED(hr) || !realDevice)
    {
        *ppReturnedDeviceInterface = nullptr;
        return hr;
    }
    auto* proxy = new Device9Proxy(realDevice, /*isEx=*/true);
    if (logicalW > 0) proxy->SetLogicalBackBufferSize(logicalW, logicalH);
    proxy->StashBackBufferReference();
    *ppReturnedDeviceInterface = static_cast<IDirect3DDevice9Ex*>(proxy);
    return hr;
}

} // namespace NvDirectMode
