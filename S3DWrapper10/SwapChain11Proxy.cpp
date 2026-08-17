/* wiz3D - IDXGISwapChain proxy implementation (Option B Stage 4b.2) */

#include "StdAfx.h"
#include "SwapChain11Proxy.h"
#include "Device11Proxy.h"
#include "Context11Proxy.h"
#include "Texture2D11Proxy.h"
#include "proxy_factory.h"     // IID_wiz3D_SwapChain11Proxy (declared in 4b.2 update)
#include "AdapterFunctions.h"  // DDILog
#include <d3dcompiler.h>

#pragma comment(lib, "dxguid.lib")
#pragma comment(lib, "d3dcompiler.lib")

namespace wiz3d
{

SwapChain11Proxy::SwapChain11Proxy(IDXGISwapChain* real, IDXGISwapChain1* real1, Device11Proxy* parent)
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
    // Stage 4b.8 fix: AddRef the parent device. Without this, if the game
    // releases its Device11Proxy reference before the swap chain proxy,
    // m_parent dangles and the next Present hook reads garbage (BioShock
    // crashed at OnPresentBoundaryPre line 92 with ECX=1 — Device11Proxy
    // memory had been freed and m_ctxProxy was reading reused heap data).
    if (m_parent) m_parent->AddRef();
    DDILog("SwapChain11Proxy ctor: real=%p real1=%p parent=%p\n", real, real1, parent);
}

SwapChain11Proxy::~SwapChain11Proxy()
{
    ReleaseStereoBackBuffer();
    ReleaseComposite();
    if (m_real1)  { m_real1->Release();  m_real1  = nullptr; }
    if (m_real)   { m_real->Release();   m_real   = nullptr; }
    if (m_parent) { m_parent->Release(); m_parent = nullptr; }
}

ULONG STDMETHODCALLTYPE SwapChain11Proxy::Release()
{
    LONG r = InterlockedDecrement(&m_refs);
    if (r == 0) delete this;
    return (ULONG)r;
}

HRESULT STDMETHODCALLTYPE SwapChain11Proxy::QueryInterface(REFIID riid, void** ppvObj)
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
    // Stage 4b.2: private identity IID — used by the dxgi.dll-side factory
    // hook (4b.3) to detect "is this swap chain one of ours?" cross-DLL.
    if (riid == IID_wiz3D_SwapChain11Proxy)
    {
        *ppvObj = static_cast<IUnknown*>(static_cast<IDXGISwapChain*>(this));
        AddRef();
        return S_OK;
    }
    // IDXGISwapChain2/3/4: pass through unwrapped for now. Future iteration
    // can extend if needed by Win11-era games. Log so we can spot games that
    // depend on the higher-version SC interfaces (same diagnostic pattern as
    // Device11Proxy::QI bypass-risk lines).
    HRESULT hr = m_real->QueryInterface(riid, ppvObj);
    static int s_logged = 0;
    if (s_logged < 8)
    {
        char iidName[64];
        FormatGUID(riid, iidName, sizeof(iidName));
        DDILog("  SwapChain11Proxy::QI(%s) hr=0x%08lX -- bypass risk\n", iidName, hr);
        ++s_logged;
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE SwapChain11Proxy::GetDevice(REFIID riid, void** ppDevice)
{
    // COM identity: route ID3D11Device QI through our wrapped device, so a
    // game that fetches the device via sc->GetDevice keeps using our proxy.
    if (!ppDevice) return E_POINTER;
    if (riid == __uuidof(ID3D11Device) && m_parent)
    {
        return m_parent->QueryInterface(riid, ppDevice);
    }
    return m_real->GetDevice(riid, ppDevice);
}

// Stage 4d: HLSL source for the SBS composite. VS emits a fullscreen triangle
// covering NDC [-1,1]^2; PS samples the left-eye texture into the left half of
// the BB and the right-eye texture into the right half (each eye horizontally
// scaled by 2x to fit). Two separate sampler calls keep the branch out of the
// flow — both sides issue one read regardless of texcoord.
static const char* k_compositeShaderSrc = R"(
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
    float  side = step(0.5, uv.x);   // 0 = left half, 1 = right half
    float2 leftSample  = float2(uv.x * 2.0,            uv.y);
    float2 rightSample = float2((uv.x - 0.5) * 2.0,    uv.y);
    float4 colL = leftTex.Sample (samp, leftSample);
    float4 colR = rightTex.Sample(samp, rightSample);
    return lerp(colL, colR, side);
}
)";

