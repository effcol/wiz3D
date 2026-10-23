/*
* Project : wiz3D
* Copyright (C) Akshay Hegde 2026
*/

// Stereo decisions, as DX9's BaseStereoRenderer_Main.cpp: whether and how each draw and dispatch runs for the right eye
// (bindings tracked per eye, shared-UAV twins, eye binds) and the constant-buffer matrix shifts it draws with.

#include "StdAfx.h"
#include "Context11Proxy-inl.h"

namespace wiz3d
{

void Context11Proxy::BeginDraw(UINT vertexCount)
{
    ScopedTicks t_(m_time.leftDraws);
    ++m_drawsThisFrame;
    m_drawVertices = vertexCount;
    if (m_activeEye != Eye::Right)
    {
        TeachBoundMatrices(vertexCount <= 6);
        if (gInfo.CollectDebugInformation) { ScopedTicks c_(m_time.coverageTally); TallyDrawCoverage(); }
    }
    if (m_activeEye != Eye::Right && ConstantBlocksOn()) SyncBoundConstants(false, false, false);
    for (auto& f : Features()) f->OnLeftDraw(*this);
}

// The bound VS's matrix registers, taught to the buffers at its slots: writes are attributed by the shader bound then,
// and Far Cry 4 fills its buffers long before binding them. Not from a fullscreen pass, whose matrices are not the camera's.
void Context11Proxy::TeachBoundMatrices(bool screenSpace)
{
    const ShaderAnalysis11Result* info = m_boundInfo[0];
    if (screenSpace || !info || !info->parsed) return;
    for (const auto& cb : info->projection.matrixData.cb)
    {
        if (cb.first >= kMaxVSCBSlots || !m_boundVSCBs[cb.first]) continue;
        Buffer11Proxy* b = TryUnwrapBuffer(m_boundVSCBs[cb.first]);
        if (!b) continue;
        for (const auto& pmd : cb.second)
        {
            if (pmd.incorrectProjection) continue;
            b->NoteMatrixUse(pmd.matrixRegister, LightViewPass());
            Buffer11Proxy::MatrixTarget mt = { pmd.matrixRegister, pmd.matrixIsTransposed, FALSE };
            b->LearnMatrix(mt);
        }
    }
}

// Everything reads as unbound: after ClearState, a state-clearing ExecuteCommandList or a state swap.
void Context11Proxy::ResetEyeTracking()
{
    ++m_bindResetsThisFrame;
    for (int st = 0; st < ST_COUNT; ++st)
    {
        SRVEyeSlots& s = m_srvSlots[st];
        for (UINT i = 0; i < s.high; ++i) { DropRef(s.left[i]); DropRef(s.right[i]); DropRef(s.game[i]); }
        CBEyeSlots& c = m_cbSlots[st];
        for (UINT i = 0; i < c.high; ++i) { DropRef(c.left[i]); DropRef(c.right[i]); DropRef(c.proxy[i]); DropRef(c.game[i]); }
    }
    for (UINT i = 0; i < kMaxRTVs; ++i) { DropRef(m_rtvLeft[i]); DropRef(m_rtvRight[i]); DropRef(m_rtvGame[i]); }
    DropRef(m_dsvLeft);
    DropRef(m_dsvRight);
    DropRef(m_dsvGame);
    for (UINT i = 0; i < m_csUAVSlots.high; ++i)
    { DropRef(m_csUAVSlots.left[i]); DropRef(m_csUAVSlots.right[i]); DropRef(m_csUAVSlots.game[i]); }
    for (UINT i = 0; i < kMaxVSCBSlots; ++i) DropRef(m_boundVSCBs[i]);
    for (UINT i = 0; i < kMaxPSCBSlots; ++i) DropRef(m_boundPSCBs[i]);
    ClearOmUavRefs();
    memset(m_srvSlots, 0, sizeof(m_srvSlots));
    memset(m_cbSlots,  0, sizeof(m_cbSlots));
    m_csUAVSlots  = {};
    m_numRTVs     = 0;
    m_numRTVGame  = 0;
    m_omAnyStereo = false;
    m_omHasUAVs   = false;
    m_omMonoDSV   = false;
    m_omUavBound  = false;
    m_omBackBuffer = false;
    m_depthTestOn  = true;
}

void Context11Proxy::TrackSRVs(StageIdx stage, UINT StartSlot, UINT NumViews,
                               ID3D11ShaderResourceView* const* ppSRVs)
{
    ScopedTicks t_(m_time.tracking);
    if (!gInfo.DuplicateDraws || stage >= ST_COUNT) return;
    SRVEyeSlots& s = m_srvSlots[stage];
    for (UINT i = 0; i < NumViews; ++i)
    {
        UINT slot = StartSlot + i;
        if (slot >= kMaxSRVs) break;
        ID3D11ShaderResourceView* p = ppSRVs ? ppSRVs[i] : nullptr;
        HoldRef(s.left[slot],  UnwrapSRVForEye(p, false));
        HoldRef(s.right[slot], UnwrapSRVForEye(p, true));
        HoldRef(s.game[slot],  p);
        if (slot + 1 > s.high) s.high = slot + 1;
    }
    s.anyStereo = false;
    for (UINT i = 0; i < s.high; ++i)
        if (s.left[i] != s.right[i]) { s.anyStereo = true; break; }
}

void Context11Proxy::TrackCBs(StageIdx stage, UINT StartSlot, UINT NumBuffers,
                              ID3D11Buffer* const* ppCBs, const UINT* pFirst, const UINT* pNum)
{
    ScopedTicks t_(m_time.tracking);
    if (!gInfo.DuplicateDraws || stage >= ST_COUNT) return;
    CBEyeSlots& s = m_cbSlots[stage];
    for (UINT i = 0; i < NumBuffers; ++i)
    {
        UINT slot = StartSlot + i;
        if (slot >= kMaxCBs) break;
        ID3D11Buffer* p = ppCBs ? ppCBs[i] : nullptr;
        ID3D11Buffer* left  = p;
        ID3D11Buffer* right = p;
        Buffer11Proxy* proxy = nullptr;
        if (p)
        {
            if (auto* bp = TryUnwrapBuffer(static_cast<ID3D11Resource*>(p)))
            {
                proxy = bp;
                left  = bp->GetReal();
                right = bp->GetRealRight() ? bp->GetRealRight() : left;
            }
        }
        HoldRef(s.left[slot],  left);
        HoldRef(s.right[slot], right);
        HoldRef(s.proxy[slot], proxy);
        HoldRef(s.game[slot],  p);
        s.first[slot] = pFirst ? pFirst[i] : 0;
        s.num[slot]   = pNum ? pNum[i] : 0;
        s.size[slot]  = 0;
        s.syncSeq[0][slot] = s.syncSeq[1][slot] = ~0u;
        if (left)
        {
            D3D11_BUFFER_DESC bd = {};
            left->GetDesc(&bd);
            s.size[slot] = bd.ByteWidth / 16;
        }
        if (slot + 1 > s.high) s.high = slot + 1;
    }
    s.anyStereo = false;
    s.anyRange = false;
    for (UINT i = 0; i < s.high; ++i)
    {
        if (s.left[i] != s.right[i]) s.anyStereo = true;
        if (s.num[i]) s.anyRange = true;
    }
}

void Context11Proxy::TrackOM(UINT NumViews, ID3D11RenderTargetView* const* ppRTVs,
                             ID3D11DepthStencilView* pDSV)
{
    ScopedTicks t_(m_time.tracking);
    if (!gInfo.DuplicateDraws) return;
    m_omHasUAVs = false;
    // A plain OMSetRenderTargets unbinds any output-merger UAVs.
    m_omUavBound = false;
    ClearOmUavRefs();
    m_numRTVs = NumViews <= kMaxRTVs ? NumViews : kMaxRTVs;
    bool anyRTVStereo = false;
    for (UINT i = 0; i < kMaxRTVs; ++i)
    {
        ID3D11RenderTargetView* p = (ppRTVs && i < m_numRTVs) ? ppRTVs[i] : nullptr;
        ID3D11RenderTargetView* left = p;
        // Mono targets must be written exactly once, so the right-eye pass
        // leaves them unbound rather than blending into them a second time.
        ID3D11RenderTargetView* right = nullptr;
        if (auto* rp = TryUnwrapRTV(p))
        {
            left  = rp->GetReal();
            right = rp->GetRealRight();
            if (right) anyRTVStereo = true;
        }
        HoldRef(m_rtvLeft[i], left);
        HoldRef(m_rtvRight[i], right);
    }
    // The first target is the swap chain's back buffer: an overlay pass once the depth test is off.
    m_omBackBuffer = false;
    if (RTV11Proxy* r0 = (ppRTVs && m_numRTVs) ? TryUnwrapRTV(ppRTVs[0]) : nullptr)
    {
        ID3D11Resource* res = nullptr;
        r0->GetReal()->GetResource(&res);
        if (res)
        {
            Texture2D11Proxy* tp = m_parent ? m_parent->LookupProxyByReal(res) : nullptr;
            m_omBackBuffer = tp && tp->IsBackBuffer();
            res->Release();
        }
    }
    // A mono depth-stencil stays bound for the right pass: losing depth testing is worse than testing against the
    // depth the left pass wrote (which then also takes the right pass's writes).
    ID3D11DepthStencilView* dsvLeft = pDSV;
    ID3D11DepthStencilView* dsvRight = pDSV;
    bool dsvStereo = false;
    if (auto* dp = TryUnwrapDSV(pDSV))
    {
        dsvLeft   = dp->GetReal();
        dsvRight  = dp->GetRealRight() ? dp->GetRealRight() : dp->GetReal();
        dsvStereo = (dp->GetRealRight() != nullptr);
    }
    HoldRef(m_dsvLeft, dsvLeft);
    HoldRef(m_dsvRight, dsvRight);
    m_omMonoDSV   = (pDSV != nullptr) && !dsvStereo;
    m_omAnyStereo = anyRTVStereo || dsvStereo;

    for (auto& f : Features()) f->OnTargetsSet(*this);
}

void Context11Proxy::RecordGameFacingOM(UINT NumViews,
                                        ID3D11RenderTargetView* const* ppRTVs,
                                        ID3D11DepthStencilView* pDSV)
{
    m_numRTVGame = NumViews <= kMaxRTVs ? NumViews : kMaxRTVs;
    for (UINT i = 0; i < kMaxRTVs; ++i)
        HoldRef(m_rtvGame[i], (ppRTVs && i < m_numRTVGame) ? ppRTVs[i] : nullptr);
    HoldRef(m_dsvGame, pDSV);
}

// The getters answer from the tracked game-facing bindings; with duplication off nothing is tracked and the real
// context answers as before.
void Context11Proxy::GetTrackedCBs(StageIdx stage, UINT StartSlot, UINT NumBuffers, ID3D11Buffer** pp,
                                   UINT* pFirst, UINT* pNum)
{
    if (!gInfo.DuplicateDraws || stage >= ST_COUNT)
    {
        const bool ranged = (pFirst || pNum) && m_real1;
        switch (stage)
        {
        case ST_VS: if (ranged) m_real1->VSGetConstantBuffers1(StartSlot, NumBuffers, pp, pFirst, pNum); else m_real->VSGetConstantBuffers(StartSlot, NumBuffers, pp); break;
        case ST_PS: if (ranged) m_real1->PSGetConstantBuffers1(StartSlot, NumBuffers, pp, pFirst, pNum); else m_real->PSGetConstantBuffers(StartSlot, NumBuffers, pp); break;
        case ST_GS: if (ranged) m_real1->GSGetConstantBuffers1(StartSlot, NumBuffers, pp, pFirst, pNum); else m_real->GSGetConstantBuffers(StartSlot, NumBuffers, pp); break;
        case ST_HS: if (ranged) m_real1->HSGetConstantBuffers1(StartSlot, NumBuffers, pp, pFirst, pNum); else m_real->HSGetConstantBuffers(StartSlot, NumBuffers, pp); break;
        case ST_DS: if (ranged) m_real1->DSGetConstantBuffers1(StartSlot, NumBuffers, pp, pFirst, pNum); else m_real->DSGetConstantBuffers(StartSlot, NumBuffers, pp); break;
        case ST_CS: if (ranged) m_real1->CSGetConstantBuffers1(StartSlot, NumBuffers, pp, pFirst, pNum); else m_real->CSGetConstantBuffers(StartSlot, NumBuffers, pp); break;
        default: break;
        }
        return;
    }
    ++m_getCBsThisFrame;
    const CBEyeSlots& s = m_cbSlots[stage];
    for (UINT i = 0; i < NumBuffers; ++i)
    {
        const UINT slot = StartSlot + i;
        ID3D11Buffer* b = slot < kMaxCBs ? s.game[slot] : nullptr;
        if (b) b->AddRef();
        if (pp) pp[i] = b;
        // A whole-buffer bind reports its full constant count, as the runtime does.
        if (pFirst) pFirst[i] = b ? s.first[slot] : 0;
        if (pNum)   pNum[i]   = b ? (s.num[slot] ? s.num[slot] : s.size[slot]) : 0;
    }
}

void Context11Proxy::GetTrackedSRVs(StageIdx stage, UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView** pp)
{
    if (!gInfo.DuplicateDraws || stage >= ST_COUNT)
    {
        switch (stage)
        {
        case ST_VS: m_real->VSGetShaderResources(StartSlot, NumViews, pp); break;
        case ST_PS: m_real->PSGetShaderResources(StartSlot, NumViews, pp); break;
        case ST_GS: m_real->GSGetShaderResources(StartSlot, NumViews, pp); break;
        case ST_HS: m_real->HSGetShaderResources(StartSlot, NumViews, pp); break;
        case ST_DS: m_real->DSGetShaderResources(StartSlot, NumViews, pp); break;
        case ST_CS: m_real->CSGetShaderResources(StartSlot, NumViews, pp); break;
        default: break;
        }
        return;
    }
    ++m_getSRVsThisFrame;
    const SRVEyeSlots& s = m_srvSlots[stage];
    for (UINT i = 0; i < NumViews; ++i)
    {
        const UINT slot = StartSlot + i;
        ID3D11ShaderResourceView* v = slot < kMaxSRVs ? s.game[slot] : nullptr;
        if (v) v->AddRef();
        if (pp) pp[i] = v;
    }
}

void Context11Proxy::GetTrackedUAVs(UINT StartSlot, UINT NumUAVs, ID3D11UnorderedAccessView** pp)
{
    if (!gInfo.DuplicateDraws) { m_real->CSGetUnorderedAccessViews(StartSlot, NumUAVs, pp); return; }
    ++m_getUAVsThisFrame;
    for (UINT i = 0; i < NumUAVs; ++i)
    {
        const UINT slot = StartSlot + i;
        ID3D11UnorderedAccessView* v = slot < kMaxUAVs ? m_csUAVSlots.game[slot] : nullptr;
        if (v) v->AddRef();
        if (pp) pp[i] = v;
    }
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
                                 ID3D11UnorderedAccessView* const* pp, const UINT* initialCounts)
{
    ScopedTicks t_(m_time.tracking);
    UAVEyeSlots& s = m_csUAVSlots;
    for (UINT i = 0; i < NumUAVs; ++i)
    {
        const UINT slot = StartSlot + i;
        if (slot >= kMaxUAVs) break;
        ID3D11UnorderedAccessView* v = pp ? pp[i] : nullptr;
        HoldRef(s.left[slot],  UnwrapUAVForEye(v, false));
        HoldRef(s.right[slot], UnwrapUAVForEye(v, true));
        HoldRef(s.game[slot],  v);
        if (slot + 1 > s.high) s.high = slot + 1;
        m_csInitCount[slot] = initialCounts ? initialCounts[i] : ~0u;
        m_csInitFresh[slot] = initialCounts != nullptr;
    }
    s.anyStereo = false;
    for (UINT i = 0; i < s.high; ++i)
        if (s.left[i] != s.right[i]) { s.anyStereo = true; break; }
}

void Context11Proxy::BindCSUAVs(bool right)
{
    UAVEyeSlots& s = m_csUAVSlots;
    if (!s.high) return;
    if (!right)
    {
        m_real->CSSetUnorderedAccessViews(0, s.high, s.left, nullptr);
        return;
    }
    // A shared UAV (no right sibling) is its twin, copied before the left dispatch: the right reads what the left read
    // and its writes stay out of the left eye's buffers.
    ID3D11UnorderedAccessView* uavs[kMaxUAVs] = {};
    unsigned shared = 0;
    for (UINT i = 0; i < s.high; ++i)
    {
        const bool isShared = s.left[i] && s.right[i] == s.left[i];
        uavs[i] = isShared ? (m_csTwinsPrepared ? m_csTwin[i] : nullptr) : s.right[i];
        if (isShared) ++shared;
        // The right dispatch writes this texture's sibling: from here on the right eye reads its own copy.
        else if (s.right[i])
            if (auto* up = wiz3d::TryUnwrapUAV(s.game[i]))
                if (up->Texture()) up->Texture()->MarkRightWritten();
    }
    if (shared)
    {
        ++m_sharedUavDispatchesThisFrame;
        m_sharedUavSlotsThisFrame += shared;
        ++m_sharedUavByCS[m_boundInfo[2] ? m_boundInfo[2]->crc32 : 0];
    }
    m_real->CSSetUnorderedAccessViews(0, s.high, uavs, m_csTwinsPrepared ? m_csTwinCount : nullptr);
    m_csTwinsPrepared = false;
}

// The UAV slots a compute shader reads (loads, or atomics that return the old value), from its disassembly once per CRC;
// all ones when unknown. A slot it only writes needs no twin copy: the right's writes are dropped (null view).
static unsigned long long CsUavReadMask(const ShaderAnalysis11Result* info)
{
    if (!info) return ~0ull;
    static std::mutex s_lock;
    static std::map<DWORD, unsigned long long> s_masks;
    {
        std::lock_guard<std::mutex> g(s_lock);
        auto it = s_masks.find(info->crc32);
        if (it != s_masks.end()) return it->second;
    }
    unsigned long long mask = ~0ull;
    ID3DBlob* text = nullptr;
    if (info->bytecode && !info->bytecode->empty() &&
        SUCCEEDED(D3DDisassemble(info->bytecode->data(), info->bytecode->size(), 0, nullptr, &text)) && text)
    {
        mask = 0;
        std::istringstream in(std::string((const char*)text->GetBufferPointer(), text->GetBufferSize()));
        std::string line;
        while (std::getline(in, line))
        {
            const size_t a = line.find_first_not_of(' ');
            if (a == std::string::npos) continue;
            const std::string op = line.substr(a, line.find_first_of(" (", a) - a);
            if (op.compare(0, 12, "ld_uav_typed") && op.compare(0, 6, "ld_raw") && op.compare(0, 13, "ld_structured") &&
                op.compare(0, 10, "imm_atomic"))
                continue;
            for (size_t p = line.find('u', a + op.size()); p != std::string::npos; p = line.find('u', p + 1))
            {
                if (p > 0 && (isalnum((unsigned char)line[p - 1]) || line[p - 1] == '_')) continue;
                if (p + 1 >= line.size() || !isdigit((unsigned char)line[p + 1])) continue;
                const int slot = atoi(line.c_str() + p + 1);
                if (slot >= 0 && slot < 64) mask |= 1ull << slot;
            }
        }
        text->Release();
    }
    std::lock_guard<std::mutex> g(s_lock);
    s_masks[info->crc32] = mask;
    return mask;
}

// Before a left dispatch the right eye repeats: each shared UAV the shader reads is copied to its twin (write-only ones
// get none); the right's counters start at the game's bind-time counts while those are still fresh.
void Context11Proxy::PrepareSharedUavTwins()
{
    UAVEyeSlots& s = m_csUAVSlots;
    const bool repeats = gInfo.DuplicateDraws && gInfo.Input.StereoActive && m_activeEye == Eye::Left && s.anyStereo;
    const unsigned long long reads = repeats ? CsUavReadMask(m_boundInfo[2]) : 0;
    for (UINT i = 0; i < s.high; ++i)
    {
        m_csTwin[i] = nullptr;
        m_csTwinCount[i] = m_csInitFresh[i] ? m_csInitCount[i] : ~0u;
        if (!repeats || !s.left[i] || s.right[i] != s.left[i]) continue;
        if (i < 64 && !(reads & (1ull << i))) { ++m_twinWriteOnlyThisFrame; continue; }
        m_csTwin[i] = TwinFor(s.left[i]);
        if (!m_csTwin[i]) ++m_twinFailedThisFrame;
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {};
        s.left[i]->GetDesc(&ud);
        if (ud.ViewDimension == D3D11_UAV_DIMENSION_BUFFER &&
            (ud.Buffer.Flags & (D3D11_BUFFER_UAV_FLAG_APPEND | D3D11_BUFFER_UAV_FLAG_COUNTER)) && !m_csInitFresh[i])
            ++m_twinCounterUnknownThisFrame;
    }
    for (UINT i = 0; i < s.high; ++i) m_csInitFresh[i] = false;
    m_csTwinsPrepared = repeats;
}

ID3D11UnorderedAccessView* Context11Proxy::TwinFor(ID3D11UnorderedAccessView* v)
{
    ID3D11Device* dev = m_parent ? m_parent->GetReal() : nullptr;
    ID3D11Resource* src = nullptr;
    v->GetResource(&src);
    if (!src || !dev) { if (src) src->Release(); return nullptr; }
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {};
    v->GetDesc(&ud);
    UavTwin& t = m_uavTwins[v];
    if (t.uav && (t.src != src || memcmp(&t.ud, &ud, sizeof(ud)) != 0))
    {
        t.uav->Release();
        t.res->Release();
        t = UavTwin();
    }
    if (!t.uav)
    {
        D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
        src->GetType(&dim);
        const UINT noShare = ~(UINT)(D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX |
                                     D3D11_RESOURCE_MISC_GDI_COMPATIBLE);
        HRESULT hr = E_FAIL;
        if (dim == D3D11_RESOURCE_DIMENSION_BUFFER)
        {
            D3D11_BUFFER_DESC d = {};
            static_cast<ID3D11Buffer*>(src)->GetDesc(&d);
            d.MiscFlags &= noShare;
            hr = dev->CreateBuffer(&d, nullptr, reinterpret_cast<ID3D11Buffer**>(&t.res));
            t.bytes = d.ByteWidth;
        }
        else if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D)
        {
            D3D11_TEXTURE2D_DESC d = {};
            static_cast<ID3D11Texture2D*>(src)->GetDesc(&d);
            d.MiscFlags &= noShare;
            hr = dev->CreateTexture2D(&d, nullptr, reinterpret_cast<ID3D11Texture2D**>(&t.res));
            t.bytes = (UINT64)d.Width * d.Height * d.ArraySize * 4;
        }
        else if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE3D)
        {
            D3D11_TEXTURE3D_DESC d = {};
            static_cast<ID3D11Texture3D*>(src)->GetDesc(&d);
            d.MiscFlags &= noShare;
            hr = dev->CreateTexture3D(&d, nullptr, reinterpret_cast<ID3D11Texture3D**>(&t.res));
            t.bytes = (UINT64)d.Width * d.Height * d.Depth * 4;
        }
        else if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE1D)
        {
            D3D11_TEXTURE1D_DESC d = {};
            static_cast<ID3D11Texture1D*>(src)->GetDesc(&d);
            d.MiscFlags &= noShare;
            hr = dev->CreateTexture1D(&d, nullptr, reinterpret_cast<ID3D11Texture1D**>(&t.res));
            t.bytes = (UINT64)d.Width * d.ArraySize * 4;
        }
        if (SUCCEEDED(hr) && t.res && FAILED(dev->CreateUnorderedAccessView(t.res, &ud, &t.uav))) t.uav = nullptr;
        if (!t.uav)
        {
            if (t.res) t.res->Release();
            t = UavTwin();
            src->Release();
            m_uavTwins.erase(v);
            return nullptr;
        }
        t.src = src;
        t.ud = ud;
    }
    m_real->CopyResource(t.res, src);
    src->Release();
    t.lastFrame = s_frameId;
    ++m_twinCopiesThisFrame;
    m_twinBytesThisFrame += t.bytes;
    return t.uav;
}

