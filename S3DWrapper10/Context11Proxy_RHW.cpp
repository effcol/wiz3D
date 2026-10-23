/*
* Project : wiz3D
* Copyright (C) Akshay Hegde 2026
*/

// Pre-transformed geometry, as DX9's BaseStereoRenderer_RHW.cpp: vertex shaders passing clip-space positions through
// (sprites, sky rays) get a right-eye geometry shader or shader twin that applies the eye offset.

#include "StdAfx.h"
#include "Context11Proxy-inl.h"

namespace wiz3d
{

// The z row of the camera the bound vertex shader's own constants hold (Frostbite's viewConstants carry the
// projection beside the view-projection), so a sprite pass takes its own camera's near plane. false = none bound.
bool Context11Proxy::SceneZRowFromBoundVS(float& m22, float& m23)
{
    const CBEyeSlots& s = m_cbSlots[ST_VS];
    const float scene = s_camP00 > 0.f ? s_camP00 : s_sceneP00;
    for (UINT slot = 0; slot < s.high && slot < kMaxCBs; ++slot)
    {
        Buffer11Proxy* b = s.proxy[slot];
        if (!b) continue;
        const bool ring = b->IsRing();
        if (!ring && b->Shadow().empty()) continue;
        const size_t base = s.num[slot] ? (size_t)s.first[slot] * 16 : 0;
        size_t size = ring ? (s.num[slot] ? (size_t)s.num[slot] * 16 : 0) : b->Shadow().size();
        if (ring && base + size > b->RingSize()) size = b->RingSize() > base ? b->RingSize() - base : 0;
        if (size > 64 * 16) size = 64 * 16;
        const unsigned char* bytes = ring ? b->RingData() + base : b->Shadow().data();
        for (size_t off = 0; off + 64 <= size; off += 16)
            for (int t = 0; t < 2; ++t)
            {
                const float* f = reinterpret_cast<const float*>(bytes + off);
                if (!CameraFamily(f, t == 1)) continue;
                float xn, wn;
                MatrixBasisNorms(f, t == 1, xn, wn);
                if (scene > 0.f && wn > 1e-6f && fabsf(xn / wn - scene) > 0.01f * scene) continue;
                if (ZRowOf(f, t == 1, m22, m23)) return true;
            }
    }
    return false;
}

// The right depth buffer for the clip-space geometry shader, copied once per frame at its first sprite draw into a
// texture of our own with a shader view; false = no usable depth (multisampled, not typeless).
bool Context11Proxy::ClipSpaceDepthViews(ClipSpaceDepth& out)
{
    ID3D11DepthStencilView* dsv = m_dsvRight;
    if (!dsv || !m_parent) return false;
    auto it = m_clipSpaceDepth.find(dsv);
    if (it == m_clipSpaceDepth.end())
    {
        ClipSpaceDepth d = {};
        dsv->AddRef();
        D3D11_DEPTH_STENCIL_VIEW_DESC dd = {};
        dsv->GetDesc(&dd);
        ID3D11Resource* res = nullptr;
        dsv->GetResource(&res);
        ID3D11Texture2D* tex = nullptr;
        if (res) res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex);
        D3D11_TEXTURE2D_DESC td = {};
        if (tex) tex->GetDesc(&td);
        DXGI_FORMAT sf = DXGI_FORMAT_UNKNOWN;
        switch (td.Format)
        {
        case DXGI_FORMAT_R24G8_TYPELESS:    sf = DXGI_FORMAT_R24_UNORM_X8_TYPELESS; break;
        case DXGI_FORMAT_R32_TYPELESS:      sf = DXGI_FORMAT_R32_FLOAT; break;
        case DXGI_FORMAT_R16_TYPELESS:      sf = DXGI_FORMAT_R16_UNORM; break;
        case DXGI_FORMAT_R32G8X24_TYPELESS: sf = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS; break;
        default: break;
        }
        HRESULT hr = E_FAIL;
        if (tex && sf != DXGI_FORMAT_UNKNOWN && td.SampleDesc.Count == 1 && dd.ViewDimension == D3D11_DSV_DIMENSION_TEXTURE2D)
        {
            D3D11_TEXTURE2D_DESC cd = td;
            cd.BindFlags = D3D11_BIND_SHADER_RESOURCE; cd.MiscFlags = 0; cd.MipLevels = 1; cd.ArraySize = 1;
            cd.Usage = D3D11_USAGE_DEFAULT; cd.CPUAccessFlags = 0;
            hr = m_parent->GetReal()->CreateTexture2D(&cd, nullptr, &d.tex);
            if (SUCCEEDED(hr))
            {
                D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
                sd.Format = sf; sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                sd.Texture2D.MostDetailedMip = 0; sd.Texture2D.MipLevels = 1;
                hr = m_parent->GetReal()->CreateShaderResourceView(d.tex, &sd, &d.srv);
                if (FAILED(hr)) { d.tex->Release(); d.tex = nullptr; }
            }
            d.w = td.Width; d.h = td.Height;
        }
        FeatureLog("  ClipSpaceSprites: right depth %ux%u fmt %u samples %u -> depth copy hr=0x%08lX\n", td.Width,
                   td.Height, (unsigned)td.Format, td.SampleDesc.Count, (unsigned long)hr);
        if (tex) tex->Release();
        if (res) res->Release();
        it = m_clipSpaceDepth.insert(std::make_pair(dsv, d)).first;
    }
    ClipSpaceDepth& d = it->second;
    if (!d.srv) return false;
    if (d.frame != s_frameId)
    {
        ID3D11Resource* res = nullptr;
        dsv->GetResource(&res);
        if (res) { m_real->CopySubresourceRegion(d.tex, 0, 0, 0, 0, res, 0, nullptr); res->Release(); }
        d.frame = s_frameId;
    }
    out = d;
    return true;
}

