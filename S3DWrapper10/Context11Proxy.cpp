/* wiz3D - ID3D11DeviceContext proxy implementation (Option B Stage 2)
 *
 * Pure passthrough port of NvDirectMode/d3d11/Context11Proxy. The stage-3 BB
 * tracking and stage-4 magic-header capture were stripped for the MVP — the
 * job here is to prove COM identity + refcounting are right, not to do any
 * stereo work yet. OMSet/RSSetViewports/CopyResource/CopySubresourceRegion
 * are forwarded unchanged; per-eye behaviour will be re-added in Stage 4.
 */

#include "StdAfx.h"
#include "Context11Proxy.h"
#include "Device11Proxy.h"
#include "Texture2D11Proxy.h"
#include "RTV11Proxy.h"
#include "DSV11Proxy.h"
#include "Buffer11Proxy.h"
#include "SRV11Proxy.h"
#include "UAV11Proxy.h"
#include "Texture1D11Proxy.h"
#include "Texture3D11Proxy.h"
#include "proxy_factory.h"     // TryUnwrap* helpers
#include "AdapterFunctions.h"  // DDILog
#include "..\S3DAPI\ShaderProfileData.h"  // g_ProfileData matrix declarations
#include <mutex>

namespace {
// Live-context registry. A context whose device has no wrapped swap chain
// never runs LogAndResetFrameDrawStats, so its counters just accumulate —
// which is exactly what makes it visible here.
std::mutex& CtxLock() { static std::mutex m; return m; }
std::vector<wiz3d::Context11Proxy*>& CtxList() { static std::vector<wiz3d::Context11Proxy*> v; return v; }
} // namespace

// Slot-count maxima now live on Context11Proxy (see the header) so the
// eye-tracking arrays and the unwrap temporaries stay the same size.
using wiz3d::Context11Proxy;
static constexpr UINT kMaxSRVs       = Context11Proxy::kMaxSRVs;
static constexpr UINT kMaxSamplers   = Context11Proxy::kMaxSamplers;
static constexpr UINT kMaxCBs        = Context11Proxy::kMaxCBs;
static constexpr UINT kMaxRTVs       = Context11Proxy::kMaxRTVs;
static constexpr UINT kMaxUAVs       = Context11Proxy::kMaxUAVs;
static constexpr UINT kMaxVBs        = Context11Proxy::kMaxVBs;
static constexpr UINT kMaxSOBuffers  = Context11Proxy::kMaxSOBuffers;
static constexpr UINT kMaxClassInst  = Context11Proxy::kMaxClassInst;

// Stage 3c.1: lightweight inline unwrap for ID3D11Buffer*. Used at every
// method boundary that hands a buffer to the real D3D11 runtime — passing
// our Buffer11Proxy directly would crash the runtime because it doesn't
// understand our vtable layout past ID3D11Buffer methods. Returns the raw
// buffer (or the original pointer if not ours, including nullptr).
static inline ID3D11Buffer* UnwrapBuf(ID3D11Buffer* p)
{
    if (!p) return nullptr;
    if (auto* bp = wiz3d::TryUnwrapBuffer(static_cast<ID3D11Resource*>(p)))
        return bp->GetReal();
    return p;
}

// Stage 3c.1: unwrap ID3D11Resource* for the eye-aware Do* helpers. Tries
// Texture2D11Proxy first (which has eye-stereo siblings) then Buffer11Proxy
// (no stereo doubling, single real). 3c.2 adds Texture1D/Texture3D — both
// passthrough (no stereo) but still need unwrap so the real runtime gets
// the real pointer.
static inline ID3D11Resource* UnwrapResourceForEye(ID3D11Resource* p, bool pickRight)
{
    if (!p) return nullptr;
    if (auto* tex = wiz3d::TryUnwrapTexture2D(p))
    {
        ID3D11Resource* right = tex->GetRealRight();
        return (pickRight && right) ? right : static_cast<ID3D11Resource*>(tex->GetReal());
    }
    if (auto* tex1 = wiz3d::TryUnwrapTexture1D(p))
        return static_cast<ID3D11Resource*>(tex1->GetReal());
    if (auto* tex3 = wiz3d::TryUnwrapTexture3D(p))
        return static_cast<ID3D11Resource*>(tex3->GetReal());
    if (auto* buf = wiz3d::TryUnwrapBuffer(p))
        return static_cast<ID3D11Resource*>(buf->GetReal());
    return p;
}

// Stage 3c.2: eye-aware unwrap helpers for SRV/UAV. Right-eye sibling is
// optional — falls back to left when null or when picking left eye.
static inline ID3D11ShaderResourceView* UnwrapSRVForEye(ID3D11ShaderResourceView* p, bool pickRight)
{
    if (!p) return nullptr;
    if (auto* sp = wiz3d::TryUnwrapSRV(p))
    {
        ID3D11ShaderResourceView* right = sp->GetRealRight();
        return (pickRight && right) ? right : sp->GetReal();
    }
    return p;
}

static inline ID3D11UnorderedAccessView* UnwrapUAVForEye(ID3D11UnorderedAccessView* p, bool pickRight)
{
    if (!p) return nullptr;
    if (auto* up = wiz3d::TryUnwrapUAV(p))
    {
        ID3D11UnorderedAccessView* right = up->GetRealRight();
        return (pickRight && right) ? right : up->GetReal();
    }
    return p;
}

#pragma comment(lib, "dxguid.lib")

namespace wiz3d
{

Context11Proxy::Context11Proxy(ID3D11DeviceContext* real, Device11Proxy* parent)
    : m_real(real)
    , m_real1(nullptr)
    , m_real2(nullptr)
    , m_real3(nullptr)
    , m_parent(parent)
    , m_refs(1)
    , m_currentBBBound(false)
    , m_activeEye(Eye::Left)
    , m_presentHookActive(false)
    , m_boundVS(nullptr)
    , m_boundPS(nullptr)
{
    for (UINT i = 0; i < kMaxVSCBSlots; ++i) m_boundVSCBs[i] = nullptr;
    for (UINT i = 0; i < kMaxPSCBSlots; ++i) m_boundPSCBs[i] = nullptr;
    ResetEyeTracking();
    if (m_real)
    {
        if (FAILED(m_real->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&m_real1)))) m_real1 = nullptr;
        if (FAILED(m_real->QueryInterface(__uuidof(ID3D11DeviceContext2), reinterpret_cast<void**>(&m_real2)))) m_real2 = nullptr;
        if (FAILED(m_real->QueryInterface(__uuidof(ID3D11DeviceContext3), reinterpret_cast<void**>(&m_real3)))) m_real3 = nullptr;
    }
    {
        std::lock_guard<std::mutex> g(CtxLock());
        CtxList().push_back(this);
    }
    DDILog("Context11Proxy ctor: this=%p real=%p parent=%p (live contexts=%u)\n",
           this, real, parent, (unsigned)CtxList().size());
}

Context11Proxy::~Context11Proxy()
{
    // Closures hold no AddRef'd state in 4b.1 (Draw/DrawIndexed capture only
    // POD args), so a plain clear is safe. Later stages that record state-
    // setting calls with captured COM pointers will release them in
    // ClearFrameCommands; the dtor will route there.
    ClearFrameCommands();
    ReleaseStereoShiftCBs();
    {
        std::lock_guard<std::mutex> g(CtxLock());
        auto& v = CtxList();
        for (size_t i = 0; i < v.size(); ++i)
            if (v[i] == this) { v.erase(v.begin() + i); break; }
    }
    if (m_real3) { m_real3->Release(); m_real3 = nullptr; }
    if (m_real2) { m_real2->Release(); m_real2 = nullptr; }
    if (m_real1) { m_real1->Release(); m_real1 = nullptr; }
}

void Context11Proxy::ClearFrameCommands()
{
    m_frameCommands.clear();
}

// Diagnostic (Aug 2026) — see the header comment on LogAndResetFrameDrawStats.
// Called once per Present from SwapChain11Proxy::OnPresentBoundaryPost, BEFORE
// ClearFrameCommands() wipes the recording, so `recorded` reflects the frame
// that just completed.
//
// Deliberately NOT routed through FrameTrace: that budget is capped at
// VerboseFrameTrace frames and auto-disables, whereas the whole point here is
// to watch the ratio hold (or not) across a long session. DDILog + a modest
// interval keeps the log small.
static float EyeShiftB(float eyeShift);   // defined with the CB-patch helpers below

// The modified shader computes x += sep * (w - conv), so we only need those two
// values. Rebuilt when separation changes, which is rare — binding is otherwise
// just a VSSetConstantBuffers on the slot ModifyShader reserved.
bool Context11Proxy::EnsureStereoShiftCBs()
{
    // NDC shift at distance is sep itself; projection scale is already in w.
    const float sep  = wiz3D_GetEffectiveEyeShift();
    // conv=0 makes this a constant NDC shift, matching the CB-patch path. Real
    // convergence needs the CB path to learn it too or the two eyes disagree.
    const float conv = 0.f;

    if (m_stereoCBLeft && m_stereoCBRight && sep == m_stereoCBSep && conv == m_stereoCBConv)
        return true;

    ID3D11Device* dev = m_parent ? m_parent->GetReal() : nullptr;
    if (!dev) return false;

    // Two registers: ModifyShader declares cb[2] and may reference the second for
    // the ZNear variant, so allocate both even though we only fill the first.
    // Left is unshifted to match the CB-patch path; mixing models desyncs eyes.
    const float dataL[8] = { 0.f, conv, 0.f, 0.f,  0.f, 0.f, 0.f, 0.f };
    const float dataR[8] = { sep, conv, 0.f, 0.f,  0.f, 0.f, 0.f, 0.f };

    ReleaseStereoShiftCBs();
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = sizeof(dataL);
    bd.Usage     = D3D11_USAGE_IMMUTABLE;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA srdL = { dataL, 0, 0 };
    D3D11_SUBRESOURCE_DATA srdR = { dataR, 0, 0 };
    if (FAILED(dev->CreateBuffer(&bd, &srdL, &m_stereoCBLeft)) ||
        FAILED(dev->CreateBuffer(&bd, &srdR, &m_stereoCBRight)))
    {
        ReleaseStereoShiftCBs();
        return false;
    }
    m_stereoCBSep  = sep;
    m_stereoCBConv = conv;
    DDILog("  StereoShiftCB: sep=%.6f conv=%.4f\n", sep, conv);
    return true;
}

void Context11Proxy::BindStereoShiftCB(bool right)
{
    if (!m_modVSShader || !EnsureStereoShiftCBs()) return;
    ID3D11Buffer* cb = right ? m_stereoCBRight : m_stereoCBLeft;
    m_real->VSSetConstantBuffers(m_modVSCBIndex, 1, &cb);
}

void Context11Proxy::ReleaseStereoShiftCBs()
{
    if (m_stereoCBLeft)  { m_stereoCBLeft->Release();  m_stereoCBLeft  = nullptr; }
    if (m_stereoCBRight) { m_stereoCBRight->Release(); m_stereoCBRight = nullptr; }
}

// Sampling budget for matrix dumps, refilled at each frame report so the sample
// tracks the current scene instead of only the first frames after launch.
static int s_matDumpRejected = 0;
static int s_matDumpShifted  = 0;

// Camera projection _11, cached from the last clean view-projection seen.
static float s_projXScale = 0.f;

// Spread of |w basis| across shifted matrices: 1.0 everywhere means every
// targeted matrix is a clean VP, anything else means scaled WVPs are in play.
static float s_wnMin = 1e30f;
static float s_wnMax = 0.f;

std::string Context11Proxy::DescribeAllContexts()
{
    std::lock_guard<std::mutex> g(CtxLock());
    std::string s;
    char buf[128];
    for (size_t i = 0; i < CtxList().size(); ++i)
    {
        Context11Proxy* c = CtxList()[i];
        sprintf_s(buf, "[ctx=%p dev=%p draws=%u disp=%u]",
                  c, c->m_parent, c->m_drawsThisFrame, c->m_dispatchesThisFrame);
        s += buf;
    }
    return s;
}

void Context11Proxy::LogAndResetFrameDrawStats()
{
    static unsigned s_frame = 0;
    ++s_frame;
    // Publish before any reset so the trace gate sees this frame's real count.
    if ((int)m_drawsThisFrame > g_FrameTraceLastFrameDraws)
        g_FrameTraceLastFrameDraws = (int)m_drawsThisFrame;

    // Every 60th frame (~1s at 60fps), plus the first few so a short capture
    // still shows something.
    const bool report = (s_frame <= 3) || (s_frame % 60 == 0);
    if (report)
    {
        // Only contexts belonging to a device with a wrapped swap chain reach
        // this function. Report every live context so work on a device that
        // never presents through us is still visible.
        DDILog("  [frame %u] contexts: %s\n", s_frame, DescribeAllContexts().c_str());
        if (gInfo.ModifyShadersDX11 && m_parent) m_parent->LogShaderModStats();
        DDILog("  [frame %u] immediate-ctx=%p: draws=%u dispatches=%u cmdLists=%u"
               " recorded=%zu | CB patches: targeted=%u blind=%u skipped=%u"
               " | matrices: shifted=%u guardRejected=%u"
               " | dynBuf: replayed=%u skipped=%u"
               " | dup: duplicated=%u mono=%u uavSkip=%u monoDSV=%u noRightRTV=%u"
               " depthOnly=%u noRightTarget=%u dispDup=%u"
               " (DuplicateDraws=%d DisableBlindCBScan=%d ortho=%d shadow=%d)%s\n",
               s_frame, this, m_drawsThisFrame, m_dispatchesThisFrame,
               m_cmdListsThisFrame, m_frameCommands.size(),
               m_cbTargetedThisFrame, m_cbBlindThisFrame, m_cbSkippedThisFrame,
               m_cbMatShiftedThisFrame, m_cbMatRejectedThisFrame,
               m_dynBufReplaysThisFrame, m_dynBufSkippedThisFrame,
               m_drawsDuplicatedThisFrame, m_drawsMonoThisFrame,
               m_drawsUavSkippedThisFrame, m_drawsMonoDsvThisFrame,
               m_drawsNoRightRTVThisFrame,
               m_drawsDepthOnlyThisFrame, m_drawsNoRightTargetThisFrame,
               m_dispatchesDuplicatedThisFrame,
               (int)gInfo.DuplicateDraws,
               (int)gInfo.DisableBlindCBScan,
               (int)!gInfo.SkipCheckOrthoMatrix, (int)gInfo.CheckShadowMatrix,
               (m_cmdListsThisFrame > 0)
                   ? "  <-- DEFERRED CONTEXT WORK: right eye cannot be correct via replay"
                   : "");
    }
    else
    {
        // Reset only when we report. A device with more than one swap chain
        // (GTA V has a 2x2 dummy alongside the real one) calls this several
        // times per game frame, and resetting every call meant the sampled
        // report almost always landed just after a wipe: 33725 of 33844
        // samples read zero while real frames were pushing 4700 draws.
        return;
    }

    DDILog("  [frame %u] basis |w|: min=%.4f max=%.4f projXScale=%.4f\n",
           s_frame, s_wnMin, s_wnMax, s_projXScale);
    s_wnMin = 1e30f;
    s_wnMax = 0.f;

    // Refill the matrix dump budget so each report is followed by a fresh sample.
    s_matDumpRejected = 6;
    s_matDumpShifted  = 3;

    m_drawsThisFrame          = 0;
    m_dispatchesThisFrame     = 0;
    m_cmdListsThisFrame       = 0;
    m_cbTargetedThisFrame     = 0;
    m_cbBlindThisFrame        = 0;
    m_cbSkippedThisFrame      = 0;
    m_cbMatShiftedThisFrame   = 0;
    m_cbMatRejectedThisFrame  = 0;
    m_dynBufReplaysThisFrame  = 0;
    m_dynBufSkippedThisFrame  = 0;
    m_drawsDuplicatedThisFrame = 0;
    m_drawsMonoThisFrame       = 0;
    m_drawsUavSkippedThisFrame = 0;
    m_drawsMonoDsvThisFrame    = 0;
    m_drawsNoRightRTVThisFrame = 0;
    m_drawsDepthOnlyThisFrame     = 0;
    m_drawsNoRightTargetThisFrame = 0;
    m_dispatchesDuplicatedThisFrame = 0;
}

void Context11Proxy::ReplayFrameCommands(Eye eye)
{
    // Snapshot + flip the active eye for the replay pass. Each recorded
    // closure re-enters our proxy methods, so OMSet/etc. pick the
    // eye-appropriate real handle via m_activeEye automatically.
    Eye saved = m_activeEye;
    m_activeEye = eye;
    if (FrameTraceActive())
        FrameTrace("--- replay begin (eye=%c, %zu commands) ---\n",
                   eye == Eye::Right ? 'R' : 'L', m_frameCommands.size());
    for (auto& fn : m_frameCommands)
        fn();
    if (FrameTraceActive())
        FrameTrace("--- replay end (eye=%c) ---\n",
                   eye == Eye::Right ? 'R' : 'L');
    m_activeEye = saved;
}

void Context11Proxy::SetPresentHookActive(bool active)
{
    // Draw duplication issues both eyes live, so recording would be overhead
    // and replaying it would double every draw a second time.
    m_presentHookActive = active && !gInfo.DuplicateDraws;
}

void Context11Proxy::ResetEyeTracking()
{
    memset(m_srvSlots, 0, sizeof(m_srvSlots));
    memset(m_cbSlots,  0, sizeof(m_cbSlots));
    memset(m_rtvLeft,  0, sizeof(m_rtvLeft));
    memset(m_rtvRight, 0, sizeof(m_rtvRight));
    m_numRTVs     = 0;
    m_dsvLeft     = nullptr;
    m_dsvRight    = nullptr;
    m_omAnyStereo = false;
    m_omHasUAVs   = false;
    m_omMonoDSV   = false;
}