HRESULT SwapChain11Proxy::EnsureStereoBackBuffer()
{
    // Stage 4d: allocate the wiz3D LEFT and RIGHT BB sibling textures the
    // first time GetBuffer(0) is called. Done lazily (not in ctor) so we
    // pick up the real swap chain's current desc — for one-call factory
    // creation the desc is known but for ResizeBuffers we have to wait
    // for the next GetBuffer call after the resize.
    if (m_leftBB && m_rightBB) return S_OK;
    if (!m_real || !m_parent) return E_FAIL;

    DXGI_SWAP_CHAIN_DESC desc = {};
    HRESULT hr = m_real->GetDesc(&desc);
    if (FAILED(hr)) return hr;

    m_bbWidth  = desc.BufferDesc.Width;
    m_bbHeight = desc.BufferDesc.Height;
    m_bbFormat = desc.BufferDesc.Format;

    ID3D11Device* dev = m_parent->GetReal();
    if (!dev) return E_FAIL;

    D3D11_TEXTURE2D_DESC td = {};
    td.Width            = m_bbWidth;
    td.Height           = m_bbHeight;
    td.MipLevels        = 1;
    td.ArraySize        = 1;
    td.Format           = m_bbFormat;
    td.SampleDesc.Count = 1;
    td.Usage            = D3D11_USAGE_DEFAULT;
    // RT so the game (left) + replay (right) can draw into them, SRV so
    // the composite pass can sample them. UAV not needed.
    td.BindFlags        = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    HRESULT hrL = dev->CreateTexture2D(&td, nullptr, &m_leftBB);
    HRESULT hrR = dev->CreateTexture2D(&td, nullptr, &m_rightBB);
    if (FAILED(hrL) || FAILED(hrR))
    {
        DDILog("SwapChain11Proxy::EnsureStereoBackBuffer: CreateTexture2D failed hrL=0x%08lX hrR=0x%08lX\n", hrL, hrR);
        if (m_leftBB)  { m_leftBB->Release();  m_leftBB  = nullptr; }
        if (m_rightBB) { m_rightBB->Release(); m_rightBB = nullptr; }
        return FAILED(hrL) ? hrL : hrR;
    }

    HRESULT hrSL = dev->CreateShaderResourceView(m_leftBB,  nullptr, &m_leftSRV);
    HRESULT hrSR = dev->CreateShaderResourceView(m_rightBB, nullptr, &m_rightSRV);
    if (FAILED(hrSL) || FAILED(hrSR))
    {
        DDILog("SwapChain11Proxy::EnsureStereoBackBuffer: CreateSRV failed hrSL=0x%08lX hrSR=0x%08lX\n", hrSL, hrSR);
        ReleaseStereoBackBuffer();
        return FAILED(hrSL) ? hrSL : hrSR;
    }

    // Wrap the pair as a Texture2D11Proxy so the rest of the wiz3D wrap
    // machinery (CreateRenderTargetView unwrap + DoOMSet eye routing) sees
    // it as a normal stereo-doubled texture. m_wrappedBB is a strong ref
    // we hold for the lifetime of the swap chain; GetBuffer just AddRefs.
    //
    // Texture2D11Proxy's ctor takes ownership of the refs passed in (its
    // dtor calls Release on m_realLeft/m_realRight) — so before handing
    // ours over we AddRef once more, so SwapChain still holds an
    // independent ref via m_leftBB / m_rightBB. Without this AddRef both
    // SwapChain and the proxy think they own the same single ref, and
    // ReleaseStereoBackBuffer's wrappedBB->Release() + leftBB->Release()
    // chain ends up double-releasing the texture (BioShock + DA2 crashed
    // on the second GetBuffer call when EnsureStereoBackBuffer
    // re-entered after a ResizeBuffers — the cached m_leftBB pointer
    // pointed at freed memory and the d3d11 runtime crashed inside
    // CreateShaderResourceView / OMSetRenderTargets).
    m_leftBB->AddRef();
    m_rightBB->AddRef();
    m_wrappedBB = new Texture2D11Proxy(m_leftBB, m_rightBB, m_parent);

    // RTV on the REAL swap-chain back buffer — used as the composite
    // destination. Acquired now so we don't pay the cost mid-frame.
    ID3D11Texture2D* realBBTex = nullptr;
    hr = m_real->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&realBBTex));
    if (FAILED(hr) || !realBBTex)
    {
        DDILog("SwapChain11Proxy::EnsureStereoBackBuffer: real GetBuffer failed 0x%08lX\n", hr);
        ReleaseStereoBackBuffer();
        return hr;
    }
    hr = dev->CreateRenderTargetView(realBBTex, nullptr, &m_realBBRTV);
    realBBTex->Release();
    if (FAILED(hr))
    {
        DDILog("SwapChain11Proxy::EnsureStereoBackBuffer: real BB RTV create failed 0x%08lX\n", hr);
        ReleaseStereoBackBuffer();
        return hr;
    }

    DDILog("SwapChain11Proxy::EnsureStereoBackBuffer: leftBB=%p rightBB=%p wrappedBB=%p (%ux%u fmt=%d)\n",
           m_leftBB, m_rightBB, m_wrappedBB, m_bbWidth, m_bbHeight, (int)m_bbFormat);
    return S_OK;
}