// ClipSpaceSprites: a geometry shader over a pass-through vertex shader's outputs that moves x by k.x + k.y * z, the
// eye offset at that depth (w = 1); a z of 0 (Frostbite's lens flares) takes the depth buffer under the sprite.
ID3D11GeometryShader* Context11Proxy::ClipSpaceShiftShader(const ShaderAnalysis11Result* info)
{
    auto it = m_clipSpaceGS.find(info->crc32);
    if (it != m_clipSpaceGS.end()) return it->second;
    ID3D11GeometryShader*& gs = m_clipSpaceGS[info->crc32];
    gs = nullptr;
    std::string src = "cbuffer wiz3D_clip : register(b0) { float4 wiz3D_k; float4 wiz3D_size; };\n"
                      "Texture2D<float> wiz3D_depth : register(t0);\nstruct V {\n";
    int pos = -1;
    for (size_t i = 0; i < info->outputs.size(); ++i)
    {
        const ShaderAnalysis11Result::OutputElement& e = info->outputs[i];
        if (e.sysValue == 1) pos = (int)i;
        else if (e.sysValue != 0)
        {
            FeatureLog("  ClipSpaceSprites: vs %08lX writes system value %u beside the position; no shift shader\n", info->crc32, e.sysValue);
            return nullptr;
        }
        unsigned n = 0;
        for (unsigned m = e.mask; m; m >>= 1) n += m & 1;
        if (!n) n = 4;
        char line[192];
        _snprintf_s(line, sizeof line, _TRUNCATE, "    %s%u m%u : %s%u;\n",
                    e.type == 1 ? "uint" : e.type == 2 ? "int" : "float", n, (unsigned)i, e.name.c_str(), e.index);
        src += line;
    }
    if (pos < 0 || !m_parent) return nullptr;
    char body[1024];
    _snprintf_s(body, sizeof body, _TRUNCATE,
                "};\n[maxvertexcount(3)]\nvoid main(triangle V i[3], inout TriangleStream<V> o)\n{\n"
                "    float2 c = (i[0].m%d.xy + i[1].m%d.xy + i[2].m%d.xy) * (1.0 / 3.0);\n"
                "    int2 px = clamp(int2((c.x * 0.5 + 0.5) * wiz3D_size.x, (0.5 - c.y * 0.5) * wiz3D_size.y), int2(0, 0), int2(wiz3D_size.xy) - 1);\n"
                "    float zd = wiz3D_size.x > 0 ? wiz3D_depth.Load(int3(px, 0)) : 0;\n"
                "    [unroll] for (int n = 0; n < 3; ++n)\n    {\n        V v = i[n];\n"
                "        float z = v.m%d.z != 0 ? v.m%d.z : zd;\n"
                "        v.m%d.x += clamp(wiz3D_k.x + wiz3D_k.y * z, wiz3D_k.w, wiz3D_k.z);\n"
                "        o.Append(v);\n    }\n}\n",
                pos, pos, pos, pos, pos, pos);
    src += body;
    const char* target = m_parent->GetReal()->GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_0 ? "gs_5_0" : "gs_4_0";
    ID3DBlob* code = nullptr; ID3DBlob* err = nullptr;
    HRESULT hr = D3DCompile(src.c_str(), src.size(), "wiz3D_clip", nullptr, nullptr, "main", target,
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
    if (SUCCEEDED(hr) && code)
        hr = m_parent->GetReal()->CreateGeometryShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &gs);
    FeatureLog("  ClipSpaceSprites: vs %08lX, %u outputs -> shift geometry shader (%s) hr=0x%08lX%s%s\n", info->crc32,
               (unsigned)info->outputs.size(), target, (unsigned long)hr, err ? ": " : "",
               err ? (const char*)err->GetBufferPointer() : "");
    if (FAILED(hr)) gs = nullptr;
    if (code) code->Release();
    if (err) err->Release();
    return gs;
}