void Context11Proxy::TrackSRVs(StageIdx stage, UINT StartSlot, UINT NumViews,
                               ID3D11ShaderResourceView* const* ppSRVs)
{
    if (!gInfo.DuplicateDraws || stage >= ST_COUNT) return;
    SRVEyeSlots& s = m_srvSlots[stage];
    for (UINT i = 0; i < NumViews; ++i)
    {
        UINT slot = StartSlot + i;
        if (slot >= kMaxSRVs) break;
        ID3D11ShaderResourceView* p = ppSRVs ? ppSRVs[i] : nullptr;
        s.left[slot]  = UnwrapSRVForEye(p, false);
        s.right[slot] = UnwrapSRVForEye(p, true);
        if (slot + 1 > s.high) s.high = slot + 1;
    }
    s.anyStereo = false;
    for (UINT i = 0; i < s.high; ++i)
        if (s.left[i] != s.right[i]) { s.anyStereo = true; break; }
}

void Context11Proxy::TrackCBs(StageIdx stage, UINT StartSlot, UINT NumBuffers,
                              ID3D11Buffer* const* ppCBs)
{
    if (!gInfo.DuplicateDraws || stage >= ST_COUNT) return;
    CBEyeSlots& s = m_cbSlots[stage];
    for (UINT i = 0; i < NumBuffers; ++i)
    {
        UINT slot = StartSlot + i;
        if (slot >= kMaxCBs) break;
        ID3D11Buffer* p = ppCBs ? ppCBs[i] : nullptr;
        ID3D11Buffer* left  = p;
        ID3D11Buffer* right = p;
        if (p)
        {
            if (auto* bp = TryUnwrapBuffer(static_cast<ID3D11Resource*>(p)))
            {
                left  = bp->GetReal();
                right = bp->GetRealRight() ? bp->GetRealRight() : left;
            }
        }
        s.left[slot]  = left;
        s.right[slot] = right;
        if (slot + 1 > s.high) s.high = slot + 1;
    }
    s.anyStereo = false;
    for (UINT i = 0; i < s.high; ++i)
        if (s.left[i] != s.right[i]) { s.anyStereo = true; break; }
}

void Context11Proxy::TrackOM(UINT NumViews, ID3D11RenderTargetView* const* ppRTVs,
                             ID3D11DepthStencilView* pDSV)
{
    if (!gInfo.DuplicateDraws) return;
    m_omHasUAVs = false;
    m_numRTVs = NumViews <= kMaxRTVs ? NumViews : kMaxRTVs;
    memset(m_rtvLeft,  0, sizeof(m_rtvLeft));
    memset(m_rtvRight, 0, sizeof(m_rtvRight));
    bool anyRTVStereo = false;
    for (UINT i = 0; i < m_numRTVs; ++i)
    {
        ID3D11RenderTargetView* p = ppRTVs ? ppRTVs[i] : nullptr;
        m_rtvLeft[i]  = p;
        // Mono targets must be written exactly once, so the right-eye pass
        // leaves them unbound rather than blending into them a second time.
        m_rtvRight[i] = nullptr;
        if (auto* rp = TryUnwrapRTV(p))
        {
            m_rtvLeft[i]  = rp->GetReal();
            m_rtvRight[i] = rp->GetRealRight();
            if (rp->GetRealRight()) anyRTVStereo = true;
        }
    }
    // A mono depth-stencil stays bound for the right pass: nulling it would
    // drop depth testing entirely, which is worse than testing against depth
    // the left pass already wrote. Counted so we can see if MP3 hits it.
    m_dsvLeft = m_dsvRight = pDSV;
    bool dsvStereo = false;
    if (auto* dp = TryUnwrapDSV(pDSV))
    {
        m_dsvLeft  = dp->GetReal();
        m_dsvRight = dp->GetRealRight() ? dp->GetRealRight() : dp->GetReal();
        dsvStereo  = (dp->GetRealRight() != nullptr);
    }
    m_omMonoDSV   = (pDSV != nullptr) && !dsvStereo;
    m_omAnyStereo = anyRTVStereo || dsvStereo;
}

void Context11Proxy::RecordGameFacingOM(UINT NumViews,
                                        ID3D11RenderTargetView* const* ppRTVs,
                                        ID3D11DepthStencilView* pDSV)
{
    m_numRTVGame = NumViews <= kMaxRTVs ? NumViews : kMaxRTVs;
    memset(m_rtvGame, 0, sizeof(m_rtvGame));
    for (UINT i = 0; i < m_numRTVGame; ++i)
        m_rtvGame[i] = ppRTVs ? ppRTVs[i] : nullptr;
    m_dsvGame = pDSV;
}

void STDMETHODCALLTYPE Context11Proxy::OMGetRenderTargets(
    UINT NumViews, ID3D11RenderTargetView** ppRenderTargetViews,
    ID3D11DepthStencilView** ppDepthStencilView)
{
    if (ppRenderTargetViews)
    {
        for (UINT i = 0; i < NumViews; ++i)
        {
            ID3D11RenderTargetView* v = (i < m_numRTVGame) ? m_rtvGame[i] : nullptr;
            if (v) v->AddRef();
            ppRenderTargetViews[i] = v;
        }
    }
    if (ppDepthStencilView)
    {
        if (m_dsvGame) m_dsvGame->AddRef();
        *ppDepthStencilView = m_dsvGame;
    }
}

void STDMETHODCALLTYPE Context11Proxy::OMGetRenderTargetsAndUnorderedAccessViews(
    UINT NumRTVs, ID3D11RenderTargetView** ppRenderTargetViews,
    ID3D11DepthStencilView** ppDepthStencilView,
    UINT UAVStartSlot, UINT NumUAVs, ID3D11UnorderedAccessView** ppUnorderedAccessViews)
{
    if (ppRenderTargetViews || ppDepthStencilView)
        OMGetRenderTargets(NumRTVs, ppRenderTargetViews, ppDepthStencilView);
    // UAVs aren't eye-doubled, so the real ones are the game-facing ones.
    if (ppUnorderedAccessViews)
        m_real->OMGetRenderTargetsAndUnorderedAccessViews(
            0, nullptr, nullptr, UAVStartSlot, NumUAVs, ppUnorderedAccessViews);
}

void Context11Proxy::BindStageSRVs(StageIdx stage, bool right)
{
    SRVEyeSlots& s = m_srvSlots[stage];
    ID3D11ShaderResourceView* const* v = right ? s.right : s.left;
    switch (stage)
    {
    case ST_VS: m_real->VSSetShaderResources(0, s.high, v); break;
    case ST_PS: m_real->PSSetShaderResources(0, s.high, v); break;
    case ST_GS: m_real->GSSetShaderResources(0, s.high, v); break;
    case ST_HS: m_real->HSSetShaderResources(0, s.high, v); break;
    case ST_DS: m_real->DSSetShaderResources(0, s.high, v); break;
    case ST_CS: m_real->CSSetShaderResources(0, s.high, v); break;
    default: break;
    }
}

void Context11Proxy::TrackCSUAVs(UINT StartSlot, UINT NumUAVs,
                                 ID3D11UnorderedAccessView* const* pp)
{
    UAVEyeSlots& s = m_csUAVSlots;
    for (UINT i = 0; i < NumUAVs; ++i)
    {
        const UINT slot = StartSlot + i;
        if (slot >= kMaxUAVs) break;
        ID3D11UnorderedAccessView* v = pp ? pp[i] : nullptr;
        s.left[slot]  = UnwrapUAVForEye(v, false);
        s.right[slot] = UnwrapUAVForEye(v, true);
        if (s.left[slot] != s.right[slot]) s.anyStereo = true;
        if (slot + 1 > s.high) s.high = slot + 1;
    }
}

void Context11Proxy::BindCSUAVs(bool right)
{
    UAVEyeSlots& s = m_csUAVSlots;
    if (!s.high) return;
    m_real->CSSetUnorderedAccessViews(0, s.high, right ? s.right : s.left, nullptr);
}

// Compute output has no right-eye copy unless the dispatch runs again with the
// right-eye UAVs bound; GTA V drives lighting and post through 1080 of these.
bool Context11Proxy::BeginRightEyeDispatch()
{
    if (!gInfo.DuplicateDraws) return false;
    if (m_activeEye != Eye::Left) return false;
    if (!m_csUAVSlots.anyStereo) return false;
    m_activeEye = Eye::Right;
    BindCSUAVs(true);
    for (UINT st = 0; st < ST_COUNT; ++st)
    {
        if (m_srvSlots[st].anyStereo) BindStageSRVs((StageIdx)st, true);
        if (m_cbSlots[st].anyStereo)  BindStageCBs((StageIdx)st, true);
    }
    return true;
}

void Context11Proxy::EndRightEyeDispatch()
{
    m_activeEye = Eye::Left;
    BindCSUAVs(false);
    for (UINT st = 0; st < ST_COUNT; ++st)
    {
        if (m_srvSlots[st].anyStereo) BindStageSRVs((StageIdx)st, false);
        if (m_cbSlots[st].anyStereo)  BindStageCBs((StageIdx)st, false);
    }
}

void Context11Proxy::BindStageCBs(StageIdx stage, bool right)
{
    CBEyeSlots& s = m_cbSlots[stage];
    ID3D11Buffer* const* v = right ? s.right : s.left;
    switch (stage)
    {
    case ST_VS: m_real->VSSetConstantBuffers(0, s.high, v); break;
    case ST_PS: m_real->PSSetConstantBuffers(0, s.high, v); break;
    case ST_GS: m_real->GSSetConstantBuffers(0, s.high, v); break;
    case ST_HS: m_real->HSSetConstantBuffers(0, s.high, v); break;
    case ST_DS: m_real->DSSetConstantBuffers(0, s.high, v); break;
    case ST_CS: m_real->CSSetConstantBuffers(0, s.high, v); break;
    default: break;
    }
}

void Context11Proxy::BindEye(bool right)
{
    // The duplicated draw and this rebind both call m_real directly, so they
    // bypass the proxy methods that carry FrameTrace. Log them here instead.
    if (FrameTraceActive())
    {
        FrameTrace("    BindEye(%c) numRTV=%u rtv0=%p dsv=%p",
                   right ? 'R' : 'L', m_numRTVs,
                   m_numRTVs ? (right ? m_rtvRight[0] : m_rtvLeft[0]) : nullptr,
                   right ? m_dsvRight : m_dsvLeft);
        for (UINT st = 0; st < ST_COUNT; ++st)
            if (m_srvSlots[st].anyStereo || m_cbSlots[st].anyStereo)
                FrameTrace(" [st%u srv=%d cb=%d]", st,
                           (int)m_srvSlots[st].anyStereo, (int)m_cbSlots[st].anyStereo);
        FrameTrace("\n");
    }
    if (m_omAnyStereo)
        m_real->OMSetRenderTargets(m_numRTVs,
                                   right ? m_rtvRight : m_rtvLeft,
                                   right ? m_dsvRight : m_dsvLeft);
    for (UINT st = 0; st < ST_COUNT; ++st)
    {
        if (m_srvSlots[st].anyStereo) BindStageSRVs((StageIdx)st, right);
        if (m_cbSlots[st].anyStereo)  BindStageCBs((StageIdx)st, right);
    }
    // Must come last: BindStageCBs rebinds the VS constant buffers from tracked
    // state and would otherwise put the game's (usually null) buffer back in the
    // slot the modified shader reads from.
    if (m_modVSShader) BindStereoShiftCB(right);
}

bool Context11Proxy::BeginRightEyeDraw()
{
    if (!gInfo.DuplicateDraws) return false;
    if (m_activeEye != Eye::Left) return false;
    if (m_omHasUAVs) { ++m_drawsUavSkippedThisFrame; return false; }
    if (!m_omAnyStereo) { ++m_drawsMonoThisFrame; return false; }
    ++m_drawsDuplicatedThisFrame;
    if (m_omMonoDSV) ++m_drawsMonoDsvThisFrame;
    // Duplicated, but every right-eye RTV is null: the second draw writes
    // nowhere. Should be impossible while m_omAnyStereo is set via an RTV.
    // Depth-only passes (shadow maps) bind no RTV at all and still reach the
    // right eye through the DSV, so they are counted apart from real misses.
    if (m_numRTVs == 0)
    {
        ++m_drawsDepthOnlyThisFrame;
        if (!m_dsvRight) ++m_drawsNoRightTargetThisFrame;
    }
    else
    {
        bool anyRight = false;
        for (UINT i = 0; i < m_numRTVs && !anyRight; ++i)
            if (m_rtvRight[i]) anyRight = true;
        if (!anyRight)
        {
            ++m_drawsNoRightRTVThisFrame;
            ++m_drawsNoRightTargetThisFrame;
        }
    }
    m_activeEye = Eye::Right;
    BindEye(true);
    return true;
}

void Context11Proxy::EndRightEyeDraw()
{
    BindEye(false);
    m_activeEye = Eye::Left;
}

// Re-issue a draw for the right eye immediately after the game's own call.
#define DUPLICATE_DRAW(CALL)              \
    do {                                  \
        if (!BeginRightEyeDraw()) break;  \
        m_real->CALL;                     \
        EndRightEyeDraw();                \
    } while (0)

// Stage 4b.4 (more state setters): record-and-replay for *SetShaderResources
// across all 6 shader stages. Each stage's method body is identical except
// for the method name, so a macro keeps the boilerplate tractable. Stage 3c.2
// wraps SRVs so we unwrap before forwarding (the runtime can't see our
// vtable past ID3D11ShaderResourceView signatures), and the closure unwraps
// again per-eye on replay so right-eye siblings get bound at the right pass.
// Same gate (m_presentHookActive) as OMSet so games whose swap chain
// bypasses us stay safely in passthrough.
#define RECORD_SRV_SET(STAGE_PREFIX)                                                        \
void STDMETHODCALLTYPE Context11Proxy::STAGE_PREFIX##SetShaderResources(                    \
    UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView* const* ppShaderResourceViews)  \
{                                                                                           \
    TrackSRVs(ST_##STAGE_PREFIX, StartSlot, NumViews, ppShaderResourceViews);               \
    ID3D11ShaderResourceView* rawSRVs[kMaxSRVs] = { 0 };                                    \
    UINT setCap = NumViews <= kMaxSRVs ? NumViews : kMaxSRVs;                               \
    bool pickRight = (m_activeEye == Eye::Right);                                           \
    for (UINT i = 0; i < setCap; ++i)                                                       \
        rawSRVs[i] = UnwrapSRVForEye(ppShaderResourceViews ? ppShaderResourceViews[i]       \
                                                           : nullptr, pickRight);           \
    m_real->STAGE_PREFIX##SetShaderResources(StartSlot, NumViews,                           \
        ppShaderResourceViews ? rawSRVs : nullptr);                                         \
    if (!m_presentHookActive) return;                                                       \
    std::vector<ComRefHolder> refs;                                                         \
    refs.reserve(NumViews);                                                                 \
    for (UINT i = 0; i < NumViews; ++i)                                                     \
        refs.emplace_back(ppShaderResourceViews ? ppShaderResourceViews[i] : nullptr);      \
    m_frameCommands.emplace_back(                                                           \
        [this, StartSlot, NumViews, refs]() {                                               \
            ID3D11ShaderResourceView* raw[kMaxSRVs] = { 0 };                                \
            UINT cap = NumViews <= kMaxSRVs ? NumViews : kMaxSRVs;                          \
            bool pr = (m_activeEye == Eye::Right);                                          \
            for (UINT i = 0; i < cap; ++i)                                                  \
                raw[i] = UnwrapSRVForEye(                                                   \
                    static_cast<ID3D11ShaderResourceView*>(refs[i].p), pr);                 \
            m_real->STAGE_PREFIX##SetShaderResources(StartSlot, NumViews, raw);             \
        });                                                                                 \
}
RECORD_SRV_SET(VS)
RECORD_SRV_SET(PS)
RECORD_SRV_SET(GS)
RECORD_SRV_SET(HS)
RECORD_SRV_SET(DS)
RECORD_SRV_SET(CS)
#undef RECORD_SRV_SET

// *SetSamplers — same shape as *SetShaderResources but with ID3D11SamplerState.
#define RECORD_SAMPLER_SET(STAGE_PREFIX)                                                    \
void STDMETHODCALLTYPE Context11Proxy::STAGE_PREFIX##SetSamplers(                           \
    UINT StartSlot, UINT NumSamplers, ID3D11SamplerState* const* ppSamplers)                \
{                                                                                           \
    m_real->STAGE_PREFIX##SetSamplers(StartSlot, NumSamplers, ppSamplers);                  \
    if (!m_presentHookActive) return;                                                       \
    std::vector<ComRefHolder> refs;                                                         \
    refs.reserve(NumSamplers);                                                              \
    for (UINT i = 0; i < NumSamplers; ++i)                                                  \
        refs.emplace_back(ppSamplers ? ppSamplers[i] : nullptr);                            \
    m_frameCommands.emplace_back(                                                           \
        [this, StartSlot, NumSamplers, refs]() {                                            \
            ID3D11SamplerState* raw[kMaxSamplers] = { 0 };                                  \
            UINT cap = NumSamplers <= kMaxSamplers ? NumSamplers : kMaxSamplers;            \
            for (UINT i = 0; i < cap; ++i)                                                  \
                raw[i] = static_cast<ID3D11SamplerState*>(refs[i].p);                       \
            m_real->STAGE_PREFIX##SetSamplers(StartSlot, NumSamplers, raw);                 \
        });                                                                                 \
}
RECORD_SAMPLER_SET(VS)
RECORD_SAMPLER_SET(PS)
RECORD_SAMPLER_SET(GS)
RECORD_SAMPLER_SET(HS)
RECORD_SAMPLER_SET(DS)
RECORD_SAMPLER_SET(CS)
#undef RECORD_SAMPLER_SET

