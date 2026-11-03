/*
* Project : wiz3D
* Copyright (C) Akshay Hegde 2026
*/

// The context's methods, as DX9's BaseStereoRenderer_Modified.cpp: each ID3D11DeviceContext call unwrapped to the real
// objects, tracked per eye, and repeated or mirrored for the right eye.

#include "StdAfx.h"
#include "Context11Proxy-inl.h"
#include "ShaderHunter.h"            // 0.5.0 hunt-mode plumbing

// The right eye's indirect arguments: the args buffer's right sibling, which the right eye's dispatches write.
static inline ID3D11Buffer* RightArgs(ID3D11Buffer* p)
{
    if (auto* bp = p ? wiz3d::TryUnwrapBuffer(static_cast<ID3D11Resource*>(p)) : nullptr)
        return bp->GetRealRight() ? bp->GetRealRight() : bp->GetReal();
    return p;
}

namespace wiz3d
{

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

// D3D11.1 partial constant-buffer binds. Same unwrap and per-eye tracking as
// the 11.0 setters, with the constant range kept so BindEye can re-issue it.
#define STEREO_SET_CBS1(STAGE_PREFIX, STAGE_IDX)                                                         \
void STDMETHODCALLTYPE Context11Proxy::STAGE_PREFIX##SetConstantBuffers1(                                 \
    UINT StartSlot, UINT NumBuffers, ID3D11Buffer* const* ppConstantBuffers,                             \
    const UINT* pFirstConstant, const UINT* pNumConstants)                                               \
{                                                                                                        \
    TrackCBs(STAGE_IDX, StartSlot, NumBuffers, ppConstantBuffers, pFirstConstant, pNumConstants);        \
    ID3D11Buffer* rawCBs[kMaxCBs] = { 0 };                                                               \
    UINT cap = NumBuffers <= kMaxCBs ? NumBuffers : kMaxCBs;                                             \
    for (UINT i = 0; i < cap; ++i)                                                                       \
    {                                                                                                    \
        ID3D11Buffer* p = ppConstantBuffers ? ppConstantBuffers[i] : nullptr;                            \
        UINT slot = StartSlot + i;                                                                       \
        if (STAGE_IDX == ST_VS && slot < kMaxVSCBSlots) HoldRef(m_boundVSCBs[slot], p);                  \
        if (STAGE_IDX == ST_PS && slot < kMaxPSCBSlots) HoldRef(m_boundPSCBs[slot], p);                  \
        if (p)                                                                                           \
        {                                                                                                \
            if (auto* bp = wiz3d::TryUnwrapBuffer(static_cast<ID3D11Resource*>(p)))                      \
            {                                                                                            \
                if (STAGE_IDX != ST_PS && STAGE_IDX != ST_CS) bp->TagVSBound();                          \
                rawCBs[i] = bp->GetReal();                                                               \
            }                                                                                            \
            else rawCBs[i] = p;                                                                          \
        }                                                                                                \
    }                                                                                                    \
    if (m_real1)                                                                                         \
        m_real1->STAGE_PREFIX##SetConstantBuffers1(StartSlot, NumBuffers,                                \
            ppConstantBuffers ? rawCBs : nullptr, pFirstConstant, pNumConstants);                        \
    MIRROR1(STAGE_PREFIX##SetConstantBuffers1(StartSlot, NumBuffers,                                     \
            ppConstantBuffers ? rawCBs : nullptr, pFirstConstant, pNumConstants));                       \
}
STEREO_SET_CBS1(VS, ST_VS)
STEREO_SET_CBS1(HS, ST_HS)
STEREO_SET_CBS1(DS, ST_DS)
STEREO_SET_CBS1(GS, ST_GS)
STEREO_SET_CBS1(PS, ST_PS)
STEREO_SET_CBS1(CS, ST_CS)
#undef STEREO_SET_CBS1

// D3D11.1 resource/view entry points: each unwraps to the real object and applies to both eyes of a stereo resource;
// the 11.1 copy flags are hints, dropped so copies take the eye-aware 11.0 paths.
ID3D11View* UnwrapViewForEye(ID3D11View* p, bool pickRight)
{
    if (!p) return nullptr;
    if (auto* r = wiz3d::TryUnwrapRTV(static_cast<ID3D11RenderTargetView*>(p)))
        return (pickRight && r->GetRealRight()) ? r->GetRealRight() : r->GetReal();
    if (auto* d = wiz3d::TryUnwrapDSV(static_cast<ID3D11DepthStencilView*>(p)))
        return (pickRight && d->GetRealRight()) ? d->GetRealRight() : d->GetReal();
    if (auto* sv = wiz3d::TryUnwrapSRV(static_cast<ID3D11ShaderResourceView*>(p)))
        return (pickRight && sv->GetRealRight()) ? sv->GetRealRight() : sv->GetReal();
    if (auto* u = wiz3d::TryUnwrapUAV(static_cast<ID3D11UnorderedAccessView*>(p)))
        return (pickRight && u->GetRealRight()) ? u->GetRealRight() : u->GetReal();
    return p;
}

void STDMETHODCALLTYPE Context11Proxy::SetResourceMinLOD(ID3D11Resource* pResource, FLOAT MinLOD)
{
    FlushRightBatch(kFlushWrite);
    ID3D11Resource* l = UnwrapResourceForEye(pResource, false);
    ID3D11Resource* r = UnwrapResourceForEye(pResource, true);
    m_real->SetResourceMinLOD(l, MinLOD);
    if (r != l) m_real->SetResourceMinLOD(r, MinLOD);
}

void STDMETHODCALLTYPE Context11Proxy::CopySubresourceRegion1(
    ID3D11Resource* pDstResource, UINT DstSubresource, UINT DstX, UINT DstY, UINT DstZ,
    ID3D11Resource* pSrcResource, UINT SrcSubresource, const D3D11_BOX* pSrcBox, UINT CopyFlags)
{
    (void)CopyFlags;
    CopySubresourceRegion(pDstResource, DstSubresource, DstX, DstY, DstZ, pSrcResource, SrcSubresource, pSrcBox);
}

void STDMETHODCALLTYPE Context11Proxy::UpdateSubresource1(
    ID3D11Resource* pDstResource, UINT DstSubresource, const D3D11_BOX* pDstBox,
    const void* pSrcData, UINT SrcRowPitch, UINT SrcDepthPitch, UINT CopyFlags)
{
    (void)CopyFlags;
    UpdateSubresource(pDstResource, DstSubresource, pDstBox, pSrcData, SrcRowPitch, SrcDepthPitch);
}

void STDMETHODCALLTYPE Context11Proxy::DiscardResource(ID3D11Resource* pResource)
{
    if (!m_real1) return;
    FlushRightBatch(kFlushWrite);
    ID3D11Resource* l = UnwrapResourceForEye(pResource, false);
    ID3D11Resource* r = UnwrapResourceForEye(pResource, true);
    m_real1->DiscardResource(l);
    if (r != l) m_real1->DiscardResource(r);
}

void STDMETHODCALLTYPE Context11Proxy::DiscardView(ID3D11View* pResourceView)
{
    if (!m_real1) return;
    FlushRightBatch(kFlushWrite);
    ID3D11View* l = UnwrapViewForEye(pResourceView, false);
    ID3D11View* r = UnwrapViewForEye(pResourceView, true);
    m_real1->DiscardView(l);
    if (r != l) m_real1->DiscardView(r);
}

void STDMETHODCALLTYPE Context11Proxy::DiscardView1(ID3D11View* pResourceView, const D3D11_RECT* pRects, UINT NumRects)
{
    if (!m_real1) return;
    FlushRightBatch(kFlushWrite);
    ID3D11View* l = UnwrapViewForEye(pResourceView, false);
    ID3D11View* r = UnwrapViewForEye(pResourceView, true);
    m_real1->DiscardView1(l, pRects, NumRects);
    if (r != l) m_real1->DiscardView1(r, pRects, NumRects);
}

void STDMETHODCALLTYPE Context11Proxy::ClearView(ID3D11View* pView, const FLOAT Color[4], const D3D11_RECT* pRect, UINT NumRects)
{
    if (!m_real1) return;
    FlushRightBatch(kFlushWrite);
    ID3D11View* l = UnwrapViewForEye(pView, false);
    ID3D11View* r = UnwrapViewForEye(pView, true);
    m_real1->ClearView(l, Color, pRect, NumRects);
    if (r != l) m_real1->ClearView(r, Color, pRect, NumRects);
}

