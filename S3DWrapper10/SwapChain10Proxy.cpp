/* wiz3D - IDXGISwapChain proxy implementation for DX10 */

#include "StdAfx.h"
#include "SwapChain10Proxy.h"
#include "Device10Proxy.h"
#include "Texture2D10Proxy.h"
#include "proxy_factory.h"
#include "AdapterFunctions.h"  // DDILog
#include "..\Shared\OutputModeNames.h"   // OUTMODE_* constants shared across wrappers
#include <d3dcompiler.h>

#pragma comment(lib, "dxguid.lib")
#pragma comment(lib, "d3dcompiler.lib")

namespace wiz3d
{

SwapChain10Proxy::SwapChain10Proxy(IDXGISwapChain* real, Device10Proxy* parent)
    : SwapChain10Proxy(real, nullptr, parent)
{
}

SwapChain10Proxy::SwapChain10Proxy(IDXGISwapChain* real, IDXGISwapChain1* real1, Device10Proxy* parent)
    : m_real(real)
    , m_real1(real1)
    , m_parent(parent)
    , m_refs(1)
    , m_leftBB(nullptr)
    , m_rightBB(nullptr)
    , m_wrappedBB(nullptr)
    , m_leftSRV(nullptr)
    , m_rightSRV(nullptr)
    , m_realBBRTV(nullptr)
    , m_bbWidth(0)
    , m_bbHeight(0)
    , m_bbFormat(DXGI_FORMAT_UNKNOWN)
    , m_compositeVS(nullptr)
    , m_compositePS(nullptr)
    , m_compositeSampler(nullptr)
    , m_compositeRaster(nullptr)
    , m_compositeBlend(nullptr)
    , m_compositeDepthStencil(nullptr)
{
    if (m_parent) m_parent->AddRef();
    DDILog("SwapChain10Proxy ctor: real=%p real1=%p parent=%p\n", real, real1, parent);
}

SwapChain10Proxy::~SwapChain10Proxy()
{
    ReleaseStereoBackBuffer();
    ReleaseComposite();
    if (m_real1)  { m_real1->Release();  m_real1  = nullptr; }
    if (m_real)   { m_real->Release();   m_real   = nullptr; }
    if (m_parent) { m_parent->Release(); m_parent = nullptr; }
}

ULONG STDMETHODCALLTYPE SwapChain10Proxy::Release()
{
    LONG r = InterlockedDecrement(&m_refs);
    if (r == 0) delete this;
    return (ULONG)r;
}

HRESULT STDMETHODCALLTYPE SwapChain10Proxy::QueryInterface(REFIID riid, void** ppvObj)
{
    if (!ppvObj) return E_POINTER;
    if (riid == IID_IUnknown          ||
        riid == IID_IDXGIObject       ||
        riid == IID_IDXGIDeviceSubObject ||
        riid == IID_IDXGISwapChain)
    {
        *ppvObj = static_cast<IDXGISwapChain*>(this);
        AddRef();
        return S_OK;
    }
    if (riid == IID_IDXGISwapChain1 && m_real1)
    {
        *ppvObj = static_cast<IDXGISwapChain1*>(this);
        AddRef();
        return S_OK;
    }
    if (riid == IID_wiz3D_SwapChain10Proxy)
    {
        *ppvObj = static_cast<IUnknown*>(static_cast<IDXGISwapChain*>(this));
        AddRef();
        return S_OK;
    }
    return m_real->QueryInterface(riid, ppvObj);
}

HRESULT STDMETHODCALLTYPE SwapChain10Proxy::GetDevice(REFIID riid, void** ppDevice)
{
    if (!ppDevice) return E_POINTER;
    if (riid == __uuidof(ID3D10Device) && m_parent)
        return m_parent->QueryInterface(riid, ppDevice);
    return m_real->GetDevice(riid, ppDevice);
}

// Identical HLSL to the DX11 composite — vs_4_0 / ps_4_0 targets work for
// both D3D10 and D3D11.
static const char* k_compositeShaderSrc10 = R"(
struct VSOut {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

VSOut VSMain(uint id : SV_VertexID) {
    VSOut o;
    o.uv  = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}

Texture2D    leftTex  : register(t0);
Texture2D    rightTex : register(t1);
SamplerState samp     : register(s0);

float4 PSMain(VSOut input) : SV_Target {
    float2 uv = input.uv;
    float  side = step(0.5, uv.x);
    float2 leftSample  = float2(uv.x * 2.0,         uv.y);
    float2 rightSample = float2((uv.x - 0.5) * 2.0, uv.y);
    float4 colL = leftTex.Sample (samp, leftSample);
    float4 colR = rightTex.Sample(samp, rightSample);
    return lerp(colL, colR, side);
}
)";

HRESULT SwapChain10Proxy::EnsureStereoBackBuffer()
{
    if (m_leftBB && m_rightBB) return S_OK;
    if (!m_real || !m_parent) return E_FAIL;

    DXGI_SWAP_CHAIN_DESC desc = {};
    HRESULT hr = m_real->GetDesc(&desc);
    if (FAILED(hr)) return hr;

    m_bbWidth  = desc.BufferDesc.Width;
    m_bbHeight = desc.BufferDesc.Height;
    m_bbFormat = desc.BufferDesc.Format;

    ID3D10Device* dev = m_parent->GetReal();
    if (!dev) return E_FAIL;

    D3D10_TEXTURE2D_DESC td = {};
    td.Width            = m_bbWidth;
    td.Height           = m_bbHeight;
    td.MipLevels        = 1;
    td.ArraySize        = 1;
    td.Format           = m_bbFormat;
    td.SampleDesc.Count = 1;
    td.Usage            = D3D10_USAGE_DEFAULT;
    td.BindFlags        = D3D10_BIND_RENDER_TARGET | D3D10_BIND_SHADER_RESOURCE;

    HRESULT hrL = dev->CreateTexture2D(&td, nullptr, &m_leftBB);
    HRESULT hrR = dev->CreateTexture2D(&td, nullptr, &m_rightBB);
    if (FAILED(hrL) || FAILED(hrR))
    {
        DDILog("SwapChain10Proxy::EnsureStereoBackBuffer: CreateTexture2D failed hrL=0x%08lX hrR=0x%08lX\n", hrL, hrR);
        if (m_leftBB)  { m_leftBB->Release();  m_leftBB  = nullptr; }
        if (m_rightBB) { m_rightBB->Release(); m_rightBB = nullptr; }
        return FAILED(hrL) ? hrL : hrR;
    }

    HRESULT hrSL = dev->CreateShaderResourceView(m_leftBB,  nullptr, &m_leftSRV);
    HRESULT hrSR = dev->CreateShaderResourceView(m_rightBB, nullptr, &m_rightSRV);
    if (FAILED(hrSL) || FAILED(hrSR))
    {
        DDILog("SwapChain10Proxy::EnsureStereoBackBuffer: CreateSRV failed hrSL=0x%08lX hrSR=0x%08lX\n", hrSL, hrSR);
        ReleaseStereoBackBuffer();
        return FAILED(hrSL) ? hrSL : hrSR;
    }

    // Same refcount discipline as DX11: Texture2D10Proxy takes ownership of
    // refs passed in, so we AddRef once more so SwapChain keeps an
    // independent ref via m_leftBB / m_rightBB.
    m_leftBB->AddRef();
    m_rightBB->AddRef();
    m_wrappedBB = new Texture2D10Proxy(m_leftBB, m_rightBB, m_parent);

    ID3D10Texture2D* realBBTex = nullptr;
    hr = m_real->GetBuffer(0, __uuidof(ID3D10Texture2D), reinterpret_cast<void**>(&realBBTex));
    if (FAILED(hr) || !realBBTex)
    {
        DDILog("SwapChain10Proxy::EnsureStereoBackBuffer: real GetBuffer failed 0x%08lX\n", hr);
        ReleaseStereoBackBuffer();
        return hr;
    }
    hr = dev->CreateRenderTargetView(realBBTex, nullptr, &m_realBBRTV);
    realBBTex->Release();
    if (FAILED(hr))
    {
        DDILog("SwapChain10Proxy::EnsureStereoBackBuffer: real BB RTV create failed 0x%08lX\n", hr);
        ReleaseStereoBackBuffer();
        return hr;
    }

    DDILog("SwapChain10Proxy::EnsureStereoBackBuffer: leftBB=%p rightBB=%p wrappedBB=%p (%ux%u fmt=%d)\n",
           m_leftBB, m_rightBB, m_wrappedBB, m_bbWidth, m_bbHeight, (int)m_bbFormat);
    return S_OK;
}

void SwapChain10Proxy::ReleaseStereoBackBuffer()
{
    if (m_realBBRTV) { m_realBBRTV->Release(); m_realBBRTV = nullptr; }
    if (m_leftSRV)   { m_leftSRV->Release();   m_leftSRV   = nullptr; }
    if (m_rightSRV)  { m_rightSRV->Release();  m_rightSRV  = nullptr; }
    if (m_wrappedBB) { m_wrappedBB->Release(); m_wrappedBB = nullptr; }
    if (m_leftBB)    { m_leftBB->Release();    m_leftBB    = nullptr; }
    if (m_rightBB)   { m_rightBB->Release();   m_rightBB   = nullptr; }
    m_bbWidth = m_bbHeight = 0;
    m_bbFormat = DXGI_FORMAT_UNKNOWN;
}

HRESULT SwapChain10Proxy::EnsureComposite()
{
    if (m_compositeVS && m_compositePS && m_compositeSampler) return S_OK;
    if (!m_parent) return E_FAIL;
    ID3D10Device* dev = m_parent->GetReal();
    if (!dev) return E_FAIL;

    ID3DBlob* vsBlob = nullptr; ID3DBlob* psBlob = nullptr; ID3DBlob* err = nullptr;
    UINT flags = D3DCOMPILE_OPTIMIZATION_LEVEL3;

    HRESULT hrVS = D3DCompile(
        k_compositeShaderSrc10, strlen(k_compositeShaderSrc10),
        "wiz3D_composite_dx10", nullptr, nullptr,
        "VSMain", "vs_4_0", flags, 0, &vsBlob, &err);
    if (FAILED(hrVS))
    {
        DDILog("SwapChain10Proxy::EnsureComposite: VS compile failed 0x%08lX: %s\n",
               hrVS, err ? (const char*)err->GetBufferPointer() : "(no log)");
        if (err) err->Release();
        return hrVS;
    }
    if (err) { err->Release(); err = nullptr; }

    HRESULT hrPS = D3DCompile(
        k_compositeShaderSrc10, strlen(k_compositeShaderSrc10),
        "wiz3D_composite_dx10", nullptr, nullptr,
        "PSMain", "ps_4_0", flags, 0, &psBlob, &err);
    if (FAILED(hrPS))
    {
        DDILog("SwapChain10Proxy::EnsureComposite: PS compile failed 0x%08lX: %s\n",
               hrPS, err ? (const char*)err->GetBufferPointer() : "(no log)");
        if (err) err->Release();
        vsBlob->Release();
        return hrPS;
    }
    if (err) { err->Release(); err = nullptr; }

    HRESULT hr = dev->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
                                          &m_compositeVS);
    vsBlob->Release();
    if (FAILED(hr)) { psBlob->Release(); return hr; }