// x_ndc += b + m41 / w_view with w_view = m23 / (z_ndc - m22): linear in z_ndc. Only a depth-tested triangle draw
// without a depth write, output UAVs or a geometry stage of the game's own (a sprite in the scene, not an overlay).
bool Context11Proxy::BindClipSpaceShift(int mode)
{
    if (m_clipSpaceBound) return false;   // ClipSpaceRays took this draw
    const ShaderAnalysis11Result* info = m_boundInfo[0];
    if (!info || !(info->clipSpacePosition || (mode == 3 && info->clipSpaceBuilt)) || !m_parent) return false;
    if (m_boundGS || m_boundHS || m_boundDS || !m_depthTestOn || m_depthWriteOn || m_omHasUAVs || OverlayPass() ||
        (m_topology != D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST && m_topology != D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP))
        return false;
    // Mode 3: a sprite the shader builds draws its right eye through the twin that measures its world position's depth.
    if (mode == 3 && info->clipSpaceBuilt && !info->clipSpacePosition) return BindSpriteDepth(info);
    const float p00 = s_camP00 > 0.f ? s_camP00 : s_sceneP00;
    float m22 = 0.f, m23 = 0.f;
    if (!SceneZRowFromBoundVS(m22, m23))
    {
        if (s_sceneZRowFrame && s_frameId - s_sceneZRowFrame <= 1) { m22 = s_sceneZRow[0]; m23 = s_sceneZRow[1]; }
    }
    if (!(p00 > 0.f) || m23 == 0.f)
    {
        ++m_clipSpaceNoCameraThisFrame;
        return false;
    }
    ID3D11GeometryShader* gs = ClipSpaceShiftShader(info);
    if (!gs) return false;
    if (!m_clipSpaceCB)
    {
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = 32; bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(m_parent->GetReal()->CreateBuffer(&bd, nullptr, &m_clipSpaceCB))) { m_clipSpaceCB = nullptr; return false; }
    }
    ClipSpaceDepth depth = {};
    // Mode 1 reads the depth under a sprite without a z of its own; 2 and 3 put a pass-through sprite at infinity.
    const bool haveDepth = mode == 1 && ClipSpaceDepthViews(depth);
    const float eyeShift = wiz3D_GetEffectiveEyeShift() * ProfileShaderMultiplier(m_parent, m_boundVS) * EyeShiftSign();
    const float b = eyeShift * p00, m41 = EyeOffsetM41(eyeShift) * p00;
    // x += k.x + k.y * z, clamped between the shift at 0.25 m and the one at infinity (b); without a depth to read,
    // a z of 0 is taken as infinity.
    float k[8] = { b - m41 * m22 / m23, m41 / m23, b, b + m41 / 0.25f, (float)depth.w, (float)depth.h, 0.f, 0.f };
    if (!haveDepth) { k[0] = b; k[1] = 0.f; k[4] = k[5] = 0.f; }
    if (k[3] > k[2]) k[3] = k[2];
    for (int i = 0; i < 4; ++i) m_clipSpaceK[i] = k[i];
    D3D11_MAPPED_SUBRESOURCE m = {};
    if (FAILED(m_real->Map(m_clipSpaceCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)) || !m.pData) return false;
    memcpy(m.pData, k, sizeof k);
    m_real->Unmap(m_clipSpaceCB, 0);
    if (haveDepth) m_real->GSSetShaderResources(0, 1, &depth.srv);
    m_real->GSSetShader(gs, nullptr, 0);
    m_real->GSSetConstantBuffers(0, 1, &m_clipSpaceCB);
    m_clipSpaceBound = true;
    ++m_clipSpaceDrawsThisFrame;
    return true;
}

// The row a sprite twin dots float4(world position, 1) with to get its depth: the scene camera's w row from the block the
// shader reads its camera position from, made world-space when that camera is camera-relative (w translation ~ 0).
bool Context11Proxy::SpriteDepthRow(const ShaderAnalysis11Result* info, float row[4])
{
    const CBEyeSlots& s = m_cbSlots[ST_VS];
    const UINT slot = (UINT)info->spriteCameraCB;
    if (info->spriteCameraCB < 0 || slot >= s.high || slot >= kMaxCBs || !s.proxy[slot]) return false;
    Buffer11Proxy* b = s.proxy[slot];
    const bool ring = b->IsRing();
    if (!ring && b->Shadow().empty()) return false;
    const size_t base = s.num[slot] ? (size_t)s.first[slot] * 16 : 0;
    const size_t total = ring ? b->RingSize() : b->Shadow().size();
    if (base >= total) return false;
    size_t size = s.num[slot] ? (size_t)s.num[slot] * 16 : total - base;
    if (base + size > total) size = total - base;
    const unsigned char* bytes = (ring ? b->RingData() : b->Shadow().data()) + base;
    if ((size_t)info->spriteCameraReg * 16 + 16 > size) return false;
    const float* c = reinterpret_cast<const float*>(bytes + (size_t)info->spriteCameraReg * 16);
    float m[16];
    bool found = false;
    for (size_t off = 0; off + 64 <= size && off < 64 * 16; off += 16)
        for (int t = 0; t < 2; ++t)
        {
            const float* f = reinterpret_cast<const float*>(bytes + off);
            if (!CameraFamily(f, t == 1)) continue;
            float cand[16];
            wiz3d::CanonicalizeMat4(f, t == 1, cand);
            const bool scene = s_mat.MatchForward(cand);   // the frame's camera wins over another camera-shaped block
            if (!found || scene) { memcpy(m, cand, sizeof m); found = true; }
            if (scene) { off = size; break; }
        }
    if (!found) return false;
    // Canonical row-vector: w = p.x m[3] + p.y m[7] + p.z m[11] + m[15].
    const float cf = c[0] * m[3] + c[1] * m[7] + c[2] * m[11];
    const bool relative = fabsf(m[15]) < 1e-3f * (1.f + fabsf(cf));
    row[0] = m[3]; row[1] = m[7]; row[2] = m[11]; row[3] = relative ? m[15] - cf : m[15];
    return true;
}