void SwapChain11Proxy::ReleaseStereoBackBuffer()
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

HRESULT SwapChain11Proxy::EnsureComposite()
{
    // Stage 4d: lazily compile the SBS composite shaders + pipeline state
    // on first Present where the composite actually runs. D3DCompile is a
    // one-time cost; the shader is independent of BB format/size so we
    // don't have to re-compile across ResizeBuffers.
    if (m_compositeVS && m_compositePS && m_compositeSampler) return S_OK;
    if (!m_parent) return E_FAIL;
    ID3D11Device* dev = m_parent->GetReal();
    if (!dev) return E_FAIL;

    ID3DBlob* vsBlob = nullptr; ID3DBlob* psBlob = nullptr; ID3DBlob* err = nullptr;
    UINT flags = D3DCOMPILE_OPTIMIZATION_LEVEL3;

    HRESULT hrVS = D3DCompile(
        k_compositeShaderSrc, strlen(k_compositeShaderSrc),
        "wiz3D_composite", nullptr, nullptr,
        "VSMain", "vs_4_0", flags, 0, &vsBlob, &err);
    if (FAILED(hrVS))
    {
        DDILog("SwapChain11Proxy::EnsureComposite: VS compile failed 0x%08lX: %s\n",
               hrVS, err ? (const char*)err->GetBufferPointer() : "(no log)");
        if (err) err->Release();
        return hrVS;
    }
    if (err) { err->Release(); err = nullptr; }

    HRESULT hrPS = D3DCompile(
        k_compositeShaderSrc, strlen(k_compositeShaderSrc),
        "wiz3D_composite", nullptr, nullptr,
        "PSMain", "ps_4_0", flags, 0, &psBlob, &err);
    if (FAILED(hrPS))
    {
        DDILog("SwapChain11Proxy::EnsureComposite: PS compile failed 0x%08lX: %s\n",
               hrPS, err ? (const char*)err->GetBufferPointer() : "(no log)");
        if (err) err->Release();
        vsBlob->Release();
        return hrPS;
    }
    if (err) { err->Release(); err = nullptr; }

    HRESULT hr = dev->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
                                          nullptr, &m_compositeVS);
    vsBlob->Release();
    if (FAILED(hr)) { psBlob->Release(); return hr; }

    hr = dev->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(),
                                 nullptr, &m_compositePS);
    psBlob->Release();
    if (FAILED(hr)) { ReleaseComposite(); return hr; }

    D3D11_SAMPLER_DESC sd = {};
    sd.Filter         = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU       = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressV       = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressW       = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MinLOD         = 0.f;
    sd.MaxLOD         = D3D11_FLOAT32_MAX;
    hr = dev->CreateSamplerState(&sd, &m_compositeSampler);
    if (FAILED(hr)) { ReleaseComposite(); return hr; }

    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;          // fullscreen tri, no cull
    rd.DepthClipEnable = TRUE;
    hr = dev->CreateRasterizerState(&rd, &m_compositeRaster);
    if (FAILED(hr)) { ReleaseComposite(); return hr; }

    D3D11_BLEND_DESC bd = {};
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    hr = dev->CreateBlendState(&bd, &m_compositeBlend);
    if (FAILED(hr)) { ReleaseComposite(); return hr; }

    D3D11_DEPTH_STENCIL_DESC dsd = {};
    dsd.DepthEnable    = FALSE;
    dsd.StencilEnable  = FALSE;
    dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    hr = dev->CreateDepthStencilState(&dsd, &m_compositeDepthStencil);
    if (FAILED(hr)) { ReleaseComposite(); return hr; }

    DDILog("SwapChain11Proxy::EnsureComposite: VS=%p PS=%p sampler=%p\n",
           m_compositeVS, m_compositePS, m_compositeSampler);
    return S_OK;
}