    hr = dev->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(),
                                 &m_compositePS);
    psBlob->Release();
    if (FAILED(hr)) { ReleaseComposite(); return hr; }

    D3D10_SAMPLER_DESC sd = {};
    sd.Filter         = D3D10_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU       = D3D10_TEXTURE_ADDRESS_CLAMP;
    sd.AddressV       = D3D10_TEXTURE_ADDRESS_CLAMP;
    sd.AddressW       = D3D10_TEXTURE_ADDRESS_CLAMP;
    sd.ComparisonFunc = D3D10_COMPARISON_NEVER;
    sd.MinLOD         = 0.f;
    sd.MaxLOD         = D3D10_FLOAT32_MAX;
    hr = dev->CreateSamplerState(&sd, &m_compositeSampler);
    if (FAILED(hr)) { ReleaseComposite(); return hr; }

    D3D10_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D10_FILL_SOLID;
    rd.CullMode = D3D10_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    hr = dev->CreateRasterizerState(&rd, &m_compositeRaster);
    if (FAILED(hr)) { ReleaseComposite(); return hr; }

    D3D10_BLEND_DESC bd = {};
    bd.BlendEnable[0]           = FALSE;
    bd.RenderTargetWriteMask[0] = D3D10_COLOR_WRITE_ENABLE_ALL;
    hr = dev->CreateBlendState(&bd, &m_compositeBlend);
    if (FAILED(hr)) { ReleaseComposite(); return hr; }

    D3D10_DEPTH_STENCIL_DESC dsd = {};
    dsd.DepthEnable    = FALSE;
    dsd.StencilEnable  = FALSE;
    dsd.DepthWriteMask = D3D10_DEPTH_WRITE_MASK_ZERO;
    hr = dev->CreateDepthStencilState(&dsd, &m_compositeDepthStencil);
    if (FAILED(hr)) { ReleaseComposite(); return hr; }

    DDILog("SwapChain10Proxy::EnsureComposite: VS=%p PS=%p sampler=%p\n",
           m_compositeVS, m_compositePS, m_compositeSampler);
    return S_OK;
}

