/* wiz3D - ID3D10Device proxy implementation (Option B for DX10) */

#include "StdAfx.h"
#include "Device10Proxy.h"
#include "Texture2D10Proxy.h"
#include "RTV10Proxy.h"
#include "DSV10Proxy.h"
#include "Buffer10Proxy.h"
#include "SRV10Proxy.h"
#include "Texture1D10Proxy.h"
#include "Texture3D10Proxy.h"
#include "DXGIDevice10Proxy.h"
#include "StereoHeuristic.h"
#include "proxy_factory.h"     // IID_wiz3D_Device10Proxy + TryUnwrap_10 helpers
#include "AdapterFunctions.h"  // DDILog
#include "ShaderHunter.h"      // cycle 3: NoteDraw / ShouldSkipDraw per-draw hunt integration
#include "../S3DAPI/GlobalData.h"       // g_ProfileData
#include "../S3DAPI/ShaderProfileData.h"  // ShaderProfileData
#include <cmath>

#include <dxgi.h>
#include <dxgi1_2.h>
#include <dxgi1_3.h>

#pragma comment(lib, "dxguid.lib")

// Per-resource caps on stack-allocated temp arrays for proxy unwrap.
// MUST match the D3D10 spec slot maximum per resource type — undersizing makes
// the runtime walk past our array into uninitialized stack memory and treat
// random values as pointers, dereference them, and AV. See Context11Proxy.cpp
// for the same rationale on the DX11 side (May 2026 Metro 2033 crash fix).
static constexpr UINT kMaxSRVs       = 128;  // D3D10_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT
static constexpr UINT kMaxSamplers   = 16;   // D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT
static constexpr UINT kMaxCBs        = 15;   // D3D10_1_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT (14 in plain D3D10)
static constexpr UINT kMaxRTVs       = 8;    // D3D10_SIMULTANEOUS_RENDER_TARGET_COUNT
static constexpr UINT kMaxVBs        = 16;   // D3D10_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT
static constexpr UINT kMaxSOBuffers  = 4;    // D3D10_SO_BUFFER_SLOT_COUNT

namespace wiz3d
{

// Forward UnwrapBuf10 helper — same shape as the DX11 UnwrapBuf in
// Context11Proxy.cpp. Passing a Buffer10Proxy directly to D3D10 would crash
// because the runtime doesn't understand our vtable past ID3D10Buffer.
static inline ID3D10Buffer* UnwrapBuf10(ID3D10Buffer* p)
{
    if (!p) return nullptr;
    if (auto* bp = TryUnwrapBuffer_10(static_cast<ID3D10Resource*>(p)))
        return bp->GetReal();
    return p;
}

// Eye-aware resource unwrap. Texture proxies pick left vs right based on
// the active eye; Buffer proxies have no eye distinction (single real).
static inline ID3D10Resource* UnwrapResForEye10(ID3D10Resource* p, bool pickRight)
{
    if (!p) return nullptr;
    if (auto* tex = TryUnwrapTexture2D_10(p))
    {
        ID3D10Resource* right = tex->GetRealRight();
        return (pickRight && right) ? right : static_cast<ID3D10Resource*>(tex->GetReal());
    }
    if (auto* buf = TryUnwrapBuffer_10(p))
        return static_cast<ID3D10Resource*>(buf->GetReal());
    return p;
}

// Stage 3c.2 DX10: eye-aware SRV unwrap helper. SRV10Proxy carries an
// optional right-eye sibling — when active eye is Right, point the runtime
// at it so downstream samples see right-eye RT content.
static inline ID3D10ShaderResourceView* UnwrapSRV10ForEye(
    ID3D10ShaderResourceView* p, bool pickRight)
{
    if (!p) return nullptr;
    if (auto* sp = TryUnwrapSRV_10(p))
    {
        ID3D10ShaderResourceView* right = sp->GetRealRight();
        return (pickRight && right) ? right : sp->GetReal();
    }
    return p;
}

Device10Proxy::Device10Proxy(ID3D10Device* real)
    : m_real(real)
    , m_refs(1)
    , m_activeEye(Eye::Left)
    , m_logicalWidth(0)
    , m_logicalHeight(0)
    , m_presentHookActive(false)
    , m_dxgiDeviceProxy(nullptr)
    , m_boundVS(nullptr)
{
    InitializeCriticalSection(&m_dxgiCacheLock);
    InitializeCriticalSection(&m_shaderProjLock);
    InitializeCriticalSection(&m_vsTwinsLock10);
    for (UINT i = 0; i < kMaxVSCBSlots; ++i) m_boundVSCBs[i] = nullptr;
}

void Device10Proxy::StoreShaderProjection(void* shaderPtr, const ShaderAnalysis11Result& info)
{
    if (!shaderPtr) return;
    EnterCriticalSection(&m_shaderProjLock);
    m_shaderProjections[shaderPtr] = info;
    LeaveCriticalSection(&m_shaderProjLock);
}

const ShaderAnalysis11Result* Device10Proxy::LookupShaderProjection(void* shaderPtr) const
{
    if (!shaderPtr) return nullptr;
    auto* self = const_cast<Device10Proxy*>(this);
    EnterCriticalSection(&self->m_shaderProjLock);
    auto it = m_shaderProjections.find(shaderPtr);
    const ShaderAnalysis11Result* p = (it == m_shaderProjections.end()) ? nullptr : &it->second;
    LeaveCriticalSection(&self->m_shaderProjLock);
    return p;
}

void Device10Proxy::ClearFrameCommands()
{
    m_frameCommands.clear();
}

void Device10Proxy::ReplayFrameCommands(Eye eye)
{
    Eye saved = m_activeEye;
    m_activeEye = eye;
    for (auto& fn : m_frameCommands) fn();
    m_activeEye = saved;
}

Device10Proxy::~Device10Proxy()
{
    // Sever the back-pointer first so any outstanding game-held refs on
    // the DXGI proxy don't route QI(ID3D10Device) through a destructed
    // parent. Then drop our cache ref — the proxy self-deletes if no
    // other refs exist.
    if (m_dxgiDeviceProxy)
    {
        m_dxgiDeviceProxy->DetachParent();
        m_dxgiDeviceProxy->Release();
        m_dxgiDeviceProxy = nullptr;
    }
    // Release DXBC-twin cache (cycle 2b).
    for (auto& e : m_vsTwins10)
        if (e.second.shader) e.second.shader->Release();
    m_vsTwins10.clear();
    if (m_twinCB10)    { m_twinCB10->Release();    m_twinCB10    = nullptr; }
    if (m_twinSavedVS10) { m_twinSavedVS10->Release(); m_twinSavedVS10 = nullptr; }
    if (m_twinSavedCB10) { m_twinSavedCB10->Release(); m_twinSavedCB10 = nullptr; }

    DeleteCriticalSection(&m_dxgiCacheLock);
    DeleteCriticalSection(&m_shaderProjLock);
    DeleteCriticalSection(&m_vsTwinsLock10);
}

DXGIDevice10Proxy* Device10Proxy::GetOrCreateDXGIDeviceProxyAddRef()
{
    EnterCriticalSection(&m_dxgiCacheLock);
    if (m_dxgiDeviceProxy)
    {
        m_dxgiDeviceProxy->AddRef();
        DXGIDevice10Proxy* hit = m_dxgiDeviceProxy;
        LeaveCriticalSection(&m_dxgiCacheLock);
        return hit;
    }

    IDXGIDevice*  r0 = nullptr;
    IDXGIDevice1* r1 = nullptr;
    IDXGIDevice2* r2 = nullptr;
    IDXGIDevice3* r3 = nullptr;
    HRESULT hr0 = m_real->QueryInterface(IID_IDXGIDevice,  reinterpret_cast<void**>(&r0));
    if (FAILED(hr0) || !r0)
    {
        LeaveCriticalSection(&m_dxgiCacheLock);
        return nullptr;
    }
    m_real->QueryInterface(IID_IDXGIDevice1, reinterpret_cast<void**>(&r1));
    m_real->QueryInterface(IID_IDXGIDevice2, reinterpret_cast<void**>(&r2));
    m_real->QueryInterface(IID_IDXGIDevice3, reinterpret_cast<void**>(&r3));

    // ctor sets m_refs=1 (cache's ref); AddRef again for caller.
    m_dxgiDeviceProxy = new DXGIDevice10Proxy(this, r0, r1, r2, r3);
    m_dxgiDeviceProxy->AddRef();
    DXGIDevice10Proxy* hit = m_dxgiDeviceProxy;
    LeaveCriticalSection(&m_dxgiCacheLock);
    return hit;
}

HRESULT STDMETHODCALLTYPE Device10Proxy::QueryInterface(REFIID riid, void** ppvObj)
{
    if (!ppvObj) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_ID3D10Device)
    {
        *ppvObj = static_cast<ID3D10Device*>(this);
        AddRef();
        return S_OK;
    }
    // Private IID for cross-DLL identity (DXGIFactoryWrapper's Option B
    // factory hook QIs incoming pDevice to detect a Device10Proxy).
    if (riid == IID_wiz3D_Device10Proxy)
    {
        *ppvObj = static_cast<IUnknown*>(static_cast<ID3D10Device*>(this));
        AddRef();
        return S_OK;
    }
    // IDXGIDevice family: route through DXGIDevice10Proxy so the game's
    // `dev->QI(IDXGIDevice)->QI(ID3D10Device)` round-trip lands on this
    // Device10Proxy. Without this, the runtime passes the unwrapped real
    // device to factory->CreateSwapChain and our factory hook misses.
    if (riid == IID_IDXGIDevice  ||
        riid == IID_IDXGIDevice1 ||
        riid == IID_IDXGIDevice2 ||
        riid == IID_IDXGIDevice3)
    {
        DXGIDevice10Proxy* dp = GetOrCreateDXGIDeviceProxyAddRef();
        if (!dp) return E_NOINTERFACE;
        HRESULT hr = dp->QueryInterface(riid, ppvObj);
        dp->Release();
        return hr;
    }
    // ID3D10Device1 + vendor IIDs — pass through unwrapped for now.
    return m_real->QueryInterface(riid, ppvObj);
}