// ClipSpaceSprites 3: the right draw runs the sprite shader's twin, which adds b + m41 / w with w its world position's
// depth, the scene's own stereo terms. No near plane and no depth buffer: Andromeda's flare z is no depth.
bool Context11Proxy::BindSpriteDepth(const ShaderAnalysis11Result* info)
{
    if (info->spriteWorldInput < 0) return false;
    const float p00 = s_camP00 > 0.f ? s_camP00 : s_sceneP00;
    float row[4];
    if (!(p00 > 0.f) || !SpriteDepthRow(info, row)) { ++m_clipSpaceNoCameraThisFrame; return false; }
    const float eyeShift = wiz3D_GetEffectiveEyeShift() * ProfileShaderMultiplier(m_parent, m_boundVS) * EyeShiftSign();
    const float k[8] = { row[0], row[1], row[2], row[3], eyeShift * p00, EyeOffsetM41(eyeShift) * p00, 0.f, 0.f };
    if (!BindTwin(ST_VS, info, kTwinSpriteDepth, SpriteDepthPlan((unsigned)info->spriteWorldInput), k, 8)) return false;
    m_clipSpaceBound = true;
    ++m_clipSpaceDrawsThisFrame;
    ++m_spriteTwinDrawsThisFrame;
    return true;
}

// Right-eye shader twins (ShaderPatch11): built once per stage, plan tag and CRC from the code the shader kept, its
// cbuffer at the highest slot the shader leaves free, and bound with the game's shader and slot saved for UnbindTwin.
bool Context11Proxy::BindTwin(StageIdx st, const ShaderAnalysis11Result* info, int tag, const ShaderPatchPlan& plan,
                              const float* consts, UINT count)
{
    if (!info || !m_parent || m_twinStage >= 0 || count > 16 || (st != ST_VS && st != ST_PS && st != ST_CS)) return false;
    const unsigned long long key = ((unsigned long long)st << 40) | ((unsigned long long)(tag & 0xFF) << 32) | info->crc32;
    auto it = m_twins.find(key);
    if (it == m_twins.end())
    {
        ShaderTwin twin;
        std::vector<unsigned char> code;
        HRESULT hr = E_FAIL;
        ID3D11Device* dev = m_parent->GetReal();
        for (UINT slot = D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - 1; info->bytecode && slot >= 8 && !twin.shader; --slot)
            if (wiz3d::PatchShader(info->bytecode->data(), info->bytecode->size(), plan, slot, code))
            {
                twin.slot = slot;
                if (st == ST_VS)
                { ID3D11VertexShader* s = nullptr; hr = dev->CreateVertexShader(code.data(), code.size(), nullptr, &s); twin.shader = s; }
                else if (st == ST_PS)
                { ID3D11PixelShader* s = nullptr; hr = dev->CreatePixelShader(code.data(), code.size(), nullptr, &s); twin.shader = s; }
                else
                { ID3D11ComputeShader* s = nullptr; hr = dev->CreateComputeShader(code.data(), code.size(), nullptr, &s); twin.shader = s; }
                if (FAILED(hr)) twin.shader = nullptr;
            }
        FeatureLog("  shader twin: %s %08lX plan %d -> cb%u, %zu bytes hr=0x%08lX%s\n", st == ST_VS ? "vs" : st == ST_PS ? "ps" : "cs",
                   info->crc32, tag, twin.slot, code.size(), (unsigned long)hr, info->bytecode ? "" : " (no code kept)");
        it = m_twins.insert(std::make_pair(key, twin)).first;
    }
    if (!it->second.shader) return false;
    if (!m_twinCB)
    {
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = 64; bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(m_parent->GetReal()->CreateBuffer(&bd, nullptr, &m_twinCB))) { m_twinCB = nullptr; return false; }
    }
    D3D11_MAPPED_SUBRESOURCE map = {};
    if (FAILED(m_real->Map(m_twinCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &map)) || !map.pData) return false;
    memset(map.pData, 0, 64);
    memcpy(map.pData, consts, count * sizeof(float));
    m_real->Unmap(m_twinCB, 0);
    memcpy(m_twinConst, consts, count * sizeof(float));
    m_twinConstCount = count; m_twinCrc = info->crc32; m_twinTag = tag;
    // The game's shader and whatever it has at the twin's slot come back after the call (UnbindTwin).
    const UINT slot = it->second.slot;
    m_twinSavedCB = nullptr; m_twinSavedFirst = m_twinSavedNum = 0; m_twinSavedSlot = slot;
    if (m_rightRecording)
    {
        // BatchRightEye: the recorder reads nothing back; UnbindTwin rebinds from the tracked bindings instead.
        m_twinSavedShader = nullptr;
        if (st == ST_VS)      m_real->VSSetShader(static_cast<ID3D11VertexShader*>(it->second.shader), nullptr, 0);
        else if (st == ST_PS) m_real->PSSetShader(static_cast<ID3D11PixelShader*>(it->second.shader), nullptr, 0);
        else                  m_real->CSSetShader(static_cast<ID3D11ComputeShader*>(it->second.shader), nullptr, 0);
        if (st == ST_VS)      m_real->VSSetConstantBuffers(slot, 1, &m_twinCB);
        else if (st == ST_PS) m_real->PSSetConstantBuffers(slot, 1, &m_twinCB);
        else                  m_real->CSSetConstantBuffers(slot, 1, &m_twinCB);
        m_twinStage = st;
        return true;
    }
    if (st == ST_VS)
    {
        ID3D11VertexShader* s = nullptr; m_real->VSGetShader(&s, nullptr, nullptr); m_twinSavedShader = s;
        if (m_real1) m_real1->VSGetConstantBuffers1(slot, 1, &m_twinSavedCB, &m_twinSavedFirst, &m_twinSavedNum);
        else m_real->VSGetConstantBuffers(slot, 1, &m_twinSavedCB);
        m_real->VSSetShader(static_cast<ID3D11VertexShader*>(it->second.shader), nullptr, 0);
        m_real->VSSetConstantBuffers(slot, 1, &m_twinCB);
    }
    else if (st == ST_PS)
    {
        ID3D11PixelShader* s = nullptr; m_real->PSGetShader(&s, nullptr, nullptr); m_twinSavedShader = s;
        if (m_real1) m_real1->PSGetConstantBuffers1(slot, 1, &m_twinSavedCB, &m_twinSavedFirst, &m_twinSavedNum);
        else m_real->PSGetConstantBuffers(slot, 1, &m_twinSavedCB);
        m_real->PSSetShader(static_cast<ID3D11PixelShader*>(it->second.shader), nullptr, 0);
        m_real->PSSetConstantBuffers(slot, 1, &m_twinCB);
    }
    else
    {
        ID3D11ComputeShader* s = nullptr; m_real->CSGetShader(&s, nullptr, nullptr); m_twinSavedShader = s;
        if (m_real1) m_real1->CSGetConstantBuffers1(slot, 1, &m_twinSavedCB, &m_twinSavedFirst, &m_twinSavedNum);
        else m_real->CSGetConstantBuffers(slot, 1, &m_twinSavedCB);
        m_real->CSSetShader(static_cast<ID3D11ComputeShader*>(it->second.shader), nullptr, 0);
        m_real->CSSetConstantBuffers(slot, 1, &m_twinCB);
    }
    m_twinStage = st;
    return true;
}

