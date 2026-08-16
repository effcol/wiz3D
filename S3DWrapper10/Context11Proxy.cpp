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

// Per-resource caps on stack-allocated temp arrays used to unwrap proxy pointers
// before forwarding to the real D3D11 runtime. These MUST be sized to the D3D11
// spec maximum per resource type — undersizing makes the runtime read past our
// array into uninitialized stack memory and treat random values as pointers,
// dereference them, and AV. Caused the Metro 2033 / MP3 / De Blob right-eye
// breakage in May 2026 (a single kMaxUnwrapArray=16 was too small for SRVs and
// VBs in particular).
static constexpr UINT kMaxSRVs       = 128;  // D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT
static constexpr UINT kMaxSamplers   = 16;   // D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT
static constexpr UINT kMaxCBs        = 15;   // D3D11_1_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT (14 in plain D3D11)
static constexpr UINT kMaxRTVs       = 8;    // D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT
static constexpr UINT kMaxUAVs       = 64;   // D3D11_1_UAV_SLOT_COUNT (8 in plain D3D11)
static constexpr UINT kMaxVBs        = 32;   // D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT
static constexpr UINT kMaxSOBuffers  = 4;    // D3D11_SO_BUFFER_SLOT_COUNT
static constexpr UINT kMaxClassInst  = 253;  // D3D11_SHADER_MAX_INTERFACES

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
{
    for (UINT i = 0; i < kMaxVSCBSlots; ++i) m_boundVSCBs[i] = nullptr;
    if (m_real)
    {
        if (FAILED(m_real->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&m_real1)))) m_real1 = nullptr;
        if (FAILED(m_real->QueryInterface(__uuidof(ID3D11DeviceContext2), reinterpret_cast<void**>(&m_real2)))) m_real2 = nullptr;
        if (FAILED(m_real->QueryInterface(__uuidof(ID3D11DeviceContext3), reinterpret_cast<void**>(&m_real3)))) m_real3 = nullptr;
    }
}