void SwapChain11Proxy::ReleaseComposite()
{
    if (m_compositeDepthStencil) { m_compositeDepthStencil->Release(); m_compositeDepthStencil = nullptr; }
    if (m_compositeBlend)        { m_compositeBlend->Release();        m_compositeBlend        = nullptr; }
    if (m_compositeRaster)       { m_compositeRaster->Release();       m_compositeRaster       = nullptr; }
    if (m_compositeSampler)      { m_compositeSampler->Release();      m_compositeSampler      = nullptr; }
    if (m_compositePS)           { m_compositePS->Release();           m_compositePS           = nullptr; }
    if (m_compositeVS)           { m_compositeVS->Release();           m_compositeVS           = nullptr; }
}

void SwapChain11Proxy::DoComposite()
{
    // Stage 4d: bind real BB as RT, LEFT + RIGHT wiz3D BB siblings as
    // PS SRVs, fullscreen triangle, draw. State the game has set is
    // clobbered; the recorded command stream replays per-eye state for
    // the next frame's draws so anything important gets re-issued.
    if (!m_parent || !m_realBBRTV || !m_leftSRV || !m_rightSRV) return;
    if (FAILED(EnsureComposite())) return;

    ID3D11DeviceContext* ctx = nullptr;
    m_parent->GetReal()->GetImmediateContext(&ctx);
    if (!ctx) return;

    // Unbind any SRVs that might collide with t0/t1 (the game's PSSet
    // probably already has shader inputs there). Cheap defensive clear.
    ID3D11ShaderResourceView* nullSRVs[2] = { nullptr, nullptr };
    ctx->PSSetShaderResources(0, 2, nullSRVs);

    // Composite pass setup.
    ctx->OMSetRenderTargets(1, &m_realBBRTV, nullptr);
    D3D11_VIEWPORT vp = {};
    vp.Width    = (float)m_bbWidth;
    vp.Height   = (float)m_bbHeight;
    vp.MinDepth = 0.f;
    vp.MaxDepth = 1.f;
    ctx->RSSetViewports(1, &vp);
    ctx->RSSetState(m_compositeRaster);

    const FLOAT blendFactor[4] = { 1.f, 1.f, 1.f, 1.f };
    ctx->OMSetBlendState(m_compositeBlend, blendFactor, 0xFFFFFFFF);
    ctx->OMSetDepthStencilState(m_compositeDepthStencil, 0);

    ID3D11ShaderResourceView* srvs[2] = { m_leftSRV, m_rightSRV };
    ctx->PSSetShaderResources(0, 2, srvs);
    ctx->PSSetSamplers(0, 1, &m_compositeSampler);

    ctx->VSSetShader(m_compositeVS, nullptr, 0);
    ctx->PSSetShader(m_compositePS, nullptr, 0);
    ctx->GSSetShader(nullptr, nullptr, 0);
    ctx->HSSetShader(nullptr, nullptr, 0);
    ctx->DSSetShader(nullptr, nullptr, 0);

    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->IASetVertexBuffers(0, 0, nullptr, nullptr, nullptr);
    ctx->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);

    ctx->Draw(3, 0);

    // Unbind our SRVs so the game's next OMSetRenderTargets binding the
    // wrapped BB-RTV doesn't trip a D3D11 read/write hazard warning.
    ctx->PSSetShaderResources(0, 2, nullSRVs);

    ctx->Release();
}