// After the right call: the game's shader and its binding at the twin's slot (ranged when it was). false = none bound.
bool Context11Proxy::UnbindTwin()
{
    if (m_twinStage < 0) return false;
    const StageIdx st = (StageIdx)m_twinStage;
    m_twinStage = -1;
    if (m_rightRecording)
    {
        // The game's shader, and the stage's buffers as the right-eye draw bound them (its own when it has none per eye).
        if (st == ST_VS)      m_real->VSSetShader(m_boundVS, nullptr, 0);
        else if (st == ST_PS) m_real->PSSetShader(m_boundPS, nullptr, 0);
        BindStageCBs(st, m_cbSlots[st].anyStereo);
        return true;
    }
    const UINT slot = m_twinSavedSlot;
    const bool ranged = m_real1 && m_twinSavedCB && m_twinSavedNum && !(m_twinSavedFirst == 0 && m_twinSavedNum == 4096);
    if (st == ST_VS)
    {
        m_real->VSSetShader(static_cast<ID3D11VertexShader*>(m_twinSavedShader), nullptr, 0);
        if (ranged) m_real1->VSSetConstantBuffers1(slot, 1, &m_twinSavedCB, &m_twinSavedFirst, &m_twinSavedNum);
        else m_real->VSSetConstantBuffers(slot, 1, &m_twinSavedCB);
    }
    else if (st == ST_PS)
    {
        m_real->PSSetShader(static_cast<ID3D11PixelShader*>(m_twinSavedShader), nullptr, 0);
        if (ranged) m_real1->PSSetConstantBuffers1(slot, 1, &m_twinSavedCB, &m_twinSavedFirst, &m_twinSavedNum);
        else m_real->PSSetConstantBuffers(slot, 1, &m_twinSavedCB);
    }
    else
    {
        m_real->CSSetShader(static_cast<ID3D11ComputeShader*>(m_twinSavedShader), nullptr, 0);
        if (ranged) m_real1->CSSetConstantBuffers1(slot, 1, &m_twinSavedCB, &m_twinSavedFirst, &m_twinSavedNum);
        else m_real->CSSetConstantBuffers(slot, 1, &m_twinSavedCB);
    }
    if (m_twinSavedShader) { m_twinSavedShader->Release(); m_twinSavedShader = nullptr; }
    if (m_twinSavedCB) { m_twinSavedCB->Release(); m_twinSavedCB = nullptr; }
    return true;
}

