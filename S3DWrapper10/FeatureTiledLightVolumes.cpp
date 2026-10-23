/*
* Project : wiz3D
* Copyright (C) Akshay Hegde 2026
*/

// <TiledLightVolumes Rays Point Spot Box/>: light volumes drawn against tile frustums built from pixel rays (ps cb0[Rays]),
// listed by vertex shader and kind, are moved with the rays into the right camera's view (BindLightVolumeShift).

#include "StdAfx.h"
#include <stdlib.h>
#include "ProfileFeature.h"
#include "Context11Proxy.h"
#include "ShaderAnalyzer11.h"
#include "AdapterFunctions.h"   // DDILog
#include <map>
#include <sstream>

namespace wiz3d
{
namespace
{

class TiledLightVolumesFeature : public ProfileFeature
{
public:
    explicit TiledLightVolumesFeature(const ProfileEntry& e)
        : m_rays(EntryValue(e, "Rays").empty() ? 41u : (UINT)atoi(EntryValue(e, "Rays").c_str()))
    {
        const char* kinds[3] = { "Point", "Spot", "Box" };
        for (int k = 0; k < 3; ++k)
        {
            std::istringstream in(EntryValue(e, kinds[k]));
            std::string t;
            while (in >> t) m_kinds[(DWORD)strtoul(t.c_str(), nullptr, 16)] = k + 1;
        }
    }

    bool Valid() const { return !m_kinds.empty(); }

    void OnRightDraw(Context11Proxy& c) override
    {
        const ShaderAnalysis11Result* vs = c.BoundShader(false);
        const auto it = vs ? m_kinds.find(vs->crc32) : m_kinds.end();
        m_bound = it != m_kinds.end() && c.BindLightVolumeShift(it->second, m_rays);
    }

    void OnRightDrawDone(Context11Proxy& c) override
    {
        if (m_bound) c.UnbindLightVolumeShift();
        m_bound = false;
    }

    void OnFrameEnd(Context11Proxy& c, unsigned frame, bool report) override
    {
        unsigned shifted = 0, refused = 0;
        c.LightVolumeCounts(shifted, refused);
        if (report)
            DDILog("  [frame %u] tiled light volumes: right draws in the right camera's view %u, refused %u\n", frame,
                   shifted, refused);
    }

private:
    UINT                 m_rays;
    std::map<DWORD, int> m_kinds;   // vertex shader CRC -> 1 point, 2 spot, 3 box
    bool                 m_bound = false;
};

} // namespace

std::unique_ptr<ProfileFeature> MakeTiledLightVolumes(const ProfileEntry* e)
{
    if (!e || EntryValue(*e, "Value") == "0") return nullptr;
    std::unique_ptr<TiledLightVolumesFeature> f(new TiledLightVolumesFeature(*e));
    if (!f->Valid()) return nullptr;
    return std::unique_ptr<ProfileFeature>(f.release());
}

} // namespace wiz3d