// Twins whose UAV the game has not dispatched with for a while (a freed view's address may come back as another view).
void Context11Proxy::ReleaseUavTwins(unsigned olderThan)
{
    for (auto it = m_uavTwins.begin(); it != m_uavTwins.end();)
    {
        if (olderThan == ~0u || s_frameId - it->second.lastFrame > olderThan)
        {
            if (it->second.uav) it->second.uav->Release();
            if (it->second.res) it->second.res->Release();
            it = m_uavTwins.erase(it);
        }
        else ++it;
    }
}

// Compute output has no right-eye copy unless the dispatch runs again with the right-eye UAVs bound (GTA V drives
// its lighting and post-processing through compute).
bool Context11Proxy::BeginRightEyeDispatch()
{
    if (!gInfo.DuplicateDraws) return false;
    if (!gInfo.Input.StereoActive) return false;
    if (m_activeEye != Eye::Left) return false;
    if (!m_csUAVSlots.anyStereo) return false;
    m_activeEye = Eye::Right;
    BindCSUAVs(true);
    for (UINT st = 0; st < ST_COUNT; ++st)
    {
        if (m_srvSlots[st].anyStereo) BindStageSRVs((StageIdx)st, true);
        if (m_cbSlots[st].anyStereo)  BindStageCBs((StageIdx)st, true);
    }
    // Compute passes that feed the scene (shadow and lighting inputs) need the right camera too: under ConstantBlocks
    // with draw-time patching, through the bound blocks.
    const bool blocks = ConstantBlocksOn();
    if ((!blocks || !gInfo.PatchConstantsAtDraw) && gInfo.DetectInverseMatrix) PatchCameraMatrices(ST_CS);
    if (blocks) SyncBoundConstants(true, false, true);
    EditRightCBs(true);
    BindLeftSRVs(true);
    for (auto& f : Features()) f->OnRightDispatch(*this);
    return true;
}