// ---------------------------------------------------------------------------
// CreateXxx — wrap returned resources/views so downstream Try*Unwrap_10
// picks them up at the COM boundary.
// ---------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE Device10Proxy::CreateBuffer(
    const D3D10_BUFFER_DESC* pDesc,
    const D3D10_SUBRESOURCE_DATA* pInitialData,
    ID3D10Buffer** ppBuffer)
{
    HRESULT hr = m_real->CreateBuffer(pDesc, pInitialData, ppBuffer);
    if (FAILED(hr) || !ppBuffer || !*ppBuffer) return hr;
    auto* bufProxy = new Buffer10Proxy(*ppBuffer, this);
    *ppBuffer = static_cast<ID3D10Buffer*>(bufProxy);
    return hr;
}

HRESULT STDMETHODCALLTYPE Device10Proxy::CreateTexture2D(
    const D3D10_TEXTURE2D_DESC* pDesc,
    const D3D10_SUBRESOURCE_DATA* pInitialData,
    ID3D10Texture2D** ppTexture2D)
{
    HRESULT hr = m_real->CreateTexture2D(pDesc, pInitialData, ppTexture2D);
    if (FAILED(hr) || !ppTexture2D || !*ppTexture2D) return hr;

    SIZE bbSize = { (LONG)m_logicalWidth, (LONG)m_logicalHeight };
    const SIZE* pBBSize = (m_logicalWidth > 0 && m_logicalHeight > 0) ? &bbSize : nullptr;

    ID3D10Texture2D* realLeft  = *ppTexture2D;
    ID3D10Texture2D* realRight = nullptr;
    if (ShouldDoubleTexture2D(pDesc, pBBSize))
    {
        HRESULT hr2 = m_real->CreateTexture2D(pDesc, nullptr, &realRight);
        if (FAILED(hr2)) realRight = nullptr;
    }

    auto* texProxy = new Texture2D10Proxy(realLeft, realRight, this);
    *ppTexture2D = static_cast<ID3D10Texture2D*>(texProxy);
    return hr;
}