// *SetConstantBuffers — same shape with ID3D11Buffer. Stage 4c will modify
// the closure body to apply per-eye CB writes (left-right view projection
// matrix), but for 4b.4 it's straight passthrough record-and-replay.
#define RECORD_CB_SET(STAGE_PREFIX, IS_VS_PIPELINE)                                         \
void STDMETHODCALLTYPE Context11Proxy::STAGE_PREFIX##SetConstantBuffers(                    \
    UINT StartSlot, UINT NumBuffers, ID3D11Buffer* const* ppConstantBuffers)                \
{                                                                                           \
    /* Stage 3c.1: unwrap wrapped buffers before forwarding. Also stage-tag */              \
    /* the proxy when VS-pipeline so 4c.1's Map filter can consult it. */                   \
    TrackCBs(ST_##STAGE_PREFIX, StartSlot, NumBuffers, ppConstantBuffers);                  \
    ID3D11Buffer* rawCBs[kMaxCBs] = { 0 };                                                  \
    UINT cap = NumBuffers <= kMaxCBs ? NumBuffers : kMaxCBs;                                \
    for (UINT i = 0; i < cap; ++i)                                                          \
    {                                                                                       \
        ID3D11Buffer* p = ppConstantBuffers ? ppConstantBuffers[i] : nullptr;               \
        if (p)                                                                              \
        {                                                                                   \
            if (auto* bp = wiz3d::TryUnwrapBuffer(static_cast<ID3D11Resource*>(p)))         \
            {                                                                               \
                if (IS_VS_PIPELINE) bp->TagVSBound();                                       \
                rawCBs[i] = bp->GetReal();                                                  \
            }                                                                               \
            else                                                                            \
            {                                                                               \
                rawCBs[i] = p;                                                              \
            }                                                                               \
        }                                                                                   \
    }                                                                                       \
    m_real->STAGE_PREFIX##SetConstantBuffers(StartSlot, NumBuffers,                         \
        ppConstantBuffers ? rawCBs : nullptr);                                              \
    if (!m_presentHookActive) return;                                                       \
    std::vector<ComRefHolder> refs;                                                         \
    refs.reserve(NumBuffers);                                                               \
    for (UINT i = 0; i < NumBuffers; ++i)                                                   \
        refs.emplace_back(ppConstantBuffers ? ppConstantBuffers[i] : nullptr);              \
    m_frameCommands.emplace_back(                                                           \
        [this, StartSlot, NumBuffers, refs]() {                                             \
            ID3D11Buffer* raw[kMaxCBs] = { 0 };                                             \
            UINT replayCap = NumBuffers <= kMaxCBs ? NumBuffers : kMaxCBs;                  \
            for (UINT i = 0; i < replayCap; ++i)                                            \
                raw[i] = UnwrapBuf(static_cast<ID3D11Buffer*>(refs[i].p));                  \
            m_real->STAGE_PREFIX##SetConstantBuffers(StartSlot, NumBuffers, raw);           \
        });                                                                                 \
}
RECORD_CB_SET(GS, 1)
RECORD_CB_SET(HS, 1)
RECORD_CB_SET(DS, 1)
RECORD_CB_SET(CS, 0)
#undef RECORD_CB_SET

// Stage 4e.2: VS variant of RECORD_CB_SET that ALSO updates m_boundVSCBs[]
// so Unmap can recover which slot each CB is at when consulting the bound
// VS's projection-matrix register table.
void STDMETHODCALLTYPE Context11Proxy::VSSetConstantBuffers(
    UINT StartSlot, UINT NumBuffers, ID3D11Buffer* const* ppConstantBuffers)
{
    TrackCBs(ST_VS, StartSlot, NumBuffers, ppConstantBuffers);
    ID3D11Buffer* rawCBs[kMaxCBs] = { 0 };
    UINT cap = NumBuffers <= kMaxCBs ? NumBuffers : kMaxCBs;
    for (UINT i = 0; i < cap; ++i)
    {
        ID3D11Buffer* p = ppConstantBuffers ? ppConstantBuffers[i] : nullptr;
        // 4e.2 slot snapshot — stored even when m_presentHookActive is off,
        // because Unmap path always uses it.
        UINT slot = StartSlot + i;
        if (slot < kMaxVSCBSlots) m_boundVSCBs[slot] = p;

        if (p)
        {
            if (auto* bp = wiz3d::TryUnwrapBuffer(static_cast<ID3D11Resource*>(p)))
            {
                bp->TagVSBound();
                rawCBs[i] = bp->GetReal();
            }
            else
            {
                rawCBs[i] = p;
            }
        }
    }
    m_real->VSSetConstantBuffers(StartSlot, NumBuffers, ppConstantBuffers ? rawCBs : nullptr);

    // A modified shader reads its shift constants from a slot ModifyShader picked
    // as unused, but the game still binds arrays that span it — usually right
    // after VSSetShader. Re-bind ours whenever that range covers it, or the
    // shader reads an unbound CB and the driver faults.
    if (m_modVSShader && m_modVSCBIndex >= StartSlot && m_modVSCBIndex < StartSlot + NumBuffers)
        BindStereoShiftCB(m_activeEye == Eye::Right);

    if (!m_presentHookActive) return;
    std::vector<ComRefHolder> refs;
    refs.reserve(NumBuffers);
    for (UINT i = 0; i < NumBuffers; ++i)
        refs.emplace_back(ppConstantBuffers ? ppConstantBuffers[i] : nullptr);
    m_frameCommands.emplace_back(
        [this, StartSlot, NumBuffers, refs]() {
            ID3D11Buffer* raw[kMaxCBs] = { 0 };
            UINT replayCap = NumBuffers <= kMaxCBs ? NumBuffers : kMaxCBs;
            for (UINT i = 0; i < replayCap; ++i)
                raw[i] = UnwrapBuf(static_cast<ID3D11Buffer*>(refs[i].p));
            m_real->VSSetConstantBuffers(StartSlot, NumBuffers, raw);
        });
}

// PS variant of RECORD_CB_SET: same body minus the VS-pipeline tag, plus the
// m_boundPSCBs[] slot snapshot the profile matrix lookup needs in Unmap.
void STDMETHODCALLTYPE Context11Proxy::PSSetConstantBuffers(
    UINT StartSlot, UINT NumBuffers, ID3D11Buffer* const* ppConstantBuffers)
{
    TrackCBs(ST_PS, StartSlot, NumBuffers, ppConstantBuffers);
    ID3D11Buffer* rawCBs[kMaxCBs] = { 0 };
    UINT cap = NumBuffers <= kMaxCBs ? NumBuffers : kMaxCBs;
    for (UINT i = 0; i < cap; ++i)
    {
        ID3D11Buffer* p = ppConstantBuffers ? ppConstantBuffers[i] : nullptr;
        UINT slot = StartSlot + i;
        if (slot < kMaxPSCBSlots) m_boundPSCBs[slot] = p;

        if (p)
        {
            if (auto* bp = wiz3d::TryUnwrapBuffer(static_cast<ID3D11Resource*>(p)))
                rawCBs[i] = bp->GetReal();
            else
                rawCBs[i] = p;
        }
    }
    m_real->PSSetConstantBuffers(StartSlot, NumBuffers, ppConstantBuffers ? rawCBs : nullptr);
    if (!m_presentHookActive) return;
    std::vector<ComRefHolder> refs;
    refs.reserve(NumBuffers);
    for (UINT i = 0; i < NumBuffers; ++i)
        refs.emplace_back(ppConstantBuffers ? ppConstantBuffers[i] : nullptr);
    m_frameCommands.emplace_back(
        [this, StartSlot, NumBuffers, refs]() {
            ID3D11Buffer* raw[kMaxCBs] = { 0 };
            UINT replayCap = NumBuffers <= kMaxCBs ? NumBuffers : kMaxCBs;
            for (UINT i = 0; i < replayCap; ++i)
                raw[i] = UnwrapBuf(static_cast<ID3D11Buffer*>(refs[i].p));
            m_real->PSSetConstantBuffers(StartSlot, NumBuffers, raw);
        });
}

// *SetShader — takes the stage-specific shader interface plus the
// class-instance array. Class instances are rarely non-null (used for
// dynamic shader linking) but the array is captured for fidelity.
#define RECORD_SHADER_SET(STAGE_PREFIX, SHADER_TYPE)                                        \
void STDMETHODCALLTYPE Context11Proxy::STAGE_PREFIX##SetShader(                             \
    SHADER_TYPE* pShader, ID3D11ClassInstance* const* ppClassInstances, UINT NumClassInstances) \
{                                                                                           \
    m_real->STAGE_PREFIX##SetShader(pShader, ppClassInstances, NumClassInstances);          \
    if (!m_presentHookActive) return;                                                       \
    ComRefHolder shaderRef(pShader);                                                        \
    std::vector<ComRefHolder> ciRefs;                                                       \
    ciRefs.reserve(NumClassInstances);                                                      \
    for (UINT i = 0; i < NumClassInstances; ++i)                                            \
        ciRefs.emplace_back(ppClassInstances ? ppClassInstances[i] : nullptr);              \
    m_frameCommands.emplace_back(                                                           \
        [this, shaderRef, ciRefs, NumClassInstances]() {                                    \
            ID3D11ClassInstance* raw[kMaxClassInst] = { 0 };                                \
            UINT cap = NumClassInstances <= kMaxClassInst ? NumClassInstances : kMaxClassInst; \
            for (UINT i = 0; i < cap; ++i)                                                  \
                raw[i] = static_cast<ID3D11ClassInstance*>(ciRefs[i].p);                    \
            m_real->STAGE_PREFIX##SetShader(                                                \
                static_cast<SHADER_TYPE*>(shaderRef.p),                                     \
                ciRefs.empty() ? nullptr : raw,                                             \
                NumClassInstances);                                                         \
        });                                                                                 \
}
RECORD_SHADER_SET(GS, ID3D11GeometryShader)
RECORD_SHADER_SET(HS, ID3D11HullShader)
RECORD_SHADER_SET(DS, ID3D11DomainShader)
RECORD_SHADER_SET(CS, ID3D11ComputeShader)

// Stage 4e.2: VS variant that ALSO snapshots m_boundVS for the Unmap path's
// projection-data lookup. Snapshot stored regardless of m_presentHookActive
// because Unmap-time consultation is independent of the replay-recording
// gate. NB: not AddRef'd — the game owns the shader; if it Releases under
// us, we'll get a stale-positive lookup that either misses (no entry in
// shaderProjections map) or hits an entry that's no longer the live shader,
// which downgrades to the original heuristic / no-op for that CB.
void STDMETHODCALLTYPE Context11Proxy::VSSetShader(
    ID3D11VertexShader* pShader, ID3D11ClassInstance* const* ppClassInstances,
    UINT NumClassInstances)
{
    m_boundVS = pShader;

    // A modified shader shifts its own output from a constant we supply, so it
    // runs for BOTH eyes and only the constant differs — the left eye gets the
    // negated separation. Bind the left CB now; BindEye swaps it per eye.
    m_modVSShader = nullptr;
    if (gInfo.ModifyShadersDX11 && m_parent)
    {
        if (const Device11Proxy::ModifiedVS* mv = m_parent->LookupModifiedVS(pShader))
        {
            m_modVSShader  = mv->shader;
            m_modVSCBIndex = mv->data.CBIndex;
        }
    }
    if (m_modVSShader)
    {
        m_real->VSSetShader(m_modVSShader, ppClassInstances, NumClassInstances);
        BindStereoShiftCB(false);
    }
    else
    {
        m_real->VSSetShader(pShader, ppClassInstances, NumClassInstances);
    }
    if (!m_presentHookActive) return;
    ComRefHolder shaderRef(pShader);
    std::vector<ComRefHolder> ciRefs;
    ciRefs.reserve(NumClassInstances);
    for (UINT i = 0; i < NumClassInstances; ++i)
        ciRefs.emplace_back(ppClassInstances ? ppClassInstances[i] : nullptr);
    m_frameCommands.emplace_back(
        [this, shaderRef, ciRefs, NumClassInstances]() {
            ID3D11ClassInstance* raw[kMaxClassInst] = { 0 };
            UINT cap = NumClassInstances <= kMaxClassInst ? NumClassInstances : kMaxClassInst;
            for (UINT i = 0; i < cap; ++i)
                raw[i] = static_cast<ID3D11ClassInstance*>(ciRefs[i].p);
            m_real->VSSetShader(
                static_cast<ID3D11VertexShader*>(shaderRef.p),
                ciRefs.empty() ? nullptr : raw, NumClassInstances);
        });
}

// PS variant: snapshots m_boundPS so Unmap can match the bound pixel shader's
// CRC against BaseProfile.xml's <PixelShader> matrix declarations. Not
// AddRef'd, same rationale as m_boundVS above.
void STDMETHODCALLTYPE Context11Proxy::PSSetShader(
    ID3D11PixelShader* pShader, ID3D11ClassInstance* const* ppClassInstances,
    UINT NumClassInstances)
{
    m_boundPS = pShader;
    m_real->PSSetShader(pShader, ppClassInstances, NumClassInstances);
    if (!m_presentHookActive) return;
    ComRefHolder shaderRef(pShader);
    std::vector<ComRefHolder> ciRefs;
    ciRefs.reserve(NumClassInstances);
    for (UINT i = 0; i < NumClassInstances; ++i)
        ciRefs.emplace_back(ppClassInstances ? ppClassInstances[i] : nullptr);
    m_frameCommands.emplace_back(
        [this, shaderRef, ciRefs, NumClassInstances]() {
            ID3D11ClassInstance* raw[kMaxClassInst] = { 0 };
            UINT cap = NumClassInstances <= kMaxClassInst ? NumClassInstances : kMaxClassInst;
            for (UINT i = 0; i < cap; ++i)
                raw[i] = static_cast<ID3D11ClassInstance*>(ciRefs[i].p);
            m_real->PSSetShader(
                static_cast<ID3D11PixelShader*>(shaderRef.p),
                ciRefs.empty() ? nullptr : raw, NumClassInstances);
        });
}
#undef RECORD_SHADER_SET

// CRC of a bound shader, 0 if unknown. Lets the frame trace name the shader a
// BaseProfile.xml <VertexShader>/<PixelShader> entry would have to target.
static DWORD BoundShaderCRC(Device11Proxy* parent, void* shader)
{
    if (!parent || !shader) return 0;
    const ShaderAnalysis11Result* info = parent->LookupShaderProjection(shader);
    return info ? info->crc32 : 0;
}

// Stage 4b.7: record-and-replay for draw/dispatch. Pure POD captures for the
// non-Indirect variants; Indirect/Dispatch-with-buffer use ComRefHolder to
// keep the arg buffer alive across replay. The closure body is the same shape
// as the original passthrough — no Do* helpers needed since draws don't
// reference our wrapped resources directly (the bound RTV/VB/IB/CB are picked
// up from the bound pipeline state, which the *Set* closures already replay
// with eye selection).

void STDMETHODCALLTYPE Context11Proxy::Draw(UINT VertexCount, UINT StartVertexLocation)
{
    ++m_drawsThisFrame;
    if (m_modVSShader) BindStereoShiftCB(m_activeEye == Eye::Right);
    if (FrameTraceActive())
        FrameTrace("    Draw eye=%c vcount=%u start=%u vs=0x%08lX ps=0x%08lX\n",
                   m_activeEye == Eye::Right ? 'R' : 'L',
                   VertexCount, StartVertexLocation,
                   BoundShaderCRC(m_parent, m_boundVS),
                   BoundShaderCRC(m_parent, m_boundPS));
    m_real->Draw(VertexCount, StartVertexLocation);
    DUPLICATE_DRAW(Draw(VertexCount, StartVertexLocation));
    if (!m_presentHookActive) return;
    m_frameCommands.emplace_back(
        [this, VertexCount, StartVertexLocation]()
        {
            if (FrameTraceActive())
                FrameTrace("    Draw eye=%c vcount=%u start=%u (replay)\n",
                           m_activeEye == Eye::Right ? 'R' : 'L',
                           VertexCount, StartVertexLocation);
            m_real->Draw(VertexCount, StartVertexLocation);
        });
}

void STDMETHODCALLTYPE Context11Proxy::DrawIndexed(
    UINT IndexCount, UINT StartIndexLocation, INT BaseVertexLocation)
{
    ++m_drawsThisFrame;
    if (m_modVSShader) BindStereoShiftCB(m_activeEye == Eye::Right);
    if (FrameTraceActive())
        FrameTrace("    DrawIndexed eye=%c icount=%u start=%u base=%d vs=0x%08lX ps=0x%08lX\n",
                   m_activeEye == Eye::Right ? 'R' : 'L',
                   IndexCount, StartIndexLocation, BaseVertexLocation,
                   BoundShaderCRC(m_parent, m_boundVS),
                   BoundShaderCRC(m_parent, m_boundPS));
    m_real->DrawIndexed(IndexCount, StartIndexLocation, BaseVertexLocation);
    DUPLICATE_DRAW(DrawIndexed(IndexCount, StartIndexLocation, BaseVertexLocation));
    if (!m_presentHookActive) return;
    m_frameCommands.emplace_back(
        [this, IndexCount, StartIndexLocation, BaseVertexLocation]()
        {
            if (FrameTraceActive())
                FrameTrace("    DrawIndexed eye=%c icount=%u start=%u base=%d (replay)\n",
                           m_activeEye == Eye::Right ? 'R' : 'L',
                           IndexCount, StartIndexLocation, BaseVertexLocation);
            m_real->DrawIndexed(IndexCount, StartIndexLocation, BaseVertexLocation);
        });
}

void STDMETHODCALLTYPE Context11Proxy::DrawInstanced(
    UINT VertexCountPerInstance, UINT InstanceCount,
    UINT StartVertexLocation, UINT StartInstanceLocation)
{
    ++m_drawsThisFrame;
    if (m_modVSShader) BindStereoShiftCB(m_activeEye == Eye::Right);
    m_real->DrawInstanced(VertexCountPerInstance, InstanceCount,
                          StartVertexLocation, StartInstanceLocation);
    DUPLICATE_DRAW(DrawInstanced(VertexCountPerInstance, InstanceCount,
                                 StartVertexLocation, StartInstanceLocation));
    if (!m_presentHookActive) return;
    m_frameCommands.emplace_back(
        [this, VertexCountPerInstance, InstanceCount,
         StartVertexLocation, StartInstanceLocation]()
        {
            m_real->DrawInstanced(VertexCountPerInstance, InstanceCount,
                                   StartVertexLocation, StartInstanceLocation);
        });
}

void STDMETHODCALLTYPE Context11Proxy::DrawIndexedInstanced(
    UINT IndexCountPerInstance, UINT InstanceCount, UINT StartIndexLocation,
    INT BaseVertexLocation, UINT StartInstanceLocation)
{
    ++m_drawsThisFrame;
    if (m_modVSShader) BindStereoShiftCB(m_activeEye == Eye::Right);
    m_real->DrawIndexedInstanced(IndexCountPerInstance, InstanceCount,
                                  StartIndexLocation, BaseVertexLocation,
                                  StartInstanceLocation);
    DUPLICATE_DRAW(DrawIndexedInstanced(IndexCountPerInstance, InstanceCount,
                                        StartIndexLocation, BaseVertexLocation,
                                        StartInstanceLocation));
    if (!m_presentHookActive) return;
    m_frameCommands.emplace_back(
        [this, IndexCountPerInstance, InstanceCount, StartIndexLocation,
         BaseVertexLocation, StartInstanceLocation]()
        {
            m_real->DrawIndexedInstanced(
                IndexCountPerInstance, InstanceCount, StartIndexLocation,
                BaseVertexLocation, StartInstanceLocation);
        });
}

