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
#include "ShaderHunter.h"
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

// --- Primitive engine skeletons --------------------------------------------
// These will fill in across cycle 2b. For now they're stubs so EditRightCBs10
// + the public header surface compiles.

bool Device10Proxy::BindClipSpaceShift10() { return false; }
void Device10Proxy::UnbindClipSpaceShift10() {}

bool Device10Proxy::BindFixedDepth10(const ShaderAnalysis11Result*, StageIdx10, float)
{ return false; }

bool Device10Proxy::BindShiftUV10(const ShaderAnalysis11Result*, StageIdx10, int, int, float)
{ return false; }

bool Device10Proxy::BindDepthFromCB10(const ShaderAnalysis11Result*, StageIdx10, int, int)
{ return false; }

bool Device10Proxy::BindAnimateDepth10(const ShaderAnalysis11Result*, StageIdx10, int, int, float, float)
{ return false; }

bool Device10Proxy::BindOverrideCBValue10(const ShaderAnalysis11Result*, StageIdx10, int, int, float)
{ return false; }

bool Device10Proxy::BindPerEyeTexture10(const ShaderAnalysis11Result*, StageIdx10, int, DWORD, DWORD)
{ return false; }

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
