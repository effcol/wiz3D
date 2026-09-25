#include "Context11Proxy.h"
#include "Device11Proxy.h"
#include "SwapChainProxy.h"
#include "magic_header_capture.h"
#include "eye_state.h"
#include "log.h"

#pragma comment(lib, "dxguid.lib")

namespace NvDirectMode
{

// Per-frame draw-call + command-list counter storage. Declared in
// Context11Proxy.h, bumped by CountDraw / CountCmdList on interception,
// drained by SwapChainProxy at Present time.
volatile LONG g_drawsThisFrame   = 0;
volatile LONG g_cmdListsThisFrame = 0;

LONG GetAndResetDrawsThisFrame()    { return InterlockedExchange(&g_drawsThisFrame,   0); }
LONG GetAndResetCmdListsThisFrame() { return InterlockedExchange(&g_cmdListsThisFrame, 0); }

Context11Proxy::Context11Proxy(ID3D11DeviceContext* real, Device11Proxy* parent)
    : m_real(real)
    , m_parent(parent)
    , m_refs(1)
    , m_currentBBBound(false)
{
}

Context11Proxy::~Context11Proxy() = default;

HRESULT STDMETHODCALLTYPE Context11Proxy::QueryInterface(REFIID riid, void** ppvObj)
{
    if (!ppvObj) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_ID3D11DeviceChild ||
        riid == IID_ID3D11DeviceContext)
    {
        *ppvObj = static_cast<ID3D11DeviceContext*>(this);
        AddRef();
        return S_OK;
    }
    // Context1/Context2/Context3+ — return E_NOINTERFACE so the game falls
    // back to the base ID3D11DeviceContext (which we wrap). Passing the
    // real Context1+ pointer through opens a COM-identity escape: the game
    // would call OMSetRenderTargets on the unwrapped pointer, bypassing our
    // active-eye viewport clamp. Tomb Raider 2013 was leaking 4 unwrapped
    // Context1+ refs per device and crashing in ucrtbase free() shortly
    // after first frame's OMSet — likely the game's Context1+ refcount
    // diverging from our Context11Proxy's tracking.
    NVDM_TRACE_FIRST_N(8,
        "  Context11Proxy::QI(unknown/higher IID) -> E_NOINTERFACE\n");
    *ppvObj = nullptr;
    return E_NOINTERFACE;
}

// Stage 3 v2: shadow is at 1x logical size, so we no longer transform the
// game's viewport. Game's natural rendering goes to the full shadow, which
// is exactly the size game expects — no clamp, no re-clamp, no boundary
// artifacts. We still note when the BB-RTV is bound (m_currentBBBound is
// useful diagnostic state and a future hook for per-eye CAPTURE in stage
// 4 — when SetActiveEye changes between renders, capture shadow into a
// per-eye texture so we can composite SBS for non-stereo display preview).

void STDMETHODCALLTYPE Context11Proxy::OMSetRenderTargets(
    UINT NumViews, ID3D11RenderTargetView* const* ppRenderTargetViews,
    ID3D11DepthStencilView* pDepthStencilView)
{
    m_real->OMSetRenderTargets(NumViews, ppRenderTargetViews, pDepthStencilView);
    if (m_parent && NumViews > 0 && ppRenderTargetViews && ppRenderTargetViews[0])
    {
        bool bbBound = m_parent->IsBackBufferRTV(ppRenderTargetViews[0]);
        m_currentBBBound = bbBound;
        if (bbBound)
            NVDM_TRACE_FIRST_N(8, "  Context11Proxy::OMSet BB-RTV bound rtv=%p (no clamp — shadow is 1x)\n",
                               ppRenderTargetViews[0]);
    }
    else
    {
        m_currentBBBound = false;
    }
}

void STDMETHODCALLTYPE Context11Proxy::OMSetRenderTargetsAndUnorderedAccessViews(
    UINT NumRTVs, ID3D11RenderTargetView* const* ppRenderTargetViews,
    ID3D11DepthStencilView* pDepthStencilView,
    UINT UAVStartSlot, UINT NumUAVs,
    ID3D11UnorderedAccessView* const* ppUnorderedAccessViews,
    const UINT* pUAVInitialCounts)
{
    m_real->OMSetRenderTargetsAndUnorderedAccessViews(
        NumRTVs, ppRenderTargetViews, pDepthStencilView,
        UAVStartSlot, NumUAVs, ppUnorderedAccessViews, pUAVInitialCounts);
    if (NumRTVs != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL && m_parent &&
        NumRTVs > 0 && ppRenderTargetViews && ppRenderTargetViews[0])
    {
        m_currentBBBound = m_parent->IsBackBufferRTV(ppRenderTargetViews[0]);
    }
    else if (NumRTVs != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL)
    {
        m_currentBBBound = false;
    }
    // Else NumRTVs == sentinel: leave m_currentBBBound unchanged.
}

