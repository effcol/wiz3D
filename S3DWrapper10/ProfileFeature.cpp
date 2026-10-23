/*
* Project : wiz3D
* Copyright (C) Akshay Hegde 2026
*/

// Per-game features: the RDEF reader and the registry (see ProfileFeature.h).

#include "StdAfx.h"
#include "ProfileFeature.h"
#include "ShaderAnalyzer11.h"              // FindDxbcChunk
#include "AdapterFunctions.h"              // DDILog
#include "..\S3DAPI\ShaderProfileData.h"   // g_ProfileData
#include <cstdarg>
#include <mutex>

namespace wiz3d
{

void FeatureLog(const char* fmt, ...)
{
    if (!gInfo.CollectDebugInformation) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    const int n = _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    if (n != 0) DDILog("%s", buf);
}

// RDEF layout: header 28 B (+'RD11' at 28 on SM5), resource binds 32 B, cbuffers 24 B, variables 24 B (SM5: 40 B).
RdefView::RdefView(const void* bytecode, SIZE_T length)
{
    DWORD size = 0;
    const BYTE* rd = FindDxbcChunk(bytecode, length, 0x46454452 /* 'RDEF' */, size);
    if (!rd || size < 28) return;
    m_rd = rd;
    m_size = size;
    m_varStride = ((size >= 32 && Rd32(28) == 0x31314452 /* 'RD11' */) || rd[17] >= 5) ? 40u : 24u;
}

DWORD RdefView::Rd32(DWORD off) const
{
    DWORD v = 0;
    if (m_rd && off + 4 <= m_size) memcpy(&v, m_rd + off, 4);
    return v;
}

const char* RdefView::Str(DWORD off) const
{
    for (DWORD i = off; m_rd && i < m_size; ++i) if (!m_rd[i]) return (const char*)m_rd + off;
    return "";
}

int RdefView::TextureSlot(const std::string& name) const
{
    const DWORD resCount = Rd32(8), resOff = Rd32(12);
    if (!m_rd || name.empty() || resOff + resCount * 32u > m_size) return -1;
    int slot = -1;
    for (DWORD i = 0; i < resCount; ++i)
        if (Rd32(resOff + i * 32u + 4) == 2 /* D3D_SIT_TEXTURE */ && name == Str(Rd32(resOff + i * 32u)))
            slot = (int)Rd32(resOff + i * 32u + 20);
    return slot;
}

bool RdefView::FindVariable(const std::string& name, std::string& cbuffer, int& slot, UINT& offset, UINT& size) const
{
    const DWORD cbCount = Rd32(0), cbOff = Rd32(4), resCount = Rd32(8), resOff = Rd32(12);
    if (!m_rd || name.empty() || cbOff + cbCount * 24u > m_size || resOff + resCount * 32u > m_size) return false;
    for (DWORD c = 0; c < cbCount; ++c)
    {
        const DWORD cb = cbOff + c * 24u, varCount = Rd32(cb + 4), varOff = Rd32(cb + 8);
        if (varOff + varCount * m_varStride > m_size) continue;
        int s = -1;
        for (DWORD i = 0; i < resCount; ++i)
            if (Rd32(resOff + i * 32u + 4) == 0 && !strcmp(Str(Rd32(resOff + i * 32u)), Str(Rd32(cb))))
                s = (int)Rd32(resOff + i * 32u + 20);
        if (s < 0) continue;
        for (DWORD v = 0; v < varCount; ++v)
        {
            const DWORD var = varOff + v * m_varStride;
            if (name != Str(Rd32(var))) continue;
            cbuffer = Str(Rd32(cb));
            slot = s;
            offset = Rd32(var + 4);
            size = Rd32(var + 8);
            return true;
        }
    }
    return false;
}

int RdefView::SrvSlot(const std::string& name) const
{
    const DWORD resCount = Rd32(8), resOff = Rd32(12);
    if (!m_rd || name.empty() || resOff + resCount * 32u > m_size) return -1;
    for (DWORD i = 0; i < resCount; ++i)
    {
        const DWORD type = Rd32(resOff + i * 32u + 4);
        // D3D_SIT_TEXTURE 2, STRUCTURED 5, BYTEADDRESS 7
        if ((type == 2 || type == 5 || type == 7) && name == Str(Rd32(resOff + i * 32u)))
            return (int)Rd32(resOff + i * 32u + 20);
    }
    return -1;
}

bool RdefView::UsesSrvAtSlot(UINT slot) const
{
    const DWORD resCount = Rd32(8), resOff = Rd32(12);
    if (!m_rd || resOff + resCount * 32u > m_size) return false;
    for (DWORD i = 0; i < resCount; ++i)
    {
        const DWORD type = Rd32(resOff + i * 32u + 4);
        if ((type == 2 || type == 5 || type == 7) &&
            Rd32(resOff + i * 32u + 20) == slot)
            return true;
    }
    return false;
}

std::vector<std::pair<int, UINT>> RdefView::Variables(const std::string& name, bool usedOnly) const
{
    std::vector<std::pair<int, UINT>> out;
    const DWORD cbCount = Rd32(0), cbOff = Rd32(4), resCount = Rd32(8), resOff = Rd32(12);
    if (!m_rd || name.empty() || cbOff + cbCount * 24u > m_size || resOff + resCount * 32u > m_size) return out;
    for (DWORD c = 0; c < cbCount; ++c)
    {
        const DWORD cb = cbOff + c * 24u, varCount = Rd32(cb + 4), varOff = Rd32(cb + 8);
        if (varOff + varCount * m_varStride > m_size) continue;
        // The cbuffer's slot is the D3D_SIT_CBUFFER binding of the same name.
        int slot = -1;
        for (DWORD i = 0; i < resCount; ++i)
            if (Rd32(resOff + i * 32u + 4) == 0 && !strcmp(Str(Rd32(resOff + i * 32u)), Str(Rd32(cb))))
                slot = (int)Rd32(resOff + i * 32u + 20);
        if (slot < 0) continue;
        for (DWORD v = 0; v < varCount; ++v)
        {
            const DWORD var = varOff + v * m_varStride;
            if (name != Str(Rd32(var))) continue;
            if (usedOnly && !(Rd32(var + 12) & 2 /* D3D_SVF_USED */)) continue;
            out.push_back(std::make_pair(slot, (UINT)Rd32(var + 4)));
        }
    }
    return out;
}

std::vector<std::pair<int, UINT>> RdefView::UsedMatrices() const
{
    std::vector<std::pair<int, UINT>> out;
    const DWORD cbCount = Rd32(0), cbOff = Rd32(4), resCount = Rd32(8), resOff = Rd32(12);
    if (!m_rd || cbOff + cbCount * 24u > m_size || resOff + resCount * 32u > m_size) return out;
    for (DWORD c = 0; c < cbCount; ++c)
    {
        const DWORD cb = cbOff + c * 24u, varCount = Rd32(cb + 4), varOff = Rd32(cb + 8);
        if (varOff + varCount * m_varStride > m_size) continue;
        int slot = -1;
        for (DWORD i = 0; i < resCount; ++i)
            if (Rd32(resOff + i * 32u + 4) == 0 && !strcmp(Str(Rd32(resOff + i * 32u)), Str(Rd32(cb))))
                slot = (int)Rd32(resOff + i * 32u + 20);
        if (slot < 0) continue;
        for (DWORD v = 0; v < varCount; ++v)
        {
            const DWORD var = varOff + v * m_varStride;
            if (!(Rd32(var + 12) & 2 /* D3D_SVF_USED */) || Rd32(var + 8) != 64) continue;
            // Type: class (row/column-major matrix), float, 4 rows, 4 columns, not an array.
            const DWORD type = Rd32(var + 16), classType = Rd32(type), rowsCols = Rd32(type + 4);
            const DWORD cls = classType & 0xFFFF;
            if ((cls != 2 && cls != 3) || (classType >> 16) != 3 || rowsCols != 0x00040004 || (Rd32(type + 8) & 0xFFFF))
                continue;
            out.push_back(std::make_pair(slot, (UINT)Rd32(var + 4)));
        }
    }
    return out;
}

bool RdefView::Variable(const std::string& name, bool usedOnly, int& cbSlot, UINT& offset) const
{
    const std::vector<std::pair<int, UINT>> all = Variables(name, usedOnly);
    if (all.empty()) return false;
    cbSlot = all.back().first;
    offset = all.back().second;
    return true;
}

int RdefView::CBufferSlot(const std::string& name) const
{
    const DWORD resCount = Rd32(8), resOff = Rd32(12);
    if (!m_rd || name.empty() || resOff + resCount * 32u > m_size) return -1;
    for (DWORD i = 0; i < resCount; ++i)
        if (Rd32(resOff + i * 32u + 4) == 0 /* D3D_SIT_CBUFFER */ && name == Str(Rd32(resOff + i * 32u)))
            return (int)Rd32(resOff + i * 32u + 20);
    return -1;
}

std::vector<std::string> RdefView::Names() const
{
    std::vector<std::string> out;
    const DWORD cbCount = Rd32(0), cbOff = Rd32(4), resCount = Rd32(8), resOff = Rd32(12);
    if (!m_rd || resOff + resCount * 32u > m_size) return out;
    char line[256];
    for (DWORD i = 0; i < resCount; ++i)
    {
        const DWORD type = Rd32(resOff + i * 32u + 4);
        if (type != 2 /* D3D_SIT_TEXTURE */ && type != 0 /* D3D_SIT_CBUFFER */) continue;
        sprintf_s(line, "%s %s%u %s", type ? "texture" : "cbuffer", type ? "t" : "cb", Rd32(resOff + i * 32u + 20),
                  Str(Rd32(resOff + i * 32u)));
        out.push_back(line);
    }
    if (cbOff + cbCount * 24u > m_size) return out;
    for (DWORD c = 0; c < cbCount; ++c)
    {
        const DWORD cb = cbOff + c * 24u, varCount = Rd32(cb + 4), varOff = Rd32(cb + 8);
        if (varOff + varCount * m_varStride > m_size) continue;
        for (DWORD v = 0; v < varCount; ++v)
        {
            const DWORD var = varOff + v * m_varStride;
            sprintf_s(line, "variable %s+%u %s", Str(Rd32(cb)), Rd32(var + 4), Str(Rd32(var)));
            out.push_back(line);
        }
    }
    return out;
}

const std::string& EntryValue(const ProfileEntry& entry, const char* attr)
{
    static const std::string none;
    const ProfileEntry::const_iterator it = entry.find(attr);
    return it == entry.end() ? none : it->second;
}

std::vector<const ProfileEntry*> ChildEntries(const char* element)
{
    std::vector<const ProfileEntry*> out;
    for (int n = 1; ; ++n)
    {
        const auto it = g_ProfileData.FeatureEntries.find(std::string(element) + "/" + std::to_string(n));
        if (it == g_ProfileData.FeatureEntries.end()) break;
        out.push_back(&it->second);
    }
    return out;
}

bool ProfileSwitchOn(const char* element)
{
    const auto it = g_ProfileData.FeatureEntries.find(element);
    return it != g_ProfileData.FeatureEntries.end() && EntryValue(it->second, "Value") != "0";
}

ProfileFeatures CreateProfileFeatures()
{
    typedef std::unique_ptr<ProfileFeature> (*Factory)(const ProfileEntry*);
    // Profile element -> feature, called in this order at every draw point. needsCamera: it moves with RightCamera's camera.
    // RayCorners, DuniaLocalLights and CameraRelativePlanes serve camera-relative deferred lighting; no shipped profile uses them yet.
    static const struct { const char* element; Factory make; bool needsCamera; } kRegistry[] =
    {
        { "StereoTexture",        &MakeStereoTexture,      false },
        { "MirroredTarget",       &MakeMirroredTarget,     false },
        { "HudDepth",             &MakeHudDepth,           false },
        { "LightViewPasses",      &MakeLightViewPasses,    false },
        { "VertexCameraMatrices", &MakeVertexCameraMatrices, false },
        { "UiProjectionParallax", &MakeUiProjectionParallax, false },
        { "AdditionalMatrixName", &MakeAdditionalMatrices, false },
        { "AdditionalMatrixLayout", &MakeAdditionalMatrixLayout, false },
        { "RightCamera",          &MakeRightCamera,        false },
        { "RayCorners",           &MakeRayCorners,         true  },
        { "DuniaLocalLights",     &MakeDuniaLocalLights,   true  },
        { "CameraProjection",     &MakeCameraProjection,   true  },
        { "InverseCameraProjection", &MakeInverseCameraProjection, true },
        { "LeftEyeTextures",      &MakeLeftEyeTextures,    false },
        { "CameraRelativePlanes", &MakeCameraRelativePlanes, true },
        { "ScreenRayMatrix",      &MakeScreenRayMatrix,    false },
        { "ClipSpaceSprites",     &MakeClipSpaceSprites,   false },
        { "ClipSpaceRays",        &MakeClipSpaceRays,      false },
        { "RingKeepsShifts",      &MakeRingKeepsShifts,    false },
        { "KeepBareProjections",  &MakeKeepBareProjections, false },
        { "WholeBufferRanges",    &MakeWholeBufferRanges,  false },
        { "ViewMatrices",         &MakeViewMatrices,       false },
        { "DeferredContexts",     &MakeDeferredContexts,   false },
        { "PixelRayBasis",        &MakePixelRayBasis,      false },
        { "DeferredLightCamera",  &MakeDeferredLightCamera, false },
        { "RawResources",         &MakeRawResources,       false },
        { "ConstantBlocks",       &MakeConstantBlocks,     false },
        { "ComputeOutputsPerEye", &MakeComputeOutputsPerEye, false },
        { "IndirectArgsPerEye",   &MakeIndirectArgsPerEye, false },
        { "CameraInverses",       &MakeCameraInverses,     false },
        { "CodeMatrices",         &MakeCodeMatrices,       false },
        { "Factory2SwapChains",   &MakeFactory2SwapChains, false },
        { "HudSceneDepth",        &MakeHudSceneDepth,      false },
        { "StereoScissor",        &MakeStereoScissor,      false },
        { "BatchRightEye",        &MakeBatchRightEye,      false },
        { "PrivateBackBufferRefs", &MakePrivateBackBufferRefs, false },
        { "RecycleRightCopies",   &MakeRecycleRightCopies, false },
        { "DecalMatrices",        &MakeDecalMatrices,      false },
        { "FogVolumeLookups",     &MakeFogVolumeLookups,   false },
        { "TypelessBackBuffer",   &MakeTypelessBackBuffer, false },
        { "ViewportMatrices",     &MakeViewportMatrices,   false },
        { "CodeMatrixRows",       &MakeCodeMatrixRows,     false },
        { "TiledLightVolumes",    &MakeTiledLightVolumes,  false },
    };
    static std::once_flag s_checked;
    std::vector<std::string> problems;
    ProfileFeatures out;
    bool haveCamera = false;
    for (const auto& r : kRegistry)
    {
        const auto it = g_ProfileData.FeatureEntries.find(r.element);
        const ProfileEntry* entry = it == g_ProfileData.FeatureEntries.end() ? nullptr : &it->second;
        std::unique_ptr<ProfileFeature> f = r.make(entry);
        if (!f)
        {
            if (entry) problems.push_back(std::string("<") + r.element + "> builds nothing: a required attribute is missing, or it is off");
            continue;
        }
        if (!strcmp(r.element, "RightCamera")) haveCamera = true;
        if (r.needsCamera && !haveCamera) problems.push_back(std::string("<") + r.element + "> needs <RightCamera>; it edits nothing");
        out.push_back(std::move(f));
    }
    // An element no feature reads is a misspelling or belongs to another wrapper.
    for (const auto& e : g_ProfileData.FeatureEntries)
    {
        bool known = false;
        // A child element ("Parent/n") belongs to its parent's feature.
        const std::string parent = e.first.substr(0, e.first.find('/'));
        for (const auto& r : kRegistry) if (parent == r.element) known = true;
        if (!known) problems.push_back("<" + e.first + "> is no DX11 feature; ignored");
    }
    std::call_once(s_checked, [&] { for (const std::string& p : problems) FeatureLog("  profile: %s\n", p.c_str()); });
    return out;
}

} // namespace wiz3d