void Context11Proxy::EndRightEyeDispatch()
{
    UnbindTwin();   // a shader twin bound for this dispatch: the game's shader and slot come back
    m_activeEye = Eye::Left;
    BindCSUAVs(false);
    for (UINT st = 0; st < ST_COUNT; ++st)
    {
        if (m_srvSlots[st].anyStereo) BindStageSRVs((StageIdx)st, false);
        if (m_cbSlots[st].anyStereo)  BindStageCBs((StageIdx)st, false);
    }
    // A feature's substitute for one right dispatch: the game's view goes back.
    for (UINT slot : m_substitutedCSSRVs)
    {
        ID3D11ShaderResourceView* l = m_srvSlots[ST_CS].left[slot];
        m_real->CSSetShaderResources(slot, 1, &l);
    }
    m_substitutedCSSRVs.clear();
}

void Context11Proxy::BindStageCBs(StageIdx stage, bool right)
{
    CBEyeSlots& s = m_cbSlots[stage];
    ID3D11Buffer* const* v = right ? s.right : s.left;
    if (s.anyRange && m_real1)
    {
        // A partial bind must be re-issued through the 11.1 entry; slots
        // bound whole spell out their full size (4096 constants max).
        UINT first[kMaxCBs], num[kMaxCBs];
        const bool roundUp = WholeBufferRanges();
        for (UINT i = 0; i < s.high; ++i)
        {
            first[i] = s.num[i] ? s.first[i] : 0;
            num[i]   = s.num[i] ? s.num[i] : (s.size[i] > 4096 ? 4096 : s.size[i]);
            // WholeBufferRanges: 11.1 drops the whole call unless every count is a multiple of 16 (reads past the end are 0).
            if (roundUp && !s.num[i] && num[i] < 4096) num[i] = (num[i] + 15) & ~15u;
            if (!num[i]) num[i] = 16;
        }
        switch (stage)
        {
        case ST_VS: m_real1->VSSetConstantBuffers1(0, s.high, v, first, num); break;
        case ST_PS: m_real1->PSSetConstantBuffers1(0, s.high, v, first, num); break;
        case ST_GS: m_real1->GSSetConstantBuffers1(0, s.high, v, first, num); break;
        case ST_HS: m_real1->HSSetConstantBuffers1(0, s.high, v, first, num); break;
        case ST_DS: m_real1->DSSetConstantBuffers1(0, s.high, v, first, num); break;
        case ST_CS: m_real1->CSSetConstantBuffers1(0, s.high, v, first, num); break;
        default: break;
        }
        return;
    }
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
    {
        if (m_omUavBound)
        {
            // Plain OMSetRenderTargets would unbind the UAVs; sibling-less
            // slots go null on the right so shared UAVs take only left writes.
            ID3D11UnorderedAccessView* uavs[kMaxUAVs] = {};
            for (UINT i = 0; i < m_omUavCount; ++i)
            {
                ID3D11UnorderedAccessView* l = UnwrapUAVForEye(m_omUavGame[i], false);
                ID3D11UnorderedAccessView* r = UnwrapUAVForEye(m_omUavGame[i], true);
                uavs[i] = right ? ((r != l) ? r : nullptr) : l;
                if (right && r != l)
                    if (auto* up = wiz3d::TryUnwrapUAV(m_omUavGame[i]))
                        if (up->Texture()) up->Texture()->MarkRightWritten();
            }
            // Initial counts once per game bind, or eye switches reset them.
            const UINT* counts = nullptr;
            if (right && m_omUavHasInit && !m_omUavRightInited)
            {
                counts = m_omUavInit;
                m_omUavRightInited = true;
            }
            m_real->OMSetRenderTargetsAndUnorderedAccessViews(
                m_numRTVs, right ? m_rtvRight : m_rtvLeft,
                right ? m_dsvRight : m_dsvLeft,
                m_omUavStart, m_omUavCount, uavs, counts);
        }
        else
            m_real->OMSetRenderTargets(m_numRTVs,
                                       right ? m_rtvRight : m_rtvLeft,
                                       right ? m_dsvRight : m_dsvLeft);
    }
    for (UINT st = 0; st < ST_COUNT; ++st)
    {
        if (m_srvSlots[st].anyStereo) BindStageSRVs((StageIdx)st, right);
        if (m_cbSlots[st].anyStereo)  BindStageCBs((StageIdx)st, right);
    }
}

void Context11Proxy::ClearOmUavRefs()
{
    for (UINT i = 0; i < m_omUavCount; ++i)
        if (m_omUavGame[i]) { m_omUavGame[i]->Release(); m_omUavGame[i] = nullptr; }
    m_omUavCount = 0;
}

bool Context11Proxy::BeginRightEyeDraw()
{
    if (!gInfo.DuplicateDraws) return false;
    // Stereo toggled off: skip the whole right-eye pass, not just the shift.
    if (!gInfo.Input.StereoActive) return false;
    if (m_activeEye != Eye::Left) return false;
    // A draw that also writes UAVs runs again only when one of its render targets is stereo; the UAVs stay shared.
    if (m_omHasUAVs)
    {
        if (!(m_omUavBound && m_omAnyStereo))
        {
            ++m_drawsUavSkippedThisFrame;
            if (FrameTraceActive()) FrameTrace("      explain: NOT duplicated (UAVs bound, no stereo target)\n");
            return false;
        }
        ++m_drawsUavDupThisFrame;
    }
    else if (!m_omAnyStereo)
    {
        ++m_drawsMonoThisFrame;
        if (FrameTraceActive()) FrameTrace("      explain: NOT duplicated (mono targets: both eyes see this one draw)\n");
        return false;
    }
    // BatchRightEye: recorded, unless it writes UAVs or stream output or tests a mono depth buffer the left eye is still
    // writing; those run now, after what was recorded.
    if (RightBatchOn() && !m_omHasUAVs && !m_soBound && !m_omMonoDSV)
    {
        BeginRightRecording();
        ++m_batchedDrawsThisFrame;
    }
    else if (m_recorder)
    {
        FlushRightBatch(kFlushUnbatched);
        ++m_unbatchedDrawsThisFrame;
    }
    // Before the right eye binds anything, its constants are patched for the shader about to run, not the one bound at
    // write time. An overlay (back buffer, no depth test) keeps the game's bytes, so the HUD stays at screen depth.
    const bool overlay = OverlayPass();
    if (overlay) ++m_overlayDrawsThisFrame;
    const bool blocks = ConstantBlocksOn();
    {
        ScopedTicks c_(m_time.rightConstants);
        if (!blocks || !gInfo.PatchConstantsAtDraw)
        {
            if (overlay) UnshiftOverlayCBs();
            else
            {
                if (gInfo.PatchConstantsAtDraw) PatchBoundCBsForDraw();
                // Under VertexCameraMatrices the vertex stage's declared camera matrices outside its position chain (last
                // frame's camera for per-object velocity), under ViewportMatrices its pixel matrices; then the pixel stage's.
                if (gInfo.DetectInverseMatrix && (VertexCameraMatrices() || ViewportMatricesOn())) PatchCameraMatrices(ST_VS);
                if (gInfo.DetectInverseMatrix) PatchCameraMatrices(ST_PS);
            }
        }
        // ConstantBlocks: every bound block the shaders read (small buffers under PatchConstantsAtDraw, ring ranges always)
        // rebuilt for the right eye in one place; an overlay takes the game's bytes.
        if (blocks)
        {
            // Recording a light-view draw: its geometry stages take the game's bytes (BindLeftGeometryConstants) instead.
            m_syncSkipGeometry = m_rightRecording && LightViewPass();
            SyncBoundConstants(true, overlay, false);
            m_syncSkipGeometry = false;
        }
    }
    ++m_drawsDuplicatedThisFrame;
    if (m_omMonoDSV) ++m_drawsMonoDsvThisFrame;
    // Counted apart: a duplicated draw whose right-eye RTVs are all null writes nowhere, while a depth-only pass (a
    // shadow map) binds no RTV and still reaches the right eye through its DSV.
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
    { ScopedTicks b_(m_time.rightBinds); BindEye(true); }
    ScopedTicks f_(m_time.rightFeatures);
    EditRightCBs(false);
    BindLeftSRVs(false);
    for (auto& f : Features()) f->OnRightDraw(*this);
    return true;
}

void Context11Proxy::EndRightEyeDraw()
{
    {
        ScopedTicks t_(m_time.rightTeardown);
        for (auto& f : Features()) f->OnRightDrawDone(*this);
        UnbindTwin();   // a shader twin a feature left bound: the game's shader and slot come back
    }
    // Recording keeps the right eye's targets and views bound until the replay ends (FlushRightBatch rebinds the left
    // eye's once): every right-eye draw binds all of them itself, so no draw reads the other eye's.
    if (!m_rightRecording) { ScopedTicks b_(m_time.rightBinds); BindEye(false); }
    m_activeEye = Eye::Left;
    if (m_rightRecording) EndRightRecording();
}

bool Context11Proxy::RightBatchOn() const
{
    return m_recorder && !FrameTraceActive();
}

// The right-eye draw goes to the recorder whole: its constant writes, binds, features and restores. The first one after
// a replay takes the immediate context's state for the next replay to start from.
void Context11Proxy::BeginRightRecording()
{
    if (!m_rightPending) { ScopedTicks b_(m_time.recordBegin); m_recorder->Begin(m_real, m_real1); }
    m_immediate = m_real;
    m_immediate1 = m_real1;
    m_real = m_recorder;
    m_real1 = m_recorder;
    m_mirror = nullptr;
    m_mirror1 = nullptr;
    m_rightRecording = m_rightPending = true;
}

void Context11Proxy::EndRightRecording()
{
    m_real = m_immediate;
    m_real1 = m_immediate1;
    m_mirror = m_recorder;
    m_mirror1 = m_recorder;
    m_rightRecording = false;
}

// The recorded right eye on the immediate context; a call the recorder could not take ends recording for the run.
void Context11Proxy::FlushRightBatch(BatchFlush reason)
{
    if (!m_rightPending || m_rightRecording) return;
    {
        ScopedTicks t_(m_time.rightFlush);
        m_rightPending = false;
        const unsigned draws = m_recorder->Draws();
        const UINT g = m_gpuTimer ? m_gpuTimer->Begin(m_real) : ~0u;
        m_recorder->Replay(m_real, m_real1);
        BindEye(false);   // the left eye's targets and views, which the recorded draws left on the right eye's
        if (m_gpuTimer) m_gpuTimer->End(m_real, g, GpuTimer11::kReplay, PassLabel(), draws);
        m_pendingLeftCBs.clear();
        ++m_batchFlushes[reason];
    }
    if (const char* what = m_recorder->Unexpected()) StopRightBatch(what);
}