// Re-issue a draw for the right eye immediately after the game's own call.
#define DUPLICATE_DRAW(CALL)              \
    do {                                  \
        if (BeginRightEyeDraw())          \
        {                                 \
            { ScopedTicks t_(m_time.rightCalls); m_real->CALL; } \
            EndRightEyeDraw();            \
        }                                 \
        ScopedTicks d_(m_time.drawDone);  \
        for (auto& f_ : Features())       \
            f_->OnDrawDone(*this);        \
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
    MIRROR(STAGE_PREFIX##SetShaderResources(StartSlot, NumViews,                            \
        ppShaderResourceViews ? rawSRVs : nullptr));                                        \
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
    MIRROR(STAGE_PREFIX##SetSamplers(StartSlot, NumSamplers, ppSamplers));                  \
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
    MIRROR(STAGE_PREFIX##SetConstantBuffers(StartSlot, NumBuffers,                          \
        ppConstantBuffers ? rawCBs : nullptr));                                             \
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
        if (slot < kMaxVSCBSlots) HoldRef(m_boundVSCBs[slot], p);

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
    MIRROR(VSSetConstantBuffers(StartSlot, NumBuffers, ppConstantBuffers ? rawCBs : nullptr));
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
        if (slot < kMaxPSCBSlots) HoldRef(m_boundPSCBs[slot], p);

        if (p)
        {
            if (auto* bp = wiz3d::TryUnwrapBuffer(static_cast<ID3D11Resource*>(p)))
                rawCBs[i] = bp->GetReal();
            else
                rawCBs[i] = p;
        }
    }
    m_real->PSSetConstantBuffers(StartSlot, NumBuffers, ppConstantBuffers ? rawCBs : nullptr);
    MIRROR(PSSetConstantBuffers(StartSlot, NumBuffers, ppConstantBuffers ? rawCBs : nullptr));
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
    TrackStageShader(ST_##STAGE_PREFIX, pShader);                                           \
    m_real->STAGE_PREFIX##SetShader(pShader, ppClassInstances, NumClassInstances);          \
    MIRROR(STAGE_PREFIX##SetShader(pShader, ppClassInstances, NumClassInstances));          \
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

void Context11Proxy::TrackStageShader(StageIdx stage, void* shader)
{
    if (stage == ST_GS) { m_boundGS = static_cast<ID3D11GeometryShader*>(shader); return; }
    if (stage == ST_HS) { m_boundHS = static_cast<ID3D11HullShader*>(shader); return; }
    if (stage != ST_DS) return;
    m_boundDS     = static_cast<ID3D11DomainShader*>(shader);
    m_boundDSInfo = (shader && m_parent) ? m_parent->LookupShaderProjection(shader) : nullptr;
}

void STDMETHODCALLTYPE Context11Proxy::CSSetShader(
    ID3D11ComputeShader* pShader, ID3D11ClassInstance* const* ppClassInstances, UINT NumClassInstances)
{
    m_boundInfo[2] = m_parent ? m_parent->LookupShaderProjection(pShader) : nullptr;
    m_real->CSSetShader(pShader, ppClassInstances, NumClassInstances);
    MIRROR(CSSetShader(pShader, ppClassInstances, NumClassInstances));
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
            m_real->CSSetShader(
                static_cast<ID3D11ComputeShader*>(shaderRef.p),
                ciRefs.empty() ? nullptr : raw, NumClassInstances);
        });
}

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
    m_boundInfo[0] = m_parent ? m_parent->LookupShaderProjection(pShader) : nullptr;
    m_real->VSSetShader(pShader, ppClassInstances, NumClassInstances);
    MIRROR(VSSetShader(pShader, ppClassInstances, NumClassInstances));
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