HRESULT STDMETHODCALLTYPE Device10Proxy::CreateRenderTargetView(
    ID3D10Resource* pResource, const D3D10_RENDER_TARGET_VIEW_DESC* pDesc,
    ID3D10RenderTargetView** ppRTView)
{
    Texture2D10Proxy* texProxy  = TryUnwrapTexture2D_10(pResource);
    Buffer10Proxy*    bufProxy  = TryUnwrapBuffer_10(pResource);
    ID3D10Resource*   realLeft  = texProxy ? static_cast<ID3D10Resource*>(texProxy->GetReal())
                                : bufProxy ? static_cast<ID3D10Resource*>(bufProxy->GetReal())
                                           : pResource;
    ID3D10Resource*   realRight = texProxy ? static_cast<ID3D10Resource*>(texProxy->GetRealRight())
                                           : nullptr;

    HRESULT hr = m_real->CreateRenderTargetView(realLeft, pDesc, ppRTView);
    if (FAILED(hr) || !ppRTView || !*ppRTView) return hr;

    if (texProxy)
    {
        ID3D10RenderTargetView* realRightRTV = nullptr;
        if (realRight)
        {
            if (FAILED(m_real->CreateRenderTargetView(realRight, pDesc, &realRightRTV)))
                realRightRTV = nullptr;
        }
        auto* rtvProxy = new RTV10Proxy(*ppRTView, realRightRTV, this);
        *ppRTView = static_cast<ID3D10RenderTargetView*>(rtvProxy);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE Device10Proxy::CreateDepthStencilView(
    ID3D10Resource* pResource, const D3D10_DEPTH_STENCIL_VIEW_DESC* pDesc,
    ID3D10DepthStencilView** ppDepthStencilView)
{
    Texture2D10Proxy* texProxy  = TryUnwrapTexture2D_10(pResource);
    Buffer10Proxy*    bufProxy  = TryUnwrapBuffer_10(pResource);
    ID3D10Resource*   realLeft  = texProxy ? static_cast<ID3D10Resource*>(texProxy->GetReal())
                                : bufProxy ? static_cast<ID3D10Resource*>(bufProxy->GetReal())
                                           : pResource;
    ID3D10Resource*   realRight = texProxy ? static_cast<ID3D10Resource*>(texProxy->GetRealRight())
                                           : nullptr;

    HRESULT hr = m_real->CreateDepthStencilView(realLeft, pDesc, ppDepthStencilView);
    if (FAILED(hr) || !ppDepthStencilView || !*ppDepthStencilView) return hr;

    if (texProxy)
    {
        ID3D10DepthStencilView* realRightDSV = nullptr;
        if (realRight)
        {
            if (FAILED(m_real->CreateDepthStencilView(realRight, pDesc, &realRightDSV)))
                realRightDSV = nullptr;
        }
        auto* dsvProxy = new DSV10Proxy(*ppDepthStencilView, realRightDSV, this);
        *ppDepthStencilView = static_cast<ID3D10DepthStencilView*>(dsvProxy);
    }
    return hr;
}

// DX10 Stage 3c.2 regression fix: Tex1D/Tex3D wrappers were causing crashes
// in Lost Planet + Far Cry 2 at startup. D3D10/d3d11 runtime internals walk
// internal struct fields on resources directly (not via COM vtable), and
// our proxy lacks those fields → segfault inside d3d11.dll. Reverted to
// passthrough; SRV10Proxy still wraps the SRV result, just doesn't try to
// unwrap a wrapped Tex1D/Tex3D input (there isn't one anymore).
HRESULT STDMETHODCALLTYPE Device10Proxy::CreateTexture1D(
    const D3D10_TEXTURE1D_DESC* pDesc,
    const D3D10_SUBRESOURCE_DATA* pInitialData,
    ID3D10Texture1D** ppTexture1D)
{
    return m_real->CreateTexture1D(pDesc, pInitialData, ppTexture1D);
}

HRESULT STDMETHODCALLTYPE Device10Proxy::CreateTexture3D(
    const D3D10_TEXTURE3D_DESC* pDesc,
    const D3D10_SUBRESOURCE_DATA* pInitialData,
    ID3D10Texture3D** ppTexture3D)
{
    return m_real->CreateTexture3D(pDesc, pInitialData, ppTexture3D);
}

HRESULT STDMETHODCALLTYPE Device10Proxy::CreateShaderResourceView(
    ID3D10Resource* pResource, const D3D10_SHADER_RESOURCE_VIEW_DESC* pDesc,
    ID3D10ShaderResourceView** ppSRView)
{
    // Stage 3c.2 DX10 port: unwrap Texture2D / Buffer inputs (Tex1D/Tex3D
    // aren't wrapped — see CreateTexture1D/CreateTexture3D for the
    // rationale). SRV result still gets wrapped so downstream
    // *SetShaderResources can eye-aware-route Tex2D-backed SRVs.
    Texture2D10Proxy* tex2Proxy = TryUnwrapTexture2D_10(pResource);
    Buffer10Proxy*    bufProxy  = TryUnwrapBuffer_10(pResource);

    ID3D10Resource* realLeftRes  = tex2Proxy ? static_cast<ID3D10Resource*>(tex2Proxy->GetReal())
                                 : bufProxy  ? static_cast<ID3D10Resource*>(bufProxy->GetReal())
                                             : pResource;
    ID3D10Resource* realRightRes = tex2Proxy ? static_cast<ID3D10Resource*>(tex2Proxy->GetRealRight())
                                             : nullptr;

    HRESULT hr = m_real->CreateShaderResourceView(realLeftRes, pDesc, ppSRView);
    if (FAILED(hr) || !ppSRView || !*ppSRView) return hr;

    ID3D10ShaderResourceView* realRightSRV = nullptr;
    if (realRightRes)
    {
        if (FAILED(m_real->CreateShaderResourceView(realRightRes, pDesc, &realRightSRV)))
            realRightSRV = nullptr;
    }

    auto* srvProxy = new SRV10Proxy(*ppSRView, realRightSRV, this);
    *ppSRView = static_cast<ID3D10ShaderResourceView*>(srvProxy);
    return hr;
}

// ---------------------------------------------------------------------------
// *SetConstantBuffers — unwrap + VS-pipeline tag (Stage 4c.1 carryover).
// ---------------------------------------------------------------------------
#define D3D10_CB_SET(STAGE_PREFIX, IS_VS_PIPELINE, ST10_STAGE)                              \
void STDMETHODCALLTYPE Device10Proxy::STAGE_PREFIX##SetConstantBuffers(                     \
    UINT StartSlot, UINT NumBuffers, ID3D10Buffer* const* ppConstantBuffers)                \
{                                                                                           \
    ID3D10Buffer* raw[kMaxCBs] = { 0 };                                                     \
    UINT cap = NumBuffers <= kMaxCBs ? NumBuffers : kMaxCBs;                                \
    for (UINT i = 0; i < cap; ++i)                                                          \
    {                                                                                       \
        ID3D10Buffer* p = ppConstantBuffers ? ppConstantBuffers[i] : nullptr;               \
        UINT slot = StartSlot + i;                                                          \
        Buffer10Proxy* bp = p ? TryUnwrapBuffer_10(static_cast<ID3D10Resource*>(p)) : nullptr; \
        if (slot < kMaxVSCBSlots) m_cbSlots[ST10_STAGE][slot] = bp;                         \
        if (p)                                                                              \
        {                                                                                   \
            if (bp)                                                                         \
            {                                                                               \
                if (IS_VS_PIPELINE) bp->TagVSBound();                                       \
                raw[i] = bp->GetReal();                                                     \
            }                                                                               \
            else                                                                            \
            {                                                                               \
                raw[i] = p;                                                                 \
            }                                                                               \
        }                                                                                   \
    }                                                                                       \
    m_real->STAGE_PREFIX##SetConstantBuffers(StartSlot, NumBuffers,                         \
        ppConstantBuffers ? raw : nullptr);                                                 \
    if (!m_presentHookActive) return;                                                       \
    std::vector<ComRefHolder10> refs;                                                       \
    refs.reserve(NumBuffers);                                                               \
    for (UINT i = 0; i < NumBuffers; ++i)                                                   \
        refs.emplace_back(ppConstantBuffers ? ppConstantBuffers[i] : nullptr);              \
    m_frameCommands.emplace_back(                                                           \
        [this, StartSlot, NumBuffers, refs]() {                                             \
            ID3D10Buffer* rraw[kMaxCBs] = { 0 };                                            \
            UINT rcap = NumBuffers <= kMaxCBs ? NumBuffers : kMaxCBs;                       \
            for (UINT i = 0; i < rcap; ++i)                                                 \
                rraw[i] = UnwrapBuf10(static_cast<ID3D10Buffer*>(refs[i].p));               \
            m_real->STAGE_PREFIX##SetConstantBuffers(StartSlot, NumBuffers, rraw);          \
        });                                                                                 \
}
D3D10_CB_SET(PS, 0, ST10_PS)
D3D10_CB_SET(GS, 1, ST10_GS)
#undef D3D10_CB_SET