std::string Context11Proxy::PassLabel() const
{
    ID3D11View* v = (m_numRTVs && m_rtvLeft[0]) ? static_cast<ID3D11View*>(m_rtvLeft[0]) : static_cast<ID3D11View*>(m_dsvLeft);
    if (!v) return "no target";
    ID3D11Resource* r = nullptr;
    v->GetResource(&r);
    if (!r) return "no target";
    D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    r->GetType(&dim);
    char buf[64] = "not 2D";
    if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D)
    {
        D3D11_TEXTURE2D_DESC d = {};
        static_cast<ID3D11Texture2D*>(r)->GetDesc(&d);
        sprintf_s(buf, "%s %ux%u f%u", v == m_dsvLeft ? "ds" : "rt", d.Width, d.Height, (unsigned)d.Format);
    }
    r->Release();
    return buf;
}

UINT Context11Proxy::GpuOutputBegin()
{
    return m_gpuTimer ? m_gpuTimer->Begin(m_real) : ~0u;
}

void Context11Proxy::GpuOutputEnd(UINT begin)
{
    if (m_gpuTimer) m_gpuTimer->End(m_real, begin, GpuTimer11::kOutput, std::string(), 0);
}

void Context11Proxy::StopRightBatch(const char* why)
{
    if (!m_recorder || m_rightRecording) return;
    FlushRightBatch(kFlushState);
    DDILog("BatchRightEye: recording stops (%s)\n", why);
    m_mirror = nullptr;
    m_mirror1 = nullptr;
    m_recorder->Release();
    m_recorder = nullptr;
}

// A right sibling written outside a right-eye draw while a recording is pending goes into it, so the draws recorded
// before the write keep the bytes they were recorded with.
ID3D11DeviceContext* Context11Proxy::RightWriter()
{
    return (m_mirror && m_rightPending) ? m_mirror : m_real;
}

// The profile's <VertexShader CRC Multiplier> scale for the bound shader's shift; Multiplier="0" is the classic
// crosshair fix (that shader's draws stay mono).
float ProfileShaderMultiplier(Device11Proxy* parent, void* vs)
{
    if (g_ProfileData.VSCRCData.empty()) return 1.f;
    const DWORD crc = BoundShaderCRC(parent, vs);
    if (!crc) return 1.f;
    ShaderProfileDataMap::const_iterator it = g_ProfileData.VSCRCData.find(crc);
    return (it != g_ProfileData.VSCRCData.end()) ? it->second.m_Multiplier : 1.f;
}

// Sibling to ProfileShaderMultiplier, reading the ConvergenceShift field a <VertexShader> entry may carry
// alongside Multiplier. Used by ResolveShaderConvergence to let specific shaders sit at a different
// zero-parallax depth from the global one -- DX9 parity (BaseStereoRenderer_Main.cpp:1037).
float ProfileShaderConvergenceShift(Device11Proxy* parent, void* vs)
{
    if (g_ProfileData.VSCRCData.empty()) return 0.f;
    const DWORD crc = BoundShaderCRC(parent, vs);
    if (!crc) return 0.f;
    ShaderProfileDataMap::const_iterator it = g_ProfileData.VSCRCData.find(crc);
    return (it != g_ProfileData.VSCRCData.end()) ? it->second.m_ConvergenceShift : 0.f;
}

// Matrix helpers live in Shared/StereoMatrix.h; these keep the call sites.
float EyeShiftB(float eyeShift) { return s_mat.EyeShiftB(eyeShift); }
using wiz3d::MatrixBasisNorms;
using wiz3d::BasesOrthogonal;
static float ProjXScaleFromMatrix(const float* f, bool transposed) { return s_mat.ProjXScaleFromMatrix(f, transposed); }

// Zero-parallax distance in world units, 0 with convergence off. The forward model is x += sep*(w - conv), so the
// forward and the inverse patch both read conv from here. The user's Shift+Numpad+/- hotkey adjusts
// GetActivePreset()->One_div_ZPS directly (KeyboardHook INCREASE_ZPS / DECREASE_ZPS), so convergence
// hotkey already drives this path. Per-shader-profile ConvergenceShift goes through
// ResolveShaderConvergence below (DX9 parity, BaseProfile.xml <VertexShader ConvergenceShift> wiring).
float StereoConvergence()
{
    const CameraPreset* p = gInfo.Input.GetActivePreset();
    return (p && p->One_div_ZPS != 0.f) ? 1.f / p->One_div_ZPS : 0.f;
}

// DX9 BaseStereoRenderer-inl.h:50 scales the shear coefficient A by (1/ZPS + shift)/(1/ZPS), which moves the
// zero-parallax point from 1/(1/ZPS) to 1/(1/ZPS + shift). Mirror that here so the DX11 CB patch (which shifts the
// camera translation by eyeShift*conv rather than scaling A directly) lands at the same world depth. Returns 0 for
// the degenerate case where the sum is zero (shift cancels global convergence); callers treat 0 as "no parallax point".
float ResolveShaderConvergence(Device11Proxy* parent, void* vs)
{
    const float shift = ProfileShaderConvergenceShift(parent, vs);
    if (shift == 0.f) return StereoConvergence();
    const CameraPreset* p = gInfo.Input.GetActivePreset();
    if (!p) return StereoConvergence();
    const float denom = p->One_div_ZPS + shift;
    return (denom != 0.f) ? 1.f / denom : 0.f;
}

// DX9 shears view space by x' = x + m31*z + m41 (BaseStereoRenderer-inl.h:141): eyeShift is m31, which only re-centres,
// and m41 is the eye offset whose 1/z falloff is the depth cue; A = -B * One_div_ZPS relates the two.
float EyeOffsetM41(float eyeShift)
{
    return -eyeShift * StereoConvergence();
}

// The core's stereo state, for profile features (ProfileFeature.h).
namespace stereo
{
float Convergence()               { return StereoConvergence(); }
float FallbackB(float eyeShift)   { return EyeShiftB(eyeShift); }
void  PublishCameraP00(float p00) { s_camP00 = p00; }
float SceneP00()                  { return s_camP00 > 0.f ? s_camP00 : s_sceneP00; }
float UiParallaxScale(float p00)
{
    const float ref = s_referenceP00;
    if (!s_uiProjectionParallax || !(ref > 0.f) || !(p00 > 0.f) || (p00 >= 0.5f * ref && p00 <= 2.f * ref)) return 1.f;
    return ref / p00;
}
bool  SceneZRow(float& m22, float& m23)
{
    if (!s_sceneZRowFrame || s_frameId - s_sceneZRowFrame > 1 || s_sceneZRow[1] == 0.f) return false;
    m22 = s_sceneZRow[0]; m23 = s_sceneZRow[1];
    return true;
}
}