// Stage 3 v2: passthrough — no per-eye clamp needed when shadow is 1x.
void STDMETHODCALLTYPE Context11Proxy::RSSetViewports(UINT NumViewports, const D3D11_VIEWPORT* pViewports)
{
    m_real->RSSetViewports(NumViewports, pViewports);
}

void STDMETHODCALLTYPE Context11Proxy::GetDevice(ID3D11Device** ppDevice)
{
    // COM identity: GetDevice on the wrapped context should return the
    // wrapped device, not the unwrapped real one — otherwise a game that
    // round-trips through GetDevice ends up holding two different "device"
    // pointers and our wrapper gets bypassed for resource creation.
    if (!ppDevice) return;
    if (m_parent)
    {
        *ppDevice = reinterpret_cast<ID3D11Device*>(m_parent);
        m_parent->AddRef();
        return;
    }
    m_real->GetDevice(ppDevice);
}

// ---------------------------------------------------------------------------
// Magic-header SBS capture intercepts. Same pattern as d3d10 — detect
// (2W × H+1) Texture2D sources tagged with NVSTEREO_IMAGE_SIGNATURE being
// copied to the swap-chain backbuffer, split them via
// SwapChainProxy::CaptureMagicHeaderSBS, and skip the real blit.
// ---------------------------------------------------------------------------
namespace
{
    constexpr size_t kMaxKnownStereoTex = 16;
    ID3D11Resource* g_knownStereoTex[kMaxKnownStereoTex] = {};
    UINT            g_knownStereoCount = 0;
    bool            g_knownStereoSwap  = false;

    bool IsKnown(ID3D11Resource* p)
    {
        for (UINT i = 0; i < g_knownStereoCount; ++i)
            if (g_knownStereoTex[i] == p) return true;
        return false;
    }
    void MarkKnown(ID3D11Resource* p)
    {
        if (IsKnown(p)) return;
        if (g_knownStereoCount < kMaxKnownStereoTex)
            g_knownStereoTex[g_knownStereoCount++] = p;
    }

    bool TryMagicHeaderCapture(ID3D11Device* dev, ID3D11DeviceContext* ctx,
                               Device11Proxy* parent,
                               ID3D11Resource* dst, ID3D11Resource* src)
    {
        if (!parent->IsBackBufferResource(dst)) return false;
        if (!src) return false;

        SwapChainProxy* sc = SwapChainProxy::GetPrimary();
        if (!sc) return false;

        UINT eyeW = 0, eyeH = 0;
        bool swap = false;
        if (IsKnown(src))
        {
            ID3D11Texture2D* t = nullptr;
            if (FAILED(src->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&t)) || !t)
                return false;
            D3D11_TEXTURE2D_DESC d = {};
            t->GetDesc(&d);
            t->Release();
            if (d.Width < 4 || (d.Width & 1) || d.Height < 2) return false;
            eyeW = d.Width / 2;
            eyeH = d.Height - 1;
            swap = g_knownStereoSwap;
        }
        else
        {
            MagicHeader::DetectResult r = MagicHeader::DetectStereoMagic(dev, ctx, src);
            if (!r.hasMagic) return false;
            MarkKnown(src);
            g_knownStereoSwap = r.swapEyes;
            eyeW = r.eyeWidth;
            eyeH = r.eyeHeight;
            swap = r.swapEyes;
            LOG_VERBOSE("  d3d11 CopyResource: magic-header SBS detected (src=%p, %ux%u eye)\n",
                        (void*)src, eyeW, eyeH);
        }

        sc->CaptureMagicHeaderSBS(ctx, src, eyeW, eyeH, swap);
        return true;
    }
}

void STDMETHODCALLTYPE Context11Proxy::CopyResource(
    ID3D11Resource* pDstResource, ID3D11Resource* pSrcResource)
{
    if (m_parent)
    {
        ID3D11Device* dev = m_parent->GetReal();
        if (dev && TryMagicHeaderCapture(dev, m_real, m_parent, pDstResource, pSrcResource))
            return;
    }
    m_real->CopyResource(pDstResource, pSrcResource);
}

void STDMETHODCALLTYPE Context11Proxy::CopySubresourceRegion(
    ID3D11Resource* pDstResource, UINT DstSubresource, UINT DstX, UINT DstY,
    UINT DstZ, ID3D11Resource* pSrcResource, UINT SrcSubresource,
    const D3D11_BOX* pSrcBox)
{
    if (m_parent && DstSubresource == 0 && DstX == 0 && DstY == 0 && DstZ == 0 &&
        SrcSubresource == 0 && pSrcBox == nullptr)
    {
        ID3D11Device* dev = m_parent->GetReal();
        if (dev && TryMagicHeaderCapture(dev, m_real, m_parent, pDstResource, pSrcResource))
            return;
    }
    m_real->CopySubresourceRegion(pDstResource, DstSubresource, DstX, DstY, DstZ,
                                  pSrcResource, SrcSubresource, pSrcBox);
}

} // namespace NvDirectMode