// Stage 4e DX10: VS variant that ALSO snapshots m_boundVSCBs[] so
// Buffer10Proxy::Unmap can find which slot a CB is bound at on the VS
// pipeline. Otherwise identical to the macro-generated PS/GS versions.
void STDMETHODCALLTYPE Device10Proxy::VSSetConstantBuffers(
    UINT StartSlot, UINT NumBuffers, ID3D10Buffer* const* ppConstantBuffers)
{
    ID3D10Buffer* raw[kMaxCBs] = { 0 };
    UINT cap = NumBuffers <= kMaxCBs ? NumBuffers : kMaxCBs;
    for (UINT i = 0; i < cap; ++i)
    {
        ID3D10Buffer* p = ppConstantBuffers ? ppConstantBuffers[i] : nullptr;
        UINT slot = StartSlot + i;
        if (slot < kMaxVSCBSlots) m_boundVSCBs[slot] = p;
        // Option A per-stage CB tracking (cycle 2): mirror into m_cbSlots.
        Buffer10Proxy* bp = p ? TryUnwrapBuffer_10(static_cast<ID3D10Resource*>(p)) : nullptr;
        if (slot < kMaxVSCBSlots) m_cbSlots[ST10_VS][slot] = bp;
        if (p)
        {
            if (bp)
            {
                bp->TagVSBound();
                raw[i] = bp->GetReal();
            }
            else
            {
                raw[i] = p;
            }
        }
    }
    m_real->VSSetConstantBuffers(StartSlot, NumBuffers, ppConstantBuffers ? raw : nullptr);
    if (!m_presentHookActive) return;
    std::vector<ComRefHolder10> refs;
    refs.reserve(NumBuffers);
    for (UINT i = 0; i < NumBuffers; ++i)
        refs.emplace_back(ppConstantBuffers ? ppConstantBuffers[i] : nullptr);
    m_frameCommands.emplace_back(
        [this, StartSlot, NumBuffers, refs]() {
            ID3D10Buffer* rraw[kMaxCBs] = { 0 };
            UINT rcap = NumBuffers <= kMaxCBs ? NumBuffers : kMaxCBs;
            for (UINT i = 0; i < rcap; ++i)
                rraw[i] = UnwrapBuf10(static_cast<ID3D10Buffer*>(refs[i].p));
            m_real->VSSetConstantBuffers(StartSlot, NumBuffers, rraw);
        });
}

void STDMETHODCALLTYPE Device10Proxy::IASetInputLayout(ID3D10InputLayout* pInputLayout)
{
    m_real->IASetInputLayout(pInputLayout);
    if (!m_presentHookActive) return;
    ComRefHolder10 layoutRef(pInputLayout);
    m_frameCommands.emplace_back(
        [this, layoutRef]() {
            m_real->IASetInputLayout(static_cast<ID3D10InputLayout*>(layoutRef.p));
        });
}

void STDMETHODCALLTYPE Device10Proxy::IASetVertexBuffers(
    UINT StartSlot, UINT NumBuffers, ID3D10Buffer* const* ppVertexBuffers,
    const UINT* pStrides, const UINT* pOffsets)
{
    ID3D10Buffer* raw[kMaxVBs] = { 0 };
    UINT cap = NumBuffers <= kMaxVBs ? NumBuffers : kMaxVBs;
    for (UINT i = 0; i < cap; ++i)
        raw[i] = ppVertexBuffers ? UnwrapBuf10(ppVertexBuffers[i]) : nullptr;
    m_real->IASetVertexBuffers(StartSlot, NumBuffers,
        ppVertexBuffers ? raw : nullptr, pStrides, pOffsets);
    if (!m_presentHookActive) return;
    std::vector<ComRefHolder10> bufRefs;
    bufRefs.reserve(NumBuffers);
    for (UINT i = 0; i < NumBuffers; ++i)
        bufRefs.emplace_back(ppVertexBuffers ? ppVertexBuffers[i] : nullptr);
    std::vector<UINT> strides, offsets;
    if (pStrides) strides.assign(pStrides, pStrides + NumBuffers);
    if (pOffsets) offsets.assign(pOffsets, pOffsets + NumBuffers);
    m_frameCommands.emplace_back(
        [this, StartSlot, NumBuffers, bufRefs, strides, offsets]() {
            ID3D10Buffer* rraw[kMaxVBs] = { 0 };
            UINT rcap = NumBuffers <= kMaxVBs ? NumBuffers : kMaxVBs;
            for (UINT i = 0; i < rcap; ++i)
                rraw[i] = UnwrapBuf10(static_cast<ID3D10Buffer*>(bufRefs[i].p));
            m_real->IASetVertexBuffers(StartSlot, NumBuffers, rraw,
                strides.empty() ? nullptr : strides.data(),
                offsets.empty() ? nullptr : offsets.data());
        });
}

void STDMETHODCALLTYPE Device10Proxy::IASetIndexBuffer(
    ID3D10Buffer* pIndexBuffer, DXGI_FORMAT Format, UINT Offset)
{
    m_real->IASetIndexBuffer(UnwrapBuf10(pIndexBuffer), Format, Offset);
    if (!m_presentHookActive) return;
    ComRefHolder10 bufRef(pIndexBuffer);
    m_frameCommands.emplace_back(
        [this, bufRef, Format, Offset]() {
            m_real->IASetIndexBuffer(UnwrapBuf10(static_cast<ID3D10Buffer*>(bufRef.p)),
                                     Format, Offset);
        });
}

void STDMETHODCALLTYPE Device10Proxy::SOSetTargets(
    UINT NumBuffers, ID3D10Buffer* const* ppSOTargets, const UINT* pOffsets)
{
    ID3D10Buffer* raw[kMaxSOBuffers] = { 0 };
    UINT cap = NumBuffers <= kMaxSOBuffers ? NumBuffers : kMaxSOBuffers;
    for (UINT i = 0; i < cap; ++i)
        raw[i] = ppSOTargets ? UnwrapBuf10(ppSOTargets[i]) : nullptr;
    m_real->SOSetTargets(NumBuffers,
        ppSOTargets ? raw : nullptr, pOffsets);
    if (!m_presentHookActive) return;
    std::vector<ComRefHolder10> bufRefs;
    bufRefs.reserve(NumBuffers);
    for (UINT i = 0; i < NumBuffers; ++i)
        bufRefs.emplace_back(ppSOTargets ? ppSOTargets[i] : nullptr);
    std::vector<UINT> offsets;
    if (pOffsets) offsets.assign(pOffsets, pOffsets + NumBuffers);
    m_frameCommands.emplace_back(
        [this, NumBuffers, bufRefs, offsets]() {
            ID3D10Buffer* rraw[kMaxSOBuffers] = { 0 };
            UINT rcap = NumBuffers <= kMaxSOBuffers ? NumBuffers : kMaxSOBuffers;
            for (UINT i = 0; i < rcap; ++i)
                rraw[i] = UnwrapBuf10(static_cast<ID3D10Buffer*>(bufRefs[i].p));
            m_real->SOSetTargets(NumBuffers, rraw,
                offsets.empty() ? nullptr : offsets.data());
        });
}

// ---------------------------------------------------------------------------
// OMSetRenderTargets — eye-aware. Stage 1 of the port: m_activeEye starts
// Left and never changes (no replay sweep yet for DX10) so the right-eye
// siblings allocated by CreateTexture2D / CreateRenderTargetView are
// allocated but never bound. Stage 4 of the DX10 port will toggle the eye.
// ---------------------------------------------------------------------------
// Split into Do* + recording wrapper so the replay closure can re-run the
// eye selection with whatever m_activeEye is set to at replay time.
static void DoOMSetRenderTargets_internal(
    Device10Proxy* self, ID3D10Device* real, bool pickRight,
    UINT NumViews, ID3D10RenderTargetView* const* ppRenderTargetViews,
    ID3D10DepthStencilView* pDepthStencilView)
{
    ID3D10RenderTargetView* rawRTVs[kMaxRTVs] = { 0 };
    ID3D10RenderTargetView* const* rtvsToUse = ppRenderTargetViews;
    if (NumViews > 0 && ppRenderTargetViews)
    {
        UINT cap = NumViews <= kMaxRTVs ? NumViews : kMaxRTVs;
        for (UINT i = 0; i < cap; ++i)
        {
            RTV10Proxy* p = TryUnwrapRTV_10(ppRenderTargetViews[i]);
            if (!p) { rawRTVs[i] = ppRenderTargetViews[i]; continue; }
            ID3D10RenderTargetView* right = p->GetRealRight();
            rawRTVs[i] = (pickRight && right) ? right : p->GetReal();
        }
        rtvsToUse = rawRTVs;
    }
    ID3D10DepthStencilView* realDSV = pDepthStencilView;
    if (DSV10Proxy* d = TryUnwrapDSV_10(pDepthStencilView))
    {
        ID3D10DepthStencilView* right = d->GetRealRight();
        realDSV = (pickRight && right) ? right : d->GetReal();
    }
    real->OMSetRenderTargets(NumViews, rtvsToUse, realDSV);
    (void)self;  // self unused but kept for future expansion
}

