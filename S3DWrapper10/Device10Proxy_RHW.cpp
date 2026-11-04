/*
 * wiz3D - Device10Proxy right-hand wrapper (RHW): per-draw right-eye edit +
 * shader primitive engine. DX10 equivalent of Context11Proxy_RHW.cpp.
 *
 * Mirrors the Context11Proxy primitive-engine methods so the 0.4.3 shader
 * primitives (<SkipStereo>, <FixedDepth>, <ShiftUV>, <DepthFromCB>,
 * <AnimateDepth>, <OverrideCBValue>, <PerEyeTexture>, <SpriteDepth>,
 * <ParallaxLayer>, <When TextureCRC>) + ShaderHunter note-draw fire on DX10.
 *
 * The 37 CB-math features from Akshay's PR #31 remain DX11-only in this
 * cycle -- each takes Context11Proxy& and is 100-500 LOC of DX11-coupled
 * logic. Porting those is a separate follow-on initiative.
 */

#include "StdAfx.h"
#include "Device10Proxy.h"
#include "Buffer10Proxy.h"
#include "../S3DAPI/GlobalData.h"
#include "../S3DAPI/ShaderProfileData.h"
#include "../S3DAPI/StereoAPIInternal.h"
#include "ShaderHunter.h"
#include "ShaderPatch11.h"       // PatchShader -- SM4/SM5 DXBC patcher, works for DX10 too
#include "FixedDepth11.h"        // FixedDepthPlan
#include "ShiftUV11.h"           // ShiftUVPlan
#include <cmath>
#include <cstring>