HRESULT STDMETHODCALLTYPE SwapChain11Proxy::GetBuffer(UINT Buffer, REFIID riid, void** ppSurface)
{
    // Stage 4d: index-0 buffer = our wrapped stereo BB. Other indices fall
    // through to the real swap chain (multi-buffer / shared-surface use is
    // rare for the games we wrap; first-pass leaves them unwrapped).
    if (Buffer != 0 || !ppSurface)
        return m_real->GetBuffer(Buffer, riid, ppSurface);

    if (FAILED(EnsureStereoBackBuffer()))
        return m_real->GetBuffer(Buffer, riid, ppSurface);

    if (!m_wrappedBB)
        return m_real->GetBuffer(Buffer, riid, ppSurface);

    // Honour the requested interface. Texture2D11Proxy::QueryInterface
    // covers the common IIDs (IUnknown, Resource, Texture2D); for anything
    // outside that surface area fall back to the real BB so the game's
    // sub-interface query doesn't get an E_NOINTERFACE here.
    HRESULT hr = m_wrappedBB->QueryInterface(riid, ppSurface);
    if (SUCCEEDED(hr))
    {
        // Register whatever interface pointer the game actually got: QI may
        // hand back a different address than m_wrappedBB, and CreateRTV
        // compares against the pointer the game passes back in.
        if (m_parent && *ppSurface)
            m_parent->RegisterBackBufferTexture(*ppSurface);
        DDILog("SwapChain11Proxy::GetBuffer(0): returned wrapped BB=%p (as %p) registered on dev=%p\n",
               m_wrappedBB, *ppSurface, m_parent);
        return hr;
    }
    return m_real->GetBuffer(Buffer, riid, ppSurface);
}

static int s_FramePresentCounter = 0;

void SwapChain11Proxy::OnPresentBoundaryPre()
{
    if (FrameTraceActive())
        FrameTrace("\n=== Present #%d PRE ===\n", s_FramePresentCounter);
    // Stage 4b.8: PRE-PRESENT sweep. The game has already issued frame N's
    // state setters and draws — those ran for the left eye via the direct
    // passthrough in each proxy method. When UseCOMWrapReplay=1 we re-issue
    // the recorded command stream with m_activeEye=Right so OMSet/Clear/
    // Update/Copy/Resolve helpers bind right-eye real handles. After this,
    // the left-eye and right-eye textures both hold their per-eye images,
    // ready for the 4d SBS composite to flatten into the swap-chain BB.
    //
    // Gated behind UseCOMWrapReplay (default off) because:
    //   - Recorded handles can dangle across ResizeBuffers / SetFullscreen
    //     (Max Payne 3 fullscreen toggle crashes inside d3d11.dll during
    //     replay because BB-derived RTVs have been invalidated).
    //   - Right-eye textures aren't displayed yet (no 4c stereo math, no
    //     4d composite) so the replay has no visible benefit, only risk.
    if (!gInfo.UseCOMWrapReplay) return;
    if (!m_parent) return;
    Context11Proxy* ctx = m_parent->GetContextProxy();
    if (!ctx) return;
    if (ctx->IsPresentHookActive())
        ctx->ReplayFrameCommands(Context11Proxy::Eye::Right);

    if (FrameTraceActive())
        FrameTrace("--- composite start ---\n");

    // Stage 4d: composite the left + right BB siblings into the real BB.
    // Coupled to UseCOMWrapReplay because without the replay there's no
    // right-eye content to composite. Skipped silently if the wrapped BB
    // wasn't allocated (game took some non-standard GetBuffer path).
    DoComposite();
    if (FrameTraceActive())
        FrameTrace("--- composite end ---\n");
}