void STDMETHODCALLTYPE Device10Proxy::OMSetRenderTargets(
    UINT NumViews, ID3D10RenderTargetView* const* ppRenderTargetViews,
    ID3D10DepthStencilView* pDepthStencilView)
{
    bool pickRight = (m_activeEye == Eye::Right);
    DoOMSetRenderTargets_internal(this, m_real, pickRight,
                                  NumViews, ppRenderTargetViews, pDepthStencilView);
    if (!m_presentHookActive) return;

    // Capture wrapped pointers so replay re-runs eye-aware unwrap with the
    // active eye set by ReplayFrameCommands.
    std::vector<ComRefHolder10> rtvRefs;
    rtvRefs.reserve(NumViews);
    for (UINT i = 0; i < NumViews; ++i)
        rtvRefs.emplace_back(ppRenderTargetViews ? ppRenderTargetViews[i] : nullptr);
    ComRefHolder10 dsvRef(pDepthStencilView);
    m_frameCommands.emplace_back(
        [this, NumViews, rtvRefs, dsvRef]() {
            ID3D10RenderTargetView* raw[kMaxRTVs] = { 0 };
            UINT cap = NumViews <= kMaxRTVs ? NumViews : kMaxRTVs;
            for (UINT i = 0; i < cap; ++i)
                raw[i] = static_cast<ID3D10RenderTargetView*>(rtvRefs[i].p);
            bool pickRightReplay = (m_activeEye == Eye::Right);
            DoOMSetRenderTargets_internal(this, m_real, pickRightReplay,
                                          NumViews, raw,
                                          static_cast<ID3D10DepthStencilView*>(dsvRef.p));
        });
}

void STDMETHODCALLTYPE Device10Proxy::CopyResource(
    ID3D10Resource* pDstResource, ID3D10Resource* pSrcResource)
{
    bool pickRight = (m_activeEye == Eye::Right);
    m_real->CopyResource(UnwrapResForEye10(pDstResource, pickRight),
                         UnwrapResForEye10(pSrcResource, pickRight));
}

void STDMETHODCALLTYPE Device10Proxy::CopySubresourceRegion(
    ID3D10Resource* pDstResource, UINT DstSubresource, UINT DstX, UINT DstY,
    UINT DstZ, ID3D10Resource* pSrcResource, UINT SrcSubresource,
    const D3D10_BOX* pSrcBox)
{
    bool pickRight = (m_activeEye == Eye::Right);
    m_real->CopySubresourceRegion(
        UnwrapResForEye10(pDstResource, pickRight), DstSubresource, DstX, DstY, DstZ,
        UnwrapResForEye10(pSrcResource, pickRight), SrcSubresource, pSrcBox);
}

void STDMETHODCALLTYPE Device10Proxy::UpdateSubresource(
    ID3D10Resource* pDstResource, UINT DstSubresource, const D3D10_BOX* pDstBox,
    const void* pSrcData, UINT SrcRowPitch, UINT SrcDepthPitch)
{
    bool pickRight = (m_activeEye == Eye::Right);
    m_real->UpdateSubresource(UnwrapResForEye10(pDstResource, pickRight),
                              DstSubresource, pDstBox, pSrcData, SrcRowPitch, SrcDepthPitch);
}

void STDMETHODCALLTYPE Device10Proxy::ResolveSubresource(
    ID3D10Resource* pDstResource, UINT DstSubresource,
    ID3D10Resource* pSrcResource, UINT SrcSubresource, DXGI_FORMAT Format)
{
    bool pickRight = (m_activeEye == Eye::Right);
    m_real->ResolveSubresource(
        UnwrapResForEye10(pDstResource, pickRight), DstSubresource,
        UnwrapResForEye10(pSrcResource, pickRight), SrcSubresource, Format);
}

void STDMETHODCALLTYPE Device10Proxy::GenerateMips(ID3D10ShaderResourceView* pShaderResourceView)
{
    bool pickRight = (m_activeEye == Eye::Right);
    m_real->GenerateMips(UnwrapSRV10ForEye(pShaderResourceView, pickRight));
}

static void DoClearRTV_internal(
    ID3D10Device* real, bool pickRight,
    ID3D10RenderTargetView* pRenderTargetView, const FLOAT ColorRGBA[4])
{
    ID3D10RenderTargetView* r = pRenderTargetView;
    if (auto* rtv = TryUnwrapRTV_10(pRenderTargetView))
    {
        ID3D10RenderTargetView* right = rtv->GetRealRight();
        r = (pickRight && right) ? right : rtv->GetReal();
    }
    real->ClearRenderTargetView(r, ColorRGBA);
}

void STDMETHODCALLTYPE Device10Proxy::ClearRenderTargetView(
    ID3D10RenderTargetView* pRenderTargetView, const FLOAT ColorRGBA[4])
{
    bool pickRight = (m_activeEye == Eye::Right);
    DoClearRTV_internal(m_real, pickRight, pRenderTargetView, ColorRGBA);
    if (!m_presentHookActive) return;
    ComRefHolder10 rtvRef(pRenderTargetView);
    FLOAT color[4] = { 0, 0, 0, 0 };
    if (ColorRGBA)
    {
        color[0] = ColorRGBA[0]; color[1] = ColorRGBA[1];
        color[2] = ColorRGBA[2]; color[3] = ColorRGBA[3];
    }
    m_frameCommands.emplace_back(
        [this, rtvRef, color]() {
            bool pickRightReplay = (m_activeEye == Eye::Right);
            DoClearRTV_internal(m_real, pickRightReplay,
                static_cast<ID3D10RenderTargetView*>(rtvRef.p), color);
        });
}

static void DoClearDSV_internal(
    ID3D10Device* real, bool pickRight,
    ID3D10DepthStencilView* pDepthStencilView, UINT ClearFlags, FLOAT Depth, UINT8 Stencil)
{
    ID3D10DepthStencilView* d = pDepthStencilView;
    if (auto* dsv = TryUnwrapDSV_10(pDepthStencilView))
    {
        ID3D10DepthStencilView* right = dsv->GetRealRight();
        d = (pickRight && right) ? right : dsv->GetReal();
    }
    real->ClearDepthStencilView(d, ClearFlags, Depth, Stencil);
}