// After the right draw: the game's geometry stage is empty again (its constant slots and the targets come back with
// BindEye).
void Context11Proxy::UnbindClipSpaceShift()
{
    if (!m_clipSpaceBound) return;
    m_clipSpaceBound = false;
    m_clipSpaceRays = false;
    if (UnbindTwin()) return;   // a sprite drawn by its twin
    m_real->GSSetShader(nullptr, nullptr, 0);
    ID3D11Buffer* none = nullptr;
    m_real->GSSetConstantBuffers(0, 1, &none);
    ID3D11ShaderResourceView* noView = nullptr;
    m_real->GSSetShaderResources(0, 1, &noView);
}

// ClipSpaceRays: the ray is linear over the triangle, ray(x, y) = A + Bx x + By y in NDC, so the right eye's pixel
// at x shows the mono ray of x - b: every vertex's ray loses b Bx, Bx solved from the three vertices.
ID3D11GeometryShader* Context11Proxy::ClipSpaceRayShader(const ShaderAnalysis11Result* info)
{
    auto it = m_clipSpaceRayGS.find(info->crc32);
    if (it != m_clipSpaceRayGS.end()) return it->second;
    ID3D11GeometryShader*& gs = m_clipSpaceRayGS[info->crc32];
    gs = nullptr;
    std::string src = "cbuffer wiz3D_clip : register(b0) { float4 wiz3D_k; float4 wiz3D_size; };\nstruct V {\n";
    int pos = -1;
    for (size_t i = 0; i < info->outputs.size(); ++i)
    {
        const ShaderAnalysis11Result::OutputElement& e = info->outputs[i];
        if (e.sysValue == 1) pos = (int)i;
        else if (e.sysValue != 0) return nullptr;
        unsigned n = 0;
        for (unsigned m = e.mask; m; m >>= 1) n += m & 1;
        if (!n) n = 4;
        char line[192];
        _snprintf_s(line, sizeof line, _TRUNCATE, "    %s%u m%u : %s%u;\n",
                    e.type == 1 ? "uint" : e.type == 2 ? "int" : "float", n, (unsigned)i, e.name.c_str(), e.index);
        src += line;
    }
    const int ray = info->rayOutput;
    if (pos < 0 || ray < 0 || !m_parent) return nullptr;
    char body[1024];
    _snprintf_s(body, sizeof body, _TRUNCATE,
                "};\n[maxvertexcount(3)]\nvoid main(triangle V i[3], inout TriangleStream<V> o)\n{\n"
                "    float2 d1 = i[1].m%d.xy / i[1].m%d.w - i[0].m%d.xy / i[0].m%d.w;\n"
                "    float2 d2 = i[2].m%d.xy / i[2].m%d.w - i[0].m%d.xy / i[0].m%d.w;\n"
                "    float det = d1.x * d2.y - d2.x * d1.y;\n"
                "    float3 bx = abs(det) > 1e-12 ? ((i[1].m%d - i[0].m%d) * d2.y - (i[2].m%d - i[0].m%d) * d1.y) / det : 0;\n"
                "    [unroll] for (int n = 0; n < 3; ++n)\n    {\n        V v = i[n];\n"
                "        v.m%d -= wiz3D_k.x * bx;\n"
                "        o.Append(v);\n    }\n}\n",
                pos, pos, pos, pos, pos, pos, pos, pos, ray, ray, ray, ray, ray);
    src += body;
    const char* target = m_parent->GetReal()->GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_0 ? "gs_5_0" : "gs_4_0";
    ID3DBlob* code = nullptr; ID3DBlob* err = nullptr;
    HRESULT hr = D3DCompile(src.c_str(), src.size(), "wiz3D_rays", nullptr, nullptr, "main", target,
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
    if (SUCCEEDED(hr) && code)
        hr = m_parent->GetReal()->CreateGeometryShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &gs);
    FeatureLog("  ClipSpaceRays: vs %08lX, %u outputs, ray in output %d -> ray geometry shader (%s) hr=0x%08lX%s%s\n", info->crc32,
               (unsigned)info->outputs.size(), ray, target, (unsigned long)hr, err ? ": " : "",
               err ? (const char*)err->GetBufferPointer() : "");
    if (FAILED(hr)) gs = nullptr;
    if (code) code->Release();
    if (err) err->Release();
    return gs;
}