Context11Proxy::~Context11Proxy()
{
    // Closures hold no AddRef'd state in 4b.1 (Draw/DrawIndexed capture only
    // POD args), so a plain clear is safe. Later stages that record state-
    // setting calls with captured COM pointers will release them in
    // ClearFrameCommands; the dtor will route there.
    ClearFrameCommands();
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
void Context11Proxy::LogAndResetFrameDrawStats()
{
    static unsigned s_frame = 0;
    ++s_frame;

    // Every 60th frame (~1s at 60fps), plus the first few so a short capture
    // still shows something.
    const bool report = (s_frame <= 3) || (s_frame % 60 == 0);
    if (report)
    {
        DDILog("  [frame %u] immediate-ctx: draws=%u dispatches=%u cmdLists=%u"
               " recorded=%zu | CB patches: targeted=%u blind=%u skipped=%u"
               " | matrices: shifted=%u guardRejected=%u"
               " | dynBuf: replayed=%u skipped=%u"
               " (DisableBlindCBScan=%d ortho=%d shadow=%d)%s\n",
               s_frame, m_drawsThisFrame, m_dispatchesThisFrame,
               m_cmdListsThisFrame, m_frameCommands.size(),
               m_cbTargetedThisFrame, m_cbBlindThisFrame, m_cbSkippedThisFrame,
               m_cbMatShiftedThisFrame, m_cbMatRejectedThisFrame,
               m_dynBufReplaysThisFrame, m_dynBufSkippedThisFrame,
               (int)gInfo.DisableBlindCBScan,
               (int)!gInfo.SkipCheckOrthoMatrix, (int)gInfo.CheckShadowMatrix,
               (m_cmdListsThisFrame > 0)
                   ? "  <-- DEFERRED CONTEXT WORK: right eye cannot be correct via replay"
                   : "");
    }

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
RECORD_CB_SET(PS, 0)
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
RECORD_SHADER_SET(PS, ID3D11PixelShader)
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
    m_real->VSSetShader(pShader, ppClassInstances, NumClassInstances);
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
#undef RECORD_SHADER_SET

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
    if (FrameTraceActive())
        FrameTrace("    Draw eye=%c vcount=%u start=%u\n",
                   m_activeEye == Eye::Right ? 'R' : 'L',
                   VertexCount, StartVertexLocation);
    m_real->Draw(VertexCount, StartVertexLocation);
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
    if (FrameTraceActive())
        FrameTrace("    DrawIndexed eye=%c icount=%u start=%u base=%d\n",
                   m_activeEye == Eye::Right ? 'R' : 'L',
                   IndexCount, StartIndexLocation, BaseVertexLocation);
    m_real->DrawIndexed(IndexCount, StartIndexLocation, BaseVertexLocation);
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
    m_real->DrawInstanced(VertexCountPerInstance, InstanceCount,
                          StartVertexLocation, StartInstanceLocation);
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
    m_real->DrawIndexedInstanced(IndexCountPerInstance, InstanceCount,
                                  StartIndexLocation, BaseVertexLocation,
                                  StartInstanceLocation);
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
    m_real->DrawAuto();
    if (!m_presentHookActive) return;
    m_frameCommands.emplace_back(
        [this]() { m_real->DrawAuto(); });
}

void STDMETHODCALLTYPE Context11Proxy::DrawInstancedIndirect(
    ID3D11Buffer* pBufferForArgs, UINT AlignedByteOffsetForArgs)
{
    ++m_drawsThisFrame;
    m_real->DrawInstancedIndirect(UnwrapBuf(pBufferForArgs), AlignedByteOffsetForArgs);
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
    m_real->DrawIndexedInstancedIndirect(UnwrapBuf(pBufferForArgs), AlignedByteOffsetForArgs);
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
    if (FrameTraceActive())
    {
        RTV11Proxy* rtv0 = (NumViews > 0 && ppRenderTargetViews)
                               ? TryUnwrapRTV(ppRenderTargetViews[0]) : nullptr;
        DSV11Proxy* dsv  = TryUnwrapDSV(pDepthStencilView);
        FrameTrace("  OMSet eye=%c NumViews=%u rtv0={proxy=%p stereo=%d right=%p} dsv={proxy=%p stereo=%d right=%p}\n",
                   pickRight ? 'R' : 'L', NumViews,
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

void STDMETHODCALLTYPE Context11Proxy::GenerateMips(ID3D11ShaderResourceView* pShaderResourceView)
{
    bool pickRight = (m_activeEye == Eye::Right);
    m_real->GenerateMips(UnwrapSRVForEye(pShaderResourceView, pickRight));
}

void Context11Proxy::DoCopyResource(
    ID3D11Resource* pDstResource, ID3D11Resource* pSrcResource)
{
    bool pickRight = (m_activeEye == Eye::Right);
    m_real->CopyResource(UnwrapResourceForEye(pDstResource, pickRight),
                         UnwrapResourceForEye(pSrcResource, pickRight));
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
        float xScale = f[0];
        if (xScale == 0.f) continue;
        const bool transposed = (m.matrixIsTransposed != 0);
        if (ShouldSkipProjectionMatrix(f, transposed))
        {
            if (outRejected) ++*outRejected;
            continue;
        }
        if (transposed)
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
    Texture2D11Proxy* tex = TryUnwrapTexture2D(pResource);
    Buffer11Proxy*    buf = TryUnwrapBuffer(pResource);
    ID3D11Resource*   realRes = tex ? static_cast<ID3D11Resource*>(tex->GetReal())
                              : buf ? static_cast<ID3D11Resource*>(buf->GetReal())
                                    : pResource;
    HRESULT hr = m_real->Map(realRes, Subresource, MapType, MapFlags, pMappedResource);
    if (FAILED(hr) || !pMappedResource) return hr;
    if (!m_presentHookActive) return hr;
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
        if (!buf->IsVSBound()) return hr;
    }
    else if (isGeom)
    {
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
    Texture2D11Proxy* tex = TryUnwrapTexture2D(pResource);
    Buffer11Proxy*    buf = TryUnwrapBuffer(pResource);
    ID3D11Resource*   realRes = tex ? static_cast<ID3D11Resource*>(tex->GetReal())
                              : buf ? static_cast<ID3D11Resource*>(buf->GetReal())
                                    : pResource;

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
                            targets.push_back(em);
                        }
                    }
                }
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