void STDMETHODCALLTYPE Device10Proxy::ClearDepthStencilView(
    ID3D10DepthStencilView* pDepthStencilView, UINT ClearFlags, FLOAT Depth, UINT8 Stencil)
{
    bool pickRight = (m_activeEye == Eye::Right);
    DoClearDSV_internal(m_real, pickRight, pDepthStencilView, ClearFlags, Depth, Stencil);
    if (!m_presentHookActive) return;
    ComRefHolder10 dsvRef(pDepthStencilView);
    m_frameCommands.emplace_back(
        [this, dsvRef, ClearFlags, Depth, Stencil]() {
            bool pickRightReplay = (m_activeEye == Eye::Right);
            DoClearDSV_internal(m_real, pickRightReplay,
                static_cast<ID3D10DepthStencilView*>(dsvRef.p),
                ClearFlags, Depth, Stencil);
        });
}

// ---------------------------------------------------------------------------
// Recording macros — mirror Context11Proxy's RECORD_* patterns at the D3D10
// API level. Same shape: execute via m_real immediately, then if the Present
// hook is active push a replay closure into m_frameCommands that re-invokes
// at right-eye time.
// ---------------------------------------------------------------------------

// *SetShaderResources (VS / PS / GS)
#define D3D10_SRV_SET(STAGE_PREFIX, ST10_STAGE)                                             \
void STDMETHODCALLTYPE Device10Proxy::STAGE_PREFIX##SetShaderResources(                     \
    UINT StartSlot, UINT NumViews, ID3D10ShaderResourceView* const* ppShaderResourceViews)  \
{                                                                                           \
    ID3D10ShaderResourceView* rawSet[kMaxSRVs] = { 0 };                                     \
    UINT setCap = NumViews <= kMaxSRVs ? NumViews : kMaxSRVs;                               \
    bool pickRight = (m_activeEye == Eye::Right);                                           \
    for (UINT i = 0; i < setCap; ++i)                                                       \
    {                                                                                       \
        ID3D10ShaderResourceView* pSRV = ppShaderResourceViews ? ppShaderResourceViews[i]   \
                                                               : nullptr;                   \
        UINT slot = StartSlot + i;                                                          \
        if (slot < kMaxSRVSlots10) m_srvSlots[ST10_STAGE][slot] = pSRV;                     \
        rawSet[i] = UnwrapSRV10ForEye(pSRV, pickRight);                                     \
    }                                                                                       \
    m_real->STAGE_PREFIX##SetShaderResources(StartSlot, NumViews,                           \
        ppShaderResourceViews ? rawSet : nullptr);                                          \
    if (!m_presentHookActive) return;                                                       \
    std::vector<ComRefHolder10> refs;                                                       \
    refs.reserve(NumViews);                                                                 \
    for (UINT i = 0; i < NumViews; ++i)                                                     \
        refs.emplace_back(ppShaderResourceViews ? ppShaderResourceViews[i] : nullptr);      \
    m_frameCommands.emplace_back(                                                           \
        [this, StartSlot, NumViews, refs]() {                                               \
            ID3D10ShaderResourceView* raw[kMaxSRVs] = { 0 };                                \
            UINT cap = NumViews <= kMaxSRVs ? NumViews : kMaxSRVs;                          \
            bool pr = (m_activeEye == Eye::Right);                                          \
            for (UINT i = 0; i < cap; ++i)                                                  \
                raw[i] = UnwrapSRV10ForEye(                                                 \
                    static_cast<ID3D10ShaderResourceView*>(refs[i].p), pr);                 \
            m_real->STAGE_PREFIX##SetShaderResources(StartSlot, NumViews, raw);             \
        });                                                                                 \
}
D3D10_SRV_SET(VS, ST10_VS)
D3D10_SRV_SET(PS, ST10_PS)
D3D10_SRV_SET(GS, ST10_GS)
#undef D3D10_SRV_SET

// *SetSamplers
#define D3D10_SAMPLER_SET(STAGE_PREFIX)                                                     \
void STDMETHODCALLTYPE Device10Proxy::STAGE_PREFIX##SetSamplers(                            \
    UINT StartSlot, UINT NumSamplers, ID3D10SamplerState* const* ppSamplers)                \
{                                                                                           \
    m_real->STAGE_PREFIX##SetSamplers(StartSlot, NumSamplers, ppSamplers);                  \
    if (!m_presentHookActive) return;                                                       \
    std::vector<ComRefHolder10> refs;                                                       \
    refs.reserve(NumSamplers);                                                              \
    for (UINT i = 0; i < NumSamplers; ++i)                                                  \
        refs.emplace_back(ppSamplers ? ppSamplers[i] : nullptr);                            \
    m_frameCommands.emplace_back(                                                           \
        [this, StartSlot, NumSamplers, refs]() {                                            \
            ID3D10SamplerState* raw[kMaxSamplers] = { 0 };                                  \
            UINT cap = NumSamplers <= kMaxSamplers ? NumSamplers : kMaxSamplers;            \
            for (UINT i = 0; i < cap; ++i)                                                  \
                raw[i] = static_cast<ID3D10SamplerState*>(refs[i].p);                       \
            m_real->STAGE_PREFIX##SetSamplers(StartSlot, NumSamplers, raw);                 \
        });                                                                                 \
}
D3D10_SAMPLER_SET(VS)
D3D10_SAMPLER_SET(PS)
D3D10_SAMPLER_SET(GS)
#undef D3D10_SAMPLER_SET

// *SetShader (single shader, no class instances on D3D10)
// Option A (cycle 2): also snapshot m_boundInfo[stage] from the analyzer cache
// so EditRightCBs10 + primitive engine can see the bound shader's rightCBs +
// CRC without a per-draw lookup.
#define D3D10_SHADER_SET(STAGE_PREFIX, SHADER_TYPE, ST10_STAGE)                             \
void STDMETHODCALLTYPE Device10Proxy::STAGE_PREFIX##SetShader(SHADER_TYPE* pShader)         \
{                                                                                           \
    m_boundInfo[ST10_STAGE] = pShader ? LookupShaderProjection(pShader) : nullptr;          \
    m_real->STAGE_PREFIX##SetShader(pShader);                                               \
    if (!m_presentHookActive) return;                                                       \
    ComRefHolder10 shaderRef(pShader);                                                      \
    m_frameCommands.emplace_back(                                                           \
        [this, shaderRef]() {                                                               \
            m_real->STAGE_PREFIX##SetShader(static_cast<SHADER_TYPE*>(shaderRef.p));        \
        });                                                                                 \
}
D3D10_SHADER_SET(PS, ID3D10PixelShader, ST10_PS)
D3D10_SHADER_SET(GS, ID3D10GeometryShader, ST10_GS)
#undef D3D10_SHADER_SET

// Stage 4e DX10: VS variant that ALSO snapshots m_boundVS so Buffer10Proxy::
// Unmap can ask LookupShaderProjection(m_boundVS) what matrix registers
// live at which CB slot.
void STDMETHODCALLTYPE Device10Proxy::VSSetShader(ID3D10VertexShader* pShader)
{
    m_boundVS = pShader;
    m_boundInfo[ST10_VS] = pShader ? LookupShaderProjection(pShader) : nullptr;
    m_real->VSSetShader(pShader);
    if (!m_presentHookActive) return;
    ComRefHolder10 shaderRef(pShader);
    m_frameCommands.emplace_back(
        [this, shaderRef]() {
            m_real->VSSetShader(static_cast<ID3D10VertexShader*>(shaderRef.p));
        });
}

void STDMETHODCALLTYPE Device10Proxy::IASetPrimitiveTopology(D3D10_PRIMITIVE_TOPOLOGY Topology)
{
    m_real->IASetPrimitiveTopology(Topology);
    if (!m_presentHookActive) return;
    m_frameCommands.emplace_back([this, Topology]() { m_real->IASetPrimitiveTopology(Topology); });
}