void STDMETHODCALLTYPE Context11Proxy::DrawAuto()
{
    ++m_drawsThisFrame;
    if (m_modVSShader) BindStereoShiftCB(m_activeEye == Eye::Right);
    m_real->DrawAuto();
    DUPLICATE_DRAW(DrawAuto());
    if (!m_presentHookActive) return;
    m_frameCommands.emplace_back(
        [this]() { m_real->DrawAuto(); });
}

void STDMETHODCALLTYPE Context11Proxy::DrawInstancedIndirect(
    ID3D11Buffer* pBufferForArgs, UINT AlignedByteOffsetForArgs)
{
    ++m_drawsThisFrame;
    if (m_modVSShader) BindStereoShiftCB(m_activeEye == Eye::Right);
    m_real->DrawInstancedIndirect(UnwrapBuf(pBufferForArgs), AlignedByteOffsetForArgs);
    DUPLICATE_DRAW(DrawInstancedIndirect(UnwrapBuf(pBufferForArgs), AlignedByteOffsetForArgs));
    if (!m_presentHookActive) return;
    ComRefHolder bufRef(pBufferForArgs);
    m_frameCommands.emplace_back(
        [this, bufRef, AlignedByteOffsetForArgs]()
        {
            m_real->DrawInstancedIndirect(
                UnwrapBuf(static_cast<ID3D11Buffer*>(bufRef.p)), AlignedByteOffsetForArgs);
        });
}

void STDMETHODCALLTYPE Context11Proxy::DrawIndexedInstancedIndirect(
    ID3D11Buffer* pBufferForArgs, UINT AlignedByteOffsetForArgs)
{
    ++m_drawsThisFrame;
    if (m_modVSShader) BindStereoShiftCB(m_activeEye == Eye::Right);
    m_real->DrawIndexedInstancedIndirect(UnwrapBuf(pBufferForArgs), AlignedByteOffsetForArgs);
    DUPLICATE_DRAW(DrawIndexedInstancedIndirect(UnwrapBuf(pBufferForArgs), AlignedByteOffsetForArgs));
    if (!m_presentHookActive) return;
    ComRefHolder bufRef(pBufferForArgs);
    m_frameCommands.emplace_back(
        [this, bufRef, AlignedByteOffsetForArgs]()
        {
            m_real->DrawIndexedInstancedIndirect(
                UnwrapBuf(static_cast<ID3D11Buffer*>(bufRef.p)), AlignedByteOffsetForArgs);
        });
}

void STDMETHODCALLTYPE Context11Proxy::Dispatch(
    UINT ThreadGroupCountX, UINT ThreadGroupCountY, UINT ThreadGroupCountZ)
{
    ++m_dispatchesThisFrame;
    m_real->Dispatch(ThreadGroupCountX, ThreadGroupCountY, ThreadGroupCountZ);
    if (BeginRightEyeDispatch())
    {
        ++m_dispatchesDuplicatedThisFrame;
        m_real->Dispatch(ThreadGroupCountX, ThreadGroupCountY, ThreadGroupCountZ);
        EndRightEyeDispatch();
    }
    if (!m_presentHookActive) return;
    m_frameCommands.emplace_back(
        [this, ThreadGroupCountX, ThreadGroupCountY, ThreadGroupCountZ]()
        {
            m_real->Dispatch(ThreadGroupCountX, ThreadGroupCountY, ThreadGroupCountZ);
        });
}

void STDMETHODCALLTYPE Context11Proxy::DispatchIndirect(
    ID3D11Buffer* pBufferForArgs, UINT AlignedByteOffsetForArgs)
{
    ++m_dispatchesThisFrame;
    m_real->DispatchIndirect(UnwrapBuf(pBufferForArgs), AlignedByteOffsetForArgs);
    if (BeginRightEyeDispatch())
    {
        ++m_dispatchesDuplicatedThisFrame;
        m_real->DispatchIndirect(UnwrapBuf(pBufferForArgs), AlignedByteOffsetForArgs);
        EndRightEyeDispatch();
    }
    if (!m_presentHookActive) return;
    ComRefHolder bufRef(pBufferForArgs);
    m_frameCommands.emplace_back(
        [this, bufRef, AlignedByteOffsetForArgs]()
        {
            m_real->DispatchIndirect(
                UnwrapBuf(static_cast<ID3D11Buffer*>(bufRef.p)), AlignedByteOffsetForArgs);
        });
}

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
    // Claim Context1/2/3 with `this` so games accessing the immediate context
    // via Device1::GetImmediateContext1 (or QI for the higher versions) land
    // on our proxy rather than the unwrapped real ctx. Without this, every
    // state-setter call after that would bypass our record-for-replay logic.
    if (riid == __uuidof(ID3D11DeviceContext1) && m_real1)
    {
        *ppvObj = static_cast<ID3D11DeviceContext1*>(this);
        AddRef();
        return S_OK;
    }
    if (riid == __uuidof(ID3D11DeviceContext2) && m_real2)
    {
        *ppvObj = static_cast<ID3D11DeviceContext2*>(this);
        AddRef();
        return S_OK;
    }
    if (riid == __uuidof(ID3D11DeviceContext3) && m_real3)
    {
        *ppvObj = static_cast<ID3D11DeviceContext3*>(this);
        AddRef();
        return S_OK;
    }
    *ppvObj = nullptr;
    return E_NOINTERFACE;
}

void Context11Proxy::DoOMSetRenderTargets(
    UINT NumViews, ID3D11RenderTargetView* const* ppRenderTargetViews,
    ID3D11DepthStencilView* pDepthStencilView)
{
    // Stage 4a: pick the left- or right-eye real handle for each wrapped
    // RTV/DSV based on m_activeEye. When the proxy isn't stereo, both
    // GetReal() and GetRealRight() resolve to the same left-eye handle (the
    // latter is null, so we fall back to left). Stage 4b.8 will flip
    // m_activeEye between L/R passes during the per-frame replay.
    bool pickRight = (m_activeEye == Eye::Right);
    RecordGameFacingOM(NumViews, ppRenderTargetViews, pDepthStencilView);
    if (FrameTraceActive())
    {
        ID3D11RenderTargetView* raw0 = (NumViews > 0 && ppRenderTargetViews)
                                           ? ppRenderTargetViews[0] : nullptr;
        RTV11Proxy* rtv0 = raw0 ? TryUnwrapRTV(raw0) : nullptr;
        DSV11Proxy* dsv  = TryUnwrapDSV(pDepthStencilView);
        // ptr!=0 with proxy==0 means the game bound a real RTV we can't resolve
        // — a bypass, not an unbind. The two used to look identical here.
        FrameTrace("  OMSet eye=%c NumViews=%u rtv0={ptr=%p proxy=%p stereo=%d right=%p} dsv={proxy=%p stereo=%d right=%p}\n",
                   pickRight ? 'R' : 'L', NumViews, raw0,
                   rtv0, rtv0 ? rtv0->GetRealRight() != nullptr : 0,
                   rtv0 ? rtv0->GetRealRight() : nullptr,
                   dsv,  dsv  ? dsv->GetRealRight() != nullptr  : 0,
                   dsv  ? dsv->GetRealRight()  : nullptr);
    }
    ID3D11RenderTargetView* realRTVs[kMaxRTVs] = { 0 };
    ID3D11RenderTargetView* const* rtvsToUse = ppRenderTargetViews;
    if (NumViews > 0 && ppRenderTargetViews)
    {
        UINT cap = NumViews <= kMaxRTVs ? NumViews : kMaxRTVs;
        for (UINT i = 0; i < cap; ++i)
        {
            RTV11Proxy* p = TryUnwrapRTV(ppRenderTargetViews[i]);
            if (!p)
            {
                realRTVs[i] = ppRenderTargetViews[i];
                continue;
            }
            ID3D11RenderTargetView* right = p->GetRealRight();
            realRTVs[i] = (pickRight && right) ? right : p->GetReal();
        }
        rtvsToUse = realRTVs;
    }
    ID3D11DepthStencilView* realDSV = pDepthStencilView;
    if (DSV11Proxy* d = TryUnwrapDSV(pDepthStencilView))
    {
        ID3D11DepthStencilView* right = d->GetRealRight();
        realDSV = (pickRight && right) ? right : d->GetReal();
    }
    m_real->OMSetRenderTargets(NumViews, rtvsToUse, realDSV);
}

void STDMETHODCALLTYPE Context11Proxy::OMSetRenderTargets(
    UINT NumViews, ID3D11RenderTargetView* const* ppRenderTargetViews,
    ID3D11DepthStencilView* pDepthStencilView)
{
    TrackOM(NumViews, ppRenderTargetViews, pDepthStencilView);
    DoOMSetRenderTargets(NumViews, ppRenderTargetViews, pDepthStencilView);

    // Stage 4b.4: record-for-replay, but only when the Present hook is
    // active. Without a flush-each-frame trigger the vector would grow
    // unbounded, so games whose swap chain bypasses us stay safely in pure
    // passthrough mode. Capture the wrapped pointers by value (ComRefHolder
    // copy ctor AddRefs) so the lambda holds its own refs for the frame
    // even if the game releases. At replay time the closure re-calls
    // DoOMSetRenderTargets, which re-runs eye-aware unwrap with whatever
    // m_activeEye is set to at that point.
    if (!m_presentHookActive) return;
    std::vector<ComRefHolder> rtvRefs;
    rtvRefs.reserve(NumViews);
    for (UINT i = 0; i < NumViews; ++i)
        rtvRefs.emplace_back(ppRenderTargetViews ? ppRenderTargetViews[i] : nullptr);
    ComRefHolder dsvRef(pDepthStencilView);
    m_frameCommands.emplace_back(
        [this, NumViews, rtvRefs, dsvRef]()
        {
            // Rebuild raw-pointer array from the captured holders.
            ID3D11RenderTargetView* raw[kMaxRTVs] = { 0 };
            UINT cap = NumViews <= kMaxRTVs ? NumViews : kMaxRTVs;
            for (UINT i = 0; i < cap; ++i)
                raw[i] = static_cast<ID3D11RenderTargetView*>(rtvRefs[i].p);
            DoOMSetRenderTargets(NumViews, raw,
                static_cast<ID3D11DepthStencilView*>(dsvRef.p));
        });
}

void Context11Proxy::DoOMSetRenderTargetsAndUnorderedAccessViews(
    UINT NumRTVs, ID3D11RenderTargetView* const* ppRenderTargetViews,
    ID3D11DepthStencilView* pDepthStencilView,
    UINT UAVStartSlot, UINT NumUAVs,
    ID3D11UnorderedAccessView* const* ppUnorderedAccessViews,
    const UINT* pUAVInitialCounts)
{
    bool pickRight = (m_activeEye == Eye::Right);
    // KEEP_RENDER_TARGETS leaves the existing binding in place, so don't
    // overwrite what we recorded from the last real bind.
    if (NumRTVs != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL)
        RecordGameFacingOM(NumRTVs, ppRenderTargetViews, pDepthStencilView);
    // Was untraced, so binds through this path looked like "no RTV bound".
    if (FrameTraceActive())
    {
        ID3D11RenderTargetView* raw0 = (NumRTVs != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL &&
                                        NumRTVs > 0 && ppRenderTargetViews)
                                       ? ppRenderTargetViews[0] : nullptr;
        RTV11Proxy* r0 = raw0 ? TryUnwrapRTV(raw0) : nullptr;
        FrameTrace("  OMSetUAV eye=%c NumRTVs=%u NumUAVs=%u rtv0={ptr=%p proxy=%p stereo=%d}\n",
                   pickRight ? 'R' : 'L', NumRTVs, NumUAVs, raw0,
                   r0, r0 ? (r0->GetRealRight() != nullptr) : 0);
    }
    ID3D11RenderTargetView* realRTVs[kMaxRTVs] = { 0 };
    ID3D11RenderTargetView* const* rtvsToUse = ppRenderTargetViews;
    if (NumRTVs != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL &&
        NumRTVs > 0 && ppRenderTargetViews)
    {
        UINT cap = NumRTVs <= kMaxRTVs ? NumRTVs : kMaxRTVs;
        for (UINT i = 0; i < cap; ++i)
        {
            RTV11Proxy* p = TryUnwrapRTV(ppRenderTargetViews[i]);
            if (!p)
            {
                realRTVs[i] = ppRenderTargetViews[i];
                continue;
            }
            ID3D11RenderTargetView* right = p->GetRealRight();
            realRTVs[i] = (pickRight && right) ? right : p->GetReal();
        }
        rtvsToUse = realRTVs;
    }
    ID3D11DepthStencilView* realDSV = pDepthStencilView;
    if (NumRTVs != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL)
    {
        if (DSV11Proxy* d = TryUnwrapDSV(pDepthStencilView))
        {
            ID3D11DepthStencilView* right = d->GetRealRight();
            realDSV = (pickRight && right) ? right : d->GetReal();
        }
    }
    // Stage 3c.2: unwrap UAVs (eye-aware) before forwarding.
    ID3D11UnorderedAccessView* realUAVs[kMaxUAVs] = { 0 };
    ID3D11UnorderedAccessView* const* uavsToUse = ppUnorderedAccessViews;
    if (NumUAVs != D3D11_KEEP_UNORDERED_ACCESS_VIEWS &&
        NumUAVs > 0 && ppUnorderedAccessViews)
    {
        UINT ucap = NumUAVs <= kMaxUAVs ? NumUAVs : kMaxUAVs;
        for (UINT i = 0; i < ucap; ++i)
            realUAVs[i] = UnwrapUAVForEye(ppUnorderedAccessViews[i], pickRight);
        uavsToUse = realUAVs;
    }
    m_real->OMSetRenderTargetsAndUnorderedAccessViews(
        NumRTVs, rtvsToUse, realDSV,
        UAVStartSlot, NumUAVs, uavsToUse, pUAVInitialCounts);
}

void STDMETHODCALLTYPE Context11Proxy::OMSetRenderTargetsAndUnorderedAccessViews(
    UINT NumRTVs, ID3D11RenderTargetView* const* ppRenderTargetViews,
    ID3D11DepthStencilView* pDepthStencilView,
    UINT UAVStartSlot, UINT NumUAVs,
    ID3D11UnorderedAccessView* const* ppUnorderedAccessViews,
    const UINT* pUAVInitialCounts)
{
    // D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL leaves the OM bindings
    // alone, so the tracked pair stays valid and must not be overwritten.
    if (NumRTVs != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL)
        TrackOM(NumRTVs, ppRenderTargetViews, pDepthStencilView);
    if (gInfo.DuplicateDraws && NumUAVs > 0 &&
        NumUAVs != D3D11_KEEP_UNORDERED_ACCESS_VIEWS)
        m_omHasUAVs = true;
    DoOMSetRenderTargetsAndUnorderedAccessViews(
        NumRTVs, ppRenderTargetViews, pDepthStencilView,
        UAVStartSlot, NumUAVs, ppUnorderedAccessViews, pUAVInitialCounts);

    if (!m_presentHookActive) return;
    // Stage 4b.4: record. Both RTV and UAV arrays need capture.
    std::vector<ComRefHolder> rtvRefs;
    if (NumRTVs != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL && ppRenderTargetViews)
    {
        rtvRefs.reserve(NumRTVs);
        for (UINT i = 0; i < NumRTVs; ++i)
            rtvRefs.emplace_back(ppRenderTargetViews[i]);
    }
    ComRefHolder dsvRef(pDepthStencilView);

    std::vector<ComRefHolder> uavRefs;
    if (ppUnorderedAccessViews)
    {
        uavRefs.reserve(NumUAVs);
        for (UINT i = 0; i < NumUAVs; ++i)
            uavRefs.emplace_back(ppUnorderedAccessViews[i]);
    }
    std::vector<UINT> initialCounts;
    if (pUAVInitialCounts)
        initialCounts.assign(pUAVInitialCounts, pUAVInitialCounts + NumUAVs);

    m_frameCommands.emplace_back(
        [this, NumRTVs, rtvRefs, dsvRef,
         UAVStartSlot, NumUAVs, uavRefs, initialCounts]()
        {
            ID3D11RenderTargetView* rawRTVs[kMaxRTVs] = { 0 };
            ID3D11RenderTargetView* const* rtvArg = nullptr;
            if (NumRTVs != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL && !rtvRefs.empty())
            {
                UINT cap = NumRTVs <= kMaxRTVs ? NumRTVs : kMaxRTVs;
                for (UINT i = 0; i < cap; ++i)
                    rawRTVs[i] = static_cast<ID3D11RenderTargetView*>(rtvRefs[i].p);
                rtvArg = rawRTVs;
            }
            // UAVs reconstructed similarly. Stage 3c.2: now wrapped, so the
            // inner DoOMSet... helper will unwrap eye-aware. Hand it the raw
            // proxies retrieved from the captured refs.
            ID3D11UnorderedAccessView* rawUAVs[kMaxUAVs] = { 0 };
            ID3D11UnorderedAccessView* const* uavArg = nullptr;
            if (!uavRefs.empty())
            {
                UINT cap = NumUAVs <= kMaxUAVs ? NumUAVs : kMaxUAVs;
                for (UINT i = 0; i < cap; ++i)
                    rawUAVs[i] = static_cast<ID3D11UnorderedAccessView*>(uavRefs[i].p);
                uavArg = rawUAVs;
            }
            const UINT* countsArg = initialCounts.empty() ? nullptr : initialCounts.data();
            DoOMSetRenderTargetsAndUnorderedAccessViews(
                NumRTVs, rtvArg,
                static_cast<ID3D11DepthStencilView*>(dsvRef.p),
                UAVStartSlot, NumUAVs, uavArg, countsArg);
        });
}