void SwapChain11Proxy::OnPresentBoundaryPost()
{
    if (FrameTraceActive())
        FrameTrace("=== Present #%d POST ===\n", s_FramePresentCounter++);
    FrameTraceTickFrame();
    // Stage 4b.8: POST-PRESENT housekeeping. Real Present has flipped the
    // BB; clear the recording vector and arm it for frame N+1. We arm
    // unconditionally (not just on the first call) so a context that's
    // been ClearState()'d still gets recording.
    if (!m_parent) return;
    Context11Proxy* ctx = m_parent->GetContextProxy();
    if (!ctx) return;
    // Diagnostic: must run BEFORE ClearFrameCommands() so the summary sees the
    // recording for the frame that just presented, not an empty vector.
    ctx->LogAndResetFrameDrawStats();
    ctx->ClearFrameCommands();
    ctx->SetPresentHookActive(true);
}

HRESULT STDMETHODCALLTYPE SwapChain11Proxy::ResizeBuffers(
    UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags)
{
    // Stage 4b.8 fix: ResizeBuffers invalidates every BB-derived RTV/DSV
    // the game (and our recorded closures) might hold. The recorded
    // command stream becomes unsafe to replay — Max Payne 3's fullscreen
    // toggle hit this and crashed inside d3d11.dll when replay tried to
    // re-bind dead handles. Clear the recording before forwarding.
    if (m_parent)
    {
        if (Context11Proxy* ctx = m_parent->GetContextProxy())
            ctx->ClearFrameCommands();
    }
    // Stage 4d: tear down the wiz3D stereo BB siblings + the RTV we hold
    // on the real BB. DXGI requires every reference to back-buffer-derived
    // resources be released before ResizeBuffers, including ours.
    // Composite shaders/sampler are format-independent so they stay alive.
    // The wrapped Texture2D11Proxy from GetBuffer is dropped — game will
    // call GetBuffer again post-resize and we'll allocate new siblings
    // matching the new BB dims/format.
    DDILog("SwapChain11Proxy::ResizeBuffers: count=%u %ux%u fmt=%d flags=0x%X\n",
           BufferCount, Width, Height, (int)NewFormat, SwapChainFlags);
    ReleaseStereoBackBuffer();
    HRESULT hr = m_real->ResizeBuffers(BufferCount, Width, Height, NewFormat, SwapChainFlags);
    DDILog("SwapChain11Proxy::ResizeBuffers -> hr=0x%08lX\n", hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE SwapChain11Proxy::SetFullscreenState(BOOL Fullscreen, IDXGIOutput* pTarget)
{
    // Stage 4b.8 fix: same logic as ResizeBuffers — fullscreen transitions
    // can implicitly resize the swap chain and invalidate BB-derived
    // handles. Clear the recording so replay can't re-issue stale state.
    DDILog("SwapChain11Proxy::SetFullscreenState: fullscreen=%d target=%p\n",
           (int)Fullscreen, pTarget);
    if (m_parent)
    {
        if (Context11Proxy* ctx = m_parent->GetContextProxy())
            ctx->ClearFrameCommands();
    }
    HRESULT hr = m_real->SetFullscreenState(Fullscreen, pTarget);
    DDILog("SwapChain11Proxy::SetFullscreenState -> hr=0x%08lX\n", hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE SwapChain11Proxy::Present(UINT SyncInterval, UINT Flags)
{
    OnPresentBoundaryPre();
    HRESULT hr = m_real->Present(SyncInterval, Flags);
    OnPresentBoundaryPost();
    return hr;
}

HRESULT STDMETHODCALLTYPE SwapChain11Proxy::Present1(
    UINT SyncInterval, UINT PresentFlags, const DXGI_PRESENT_PARAMETERS* pPresentParameters)
{
    OnPresentBoundaryPre();
    HRESULT hr = m_real1 ? m_real1->Present1(SyncInterval, PresentFlags, pPresentParameters)
                          : E_NOINTERFACE;
    OnPresentBoundaryPost();
    return hr;
}

} // namespace wiz3d