void STDMETHODCALLTYPE Device10Proxy::RSSetState(ID3D10RasterizerState* pRasterizerState)
{
    m_real->RSSetState(pRasterizerState);
    if (!m_presentHookActive) return;
    ComRefHolder10 stateRef(pRasterizerState);
    m_frameCommands.emplace_back(
        [this, stateRef]() { m_real->RSSetState(static_cast<ID3D10RasterizerState*>(stateRef.p)); });
}

void STDMETHODCALLTYPE Device10Proxy::RSSetViewports(UINT NumViewports, const D3D10_VIEWPORT* pViewports)
{
    m_real->RSSetViewports(NumViewports, pViewports);
    if (!m_presentHookActive) return;
    std::vector<D3D10_VIEWPORT> vps;
    if (pViewports) vps.assign(pViewports, pViewports + NumViewports);
    m_frameCommands.emplace_back(
        [this, NumViewports, vps]() {
            m_real->RSSetViewports(NumViewports, vps.empty() ? nullptr : vps.data());
        });
}

void STDMETHODCALLTYPE Device10Proxy::RSSetScissorRects(UINT NumRects, const D3D10_RECT* pRects)
{
    m_real->RSSetScissorRects(NumRects, pRects);
    if (!m_presentHookActive) return;
    std::vector<D3D10_RECT> rects;
    if (pRects) rects.assign(pRects, pRects + NumRects);
    m_frameCommands.emplace_back(
        [this, NumRects, rects]() {
            m_real->RSSetScissorRects(NumRects, rects.empty() ? nullptr : rects.data());
        });
}

void STDMETHODCALLTYPE Device10Proxy::OMSetBlendState(
    ID3D10BlendState* pBlendState, const FLOAT BlendFactor[4], UINT SampleMask)
{
    m_real->OMSetBlendState(pBlendState, BlendFactor, SampleMask);
    if (!m_presentHookActive) return;
    ComRefHolder10 stateRef(pBlendState);
    FLOAT factor[4] = { 0, 0, 0, 0 };
    bool hasFactor = (BlendFactor != nullptr);
    if (hasFactor) { factor[0] = BlendFactor[0]; factor[1] = BlendFactor[1];
                     factor[2] = BlendFactor[2]; factor[3] = BlendFactor[3]; }
    m_frameCommands.emplace_back(
        [this, stateRef, factor, hasFactor, SampleMask]() {
            m_real->OMSetBlendState(static_cast<ID3D10BlendState*>(stateRef.p),
                                     hasFactor ? factor : nullptr, SampleMask);
        });
}

void STDMETHODCALLTYPE Device10Proxy::OMSetDepthStencilState(
    ID3D10DepthStencilState* pDepthStencilState, UINT StencilRef)
{
    m_real->OMSetDepthStencilState(pDepthStencilState, StencilRef);
    if (!m_presentHookActive) return;
    ComRefHolder10 stateRef(pDepthStencilState);
    m_frameCommands.emplace_back(
        [this, stateRef, StencilRef]() {
            m_real->OMSetDepthStencilState(static_cast<ID3D10DepthStencilState*>(stateRef.p), StencilRef);
        });
}

// Draws — pure POD captures.
// Draws — left eye runs with the game's CBs; right-eye replay closures also
// invoke ApplySnapshotRightCBs10 so the Option A per-draw sibling-bind path
// fires for CBs the analyzer flagged as rightCBs on the bound shaders.
// ShaderHunter integration (cycle 3) lives at the top of each: NoteDraw +
// HuntMaybeCaptureFingerprint10 record the per-draw CRC + fingerprint, and
// ShouldSkipDraw lets Hunter hide a specific CRC's draws to isolate it.
#define WIZ3D_DX10_HUNT_PROLOGUE()                                                  \
    const uint32_t _huntVS = m_boundInfo[ST10_VS] ? m_boundInfo[ST10_VS]->crc32 : 0;\
    const uint32_t _huntPS = m_boundInfo[ST10_PS] ? m_boundInfo[ST10_PS]->crc32 : 0;\
    wiz3d::ShaderHunter::NoteDraw(_huntVS, _huntPS);                                \
    HuntMaybeCaptureFingerprint10();                                                \
    if (wiz3d::ShaderHunter::ShouldSkipDraw(_huntVS, _huntPS)) return

// Right-eye draw closure template:
//   1. ApplySnapshotRightCBs10 -- per-draw CB sibling-bind
//   2. BindClipSpaceShift10 -- DXBC twin primitive dispatch
//   3. m_real->Draw*
//   4. UnbindClipSpaceShift10 -- restore saved VS + CB if twin was bound
#define WIZ3D_DX10_RIGHT_EYE_DRAW(REAL_CALL) \
    ApplySnapshotRightCBs10(snap); \
    const bool _twinBound = BindClipSpaceShift10(snap.vsInfo, snap.psInfo); \
    REAL_CALL; \
    if (_twinBound) UnbindClipSpaceShift10()

void STDMETHODCALLTYPE Device10Proxy::Draw(UINT VertexCount, UINT StartVertexLocation)
{
    WIZ3D_DX10_HUNT_PROLOGUE();
    m_real->Draw(VertexCount, StartVertexLocation);
    if (!m_presentHookActive) return;
    DrawSnapshot10 snap = CaptureDrawSnapshot10();
    m_frameCommands.emplace_back(
        [this, VertexCount, StartVertexLocation, snap]() {
            WIZ3D_DX10_RIGHT_EYE_DRAW(m_real->Draw(VertexCount, StartVertexLocation));
        });
}

void STDMETHODCALLTYPE Device10Proxy::DrawIndexed(UINT IndexCount, UINT StartIndexLocation, INT BaseVertexLocation)
{
    WIZ3D_DX10_HUNT_PROLOGUE();
    m_real->DrawIndexed(IndexCount, StartIndexLocation, BaseVertexLocation);
    if (!m_presentHookActive) return;
    DrawSnapshot10 snap = CaptureDrawSnapshot10();
    m_frameCommands.emplace_back(
        [this, IndexCount, StartIndexLocation, BaseVertexLocation, snap]() {
            WIZ3D_DX10_RIGHT_EYE_DRAW(m_real->DrawIndexed(IndexCount, StartIndexLocation, BaseVertexLocation));
        });
}

void STDMETHODCALLTYPE Device10Proxy::DrawInstanced(
    UINT VertexCountPerInstance, UINT InstanceCount,
    UINT StartVertexLocation, UINT StartInstanceLocation)
{
    WIZ3D_DX10_HUNT_PROLOGUE();
    m_real->DrawInstanced(VertexCountPerInstance, InstanceCount, StartVertexLocation, StartInstanceLocation);
    if (!m_presentHookActive) return;
    DrawSnapshot10 snap = CaptureDrawSnapshot10();
    m_frameCommands.emplace_back(
        [this, VertexCountPerInstance, InstanceCount, StartVertexLocation, StartInstanceLocation, snap]() {
            WIZ3D_DX10_RIGHT_EYE_DRAW(
                m_real->DrawInstanced(VertexCountPerInstance, InstanceCount, StartVertexLocation, StartInstanceLocation));
        });
}