void STDMETHODCALLTYPE Context11Proxy::RSSetViewports(UINT NumViewports, const D3D11_VIEWPORT* pViewports)
{
    m_real->RSSetViewports(NumViewports, pViewports);
    if (!m_presentHookActive) return;
    // Capture for replay. Without this the right-eye pass runs all draws
    // against whatever viewport is current at Present time (likely the SBS
    // compose viewport or the last UI viewport), producing the right-eye
    // geometry glitching / missing-elements / doubled-cursor symptoms that
    // followed the May 2026 kMaxUnwrapArray fix.
    std::vector<D3D11_VIEWPORT> vps;
    if (pViewports) vps.assign(pViewports, pViewports + NumViewports);
    m_frameCommands.emplace_back(
        [this, NumViewports, vps]()
        {
            m_real->RSSetViewports(
                NumViewports, vps.empty() ? nullptr : vps.data());
        });
}

void STDMETHODCALLTYPE Context11Proxy::CopyStructureCount(
    ID3D11Buffer* pDstBuffer, UINT DstAlignedByteOffset, ID3D11UnorderedAccessView* pSrcView)
{
    bool pickRight = (m_activeEye == Eye::Right);
    m_real->CopyStructureCount(UnwrapBuf(pDstBuffer), DstAlignedByteOffset,
                               UnwrapUAVForEye(pSrcView, pickRight));
}

void STDMETHODCALLTYPE Context11Proxy::ClearUnorderedAccessViewUint(
    ID3D11UnorderedAccessView* pUnorderedAccessView, const UINT Values[4])
{
    bool pickRight = (m_activeEye == Eye::Right);
    m_real->ClearUnorderedAccessViewUint(UnwrapUAVForEye(pUnorderedAccessView, pickRight), Values);
}

void STDMETHODCALLTYPE Context11Proxy::ClearUnorderedAccessViewFloat(
    ID3D11UnorderedAccessView* pUnorderedAccessView, const FLOAT Values[4])
{
    bool pickRight = (m_activeEye == Eye::Right);
    m_real->ClearUnorderedAccessViewFloat(UnwrapUAVForEye(pUnorderedAccessView, pickRight), Values);
}

// Under duplication there is no replay pass to redo an operation for the right
// eye, so anything writing a stereo resource has to mirror into the sibling now.
static inline bool NeedsRightMirror(ID3D11Resource* pDst, bool pickRight)
{
    return gInfo.DuplicateDraws && !pickRight && pDst &&
           UnwrapResourceForEye(pDst, true) != UnwrapResourceForEye(pDst, false);
}

void STDMETHODCALLTYPE Context11Proxy::GenerateMips(ID3D11ShaderResourceView* pShaderResourceView)
{
    bool pickRight = (m_activeEye == Eye::Right);
    m_real->GenerateMips(UnwrapSRVForEye(pShaderResourceView, pickRight));
    if (gInfo.DuplicateDraws && !pickRight &&
        UnwrapSRVForEye(pShaderResourceView, true) !=
        UnwrapSRVForEye(pShaderResourceView, false))
        m_real->GenerateMips(UnwrapSRVForEye(pShaderResourceView, true));
}

void Context11Proxy::DoCopyResource(
    ID3D11Resource* pDstResource, ID3D11Resource* pSrcResource)
{
    bool pickRight = (m_activeEye == Eye::Right);
    m_real->CopyResource(UnwrapResourceForEye(pDstResource, pickRight),
                         UnwrapResourceForEye(pSrcResource, pickRight));
    if (NeedsRightMirror(pDstResource, pickRight))
        m_real->CopyResource(UnwrapResourceForEye(pDstResource, true),
                             UnwrapResourceForEye(pSrcResource, true));
}

void STDMETHODCALLTYPE Context11Proxy::CopyResource(
    ID3D11Resource* pDstResource, ID3D11Resource* pSrcResource)
{
    DoCopyResource(pDstResource, pSrcResource);
    if (!m_presentHookActive) return;
    ComRefHolder dstRef(pDstResource);
    ComRefHolder srcRef(pSrcResource);
    m_frameCommands.emplace_back(
        [this, dstRef, srcRef]()
        {
            DoCopyResource(static_cast<ID3D11Resource*>(dstRef.p),
                           static_cast<ID3D11Resource*>(srcRef.p));
        });
}

void Context11Proxy::DoCopySubresourceRegion(
    ID3D11Resource* pDstResource, UINT DstSubresource, UINT DstX, UINT DstY,
    UINT DstZ, ID3D11Resource* pSrcResource, UINT SrcSubresource,
    const D3D11_BOX* pSrcBox)
{
    bool pickRight = (m_activeEye == Eye::Right);
    m_real->CopySubresourceRegion(
        UnwrapResourceForEye(pDstResource, pickRight), DstSubresource, DstX, DstY, DstZ,
        UnwrapResourceForEye(pSrcResource, pickRight), SrcSubresource, pSrcBox);
    if (NeedsRightMirror(pDstResource, pickRight))
        m_real->CopySubresourceRegion(
            UnwrapResourceForEye(pDstResource, true), DstSubresource, DstX, DstY, DstZ,
            UnwrapResourceForEye(pSrcResource, true), SrcSubresource, pSrcBox);
}

void STDMETHODCALLTYPE Context11Proxy::CopySubresourceRegion(
    ID3D11Resource* pDstResource, UINT DstSubresource, UINT DstX, UINT DstY,
    UINT DstZ, ID3D11Resource* pSrcResource, UINT SrcSubresource,
    const D3D11_BOX* pSrcBox)
{
    DoCopySubresourceRegion(pDstResource, DstSubresource, DstX, DstY, DstZ,
                            pSrcResource, SrcSubresource, pSrcBox);
    if (!m_presentHookActive) return;
    ComRefHolder dstRef(pDstResource);
    ComRefHolder srcRef(pSrcResource);
    bool hasBox = (pSrcBox != nullptr);
    D3D11_BOX box = {};
    if (hasBox) box = *pSrcBox;
    m_frameCommands.emplace_back(
        [this, dstRef, DstSubresource, DstX, DstY, DstZ,
         srcRef, SrcSubresource, hasBox, box]()
        {
            DoCopySubresourceRegion(
                static_cast<ID3D11Resource*>(dstRef.p),
                DstSubresource, DstX, DstY, DstZ,
                static_cast<ID3D11Resource*>(srcRef.p),
                SrcSubresource, hasBox ? &box : nullptr);
        });
}

// Stage 4c: walk the captured CB bytes 64-byte (4x4 float) at a time. If the
// chunk matches the D3D row-major perspective projection pattern, apply the
// configured eye-shift to the m[2][0] element so the right eye renders with
// a horizontally-offset projection.
//
// D3D row-major perspective projection layout (HLSL default mul order):
//   m[0][0]=xScale   m[0][1]=0     m[0][2]=0       m[0][3]=0
//   m[1][0]=0        m[1][1]=yScale m[1][2]=0      m[1][3]=0
//   m[2][0]=0        m[2][1]=0     m[2][2]=zFactor m[2][3]=1
//   m[3][0]=0        m[3][1]=0     m[3][2]=zOffset m[3][3]=0
//
// As floats[0..15], we check floats[11]==1 (m[2][3]) and floats[15]==0
// (m[3][3]) — distinguishes perspective projections from identity, view,
// world, and orthographic. m[0][0] != 0 too (some xScale). When matched,
// add eyeShift to floats[8] (m[2][0]). This is the standard "horizontal
// off-axis projection" stereo trick used by iZ3D and 3DMigoto for view-
// shift without parallax errors.
// Stage 4e.2: targeted modifier. matrices[] enumerates known matrix start
// registers (matrixRegister) + their transposed flag. Each register is
// 16 bytes (4 floats). Non-transposed (HLSL row_major): matrix spans
// 4 consecutive registers row-by-row; m[2][0] is at the (register+2)*16
// byte offset, component 0. Transposed (HLSL default column_major): same
// 4 registers but stored column-by-column; m[2][0] is at register*16 + 8.
// We also need m[0][0] (xScale) to scale the shift by — at offset 0 of
// the first register in both layouts.
struct EyeShiftMatrix
{
    DWORD matrixRegister;   // register index inside CB
    BOOL  matrixIsTransposed;
    // Set for matrices declared <Inverse Value="1"/> in BaseProfile.xml: the CB
    // holds P^-1 (deferred passes reconstructing position), not P.
    BOOL  matrixIsInverse;
    // Recalled from the buffer instead of confirmed by the bound shader, so the
    // contents get a shape check before we trust them.
    BOOL  matrixFromLearned;
};

// ---------------------------------------------------------------------------
// Guard: the analyzer tells us "there is a 4x4 projection-shaped matrix at this
// register", but not whether it is the CAMERA projection. Shifting anything
// else produces right-eye-only artefacts:
//   * orthographic matrices drive HUD / UI passes — shifting them moves or
//     destroys the overlay in one eye
//   * light- and shadow-space projections drive shadow maps and light volumes
//     — shifting them lights the right eye from the wrong place
//
// The legacy DDI path already rejected both (ProjectionMatrixModifier::
// CheckMatricesTransposed / ...NonTransposed in ConstantBufferWrapper.cpp) but
// that logic never made it into the COM-wrap path, so Context11Proxy shifted
// every flagged matrix unconditionally. This is a direct port, reusing the
// same gInfo flags with the same polarity:
//
//   SkipCheckOrthoMatrix        0 (default) = perform the ortho check
//   CheckShadowMatrix           1           = reject perspective shadow maps
//   CheckExistenceInverseMatrix 1           = reject non-invertible matrices
//
// `f` is the matrix in register order, so f[0]=_11, f[3]=_14, f[12]=_41,
// f[15]=_44 — matching the legacy _RC element names used below.
// ---------------------------------------------------------------------------
// Last forward matrix we shifted, kept so the inverse detector further down can
// recognise an inverse by multiplying the two out. Diagnostic, immediate ctx.
static float s_lastForwardMatrix[16];
static bool  s_haveForwardMatrix = false;

static void DumpMatrix(const char* tag, const float* f, DWORD reg, bool transposed)
{
    DDILog("  [mat %s] reg=%u transposed=%d\n"
           "      %9.4f %9.4f %9.4f %9.4f\n"
           "      %9.4f %9.4f %9.4f %9.4f\n"
           "      %9.4f %9.4f %9.4f %9.4f\n"
           "      %9.4f %9.4f %9.4f %9.4f\n",
           tag, reg, (int)transposed,
           f[0],  f[1],  f[2],  f[3],
           f[4],  f[5],  f[6],  f[7],
           f[8],  f[9],  f[10], f[11],
           f[12], f[13], f[14], f[15]);
}

// Ring of distinct mono matrices we shifted this frame. The camera view-
// projection is among them, so a candidate inverse can be identified by
// multiplying against each and looking for identity.
static const int kFwdRing = 12;
static float s_fwdRing[kFwdRing][16];
static int   s_fwdRingCount = 0;

static void RememberForwardMatrix(const float* f)
{
    for (int i = 0; i < s_fwdRingCount; ++i)
    {
        bool same = true;
        for (int k = 0; k < 16 && same; ++k)
            if (fabsf(s_fwdRing[i][k] - f[k]) > 1e-5f) same = false;
        if (same) return;
    }
    // Roll, so the ring always holds the most recent distinct matrices and
    // tracks the camera as it moves.
    static int s_next = 0;
    memcpy(s_fwdRing[s_next], f, 16 * sizeof(float));
    s_next = (s_next + 1) % kFwdRing;
    if (s_fwdRingCount < kFwdRing) ++s_fwdRingCount;
}

// Right-eye clip-space x shift per unit w. The forward and inverse paths
// describe the same transform T, so both must use this same value.
static float EyeShiftB(float eyeShift)
{
    return eyeShift * (s_projXScale != 0.f ? s_projXScale : 1.f);
}

// 3x3 norms of the x and w bases. |w basis| is 1 for a clean view-projection
// (the camera forward axis is unit) and carries the object scale for a WVP.
static void MatrixBasisNorms(const float* f, bool transposed, float& xn, float& wn)
{
    float x0, x1, x2, w0, w1, w2;
    if (transposed) { x0 = f[0]; x1 = f[1]; x2 = f[2];  w0 = f[12]; w1 = f[13]; w2 = f[14]; }
    else            { x0 = f[0]; x1 = f[4]; x2 = f[8];  w0 = f[3];  w1 = f[7];  w2 = f[11]; }
    xn = sqrtf(x0 * x0 + x1 * x1 + x2 * x2);
    wn = sqrtf(w0 * w0 + w1 * w1 + w2 * w2);
}

// Camera _11. Both stereo terms are world-matrix independent, so _11 is the only
// per-matrix unknown -- and xn/wn only recovers it when the object scale is
// uniform. Trust the ratio when |w basis| is unit, else reuse the last matrix
// that was, so a non-uniformly scaled mesh cannot displace itself.
static float ProjXScaleFromMatrix(const float* f, bool transposed)
{
    float xn, wn;
    MatrixBasisNorms(f, transposed, xn, wn);
    if (wn <= 1e-6f) return s_projXScale;
    const float ratio = xn / wn;
    if (fabsf(wn - 1.f) < 0.05f) return ratio;
    return (s_projXScale != 0.f) ? s_projXScale : ratio;
}

// DX9 shears view space by x' = x + m31*z + m41 (BaseStereoRenderer-inl.h:141).
// eyeShift is m31, which only re-centres; m41 is the eye offset whose 1/z
// falloff is the entire depth cue. A = -B * One_div_ZPS relates the two.
static float EyeOffsetM41(float eyeShift)
{
    const CameraPreset* p = gInfo.Input.GetActivePreset();
    const float invZPS = p ? p->One_div_ZPS : 0.f;
    return (invZPS != 0.f) ? (-eyeShift / invZPS) : 0.f;
}

static bool ShouldSkipProjectionMatrix(const float* f, bool transposed)
{
    // Orthographic: no perspective divide, i.e. the row/column that produces w
    // is (0,0,0,non-zero). A camera projection puts +/-1 in one of those slots.
    if (!gInfo.SkipCheckOrthoMatrix)
    {
        const bool ortho = transposed
            ? (f[12] == 0.f && f[13] == 0.f && f[14] == 0.f && f[15] != 0.f)  // _41.._44
            : (f[3]  == 0.f && f[7]  == 0.f && f[11] == 0.f && f[15] != 0.f); // _14.._44
        if (ortho) return true;
    }

    if (gInfo.CheckShadowMatrix || gInfo.CheckExistenceInverseMatrix)
    {
        D3DXMATRIX m(f);
        D3DXMATRIX inv;
        if (!D3DXMatrixInverse(&inv, nullptr, &m))
        {
            // Singular — legacy treats "no inverse" as not-a-camera-projection.
            if (gInfo.CheckExistenceInverseMatrix) return true;
        }
        else if (gInfo.CheckShadowMatrix)
        {
            const float probe = transposed ? inv._43 : inv._34;
            if (fabsf(probe) < 0.01f) return true;   // perspective shadow map
        }
    }
    return false;
}

// outShifted / outRejected are diagnostic tallies (may be null); they feed the
// per-frame summary so we can see the guards actually firing.
static void ApplyTargetedEyeShiftToCB(unsigned char* data, size_t byteCount,
                                      float eyeShift,
                                      const std::vector<EyeShiftMatrix>& matrices,
                                      unsigned* outShifted = nullptr,
                                      unsigned* outRejected = nullptr)
{
    if (eyeShift == 0.f || matrices.empty()) return;
    constexpr size_t kRegBytes  = 16;
    constexpr size_t kMat4Bytes = 4 * kRegBytes;
    for (const auto& m : matrices)
    {
        size_t base = size_t(m.matrixRegister) * kRegBytes;
        if (base + kMat4Bytes > byteCount) continue;
        float* f = reinterpret_cast<float*>(data + base);
        const bool transposed = (m.matrixIsTransposed != 0);

        // Inverse (unprojection) matrices. The forward path is P' = P*T with
        // T = I except T._41 = b, so the corrected inverse is T^-1 * P^-1.
        // NB: none of the guards below apply — an inverse view-projection has
        // no ortho / shadow signature to test.
        if (m.matrixIsInverse)
        {
            const float s = EyeShiftB(eyeShift);
            if (transposed)
            {
                // stored = Q^T, so the update is column 4 -= s * column 1.
                f[3] -= s * f[0];   f[7]  -= s * f[4];
                f[11] -= s * f[8];  f[15] -= s * f[12];
            }
            else
            {
                // T^-1 touches row 4 only: row4 -= s * row1.
                f[12] -= s * f[0];  f[13] -= s * f[1];
                f[14] -= s * f[2];  f[15] -= s * f[3];
            }
            if (outShifted) ++*outShifted;
            continue;
        }

        float xScale = f[0];
        if (xScale == 0.f) continue;
        // A recalled target is only a guess about a buffer that may be pooled
        // and now hold something else, so require the unit w basis that every
        // real view-projection has before touching it.
        if (m.matrixFromLearned)
        {
            float lxn, lwn;
            MatrixBasisNorms(f, transposed, lxn, lwn);
            if (fabsf(lwn - 1.f) >= 0.05f)
            {
                if (outRejected) ++*outRejected;
                continue;
            }
        }
        if (ShouldSkipProjectionMatrix(f, transposed))
        {
            if (outRejected) ++*outRejected;
            if (s_matDumpRejected > 0)
            {
                --s_matDumpRejected;
                DumpMatrix("REJECTED", f, m.matrixRegister, transposed);
            }
            continue;
        }
        if (s_matDumpShifted > 0)
        {
            --s_matDumpShifted;
            DumpMatrix("shifted", f, m.matrixRegister, transposed);
            const float xs = ProjXScaleFromMatrix(f, transposed);
            DDILog("      eyeShift=%.6f xScale=%.6f b=%.6f m41=%.6f\n",
                   eyeShift, xs, eyeShift * xs, EyeOffsetM41(eyeShift) * xs);
        }
        // Snapshot the mono matrix before shifting, for the inverse detector.
        memcpy(s_lastForwardMatrix, f, sizeof(s_lastForwardMatrix));
        s_haveForwardMatrix = true;
        RememberForwardMatrix(f);

        // Cache only from clean view-projections, so neither a scaled WVP nor a
        // light projection can poison the value the rest of the frame reuses.
        float basisXn, basisWn;
        MatrixBasisNorms(f, transposed, basisXn, basisWn);
        if (basisWn > 1e-6f && fabsf(basisWn - 1.f) < 0.05f)
            s_projXScale = basisXn / basisWn;
        if (basisWn < s_wnMin) s_wnMin = basisWn;
        if (basisWn > s_wnMax) s_wnMax = basisWn;
        const float camXScale = ProjXScaleFromMatrix(f, transposed);

        if (gInfo.FullColumnEyeShift)
        {
            const float b  = eyeShift * camXScale;
            const float m41 = EyeOffsetM41(eyeShift) * camXScale;
            if (transposed)
            {
                f[0] += b * f[12];  f[1] += b * f[13];
                f[2] += b * f[14];  f[3] += b * f[15];
                f[3] += m41;        // constant on x: the 1/z parallax term
            }
            else
            {
                f[0] += b * f[3];   f[4]  += b * f[7];
                f[8] += b * f[11];  f[12] += b * f[15];
                f[12] += m41;       // constant on x: the 1/z parallax term
            }
        }
        else if (transposed)
        {
            // m[2][0] = register 0, component 2
            f[2] += eyeShift * xScale;
        }
        else
        {
            // m[2][0] = register 2, component 0
            f[8] += eyeShift * xScale;
        }
        if (outShifted) ++*outShifted;
    }
}