void SwapChain10Proxy::ReleaseComposite()
{
    if (m_compositeDepthStencil) { m_compositeDepthStencil->Release(); m_compositeDepthStencil = nullptr; }
    if (m_compositeBlend)        { m_compositeBlend->Release();        m_compositeBlend        = nullptr; }
    if (m_compositeRaster)       { m_compositeRaster->Release();       m_compositeRaster       = nullptr; }
    if (m_compositeSampler)      { m_compositeSampler->Release();      m_compositeSampler      = nullptr; }
    if (m_compositePS)           { m_compositePS->Release();           m_compositePS           = nullptr; }
    if (m_compositeVS)           { m_compositeVS->Release();           m_compositeVS           = nullptr; }
}

void SwapChain10Proxy::DoComposite()
{
    if (!m_parent || !m_realBBRTV || !m_leftSRV || !m_rightSRV) return;
    if (FAILED(EnsureComposite())) return;

    ID3D10Device* dev = m_parent->GetReal();
    if (!dev) return;

    ID3D10ShaderResourceView* nullSRVs[2] = { nullptr, nullptr };
    dev->PSSetShaderResources(0, 2, nullSRVs);

    dev->OMSetRenderTargets(1, &m_realBBRTV, nullptr);

    // D3D10_VIEWPORT uses INT/UINT for pos/size (D3D11 uses FLOAT).
    D3D10_VIEWPORT vp = {};
    vp.TopLeftX = 0;
    vp.TopLeftY = 0;
    vp.Width    = m_bbWidth;
    vp.Height   = m_bbHeight;
    vp.MinDepth = 0.f;
    vp.MaxDepth = 1.f;
    dev->RSSetViewports(1, &vp);
    dev->RSSetState(m_compositeRaster);

    const FLOAT blendFactor[4] = { 1.f, 1.f, 1.f, 1.f };
    dev->OMSetBlendState(m_compositeBlend, blendFactor, 0xFFFFFFFF);
    dev->OMSetDepthStencilState(m_compositeDepthStencil, 0);

    ID3D10ShaderResourceView* srvs[2] = { m_leftSRV, m_rightSRV };
    dev->PSSetShaderResources(0, 2, srvs);
    dev->PSSetSamplers(0, 1, &m_compositeSampler);

    dev->VSSetShader(m_compositeVS);
    dev->PSSetShader(m_compositePS);
    dev->GSSetShader(nullptr);

    dev->IASetInputLayout(nullptr);
    dev->IASetPrimitiveTopology(D3D10_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    dev->IASetVertexBuffers(0, 0, nullptr, nullptr, nullptr);
    dev->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);

    dev->Draw(3, 0);

    dev->PSSetShaderResources(0, 2, nullSRVs);
}