void STDMETHODCALLTYPE Device10Proxy::DrawIndexedInstanced(
    UINT IndexCountPerInstance, UINT InstanceCount, UINT StartIndexLocation,
    INT BaseVertexLocation, UINT StartInstanceLocation)
{
    WIZ3D_DX10_HUNT_PROLOGUE();
    m_real->DrawIndexedInstanced(IndexCountPerInstance, InstanceCount, StartIndexLocation,
                                  BaseVertexLocation, StartInstanceLocation);
    if (!m_presentHookActive) return;
    DrawSnapshot10 snap = CaptureDrawSnapshot10();
    m_frameCommands.emplace_back(
        [this, IndexCountPerInstance, InstanceCount, StartIndexLocation,
         BaseVertexLocation, StartInstanceLocation, snap]() {
            WIZ3D_DX10_RIGHT_EYE_DRAW(
                m_real->DrawIndexedInstanced(IndexCountPerInstance, InstanceCount, StartIndexLocation,
                                              BaseVertexLocation, StartInstanceLocation));
        });
}

void STDMETHODCALLTYPE Device10Proxy::DrawAuto()
{
    WIZ3D_DX10_HUNT_PROLOGUE();
    m_real->DrawAuto();
    if (!m_presentHookActive) return;
    DrawSnapshot10 snap = CaptureDrawSnapshot10();
    m_frameCommands.emplace_back([this, snap]() {
        WIZ3D_DX10_RIGHT_EYE_DRAW(m_real->DrawAuto());
    });
}
#undef WIZ3D_DX10_HUNT_PROLOGUE
#undef WIZ3D_DX10_RIGHT_EYE_DRAW

// Stage 4e DX10: shader-create methods now run the DXBC analyzer + cache
// per-shader projection-matrix info. DX10 bytecode is SHDR (SM4) — the
// shared analyzer in ShaderAnalyzer11 already tries SHEX first then falls
// back to SHDR, so the same call works.
namespace {

// Cycle 2b: check whether the shader's CRC is primitive-tagged in the loaded
// profile (BaseProfile.xml / UserProfile.xml). If yes, keep the DXBC bytecode
// so Device10Proxy_RHW.cpp's twin compiler has something to work from at
// right-eye draw time.
static bool ShaderCRCHasPrimitive10(DWORD crc, bool isVS)
{
    const auto& table = isVS ? g_ProfileData.VSCRCData : g_ProfileData.PSCRCData;
    auto it = table.find(crc);
    if (it == table.end()) return false;
    const ShaderProfileData& p = it->second;
    if (p.m_bSkipStereo) return true;
    if (!std::isnan(p.m_FixedDepth)) return true;
    if (p.m_DepthFromCB_Slot >= 0) return true;
    if (!std::isnan(p.m_AnimDepth_Start)) return true;
    if (p.m_ShiftUV_OutputReg >= 0) return true;
    if (p.m_PerEye_Slot >= 0) return true;
    if (p.m_OverrideCB_Slot >= 0) return true;
    return false;
}

template<typename TShader>
HRESULT AnalyzeAndCreate10(const char* tag, Device10Proxy* self,
                           std::function<HRESULT(const void*, SIZE_T, TShader**)> createReal,
                           const void* pBytecode, SIZE_T byteLength, TShader** ppShader)
{
    HRESULT hr = createReal(pBytecode, byteLength, ppShader);
    if (FAILED(hr) || !ppShader || !*ppShader) return hr;
    ShaderAnalysis11Result info;
    const bool analyzed = AnalyzeShader11(pBytecode, byteLength, info);

    // Cycle 2b: retain bytecode for CRCs that have primitive-tagged profile
    // entries. The DXBC-twin compiler in Device10Proxy_RHW.cpp (BindFixedDepth10
    // etc.) feeds PatchShader + m_real->CreateVertexShader from this.
    const bool isVS = !strcmp(tag, "Vertex");
    const bool isPS = !strcmp(tag, "Pixel");
    const bool primitiveTagged = (isVS && ShaderCRCHasPrimitive10(info.crc32, true))
                              || (isPS && ShaderCRCHasPrimitive10(info.crc32, false));
    if (pBytecode && byteLength && primitiveTagged)
    {
        info.bytecode = std::make_shared<const std::vector<BYTE>>(
            (const BYTE*)pBytecode, (const BYTE*)pBytecode + byteLength);
        info.keepCode = true;
    }

    if (analyzed && !info.projection.matrixData.cb.empty())
    {
        DDILog("  Device10Proxy::Create%sShader: CRC=0x%08lX shader=%p matrices in %u CB(s)%s\n",
               tag, info.crc32, *ppShader,
               (unsigned)info.projection.matrixData.cb.size(),
               primitiveTagged ? " [primitive-tagged, keeping bytecode]" : "");
        self->StoreShaderProjection(*ppShader, info);
    }
    else if (primitiveTagged || info.crc32)
    {
        // Even without projection matrices, store if primitive-tagged so the
        // twin compiler can reach the bytecode.
        DDILog("  Device10Proxy::Create%sShader: CRC=0x%08lX shader=%p (parsed=%d)%s\n",
               tag, info.crc32, *ppShader, (int)info.parsed,
               primitiveTagged ? " [primitive-tagged, keeping bytecode]" : "");
        self->StoreShaderProjection(*ppShader, info);
    }
    else
    {
        DDILog("  Device10Proxy::Create%sShader: CRC=0x%08lX shader=%p (no projection, parsed=%d)\n",
               tag, info.crc32, *ppShader, (int)info.parsed);
    }
    return hr;
}

} // namespace

HRESULT STDMETHODCALLTYPE Device10Proxy::CreateVertexShader(
    const void* pShaderBytecode, SIZE_T BytecodeLength,
    ID3D10VertexShader** ppVertexShader)
{
    return AnalyzeAndCreate10<ID3D10VertexShader>("Vertex", this,
        [this](const void* b, SIZE_T n, ID3D10VertexShader** pp) {
            return m_real->CreateVertexShader(b, n, pp);
        },
        pShaderBytecode, BytecodeLength, ppVertexShader);
}

HRESULT STDMETHODCALLTYPE Device10Proxy::CreateGeometryShader(
    const void* pShaderBytecode, SIZE_T BytecodeLength,
    ID3D10GeometryShader** ppGeometryShader)
{
    return AnalyzeAndCreate10<ID3D10GeometryShader>("Geometry", this,
        [this](const void* b, SIZE_T n, ID3D10GeometryShader** pp) {
            return m_real->CreateGeometryShader(b, n, pp);
        },
        pShaderBytecode, BytecodeLength, ppGeometryShader);
}

HRESULT STDMETHODCALLTYPE Device10Proxy::CreateGeometryShaderWithStreamOutput(
    const void* pShaderBytecode, SIZE_T BytecodeLength,
    const D3D10_SO_DECLARATION_ENTRY* pSODeclaration, UINT NumEntries,
    UINT OutputStreamStride, ID3D10GeometryShader** ppGeometryShader)
{
    HRESULT hr = m_real->CreateGeometryShaderWithStreamOutput(
        pShaderBytecode, BytecodeLength, pSODeclaration, NumEntries,
        OutputStreamStride, ppGeometryShader);
    if (FAILED(hr) || !ppGeometryShader || !*ppGeometryShader) return hr;
    ShaderAnalysis11Result info;
    if (AnalyzeShader11(pShaderBytecode, BytecodeLength, info) && !info.projection.matrixData.cb.empty())
    {
        StoreShaderProjection(*ppGeometryShader, info);
    }
    return hr;
}

} // namespace wiz3d