// A*B == I within tolerance, with either operand optionally transposed so all
// four storage conventions are covered.
static bool MultiplyIsIdentity(const float* A, const float* B, bool transA, bool transB)
{
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
        {
            float sum = 0.f;
            for (int k = 0; k < 4; ++k)
                sum += (transA ? A[k * 4 + r] : A[r * 4 + k]) *
                       (transB ? B[c * 4 + k] : B[k * 4 + c]);
            if (fabsf(sum - ((r == c) ? 1.f : 0.f)) > 2e-2f) return false;
        }
    return true;
}

// An inverse perspective projection has a distinctive zero pattern: only
// _11, _22 and the two z/w terms are populated, and that pattern is symmetric
// under transpose, so only which of the two terms is ~1 tells them apart.
static bool LooksLikeInverseProjection(const float* f, bool& outTransposed)
{
    static const int kZero[] = { 1, 2, 3, 4, 6, 7, 8, 9, 10, 12, 13 };
    for (int i = 0; i < _countof(kZero); ++i)
        if (fabsf(f[kZero[i]]) > 1e-4f) return false;
    if (fabsf(f[0]) < 1e-6f || fabsf(f[5]) < 1e-6f) return false;
    if (fabsf(f[11]) < 1e-6f || fabsf(f[14]) < 1e-6f) return false;
    outTransposed = fabsf(f[11] - 1.f) < fabsf(f[14] - 1.f);
    return true;
}

// A view-projection factors as V*P with V rigid, so in the product's last
// column the first three components are the camera forward axis: unit length,
// and orthogonal to the (scaled) right and up columns. A candidate is an
// inverse view-projection exactly when its inverse has that structure — which
// the _44==0 tests above cannot see, since (V*P)._44 is the camera-relative z
// of the world origin and is generally non-zero.
static bool InverseIsViewProjection(const float* f, bool& outTransposed)
{
    D3DXMATRIX cand(f), A;
    if (!D3DXMatrixInverse(&A, nullptr, &cand)) return false;

    for (int pass = 0; pass < 2; ++pass)
    {
        #define ELEM(r, c) (pass ? A.m[c][r] : A.m[r][c])
        const float c3[3] = { ELEM(0,3), ELEM(1,3), ELEM(2,3) };
        const float n3 = sqrtf(c3[0]*c3[0] + c3[1]*c3[1] + c3[2]*c3[2]);
        if (fabsf(n3 - 1.f) > 0.02f) continue;

        const float c0[3] = { ELEM(0,0), ELEM(1,0), ELEM(2,0) };
        const float c1[3] = { ELEM(0,1), ELEM(1,1), ELEM(2,1) };
        #undef ELEM
        const float n0 = sqrtf(c0[0]*c0[0] + c0[1]*c0[1] + c0[2]*c0[2]);
        const float n1 = sqrtf(c1[0]*c1[0] + c1[1]*c1[1] + c1[2]*c1[2]);
        if (n0 < 1e-4f || n1 < 1e-4f) continue;
        if (fabsf((c0[0]*c3[0] + c0[1]*c3[1] + c0[2]*c3[2]) / n0) > 0.02f) continue;
        if (fabsf((c1[0]*c3[0] + c1[1]*c3[1] + c1[2]*c3[2]) / n1) > 0.02f) continue;
        outTransposed = (pass == 1);
        return true;
    }
    return false;
}

// Inverse view matrix: a rigid transform, so the upper 3x3 is orthonormal and
// the last column is exactly (0,0,0,1). Its translation row is the camera
// world position, which differs per eye — a prime suspect for screen-space
// passes that reconstruct view-space position and then go to world space.
static bool LooksLikeInverseView(const float* f, bool& outTransposed, float outCamPos[3])
{
    for (int pass = 0; pass < 2; ++pass)
    {
        #define M(r, c) (pass ? f[(c) * 4 + (r)] : f[(r) * 4 + (c)])
        if (fabsf(M(0,3)) > 1e-4f || fabsf(M(1,3)) > 1e-4f ||
            fabsf(M(2,3)) > 1e-4f || fabsf(M(3,3) - 1.f) > 1e-3f) continue;

        bool ok = true;
        float row[3][3];
        for (int r = 0; r < 3 && ok; ++r)
        {
            for (int c = 0; c < 3; ++c) row[r][c] = M(r, c);
            const float n = sqrtf(row[r][0]*row[r][0] + row[r][1]*row[r][1] + row[r][2]*row[r][2]);
            if (fabsf(n - 1.f) > 0.02f) ok = false;
        }
        if (!ok) continue;
        // Mutually orthogonal rows confirm a rotation rather than a coincidence.
        if (fabsf(row[0][0]*row[1][0] + row[0][1]*row[1][1] + row[0][2]*row[1][2]) > 0.02f) continue;
        if (fabsf(row[0][0]*row[2][0] + row[0][1]*row[2][1] + row[0][2]*row[2][2]) > 0.02f) continue;

        outCamPos[0] = M(3,0); outCamPos[1] = M(3,1); outCamPos[2] = M(3,2);
        #undef M
        outTransposed = (pass == 1);
        return true;
    }
    return false;
}

// Diagnostic only: names every CB register holding such a matrix, with the
// shader CRCs and slot a BaseProfile.xml <Inverse> entry would need.
static void ReportInverseProjectionCandidates(
    const unsigned char* data, size_t byteCount,
    DWORD vsCRC, DWORD psCRC, int vsSlot, int psSlot)
{
    constexpr size_t kRegBytes = 16;
    // Proof the scan ran, so an empty result is a real negative.
    static unsigned s_scans = 0;
    if ((++s_scans % 512) == 0)
        FrameTrace("    INVSCAN buffers=%u haveFwd=%d lastSize=%u\n",
                   s_scans, (int)s_haveForwardMatrix, (unsigned)byteCount);

    for (size_t reg = 0; (reg + 4) * kRegBytes <= byteCount; ++reg)
    {
        // Matrices are laid out on 4-register boundaries; scanning every offset
        // reports misaligned windows into a neighbouring matrix as hits.
        if ((reg & 3) != 0) continue;
        const float* f = reinterpret_cast<const float*>(data + reg * kRegBytes);
        // Uninitialised fill (0xCDCDCDCD decodes to ~-4.3e8) and NaN otherwise
        // pass the inverse test as noise. Reject before testing.
        bool sane = true;
        for (int i = 0; i < 16 && sane; ++i)
            if (!(fabsf(f[i]) < 1e6f)) sane = false;
        if (!sane) continue;

        bool transposed = false;
        const char* how = nullptr;
        float camPos[3] = { 0, 0, 0 };
        if (InverseIsViewProjection(f, transposed))
        {
            how = "INVVP";
        }
        else if (LooksLikeInverseView(f, transposed, camPos) &&
                 (fabsf(camPos[0]) + fabsf(camPos[1]) + fabsf(camPos[2])) > 0.01f)
        {
            // Identity trivially satisfies the rigid test and is everywhere in
            // constant buffers, so require a real translation.
            DDILog("  INVVIEW: reg=%u transposed=%d vs=0x%08lX ps=0x%08lX"
                   " cb(vs=%d ps=%d) camPos=[%.2f %.2f %.2f]\n",
                   (unsigned)reg, (int)transposed, vsCRC, psCRC,
                   vsSlot, psSlot, camPos[0], camPos[1], camPos[2]);
            how = "INVVIEW";
        }
        else if (LooksLikeInverseProjection(f, transposed))
        {
            how = "struct";
        }
        else
        {
            // Pairing-free test: invert the candidate and ask whether the
            // result is a perspective transform. True of any inverse
            // view-projection regardless of how dense the matrix itself is.
            D3DXMATRIX cand(f), inv;
            if (D3DXMatrixInverse(&inv, nullptr, &cand))
            {
                if (fabsf(inv._44) < 1e-3f && fabsf(inv._34) > 0.5f)
                { how = "invIsProj";  transposed = false; }
                else if (fabsf(inv._44) < 1e-3f && fabsf(inv._43) > 0.5f)
                { how = "invIsProjT"; transposed = true;  }
            }
        }

        // Decisive check: does this candidate invert a matrix we actually
        // shift? If so it is the camera inverse and this is the register.
        int pairedWith = -1;
        for (int i = 0; i < s_fwdRingCount && pairedWith < 0; ++i)
        {
            if      (MultiplyIsIdentity(f, s_fwdRing[i], false, false)) { pairedWith = i; transposed = false; }
            else if (MultiplyIsIdentity(f, s_fwdRing[i], true,  false)) { pairedWith = i; transposed = true;  }
            else if (MultiplyIsIdentity(f, s_fwdRing[i], false, true))  { pairedWith = i; transposed = true;  }
            if (pairedWith >= 0) how = "PAIRED";
        }
        if (!how) continue;
        if (pairedWith >= 0)
            DDILog("  INVPAIR: reg=%u transposed=%d vs=0x%08lX ps=0x%08lX cb(vs=%d ps=%d)"
                   " inverts shifted matrix #%d\n",
                   (unsigned)reg, (int)transposed, vsCRC, psCRC, vsSlot, psSlot, pairedWith);
        FrameTrace("    INVPROJ(%s) reg=%u transposed=%d vs=0x%08lX ps=0x%08lX"
                   " vsSlot=%d psSlot=%d [%.4f %.4f %.4f %.4f]\n",
                   how, (unsigned)reg, (int)transposed, vsCRC, psCRC,
                   vsSlot, psSlot, f[0], f[5], f[11], f[14]);
    }
}

// Hand-authored matrix declarations from BaseProfile.xml, covering what the
// bytecode analyzer cannot detect — chiefly inverse view-projection matrices.
static void AddProfileMatrixTargets(const ShaderProfileDataMap& map, DWORD crc,
                                    ID3D11Buffer* const* boundCBs, UINT numSlots,
                                    ID3D11Resource* pResource,
                                    std::vector<EyeShiftMatrix>& targets)
{
    if (!crc) return;
    ShaderProfileDataMap::const_iterator it = map.find(crc);
    if (it == map.end() || !it->second.m_pMatrices) return;
    const ShaderMatrices* sm = it->second.m_pMatrices;
    for (BYTE i = 0; i < sm->matrixSize; ++i)
    {
        const ShaderMatrixData& md = sm->matrix[i];
        if (md.incorrectProjection) continue;
        if (md.constantBuffer >= numSlots) continue;
        static unsigned s_miss = 0, s_hit = 0;
        if (boundCBs[md.constantBuffer] != pResource)
        {
            if (s_miss++ < 8)
                DDILog("  Profile: crc=0x%08lX declares cb=%u but that slot holds a different buffer\n",
                       crc, (unsigned)md.constantBuffer);
            continue;
        }
        if (s_hit++ < 8)
            DDILog("  Profile HIT: crc=0x%08lX cb=%u reg=%u transposed=%d inverse=%d\n",
                   crc, (unsigned)md.constantBuffer, (unsigned)md.matrixRegister,
                   (int)md.matrixIsTransposed, (int)md.inverse);
        EyeShiftMatrix em;
        em.matrixRegister     = md.matrixRegister;
        em.matrixIsTransposed = md.matrixIsTransposed;
        em.matrixIsInverse    = md.inverse;
        targets.push_back(em);
    }
}

static void ApplyEyeShiftToCB(unsigned char* data, size_t byteCount, float eyeShift)
{
    if (eyeShift == 0.f) return;
    constexpr size_t kMat4Bytes = 16 * sizeof(float);
    if (byteCount < kMat4Bytes) return;
    for (size_t off = 0; off + kMat4Bytes <= byteCount; off += 4)
    {
        float* f = reinterpret_cast<float*>(data + off);
        // Perspective projection pattern: m[2][3]==1, m[3][3]==0,
        // m[0][0]!=0, m[1][1]!=0. The xScale / yScale checks reject
        // matrices that happen to have 1/0 in those slots for unrelated
        // reasons (lookups, bone weights, etc).
        if (f[11] != 1.f) continue;
        if (f[15] != 0.f) continue;
        if (f[0]  == 0.f) continue;
        if (f[5]  == 0.f) continue;
        // Hit — shift m[2][0]. Scaled by xScale so the shift magnitude
        // is proportional to the projected coord range (otherwise high-
        // FOV games get too much shift, narrow-FOV games too little).
        f[8] += eyeShift * f[0];
    }
}

// Upper bound on a single dynamic vertex/index buffer snapshot. Chosen to
// comfortably cover UI/text/particle batch buffers (tens of KB) while refusing
// to shadow-copy multi-megabyte streaming buffers many times per frame.
static constexpr UINT kMaxDynamicBufferReplayBytes = 4u * 1024u * 1024u;

HRESULT STDMETHODCALLTYPE Context11Proxy::Map(
    ID3D11Resource* pResource, UINT Subresource, D3D11_MAP MapType, UINT MapFlags,
    D3D11_MAPPED_SUBRESOURCE* pMappedResource)
{
    // Stage 3c.1: unwrap either texture or buffer proxies before forwarding.
    Buffer11Proxy*    buf = TryUnwrapBuffer(pResource);
    // Must cover Texture1D/3D too: hand-rolling Tex2D+Buffer only passed our
    // own proxy pointer through as if it were a real resource, and d3d11
    // dereferenced it (GTA V mapping a volume texture / LUT).
    ID3D11Resource*   realRes = UnwrapResourceForEye(pResource, false);
    HRESULT hr = m_real->Map(realRes, Subresource, MapType, MapFlags, pMappedResource);
    if (FAILED(hr) || !pMappedResource) return hr;
    // Duplication needs CB capture too, and never arms m_presentHookActive.
    if (!m_presentHookActive && !gInfo.DuplicateDraws) return hr;
    if (!gInfo.UseCOMWrapReplay) return hr;

    // Stage 4c: record write maps on CONSTANT BUFFERS. Stage 4c.1 additionally
    // requires the buffer to have been ever bound through a vertex-pipeline
    // stage — the IsVSBound tag is set on Buffer11Proxy by *SetConstantBuffers
    // when its stage is VS/GS/HS/DS.
    //
    // Stage 4f: also record VERTEX and INDEX buffer writes. The replay re-issues
    // the frame's draws at Present time, long after the game has finished
    // writing. A buffer the game refills between draws — the classic
    // map(WRITE_DISCARD) / fill / unmap / draw loop that HUD, text and particle
    // batches use — therefore holds only its FINAL contents by then, so every
    // replayed draw that reads it gets the last batch's geometry instead of its
    // own. Symptom in Max Payne 3: ~50 tail-of-frame HUD draws replay correctly
    // (right sibling bound, correct vertex counts, all at start=0) and render
    // nothing, because the vertex data underneath them is stale. Static buffers
    // are unaffected — nothing rewrites them mid-frame — which is why world
    // geometry survived the replay and the HUD did not.
    if (MapType != D3D11_MAP_WRITE_DISCARD &&
        MapType != D3D11_MAP_WRITE &&
        MapType != D3D11_MAP_WRITE_NO_OVERWRITE &&
        MapType != D3D11_MAP_READ_WRITE)
        return hr;
    if (!buf) return hr;

    D3D11_BUFFER_DESC desc;
    buf->GetReal()->GetDesc(&desc);
    if (desc.ByteWidth == 0) return hr;

    const bool isCB = (desc.BindFlags & D3D11_BIND_CONSTANT_BUFFER) != 0;
    const bool isGeom = (desc.BindFlags & (D3D11_BIND_VERTEX_BUFFER |
                                           D3D11_BIND_INDEX_BUFFER)) != 0;
    if (isCB)
    {
        // Under duplication every CB write must reach the sibling even when it
        // needs no eye shift, or the right eye reads stale constants.
        if (!gInfo.DuplicateDraws && !buf->IsVSBound()) return hr;
    }
    else if (isGeom)
    {
        // Draw duplication re-issues each draw while its geometry is still
        // live, so there is nothing to snapshot and nothing to go stale.
        if (gInfo.DuplicateDraws) return hr;
        if (!gInfo.ReplayDynamicBuffers) return hr;
        // WRITE_DISCARD only, and the restriction is a correctness requirement
        // rather than a heuristic. Our replay rewrites the whole buffer, which
        // is safe under DISCARD because the driver renames the allocation: the
        // left eye's already-queued draws keep reading the memory they were
        // issued against. WRITE_NO_OVERWRITE carries the opposite contract —
        // the app guarantees it only touches ranges no pending draw is using,
        // so the driver returns the same memory. Replaying a whole-buffer copy
        // under that map type overwrites the ranges the left eye's queued draws
        // are about to read, corrupting the left image while the right stays
        // correct. We cannot tell which sub-range the game actually wrote, so
        // the only sound option is to leave these alone.
        if (MapType != D3D11_MAP_WRITE_DISCARD)
        {
            ++m_dynBufSkippedThisFrame;
            return hr;
        }
        // Snapshotting costs a full ByteWidth copy per Unmap: WRITE_DISCARD
        // leaves the whole buffer undefined, so we cannot know which prefix the
        // game actually wrote and must take all of it. Streaming buffers can be
        // many MB and get refilled dozens of times a frame, so cap it. Over the
        // cap we skip the snapshot and tally it — those draws still replay,
        // just from stale geometry, i.e. the old behaviour.
        if (desc.ByteWidth > kMaxDynamicBufferReplayBytes)
        {
            ++m_dynBufSkippedThisFrame;
            return hr;
        }
    }
    else
    {
        return hr;
    }

    ActiveMap am;
    am.resource         = pResource;
    am.subresource      = Subresource;
    am.mapType          = MapType;
    am.mappedData       = pMappedResource->pData;
    am.byteWidth        = desc.ByteWidth;
    am.isConstantBuffer = isCB;
    m_activeMaps.push_back(am);
    return hr;
}