HRESULT STDMETHODCALLTYPE SwapChain10Proxy::GetBuffer(UINT Buffer, REFIID riid, void** ppSurface)
{
    if (Buffer != 0 || !ppSurface)
        return m_real->GetBuffer(Buffer, riid, ppSurface);

    if (FAILED(EnsureStereoBackBuffer()))
        return m_real->GetBuffer(Buffer, riid, ppSurface);

    if (!m_wrappedBB)
        return m_real->GetBuffer(Buffer, riid, ppSurface);

    HRESULT hr = m_wrappedBB->QueryInterface(riid, ppSurface);
    if (SUCCEEDED(hr))
    {
        DDILog("SwapChain10Proxy::GetBuffer(0): returned wrapped BB=%p\n", m_wrappedBB);
        return hr;
    }
    return m_real->GetBuffer(Buffer, riid, ppSurface);
}

HRESULT STDMETHODCALLTYPE SwapChain10Proxy::ResizeBuffers(
    UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags)
{
    DDILog("SwapChain10Proxy::ResizeBuffers: count=%u %ux%u fmt=%d flags=0x%X\n",
           BufferCount, Width, Height, (int)NewFormat, SwapChainFlags);
    // Clear recording — BB-derived handles in the closures would dangle.
    if (m_parent) m_parent->ClearFrameCommands();
    ReleaseStereoBackBuffer();
    HRESULT hr = m_real->ResizeBuffers(BufferCount, Width, Height, NewFormat, SwapChainFlags);
    DDILog("SwapChain10Proxy::ResizeBuffers -> hr=0x%08lX\n", hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE SwapChain10Proxy::SetFullscreenState(BOOL Fullscreen, IDXGIOutput* pTarget)
{
    DDILog("SwapChain10Proxy::SetFullscreenState: fullscreen=%d target=%p\n",
           (int)Fullscreen, pTarget);
    if (m_parent) m_parent->ClearFrameCommands();
    HRESULT hr = m_real->SetFullscreenState(Fullscreen, pTarget);
    DDILog("SwapChain10Proxy::SetFullscreenState -> hr=0x%08lX\n", hr);
    return hr;
}

void SwapChain10Proxy::OnPresentBoundaryPre()
{
    if (!gInfo.UseCOMWrapReplay) return;
    if (!m_parent) return;
    if (m_parent->IsPresentHookActive())
        m_parent->ReplayFrameCommands(Device10Proxy::Eye::Right);

    // 0.5.0: output-mode dispatch at Present boundary. DX10 currently has
    // only the SBS composite implemented -- SR Weave, 3DVisionGlasses, and
    // Katanga are DX11 Stereo-Injector features whose DX10 ports are
    // pending (each needs a DX10->DX11 shared-handle bridge since the SR
    // weaver and NV3D-Lib both take ID3D11Device inputs). This dispatch
    // layer is the hook point where those ports will land as drop-in
    // methods, and the diagnostic logging below answers the recurring
    // "DX10 is still the same" test observation -- it reports which output
    // mode was selected and what the composite inputs looked like at the
    // first few Present calls, so a user can confirm whether DX10 is even
    // reaching this path on a given game.
    static int   s_logN  = 0;
    static DWORD s_lastMode = 0xFFFFFFFF;
    const DWORD mode = gInfo.OutputMode;
    if (s_logN < 5 || mode != s_lastMode)
    {
        if (s_logN < 5) ++s_logN;
        s_lastMode = mode;
        DDILog("SwapChain10Proxy::OnPresentBoundaryPre: OutputMode=%u (%s) "
               "leftSRV=%p rightSRV=%p realBBRTV=%p BB=%ux%u\n",
               mode,
               (mode == wiz3d::OUTMODE_HALF_SIDE_BY_SIDE)    ? "HalfSBS" :
               (mode == wiz3d::OUTMODE_FULL_SIDE_BY_SIDE)    ? "FullSBS" :
               (mode == wiz3d::OUTMODE_HALF_TOP_AND_BOTTOM)  ? "HalfTAB" :
               (mode == wiz3d::OUTMODE_FULL_TOP_AND_BOTTOM)  ? "FullTAB" :
               (mode == wiz3d::OUTMODE_ANAGLYPH)             ? "Anaglyph" :
               (mode == wiz3d::OUTMODE_ROW_INTERLEAVED)      ? "RowIL" :
               (mode == wiz3d::OUTMODE_COLUMN_INTERLEAVED)   ? "ColumnIL" :
               (mode == wiz3d::OUTMODE_CHECKERBOARD)         ? "Checker" :
               (mode == wiz3d::OUTMODE_SR_WEAVE)             ? "SRWeave" :
               (mode == wiz3d::OUTMODE_STEREO_MIRROR)        ? "Mirror" :
               (mode == wiz3d::OUTMODE_3DVISION_GLASSES)     ? "3DVisionGlasses" :
               (mode == wiz3d::OUTMODE_KATANGA)              ? "Katanga" :
               "Unknown",
               (void*)m_leftSRV, (void*)m_rightSRV, (void*)m_realBBRTV,
               m_bbWidth, m_bbHeight);

        if (mode == wiz3d::OUTMODE_SR_WEAVE)
            DDILog("  -> SR Weave DX10 output not yet implemented; falling back to SBS composite.\n");
        else if (mode == wiz3d::OUTMODE_3DVISION_GLASSES)
            DDILog("  -> 3DVisionGlasses DX10 output not yet implemented; falling back to SBS composite.\n");
        else if (mode == wiz3d::OUTMODE_KATANGA)
            DDILog("  -> Katanga DX10 output not yet implemented; falling back to SBS composite.\n");
    }

    // Only path currently implemented. Future output ports (SR Weave,
    // NV3D-Lib shutter, Katanga) will add their own invocations here and
    // skip DoComposite when they succeed, mirroring SwapChain11Proxy's
    // RunKatanga / RunNV3DGlasses / RunSRWeave dispatch (SwapChain11Proxy.cpp:791-793).
    DoComposite();
}

void SwapChain10Proxy::OnPresentBoundaryPost()
{
    if (!m_parent) return;
    m_parent->ClearFrameCommands();
    m_parent->SetPresentHookActive(true);
}

HRESULT STDMETHODCALLTYPE SwapChain10Proxy::Present(UINT SyncInterval, UINT Flags)
{
    OnPresentBoundaryPre();
    HRESULT hr = m_real->Present(SyncInterval, Flags);
    OnPresentBoundaryPost();
    return hr;
}

HRESULT STDMETHODCALLTYPE SwapChain10Proxy::Present1(UINT SyncInterval, UINT PresentFlags,
    const DXGI_PRESENT_PARAMETERS* pPresentParameters)
{
    if (!m_real1) return E_NOINTERFACE;
    OnPresentBoundaryPre();
    HRESULT hr = m_real1->Present1(SyncInterval, PresentFlags, pPresentParameters);
    OnPresentBoundaryPost();
    return hr;
}

} // namespace wiz3d