// PS variant: snapshots m_boundPS so Unmap can match the bound pixel shader's CRC against BaseProfile.xml's
// <PixelShader> matrix declarations (not AddRef'd, as m_boundVS).
void STDMETHODCALLTYPE Context11Proxy::PSSetShader(
    ID3D11PixelShader* pShader, ID3D11ClassInstance* const* ppClassInstances,
    UINT NumClassInstances)
{
    m_boundPS = pShader;
    m_boundInfo[1] = m_parent ? m_parent->LookupShaderProjection(pShader) : nullptr;
    m_real->PSSetShader(pShader, ppClassInstances, NumClassInstances);
    MIRROR(PSSetShader(pShader, ppClassInstances, NumClassInstances));
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

// Stage 4b.7: record-and-replay for draw/dispatch. Pure POD captures for the
// non-Indirect variants; Indirect/Dispatch-with-buffer use ComRefHolder to
// keep the arg buffer alive across replay. The closure body is the same shape
// as the original passthrough — no Do* helpers needed since draws don't
// reference our wrapped resources directly (the bound RTV/VB/IB/CB are picked
// up from the bound pipeline state, which the *Set* closures already replay
// with eye selection).

void STDMETHODCALLTYPE Context11Proxy::Draw(UINT VertexCount, UINT StartVertexLocation)
{
    BeginDraw(VertexCount);
    const DWORD _huntVS = BoundShaderCRC(m_parent, m_boundVS);
    const DWORD _huntPS = BoundShaderCRC(m_parent, m_boundPS);
    wiz3d::ShaderHunter::NoteDraw(_huntVS, _huntPS);
    HuntMaybeCaptureFingerprint();
    // 0.5.0 ShaderHunter: symmetric skip — if the bound VS or PS matches the
    // currently hunted CRC, we drop the real left-eye draw AND skip the
    // m_frameCommands.emplace_back below, so right-eye replay sees the same
    // no-op. Keeps left/right in lockstep per the design doc risk #1.
    if (wiz3d::ShaderHunter::ShouldSkipDraw(_huntVS, _huntPS)) return;
    if (FrameTraceActive())
        FrameTrace("    Draw eye=%c vcount=%u start=%u vs=0x%08lX ps=0x%08lX\n",
                   m_activeEye == Eye::Right ? 'R' : 'L',
                   VertexCount, StartVertexLocation,
                   _huntVS, _huntPS);
    { ScopedTicks t_(m_time.gameCalls); m_real->Draw(VertexCount, StartVertexLocation); }
    DUPLICATE_DRAW(Draw(VertexCount, StartVertexLocation));
    if (!m_presentHookActive) return;
    ID3D11VertexShader* vsAtDraw = m_boundVS;   // Phase 2: snapshot for right-eye modified-VS swap
    m_frameCommands.emplace_back(
        [this, VertexCount, StartVertexLocation, vsAtDraw]()
        {
            if (FrameTraceActive())
                FrameTrace("    Draw eye=%c vcount=%u start=%u (replay)\n",
                           m_activeEye == Eye::Right ? 'R' : 'L',
                           VertexCount, StartVertexLocation);
            ID3D11VertexShader* _origVS = nullptr; UINT _slot = 0;
            const bool _swapped = BeginRightEyeShaderSwap(vsAtDraw, _origVS, _slot);
            m_real->Draw(VertexCount, StartVertexLocation);
            if (_swapped) EndRightEyeShaderSwap(_origVS);
        });
}

void STDMETHODCALLTYPE Context11Proxy::DrawIndexed(
    UINT IndexCount, UINT StartIndexLocation, INT BaseVertexLocation)
{
    BeginDraw(IndexCount);
    const DWORD _huntVS = BoundShaderCRC(m_parent, m_boundVS);
    const DWORD _huntPS = BoundShaderCRC(m_parent, m_boundPS);
    wiz3d::ShaderHunter::NoteDraw(_huntVS, _huntPS);
    HuntMaybeCaptureFingerprint();
    if (wiz3d::ShaderHunter::ShouldSkipDraw(_huntVS, _huntPS)) return;
    if (FrameTraceActive())
        FrameTrace("    DrawIndexed eye=%c icount=%u start=%u base=%d vs=0x%08lX ps=0x%08lX\n",
                   m_activeEye == Eye::Right ? 'R' : 'L',
                   IndexCount, StartIndexLocation, BaseVertexLocation,
                   _huntVS, _huntPS);
    { ScopedTicks t_(m_time.gameCalls); m_real->DrawIndexed(IndexCount, StartIndexLocation, BaseVertexLocation); }
    DUPLICATE_DRAW(DrawIndexed(IndexCount, StartIndexLocation, BaseVertexLocation));
    if (!m_presentHookActive) return;
    ID3D11VertexShader* vsAtDraw = m_boundVS;   // Phase 2: snapshot for right-eye modified-VS swap
    m_frameCommands.emplace_back(
        [this, IndexCount, StartIndexLocation, BaseVertexLocation, vsAtDraw]()
        {
            if (FrameTraceActive())
                FrameTrace("    DrawIndexed eye=%c icount=%u start=%u base=%d (replay)\n",
                           m_activeEye == Eye::Right ? 'R' : 'L',
                           IndexCount, StartIndexLocation, BaseVertexLocation);
            ID3D11VertexShader* _origVS = nullptr; UINT _slot = 0;
            const bool _swapped = BeginRightEyeShaderSwap(vsAtDraw, _origVS, _slot);
            m_real->DrawIndexed(IndexCount, StartIndexLocation, BaseVertexLocation);
            if (_swapped) EndRightEyeShaderSwap(_origVS);
        });
}

void STDMETHODCALLTYPE Context11Proxy::DrawInstanced(
    UINT VertexCountPerInstance, UINT InstanceCount,
    UINT StartVertexLocation, UINT StartInstanceLocation)
{
    BeginDraw(VertexCountPerInstance);
    const DWORD _huntVS = BoundShaderCRC(m_parent, m_boundVS);
    const DWORD _huntPS = BoundShaderCRC(m_parent, m_boundPS);
    wiz3d::ShaderHunter::NoteDraw(_huntVS, _huntPS);
    HuntMaybeCaptureFingerprint();
    if (wiz3d::ShaderHunter::ShouldSkipDraw(_huntVS, _huntPS)) return;
    { ScopedTicks t_(m_time.gameCalls); m_real->DrawInstanced(VertexCountPerInstance, InstanceCount,
                                                             StartVertexLocation, StartInstanceLocation); }
    DUPLICATE_DRAW(DrawInstanced(VertexCountPerInstance, InstanceCount,
                                 StartVertexLocation, StartInstanceLocation));
    if (!m_presentHookActive) return;
    ID3D11VertexShader* vsAtDraw = m_boundVS;   // Phase 2: snapshot for right-eye modified-VS swap
    m_frameCommands.emplace_back(
        [this, VertexCountPerInstance, InstanceCount,
         StartVertexLocation, StartInstanceLocation, vsAtDraw]()
        {
            ID3D11VertexShader* _origVS = nullptr; UINT _slot = 0;
            const bool _swapped = BeginRightEyeShaderSwap(vsAtDraw, _origVS, _slot);
            m_real->DrawInstanced(VertexCountPerInstance, InstanceCount,
                                   StartVertexLocation, StartInstanceLocation);
            if (_swapped) EndRightEyeShaderSwap(_origVS);
        });
}

void STDMETHODCALLTYPE Context11Proxy::DrawIndexedInstanced(
    UINT IndexCountPerInstance, UINT InstanceCount, UINT StartIndexLocation,
    INT BaseVertexLocation, UINT StartInstanceLocation)
{
    BeginDraw(IndexCountPerInstance);
    const DWORD _huntVS = BoundShaderCRC(m_parent, m_boundVS);
    const DWORD _huntPS = BoundShaderCRC(m_parent, m_boundPS);
    wiz3d::ShaderHunter::NoteDraw(_huntVS, _huntPS);
    HuntMaybeCaptureFingerprint();
    if (wiz3d::ShaderHunter::ShouldSkipDraw(_huntVS, _huntPS)) return;
    { ScopedTicks t_(m_time.gameCalls); m_real->DrawIndexedInstanced(IndexCountPerInstance, InstanceCount,
                                                                    StartIndexLocation, BaseVertexLocation,
                                                                    StartInstanceLocation); }
    DUPLICATE_DRAW(DrawIndexedInstanced(IndexCountPerInstance, InstanceCount,
                                        StartIndexLocation, BaseVertexLocation,
                                        StartInstanceLocation));
    if (!m_presentHookActive) return;
    ID3D11VertexShader* vsAtDraw = m_boundVS;   // Phase 2: snapshot for right-eye modified-VS swap
    m_frameCommands.emplace_back(
        [this, IndexCountPerInstance, InstanceCount, StartIndexLocation,
         BaseVertexLocation, StartInstanceLocation, vsAtDraw]()
        {
            ID3D11VertexShader* _origVS = nullptr; UINT _slot = 0;
            const bool _swapped = BeginRightEyeShaderSwap(vsAtDraw, _origVS, _slot);
            m_real->DrawIndexedInstanced(
                IndexCountPerInstance, InstanceCount, StartIndexLocation,
                BaseVertexLocation, StartInstanceLocation);
            if (_swapped) EndRightEyeShaderSwap(_origVS);
        });
}

void STDMETHODCALLTYPE Context11Proxy::DrawAuto()
{
    BeginDraw(UINT_MAX);   // count unknown; assume geometry
    const DWORD _huntVS = BoundShaderCRC(m_parent, m_boundVS);
    const DWORD _huntPS = BoundShaderCRC(m_parent, m_boundPS);
    wiz3d::ShaderHunter::NoteDraw(_huntVS, _huntPS);
    HuntMaybeCaptureFingerprint();
    if (wiz3d::ShaderHunter::ShouldSkipDraw(_huntVS, _huntPS)) return;
    { ScopedTicks t_(m_time.gameCalls); m_real->DrawAuto(); }
    DUPLICATE_DRAW(DrawAuto());
    if (!m_presentHookActive) return;
    ID3D11VertexShader* vsAtDraw = m_boundVS;   // Phase 2: snapshot for right-eye modified-VS swap
    m_frameCommands.emplace_back(
        [this, vsAtDraw]() {
            ID3D11VertexShader* _origVS = nullptr; UINT _slot = 0;
            const bool _swapped = BeginRightEyeShaderSwap(vsAtDraw, _origVS, _slot);
            m_real->DrawAuto();
            if (_swapped) EndRightEyeShaderSwap(_origVS);
        });
}

void STDMETHODCALLTYPE Context11Proxy::DrawInstancedIndirect(
    ID3D11Buffer* pBufferForArgs, UINT AlignedByteOffsetForArgs)
{
    BeginDraw(UINT_MAX);   // count unknown; assume geometry
    const DWORD _huntVS = BoundShaderCRC(m_parent, m_boundVS);
    const DWORD _huntPS = BoundShaderCRC(m_parent, m_boundPS);
    wiz3d::ShaderHunter::NoteDraw(_huntVS, _huntPS);
    HuntMaybeCaptureFingerprint();
    if (wiz3d::ShaderHunter::ShouldSkipDraw(_huntVS, _huntPS)) return;
    { ScopedTicks t_(m_time.gameCalls); m_real->DrawInstancedIndirect(UnwrapBuf(pBufferForArgs), AlignedByteOffsetForArgs); }
    DUPLICATE_DRAW(DrawInstancedIndirect(RightArgs(pBufferForArgs), AlignedByteOffsetForArgs));
    if (!m_presentHookActive) return;
    ComRefHolder bufRef(pBufferForArgs);
    ID3D11VertexShader* vsAtDraw = m_boundVS;   // Phase 2: snapshot for right-eye modified-VS swap
    m_frameCommands.emplace_back(
        [this, bufRef, AlignedByteOffsetForArgs, vsAtDraw]()
        {
            ID3D11VertexShader* _origVS = nullptr; UINT _slot = 0;
            const bool _swapped = BeginRightEyeShaderSwap(vsAtDraw, _origVS, _slot);
            m_real->DrawInstancedIndirect(
                UnwrapBuf(static_cast<ID3D11Buffer*>(bufRef.p)), AlignedByteOffsetForArgs);
            if (_swapped) EndRightEyeShaderSwap(_origVS);
        });
}

void STDMETHODCALLTYPE Context11Proxy::DrawIndexedInstancedIndirect(
    ID3D11Buffer* pBufferForArgs, UINT AlignedByteOffsetForArgs)
{
    BeginDraw(UINT_MAX);   // count unknown; assume geometry
    const DWORD _huntVS = BoundShaderCRC(m_parent, m_boundVS);
    const DWORD _huntPS = BoundShaderCRC(m_parent, m_boundPS);
    wiz3d::ShaderHunter::NoteDraw(_huntVS, _huntPS);
    HuntMaybeCaptureFingerprint();
    if (wiz3d::ShaderHunter::ShouldSkipDraw(_huntVS, _huntPS)) return;
    { ScopedTicks t_(m_time.gameCalls); m_real->DrawIndexedInstancedIndirect(UnwrapBuf(pBufferForArgs), AlignedByteOffsetForArgs); }
    DUPLICATE_DRAW(DrawIndexedInstancedIndirect(RightArgs(pBufferForArgs), AlignedByteOffsetForArgs));
    if (!m_presentHookActive) return;
    ComRefHolder bufRef(pBufferForArgs);
    ID3D11VertexShader* vsAtDraw = m_boundVS;   // Phase 2: snapshot for right-eye modified-VS swap
    m_frameCommands.emplace_back(
        [this, bufRef, AlignedByteOffsetForArgs, vsAtDraw]()
        {
            ID3D11VertexShader* _origVS = nullptr; UINT _slot = 0;
            const bool _swapped = BeginRightEyeShaderSwap(vsAtDraw, _origVS, _slot);
            m_real->DrawIndexedInstancedIndirect(
                UnwrapBuf(static_cast<ID3D11Buffer*>(bufRef.p)), AlignedByteOffsetForArgs);
            if (_swapped) EndRightEyeShaderSwap(_origVS);
        });
}

void STDMETHODCALLTYPE Context11Proxy::Dispatch(
    UINT ThreadGroupCountX, UINT ThreadGroupCountY, UINT ThreadGroupCountZ)
{
    FlushRightBatch(kFlushCompute);
    ++m_dispatchesThisFrame;
    {
        ScopedTicks t_(m_time.dispatches);
        PrepareSharedUavTwins();
        if (ConstantBlocksOn()) SyncBoundConstants(false, false, true);
    }
    { ScopedTicks t_(m_time.gameCalls); m_real->Dispatch(ThreadGroupCountX, ThreadGroupCountY, ThreadGroupCountZ); }
    ScopedTicks right_(m_time.dispatches);
    const UINT g = m_gpuTimer ? m_gpuTimer->Begin(m_real) : ~0u;
    if (BeginRightEyeDispatch())
    {
        ++m_dispatchesDuplicatedThisFrame;
        m_real->Dispatch(ThreadGroupCountX, ThreadGroupCountY, ThreadGroupCountZ);
        EndRightEyeDispatch();
        if (m_gpuTimer) m_gpuTimer->End(m_real, g, GpuTimer11::kRightDispatch, std::string(), 0);
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
    FlushRightBatch(kFlushCompute);
    ++m_dispatchesThisFrame;
    {
        ScopedTicks t_(m_time.dispatches);
        PrepareSharedUavTwins();
        if (ConstantBlocksOn()) SyncBoundConstants(false, false, true);
    }
    { ScopedTicks t_(m_time.gameCalls); m_real->DispatchIndirect(UnwrapBuf(pBufferForArgs), AlignedByteOffsetForArgs); }
    ScopedTicks right_(m_time.dispatches);
    const UINT g = m_gpuTimer ? m_gpuTimer->Begin(m_real) : ~0u;
    if (BeginRightEyeDispatch())
    {
        ++m_dispatchesDuplicatedThisFrame;
        m_real->DispatchIndirect(RightArgs(pBufferForArgs), AlignedByteOffsetForArgs);
        EndRightEyeDispatch();
        if (m_gpuTimer) m_gpuTimer->End(m_real, g, GpuTimer11::kRightDispatch, std::string(), 0);
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
    MIRROR(OMSetRenderTargets(NumViews, rtvsToUse, realDSV));
}

void STDMETHODCALLTYPE Context11Proxy::OMSetRenderTargets(
    UINT NumViews, ID3D11RenderTargetView* const* ppRenderTargetViews,
    ID3D11DepthStencilView* pDepthStencilView)
{
    // BatchRightEye: the pass ends, and a later pass may write what its recorded draws read.
    FlushRightBatch(kFlushTargets);
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
    if (m_mirror)
    {
        // The recording keeps each counter where the immediate context's bind set it.
        UINT keep[kMaxUAVs];
        for (UINT i = 0; i < kMaxUAVs; ++i) keep[i] = (UINT)-1;
        m_mirror->OMSetRenderTargetsAndUnorderedAccessViews(NumRTVs, rtvsToUse, realDSV, UAVStartSlot, NumUAVs, uavsToUse,
                                                            pUAVInitialCounts ? keep : nullptr);
    }
}

void STDMETHODCALLTYPE Context11Proxy::OMSetRenderTargetsAndUnorderedAccessViews(
    UINT NumRTVs, ID3D11RenderTargetView* const* ppRenderTargetViews,
    ID3D11DepthStencilView* pDepthStencilView,
    UINT UAVStartSlot, UINT NumUAVs,
    ID3D11UnorderedAccessView* const* ppUnorderedAccessViews,
    const UINT* pUAVInitialCounts)
{
    FlushRightBatch(kFlushTargets);
    // D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL leaves the OM bindings
    // alone, so the tracked pair stays valid and must not be overwritten.
    if (NumRTVs != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL)
        TrackOM(NumRTVs, ppRenderTargetViews, pDepthStencilView);
    if (gInfo.DuplicateDraws && NumUAVs > 0 &&
        NumUAVs != D3D11_KEEP_UNORDERED_ACCESS_VIEWS)
    {
        m_omHasUAVs = true;
        // Track the UAV binding (AddRef'd — the game may release views while
        // the binding persists) so BindEye can re-issue it per eye.
        ClearOmUavRefs();
        m_omUavBound = true;
        m_omUavStart = UAVStartSlot;
        m_omUavCount = NumUAVs <= kMaxUAVs ? NumUAVs : kMaxUAVs;
        m_omUavHasInit = (pUAVInitialCounts != nullptr);
        m_omUavRightInited = false;
        for (UINT i = 0; i < m_omUavCount; ++i)
        {
            ID3D11UnorderedAccessView* p =
                ppUnorderedAccessViews ? ppUnorderedAccessViews[i] : nullptr;
            if (p) p->AddRef();
            m_omUavGame[i] = p;
            m_omUavInit[i] = pUAVInitialCounts ? pUAVInitialCounts[i] : (UINT)-1;
        }
    }
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
    MIRROR(RSSetViewports(NumViewports, pViewports));
    m_gameVpCount = 0;
    for (UINT i = 0; pViewports && i < NumViewports && i < D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE; ++i)
        m_gameVp[m_gameVpCount++] = pViewports[i];
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

// The right eye's copy of a resource: a buffer's right sibling or a stereo texture's, else the resource itself.
ID3D11Resource* RightEyeResource(ID3D11Resource* p)
{
    if (auto* buf = p ? wiz3d::TryUnwrapBuffer(p) : nullptr)
        if (buf->GetRealRight()) return static_cast<ID3D11Resource*>(buf->GetRealRight());
    return UnwrapResourceForEye(p, true);
}

// Under duplication no replay pass redoes a copy for the right eye: the destination's right copy (a stereo texture's
// or a buffer's sibling) is written now; null when it has none.
static inline ID3D11Resource* RightMirrorTarget(ID3D11Resource* pDst, bool pickRight)
{
    if (!gInfo.DuplicateDraws || pickRight || !pDst) return nullptr;
    ID3D11Resource* r = RightEyeResource(pDst);
    return r != UnwrapResourceForEye(pDst, false) ? r : nullptr;
}

void STDMETHODCALLTYPE Context11Proxy::CopyStructureCount(
    ID3D11Buffer* pDstBuffer, UINT DstAlignedByteOffset, ID3D11UnorderedAccessView* pSrcView)
{
    FlushRightBatch(kFlushWrite);
    bool pickRight = (m_activeEye == Eye::Right);
    m_real->CopyStructureCount(UnwrapBuf(pDstBuffer), DstAlignedByteOffset,
                               UnwrapUAVForEye(pSrcView, pickRight));
    // IndirectArgsPerEye: an args buffer without a sibling gets one, seeded from the left, so the right dispatch takes
    // the right UAV's count (Hardline's tiled lighting runs one group per compacted tile).
    Buffer11Proxy* dp = IndirectArgsPerEyeOn() ? TryUnwrapBuffer(pDstBuffer) : nullptr;
    if (dp && !dp->GetRealRight() && gInfo.DuplicateDraws && m_parent)
    {
        D3D11_BUFFER_DESC bd = {};
        dp->GetReal()->GetDesc(&bd);
        ID3D11Buffer* twin = nullptr;
        if (SUCCEEDED(m_parent->GetReal()->CreateBuffer(&bd, nullptr, &twin)) && twin)
        {
            m_real->CopyResource(twin, dp->GetReal());
            dp->SetRealRight(twin);
        }
    }
    // A destination with a right sibling takes the right UAV's count too (indirect arguments the right eye reads).
    if (ID3D11Resource* rd = RightMirrorTarget(pDstBuffer, pickRight))
        m_real->CopyStructureCount(static_cast<ID3D11Buffer*>(rd), DstAlignedByteOffset, UnwrapUAVForEye(pSrcView, true));
}

void STDMETHODCALLTYPE Context11Proxy::ClearUnorderedAccessViewUint(
    ID3D11UnorderedAccessView* pUnorderedAccessView, const UINT Values[4])
{
    FlushRightBatch(kFlushWrite);
    bool pickRight = (m_activeEye == Eye::Right);
    m_real->ClearUnorderedAccessViewUint(UnwrapUAVForEye(pUnorderedAccessView, pickRight), Values);
    // The right view too, as for render targets: without it right-eye counters and histograms accumulate.
    ID3D11UnorderedAccessView* r = UnwrapUAVForEye(pUnorderedAccessView, true);
    if (gInfo.DuplicateDraws && !pickRight && r != UnwrapUAVForEye(pUnorderedAccessView, false))
        m_real->ClearUnorderedAccessViewUint(r, Values);
}

void STDMETHODCALLTYPE Context11Proxy::ClearUnorderedAccessViewFloat(
    ID3D11UnorderedAccessView* pUnorderedAccessView, const FLOAT Values[4])
{
    FlushRightBatch(kFlushWrite);
    bool pickRight = (m_activeEye == Eye::Right);
    m_real->ClearUnorderedAccessViewFloat(UnwrapUAVForEye(pUnorderedAccessView, pickRight), Values);
    ID3D11UnorderedAccessView* r = UnwrapUAVForEye(pUnorderedAccessView, true);
    if (gInfo.DuplicateDraws && !pickRight && r != UnwrapUAVForEye(pUnorderedAccessView, false))
        m_real->ClearUnorderedAccessViewFloat(r, Values);
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
    FlushRightBatch(kFlushWrite);
    bool pickRight = (m_activeEye == Eye::Right);
    m_real->GenerateMips(UnwrapSRVForEye(pShaderResourceView, pickRight));
    if (gInfo.DuplicateDraws && !pickRight &&
        UnwrapSRVForEye(pShaderResourceView, true) !=
        UnwrapSRVForEye(pShaderResourceView, false))
        m_real->GenerateMips(UnwrapSRVForEye(pShaderResourceView, true));
}

ID3D11Resource* Context11Proxy::GameResource(ID3D11Resource* p) const
{
    if (!p || !m_parent || !RawResourcesOn()) return p;
    if (TryUnwrapTexture2D(p) || TryUnwrapBuffer(p) || TryUnwrapTexture1D(p) || TryUnwrapTexture3D(p)) return p;
    Texture2D11Proxy* t = m_parent->LookupProxyByReal(p);
    if (!t) return p;
    NoteRawResolved();
    return static_cast<ID3D11Resource*>(t);
}

void Context11Proxy::DoCopyResource(
    ID3D11Resource* pDstResource, ID3D11Resource* pSrcResource)
{
    FlushRightBatch(kFlushWrite);
    pDstResource = GameResource(pDstResource);
    pSrcResource = GameResource(pSrcResource);
    bool pickRight = (m_activeEye == Eye::Right);
    m_real->CopyResource(UnwrapResourceForEye(pDstResource, pickRight),
                         UnwrapResourceForEye(pSrcResource, pickRight));
    if (ID3D11Resource* rd = RightMirrorTarget(pDstResource, pickRight))
    {
        m_real->CopyResource(rd, RightEyeResource(pSrcResource));
        if (auto* t = TryUnwrapTexture2D(pDstResource)) t->MarkRightWritten();
    }
    if (FrameTraceActive() && !TryUnwrapBuffer(pDstResource))
        FrameTrace("      fill CopyResource dst=%p(%s) src=%p(%s) mirrored=%d\n", pDstResource,
                   TryUnwrapTexture2D(pDstResource) ? "proxy" : "raw", pSrcResource,
                   TryUnwrapTexture2D(pSrcResource) ? "proxy" : "raw",
                   (int)(RightMirrorTarget(pDstResource, pickRight) != nullptr));
    // A copied-into buffer no longer holds its last written bytes; a ring's copied range is read back a frame later.
    if (Buffer11Proxy* db = TryUnwrapBuffer(pDstResource))
    {
        db->SetRightFromShadow(false);
        db->ForgetShadow();
        if (db->IsRing()) NoteRingCopy(db, 0, db->RingSize());
    }
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
    FlushRightBatch(kFlushWrite);
    pDstResource = GameResource(pDstResource);
    pSrcResource = GameResource(pSrcResource);
    bool pickRight = (m_activeEye == Eye::Right);
    m_real->CopySubresourceRegion(
        UnwrapResourceForEye(pDstResource, pickRight), DstSubresource, DstX, DstY, DstZ,
        UnwrapResourceForEye(pSrcResource, pickRight), SrcSubresource, pSrcBox);
    if (ID3D11Resource* rd = RightMirrorTarget(pDstResource, pickRight))
    {
        m_real->CopySubresourceRegion(rd, DstSubresource, DstX, DstY, DstZ,
                                      RightEyeResource(pSrcResource), SrcSubresource, pSrcBox);
        if (auto* t = TryUnwrapTexture2D(pDstResource)) t->MarkRightWritten();
    }
    if (FrameTraceActive() && (TryUnwrapTexture2D(pDstResource) || !TryUnwrapBuffer(pDstResource)))
        FrameTrace("      fill CopySubresourceRegion dst=%p sub=%u src=%p(%s) sub=%u mirrored=%d\n", pDstResource,
                   DstSubresource, pSrcResource, TryUnwrapTexture2D(pSrcResource) ? "proxy" : "raw", SrcSubresource,
                   (int)(RightMirrorTarget(pDstResource, pickRight) != nullptr));
    if (Buffer11Proxy* db = TryUnwrapBuffer(pDstResource))
    {
        db->SetRightFromShadow(false);
        db->ForgetShadow();
        // A ring's copied range is read back a frame later; until then its pages are the game's.
        if (db->IsRing())
        {
            size_t n = db->RingSize() > DstX ? db->RingSize() - DstX : 0;
            if (pSrcBox && pSrcBox->right > pSrcBox->left && pSrcBox->right - pSrcBox->left < n) n = pSrcBox->right - pSrcBox->left;
            if (n) NoteRingCopy(db, DstX, n);
        }
    }
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

// The first inverse-shaped block only: a buffer can hold a previous frame's reprojection matrix beside the current one.
static bool DetectInverseInCB(const unsigned char* data, size_t byteCount,
                              wiz3d::StereoMatrixState::DetectedInverse& out, float& bestMiss)
{
    return s_mat.DetectInverses(data, byteCount, &out, 1, bestMiss) != 0;
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
        if (boundCBs[md.constantBuffer] != pResource) continue;
        EyeShiftMatrix em;
        em.matrixRegister     = md.matrixRegister;
        em.matrixIsTransposed = md.matrixIsTransposed;
        em.matrixIsInverse    = md.inverse;
        em.matrixFromLearned  = FALSE;
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

HRESULT STDMETHODCALLTYPE Context11Proxy::Map(
    ID3D11Resource* pResource, UINT Subresource, D3D11_MAP MapType, UINT MapFlags,
    D3D11_MAPPED_SUBRESOURCE* pMappedResource)
{
    ScopedTicks t_(m_time.maps);
    // Stage 3c.1: unwrap either texture or buffer proxies before forwarding.
    Buffer11Proxy*    buf = TryUnwrapBuffer(pResource);
    // BatchRightEye: the recorded draws read everything but a constant buffer's right sibling in place (the recording
    // writes that), so they replay before the game writes it.
    // A ring's DISCARD renames both eyes' real buffers now, so the ranges recorded for the old ones replay first.
    // NO_OVERWRITE leaves every byte an issued draw reads, a read writes nothing: neither touches a recorded draw.
    const bool writesInUse = MapType == D3D11_MAP_WRITE_DISCARD || MapType == D3D11_MAP_WRITE || MapType == D3D11_MAP_READ_WRITE;
    if ((writesInUse && !(buf && buf->GetRealRight()) && !(buf && IAOnlyUnread(buf->GetReal()))) || PendingLeftCB(buf) ||
        (buf && buf->IsRing() && MapType == D3D11_MAP_WRITE_DISCARD))
        FlushRightBatch(kFlushMap);
    // A ring constant buffer: the game writes our shadow, and the ranges it binds reach the real buffers at the draw
    // (SyncBoundConstants). A DISCARD renames both real buffers now, so range writes never land on in-flight memory.
    if (buf && buf->IsRing() && gInfo.DuplicateDraws && pMappedResource &&
        (MapType == D3D11_MAP_WRITE_DISCARD || MapType == D3D11_MAP_WRITE_NO_OVERWRITE || MapType == D3D11_MAP_WRITE))
    {
        if (MapType == D3D11_MAP_WRITE_DISCARD)
        {
            ID3D11Buffer* reals[2] = { buf->GetReal(), buf->GetRealRight() };
            for (ID3D11Buffer* r : reals)
            {
                D3D11_MAPPED_SUBRESOURCE m = {};
                if (r && SUCCEEDED(m_real->Map(r, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) m_real->Unmap(r, 0);
            }
        }
        pMappedResource->pData = buf->RingData();
        pMappedResource->RowPitch = pMappedResource->DepthPitch = (UINT)buf->RingSize();
        return S_OK;
    }
    // Texture1D/3D too: d3d11 must get the real resource, never our proxy.
    ID3D11Resource*   realRes = UnwrapResourceForEye(pResource, false);
    HRESULT hr;
    LARGE_INTEGER m0 = {};
    if (gInfo.CollectDebugInformation) QueryPerformanceCounter(&m0);
    { ScopedTicks d_(m_time.mapDriver); hr = m_real->Map(realRes, Subresource, MapType, MapFlags, pMappedResource); }
    if (gInfo.CollectDebugInformation) NoteSlowMap(realRes, MapType, m0.QuadPart, false);
    if (FAILED(hr) || !pMappedResource) return hr;
    // Duplication needs CB capture too, and never arms m_presentHookActive.
    if (!m_presentHookActive && !gInfo.DuplicateDraws) return hr;
    if (!gInfo.UseCOMWrapReplay) return hr;

    // Stage 4c: only record write maps on CONSTANT BUFFERS. Stage 4c.1
    // additionally requires the buffer to have been ever bound through a
    // vertex-pipeline stage — the IsVSBound tag is set on Buffer11Proxy by
    // *SetConstantBuffers when its stage is VS/GS/HS/DS.
    if (MapType != D3D11_MAP_WRITE_DISCARD &&
        MapType != D3D11_MAP_WRITE &&
        MapType != D3D11_MAP_WRITE_NO_OVERWRITE &&
        MapType != D3D11_MAP_READ_WRITE)
        return hr;
    // Under duplication every CB write must reach the sibling even when it needs no eye shift, or the right eye
    // reads stale constants.
    if (!buf || (!gInfo.DuplicateDraws && !buf->IsVSBound())) return hr;

    D3D11_BUFFER_DESC desc;
    buf->GetReal()->GetDesc(&desc);
    if ((desc.BindFlags & D3D11_BIND_CONSTANT_BUFFER) == 0) return hr;
    if (desc.ByteWidth == 0) return hr;

    ActiveMap am;
    am.resource    = pResource;
    am.subresource = Subresource;
    am.mapType     = MapType;
    am.mappedData  = pMappedResource->pData;
    am.byteWidth   = desc.ByteWidth;
    m_activeMaps.push_back(am);
    return hr;
}

// The registers the bound VS reads as projection matrices at the slots this buffer occupies. boundToVS: it is bound to
// that VS, so "none here" is final, as UE3 reuses one buffer per slot.
static void AnalyzerTargetsForCB(Device11Proxy* parent, void* boundVS,
                                 ID3D11Buffer* const* boundVSCBs,
                                 ID3D11Resource* pResource,
                                 std::vector<EyeShiftMatrix>& targets,
                                 bool& analyzerKnows, bool& boundToVS)
{
    analyzerKnows = false;
    boundToVS = false;
    if (!boundVS || !parent) return;
    const ShaderAnalysis11Result* info = parent->LookupShaderProjection(boundVS);
    if (!info || !info->parsed) return;
    analyzerKnows = true;
    // Only a shader with projection matrices somewhere is authoritative about a buffer it binds but reads none from: a
    // matrix-less (UI, post) shader knows nothing, and Far Cry 4 writes its shared view buffer under one.
    const bool vsHasMatrices = !info->projection.matrixData.cb.empty();
    for (UINT slot = 0; slot < Context11Proxy::kMaxCBs; ++slot)   // == kMaxVSCBSlots
    {
        if (static_cast<ID3D11Resource*>(boundVSCBs[slot]) != pResource) continue;
        if (vsHasMatrices) boundToVS = true;
        auto cbIt = info->projection.matrixData.cb.find(slot);
        if (cbIt == info->projection.matrixData.cb.end()) continue;
        for (const auto& pmd : cbIt->second)
        {
            if (pmd.incorrectProjection) continue;
            EyeShiftMatrix em = { pmd.matrixRegister, pmd.matrixIsTransposed, FALSE, FALSE };
            targets.push_back(em);
        }
    }
}

void STDMETHODCALLTYPE Context11Proxy::Unmap(ID3D11Resource* pResource, UINT Subresource)
{
    ScopedTicks t_(m_time.maps);
    Buffer11Proxy*    buf = TryUnwrapBuffer(pResource);
    // A ring constant buffer was never mapped for real: the shadow changed, and its bound ranges resync at the draw.
    if (buf && buf->IsRing() && gInfo.DuplicateDraws) { buf->MarkRingWritten(); return; }
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
            std::vector<unsigned char>& bytes = m_cbScratch;   // reused: no allocation per write
            bytes.resize(it->byteWidth);
            {
                ScopedTicks r_(m_time.mapRead);
                memcpy(bytes.data(), it->mappedData, it->byteWidth);
            }
            if (gInfo.CollectDebugInformation) m_time.mapBytes += it->byteWidth;
            UINT subres = it->subresource;
            D3D11_MAP mapType = it->mapType;
            ComRefHolder resRef(pResource);

            // Stage 4e.2: consult the analyzer for the currently bound VS.
            // If the bound shader has known projection matrices at any VS-CB
            // slot where this buffer is bound, build a targeted matrix list.
            // Empty list ⇒ fall back to the m[2][3]==1 / m[3][3]==0 heuristic.
            std::vector<EyeShiftMatrix> targets;
            // analyzerKnows: the bound VS was parsed, so empty targets mean "no projection matrix here", not "unknown".
            bool analyzerKnows = false, boundToVS = false;
            AnalyzerTargetsForCB(m_parent, m_boundVS, m_boundVSCBs, pResource,
                                 targets, analyzerKnows, boundToVS);

            // Profile-declared matrices on top of the analyzer's; a profile hit also counts as knowing this buffer.
            size_t beforeProfile = targets.size();
            AddProfileMatrixTargets(g_ProfileData.VSCRCData,
                                    BoundShaderCRC(m_parent, m_boundVS),
                                    m_boundVSCBs, kMaxVSCBSlots, pResource, targets);
            AddProfileMatrixTargets(g_ProfileData.PSCRCData,
                                    BoundShaderCRC(m_parent, m_boundPS),
                                    m_boundPSCBs, kMaxPSCBSlots, pResource, targets);
            if (targets.size() != beforeProfile) analyzerKnows = true;

            // Written for an overlay draw (back buffer, no depth test): no shift, and nothing learned from it.
            const bool overlayWrite = OverlayPass();
            if (overlayWrite) { targets.clear(); analyzerKnows = true; boundToVS = true; }

            // A buffer is often written before its consumer is bound, so the targets are remembered per buffer: without
            // them such writes reached the right eye unshifted, pinned at screen depth.
            if (Buffer11Proxy* lb = TryUnwrapBuffer(pResource))
            {
                lb->SetShadow(bytes.data(), bytes.size());
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
                // Learned registers fill that gap; they never override a parsed VS that has this buffer bound and says
                // "no matrix here".
                else if (!(analyzerKnows && boundToVS))
                {
                    // Under the draw patch each consumer gets its own matrices at its draw, so a pooled buffer's
                    // remembered forward targets stay off the plain constants written here; inverses still ride along.
                    const bool forwardWithheld = gInfo.PatchConstantsAtDraw;
                    const std::vector<Buffer11Proxy::MatrixTarget>& mem = lb->LearnedMatrices();
                    for (size_t ti = 0; ti < mem.size(); ++ti)
                    {
                        if (forwardWithheld && !mem[ti].inverse) { ++m_learnedWithheldThisFrame; continue; }
                        EyeShiftMatrix e = { mem[ti].reg, mem[ti].transposed, mem[ti].inverse, TRUE };
                        targets.push_back(e);
                        if (mem[ti].inverse) ++m_invLearnedThisFrame;
                    }
                    if (!targets.empty()) analyzerKnows = true;
                }
            }

            // Still unidentified: deferred lighting keeps its inverse view-projection in pixel-shader-only buffers no
            // shader analysis reaches, so the bytes are tested numerically.
            Buffer11Proxy* scanBp = TryUnwrapBuffer(pResource);
            wiz3d::StereoMatrixState::DetectedInverse hit;
            if (targets.empty() && !overlayWrite && gInfo.DetectInverseMatrix && bytes.size() >= 64 &&
                scanBp && scanBp->ShouldScanForInverse())
            {
                ++m_invScansThisFrame;
                if (DetectInverseInCB(bytes.data(), bytes.size(), hit, m_invBestMissThisFrame))
                {
                    ++m_invHitsThisFrame;
                    const BOOL itr = hit.transposed ? TRUE : FALSE;
                    EyeShiftMatrix em = { (DWORD)hit.reg, itr, TRUE, FALSE };
                    targets.push_back(em);
                    Buffer11Proxy::MatrixTarget mt = { (DWORD)hit.reg, itr, TRUE };
                    scanBp->LearnMatrix(mt);
                }
            }
            if (!targets.empty()) ++m_cbTargetedThisFrame;
            else                  ++m_cbUntargetedThisFrame;
            if (targets.empty() && gInfo.CollectDebugInformation)
            {
                const ShaderAnalysis11Result* vsInfo = m_boundVS ? m_parent->LookupShaderProjection(m_boundVS) : nullptr;
                const int why = !m_boundVS ? 0 : (!vsInfo || !vsInfo->parsed) ? 1
                              : vsInfo->projection.matrixData.cb.empty() ? 2 : boundToVS ? 4 : 3;
                ++m_untargetedWhy[why];
            }

            // Draw duplication keeps each eye's constants in its own buffer, so the patch lands in the sibling now; a
            // buffer no target names is copied through unshifted (no guessing).
            if (gInfo.DuplicateDraws)
            {
                Buffer11Proxy* bp = TryUnwrapBuffer(pResource);
                ID3D11Buffer* rightBuf = bp ? bp->GetRealRight() : nullptr;
                if (rightBuf)
                {
                    D3D11_MAPPED_SUBRESOURCE mapped = {};
                    HRESULT rhr;
                    // The recorder takes a whole-buffer DISCARD; the whole buffer is written either way.
                    ID3D11DeviceContext* rc = RightWriter();
                    const D3D11_MAP rightMap = rc != m_real ? D3D11_MAP_WRITE_DISCARD : mapType;
                    LARGE_INTEGER m0 = {};
                    if (gInfo.CollectDebugInformation) QueryPerformanceCounter(&m0);
                    { ScopedTicks d_(m_time.mapDriver); rhr = rc->Map(rightBuf, subres, rightMap, 0, &mapped); }
                    if (gInfo.CollectDebugInformation && rc == m_real) NoteSlowMap(rightBuf, rightMap, m0.QuadPart, true);
                    if (SUCCEEDED(rhr) && mapped.pData)
                    {
                        memcpy(mapped.pData, bytes.data(), bytes.size());
                        // Written while an x-flipped target is bound (a profile feature says so): the shift flips.
                        const float sign = EyeShiftSign();
                        float eyeShift = wiz3D_GetEffectiveEyeShift()
                                       * ProfileShaderMultiplier(m_parent, m_boundVS) * sign;
                        const float conv = ResolveShaderConvergence(m_parent, m_boundVS);
                        if (sign < 0.f && !targets.empty()) ++m_flippedWritesThisFrame;
                        const unsigned before = m_cbMatShiftedThisFrame, invBefore = m_invPatchedThisFrame;
                        if (!targets.empty())
                        {
                            ApplyTargetedEyeShiftToCB(
                                static_cast<unsigned char*>(mapped.pData),
                                bytes.size(), eyeShift, conv, targets, true,
                                &m_cbMatShiftedThisFrame, &m_cbMatRejectedThisFrame,
                                &m_invPatchedThisFrame, pResource);
                            if (m_cbMatShiftedThisFrame != before) bp->MarkShifted(s_frameId);
                        }
                        const unsigned inv = m_invPatchedThisFrame - invBefore;
                        bp->NoteRightShift(m_cbMatShiftedThisFrame - before - inv, inv);
                        NoteRightConstants(bp);
                        ScopedTicks d_(m_time.mapDriver);
                        rc->Unmap(rightBuf, subres);
                    }
                }
                m_activeMaps.erase(it);
                break;
            }

            // Multiplier resolved now: the shader bound at write time is the
            // one the profile entry describes, not whatever replays later. The
            // convergence shift is captured here too, since the replay happens
            // after the VS binding may have changed.
            const float profMult = ProfileShaderMultiplier(m_parent, m_boundVS);
            const float profConv = ResolveShaderConvergence(m_parent, m_boundVS);
            m_frameCommands.emplace_back(
                [this, resRef, subres, bytes, mapType, targets, profMult, profConv]()
                {
                    if (m_activeEye != Eye::Right) return;
                    auto* gameRes = static_cast<ID3D11Resource*>(resRef.p);
                    ID3D11Resource* real = UnwrapResourceForEye(gameRes, false);
                    D3D11_MAPPED_SUBRESOURCE mapped = {};
                    if (FAILED(m_real->Map(real, subres, mapType, 0, &mapped))
                        || !mapped.pData) return;
                    memcpy(mapped.pData, bytes.data(), bytes.size());
                    float eyeShift = wiz3D_GetEffectiveEyeShift() * profMult;
                    if (!targets.empty())
                    {
                        ApplyTargetedEyeShiftToCB(
                            static_cast<unsigned char*>(mapped.pData),
                            bytes.size(), eyeShift, profConv, targets, false,
                            &m_cbMatShiftedThisFrame, &m_cbMatRejectedThisFrame,
                            &m_invPatchedThisFrame, gameRes);
                    }
                    else
                    {
                        ApplyEyeShiftToCB(static_cast<unsigned char*>(mapped.pData),
                                          bytes.size(), eyeShift);
                    }
                    m_real->Unmap(real, subres);
                });
        }
        m_activeMaps.erase(it);
        break;
    }

    ScopedTicks d_(m_time.mapDriver);
    m_real->Unmap(realRes, Subresource);
}

void Context11Proxy::DoUpdateSubresource(
    ID3D11Resource* pDstResource, UINT DstSubresource, const D3D11_BOX* pDstBox,
    const void* pSrcData, UINT SrcRowPitch, UINT SrcDepthPitch)
{
    ScopedTicks t_(m_time.updates);
    if (Buffer11Proxy* ub = TryUnwrapBuffer(pDstResource))
    {
        const size_t at = pDstBox ? pDstBox->left : 0;
        const size_t len = pDstBox ? (pDstBox->right > pDstBox->left ? pDstBox->right - pDstBox->left : 0) : ~size_t(0) / 2;
        const bool shared = !ub->GetRealRight() && !IAOnlyUnread(ub->GetReal());
        if (shared || PendingLeftCB(ub) || PendingLeftRange(ub->GetReal(), at, len))
        {
            // CollectDebugInformation: what the replays before an update wrote (the recording row).
            if (m_rightPending && shared)
            {
                ++m_updReplayBuf;
                D3D11_BUFFER_DESC bd = {};
                ub->GetReal()->GetDesc(&bd);
                m_updReplayBufBinds |= bd.BindFlags;
            }
            else if (m_rightPending) ++m_updReplayCB;
            FlushRightBatch(kFlushUpdate);
        }
    }
    else
    {
        if (m_rightPending) ++m_updReplayTex;
        FlushRightBatch(kFlushUpdate);
    }
    bool pickRight = (m_activeEye == Eye::Right);
    m_real->UpdateSubresource(UnwrapResourceForEye(pDstResource, pickRight),
                              DstSubresource, pDstBox,
                              pSrcData, SrcRowPitch, SrcDepthPitch);

    if (NeedsRightMirror(pDstResource, pickRight))
    {
        m_real->UpdateSubresource(UnwrapResourceForEye(pDstResource, true),
                                  DstSubresource, pDstBox,
                                  pSrcData, SrcRowPitch, SrcDepthPitch);
        if (auto* t = TryUnwrapTexture2D(pDstResource)) t->MarkRightWritten();
    }
    if (FrameTraceActive() && TryUnwrapTexture2D(pDstResource))
        FrameTrace("      fill UpdateSubresource dst=%p sub=%u box=%d mirrored=%d\n", pDstResource, DstSubresource,
                   pDstBox != nullptr, (int)NeedsRightMirror(pDstResource, pickRight));

    // Buffers have no right sibling in UnwrapResourceForEye, so a CB written here is mirrored explicitly: a whole-buffer
    // write gets the Map path's targets and inverse detection, a boxed partial write is copied as is.
    if (gInfo.DuplicateDraws)
    {
        Buffer11Proxy* bp = TryUnwrapBuffer(pDstResource);
        if (bp && bp->GetRealRight())
        {
            ++m_updCBWritesThisFrame;
            bool patched = false;
            const unsigned shiftedBefore = m_cbMatShiftedThisFrame, invBeforeUpd = m_invPatchedThisFrame;
            bp->SetRightFromShadow(false);
            if (!pDstBox && pSrcData)
            {
                D3D11_BUFFER_DESC bd = {};
                bp->GetRealRight()->GetDesc(&bd);
                std::vector<unsigned char>& tmp = m_cbScratch;   // reused: no allocation per write
                tmp.resize(bd.ByteWidth);
                memcpy(tmp.data(), pSrcData, bd.ByteWidth);
                bp->SetShadow(tmp.data(), tmp.size());

                // Same attribution as the Map path: the bound VS first (UE3 commits CBs right before the draw that
                // consumes them), learned registers only where no parsed VS has this buffer bound.
                std::vector<EyeShiftMatrix> targets;
                bool analyzerKnows = false, boundToVS = false;
                AnalyzerTargetsForCB(m_parent, m_boundVS, m_boundVSCBs, pDstResource,
                                     targets, analyzerKnows, boundToVS);
                if (!targets.empty())
                {
                    for (size_t ti = 0; ti < targets.size(); ++ti)
                    {
                        Buffer11Proxy::MatrixTarget mt = { targets[ti].matrixRegister,
                                                           targets[ti].matrixIsTransposed,
                                                           targets[ti].matrixIsInverse };
                        bp->LearnMatrix(mt);
                    }
                }
                else if (!(analyzerKnows && boundToVS))
                {
                    // Same rule as the Map path: forward targets wait for the consumer's draw patch.
                    const bool forwardWithheld = gInfo.PatchConstantsAtDraw;
                    const std::vector<Buffer11Proxy::MatrixTarget>& mem = bp->LearnedMatrices();
                    for (size_t ti = 0; ti < mem.size(); ++ti)
                    {
                        if (forwardWithheld && !mem[ti].inverse) { ++m_learnedWithheldThisFrame; continue; }
                        EyeShiftMatrix e = { mem[ti].reg, mem[ti].transposed,
                                             mem[ti].inverse, TRUE };
                        targets.push_back(e);
                        if (mem[ti].inverse) ++m_invLearnedThisFrame;
                    }
                }
                if (targets.empty() && gInfo.DetectInverseMatrix && bd.ByteWidth >= 64 &&
                    bp->ShouldScanForInverse())
                {
                    ++m_invScansThisFrame;
                    wiz3d::StereoMatrixState::DetectedInverse hit;
                    if (DetectInverseInCB(tmp.data(), bd.ByteWidth, hit, m_invBestMissThisFrame))
                    {
                        ++m_invHitsThisFrame;
                        const BOOL itr = hit.transposed ? TRUE : FALSE;
                        EyeShiftMatrix em = { (DWORD)hit.reg, itr, TRUE, FALSE };
                        targets.push_back(em);
                        Buffer11Proxy::MatrixTarget mt = { (DWORD)hit.reg, itr, TRUE };
                        bp->LearnMatrix(mt);
                    }
                }
                if (!targets.empty())
                {
                    const unsigned before = m_cbMatShiftedThisFrame;
                    const float sign = EyeShiftSign();
                    float eyeShift = wiz3D_GetEffectiveEyeShift()
                                   * ProfileShaderMultiplier(m_parent, m_boundVS) * sign;
                    const float conv = ResolveShaderConvergence(m_parent, m_boundVS);
                    if (sign < 0.f) ++m_flippedWritesThisFrame;
                    ApplyTargetedEyeShiftToCB(tmp.data(), bd.ByteWidth, eyeShift, conv,
                                              targets, true, &m_cbMatShiftedThisFrame,
                                              &m_cbMatRejectedThisFrame,
                                              &m_invPatchedThisFrame, pDstResource);
                    if (m_cbMatShiftedThisFrame != before) bp->MarkShifted(s_frameId);
                    RightWriter()->UpdateSubresource(bp->GetRealRight(), DstSubresource,
                                                     nullptr, tmp.data(), 0, 0);
                    ++m_updCBPatchedThisFrame;
                    patched = true;
                }
                const unsigned inv = m_invPatchedThisFrame - invBeforeUpd;
                bp->NoteRightShift(m_cbMatShiftedThisFrame - shiftedBefore - inv, inv);
            }
            if (!patched)
                RightWriter()->UpdateSubresource(bp->GetRealRight(), DstSubresource, pDstBox,
                                                 pSrcData, SrcRowPitch, SrcDepthPitch);
            if (pDstBox || !pSrcData) bp->ForgetShadow();
            NoteRightConstants(bp);
        }
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

void Context11Proxy::DoResolveSubresource(
    ID3D11Resource* pDstResource, UINT DstSubresource,
    ID3D11Resource* pSrcResource, UINT SrcSubresource, DXGI_FORMAT Format)
{
    FlushRightBatch(kFlushWrite);
    pDstResource = GameResource(pDstResource);
    pSrcResource = GameResource(pSrcResource);
    bool pickRight = (m_activeEye == Eye::Right);
    m_real->ResolveSubresource(
        UnwrapResourceForEye(pDstResource, pickRight), DstSubresource,
        UnwrapResourceForEye(pSrcResource, pickRight), SrcSubresource, Format);
    if (ID3D11Resource* rd = RightMirrorTarget(pDstResource, pickRight))
    {
        m_real->ResolveSubresource(rd, DstSubresource, RightEyeResource(pSrcResource), SrcSubresource, Format);
        if (auto* t = TryUnwrapTexture2D(pDstResource)) t->MarkRightWritten();
    }
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
    FlushRightBatch(kFlushWrite);
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
    FlushRightBatch(kFlushWrite);
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
    MIRROR(IASetInputLayout(pInputLayout));
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
    MIRROR(IASetVertexBuffers(StartSlot, NumBuffers, ppVertexBuffers ? rawVBs : nullptr, pStrides, pOffsets));
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
    MIRROR(IASetIndexBuffer(UnwrapBuf(pIndexBuffer), Format, Offset));
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
    m_topology = Topology;
    m_real->IASetPrimitiveTopology(Topology);
    MIRROR(IASetPrimitiveTopology(Topology));
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
    MIRROR(RSSetState(pRasterizerState));
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
    MIRROR(RSSetScissorRects(NumRects, pRects));
    m_gameScCount = 0;
    for (UINT i = 0; pRects && i < NumRects && i < D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE; ++i)
        m_gameSc[m_gameScCount++] = pRects[i];
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
    MIRROR(OMSetBlendState(pBlendState, BlendFactor, SampleMask));
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
    MIRROR(OMSetDepthStencilState(pDepthStencilState, StencilRef));
    // The default state (null) tests and writes depth.
    m_depthTestOn = m_depthWriteOn = true;
    if (pDepthStencilState)
    {
        D3D11_DEPTH_STENCIL_DESC dd = {};
        pDepthStencilState->GetDesc(&dd);
        m_depthTestOn  = dd.DepthEnable != FALSE;
        m_depthWriteOn = dd.DepthEnable != FALSE && dd.DepthWriteMask != D3D11_DEPTH_WRITE_MASK_ZERO;
    }
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
    // BatchRightEye: no recording spans a stream-output binding (the recorder never sets one); unbinding needs nothing.
    for (UINT i = 0; ppSOTargets && i < NumBuffers; ++i) if (ppSOTargets[i]) { FlushRightBatch(kFlushState); break; }
    // Stage 3c.1: unwrap each SO target before forwarding to D3D11.
    ID3D11Buffer* rawSOs[kMaxSOBuffers] = { 0 };
    UINT cap = NumBuffers <= kMaxSOBuffers ? NumBuffers : kMaxSOBuffers;
    m_soBound = false;
    for (UINT i = 0; i < cap; ++i)
    {
        rawSOs[i] = ppSOTargets ? UnwrapBuf(ppSOTargets[i]) : nullptr;
        if (rawSOs[i]) m_soBound = true;   // BatchRightEye: such draws run unrecorded, and the recording binds no SO target
    }
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
    MIRROR(SetPredication(pPredicate, PredicateValue));
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
    TrackCSUAVs(StartSlot, NumUAVs, ppUnorderedAccessViews, pUAVInitialCounts);
    // Stage 3c.2: unwrap UAVs eye-aware before forwarding.
    ID3D11UnorderedAccessView* rawSet[kMaxUAVs] = { 0 };
    UINT setCap = NumUAVs <= kMaxUAVs ? NumUAVs : kMaxUAVs;
    bool pickRight = (m_activeEye == Eye::Right);
    for (UINT i = 0; i < setCap; ++i)
        rawSet[i] = UnwrapUAVForEye(ppUnorderedAccessViews ? ppUnorderedAccessViews[i]
                                                           : nullptr, pickRight);
    m_real->CSSetUnorderedAccessViews(StartSlot, NumUAVs,
        ppUnorderedAccessViews ? rawSet : nullptr, pUAVInitialCounts);
    if (m_mirror)
    {
        UINT keep[kMaxUAVs];
        for (UINT i = 0; i < kMaxUAVs; ++i) keep[i] = (UINT)-1;   // counters stay where the immediate bind set them
        m_mirror->CSSetUnorderedAccessViews(StartSlot, NumUAVs, ppUnorderedAccessViews ? rawSet : nullptr,
                                            pUAVInitialCounts ? keep : nullptr);
    }
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

} // namespace wiz3d