namespace wiz3d
{

// FNV-1a 32-bit over an ID3D10Resource* pointer, matching the DX11 side so
// Hunter's draw-level fingerprint capture (Numpad 5) produces consistent
// hashes across wrappers.
static uint32_t FnvHashPtr10(const void* p)
{
    uint32_t h = 0x811c9dc5u;
    const uint8_t* b = reinterpret_cast<const uint8_t*>(&p);
    for (size_t i = 0; i < sizeof(void*); ++i)
    {
        h ^= b[i];
        h *= 0x01000193u;
    }
    return h;
}

// Hash the ID3D10Resource* under each currently-bound PS SRV. Used by
// <When TextureCRC="..."> sub-filter + Hunter fingerprint.
void Device10Proxy::PsBoundResourceHashes10(uint32_t out[16], unsigned& count)
{
    count = 0;
    for (UINT slot = 0; slot < 16 && count < 16; ++slot)
    {
        ID3D10ShaderResourceView* srv = m_srvSlots[ST10_PS][slot];
        if (!srv) continue;
        ID3D10Resource* res = nullptr;
        srv->GetResource(&res);
        if (!res) continue;
        out[count++] = FnvHashPtr10(res);
        res->Release();
    }
}

// <When TextureCRC="0x..."> sub-filter -- scope a primitive to fire only when
// a specific texture is bound on the PS pipeline. Matches the DX11 semantics.
bool Device10Proxy::WhenFilterMatches10(const ::ShaderProfileData& p)
{
    if (p.m_WhenTextureCRCs.empty()) return true;
    uint32_t hashes[16];
    unsigned n = 0;
    PsBoundResourceHashes10(hashes, n);
    for (const DWORD want : p.m_WhenTextureCRCs)
    {
        for (unsigned i = 0; i < n; ++i)
            if (hashes[i] == static_cast<uint32_t>(want))
                return true;
    }
    return false;
}

// <SkipStereo/> short-circuit. If either bound VS or PS has SkipStereo set
// in its profile, the right eye renders with the left eye's CB bytes
// (identical to left) and primitive dispatch is skipped. Returns true when
// the current draw should skip the right-eye edit pass entirely.
bool Device10Proxy::BindSkipStereoGate(bool& skipRightEye)
{
    skipRightEye = false;
    const ShaderAnalysis11Result* vsInfo = m_boundInfo[ST10_VS];
    const ShaderAnalysis11Result* psInfo = m_boundInfo[ST10_PS];

    if (vsInfo)
    {
        auto it = g_ProfileData.VSCRCData.find(vsInfo->crc32);
        if (it != g_ProfileData.VSCRCData.end() && it->second.m_bSkipStereo)
        {
            if (!WhenFilterMatches10(it->second)) {}
            else { skipRightEye = true; return true; }
        }
    }
    if (psInfo)
    {
        auto it = g_ProfileData.PSCRCData.find(psInfo->crc32);
        if (it != g_ProfileData.PSCRCData.end() && it->second.m_bSkipStereo)
        {
            if (!WhenFilterMatches10(it->second)) {}
            else { skipRightEye = true; return true; }
        }
    }
    return false;
}

// Snapshot the right-CB bindings for the current draw. Called at Draw-record
// time in the game's thread. Buffer10Proxy pointers are captured + AddRef'd
// via ComRefHolder10 so they outlive the game's own releases through replay.
Device10Proxy::DrawSnapshot10 Device10Proxy::CaptureDrawSnapshot10()
{
    DrawSnapshot10 snap;

    const ShaderAnalysis11Result* vsInfo = m_boundInfo[ST10_VS];
    const ShaderAnalysis11Result* psInfo = m_boundInfo[ST10_PS];
    const ShaderAnalysis11Result* gsInfo = m_boundInfo[ST10_GS];
    snap.vsInfo = vsInfo;
    snap.psInfo = psInfo;

    // SkipStereo gating — mirrors BindSkipStereoGate but in snapshot form.
    if (vsInfo)
    {
        auto it = g_ProfileData.VSCRCData.find(vsInfo->crc32);
        if (it != g_ProfileData.VSCRCData.end() && it->second.m_bSkipStereo &&
            WhenFilterMatches10(it->second))
            snap.skipStereo = true;
    }
    if (!snap.skipStereo && psInfo)
    {
        auto it = g_ProfileData.PSCRCData.find(psInfo->crc32);
        if (it != g_ProfileData.PSCRCData.end() && it->second.m_bSkipStereo &&
            WhenFilterMatches10(it->second))
            snap.skipStereo = true;
    }
    if (snap.skipStereo) return snap;

    // Gather (stage, slot, Buffer10Proxy*) for each rightCB.
    auto gather = [&](int st, const ShaderAnalysis11Result* info) {
        if (!info) return;
        for (const auto& named : info->rightCBs)
        {
            UINT slot = static_cast<UINT>(named.second);
            if (slot >= kMaxVSCBSlots) continue;
            Buffer10Proxy* bp = m_cbSlots[st][slot];
            if (!bp) continue;
            RightCBSnap10 e;
            e.stage = st;
            e.slot  = slot;
            e.bp    = bp;
            snap.rightCBs.push_back(e);
            snap.cbHolders.emplace_back(static_cast<IUnknown*>(bp));
        }
    };
    gather(ST10_VS, vsInfo);
    gather(ST10_GS, gsInfo);
    gather(ST10_PS, psInfo);

    return snap;
}

// Right-eye replay: write shifted bytes into each snapshotted Buffer10Proxy's
// right sibling and bind it. Falls back silently if the sibling can't be
// created or the shadow is empty.
void Device10Proxy::ApplySnapshotRightCBs10(const DrawSnapshot10& snap)
{
    if (snap.skipStereo || snap.rightCBs.empty()) return;

    const float eyeShift = wiz3D_GetEffectiveEyeShift();
    const float conv     = 0.f;

    for (const auto& e : snap.rightCBs)
    {
        Buffer10Proxy* b = e.bp;
        if (!b || b->Shadow().empty()) continue;

        ID3D10Buffer* sibling = b->RightCopy(0);
        if (!sibling) continue;

        if (b->NeedsRightEdit(0, eyeShift, conv))
        {
            m_editScratch10.assign(b->Shadow().begin(), b->Shadow().end());
            // Apply projection-matrix eye-shift heuristic (same as Option B
            // fallback). The 37 features would layer on top here on DX11.
            if (eyeShift != 0.f && !m_editScratch10.empty())
            {
                constexpr size_t kMat4Bytes = 16 * sizeof(float);
                unsigned char* data = m_editScratch10.data();
                const size_t byteCount = m_editScratch10.size();
                for (size_t off = 0; off + kMat4Bytes <= byteCount; off += 4)
                {
                    float* f = reinterpret_cast<float*>(data + off);
                    if (f[11] != 1.f) continue;
                    if (f[15] != 0.f) continue;
                    if (f[0]  == 0.f) continue;
                    if (f[5]  == 0.f) continue;
                    f[8] += eyeShift * f[0];
                }
            }
            m_real->UpdateSubresource(sibling, 0, nullptr,
                                      m_editScratch10.data(),
                                      static_cast<UINT>(m_editScratch10.size()),
                                      0);
            b->NoteRightWrite();
            b->MarkRightEdited(0, eyeShift, conv);
        }

        ID3D10Buffer* bufs[1] = { sibling };
        switch (e.stage)
        {
        case ST10_VS: m_real->VSSetConstantBuffers(e.slot, 1, bufs); break;
        case ST10_GS: m_real->GSSetConstantBuffers(e.slot, 1, bufs); break;
        case ST10_PS: m_real->PSSetConstantBuffers(e.slot, 1, bufs); break;
        }
    }
}

// Per-draw right-eye CB edit entry point. Mirrors Context11Proxy::EditRightCBs
// minus the 37-feature dispatch. For each bound CB on VS/GS/PS that the
// analyzer flagged as a right-eye target (rightCBs), this writes shifted
// bytes into the Buffer10Proxy's right sibling and rebinds it.
//
// Called by the primitive engine's direct path (without snapshot). The
// snapshot path goes through ApplySnapshotRightCBs10 and is the primary
// entry point from Draw closures.
void Device10Proxy::EditRightCBs10()
{
    // SkipStereo short-circuit: identical behaviour on DX10 to DX11.
    bool skipRightEye = false;
    if (BindSkipStereoGate(skipRightEye)) return;

    const ShaderAnalysis11Result* infos[ST10_COUNT] = {
        m_boundInfo[ST10_VS], m_boundInfo[ST10_GS], m_boundInfo[ST10_PS]
    };
    if (!(infos[ST10_VS] && !infos[ST10_VS]->rightCBs.empty()) &&
        !(infos[ST10_GS] && !infos[ST10_GS]->rightCBs.empty()) &&
        !(infos[ST10_PS] && !infos[ST10_PS]->rightCBs.empty()))
        return;

    const float eyeShift = wiz3D_GetEffectiveEyeShift();
    const float conv     = 0.f;  // DX10 doesn't yet track convergence separately

    for (int s = 0; s < ST10_COUNT; ++s)
    {
        const ShaderAnalysis11Result* info = infos[s];
        if (!info) continue;
        for (const auto& named : info->rightCBs)
        {
            const UINT slot = static_cast<UINT>(named.second);
            if (slot >= kMaxVSCBSlots) continue;
            Buffer10Proxy* b = m_cbSlots[s][slot];
            if (!b || b->Shadow().empty()) continue;

            ID3D10Buffer* sibling = b->RightCopy(0);
            if (!sibling) continue;

            if (b->NeedsRightEdit(0, eyeShift, conv))
            {
                m_editScratch10.assign(b->Shadow().begin(), b->Shadow().end());
                // Features would hook in here on the DX11 side; cycle 2 only
                // applies the basic eye-shift. The 37-feature port is cycle 4+.
                // Apply the m[2][3]==1 / m[3][3]==0 projection-matrix heuristic
                // the DX10 replay path already uses (Buffer10Proxy's
                // ApplyEyeShiftToCB10) so basic CB eye-shift keeps working.
                if (!m_editScratch10.empty() && eyeShift != 0.f)
                {
                    constexpr size_t kMat4Bytes = 16 * sizeof(float);
                    unsigned char* data = m_editScratch10.data();
                    const size_t byteCount = m_editScratch10.size();
                    for (size_t off = 0; off + kMat4Bytes <= byteCount; off += 4)
                    {
                        float* f = reinterpret_cast<float*>(data + off);
                        if (f[11] != 1.f) continue;
                        if (f[15] != 0.f) continue;
                        if (f[0]  == 0.f) continue;
                        if (f[5]  == 0.f) continue;
                        f[8] += eyeShift * f[0];
                    }
                }

                // Write scratch bytes into the right sibling. DX10 buffers
                // need Map+memcpy+Unmap since D3D10_USAGE_DYNAMIC was likely
                // used by the game; for siblings we used DEFAULT in
                // RightCopy(), so use UpdateSubresource instead.
                m_real->UpdateSubresource(sibling, 0, nullptr,
                                          m_editScratch10.data(),
                                          static_cast<UINT>(m_editScratch10.size()),
                                          0);
                b->NoteRightWrite();
                b->MarkRightEdited(0, eyeShift, conv);
            }

            // Bind the right sibling for the right-eye draw.
            ID3D10Buffer* bufs[1] = { sibling };
            switch (s)
            {
            case ST10_VS: m_real->VSSetConstantBuffers(slot, 1, bufs); break;
            case ST10_GS: m_real->GSSetConstantBuffers(slot, 1, bufs); break;
            case ST10_PS: m_real->PSSetConstantBuffers(slot, 1, bufs); break;
            }
        }
    }

    m_editLastShift = eyeShift;
    m_editLastConv  = conv;
}

// --- DXBC-twin primitive engine (cycle 2b) ---------------------------------
//
// Mirrors Context11Proxy::BindTwin: compiles a right-eye twin of the game's
// VS via PatchShader + ID3D10Device::CreateVertexShader, caches by
// (stage, tag, CRC), binds the twin + a 64-byte dynamic CB holding the
// plan's constants on the right-eye draw. UnbindTwin10 restores the
// saved VS + CB at the twin's slot after the draw.
//
// The twin CB slot is picked at compile time by scanning D3D10's 14-slot
// CB space from the top down, same as DX11 (PatchShader refuses a slot
// the game's shader declares, so we try high slots first).

namespace {

// Reserve the top 6 slots (8..13) of DX10's 14-CB space for twin CBs.
// PatchShader will try these in descending order until one is unused.
constexpr UINT kTwinSlotScanMax10 = 13;
constexpr UINT kTwinSlotScanMin10 = 8;

// Twin-plan hash: combine stage, plan-tag, and CRC into a 64-bit key.
inline unsigned long long TwinKey10(int stage, int tag, DWORD crc32)
{
    return ((unsigned long long)stage << 40)
         | ((unsigned long long)(tag & 0xFF) << 32)
         | (unsigned long long)crc32;
}

// Lazy-create the 64-byte dynamic CB that carries the plan constants.
bool EnsureTwinCB10(Device10Proxy* self, ID3D10Device* dev, ID3D10Buffer*& outCB)
{
    if (outCB) return true;
    if (!dev) return false;
    D3D10_BUFFER_DESC bd = {};
    bd.ByteWidth      = 64;
    bd.Usage          = D3D10_USAGE_DYNAMIC;
    bd.BindFlags      = D3D10_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D10_CPU_ACCESS_WRITE;
    ID3D10Buffer* cb = nullptr;
    if (FAILED(dev->CreateBuffer(&bd, nullptr, &cb)) || !cb) return false;
    outCB = cb;
    return true;
}

} // namespace

// Build (or fetch cached) VS twin for a given plan + tag combination.
// Returns the cached twin's shader + slot; nullptr shader on failure.
static Device10Proxy::VSTwin10 GetOrCompileTwin10(
    Device10Proxy* self,
    ID3D10Device* dev,
    const ShaderAnalysis11Result* info,
    int tag,
    const ShaderPatchPlan& plan,
    CRITICAL_SECTION& lock,
    std::unordered_map<unsigned long long, Device10Proxy::VSTwin10>& cache)
{
    const unsigned long long key = TwinKey10(Device10Proxy::ST10_VS, tag, info->crc32);

    EnterCriticalSection(&lock);
    auto it = cache.find(key);
    if (it != cache.end())
    {
        Device10Proxy::VSTwin10 twin = it->second;
        LeaveCriticalSection(&lock);
        return twin;
    }
    LeaveCriticalSection(&lock);

    Device10Proxy::VSTwin10 twin;
    if (!info->bytecode || info->bytecode->empty()) return twin;

    std::vector<unsigned char> code;
    for (UINT slot = kTwinSlotScanMax10; slot >= kTwinSlotScanMin10 && !twin.shader; --slot)
    {
        code.clear();
        if (!wiz3d::PatchShader(info->bytecode->data(), info->bytecode->size(),
                                 plan, slot, code))
            continue;
        ID3D10VertexShader* s = nullptr;
        if (SUCCEEDED(dev->CreateVertexShader(code.data(), code.size(), &s)) && s)
        {
            twin.shader = s;
            twin.slot   = slot;
            break;
        }
    }

    EnterCriticalSection(&lock);
    auto ins = cache.insert({ key, twin });
    if (!ins.second && twin.shader)
    {
        // Lost the race; release our twin and use the cached one.
        twin.shader->Release();
        twin = ins.first->second;
    }
    LeaveCriticalSection(&lock);
    return twin;
}

// Bind a VS twin: compile if needed, save the game's current VS + CB at the
// twin's slot, upload the plan constants into the twin CB, bind twin + CB.
// Returns true on success; caller must call UnbindClipSpaceShift10 after.
static bool BindTwin10(Device10Proxy* self, ID3D10Device* dev, ID3D10Device* real,
                       const ShaderAnalysis11Result* info, int tag,
                       const ShaderPatchPlan& plan,
                       const float* consts, UINT count,
                       int& outTwinStage, UINT& outTwinSlot,
                       ID3D10VertexShader*& outSavedVS, ID3D10Buffer*& outSavedCB,
                       ID3D10Buffer*& twinCB,
                       CRITICAL_SECTION& lock,
                       std::unordered_map<unsigned long long, Device10Proxy::VSTwin10>& cache)
{
    if (!info || count > 16) return false;
    if (outTwinStage >= 0) return false;  // already bound; caller forgot to unbind

    Device10Proxy::VSTwin10 twin = GetOrCompileTwin10(self, dev, info, tag, plan, lock, cache);
    if (!twin.shader) return false;

    if (!EnsureTwinCB10(self, dev, twinCB)) return false;

    // Upload constants: Map-discard + memcpy.
    D3D10_MAPPED_TEXTURE2D m2 = {};
    void* mapped = nullptr;
    if (FAILED(twinCB->Map(D3D10_MAP_WRITE_DISCARD, 0, &mapped)) || !mapped) return false;
    memset(mapped, 0, 64);
    memcpy(mapped, consts, count * sizeof(float));
    twinCB->Unmap();

    // Save current VS + CB at twin slot so UnbindTwin10 can restore them.
    ID3D10VertexShader* savedVS = nullptr;
    real->VSGetShader(&savedVS);
    outSavedVS = savedVS;
    ID3D10Buffer* savedCB = nullptr;
    real->VSGetConstantBuffers(twin.slot, 1, &savedCB);
    outSavedCB = savedCB;

    // Bind twin.
    real->VSSetShader(twin.shader);
    ID3D10Buffer* cbs[1] = { twinCB };
    real->VSSetConstantBuffers(twin.slot, 1, cbs);

    outTwinStage = Device10Proxy::ST10_VS;
    outTwinSlot  = twin.slot;
    return true;
    (void)m2;  // unused
}

// Dispatch: check the bound VS's profile for a primitive and bind the
// corresponding twin + CB. Priority mirrors Context11Proxy::BindClipSpaceShift:
// PerEyeTexture -> ShiftUV -> AnimateDepth -> DepthFromCB -> FixedDepth ->
// OverrideCBValue. Only triggered on the right-eye replay pass.
// Takes explicit vsInfo/psInfo because at replay time m_boundInfo holds the
// FINAL game state, not the state at each draw.
bool Device10Proxy::BindClipSpaceShift10(const ShaderAnalysis11Result* vsInfo,
                                          const ShaderAnalysis11Result* psInfo)
{
    (void)psInfo;  // only VS primitives reach here in cycle 2b
    if (!vsInfo) return false;

    auto it = g_ProfileData.VSCRCData.find(vsInfo->crc32);
    if (it == g_ProfileData.VSCRCData.end()) return false;
    const ShaderProfileData& p = it->second;

    if (!WhenFilterMatches10(p)) return false;

    // PerEyeTexture: no twin, just SRV substitution.
    if (p.m_PerEye_Slot >= 0 &&
        BindPerEyeTexture10(vsInfo, ST10_VS, p.m_PerEye_Slot,
                             p.m_PerEye_LeftCRC, p.m_PerEye_RightCRC))
        return true;

    // ShiftUV: twin via ShiftUVPlan.
    if (p.m_ShiftUV_OutputReg >= 0 &&
        BindShiftUV10(vsInfo, ST10_VS, p.m_ShiftUV_OutputReg,
                       p.m_ShiftUV_Component, p.m_ShiftUV_Amount))
        return true;

    // AnimateDepth: twin via FixedDepthPlan, depth = lerp(start, end, progress).
    if (p.m_AnimDepth_ProgressCB >= 0 &&
        BindAnimateDepth10(vsInfo, ST10_VS, p.m_AnimDepth_ProgressCB,
                            p.m_AnimDepth_ProgressOffset,
                            p.m_AnimDepth_Start, p.m_AnimDepth_End))
        return true;

    // DepthFromCB: twin via FixedDepthPlan, depth read from a CB slot.
    if (p.m_DepthFromCB_Slot >= 0 &&
        BindDepthFromCB10(vsInfo, ST10_VS, p.m_DepthFromCB_Slot,
                           p.m_DepthFromCB_OffsetBytes))
        return true;

    // FixedDepth: twin via FixedDepthPlan with static scalar.
    if (!std::isnan(p.m_FixedDepth) &&
        BindFixedDepth10(vsInfo, ST10_VS, p.m_FixedDepth))
        return true;

    // OverrideCBValue: no twin; write a float into the right-eye CB.
    if (p.m_OverrideCB_Slot >= 0 &&
        BindOverrideCBValue10(vsInfo, ST10_VS, p.m_OverrideCB_Slot,
                               p.m_OverrideCB_OffsetBytes, p.m_OverrideCB_Value))
        return true;

    return false;
}

// Restore the shader + CB saved by BindTwin10.
void Device10Proxy::UnbindClipSpaceShift10()
{
    if (m_twinStage10 < 0) return;
    if (m_twinStage10 == ST10_VS)
    {
        m_real->VSSetShader(m_twinSavedVS10);
        ID3D10Buffer* cbs[1] = { m_twinSavedCB10 };
        m_real->VSSetConstantBuffers(m_twinSavedSlot10, 1, cbs);
    }
    if (m_twinSavedVS10) { m_twinSavedVS10->Release(); m_twinSavedVS10 = nullptr; }
    if (m_twinSavedCB10) { m_twinSavedCB10->Release(); m_twinSavedCB10 = nullptr; }
    m_twinStage10 = -1;
}

// <FixedDepth Value="X"/> — pin clip-space x to a scalar depth.
bool Device10Proxy::BindFixedDepth10(const ShaderAnalysis11Result* info, StageIdx10 st, float depth)
{
    if (!info || st != ST10_VS) return false;
    const float eyeShift = wiz3D_GetEffectiveEyeShift();
    // Simplified vs DX11: no per-shader multiplier or EyeShiftSign (features
    // own those on DX11). We approximate by taking eyeShift * depth directly.
    const float k[4] = { eyeShift * depth, 0.f, 0.f, 0.f };
    return BindTwin10(this, m_real, m_real, info, /*tag=*/3, FixedDepthPlan(), k, 4,
                      m_twinStage10, m_twinSavedSlot10, m_twinSavedVS10, m_twinSavedCB10,
                      m_twinCB10, m_vsTwinsLock10, m_vsTwins10);
}

// <DepthFromCB CB=N OffsetBytes=M/> — depth read from a bound CB float.
bool Device10Proxy::BindDepthFromCB10(const ShaderAnalysis11Result* info, StageIdx10 st,
                                       int cbSlot, int offsetBytes)
{
    if (!info || st != ST10_VS) return false;
    if (cbSlot < 0 || (UINT)cbSlot >= kMaxVSCBSlots || offsetBytes < 0) return false;
    Buffer10Proxy* b = m_cbSlots[ST10_VS][cbSlot];
    if (!b || b->Shadow().empty()) return false;
    if ((size_t)offsetBytes + sizeof(float) > b->Shadow().size()) return false;
    float depth = 0.f;
    memcpy(&depth, b->Shadow().data() + offsetBytes, sizeof(float));
    return BindFixedDepth10(info, st, depth);
}

// <AnimateDepth ProgressCB=N Offset=M Start=A End=B/>
bool Device10Proxy::BindAnimateDepth10(const ShaderAnalysis11Result* info, StageIdx10 st,
                                        int cbSlot, int offsetBytes, float start, float end)
{
    if (!info || st != ST10_VS) return false;
    if (cbSlot < 0 || (UINT)cbSlot >= kMaxVSCBSlots || offsetBytes < 0) return false;
    Buffer10Proxy* b = m_cbSlots[ST10_VS][cbSlot];
    if (!b || b->Shadow().empty()) return false;
    if ((size_t)offsetBytes + sizeof(float) > b->Shadow().size()) return false;
    float progress = 0.f;
    memcpy(&progress, b->Shadow().data() + offsetBytes, sizeof(float));
    if (progress < 0.f) progress = 0.f;
    if (progress > 1.f) progress = 1.f;
    const float depth = start + (end - start) * progress;
    return BindFixedDepth10(info, st, depth);
}

// <ShiftUV OutputReg=N Component=x|y Amount=X/>
bool Device10Proxy::BindShiftUV10(const ShaderAnalysis11Result* info, StageIdx10 st,
                                   int outputReg, int component, float amount)
{
    if (!info || st != ST10_VS) return false;
    if (outputReg < 0 || component < 0 || component > 3) return false;
    const float eyeShift = wiz3D_GetEffectiveEyeShift();
    const float k[4] = { eyeShift * amount, 0.f, 0.f, 0.f };
    return BindTwin10(this, m_real, m_real, info, /*tag=*/5,
                      ShiftUVPlan((unsigned)outputReg, (unsigned)component), k, 4,
                      m_twinStage10, m_twinSavedSlot10, m_twinSavedVS10, m_twinSavedCB10,
                      m_twinCB10, m_vsTwinsLock10, m_vsTwins10);
}

// <OverrideCBValue CB=N OffsetBytes=M Value=X/> — no twin; write a float into
// the right-eye sibling CB.
bool Device10Proxy::BindOverrideCBValue10(const ShaderAnalysis11Result* info, StageIdx10 st,
                                           int cbSlot, int offsetBytes, float value)
{
    if (!info || st != ST10_VS) return false;
    if (cbSlot < 0 || (UINT)cbSlot >= kMaxVSCBSlots || offsetBytes < 0) return false;
    Buffer10Proxy* b = m_cbSlots[ST10_VS][cbSlot];
    if (!b || b->Shadow().empty()) return false;
    if ((size_t)offsetBytes + sizeof(float) > b->Shadow().size()) return false;

    ID3D10Buffer* rawCB = b->GetRealRight();
    if (!rawCB) rawCB = b->GetReal();
    if (!rawCB) return false;

    D3D10_BUFFER_DESC desc = {};
    rawCB->GetDesc(&desc);
    if (!(desc.CPUAccessFlags & D3D10_CPU_ACCESS_WRITE))
    {
        // Can't Map a non-dynamic CB; fall back to UpdateSubresource via scratch.
        std::vector<unsigned char> tmp(b->Shadow().begin(), b->Shadow().end());
        memcpy(tmp.data() + offsetBytes, &value, sizeof(float));
        m_real->UpdateSubresource(rawCB, 0, nullptr, tmp.data(), (UINT)tmp.size(), 0);
        return true;
    }

    void* mapped = nullptr;
    if (FAILED(rawCB->Map(D3D10_MAP_WRITE_DISCARD, 0, &mapped)) || !mapped) return false;
    memcpy(mapped, b->Shadow().data(), b->Shadow().size());
    memcpy((char*)mapped + offsetBytes, &value, sizeof(float));
    rawCB->Unmap();
    return true;
}

// <PerEyeTexture Slot=N LeftCRC=X RightCRC=Y/> — SRV swap on right eye.
// Scans currently-bound PS SRVs for a matching leftCRC + rightCRC pair
// (FNV-1a over ID3D10Resource* pointer, same hash as Hunter/WhenFilter).
bool Device10Proxy::BindPerEyeTexture10(const ShaderAnalysis11Result* info, StageIdx10 st,
                                         int slot, DWORD leftCRC, DWORD rightCRC)
{
    (void)info;
    if (st != ST10_PS || slot < 0 || slot >= 16 || !leftCRC || !rightCRC) return false;

    // Look for the right-eye SRV in currently-bound PS SRVs.
    ID3D10ShaderResourceView* leftAtSlot = m_srvSlots[ST10_PS][slot];
    if (!leftAtSlot) return false;
    ID3D10Resource* leftRes = nullptr;
    leftAtSlot->GetResource(&leftRes);
    if (!leftRes) return false;
    const uint32_t leftHash = FnvHashPtr10(leftRes);
    leftRes->Release();
    if (leftHash != leftCRC) return false;

    ID3D10ShaderResourceView* rightSRV = nullptr;
    for (int i = 0; i < 16 && !rightSRV; ++i)
    {
        ID3D10ShaderResourceView* srv = m_srvSlots[ST10_PS][i];
        if (!srv) continue;
        ID3D10Resource* r = nullptr;
        srv->GetResource(&r);
        if (!r) continue;
        if (FnvHashPtr10(r) == rightCRC) rightSRV = srv;
        r->Release();
    }
    if (!rightSRV) return false;

    ID3D10ShaderResourceView* bind[1] = { rightSRV };
    m_real->PSSetShaderResources((UINT)slot, 1, bind);
    // No explicit unbind: next game PSSetShaderResources will restore naturally.
    return true;
}

// Hunter integration: on each real draw, note the CRC + stage combo and
// capture a per-draw fingerprint when hunt mode is in Draw granularity.
// The DX10 analyzer stores CRCs in ShaderAnalysis11Result::crc32, same as DX11.
void Device10Proxy::HuntMaybeCaptureFingerprint10()
{
    // Draw-level fingerprint capture is driven by the Hunter's own state;
    // this method is called after NoteDraw at each draw site.
    if (!ShaderHunter::ShouldCaptureFingerprint()) return;

    uint32_t hashes[16];
    unsigned n = 0;
    PsBoundResourceHashes10(hashes, n);

    ShaderHunter::StoreCurrentDrawFingerprint(hashes, n);
}

} // namespace wiz3d