void STDMETHODCALLTYPE Context11Proxy::Unmap(ID3D11Resource* pResource, UINT Subresource)
{
    Buffer11Proxy*    buf = TryUnwrapBuffer(pResource);
    // Same Texture1D/3D gap as Map above.
    ID3D11Resource*   realRes = UnwrapResourceForEye(pResource, false);

    // Stage 4c: if Map captured this CB write, snapshot the bytes BEFORE
    // forwarding Unmap (which invalidates the mapped pointer), then push a
    // closure that re-maps + memcpy + applies the eye-shift heuristic +
    // unmaps at replay time. The closure only fires its modify-and-write
    // path on the right-eye pass (m_activeEye == Eye::Right) — the left
    // eye is the direct path the game just did, no replay needed.
    for (auto it = m_activeMaps.begin(); it != m_activeMaps.end(); ++it)
    {
        if (it->resource != pResource || it->subresource != Subresource) continue;
        if (it->mappedData && it->byteWidth)
        {
            std::vector<unsigned char> bytes(it->byteWidth);
            memcpy(bytes.data(), it->mappedData, it->byteWidth);
            UINT subres = it->subresource;
            D3D11_MAP mapType = it->mapType;
            ComRefHolder resRef(pResource);

            // Stage 4f: geometry buffers replay the write byte-for-byte. No
            // analyzer lookup and no eye shift — the stereo offset belongs in
            // the projection matrix only; displacing vertices as well would
            // apply it twice.
            if (!it->isConstantBuffer)
            {
                ++m_dynBufReplaysThisFrame;
                m_frameCommands.emplace_back(
                    [this, resRef, subres, bytes, mapType]()
                    {
                        if (m_activeEye != Eye::Right) return;
                        auto* gameRes = static_cast<ID3D11Resource*>(resRef.p);
                        ID3D11Resource* real = UnwrapResourceForEye(gameRes, false);
                        D3D11_MAPPED_SUBRESOURCE mapped = {};
                        if (FAILED(m_real->Map(real, subres, mapType, 0, &mapped))
                            || !mapped.pData) return;
                        memcpy(mapped.pData, bytes.data(), bytes.size());
                        m_real->Unmap(real, subres);
                    });
                m_activeMaps.erase(it);
                break;
            }

            // Stage 4e.2: consult the analyzer for the currently bound VS.
            // If the bound shader has known projection matrices at any VS-CB
            // slot where this buffer is bound, build a targeted matrix list.
            // Empty list ⇒ fall back to the m[2][3]==1 / m[3][3]==0 heuristic.
            std::vector<EyeShiftMatrix> targets;
            // analyzerKnows: the analyzer successfully parsed the bound VS, so
            // an empty `targets` is a positive "this buffer holds no projection
            // matrix" rather than "we have no idea".
            bool analyzerKnows = false;
            if (m_boundVS && m_parent)
            {
                const ShaderAnalysis11Result* info =
                    m_parent->LookupShaderProjection(m_boundVS);
                if (info && info->parsed)
                {
                    analyzerKnows = true;
                    for (UINT slot = 0; slot < kMaxVSCBSlots; ++slot)
                    {
                        if (m_boundVSCBs[slot] != pResource) continue;
                        auto cbIt = info->projection.matrixData.cb.find(slot);
                        if (cbIt == info->projection.matrixData.cb.end()) continue;
                        for (const auto& pmd : cbIt->second)
                        {
                            if (pmd.incorrectProjection) continue;
                            EyeShiftMatrix em;
                            em.matrixRegister     = pmd.matrixRegister;
                            em.matrixIsTransposed = pmd.matrixIsTransposed;
                            em.matrixIsInverse    = FALSE;
                            targets.push_back(em);
                        }
                    }
                }
            }

            // Profile-declared matrices, added on top of whatever the analyzer
            // found. A profile hit also counts as knowing this buffer.
            // One-time proof that BaseProfile.xml actually loaded for this exe.
            static bool s_profileLogged = false;
            if (!s_profileLogged)
            {
                s_profileLogged = true;
                DDILog("  Profile data loaded: VS=%u PS=%u GS=%u entries (profile='%S')\n",
                       (unsigned)g_ProfileData.VSCRCData.size(),
                       (unsigned)g_ProfileData.PSCRCData.size(),
                       (unsigned)g_ProfileData.GSCRCData.size(),
                       gInfo.ProfileName);
            }

            size_t beforeProfile = targets.size();
            AddProfileMatrixTargets(g_ProfileData.VSCRCData,
                                    BoundShaderCRC(m_parent, m_boundVS),
                                    m_boundVSCBs, kMaxVSCBSlots, pResource, targets);
            AddProfileMatrixTargets(g_ProfileData.PSCRCData,
                                    BoundShaderCRC(m_parent, m_boundPS),
                                    m_boundPSCBs, kMaxPSCBSlots, pResource, targets);
            if (targets.size() != beforeProfile) analyzerKnows = true;

            // Targets above come from the shader bound right now, but a CB is
            // routinely written before its consumer is bound and those writes
            // reached the right eye unshifted -- objects pinned at screen depth
            // while their surroundings had parallax. Remember per buffer.
            if (Buffer11Proxy* lb = TryUnwrapBuffer(pResource))
            {
                if (!targets.empty())
                {
                    for (size_t ti = 0; ti < targets.size(); ++ti)
                    {
                        Buffer11Proxy::MatrixTarget mt = { targets[ti].matrixRegister,
                                                           targets[ti].matrixIsTransposed,
                                                           targets[ti].matrixIsInverse };
                        lb->LearnMatrix(mt);
                    }
                }
                else
                {
                    const std::vector<Buffer11Proxy::MatrixTarget>& mem = lb->LearnedMatrices();
                    for (size_t ti = 0; ti < mem.size(); ++ti)
                    {
                        EyeShiftMatrix e = { mem[ti].reg, mem[ti].transposed, mem[ti].inverse, TRUE };
                        targets.push_back(e);
                    }
                    if (!targets.empty()) analyzerKnows = true;
                }
            }

            if (FrameTraceActive())
            {
                // Seed the ring from this buffer's own forward matrices first:
                // a camera CB usually holds both the view-projection and its
                // inverse, and the pairing test needs this frame's values.
                for (size_t ti = 0; ti < targets.size(); ++ti)
                {
                    if (targets[ti].matrixIsInverse) continue;
                    size_t off = size_t(targets[ti].matrixRegister) * 16;
                    if (off + 64 <= bytes.size())
                        RememberForwardMatrix(
                            reinterpret_cast<const float*>(bytes.data() + off));
                }
                int vsSlot = -1, psSlot = -1;
                for (UINT s = 0; s < kMaxVSCBSlots; ++s)
                    if (m_boundVSCBs[s] == pResource) { vsSlot = (int)s; break; }
                for (UINT s = 0; s < kMaxPSCBSlots; ++s)
                    if (m_boundPSCBs[s] == pResource) { psSlot = (int)s; break; }
                // Scanned regardless of current binding: games commonly write
                // a CB before binding it, so slot may legitimately be unknown.
                ReportInverseProjectionCandidates(
                        bytes.data(), bytes.size(),
                        BoundShaderCRC(m_parent, m_boundVS),
                        BoundShaderCRC(m_parent, m_boundPS), vsSlot, psSlot);
            }

            // Draw duplication keeps the two eyes' constants in separate
            // buffers, so the patch lands in the sibling now rather than in a
            // replay closure that rewrites one shared buffer later.
            if (gInfo.DuplicateDraws)
            {
                Buffer11Proxy* bp = TryUnwrapBuffer(pResource);
                ID3D11Buffer* rightBuf = bp ? bp->GetRealRight() : nullptr;
                if (rightBuf)
                {
                    const bool useBlindNow = targets.empty() &&
                        !(gInfo.DisableBlindCBScan && analyzerKnows);
                    if      (!targets.empty()) ++m_cbTargetedThisFrame;
                    else if (useBlindNow)      ++m_cbBlindThisFrame;
                    else                       ++m_cbSkippedThisFrame;

                    D3D11_MAPPED_SUBRESOURCE mapped = {};
                    if (SUCCEEDED(m_real->Map(rightBuf, subres, mapType, 0, &mapped))
                        && mapped.pData)
                    {
                        memcpy(mapped.pData, bytes.data(), bytes.size());
                        float eyeShift = wiz3D_GetEffectiveEyeShift();
                        if (!targets.empty())
                            ApplyTargetedEyeShiftToCB(
                                static_cast<unsigned char*>(mapped.pData),
                                bytes.size(), eyeShift, targets,
                                &m_cbMatShiftedThisFrame, &m_cbMatRejectedThisFrame);
                        else if (useBlindNow)
                            ApplyEyeShiftToCB(static_cast<unsigned char*>(mapped.pData),
                                              bytes.size(), eyeShift);
                        m_real->Unmap(rightBuf, subres);
                    }
                }
                m_activeMaps.erase(it);
                break;
            }

            // Patch policy. With DisableBlindCBScan set we never guess: a
            // buffer the analyzer has cleared, or one written under a shader we
            // could not parse, is copied through to the right eye untouched.
            // Legacy behaviour (flag clear) always falls back to the heuristic
            // whole-buffer scan whenever `targets` is empty.
            const bool useBlind = targets.empty() &&
                                  !(gInfo.DisableBlindCBScan && analyzerKnows);
            if      (!targets.empty()) ++m_cbTargetedThisFrame;
            else if (useBlind)         ++m_cbBlindThisFrame;
            else                       ++m_cbSkippedThisFrame;

            m_frameCommands.emplace_back(
                [this, resRef, subres, bytes, mapType, targets, useBlind]()
                {
                    if (m_activeEye != Eye::Right) return;
                    auto* gameRes = static_cast<ID3D11Resource*>(resRef.p);
                    ID3D11Resource* real = UnwrapResourceForEye(gameRes, false);
                    D3D11_MAPPED_SUBRESOURCE mapped = {};
                    if (FAILED(m_real->Map(real, subres, mapType, 0, &mapped))
                        || !mapped.pData) return;
                    // The right eye always gets the game's bytes; only the
                    // stereo shift on top of them is policy-dependent.
                    memcpy(mapped.pData, bytes.data(), bytes.size());
                    float eyeShift = wiz3D_GetEffectiveEyeShift();
                    if (!targets.empty())
                    {
                        ApplyTargetedEyeShiftToCB(
                            static_cast<unsigned char*>(mapped.pData),
                            bytes.size(), eyeShift, targets,
                            &m_cbMatShiftedThisFrame, &m_cbMatRejectedThisFrame);
                    }
                    else if (useBlind)
                    {
                        ApplyEyeShiftToCB(static_cast<unsigned char*>(mapped.pData),
                                          bytes.size(), eyeShift);
                    }
                    // else: analyzer cleared this buffer — copy through unshifted.
                    m_real->Unmap(real, subres);
                });
        }
        m_activeMaps.erase(it);
        break;
    }

    m_real->Unmap(realRes, Subresource);
}

void Context11Proxy::DoUpdateSubresource(
    ID3D11Resource* pDstResource, UINT DstSubresource, const D3D11_BOX* pDstBox,
    const void* pSrcData, UINT SrcRowPitch, UINT SrcDepthPitch)
{
    bool pickRight = (m_activeEye == Eye::Right);
    m_real->UpdateSubresource(UnwrapResourceForEye(pDstResource, pickRight),
                              DstSubresource, pDstBox,
                              pSrcData, SrcRowPitch, SrcDepthPitch);

    if (NeedsRightMirror(pDstResource, pickRight))
        m_real->UpdateSubresource(UnwrapResourceForEye(pDstResource, true),
                                  DstSubresource, pDstBox,
                                  pSrcData, SrcRowPitch, SrcDepthPitch);

    // Buffers have no stereo sibling in UnwrapResourceForEye, so CBs filled by
    // UpdateSubresource rather than Map need mirroring explicitly.
    if (gInfo.DuplicateDraws)
    {
        Buffer11Proxy* bp = TryUnwrapBuffer(pDstResource);
        if (bp && bp->GetRealRight())
            m_real->UpdateSubresource(bp->GetRealRight(), DstSubresource, pDstBox,
                                      pSrcData, SrcRowPitch, SrcDepthPitch);
    }
}

// True for the BC1..BC7 block-compressed format families. UpdateSubresource on
// BC textures sends 4x4-block rows, so byte counts are (height/4) * rowPitch
// not height * rowPitch.
static bool IsBCFormat(DXGI_FORMAT f)
{
    return (f >= DXGI_FORMAT_BC1_TYPELESS && f <= DXGI_FORMAT_BC5_SNORM)
        || (f >= DXGI_FORMAT_BC6H_TYPELESS && f <= DXGI_FORMAT_BC7_UNORM_SRGB);
}

// Compute how many source bytes UpdateSubresource will read so we can capture
// the right amount for replay. Returns 0 if uncomputable — caller should skip
// recording in that case. The previous Stage-4d attempt to record this call
// crashed Batman because it under-sized pSrcData; this version queries the
// destination resource's actual dimensions before copying.
static SIZE_T ComputeUpdateSubresourceCopyBytes(
    ID3D11Resource* res, UINT subresource, const D3D11_BOX* box,
    UINT srcRowPitch, UINT srcDepthPitch)
{
    if (!res) return 0;
    D3D11_RESOURCE_DIMENSION type;
    res->GetType(&type);
    switch (type)
    {
        case D3D11_RESOURCE_DIMENSION_TEXTURE2D:
        {
            D3D11_TEXTURE2D_DESC desc;
            static_cast<ID3D11Texture2D*>(res)->GetDesc(&desc);
            UINT mips = desc.MipLevels ? desc.MipLevels : 1;
            UINT mip = subresource % mips;
            UINT mipHeight = desc.Height >> mip;
            if (mipHeight == 0) mipHeight = 1;
            UINT height = box ? (box->bottom - box->top) : mipHeight;
            UINT rows = IsBCFormat(desc.Format) ? (height + 3) / 4 : height;
            return SIZE_T(rows) * srcRowPitch;
        }
        case D3D11_RESOURCE_DIMENSION_TEXTURE3D:
        {
            D3D11_TEXTURE3D_DESC desc;
            static_cast<ID3D11Texture3D*>(res)->GetDesc(&desc);
            UINT mips = desc.MipLevels ? desc.MipLevels : 1;
            UINT mip = subresource % mips;
            UINT mipDepth = desc.Depth >> mip;
            if (mipDepth == 0) mipDepth = 1;
            UINT depth = box ? (box->back - box->front) : mipDepth;
            return SIZE_T(depth) * srcDepthPitch;
        }
        case D3D11_RESOURCE_DIMENSION_TEXTURE1D:
            return srcRowPitch;
        case D3D11_RESOURCE_DIMENSION_BUFFER:
        {
            if (box) return box->right - box->left;
            D3D11_BUFFER_DESC desc;
            static_cast<ID3D11Buffer*>(res)->GetDesc(&desc);
            return desc.ByteWidth;
        }
        default:
            return 0;
    }
}

void STDMETHODCALLTYPE Context11Proxy::UpdateSubresource(
    ID3D11Resource* pDstResource, UINT DstSubresource, const D3D11_BOX* pDstBox,
    const void* pSrcData, UINT SrcRowPitch, UINT SrcDepthPitch)
{
    // Live call — applies to the active-eye sibling (left when called from
    // the game directly; right when called from replay).
    DoUpdateSubresource(pDstResource, DstSubresource, pDstBox,
                        pSrcData, SrcRowPitch, SrcDepthPitch);
    if (!m_presentHookActive) return;
    if (!pSrcData) return;

    // Only worth recording for replay when the destination is a stereo-doubled
    // Texture2D — that's the only resource kind where the right-eye sibling
    // diverges from the left. Texture1D/3D/Buffer have a single backing real,
    // so the live call already covered both eyes; recording would just bloat
    // memory and pay another driver-side upload cost for no visible benefit.
    Texture2D11Proxy* tex2d = TryUnwrapTexture2D(pDstResource);
    if (!tex2d || !tex2d->IsStereo()) return;

    // Use the wrapped resource's real (left) handle for sizing — both eye
    // siblings have identical descriptors by construction.
    SIZE_T bytes = ComputeUpdateSubresourceCopyBytes(
        static_cast<ID3D11Resource*>(tex2d->GetReal()),
        DstSubresource, pDstBox, SrcRowPitch, SrcDepthPitch);
    if (bytes == 0) return;

    std::vector<BYTE> data(bytes);
    memcpy(data.data(), pSrcData, bytes);

    ComRefHolder dstRef(pDstResource);
    bool hasBox = (pDstBox != nullptr);
    D3D11_BOX box = {};
    if (hasBox) box = *pDstBox;

    m_frameCommands.emplace_back(
        [this, dstRef, DstSubresource, hasBox, box, data, SrcRowPitch, SrcDepthPitch]()
        {
            DoUpdateSubresource(
                static_cast<ID3D11Resource*>(dstRef.p),
                DstSubresource,
                hasBox ? &box : nullptr,
                data.data(), SrcRowPitch, SrcDepthPitch);
        });
}

// ClearState resets every pipeline slot to defaults (shaders, SRVs, RTs,
// viewports, blend/depth/raster state, etc). Without recording, the right-eye
// replay sees subsequent state-setters applied on top of whatever state was
// in effect from the previous frame's replay tail — so eye-aware bindings
// can leak across the ClearState boundary. Closure has no captures; the real
// runtime handles the reset itself when replayed.
void STDMETHODCALLTYPE Context11Proxy::ClearState()
{
    m_real->ClearState();
    ResetEyeTracking();
    // ClearState unbinds the shader too, so the modified-VS state is stale until
    // the game sets a shader again.
    m_modVSShader = nullptr;
    if (!m_presentHookActive) return;
    m_frameCommands.emplace_back(
        [this]() { m_real->ClearState(); });
}