// The analysis finds a projection-shaped matrix, not necessarily the camera's: orthographic (HUD) and light or shadow
// projections are rejected as the legacy DDI path does (ConstantBufferWrapper.cpp), under the same gInfo flags.
static bool ShouldSkipProjectionMatrix(const float* f, bool transposed)
{
    // Orthographic: no perspective divide, i.e. the row/column that produces w
    // is (0,0,0,non-zero). A camera projection puts +/-1 in one of those slots.
    if (!gInfo.SkipCheckOrthoMatrix)
    {
        const float w0 = transposed ? f[12] : f[3];
        const float w1 = transposed ? f[13] : f[7];
        const float w2 = transposed ? f[14] : f[11];
        const float w3 = f[15];
        // Exact zeros by default; CheckShadowMatrix allows the ~1e-8 a
        // concatenated light matrix carries, which otherwise read as perspective.
        const bool ortho = gInfo.CheckShadowMatrix
            ? (fabsf(w3) > 1e-12f &&
               (fabsf(w0) + fabsf(w1) + fabsf(w2)) <= 1e-4f * fabsf(w3))
            : (w0 == 0.f && w1 == 0.f && w2 == 0.f && w3 != 0.f);
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

// Numeric inverse-matrix detection (DetectInverseMatrix): the method is in Shared/StereoMatrix.h.
static void CacheForwardVP(const float* f, bool transposed) { s_mat.CacheForwardVP(f, transposed); }


// The projection's z row out of a clean view-projection: its w column is the view z axis (unit), so m22 is the z
// column's component along it and m23 what the translation row keeps beyond that.
bool ZRowOf(const float* f, bool transposed, float& m22, float& m23)
{
    float z[3], w[3], z3, w3;
    if (transposed) { z[0] = f[8]; z[1] = f[9]; z[2] = f[10]; w[0] = f[12]; w[1] = f[13]; w[2] = f[14]; z3 = f[11]; w3 = f[15]; }
    else            { z[0] = f[2]; z[1] = f[6]; z[2] = f[10]; w[0] = f[3];  w[1] = f[7];  w[2] = f[11]; z3 = f[14]; w3 = f[15]; }
    const float ww = w[0] * w[0] + w[1] * w[1] + w[2] * w[2];
    if (!(ww > 1e-12f)) return false;
    m22 = (z[0] * w[0] + z[1] * w[1] + z[2] * w[2]) / ww;
    m23 = z3 - w3 * m22;
    return fabsf(m22) < 1e30f && fabsf(m23) < 1e30f && fabsf(m23) > 1e-9f;
}

// The frame's last clean view-projection at the scene focal (a weapon or UI camera has its own near plane).
static void RecordSceneZRow(const float* f, bool transposed)
{
    float xn, wn;
    MatrixBasisNorms(f, transposed, xn, wn);
    const float scene = s_camP00 > 0.f ? s_camP00 : s_sceneP00;
    if (scene > 0.f && wn > 1e-6f && fabsf(xn / wn - scene) > 0.01f * scene) return;
    float m22, m23;
    if (!ZRowOf(f, transposed, m22, m23)) return;
    s_sceneZRow[0] = m22; s_sceneZRow[1] = m23; s_sceneZRowFrame = s_frameId;
}

// CameraInverses: the focal of the camera an unprojection inverts, 0 = not a camera's. The last cached focal is
// whatever camera drew before, which may be a light's or a UI view's.
static float InverseFocal(const float* f, bool transposed)
{
    float canon[16];
    wiz3d::CanonicalizeMat4(f, transposed, canon);
    D3DXMATRIX c(canon), inv;
    if (!D3DXMatrixInverse(&inv, nullptr, &c)) return 0.f;
    float xn, wn;
    MatrixBasisNorms(reinterpret_cast<const float*>(&inv), false, xn, wn);
    return (wn > 1e-6f && fabsf(wn - 1.f) < 0.05f) ? xn / wn : 0.f;
}

// outShifted / outRejected / outInvPatched count for the frame report (may be null). fullColumn (draw duplication):
// shift the whole first column and add the eye offset; else the replay's m[2][0] shear, its inverse likewise.
// conv is the effective zero-parallax distance for this draw -- normally StereoConvergence(), or a per-shader
// override from ResolveShaderConvergence when the bound VS carries a profile ConvergenceShift.
void ApplyTargetedEyeShiftToCB(unsigned char* data, size_t byteCount,
                               float eyeShift,
                               float conv,
                               const std::vector<EyeShiftMatrix>& matrices,
                               bool fullColumn,
                               unsigned* outShifted,
                               unsigned* outRejected,
                               unsigned* outInvPatched,
                               const void* srcBuffer)
{
    if (eyeShift == 0.f || matrices.empty()) return;
    constexpr size_t kRegBytes  = 16;
    constexpr size_t kMat4Bytes = 4 * kRegBytes;
    Buffer11Proxy* lightBuf = srcBuffer ? TryUnwrapBuffer(static_cast<ID3D11Resource*>(const_cast<void*>(srcBuffer))) : nullptr;
    for (const auto& m : matrices)
    {
        size_t base = size_t(m.matrixRegister) * kRegBytes;
        // A camera view: the right camera sits the eye offset along the view's x axis.
        if (m.viewComponent >= 0)
        {
            if (base + kRegBytes > byteCount) continue;
            reinterpret_cast<float*>(data + base)[m.viewComponent & 3] -= m.viewSign * eyeShift * conv;
            if (outShifted) ++*outShifted;
            continue;
        }
        if (base + kMat4Bytes > byteCount) continue;
        float* f = reinterpret_cast<float*>(data + base);
        const bool transposed = (m.matrixIsTransposed != 0);
        // ViewportMatrices: x * w gains s * (b * w + m41), s the viewport scale, so b and m41 take the focal in pixels; w
        // scaled by wn takes m41 scaled too. An unprojection takes the exact inverse of that.
        if (m.viewport)
        {
            float focal = 0.f, wn = 0.f;
            const bool ok = ViewportFocal(f, transposed, m.matrixIsInverse != 0, s_camP00 > 0.f ? s_camP00 : s_sceneP00, focal, wn);
            CountViewportPatch(m.matrixIsInverse != 0, ok);
            if (!ok) { if (outRejected) ++*outRejected; continue; }
            if (m.matrixIsInverse)
            {
                s_mat.PatchInverse(f, transposed, eyeShift, fullColumn ? conv * wn : 0.f, focal);
                if (outInvPatched) ++*outInvPatched;
            }
            else
            {
                const float b = eyeShift * focal, m41 = fullColumn ? -eyeShift * conv * focal * wn : 0.f;
                if (transposed) { f[0] += b * f[12]; f[1] += b * f[13]; f[2] += b * f[14]; f[3] += b * f[15] + m41; }
                else            { f[0] += b * f[3];  f[4] += b * f[7];  f[8] += b * f[11]; f[12] += b * f[15] + m41; }
            }
            if (outShifted) ++*outShifted;
            continue;
        }
        // Only shadow passes read it as a position matrix: a light's view, the same for both eyes.
        if (lightBuf && !m.matrixIsInverse && lightBuf->LightOnlyMatrix(m.matrixRegister)) { ++s_lightOnlyKept; continue; }

        // Inverse (unprojection) matrices take the exact inverse of the whole forward shift (PatchInverse). No guard
        // below applies to them, so a learned one must re-match the ring; a profile-declared one is trusted.
        if (m.matrixIsInverse)
        {
            bool shapeReject = false;
            if (m.matrixFromLearned && !s_mat.VerifyInverseAgainstRing(f, transposed, &shapeReject))
            {
                ++s_mat.invStaleThisFrame;
                if (shapeReject) ++s_mat.invStaleShapeThisFrame;
                if (outRejected) ++*outRejected;
                continue;
            }
            s_mat.PatchInverse(f, transposed, eyeShift, fullColumn ? conv : 0.f,
                               CameraInversesOn() ? InverseFocal(f, transposed) : 0.f);
            if (outShifted) ++*outShifted;
            if (outInvPatched) ++*outInvPatched;
            continue;
        }

        float xScale = f[0];
        if (xScale == 0.f) continue;
        // A recalled target is a guess about a buffer that may be pooled and now hold something else, so it needs the
        // unit w basis of a real view-projection (a near-zero |w| is a HUD's orthographic matrix).
        if (m.matrixFromLearned)
        {
            float lxn, lwn;
            MatrixBasisNorms(f, transposed, lxn, lwn);
            if (fabsf(lwn - 1.f) >= 0.05f)
            {
                if (outRejected) ++*outRejected;
                continue;
            }
            // And orthogonal x and w bases: UE3 reuses one buffer per slot, so a learned register also lands on
            // material blocks {color, unit direction} that pass the w test.
            if (!BasesOrthogonal(f, transposed, lxn, lwn))
            {
                if (outRejected) ++*outRejected;
                continue;
            }
        }
        if (ShouldSkipProjectionMatrix(f, transposed))
        {
            if (outRejected) ++*outRejected;
            continue;
        }
        // The camera scale and the detector's ring take only clean view-projections (unit w, orthogonal bases): no
        // scaled WVP, light projection or UE3 material block. Pre-shift values: the game derives its inverse from them.
        float basisXn, basisWn;
        MatrixBasisNorms(f, transposed, basisXn, basisWn);
        if (basisWn > 1e-6f && fabsf(basisWn - 1.f) < 0.05f &&
            BasesOrthogonal(f, transposed, basisXn, basisWn))
        {
            s_mat.projXScale = basisXn / basisWn;
            if (gInfo.DetectInverseMatrix) CacheForwardVP(f, transposed);
            RecordSceneZRow(f, transposed);
            // The frame's most frequent clean focal is the scene camera's: the reference for UI projections, scaled
            // worlds and last frame's camera alike.
            auto& vote = s_cleanP00Votes[(int)lroundf(s_mat.projXScale * 1000.f)];
            ++vote.first;
            vote.second = s_mat.projXScale;
        }
        // A scaled world (MP3's light volumes: unit mesh * radius) has no unit w row; its x scale is the camera's p00,
        // not the cached last unit-w matrix's.
        const bool unitW = basisWn > 1e-6f && fabsf(basisWn - 1.f) < 0.05f;
        const float own = ProjXScaleFromMatrix(f, transposed);
        // UiProjectionParallax: a clean matrix whose focal is beyond 2x from the scene camera's is a UI projection, not
        // another FOV (a weapon's stays within), and its draw takes the scene's parallax.
        const float scene = s_camP00 > 0.f ? s_camP00 : s_sceneP00;
        // SceneNear: the reference is the gameplay camera's, as a menu's own 3D view is no scene camera.
        const float uiScene = s_referenceP00 > 0.f ? s_referenceP00 : scene;
        const bool uiFocal = s_uiProjectionParallax && unitW && uiScene > 0.f && (own < 0.5f * uiScene || own > 2.f * uiScene);
        if (!unitW && s_camP00 > 0.f) ++s_scaledByCamera;
        const float camXScale = uiFocal ? uiScene : (!unitW && s_camP00 > 0.f) ? s_camP00 : own;

        if (fullColumn)
        {
            const float b  = eyeShift * camXScale;
            // EyeOffsetM41(eyeShift) = -eyeShift * StereoConvergence(); use the per-draw conv override
            // so a per-shader ConvergenceShift reshapes the parallax constant to match.
            const float m41 = -eyeShift * conv * camXScale;
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


// Patches for the shader about to read the constants, not whichever was bound at write time (incidental when a CB is
// filled long before its draw, as in GTA V). Unchanged contents and the same shader cost one compare.
void Context11Proxy::PatchBoundCBsForDraw()
{
    if (!m_boundVS || !m_parent) return;
    const float eyeShift = wiz3D_GetEffectiveEyeShift()
                         * ProfileShaderMultiplier(m_parent, m_boundVS) * EyeShiftSign();
    if (eyeShift == 0.f) return;
    // Per-shader convergence (DX9 BaseStereoRenderer_Main.cpp:1037 parity). The VS owns the forward
    // transform, so its profile ConvergenceShift sets the zero-parallax point for every CB it reads;
    // the DS inherits it since the two stages are co-bound.
    const float conv = ResolveShaderConvergence(m_parent, m_boundVS);
    PatchStageCBsForDraw(ST_VS, m_boundVS, m_parent->LookupShaderProjection(m_boundVS), eyeShift, conv);
    // Tessellated geometry projects in the domain shader; its buffers come after the vertex stage's, so a buffer both
    // read ends up carrying both shaders' matrices.
    if (m_boundDS && m_boundDSInfo) PatchStageCBsForDraw(ST_DS, m_boundDS, m_boundDSInfo, eyeShift, conv);
}

// One stage's constant buffers for the draw: its projection matrices shifted and learned inverses re-applied; the vertex
// stage also gives back the game's bytes where it reads no matrix, unless the domain stage reads one there.
void Context11Proxy::PatchStageCBsForDraw(StageIdx stage, void* shader, const ShaderAnalysis11Result* info, float eyeShift, float conv)
{
    if (!info || !info->parsed) return;
    const DWORD crc = BoundShaderCRC(m_parent, shader);

    const CBEyeSlots& s = m_cbSlots[stage];
    for (UINT slot = 0; slot < s.high && slot < kMaxVSCBSlots; ++slot)
    {
        Buffer11Proxy* b = s.proxy[slot];
        if (!b || !b->GetRealRight() || b->Shadow().empty() || b->IsRing()) continue;
        auto it = info->projection.matrixData.cb.find(slot);
        // No matrix read here: the shader wants the game's bytes, which a write under another shader may have shifted.
        if (it == info->projection.matrixData.cb.end())
        {
            bool dsReads = false;
            if (stage == ST_VS && m_boundDS && m_boundDSInfo && m_boundDSInfo->parsed)
                for (UINT k = 0; k < m_cbSlots[ST_DS].high && k < kMaxVSCBSlots; ++k)
                    dsReads |= (m_cbSlots[ST_DS].proxy[k] == b && m_boundDSInfo->projection.matrixData.cb.count(k) != 0);
            // Under VertexCameraMatrices a declared matrix outside the chain is PatchCameraMatrices' to shift or restore.
            bool rdefReads = false;
            if (VertexCameraMatrices())
                for (const auto& m : info->readMatrices) rdefReads |= (m.first == (int)slot);
            if (stage == ST_VS && !dsReads && !rdefReads && b->RightForwardShifted()) { RestoreRightCB(b); ++m_plainRestoredThisFrame; }
            continue;
        }
        if (!b->NeedsDrawPatch(crc, eyeShift)) continue;

        std::vector<EyeShiftMatrix> targets;
        // A ranged bind (11.1 first constant) places the shader's register that far into the buffer.
        const DWORD rangeBase = s.num[slot] ? s.first[slot] : 0;
        for (const auto& pmd : it->second)
        {
            if (pmd.incorrectProjection) continue;
            EyeShiftMatrix em = { pmd.matrixRegister + rangeBase, pmd.matrixIsTransposed, FALSE, FALSE };
            targets.push_back(em);
        }
        // The vertex stage's matrices in the same buffer stay shifted when the domain stage rewrites it.
        if (stage == ST_DS && m_boundInfo[0] && m_boundInfo[0]->parsed)
            for (UINT k = 0; k < m_cbSlots[ST_VS].high && k < kMaxVSCBSlots; ++k)
            {
                if (m_cbSlots[ST_VS].proxy[k] != b) continue;
                auto vit = m_boundInfo[0]->projection.matrixData.cb.find(k);
                if (vit == m_boundInfo[0]->projection.matrixData.cb.end()) continue;
                const DWORD vsBase = m_cbSlots[ST_VS].num[k] ? m_cbSlots[ST_VS].first[k] : 0;
                for (const auto& pmd : vit->second)
                {
                    if (pmd.incorrectProjection) continue;
                    bool dup = false;
                    for (const auto& t : targets) dup |= (t.matrixRegister == pmd.matrixRegister + vsBase);
                    if (dup) continue;
                    EyeShiftMatrix em = { pmd.matrixRegister + vsBase, pmd.matrixIsTransposed, FALSE, FALSE };
                    targets.push_back(em);
                }
            }
        // The whole sibling is rewritten, so any inverse it carries is re-applied: an inverse VP does not depend on the
        // consumer, and the ring re-verification still guards it.
        for (const auto& mem : b->LearnedMatrices())
        {
            if (!mem.inverse) continue;
            EyeShiftMatrix em = { mem.reg, mem.transposed, TRUE, TRUE };
            targets.push_back(em);
        }
        if (targets.empty()) { b->MarkDrawPatched(crc, eyeShift); continue; }

        m_drawPatchScratch.assign(b->Shadow().begin(), b->Shadow().end());
        unsigned shifted = 0, inverses = 0;
        ApplyTargetedEyeShiftToCB(m_drawPatchScratch.data(), m_drawPatchScratch.size(),
                                  eyeShift, conv, targets, true, &shifted, nullptr, &inverses, b);
        NoteRightConstants(b);
        if (WriteWholeRightCB(b, m_drawPatchScratch))
        {
            b->MarkDrawPatched(crc, eyeShift);
            if (shifted) b->MarkShifted(s_frameId);
            b->NoteRightShift(shifted - inverses, inverses);
            b->InvalidateInverseDrawCheck();
            if (stage == ST_DS) ++m_dsBuffersPatchedThisFrame;
        }
    }
}

// The right sibling gets the game's bytes back; the shader about to read them wants no shift there.
void Context11Proxy::RestoreRightCB(Buffer11Proxy* b)
{
    if (!b || !b->GetRealRight() || b->Shadow().empty() || b->IsRing()) return;
    NoteRightConstants(b);
    if (!WriteWholeRightCB(b, b->Shadow())) return;
    b->SetRightFromShadow(true);
    b->MarkDrawPatched(0, 0.f);
    b->InvalidateInverseDrawCheck();
}

// An overlay draw (back buffer, no depth test) reads every vertex-stage buffer as the game wrote it.
void Context11Proxy::UnshiftOverlayCBs()
{
    const CBEyeSlots& s = m_cbSlots[ST_VS];
    for (UINT slot = 0; slot < s.high && slot < kMaxVSCBSlots; ++slot)
    {
        Buffer11Proxy* b = s.proxy[slot];
        if (!b || !b->GetRealRight() || b->Shadow().empty() || b->RightIsShadow() || b->IsRing()) continue;
        RestoreRightCB(b);
        ++m_overlayRestoredThisFrame;
    }
}

// A GPU copy landed in a ring: the real buffers hold it, the shadow does not. Its pages are foreign until read back.
void Context11Proxy::NoteRingCopy(Buffer11Proxy* b, size_t off, size_t n)
{
    if (!b || !n) return;
    b->MarkForeign(off, n);
    RingReadback r = { b, off, n, s_frameId };
    b->AddRef();
    m_ringReadbacks.push_back(r);
}

// One range of a buffer into CPU memory through a staging copy; the caller waits a frame first, so the map does not
// stall. n <= 64 KB per call.
bool Context11Proxy::RingReadBack(ID3D11Buffer* b, size_t off, size_t n, unsigned char* out)
{
    if (!b || !m_parent || !n || n > 65536) return false;
    ID3D11Buffer*& stg = m_ringStaging[(UINT)n];
    if (!stg)
    {
        D3D11_BUFFER_DESC sd = {};
        sd.ByteWidth = (UINT)n; sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(m_parent->GetReal()->CreateBuffer(&sd, nullptr, &stg))) { stg = nullptr; return false; }
    }
    D3D11_BOX box = { (UINT)off, 0, 0, (UINT)(off + n), 1, 1 };
    m_real->CopySubresourceRegion(stg, 0, 0, 0, 0, b, 0, &box);
    D3D11_MAPPED_SUBRESOURCE m = {};
    if (FAILED(m_real->Map(stg, 0, D3D11_MAP_READ, 0, &m)) || !m.pData) return false;
    memcpy(out, m.pData, n);
    m_real->Unmap(stg, 0);
    return true;
}

// Copies into rings noted a frame or more ago: their bytes come back from the real left buffer into the shadow, the
// pages stop being foreign, and the right copies are rebuilt from them at the next draw.
void Context11Proxy::ServiceRingReadbacks()
{
    for (size_t i = 0; i < m_ringReadbacks.size();)
    {
        RingReadback& r = m_ringReadbacks[i];
        if (s_frameId <= r.frame + 1) { ++i; continue; }
        bool ok = true;
        for (size_t o = 0; o < r.n && ok; o += 65536)
        {
            const size_t chunk = r.n - o < 65536 ? r.n - o : 65536;
            ok = RingReadBack(r.b->GetReal(), r.off + o, chunk, r.b->RingData() + r.off + o);
        }
        if (ok) { r.b->ClearForeign(r.off, r.n); r.b->MarkRingWritten(); }
        r.b->Release();
        m_ringReadbacks.erase(m_ringReadbacks.begin() + i);
    }
}

// One range of a ring's real buffer, written in place: NO_OVERWRITE, as the game itself writes rings, so nothing the
// GPU may still read is touched.
bool Context11Proxy::WriteRingRange(ID3D11Buffer* real, size_t off, const unsigned char* p, size_t n)
{
    // BatchRightEye: a right-eye draw's range goes into the recording; a left range a recorded draw reads replays it first.
    if (m_rightRecording && m_real == m_recorder)
    {
        m_recorder->WriteRange(real, off, p, n);
        return true;
    }
    if (PendingLeftRange(real, off, n)) FlushRightBatch(kFlushWrite);
    D3D11_MAPPED_SUBRESOURCE m = {};
    if (!real || FAILED(m_real->Map(real, 0, D3D11_MAP_WRITE_NO_OVERWRITE, 0, &m)) || !m.pData) return false;
    memcpy(static_cast<unsigned char*>(m.pData) + off, p, n);
    m_real->Unmap(real, 0);
    return true;
}

// RingKeepsShifts: a right ring version written in place reaches draws queued before it, so a register whose game bytes
// are as at our last write keeps the shift that write gave it. A plain write (eye shift 0) is never compared.
void Context11Proxy::MergeRightRing(Buffer11Proxy* b, size_t off, unsigned char* p, size_t n, float eyeShift)
{
    std::vector<unsigned char>& mirror = b->RightMirror();
    std::vector<unsigned char>& then = b->GameAtRightWrite();
    const size_t size = b->RingSize();
    if (mirror.size() != size) { mirror.assign(size, 0); then.assign(size, 0); }
    if (off >= size) return;
    if (off + n > size) n = size - off;
    const unsigned char* game = b->RingData();
    const bool keep = eyeShift != 0.f && b->RightMirrorShift() == eyeShift;   // a new separation re-bases
    if (eyeShift != 0.f) b->RightMirrorShift() = eyeShift;
    if (keep)
        for (size_t r = off; r + 16 <= off + n; r += 16)
        {
            unsigned char* now = p + (r - off);
            const unsigned char* last = mirror.data() + r;
            if (memcmp(then.data() + r, game + r, 16) || !memcmp(last, now, 16) || !memcmp(last, game + r, 16)) continue;
            if (!memcmp(now, game + r, 16)) memcpy(now, last, 16);   // the new version would take the shift away
        }
    memcpy(mirror.data() + off, p, n);
    memcpy(then.data() + off, game + off, n);
}

// CameraInverses: an inverse whose forward is camera-shaped, whatever space it maps to (Frostbite 3's lighting inverts
// the world view-projection while its geometry uses the camera-relative one, so no ring entry matches).
static bool InverseOfCamera(const float* canon)
{
    D3DXMATRIX c(canon), inv;
    if (!D3DXMatrixInverse(&inv, nullptr, &c)) return false;
    return CameraFamily(reinterpret_cast<const float*>(&inv), false);
}

// A declared float4x4 at `reg` of `slot`, classified once per shader register as an inverse or forward camera matrix
// and trusted while its bytes keep that shape (an inverse that loses it is matched afresh). false = not a camera matrix.
bool Context11Proxy::ClassifyDeclaredMatrix(const ShaderAnalysis11Result* info, UINT slot, DWORD reg, const float* f,
                                            EyeShiftMatrix& out)
{
    const unsigned long long key = ((unsigned long long)info->crc32 << 32) | (slot << 16) | reg;
    auto known = m_psCameraMatrices.find(key);
    if (known != m_psCameraMatrices.end() && known->second.inverse)
    {
        float cand[16];
        wiz3d::CanonicalizeMat4(f, known->second.transposed, cand);
        if (!wiz3d::LooksLikeUnprojection(cand)) { m_psCameraMatrices.erase(known); known = m_psCameraMatrices.end(); }
    }
    // CodeMatrixRows: a matrix found from the code is read as dp4 rows, so only that orientation is tried (an axis-aligned
    // camera-relative one passes CameraFamily as columns too).
    const bool rowsOnly = info->matricesFromCode && CodeMatrixRowsOn();
    if (known == m_psCameraMatrices.end())
        for (int o = rowsOnly ? 1 : 0; o < 2 && known == m_psCameraMatrices.end(); ++o)
        {
            float cand[16];
            wiz3d::CanonicalizeMat4(f, o == 1, cand);
            const bool inverse = wiz3d::LooksLikeUnprojection(cand) &&
                                 (s_mat.MatchInverse(cand, nullptr) || (CameraInversesOn() && InverseOfCamera(cand)));
            if (inverse || s_mat.MatchForward(cand) || CameraFamily(f, o == 1))
                known = m_psCameraMatrices.insert(std::make_pair(key, PsCameraMatrix{ inverse, o == 1 })).first;
        }
    // ViewportMatrices: the scene camera with the viewport folded in, world to pixel or pixel to world.
    if (known == m_psCameraMatrices.end() && ViewportMatricesOn())
    {
        bool transposed = false;
        const int kind = ClassifyViewportMatrix(f, s_camP00 > 0.f ? s_camP00 : s_sceneP00, rowsOnly, transposed, info->crc32,
                                                slot, reg);
        if (kind) known = m_psCameraMatrices.insert(std::make_pair(key, PsCameraMatrix{ kind == 2, transposed, true })).first;
    }
    if (known == m_psCameraMatrices.end()) return false;
    float cand[16];
    wiz3d::CanonicalizeMat4(f, known->second.transposed, cand);
    if (known->second.inverse && !wiz3d::LooksLikeUnprojection(cand)) return false;
    out.matrixRegister     = reg;
    out.matrixIsTransposed = known->second.transposed ? TRUE : FALSE;
    out.matrixIsInverse    = known->second.inverse ? TRUE : FALSE;
    out.matrixFromLearned  = FALSE;
    out.viewport           = known->second.viewport;
    return true;
}

// KeepBareProjections: a bare projection (no view rotation or translation) a pixel or compute shader reads projects
// positions rebuilt from its own eye's depth, so it stays the game's (Andromeda's reflection hits). true = left as is.
bool Context11Proxy::BareProjectionKept(StageIdx st, const ShaderAnalysis11Result* info, UINT slot, const float* f,
                                        const EyeShiftMatrix& em)
{
    if ((st != ST_PS && st != ST_CS) || em.matrixIsInverse || !KeepBareProjections()) return false;
    float m[16];
    wiz3d::CanonicalizeMat4(f, em.matrixIsTransposed != FALSE, m);   // row-vector: x is column 0, w column 3
    const float xn = sqrtf(m[0] * m[0] + m[4] * m[4] + m[8] * m[8]);
    if (xn < 1e-6f || fabsf(m[4]) > 1e-3f * xn || fabsf(m[8]) > 2e-3f * xn || fabsf(m[12]) > 1e-3f * xn ||
        fabsf(m[3]) > 1e-3f || fabsf(m[7]) > 1e-3f || fabsf(fabsf(m[11]) - 1.f) > 1e-3f || fabsf(m[15]) > 1e-3f)
        return false;
    if (FrameTraceActive())
        FrameTrace("      declared %s cb%u r%u (%08lX): bare projection p00 %.4f, kept (KeepBareProjections)\n",
                   st == ST_PS ? "ps" : "cs", slot, (unsigned)em.matrixRegister, info->crc32, m[0]);
    return true;
}

// ViewMatrices: a 3x4 affine read as columns is the camera's view if its axes are orthonormal and its x and z run along a
// fresh scene view-projection's x and w columns (rows would match any rotation at yaw 0 or 180).
bool Context11Proxy::ClassifyViewMatrix(const ShaderAnalysis11Result* info, UINT slot, size_t off,
                                        const unsigned char* block, size_t n, EyeShiftMatrix& out)
{
    const float scene = s_camP00 > 0.f ? s_camP00 : s_sceneP00;
    if (scene <= 0.f || off % 16 || off + 64 > n) return false;
    const float* f = reinterpret_cast<const float*>(block + off);
    float a[3][3];   // the view's x, y and z axes over world x, y, z: register k + c holds world axis c's column
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) a[r][c] = f[c * 4 + r];
    bool unit = true;
    for (int r = 0; r < 3 && unit; ++r)
    {
        const float len = sqrtf(a[r][0] * a[r][0] + a[r][1] * a[r][1] + a[r][2] * a[r][2]);
        unit = fabsf(len - 1.f) < 0.01f;
        for (int q = r + 1; q < 3 && unit; ++q) unit = fabsf(a[r][0] * a[q][0] + a[r][1] * a[q][1] + a[r][2] * a[q][2]) < 0.01f;
    }
    float bestX = 0.f, bestZ = 0.f, sense = 0.f;   // the closest camera axes, for the frame trace
    for (unsigned i = 0; unit && i < wiz3d::StereoMatrixState::kRing && sense == 0.f; ++i)
    {
        const auto& e = s_mat.ring[i];
        if (!s_mat.EntryUsable(e, wiz3d::StereoMatrixState::kMaxAge)) continue;
        const float* m = e.m;   // canonical row-vector: x is column 0, w column 3
        const float xn = sqrtf(m[0] * m[0] + m[4] * m[4] + m[8] * m[8]);
        const float wn = sqrtf(m[3] * m[3] + m[7] * m[7] + m[11] * m[11]);
        if (wn < 1e-6f || fabsf(wn - 1.f) > 0.05f || fabsf(xn / wn - scene) > 0.01f * scene) continue;
        const float dx = (a[0][0] * m[0] + a[0][1] * m[4] + a[0][2] * m[8]) / xn;
        const float dz = (a[2][0] * m[3] + a[2][1] * m[7] + a[2][2] * m[11]) / wn;
        if (fabsf(dx) > fabsf(bestX)) bestX = dx;
        if (fabsf(dz) > fabsf(bestZ)) bestZ = dz;
        if (fabsf(dx) > 0.999f && fabsf(dz) > 0.999f) sense = dx > 0.f ? 1.f : -1.f;
    }
    if (sense != 0.f)
    {
        out.matrixRegister = (DWORD)(off / 16) + 3u;   // the translation column, x first
        out.viewComponent = 0;
        out.viewSign = sense;
    }
    if (FrameTraceActive())
        FrameTrace("      view cb%u r%u (%08lX): %s | unit %d x %+.4f z %+.4f | scene p00 %.4f\n", slot, (unsigned)(off / 16),
                   info->crc32, sense != 0.f ? "moved" : "no camera view", unit ? 1 : 0, bestX, bestZ, scene);
    return sense != 0.f;
}

// The targets every shader so far asked of a ring block's bytes (by buffer and offset, for one write sequence and
// shift), so its right version in place is always the union; cleared at Present.
struct BlockTargets { unsigned hash = 0; float shift = 0.f; std::vector<EyeShiftMatrix> targets; };
static std::map<std::pair<Buffer11Proxy*, size_t>, BlockTargets> s_blockTargets;
void ClearBlockTargets() { s_blockTargets.clear(); }

// FNV-1a over a block's first kilobyte (its matrices sit at the front): the union resets on the block's own bytes
// changing, not on the ring's write sequence, which ticks for every other block the game writes.
// Change detection over the first 1024 bytes: FNV-1a eight bytes a step, folded to 32 bits.
static unsigned BlockHash(const unsigned char* p, size_t n)
{
    if (n > 1024) n = 1024;
    UINT64 h = 14695981039346656037ull;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) { UINT64 w; memcpy(&w, p + i, 8); h ^= w; h *= 1099511628211ull; }
    for (; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return (unsigned)(h ^ (h >> 32));
}

// ConstantBlocks: each bound block (a whole small buffer, or the ring range an 11.1 bind names) rebuilt from the game's
// bytes with every reading stage's targets and written to the right copy once per change; left, a ring's ranges.
void Context11Proxy::SyncBoundConstants(bool right, bool plainOnly, bool fromDispatch)
{
    struct Job
    {
        Buffer11Proxy* b;
        size_t off, n;
        DWORD crc;
        std::vector<EyeShiftMatrix> targets;
        std::vector<std::pair<int, UINT>> slots;
    };
    // One list per thread, reused call to call: its jobs' vectors keep their capacity, so a draw allocates nothing.
    static thread_local std::vector<Job> t_jobs;
    std::vector<Job>& jobs = t_jobs;
    size_t used = 0;
    const bool smallToo = right && gInfo.PatchConstantsAtDraw;
    { ScopedTicks sb_(right ? m_time.syncBuild : m_timeScratch);
    for (int st = 0; st < ST_COUNT; ++st)
    {
        if (m_syncSkipGeometry && st != ST_PS && st != ST_CS) continue;
        const CBEyeSlots& s = m_cbSlots[st];
        for (UINT slot = 0; slot < s.high && slot < kMaxCBs; ++slot)
        {
            Buffer11Proxy* b = s.proxy[slot];
            if (!b || !(right ? b->GetRealRight() : b->GetReal())) continue;
            const bool ring = b->IsRing();
            if (!ring && (!smallToo || b->Shadow().empty())) continue;
            size_t off = 0, n = 0;
            if (ring)
            {
                const size_t size = b->RingSize();
                off = s.num[slot] ? (size_t)s.first[slot] * 16 : 0;
                if (off >= size) continue;
                n = s.num[slot] ? (size_t)s.num[slot] * 16 : 65536;   // a whole bind sees the first 4096 constants
                if (off + n > size) n = size - off;
            }
            else n = b->Shadow().size();
            // A small buffer bound with a range still holds the shader's registers that far in.
            const DWORD base = ring ? 0 : (s.num[slot] ? s.first[slot] : 0);
            Job* job = nullptr;
            for (size_t i = 0; i < used; ++i) if (jobs[i].b == b && jobs[i].off == off) { job = &jobs[i]; break; }
            if (!job)
            {
                if (used == jobs.size()) jobs.push_back(Job());
                job = &jobs[used++];
                job->targets.clear();
                job->slots.clear();
                job->b = b; job->off = off; job->n = n; job->crc = 0;
                // The buffer's learned inverses (found at a write), re-verified against the ring when applied.
                if (!ring && right && !plainOnly)
                    for (const auto& mem : b->LearnedMatrices())
                        if (mem.inverse)
                        {
                            EyeShiftMatrix em = { mem.reg, mem.transposed, TRUE, TRUE };
                            job->targets.push_back(em);
                        }
            }
            if (n > job->n) job->n = n;
            job->slots.push_back(std::make_pair(st, slot));
            if (!right || plainOnly) continue;
            const ShaderAnalysis11Result* info = st == ST_VS ? m_boundInfo[0] : st == ST_PS ? m_boundInfo[1]
                                               : st == ST_CS ? m_boundInfo[2] : st == ST_DS ? m_boundDSInfo : nullptr;
            if (!info) continue;
            job->crc ^= info->crc32 * (DWORD)(st + 1);
            const unsigned char* bytes = ring ? b->RingData() + off : b->Shadow().data();
            // The position chain needs the bytecode analysis; declared matrices stand on the reflection alone, as a
            // compute shader's do (it has no position output to parse).
            auto it = info->parsed ? info->projection.matrixData.cb.find(slot) : info->projection.matrixData.cb.end();
            if (it != info->projection.matrixData.cb.end())
                for (const auto& pmd : it->second)
                {
                    if (pmd.incorrectProjection) continue;
                    const DWORD reg = pmd.matrixRegister + base;
                    bool dup = false;
                    for (const auto& t : job->targets) dup |= (t.matrixRegister == reg);
                    if (dup) continue;
                    EyeShiftMatrix em = { reg, pmd.matrixIsTransposed, FALSE, FALSE };
                    job->targets.push_back(em);
                }
            // Declared camera matrices: the pixel and compute stages always, the vertex stage under VertexCameraMatrices
            // (its pixel matrices under ViewportMatrices).
            const bool geometryStage = st != ST_PS && st != ST_CS;
            if (!gInfo.DetectInverseMatrix || (geometryStage && !VertexCameraMatrices() && info->pixelMatrices.empty())) continue;
            for (const auto& m : info->readMatrices)
            {
                if (m.first != (int)slot || (m.second & 15)) continue;
                const size_t moff = m.second + (size_t)base * 16;
                if (moff + 64 > n) continue;
                const DWORD reg = (DWORD)(moff / 16);
                bool dup = false;
                for (const auto& t : job->targets) dup |= (t.matrixRegister == reg);
                if (dup) continue;
                EyeShiftMatrix em;
                const float* f = reinterpret_cast<const float*>(bytes + moff);
                if (ClassifyDeclaredMatrix(info, slot, reg, f, em))
                {
                    if (geometryStage && !VertexCameraMatrices() && !em.viewport) continue;
                    if (!BareProjectionKept((StageIdx)st, info, slot, f, em)) job->targets.push_back(em);
                }
                else if (FrameTraceActive())
                {
                    float xn, wn, miss = -1.f;
                    MatrixBasisNorms(f, true, xn, wn);
                    float cand[16];
                    wiz3d::CanonicalizeMat4(f, true, cand);
                    bool shape = false;
                    s_mat.MatchInverse(cand, &miss, &shape);
                    FrameTrace("      declared %s cb%u r%u (%08lX): no camera match | row-basis xn %.4f wn %.4f | inverse miss"
                               " %.3g%s | scene p00 %.4f\n", st == ST_VS ? "vs" : st == ST_PS ? "ps" : st == ST_CS ? "cs" : "ds",
                               slot, reg, info->crc32, xn, wn, miss, shape ? " (not unprojection-shaped)" : "",
                               s_camP00 > 0.f ? s_camP00 : s_sceneP00);
                }
            }
            // The camera's view a pixel or compute shader casts rays from: the right camera's translation (ViewMatrices).
            if ((st == ST_PS || st == ST_CS) && ViewMatrices())
                for (const auto& v : info->viewMatrices)
                {
                    if (v.first != (int)slot) continue;
                    EyeShiftMatrix em = {};
                    if (!ClassifyViewMatrix(info, slot, v.second + (size_t)base * 16, bytes, n, em)) continue;
                    bool dup = false;
                    for (const auto& t : job->targets)
                        dup |= (t.matrixRegister == em.matrixRegister && t.viewComponent == em.viewComponent);
                    if (dup) continue;
                    job->targets.push_back(em);
                }
        }
    }
    }
    const int e = right ? 1 : 0;
    const float eyeShift = !right ? 0.f
        : wiz3D_GetEffectiveEyeShift() * (fromDispatch ? 1.f : ProfileShaderMultiplier(m_parent, m_boundVS)) * EyeShiftSign();
    // Compute shaders have no profile shader bound, so they always use the global convergence.
    const float conv = (right && !fromDispatch) ? ResolveShaderConvergence(m_parent, m_boundVS) : StereoConvergence();
    ScopedTicks sa_(right ? m_time.syncApply : m_timeScratch);
    for (size_t ji = 0; ji < used; ++ji)
    {
        Job& j = jobs[ji];
        const unsigned seq = j.b->WriteSeq();
        // A ring block's right version, overlays included, carries the union of every shader's targets for these bytes:
        // a rewrite in place then never takes a shift away from a draw queued earlier (Hardline's lights and cones).
        bool targetsGrew = false;
        const unsigned hash = (right && j.b->IsRing()) ? BlockHash(j.b->RingData() + j.off, j.n) : 0;
        if (right && j.b->IsRing())
        {
            BlockTargets& acc = s_blockTargets[std::make_pair(j.b, j.off)];
            if (acc.hash != hash || acc.shift != eyeShift)
            { acc.hash = hash; acc.shift = eyeShift; acc.targets.clear(); targetsGrew = true; }
            for (const auto& t : j.targets)
            {
                bool have = false;
                for (const auto& a : acc.targets)
                    have |= (a.matrixRegister == t.matrixRegister && a.viewComponent == t.viewComponent);
                if (!have) { acc.targets.push_back(t); targetsGrew = true; }
            }
            j.targets = acc.targets;
        }
        bool inSync = true;
        for (const auto& sl : j.slots)
        {
            const CBEyeSlots& s = m_cbSlots[sl.first];
            // A right ring block: in sync while its bytes, union and shift are what this slot last received, whatever
            // the ring's write sequence did meanwhile (a same-union rewrite could re-apply an inverse differently).
            if (right && j.b->IsRing())
                inSync = inSync && s.syncOff[e][sl.second] == (UINT)j.off && s.syncHash[sl.second] == hash &&
                         !targetsGrew && s.syncShift[sl.second] == eyeShift;
            else
                inSync = inSync && s.syncSeq[e][sl.second] == seq && s.syncOff[e][sl.second] == (UINT)j.off &&
                         (!right || (s.syncCrc[sl.second] == j.crc && s.syncShift[sl.second] == eyeShift));
        }
        if (inSync) continue;
        // A block on a page a GPU copy touched (not yet read back): the copied bytes stay as the real buffers hold them,
        // and the rest, which the game wrote into our shadow only, still reaches this eye's real buffer unshifted.
        if (j.b->IsRing() && j.b->AnyForeign(j.off, j.n))
        {
            ID3D11Buffer* real = right ? j.b->GetRealRight() : j.b->GetReal();
            size_t at = j.off;
            const size_t end = j.off + j.n;
            while (at < end)
            {
                size_t stop = end;
                bool covered = false;
                for (const auto& r : m_ringReadbacks)
                {
                    if (r.b != j.b || r.off + r.n <= at) continue;
                    if (r.off <= at) { covered = true; if (r.off + r.n < stop) stop = r.off + r.n; }
                    else if (r.off < stop) stop = r.off;
                }
                if (!covered) WriteRingRange(real, at, j.b->RingData() + at, stop - at);
                at = stop;
            }
            continue;
        }
        const unsigned char* src = j.b->IsRing() ? j.b->RingData() + j.off : j.b->Shadow().data();
        m_drawPatchScratch.assign(src, src + j.n);
        unsigned shifted = 0, inverses = 0;
        if (right && !j.targets.empty())
            ApplyTargetedEyeShiftToCB(m_drawPatchScratch.data(), m_drawPatchScratch.size(), eyeShift, conv, j.targets, true,
                                      &shifted, nullptr, &inverses, j.b);
        // The profile's features edit a ring block here (a small buffer's copy they edit in EditRightCBs): the block
        // is the cbuffer they named, at the offsets they found in it.
        if (right && !plainOnly && j.b->IsRing() && !Features().empty() && OutputAtPresentAspect(fromDispatch))
        {
            m_editAtDispatch = fromDispatch;
            for (const auto& sl : j.slots)
            {
                const int st = sl.first;
                const ShaderAnalysis11Result* info = st == ST_VS ? m_boundInfo[0] : st == ST_PS ? m_boundInfo[1]
                                                   : st == ST_CS ? m_boundInfo[2] : st == ST_DS ? m_boundDSInfo : nullptr;
                if (!info) continue;
                for (const auto& named : info->rightCBs)
                {
                    if (named.second != (int)sl.second) continue;
                    m_editShader = info;
                    m_editRetry = false;
                    for (auto& f : m_features)
                        f->EditRightConstants(*this, named.first, m_drawPatchScratch.data(), m_drawPatchScratch.size());
                    m_editShader = nullptr;
                }
            }
        }
        bool written;
        if (j.b->IsRing())
        {
            // In place, in the game's own sibling at the game's range: the union above makes the rewrite safe.
            if (right && RingKeepsShifts()) MergeRightRing(j.b, j.off, m_drawPatchScratch.data(), j.n, eyeShift);
            written = WriteRingRange(right ? j.b->GetRealRight() : j.b->GetReal(), j.off, m_drawPatchScratch.data(), j.n);
        }
        else
        {
            NoteRightConstants(j.b);
            written = WriteWholeRightCB(j.b, m_drawPatchScratch);
        }
        if (!written) continue;
        for (const auto& sl : j.slots)
        {
            CBEyeSlots& s = m_cbSlots[sl.first];
            s.syncSeq[e][sl.second] = seq;
            s.syncOff[e][sl.second] = (UINT)j.off;
            if (right) { s.syncCrc[sl.second] = j.crc; s.syncShift[sl.second] = eyeShift; s.syncHash[sl.second] = hash; }
        }
        if (!right) continue;
        if (shifted) j.b->MarkShifted(s_frameId);
        j.b->NoteRightShift(shifted - inverses, inverses);
        j.b->InvalidateInverseDrawCheck();
        m_invDrawPatchedThisFrame += inverses;
        m_fwdDrawPatchedThisFrame += shifted - inverses;
        if (j.b->IsRing()) NoteRightConstants(j.b);
        else j.b->MarkDrawPatched(j.crc, eyeShift);
    }
}

// WRITE_DISCARD works on a dynamic buffer only; a DEFAULT one (UpdateSubresource style) is written the way the game
// writes it, or it is never patched.
bool Context11Proxy::WriteWholeRightCB(Buffer11Proxy* b, const std::vector<unsigned char>& bytes)
{
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (SUCCEEDED(m_real->Map(b->GetRealRight(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)) && mapped.pData)
    {
        memcpy(mapped.pData, bytes.data(), bytes.size());
        m_real->Unmap(b->GetRealRight(), 0);
        return true;
    }
    D3D11_BUFFER_DESC bd = {};
    b->GetRealRight()->GetDesc(&bd);
    if (bd.Usage != D3D11_USAGE_DEFAULT || bd.ByteWidth != bytes.size()) return false;
    m_real->UpdateSubresource(b->GetRealRight(), 0, nullptr, bytes.data(), 0, 0);
    return true;
}

// A pixel or compute shader's declared camera matrices (inverse or last frame's view-projection), matched at the right
// draw since they precede the detector's ring: the scene camera's x scale, a unit w basis and orthogonal bases.
bool CameraFamily(const float* f, bool transposed)
{
    const float scene = s_camP00 > 0.f ? s_camP00 : s_sceneP00;
    if (scene <= 0.f) return false;
    float xn, wn;
    MatrixBasisNorms(f, transposed, xn, wn);
    if (wn <= 1e-6f || fabsf(wn - 1.f) >= 0.05f || !BasesOrthogonal(f, transposed, xn, wn)) return false;
    return fabsf(xn / wn - scene) <= 0.002f * scene;
}

// ViewportMatrices: the shader reads a pixel position from a matrix in this slot (empty list when the switch is off).
static bool PixelMatrixAt(const ShaderAnalysis11Result* info, UINT slot)
{
    for (const auto& m : info->pixelMatrices) if (m.first == (int)slot) return true;
    return false;
}

void Context11Proxy::PatchCameraMatrices(StageIdx stage)
{
    const ShaderAnalysis11Result* ps = m_boundInfo[stage == ST_CS ? 2 : stage == ST_VS ? 0 : 1];
    if (!ps || ps->readMatrices.empty() || !m_parent) return;
    const float multiplier = stage == ST_CS ? 1.f : ProfileShaderMultiplier(m_parent, m_boundVS);
    const float eyeShift = wiz3D_GetEffectiveEyeShift() * multiplier * EyeShiftSign();
    if (eyeShift == 0.f) return;
    // Compute shaders use the global convergence; VS/PS inherit the bound VS's per-shader shift so a camera-matrix
    // inverse rebuild lands on the same zero-parallax plane as the forward draw patch.
    const float conv = (stage == ST_CS) ? StereoConvergence() : ResolveShaderConvergence(m_parent, m_boundVS);
    const CBEyeSlots& s = m_cbSlots[stage];
    std::vector<EyeShiftMatrix> targets;
    for (UINT slot = 0; slot < s.high && slot < kMaxCBs; ++slot)
    {
        Buffer11Proxy* b = s.proxy[slot];
        if (!b || !b->GetRealRight() || b->Shadow().empty() || b->IsRing()) continue;
        // A vertex buffer holding the position chain's matrix is the draw patch's, unless the shader reads a pixel
        // matrix there too (ViewportMatrices).
        const bool chain = stage == ST_VS && ps->projection.matrixData.cb.count(slot);
        if (chain && !PixelMatrixAt(ps, slot)) continue;
        bool reads = false;
        for (const auto& m : ps->readMatrices) reads |= (m.first == (int)slot);
        if (!reads || !b->InverseDrawCheckDue(eyeShift, conv)) continue;
        b->MarkInverseDrawChecked(eyeShift, conv);
        ++m_invDrawChecksThisFrame;

        const std::vector<unsigned char>& bytes = b->Shadow();
        targets.clear();
        // Every bound stage's declared matrices in this buffer go in together: one stage's rebuild must not drop the
        // other's (Metro's full-screen VS reads m_iP and its lighting PS m_iVP from one buffer).
        for (int st = 0; st < 3; ++st)
        {
        const StageIdx other = st == 0 ? ST_VS : st == 1 ? ST_PS : ST_CS;
        const ShaderAnalysis11Result* info = m_boundInfo[st];
        if (!info || (other == ST_CS) != (stage == ST_CS)) continue;
        if (other == ST_VS && !VertexCameraMatrices() && info->pixelMatrices.empty()) continue;
        for (UINT oslot = 0; oslot < m_cbSlots[other].high && oslot < kMaxCBs; ++oslot)
        {
        if (m_cbSlots[other].proxy[oslot] != b) continue;
        if (other == ST_VS && info->projection.matrixData.cb.count(oslot) && !PixelMatrixAt(info, oslot)) continue;
        for (const auto& m : info->readMatrices)
        {
            if (m.first != (int)oslot || (m.second & 15)) continue;
            // A ranged bind (11.1 first constant) places the shader's register that far into the buffer.
            const size_t off = m.second + (size_t)(m_cbSlots[other].num[oslot] ? m_cbSlots[other].first[oslot] : 0) * 16;
            if (off + 64 > bytes.size()) continue;
            const DWORD reg = (DWORD)(off / 16);
            bool dup = false;
            for (const auto& t : targets) dup |= (t.matrixRegister == reg);
            if (dup) continue;
            const float* f = reinterpret_cast<const float*>(bytes.data() + off);
            EyeShiftMatrix em;
            if (!ClassifyDeclaredMatrix(info, oslot, reg, f, em)) continue;
            // Without VertexCameraMatrices a vertex shader's declared matrix counts only with the viewport folded in.
            if (other == ST_VS && !VertexCameraMatrices() && !em.viewport) continue;
            if (BareProjectionKept(other, info, oslot, f, em)) continue;
            targets.push_back(em);
        }
        }
        }
        // ViewportMatrices: the whole sibling is rebuilt, so the bound vertex shader's position chain in it is shifted again.
        if (!targets.empty() && stage != ST_CS && ViewportMatricesOn() && m_boundInfo[0])
            for (UINT k = 0; k < m_cbSlots[ST_VS].high && k < kMaxVSCBSlots; ++k)
            {
                if (m_cbSlots[ST_VS].proxy[k] != b) continue;
                auto vit = m_boundInfo[0]->projection.matrixData.cb.find(k);
                if (vit == m_boundInfo[0]->projection.matrixData.cb.end()) continue;
                const DWORD vsBase = m_cbSlots[ST_VS].num[k] ? m_cbSlots[ST_VS].first[k] : 0;
                for (const auto& pmd : vit->second)
                {
                    if (pmd.incorrectProjection) continue;
                    bool dup = false;
                    for (const auto& t : targets) dup |= (t.matrixRegister == pmd.matrixRegister + vsBase);
                    if (dup) continue;
                    EyeShiftMatrix em = { pmd.matrixRegister + vsBase, pmd.matrixIsTransposed, FALSE, FALSE };
                    targets.push_back(em);
                }
            }
        // A vertex buffer kept from the plain restore for its declared matrix, none of which is the camera's, is restored
        // (never the position chain's own buffer, which ViewportMatrices lets through).
        if (targets.empty()) { if (stage == ST_VS && b->RightForwardShifted() && !chain) RestoreRightCB(b); continue; }
        // The whole sibling is rebuilt from the game's bytes: a geometry shift a write put there (a buffer shared with
        // the vertex stage) is re-applied from the buffer's remembered targets, under their guards.
        if (b->RightForwardShifted())
            for (const auto& mem : b->LearnedMatrices())
            {
                if (mem.inverse) continue;
                bool known = false;
                for (const auto& t : targets) known |= (t.matrixRegister == mem.reg);
                if (known) continue;
                const EyeShiftMatrix em = { mem.reg, mem.transposed, FALSE, TRUE };
                targets.push_back(em);
            }
        m_drawPatchScratch.assign(bytes.begin(), bytes.end());
        unsigned shifted = 0, inverses = 0;
        ApplyTargetedEyeShiftToCB(m_drawPatchScratch.data(), m_drawPatchScratch.size(), eyeShift, conv, targets, true,
                                  &shifted, nullptr, &inverses, b);
        if (!shifted || !WriteWholeRightCB(b, m_drawPatchScratch)) continue;
        NoteRightConstants(b);
        b->NoteRightShift(shifted - inverses, inverses);
        m_invDrawPatchedThisFrame += inverses;
        m_fwdDrawPatchedThisFrame += shifted - inverses;
        if (gInfo.CollectDebugInformation) ++m_invDrawByPS[ps->crc32];
    }
}

} // namespace wiz3d