// A right draw whose vertex shader passes NDC and a world ray through (Frostbite's sky quad: the panorama is looked
// up by the interpolated ray, no matrix): the ray moves by b, the eye offset at infinity; no depth test needed.
bool Context11Proxy::BindClipSpaceRayShift()
{
    if (m_clipSpaceBound) return false;   // ClipSpaceSprites took this draw
    const ShaderAnalysis11Result* info = m_boundInfo[0];
    if (!info || !info->clipSpacePosition || info->rayOutput < 0 || !m_parent) return false;
    if (m_boundGS || m_boundHS || m_boundDS || m_omHasUAVs || OverlayPass()) return false;
    if (m_topology != D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST && m_topology != D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP) return false;
    const float p00 = s_camP00 > 0.f ? s_camP00 : s_sceneP00;
    if (!(p00 > 0.f)) { ++m_clipSpaceNoCameraThisFrame; return false; }
    ID3D11GeometryShader* gs = ClipSpaceRayShader(info);
    if (!gs) return false;
    if (!m_clipSpaceCB)
    {
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = 32; bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(m_parent->GetReal()->CreateBuffer(&bd, nullptr, &m_clipSpaceCB))) { m_clipSpaceCB = nullptr; return false; }
    }
    const float eyeShift = wiz3D_GetEffectiveEyeShift() * ProfileShaderMultiplier(m_parent, m_boundVS) * EyeShiftSign();
    const float b = eyeShift * p00;
    float k[8] = { b, 0.f, b, b, 0.f, 0.f, 0.f, 0.f };
    for (int i = 0; i < 4; ++i) m_clipSpaceK[i] = k[i];
    D3D11_MAPPED_SUBRESOURCE m = {};
    if (FAILED(m_real->Map(m_clipSpaceCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)) || !m.pData) return false;
    memcpy(m.pData, k, sizeof k);
    m_real->Unmap(m_clipSpaceCB, 0);
    m_real->GSSetShader(gs, nullptr, 0);
    m_real->GSSetConstantBuffers(0, 1, &m_clipSpaceCB);
    m_clipSpaceBound = true;
    m_clipSpaceRays = true;
    ++m_clipSpaceRayDrawsThisFrame;
    return true;
}

// TiledLightVolumes: view-space positions move by k.x along x (point o1, spot o1 and o3); a box's plane offsets on the axes
// o1, o2 and their cross (o1.w, o2.w, o3.x) lose axis.x * k.x.
ID3D11GeometryShader* Context11Proxy::LightVolumeShader(const ShaderAnalysis11Result* info, int kind)
{
    auto it = m_lightVolumeGS.find(info->crc32);
    if (it != m_lightVolumeGS.end()) return it->second;
    ID3D11GeometryShader*& gs = m_lightVolumeGS[info->crc32];
    gs = nullptr;
    std::string src = "cbuffer wiz3D_clip : register(b0) { float4 wiz3D_k; float4 wiz3D_size; };\nstruct V {\n";
    int pos = -1, at[4] = { -1, -1, -1, -1 };   // float4 outputs by register
    for (size_t i = 0; i < info->outputs.size(); ++i)
    {
        const ShaderAnalysis11Result::OutputElement& e = info->outputs[i];
        if (e.sysValue == 1) pos = (int)i;
        else if (e.sysValue != 0) return nullptr;
        if (e.reg < 4 && e.type == 3 && (e.mask & 15) == 15) at[e.reg] = (int)i;
        unsigned n = 0;
        for (unsigned m = e.mask; m; m >>= 1) n += m & 1;
        if (!n) n = 4;
        char line[192];
        _snprintf_s(line, sizeof line, _TRUNCATE, "    %s%u m%u : %s%u;\n",
                    e.type == 1 ? "uint" : e.type == 2 ? "int" : "float", n, (unsigned)i, e.name.c_str(), e.index);
        src += line;
    }
    char edit[512] = "";
    if (kind == 1 && at[1] >= 0)
        _snprintf_s(edit, sizeof edit, _TRUNCATE, "        v.m%d.x += wiz3D_k.x;\n", at[1]);
    else if (kind == 2 && at[1] >= 0 && at[3] >= 0)
        _snprintf_s(edit, sizeof edit, _TRUNCATE, "        v.m%d.x += wiz3D_k.x;\n        v.m%d.x += wiz3D_k.x;\n", at[1], at[3]);
    else if (kind == 3 && at[1] >= 0 && at[2] >= 0 && at[3] >= 0)
        _snprintf_s(edit, sizeof edit, _TRUNCATE,
                    "        float3 n = cross(v.m%d.xyz, v.m%d.xyz);\n"
                    "        v.m%d.w -= v.m%d.x * wiz3D_k.x;\n        v.m%d.w -= v.m%d.x * wiz3D_k.x;\n"
                    "        v.m%d.x -= n.x * wiz3D_k.x;\n", at[1], at[2], at[1], at[1], at[2], at[2], at[3]);
    if (pos < 0 || !edit[0] || !m_parent)
    {
        FeatureLog("  TiledLightVolumes: vs %08lX kind %d: position %d, float4 outputs o1 %d o2 %d o3 %d; no volume shader\n",
                   info->crc32, kind, pos, at[1], at[2], at[3]);
        return nullptr;
    }
    src += "};\n[maxvertexcount(3)]\nvoid main(triangle V i[3], inout TriangleStream<V> o)\n{\n"
           "    [unroll] for (int k = 0; k < 3; ++k)\n    {\n        V v = i[k];\n";
    src += edit;
    src += "        o.Append(v);\n    }\n}\n";
    const char* target = m_parent->GetReal()->GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_0 ? "gs_5_0" : "gs_4_0";
    ID3DBlob* code = nullptr; ID3DBlob* err = nullptr;
    HRESULT hr = D3DCompile(src.c_str(), src.size(), "wiz3D_lights", nullptr, nullptr, "main", target,
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
    if (SUCCEEDED(hr) && code)
        hr = m_parent->GetReal()->CreateGeometryShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &gs);
    FeatureLog("  TiledLightVolumes: vs %08lX kind %d, %u outputs -> volume geometry shader (%s) hr=0x%08lX%s%s\n",
               info->crc32, kind, (unsigned)info->outputs.size(), target, (unsigned long)hr, err ? ": " : "",
               err ? (const char*)err->GetBufferPointer() : "");
    if (FAILED(hr)) gs = nullptr;
    if (code) code->Release();
    if (err) err->Release();
    return gs;
}