// ExecuteCommandList submits a previously-built command list (typically from
// a deferred context). Without recording, deferred-context games (some
// DA2-era engines) submit entire batches of state + draws via this single
// call and the right-eye replay never sees them. Command lists are
// idempotent at the API level; replaying the same call is safe.
void STDMETHODCALLTYPE Context11Proxy::ExecuteCommandList(
    ID3D11CommandList* pCommandList, BOOL RestoreContextState)
{
    // NOTE (Aug 2026): replaying this call re-submits a command list whose
    // resource bindings were baked at RECORD time on a deferred context we
    // never wrapped — i.e. bound to LEFT-eye reals. So the replay redraws the
    // left eye rather than filling the right. Counted here so the per-frame
    // summary can tell us whether MP3 actually takes this path.
    ++m_cmdListsThisFrame;
    m_real->ExecuteCommandList(pCommandList, RestoreContextState);
    if (!m_presentHookActive) return;
    ComRefHolder cmdRef(pCommandList);
    m_frameCommands.emplace_back(
        [this, cmdRef, RestoreContextState]()
        {
            m_real->ExecuteCommandList(
                static_cast<ID3D11CommandList*>(cmdRef.p), RestoreContextState);
        });
}

void Context11Proxy::DoResolveSubresource(
    ID3D11Resource* pDstResource, UINT DstSubresource,
    ID3D11Resource* pSrcResource, UINT SrcSubresource, DXGI_FORMAT Format)
{
    bool pickRight = (m_activeEye == Eye::Right);
    m_real->ResolveSubresource(
        UnwrapResourceForEye(pDstResource, pickRight), DstSubresource,
        UnwrapResourceForEye(pSrcResource, pickRight), SrcSubresource, Format);
    if (NeedsRightMirror(pDstResource, pickRight))
        m_real->ResolveSubresource(
            UnwrapResourceForEye(pDstResource, true), DstSubresource,
            UnwrapResourceForEye(pSrcResource, true), SrcSubresource, Format);
}

void STDMETHODCALLTYPE Context11Proxy::ResolveSubresource(
    ID3D11Resource* pDstResource, UINT DstSubresource,
    ID3D11Resource* pSrcResource, UINT SrcSubresource, DXGI_FORMAT Format)
{
    DoResolveSubresource(pDstResource, DstSubresource,
                         pSrcResource, SrcSubresource, Format);
    if (!m_presentHookActive) return;
    ComRefHolder dstRef(pDstResource);
    ComRefHolder srcRef(pSrcResource);
    m_frameCommands.emplace_back(
        [this, dstRef, DstSubresource, srcRef, SrcSubresource, Format]()
        {
            DoResolveSubresource(
                static_cast<ID3D11Resource*>(dstRef.p), DstSubresource,
                static_cast<ID3D11Resource*>(srcRef.p), SrcSubresource, Format);
        });
}

void Context11Proxy::DoClearRenderTargetView(
    ID3D11RenderTargetView* pRenderTargetView, const FLOAT ColorRGBA[4])
{
    bool pickRight = (m_activeEye == Eye::Right);
    RTV11Proxy* rtv = TryUnwrapRTV(pRenderTargetView);
    ID3D11RenderTargetView* real = pRenderTargetView;
    if (rtv)
    {
        ID3D11RenderTargetView* right = rtv->GetRealRight();
        real = (pickRight && right) ? right : rtv->GetReal();
    }
    if (FrameTraceActive())
    {
        FrameTrace("  Clear eye=%c rtv={proxy=%p stereo=%d sibling=%c real=%p} color=[%.2f %.2f %.2f %.2f]\n",
                   pickRight ? 'R' : 'L',
                   rtv, rtv ? rtv->GetRealRight() != nullptr : 0,
                   (pickRight && rtv && rtv->GetRealRight()) ? 'R' : 'L',
                   real,
                   ColorRGBA ? ColorRGBA[0] : 0.f, ColorRGBA ? ColorRGBA[1] : 0.f,
                   ColorRGBA ? ColorRGBA[2] : 0.f, ColorRGBA ? ColorRGBA[3] : 0.f);
    }
    m_real->ClearRenderTargetView(real, ColorRGBA);
    // No replay pass to clear the sibling later, so do it now.
    if (gInfo.DuplicateDraws && rtv && rtv->GetRealRight() && !pickRight)
        m_real->ClearRenderTargetView(rtv->GetRealRight(), ColorRGBA);
}

void STDMETHODCALLTYPE Context11Proxy::ClearRenderTargetView(
    ID3D11RenderTargetView* pRenderTargetView, const FLOAT ColorRGBA[4])
{
    DoClearRenderTargetView(pRenderTargetView, ColorRGBA);
    if (!m_presentHookActive) return;
    ComRefHolder rtvRef(pRenderTargetView);
    FLOAT color[4] = { 0, 0, 0, 0 };
    if (ColorRGBA)
    {
        color[0] = ColorRGBA[0]; color[1] = ColorRGBA[1];
        color[2] = ColorRGBA[2]; color[3] = ColorRGBA[3];
    }
    m_frameCommands.emplace_back(
        [this, rtvRef, color]()
        {
            DoClearRenderTargetView(
                static_cast<ID3D11RenderTargetView*>(rtvRef.p), color);
        });
}

void Context11Proxy::DoClearDepthStencilView(
    ID3D11DepthStencilView* pDepthStencilView, UINT ClearFlags, FLOAT Depth, UINT8 Stencil)
{
    bool pickRight = (m_activeEye == Eye::Right);
    DSV11Proxy* dsv = TryUnwrapDSV(pDepthStencilView);
    ID3D11DepthStencilView* real = pDepthStencilView;
    if (dsv)
    {
        ID3D11DepthStencilView* right = dsv->GetRealRight();
        real = (pickRight && right) ? right : dsv->GetReal();
    }
    m_real->ClearDepthStencilView(real, ClearFlags, Depth, Stencil);
    // No replay pass to clear the sibling later, so do it now.
    if (gInfo.DuplicateDraws && dsv && dsv->GetRealRight() && !pickRight)
        m_real->ClearDepthStencilView(dsv->GetRealRight(), ClearFlags, Depth, Stencil);
}

void STDMETHODCALLTYPE Context11Proxy::ClearDepthStencilView(
    ID3D11DepthStencilView* pDepthStencilView, UINT ClearFlags, FLOAT Depth, UINT8 Stencil)
{
    DoClearDepthStencilView(pDepthStencilView, ClearFlags, Depth, Stencil);
    if (!m_presentHookActive) return;
    ComRefHolder dsvRef(pDepthStencilView);
    m_frameCommands.emplace_back(
        [this, dsvRef, ClearFlags, Depth, Stencil]()
        {
            DoClearDepthStencilView(
                static_cast<ID3D11DepthStencilView*>(dsvRef.p),
                ClearFlags, Depth, Stencil);
        });
}

// Stage 4b.4 Group C: remaining state setters. Same record-and-replay pattern
// as the macro-generated groups above, but each has a slightly different
// argument shape so they're written out individually. All gated on
// m_presentHookActive so recording is bounded.

void STDMETHODCALLTYPE Context11Proxy::IASetInputLayout(ID3D11InputLayout* pInputLayout)
{
    m_real->IASetInputLayout(pInputLayout);
    if (!m_presentHookActive) return;
    ComRefHolder layoutRef(pInputLayout);
    m_frameCommands.emplace_back(
        [this, layoutRef]()
        {
            m_real->IASetInputLayout(static_cast<ID3D11InputLayout*>(layoutRef.p));
        });
}

void STDMETHODCALLTYPE Context11Proxy::IASetVertexBuffers(
    UINT StartSlot, UINT NumBuffers, ID3D11Buffer* const* ppVertexBuffers,
    const UINT* pStrides, const UINT* pOffsets)
{
    // Stage 3c.1: unwrap each VB before forwarding to D3D11.
    ID3D11Buffer* rawVBs[kMaxVBs] = { 0 };
    UINT cap = NumBuffers <= kMaxVBs ? NumBuffers : kMaxVBs;
    for (UINT i = 0; i < cap; ++i)
        rawVBs[i] = ppVertexBuffers ? UnwrapBuf(ppVertexBuffers[i]) : nullptr;
    m_real->IASetVertexBuffers(StartSlot, NumBuffers,
        ppVertexBuffers ? rawVBs : nullptr, pStrides, pOffsets);
    if (!m_presentHookActive) return;
    std::vector<ComRefHolder> bufRefs;
    bufRefs.reserve(NumBuffers);
    for (UINT i = 0; i < NumBuffers; ++i)
        bufRefs.emplace_back(ppVertexBuffers ? ppVertexBuffers[i] : nullptr);
    std::vector<UINT> strides;
    if (pStrides) strides.assign(pStrides, pStrides + NumBuffers);
    std::vector<UINT> offsets;
    if (pOffsets) offsets.assign(pOffsets, pOffsets + NumBuffers);
    m_frameCommands.emplace_back(
        [this, StartSlot, NumBuffers, bufRefs, strides, offsets]()
        {
            ID3D11Buffer* raw[kMaxVBs] = { 0 };
            UINT replayCap = NumBuffers <= kMaxVBs ? NumBuffers : kMaxVBs;
            for (UINT i = 0; i < replayCap; ++i)
                raw[i] = UnwrapBuf(static_cast<ID3D11Buffer*>(bufRefs[i].p));
            m_real->IASetVertexBuffers(
                StartSlot, NumBuffers, raw,
                strides.empty() ? nullptr : strides.data(),
                offsets.empty() ? nullptr : offsets.data());
        });
}

void STDMETHODCALLTYPE Context11Proxy::IASetIndexBuffer(
    ID3D11Buffer* pIndexBuffer, DXGI_FORMAT Format, UINT Offset)
{
    m_real->IASetIndexBuffer(UnwrapBuf(pIndexBuffer), Format, Offset);
    if (!m_presentHookActive) return;
    ComRefHolder bufRef(pIndexBuffer);
    m_frameCommands.emplace_back(
        [this, bufRef, Format, Offset]()
        {
            m_real->IASetIndexBuffer(
                UnwrapBuf(static_cast<ID3D11Buffer*>(bufRef.p)), Format, Offset);
        });
}

void STDMETHODCALLTYPE Context11Proxy::IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY Topology)
{
    m_real->IASetPrimitiveTopology(Topology);
    if (!m_presentHookActive) return;
    m_frameCommands.emplace_back(
        [this, Topology]()
        {
            m_real->IASetPrimitiveTopology(Topology);
        });
}

void STDMETHODCALLTYPE Context11Proxy::RSSetState(ID3D11RasterizerState* pRasterizerState)
{
    m_real->RSSetState(pRasterizerState);
    if (!m_presentHookActive) return;
    ComRefHolder stateRef(pRasterizerState);
    m_frameCommands.emplace_back(
        [this, stateRef]()
        {
            m_real->RSSetState(static_cast<ID3D11RasterizerState*>(stateRef.p));
        });
}

void STDMETHODCALLTYPE Context11Proxy::RSSetScissorRects(UINT NumRects, const D3D11_RECT* pRects)
{
    m_real->RSSetScissorRects(NumRects, pRects);
    if (!m_presentHookActive) return;
    std::vector<D3D11_RECT> rects;
    if (pRects) rects.assign(pRects, pRects + NumRects);
    m_frameCommands.emplace_back(
        [this, NumRects, rects]()
        {
            m_real->RSSetScissorRects(
                NumRects, rects.empty() ? nullptr : rects.data());
        });
}

void STDMETHODCALLTYPE Context11Proxy::OMSetBlendState(
    ID3D11BlendState* pBlendState, const FLOAT BlendFactor[4], UINT SampleMask)
{
    m_real->OMSetBlendState(pBlendState, BlendFactor, SampleMask);
    if (!m_presentHookActive) return;
    ComRefHolder stateRef(pBlendState);
    FLOAT factor[4] = { 0, 0, 0, 0 };
    bool hasFactor = (BlendFactor != nullptr);
    if (hasFactor)
    {
        factor[0] = BlendFactor[0]; factor[1] = BlendFactor[1];
        factor[2] = BlendFactor[2]; factor[3] = BlendFactor[3];
    }
    m_frameCommands.emplace_back(
        [this, stateRef, factor, hasFactor, SampleMask]()
        {
            m_real->OMSetBlendState(
                static_cast<ID3D11BlendState*>(stateRef.p),
                hasFactor ? factor : nullptr, SampleMask);
        });
}

void STDMETHODCALLTYPE Context11Proxy::OMSetDepthStencilState(
    ID3D11DepthStencilState* pDepthStencilState, UINT StencilRef)
{
    m_real->OMSetDepthStencilState(pDepthStencilState, StencilRef);
    if (!m_presentHookActive) return;
    ComRefHolder stateRef(pDepthStencilState);
    m_frameCommands.emplace_back(
        [this, stateRef, StencilRef]()
        {
            m_real->OMSetDepthStencilState(
                static_cast<ID3D11DepthStencilState*>(stateRef.p), StencilRef);
        });
}

void STDMETHODCALLTYPE Context11Proxy::SOSetTargets(
    UINT NumBuffers, ID3D11Buffer* const* ppSOTargets, const UINT* pOffsets)
{
    // Stage 3c.1: unwrap each SO target before forwarding to D3D11.
    ID3D11Buffer* rawSOs[kMaxSOBuffers] = { 0 };
    UINT cap = NumBuffers <= kMaxSOBuffers ? NumBuffers : kMaxSOBuffers;
    for (UINT i = 0; i < cap; ++i)
        rawSOs[i] = ppSOTargets ? UnwrapBuf(ppSOTargets[i]) : nullptr;
    m_real->SOSetTargets(NumBuffers,
        ppSOTargets ? rawSOs : nullptr, pOffsets);
    if (!m_presentHookActive) return;
    std::vector<ComRefHolder> bufRefs;
    bufRefs.reserve(NumBuffers);
    for (UINT i = 0; i < NumBuffers; ++i)
        bufRefs.emplace_back(ppSOTargets ? ppSOTargets[i] : nullptr);
    std::vector<UINT> offsets;
    if (pOffsets) offsets.assign(pOffsets, pOffsets + NumBuffers);
    m_frameCommands.emplace_back(
        [this, NumBuffers, bufRefs, offsets]()
        {
            ID3D11Buffer* raw[kMaxSOBuffers] = { 0 };
            UINT replayCap = NumBuffers <= kMaxSOBuffers ? NumBuffers : kMaxSOBuffers;
            for (UINT i = 0; i < replayCap; ++i)
                raw[i] = UnwrapBuf(static_cast<ID3D11Buffer*>(bufRefs[i].p));
            m_real->SOSetTargets(
                NumBuffers, raw,
                offsets.empty() ? nullptr : offsets.data());
        });
}

void STDMETHODCALLTYPE Context11Proxy::SetPredication(
    ID3D11Predicate* pPredicate, BOOL PredicateValue)
{
    m_real->SetPredication(pPredicate, PredicateValue);
    if (!m_presentHookActive) return;
    ComRefHolder predRef(pPredicate);
    m_frameCommands.emplace_back(
        [this, predRef, PredicateValue]()
        {
            m_real->SetPredication(
                static_cast<ID3D11Predicate*>(predRef.p), PredicateValue);
        });
}

void STDMETHODCALLTYPE Context11Proxy::CSSetUnorderedAccessViews(
    UINT StartSlot, UINT NumUAVs,
    ID3D11UnorderedAccessView* const* ppUnorderedAccessViews,
    const UINT* pUAVInitialCounts)
{
    TrackCSUAVs(StartSlot, NumUAVs, ppUnorderedAccessViews);
    // Stage 3c.2: unwrap UAVs eye-aware before forwarding.
    ID3D11UnorderedAccessView* rawSet[kMaxUAVs] = { 0 };
    UINT setCap = NumUAVs <= kMaxUAVs ? NumUAVs : kMaxUAVs;
    bool pickRight = (m_activeEye == Eye::Right);
    for (UINT i = 0; i < setCap; ++i)
        rawSet[i] = UnwrapUAVForEye(ppUnorderedAccessViews ? ppUnorderedAccessViews[i]
                                                           : nullptr, pickRight);
    m_real->CSSetUnorderedAccessViews(StartSlot, NumUAVs,
        ppUnorderedAccessViews ? rawSet : nullptr, pUAVInitialCounts);
    if (!m_presentHookActive) return;
    std::vector<ComRefHolder> uavRefs;
    uavRefs.reserve(NumUAVs);
    for (UINT i = 0; i < NumUAVs; ++i)
        uavRefs.emplace_back(ppUnorderedAccessViews ? ppUnorderedAccessViews[i] : nullptr);
    std::vector<UINT> initialCounts;
    if (pUAVInitialCounts)
        initialCounts.assign(pUAVInitialCounts, pUAVInitialCounts + NumUAVs);
    m_frameCommands.emplace_back(
        [this, StartSlot, NumUAVs, uavRefs, initialCounts]()
        {
            ID3D11UnorderedAccessView* raw[kMaxUAVs] = { 0 };
            UINT cap = NumUAVs <= kMaxUAVs ? NumUAVs : kMaxUAVs;
            bool pr = (m_activeEye == Eye::Right);
            for (UINT i = 0; i < cap; ++i)
                raw[i] = UnwrapUAVForEye(
                    static_cast<ID3D11UnorderedAccessView*>(uavRefs[i].p), pr);
            m_real->CSSetUnorderedAccessViews(
                StartSlot, NumUAVs, raw,
                initialCounts.empty() ? nullptr : initialCounts.data());
        });
}

void STDMETHODCALLTYPE Context11Proxy::GetDevice(ID3D11Device** ppDevice)
{
    // COM identity: GetDevice must return the wrapped device, not the real
    // one — otherwise a game that round-trips through GetDevice ends up
    // bypassing our wrapper for subsequent resource creation.
    if (!ppDevice) return;
    if (m_parent)
    {
        // Device11Proxy publicly inherits from ID3D11Device — real upcast,
        // static_cast keeps /W4 + warnings-as-errors happy.
        *ppDevice = static_cast<ID3D11Device*>(m_parent);
        m_parent->AddRef();
        return;
    }
    m_real->GetDevice(ppDevice);
}

} // namespace wiz3d