// A right draw of a light volume into the tiles: the right camera sits eyeShift * conv along the view's x and its rays
// are off-axis by eyeShift (x_right = x - eyeShift * conv, x/z = ndc / p00 - eyeShift), the sign the rays' own x scale.
bool Context11Proxy::BindLightVolumeShift(int kind, UINT rayReg)
{
    const ShaderAnalysis11Result* info = m_boundInfo[0];
    if (m_clipSpaceBound || !info || !info->parsed || !m_parent) { ++m_lightVolumeRefusedThisFrame; return false; }
    if (m_boundGS || m_boundHS || m_boundDS ||
        (m_topology != D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST && m_topology != D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP))
    { ++m_lightVolumeRefusedThisFrame; return false; }
    const CBEyeSlots& s = m_cbSlots[ST_PS];
    Buffer11Proxy* b = s.high ? s.proxy[0] : nullptr;
    const size_t base = s.num[0] ? (size_t)s.first[0] * 16 : 0;
    if (!b || b->IsRing() || b->Shadow().size() < base + ((size_t)rayReg + 1) * 16) { ++m_lightVolumeRefusedThisFrame; return false; }
    ID3D11GeometryShader* gs = LightVolumeShader(info, kind);
    if (!gs) { ++m_lightVolumeRefusedThisFrame; return false; }
    size_t size = ((b->Shadow().size() - base) + 15) & ~(size_t)15;
    if (size > 65536) size = 65536;
    if (!m_lightVolumePsCB || m_lightVolumePsCBSize != (UINT)size)
    {
        if (m_lightVolumePsCB) { m_lightVolumePsCB->Release(); m_lightVolumePsCB = nullptr; }
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = (UINT)size; bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(m_parent->GetReal()->CreateBuffer(&bd, nullptr, &m_lightVolumePsCB))) { m_lightVolumePsCB = nullptr; return false; }
        m_lightVolumePsCBSize = (UINT)size;
    }
    if (!m_clipSpaceCB)
    {
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = 32; bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(m_parent->GetReal()->CreateBuffer(&bd, nullptr, &m_clipSpaceCB))) { m_clipSpaceCB = nullptr; return false; }
    }
    const unsigned char* game = b->Shadow().data() + base;
    const float* ray = reinterpret_cast<const float*>(game + (size_t)rayReg * 16);
    const float sign = ray[0] < 0.f ? -1.f : 1.f;
    const float eyeShift = wiz3D_GetEffectiveEyeShift() * ProfileShaderMultiplier(m_parent, m_boundVS) * EyeShiftSign();
    const float conv = ResolveShaderConvergence(m_parent, m_boundVS);
    D3D11_MAPPED_SUBRESOURCE m = {};
    if (FAILED(m_real->Map(m_lightVolumePsCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)) || !m.pData) return false;
    const size_t copy = b->Shadow().size() - base < m_lightVolumePsCBSize ? b->Shadow().size() - base : m_lightVolumePsCBSize;
    memcpy(m.pData, game, copy);
    reinterpret_cast<float*>(static_cast<unsigned char*>(m.pData) + (size_t)rayReg * 16)[2] -= sign * eyeShift;
    m_real->Unmap(m_lightVolumePsCB, 0);
    const float k[8] = { -sign * eyeShift * conv, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f };
    if (FAILED(m_real->Map(m_clipSpaceCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)) || !m.pData) return false;
    memcpy(m.pData, k, sizeof k);
    m_real->Unmap(m_clipSpaceCB, 0);
    m_real->PSSetConstantBuffers(0, 1, &m_lightVolumePsCB);
    m_real->GSSetShader(gs, nullptr, 0);
    m_real->GSSetConstantBuffers(0, 1, &m_clipSpaceCB);
    m_lightVolumeBound = true;
    ++m_lightVolumeDrawsThisFrame;
    return true;
}

void Context11Proxy::UnbindLightVolumeShift()
{
    if (!m_lightVolumeBound) return;
    m_lightVolumeBound = false;
    m_real->GSSetShader(nullptr, nullptr, 0);
    ID3D11Buffer* none = nullptr;
    m_real->GSSetConstantBuffers(0, 1, &none);
    const CBEyeSlots& s = m_cbSlots[ST_PS];
    BindRightCB(ST_PS, 0, s.right[0] ? s.right[0] : s.left[0]);
}

} // namespace wiz3d
