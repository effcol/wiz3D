/* NvDirectMode - IDirect3DDevice9 proxy (passthrough)
 *
 * Stage 1b-i implementation. Every method forwards verbatim. The point of
 * this layer is to *exist* — once we know the wrapping doesn't break
 * anything, 1b-iii hooks GetBackBuffer / CreateRenderTarget for buffer
 * doubling and 1b-iv hooks SetRenderTarget for per-eye routing.
 */

#include "Device9Proxy.h"
#include "magic_header_capture.h"
#include "swapchain_helpers.h"
#include "eye_state.h"
#include "log.h"
#include "../anaglyph_matrices.h"

// SR-Lib — static library wrapping the Simulated Reality SDK.
// All SR import libs are merged into SR-mt[d].lib; the final DLL must
// delay-load the SR runtime DLLs (see SR.hpp header comment for the list).
#include "SR.hpp"

#include <ctype.h>
#include <stdlib.h>

#include <d3dcommon.h>
#include <string.h>

extern "C" int NvDM_OutputMode();
extern "C" int NvDM_SwapEyes();
extern "C" int NvDM_OutputIsTopBottom();
extern "C" int NvDM_AnaglyphColour();
extern "C" int NvDM_AnaglyphMethod();
extern "C" int NvDM_ForceSRWeave();

namespace NvDirectMode
{

// Primary-device registry for the eye-change callback dispatcher.
// Mirror of the d3d11/d3d10 SwapChainProxy primary-pointer pattern.
namespace
{
    Device9Proxy*    g_primaryDevice = nullptr;
    CRITICAL_SECTION g_primaryLock;
    bool             g_primaryLockInit = false;

    void EnsurePrimaryLock()
    {
        if (g_primaryLockInit) return;
        InitializeCriticalSection(&g_primaryLock);
        g_primaryLockInit = true;
    }

    void OnEyeChange(int oldEye, int /*newEye*/)
    {
        EnsurePrimaryLock();
        EnterCriticalSection(&g_primaryLock);
        // If the magic-header SBS capture path has fired this session, the
        // game isn't actually using SetActiveEye to drive per-eye rendering —
        // some other component (often NvAPI proxy itself) is calling
        // Stereo_SetActiveEye spuriously. Skip the shadow-capture here to
        // avoid clobbering the per-eye surfaces we just populated from the
        // SBS split.
        if (g_primaryDevice && !g_primaryDevice->IsMagicHeaderActive())
            g_primaryDevice->CaptureEye(oldEye);
        LeaveCriticalSection(&g_primaryLock);
    }
}

Device9Proxy::Device9Proxy(IDirect3DDevice9* real, bool isEx)
    : m_real(real)
    , m_realEx(isEx ? static_cast<IDirect3DDevice9Ex*>(real) : nullptr)
    , m_isEx(isEx)
    , m_refs(1)
    , m_logicalWidth(0)
    , m_logicalHeight(0)
    , m_pTrackedBackBuffer(nullptr)
    , m_shadowBB(nullptr)
    , m_leftEyeSurf(nullptr)
    , m_rightEyeSurf(nullptr)
    , m_leftEyeTex(nullptr)
    , m_rightEyeTex(nullptr)
    , m_compositeVS(nullptr)
    , m_compositePS_Line(nullptr)
    , m_compositePS_Col(nullptr)
    , m_compositePS_Checker(nullptr)
    , m_compositePS_Anaglyph(nullptr)
    , m_compositeVB(nullptr)
    , m_compositeDecl(nullptr)
    , m_shadersFailed(false)
    , m_srBlacklistedOrFailed(false)
    , m_srInterfaceDX9(nullptr)
    , m_srSBSTex(nullptr)
    , m_srSBSSurf(nullptr)
    , m_srSBSW(0)
    , m_srSBSH(0)
    , m_srSBSFmt(D3DFMT_UNKNOWN)
    , m_magicHeaderActive(false)
    , m_magicHeaderSwapEyes(false)
    , m_knownStereoSurfaceCount(0)
{
    for (size_t i = 0; i < kMaxKnownStereoSurfaces; ++i)
        m_knownStereoSurfaces[i] = nullptr;
    EnsurePrimaryLock();
    EnterCriticalSection(&g_primaryLock);
    g_primaryDevice = this;
    LeaveCriticalSection(&g_primaryLock);
    NvDirectMode::RegisterEyeChangeHandler(&OnEyeChange);
}

Device9Proxy::~Device9Proxy()
{
    EnsurePrimaryLock();
    EnterCriticalSection(&g_primaryLock);
    if (g_primaryDevice == this) g_primaryDevice = nullptr;
    LeaveCriticalSection(&g_primaryLock);

    ReleaseSRPipeline();
    ReleaseShaderPipeline();
    ReleaseShadow();
    ReleaseBackBufferReference();
}

void Device9Proxy::ReleaseShadow()
{
    if (m_shadowBB)     { m_shadowBB->Release();     m_shadowBB = nullptr; }
    if (m_leftEyeSurf)  { m_leftEyeSurf->Release();  m_leftEyeSurf = nullptr; }
    if (m_rightEyeSurf) { m_rightEyeSurf->Release(); m_rightEyeSurf = nullptr; }
    if (m_leftEyeTex)   { m_leftEyeTex->Release();   m_leftEyeTex = nullptr; }
    if (m_rightEyeTex)  { m_rightEyeTex->Release();  m_rightEyeTex = nullptr; }
}

void Device9Proxy::ReleaseShaderPipeline()
{
    if (m_compositeDecl)        { m_compositeDecl->Release();        m_compositeDecl = nullptr; }
    if (m_compositeVB)          { m_compositeVB->Release();          m_compositeVB = nullptr; }
    if (m_compositePS_Anaglyph) { m_compositePS_Anaglyph->Release(); m_compositePS_Anaglyph = nullptr; }
    if (m_compositePS_Checker)  { m_compositePS_Checker->Release();  m_compositePS_Checker = nullptr; }
    if (m_compositePS_Col)      { m_compositePS_Col->Release();      m_compositePS_Col = nullptr; }
    if (m_compositePS_Line)     { m_compositePS_Line->Release();     m_compositePS_Line = nullptr; }
    if (m_compositeVS)          { m_compositeVS->Release();          m_compositeVS = nullptr; }
}

void Device9Proxy::EnsureShadow()
{
    if (m_shadowBB || !m_real || !m_pTrackedBackBuffer) return;

    D3DSURFACE_DESC desc = {};
    if (FAILED(m_pTrackedBackBuffer->GetDesc(&desc))) return;
    m_logicalWidth = desc.Width;
    m_logicalHeight = desc.Height;

    // Shadow has same format/MS as the real BB so StretchRect can copy
    // between them without conversion.
    HRESULT hr = m_real->CreateRenderTarget(desc.Width, desc.Height, desc.Format,
                                             desc.MultiSampleType, desc.MultiSampleQuality,
                                             FALSE, &m_shadowBB, NULL);
    if (FAILED(hr) || !m_shadowBB)
    {
        LOG_VERBOSE("  d3d9 EnsureShadow: CreateRenderTarget(%ux%u) FAILED hr=0x%08lX\n",
                    desc.Width, desc.Height, hr);
        m_shadowBB = nullptr;
        return;
    }
    LOG_VERBOSE("  d3d9 EnsureShadow: shadow=%p (%ux%u, fmt=%d) for realBB=%p\n",
                m_shadowBB, desc.Width, desc.Height, (int)desc.Format,
                (void*)m_pTrackedBackBuffer);
}

void Device9Proxy::CaptureEye(int eyeBeingLeft)
{
    if (!m_shadowBB || !m_real) return;
    IDirect3DSurface9** slot = nullptr;
    if      (eyeBeingLeft == NvDirectMode::kEyeLeft)  slot = &m_leftEyeSurf;
    else if (eyeBeingLeft == NvDirectMode::kEyeRight) slot = &m_rightEyeSurf;
    else return;

    if (!*slot)
    {
        D3DSURFACE_DESC desc = {};
        m_shadowBB->GetDesc(&desc);
        HRESULT hr = m_real->CreateRenderTarget(desc.Width, desc.Height, desc.Format,
                                                 desc.MultiSampleType, desc.MultiSampleQuality,
                                                 FALSE, slot, NULL);
        if (FAILED(hr) || !*slot) return;
        LOG_VERBOSE("  d3d9 CaptureEye(%d): allocated eye surface=%p\n", eyeBeingLeft, *slot);
    }

    m_real->StretchRect(m_shadowBB, NULL, *slot, NULL, D3DTEXF_NONE);
    NVDM_TRACE_FIRST_N(8, "  d3d9 CaptureEye(eye=%d): StretchRect shadow=%p -> eyeSurf=%p\n",
                       eyeBeingLeft, m_shadowBB, *slot);
}

// ---------------------------------------------------------------------------
// OutputMode 4-7 shader pipeline (Line/Col Interleaved, Checkerboard, Anaglyph)
//
// DX9 doesn't naturally have a shader composite path on this proxy — the
// existing modes 0-3 use StretchRect surface→surface, no shaders involved.
// For modes 4-7 we lazily compile vs_3_0 + ps_3_0 HLSL via D3DCompile from
// d3dcompiler_47.dll, and run a fullscreen-triangle draw to the real BB
// sampling each eye texture per-pixel.
//
// Eye storage is the same surfaces as before (m_leftEyeSurf etc.) — we
// allocate matching textures (m_leftEyeTex / m_rightEyeTex) on demand and
// StretchRect surface→texture-level0 right before the shader pass. Costs
// one extra GPU copy per eye per frame in shader modes; trivial at the
// resolutions DX9 games run at.
//
// Modes 0-3 fall through to the original StretchRect path below — no
// shader compile required, no risk of breaking working titles.
// ---------------------------------------------------------------------------
namespace
{
    typedef HRESULT (WINAPI *PFN_D3DCompile)(
        LPCVOID pSrcData, SIZE_T SrcDataSize, LPCSTR pSourceName,
        const D3D_SHADER_MACRO* pDefines, ID3DInclude* pInclude,
        LPCSTR pEntrypoint, LPCSTR pTarget, UINT Flags1, UINT Flags2,
        ID3DBlob** ppCode, ID3DBlob** ppErrorMsgs);

    PFN_D3DCompile g_pfnD3DCompile_dx9 = nullptr;
    volatile LONG  g_compileResolved   = 0;

    bool EnsureD3DCompile()
    {
        if (g_pfnD3DCompile_dx9) return true;
        if (InterlockedCompareExchange(&g_compileResolved, 1, 0) != 0)
            return g_pfnD3DCompile_dx9 != nullptr;
        HMODULE h = LoadLibraryW(L"d3dcompiler_47.dll");
        if (!h) h = LoadLibraryW(L"d3dcompiler_46.dll");
        if (!h) h = LoadLibraryW(L"d3dcompiler_43.dll");
        if (!h) return false;
        g_pfnD3DCompile_dx9 = (PFN_D3DCompile)GetProcAddress(h, "D3DCompile");
        return g_pfnD3DCompile_dx9 != nullptr;
    }

    // Vertex shader: writes a fullscreen triangle straight into clip space.
    // Half-pixel offset (-0.5/W, +0.5/H in clip space ≈ +0.5 pixel) keeps
    // pixel sample centres aligned with texel centres on DX9's "pixel
    // origin top-left, sample at top-left" convention.
    const char kCompositeVS_DX9[] =
        "struct VSI { float2 pos : POSITION; float2 uv : TEXCOORD0; };\n"
        "struct VSO { float4 pos : POSITION; float2 uv : TEXCOORD0; };\n"
        "VSO main(VSI i) { VSO o; o.pos = float4(i.pos, 0, 1); o.uv = i.uv; return o; }\n";

    // PS_3_0 has VPOS register giving screen pixel coords — perfect for
    // the interleaved/checker patterns. uv comes from VS for texture sampling.
    const char kCompositePS_Line_DX9[] =
        "sampler leftS  : register(s0);\n"
        "sampler rightS : register(s1);\n"
        "float4 main(float4 vPos : VPOS, float2 uv : TEXCOORD0) : COLOR {\n"
        "  float4 L = tex2D(leftS,  uv);\n"
        "  float4 R = tex2D(rightS, uv);\n"
        "  float ev = fmod(floor(vPos.y), 2);\n"
        "  return ev < 1 ? L : R;\n"
        "}\n";

    const char kCompositePS_Col_DX9[] =
        "sampler leftS  : register(s0);\n"
        "sampler rightS : register(s1);\n"
        "float4 main(float4 vPos : VPOS, float2 uv : TEXCOORD0) : COLOR {\n"
        "  float4 L = tex2D(leftS,  uv);\n"
        "  float4 R = tex2D(rightS, uv);\n"
        "  float ev = fmod(floor(vPos.x), 2);\n"
        "  return ev < 1 ? L : R;\n"
        "}\n";

    const char kCompositePS_Checker_DX9[] =
        "sampler leftS  : register(s0);\n"
        "sampler rightS : register(s1);\n"
        "float4 main(float4 vPos : VPOS, float2 uv : TEXCOORD0) : COLOR {\n"
        "  float4 L = tex2D(leftS,  uv);\n"
        "  float4 R = tex2D(rightS, uv);\n"
        "  float ev = fmod(floor(vPos.x) + floor(vPos.y), 2);\n"
        "  return ev < 1 ? L : R;\n"
        "}\n";

    // Anaglyph: 6 PS constants (c0..c5) carry the colour-method matrix rows
    // (xyz used; w padded). SetPixelShaderConstantF uploads 6 float4s from
    // UpdateAnaglyphConsts() at composite time.
    const char kCompositePS_Anaglyph_DX9[] =
        "sampler leftS  : register(s0);\n"
        "sampler rightS : register(s1);\n"
        "float4 lR : register(c0);\n"
        "float4 lG : register(c1);\n"
        "float4 lB : register(c2);\n"
        "float4 rR : register(c3);\n"
        "float4 rG : register(c4);\n"
        "float4 rB : register(c5);\n"
        "float4 main(float4 vPos : VPOS, float2 uv : TEXCOORD0) : COLOR {\n"
        "  float3 L = tex2D(leftS,  uv).rgb;\n"
        "  float3 R = tex2D(rightS, uv).rgb;\n"
        "  float3 a;\n"
        "  a.r = dot(lR.xyz, L) + dot(rR.xyz, R);\n"
        "  a.g = dot(lG.xyz, L) + dot(rG.xyz, R);\n"
        "  a.b = dot(lB.xyz, L) + dot(rB.xyz, R);\n"
        "  return float4(saturate(a), 1);\n"
        "}\n";

    bool CompilePS(const char* src, size_t len, const char* tag,
                   IDirect3DDevice9* dev, IDirect3DPixelShader9** out)
    {
        if (!g_pfnD3DCompile_dx9 || !dev) return false;
        ID3DBlob* code = nullptr;
        ID3DBlob* err  = nullptr;
        HRESULT hr = g_pfnD3DCompile_dx9(src, len, tag, nullptr, nullptr,
                                         "main", "ps_3_0", 0, 0, &code, &err);
        if (err) { err->Release(); err = nullptr; }
        if (FAILED(hr) || !code) { LOG_VERBOSE("  d3d9 CompilePS(%s) FAILED hr=0x%08lX\n", tag, hr); return false; }
        hr = dev->CreatePixelShader(static_cast<const DWORD*>(code->GetBufferPointer()), out);
        code->Release();
        return SUCCEEDED(hr) && *out;
    }
} // anonymous namespace

bool Device9Proxy::EnsureShaderPipeline()
{
    if (m_shadersFailed) return false;
    if (m_compositeVS && m_compositePS_Line && m_compositePS_Col &&
        m_compositePS_Checker && m_compositePS_Anaglyph &&
        m_compositeVB && m_compositeDecl) return true;
    if (!m_real || !EnsureD3DCompile()) { m_shadersFailed = true; return false; }

    // Vertex declaration: POSITION (float2) + TEXCOORD0 (float2). 16 bytes.
    if (!m_compositeDecl)
    {
        D3DVERTEXELEMENT9 elems[] = {
            { 0, 0, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION,  0 },
            { 0, 8, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD,  0 },
            D3DDECL_END()
        };
        if (FAILED(m_real->CreateVertexDeclaration(elems, &m_compositeDecl)))
        { m_shadersFailed = true; return false; }
    }

    if (!m_compositeVB)
    {
        // Fullscreen triangle (covers [-1,1]^2 with one extra triangle clipped).
        // 3 verts × 16 bytes each.
        struct V { float x, y, u, v; };
        const V verts[3] = {
            { -1.0f,  3.0f, 0.0f, -1.0f },   // top-left of an oversized triangle
            { -1.0f, -1.0f, 0.0f,  1.0f },   // bottom-left
            {  3.0f, -1.0f, 2.0f,  1.0f },   // bottom-right
        };
        if (FAILED(m_real->CreateVertexBuffer(sizeof(verts), 0, 0, D3DPOOL_DEFAULT,
                                              &m_compositeVB, nullptr)))
        { m_shadersFailed = true; return false; }
        void* p = nullptr;
        if (FAILED(m_compositeVB->Lock(0, sizeof(verts), &p, 0)) || !p)
        { m_shadersFailed = true; return false; }
        memcpy(p, verts, sizeof(verts));
        m_compositeVB->Unlock();
    }

    if (!m_compositeVS)
    {
        ID3DBlob* code = nullptr;
        ID3DBlob* err  = nullptr;
        HRESULT hr = g_pfnD3DCompile_dx9(kCompositeVS_DX9, sizeof(kCompositeVS_DX9) - 1,
                                          "vs", nullptr, nullptr,
                                          "main", "vs_3_0", 0, 0, &code, &err);
        if (err) err->Release();
        if (FAILED(hr) || !code) { m_shadersFailed = true; return false; }
        hr = m_real->CreateVertexShader(static_cast<const DWORD*>(code->GetBufferPointer()),
                                         &m_compositeVS);
        code->Release();
        if (FAILED(hr)) { m_shadersFailed = true; return false; }
    }

    bool ok = true;
    if (!m_compositePS_Line)
        ok = ok && CompilePS(kCompositePS_Line_DX9,    sizeof(kCompositePS_Line_DX9)    - 1, "ps_line",     m_real, &m_compositePS_Line);
    if (!m_compositePS_Col)
        ok = ok && CompilePS(kCompositePS_Col_DX9,     sizeof(kCompositePS_Col_DX9)     - 1, "ps_col",      m_real, &m_compositePS_Col);
    if (!m_compositePS_Checker)
        ok = ok && CompilePS(kCompositePS_Checker_DX9, sizeof(kCompositePS_Checker_DX9) - 1, "ps_checker",  m_real, &m_compositePS_Checker);
    if (!m_compositePS_Anaglyph)
        ok = ok && CompilePS(kCompositePS_Anaglyph_DX9,sizeof(kCompositePS_Anaglyph_DX9)- 1, "ps_anaglyph", m_real, &m_compositePS_Anaglyph);
    if (!ok) { m_shadersFailed = true; return false; }

    LOG_VERBOSE("  d3d9 EnsureShaderPipeline: OK\n");
    return true;
}

void Device9Proxy::UpdateAnaglyphConsts()
{
    if (!m_real) return;
    int colour = NvDM_AnaglyphColour();
    int method = NvDM_AnaglyphMethod();
    if (colour < 0 || colour > 2) colour = 0;
    if (method < 0 || method > 6) method = 0;
    const NvDirectMode::AnaglyphMatrix& m = NvDirectMode::kAnaglyphMatrices[colour][method];
    const float* rows[6] = { m.lR, m.lG, m.lB, m.rR, m.rG, m.rB };
    float buf[6 * 4];
    for (int i = 0; i < 6; ++i)
    {
        buf[i * 4 + 0] = rows[i][0];
        buf[i * 4 + 1] = rows[i][1];
        buf[i * 4 + 2] = rows[i][2];
        buf[i * 4 + 3] = 0.0f;
    }
    m_real->SetPixelShaderConstantF(0, buf, 6);
}

bool Device9Proxy::RunShaderComposite(int mode)
{
    if (!EnsureShaderPipeline()) return false;
    if (!m_pTrackedBackBuffer || !m_leftEyeSurf || !m_rightEyeSurf) return false;

    // Allocate the eye textures lazily (matching format/size to shadow).
    D3DSURFACE_DESC bbDesc = {};
    if (FAILED(m_pTrackedBackBuffer->GetDesc(&bbDesc))) return false;
    D3DSURFACE_DESC eyeDesc = {};
    m_leftEyeSurf->GetDesc(&eyeDesc);
    if (!m_leftEyeTex)
    {
        if (FAILED(m_real->CreateTexture(eyeDesc.Width, eyeDesc.Height, 1,
                                          D3DUSAGE_RENDERTARGET, eyeDesc.Format,
                                          D3DPOOL_DEFAULT, &m_leftEyeTex, nullptr)))
            return false;
    }
    if (!m_rightEyeTex)
    {
        if (FAILED(m_real->CreateTexture(eyeDesc.Width, eyeDesc.Height, 1,
                                          D3DUSAGE_RENDERTARGET, eyeDesc.Format,
                                          D3DPOOL_DEFAULT, &m_rightEyeTex, nullptr)))
            return false;
    }

    // Stretch each eye SURFACE -> matching TEXTURE level 0 so the shader can
    // sample. Cheap GPU copy; saves us re-architecting eye storage.
    bool swap = NvDM_SwapEyes() != 0;
    IDirect3DSurface9* srcLeft  = swap ? m_rightEyeSurf : m_leftEyeSurf;
    IDirect3DSurface9* srcRight = swap ? m_leftEyeSurf  : m_rightEyeSurf;
    IDirect3DSurface9* dstLeft  = nullptr;
    IDirect3DSurface9* dstRight = nullptr;
    m_leftEyeTex->GetSurfaceLevel(0, &dstLeft);
    m_rightEyeTex->GetSurfaceLevel(0, &dstRight);
    if (!dstLeft || !dstRight)
    { if (dstLeft) dstLeft->Release(); if (dstRight) dstRight->Release(); return false; }
    m_real->StretchRect(srcLeft,  nullptr, dstLeft,  nullptr, D3DTEXF_NONE);
    m_real->StretchRect(srcRight, nullptr, dstRight, nullptr, D3DTEXF_NONE);
    dstLeft->Release();
    dstRight->Release();

    // Save-and-restore the game's render state. Game's next frame rebinds
    // most of its pipeline anyway, but a state block protects against any
    // sticky state surviving (samplers, vertex decl, render targets).
    IDirect3DStateBlock9* sb = nullptr;
    if (FAILED(m_real->CreateStateBlock(D3DSBT_ALL, &sb)) || !sb) return false;

    IDirect3DPixelShader9* ps = m_compositePS_Line;
    bool needsAnaglyph = false;
    switch (mode)
    {
        case 4: ps = m_compositePS_Line;     break;
        case 5: ps = m_compositePS_Col;      break;
        case 6: ps = m_compositePS_Checker;  break;
        case 7: ps = m_compositePS_Anaglyph; needsAnaglyph = true; break;
        default: sb->Release(); return false;
    }

    m_real->SetRenderTarget(0, m_pTrackedBackBuffer);
    m_real->SetDepthStencilSurface(nullptr);

    // Set viewport to full real BB.
    D3DVIEWPORT9 vp = { 0, 0, bbDesc.Width, bbDesc.Height, 0.0f, 1.0f };
    m_real->SetViewport(&vp);

    m_real->SetVertexDeclaration(m_compositeDecl);
    m_real->SetStreamSource(0, m_compositeVB, 0, sizeof(float) * 4);
    m_real->SetVertexShader(m_compositeVS);
    m_real->SetPixelShader(ps);
    m_real->SetTexture(0, m_leftEyeTex);
    m_real->SetTexture(1, m_rightEyeTex);
    // Point sampling for interleaved/checker (avoids row blending);
    // linear is fine for anaglyph.
    DWORD samplerFilter = (mode == 7) ? D3DTEXF_LINEAR : D3DTEXF_POINT;
    m_real->SetSamplerState(0, D3DSAMP_MINFILTER, samplerFilter);
    m_real->SetSamplerState(0, D3DSAMP_MAGFILTER, samplerFilter);
    m_real->SetSamplerState(1, D3DSAMP_MINFILTER, samplerFilter);
    m_real->SetSamplerState(1, D3DSAMP_MAGFILTER, samplerFilter);
    m_real->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    m_real->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    m_real->SetSamplerState(1, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    m_real->SetSamplerState(1, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    m_real->SetRenderState(D3DRS_ZENABLE,        FALSE);
    m_real->SetRenderState(D3DRS_ZWRITEENABLE,   FALSE);
    m_real->SetRenderState(D3DRS_CULLMODE,       D3DCULL_NONE);
    m_real->SetRenderState(D3DRS_LIGHTING,       FALSE);
    m_real->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    m_real->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);

    if (needsAnaglyph) UpdateAnaglyphConsts();

    m_real->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 1);

    sb->Apply();
    sb->Release();
    NVDM_TRACE_FIRST_N(2, "  d3d9 RunShaderComposite: mode=%d -> realBB=%p\n",
                       mode, (void*)m_pTrackedBackBuffer);
    return true;
}

// Mirror of the d3d11 blacklist. Keep the lists aligned manually.
static bool IsSRIncompatibleExe()
{
    wchar_t exePath[MAX_PATH] = {};
    if (!GetModuleFileNameW(nullptr, exePath, MAX_PATH)) return false;
    for (wchar_t* p = exePath; *p; ++p) *p = (wchar_t)towlower(*p);
    // Kept aligned with d3d11's blacklist. See d3d11 SwapChainProxy.cpp.
    static const wchar_t* const kBlacklist[] = {
        L"tombraider.exe",
    };
    for (auto entry : kBlacklist)
        if (wcsstr(exePath, entry)) return true;
    return false;
}

void Device9Proxy::ReleaseSRPipeline()
{
    if (m_srSBSSurf) { m_srSBSSurf->Release(); m_srSBSSurf = nullptr; }
    if (m_srSBSTex)  { m_srSBSTex->Release();  m_srSBSTex  = nullptr; }
    m_srSBSW = 0; m_srSBSH = 0; m_srSBSFmt = D3DFMT_UNKNOWN;

    if (m_srInterfaceDX9)
    {
        m_srInterfaceDX9->Delete();
        m_srInterfaceDX9 = nullptr;
    }
}

bool Device9Proxy::EnsureSRSBSTexture()
{
    if (!m_real) return false;
    if (m_logicalWidth == 0 || m_logicalHeight == 0) return false;

    // BB format — query from tracked BB or fall back to A8R8G8B8.
    D3DFORMAT wantFmt = D3DFMT_A8R8G8B8;
    if (m_pTrackedBackBuffer)
    {
        D3DSURFACE_DESC desc = {};
        if (SUCCEEDED(m_pTrackedBackBuffer->GetDesc(&desc)))
            wantFmt = desc.Format;
    }

    UINT wantW = m_logicalWidth * 2;
    UINT wantH = m_logicalHeight;
    if (m_srSBSTex && m_srSBSW == wantW && m_srSBSH == wantH && m_srSBSFmt == wantFmt)
        return true;

    if (m_srSBSSurf) { m_srSBSSurf->Release(); m_srSBSSurf = nullptr; }
    if (m_srSBSTex)  { m_srSBSTex->Release();  m_srSBSTex  = nullptr; }

    // Render-target texture, DEFAULT pool. StretchRect from m_leftEyeSurf /
    // m_rightEyeSurf into its surface level 0 every frame populates the SBS.
    HRESULT hr = m_real->CreateTexture(wantW, wantH, 1, D3DUSAGE_RENDERTARGET,
                                       wantFmt, D3DPOOL_DEFAULT,
                                       &m_srSBSTex, nullptr);
    if (FAILED(hr) || !m_srSBSTex)
    {
        LOG_VERBOSE("  d3d9 EnsureSRSBSTexture: CreateTexture(%ux%u fmt=%d) FAILED hr=0x%08lX\n",
                    wantW, wantH, (int)wantFmt, hr);
        m_srSBSTex = nullptr;
        return false;
    }
    if (FAILED(m_srSBSTex->GetSurfaceLevel(0, &m_srSBSSurf)) || !m_srSBSSurf)
    {
        m_srSBSTex->Release(); m_srSBSTex = nullptr;
        return false;
    }

    m_srSBSW = wantW; m_srSBSH = wantH; m_srSBSFmt = wantFmt;

    // Bind SBS texture to the SR interface once per texture creation (not per frame).
    if (m_srInterfaceDX9)
        m_srInterfaceDX9->SetInputTexture(m_srSBSTex, false);

    return true;
}

bool Device9Proxy::EnsureSRWeaver()
{
    if (m_srBlacklistedOrFailed) return false;
    if (m_srInterfaceDX9) return true;
    if (!m_real) return false;

    static bool s_blacklistChecked = false;
    static bool s_isBlacklisted    = false;
    if (!s_blacklistChecked)
    {
        s_blacklistChecked = true;
        s_isBlacklisted    = IsSRIncompatibleExe();
        if (s_isBlacklisted)
        {
            if (NvDM_ForceSRWeave())
            {
                LOG_VERBOSE("  d3d9 EnsureSRWeaver: exe is SR-blacklisted but ForceSRWeave=1 — attempting anyway (diagnostic)\n");
                s_isBlacklisted = false;
            }
            else
            {
                LOG_VERBOSE("  d3d9 EnsureSRWeaver: exe is SR-blacklisted; falling back to SBS (set ForceSRWeave=1 to override)\n");
            }
        }
    }
    if (s_isBlacklisted) { m_srBlacklistedOrFailed = true; return false; }

    // Output-window HWND comes from the swap-chain's presentation parameters.
    // GetCreationParameters gives us the device focus window, which matches
    // the present window in 99%+ of cases.
    D3DDEVICE_CREATION_PARAMETERS cp = {};
    HWND hWnd = nullptr;
    if (SUCCEEDED(m_real->GetCreationParameters(&cp)))
        hWnd = cp.hFocusWindow;
    if (!hWnd)
    {
        IDirect3DSwapChain9* sc = nullptr;
        if (SUCCEEDED(m_real->GetSwapChain(0, &sc)) && sc)
        {
            D3DPRESENT_PARAMETERS pp = {};
            sc->GetPresentParameters(&pp);
            hWnd = pp.hDeviceWindow;
            sc->Release();
        }
    }

    // SR-Lib: single-call init — creates SRContext + weaver internally.
    HRESULT hr = SimulatedReality::CreateSRInterfaceDX9(m_real, hWnd, &m_srInterfaceDX9);
    if (FAILED(hr) || !m_srInterfaceDX9)
    {
        LOG_VERBOSE("  d3d9 EnsureSRWeaver: CreateSRInterfaceDX9 failed (hr=0x%08lX hWnd=%p dev=%p)\n",
                    hr, (void*)hWnd, (void*)m_real);
        m_srBlacklistedOrFailed = true;
        return false;
    }

    LOG_VERBOSE("  d3d9 EnsureSRWeaver: ready (hWnd=%p srInterface=%p)\n",
                (void*)hWnd, m_srInterfaceDX9);
    return true;
}

bool Device9Proxy::RunSRWeave()
{
    if (!EnsureSRWeaver())     return false;
    if (!EnsureSRSBSTexture()) return false;
    if (!m_leftEyeSurf || !m_rightEyeSurf || !m_pTrackedBackBuffer) return false;

    NVDM_TRACE_FIRST_N(5, "  d3d9 RunSRWeave: entry tid=%lu L=%p R=%p sbs=%p bb=%p\n",
                       GetCurrentThreadId(),
                       (void*)m_leftEyeSurf, (void*)m_rightEyeSurf,
                       (void*)m_srSBSSurf, (void*)m_pTrackedBackBuffer);

    bool swap = (NvDM_SwapEyes() != 0);
    IDirect3DSurface9* leftSrc  = swap ? m_rightEyeSurf : m_leftEyeSurf;
    IDirect3DSurface9* rightSrc = swap ? m_leftEyeSurf  : m_rightEyeSurf;

    // Step A: blit each eye into the corresponding half of the SBS texture.
    RECT leftHalf  = { 0,                                 0,
                       (LONG)m_logicalWidth,              (LONG)m_logicalHeight };
    RECT rightHalf = { (LONG)m_logicalWidth,              0,
                       (LONG)(m_logicalWidth * 2),        (LONG)m_logicalHeight };
    HRESULT hr;
    hr = m_real->StretchRect(leftSrc,  nullptr, m_srSBSSurf, &leftHalf,  D3DTEXF_LINEAR);
    if (FAILED(hr)) return false;
    hr = m_real->StretchRect(rightSrc, nullptr, m_srSBSSurf, &rightHalf, D3DTEXF_LINEAR);
    if (FAILED(hr)) return false;

    // Step B: bind the real BB as RT and call Weave() — the SR-Lib
    // interface already has the SBS texture from SetInputTexture
    // (bound once in EnsureSRSBSTexture).
    IDirect3DSurface9* prevRT = nullptr;
    m_real->GetRenderTarget(0, &prevRT);
    m_real->SetRenderTarget(0, m_pTrackedBackBuffer);

    D3DVIEWPORT9 vp = { 0, 0, m_logicalWidth, m_logicalHeight, 0.0f, 1.0f };
    m_real->SetViewport(&vp);

    bool sceneBegun = SUCCEEDED(m_real->BeginScene());
    NVDM_TRACE_FIRST_N(5, "  d3d9 RunSRWeave: weave call - srInterface=%p SBS=%ux%u\n",
                       (void*)m_srInterfaceDX9, m_srSBSW, m_srSBSH);
    m_srInterfaceDX9->Weave();
    NVDM_TRACE_FIRST_N(5, "  d3d9 RunSRWeave: weave returned OK\n");
    if (sceneBegun) m_real->EndScene();

    if (prevRT) { m_real->SetRenderTarget(0, prevRT); prevRT->Release(); }

    static volatile long s_weaveCount = 0;
    long n = _InterlockedIncrement(&s_weaveCount);
    if (n == 60 || n == 180 || n == 600 || n == 1800)
        NvDM_Log("  d3d9 RunSRWeave: heartbeat #%ld (SR still alive)\n", n);

    return true;
}

void Device9Proxy::CompositeAndPresent()
{
    if (!m_real || !m_pTrackedBackBuffer) return;

    int currentEye = NvDirectMode::GetActiveEye();
    if (currentEye == NvDirectMode::kEyeLeft || currentEye == NvDirectMode::kEyeRight)
        CaptureEye(currentEye);

    // OutputMode 4-7: shader composite (Line/Col Interleaved, Checker, Anaglyph).
    // OutputMode 8:   SR weave (Leia / Samsung Odyssey ML).
    // Both fall through to the legacy StretchRect path below if their
    // resources aren't available (no d3dcompiler, runtime compile failure,
    // SR runtime not installed, SR Service down, blacklisted exe, etc.).
    int mode = NvDM_OutputMode();
    if (mode == 8 && m_leftEyeSurf && m_rightEyeSurf && RunSRWeave())
        return;
    if (mode >= 4 && mode <= 7 && m_leftEyeSurf && m_rightEyeSurf &&
        RunShaderComposite(mode))
        return;

    bool topBottom = NvDM_OutputIsTopBottom() != 0;
    bool swap      = NvDM_SwapEyes() != 0;
    IDirect3DSurface9* leftSrc  = swap ? m_rightEyeSurf : m_leftEyeSurf;
    IDirect3DSurface9* rightSrc = swap ? m_leftEyeSurf  : m_rightEyeSurf;

    D3DSURFACE_DESC bbDesc = {};
    m_pTrackedBackBuffer->GetDesc(&bbDesc);

    if (leftSrc && rightSrc)
    {
        // Composite: each eye into half the real BB.
        RECT leftRect, rightRect;
        if (topBottom)
        {
            leftRect  = { 0, 0, (LONG)bbDesc.Width, (LONG)(bbDesc.Height / 2) };
            rightRect = { 0, (LONG)(bbDesc.Height / 2), (LONG)bbDesc.Width, (LONG)bbDesc.Height };
        }
        else
        {
            leftRect  = { 0, 0, (LONG)(bbDesc.Width / 2), (LONG)bbDesc.Height };
            rightRect = { (LONG)(bbDesc.Width / 2), 0, (LONG)bbDesc.Width, (LONG)bbDesc.Height };
        }
        m_real->StretchRect(leftSrc,  NULL, m_pTrackedBackBuffer, &leftRect,  D3DTEXF_LINEAR);
        m_real->StretchRect(rightSrc, NULL, m_pTrackedBackBuffer, &rightRect, D3DTEXF_LINEAR);
        NVDM_TRACE_FIRST_N(2, "  d3d9 CompositeAndPresent: %s eyes (swap=%d) -> realBB=%p\n",
                           topBottom ? "T-B" : "SBS", (int)swap, (void*)m_pTrackedBackBuffer);
    }
    else
    {
        // Single eye / mono fallback.
        IDirect3DSurface9* src = leftSrc ? leftSrc : (rightSrc ? rightSrc : m_shadowBB);
        if (src)
            m_real->StretchRect(src, NULL, m_pTrackedBackBuffer, NULL, D3DTEXF_NONE);
        NVDM_TRACE_FIRST_N(4, "  d3d9 CompositeAndPresent (fallback): src=%s\n",
                           (src == m_leftEyeSurf  ? "leftEye"  :
                            src == m_rightEyeSurf ? "rightEye" :
                            src == m_shadowBB     ? "shadow"   : "none"));
    }
}

void Device9Proxy::SetLogicalBackBufferSize(UINT w, UINT h)
{
    m_logicalWidth  = w;
    m_logicalHeight = h;
}

void Device9Proxy::StashBackBufferReference()
{
    ReleaseBackBufferReference();
    ReleaseShadow();
    if (!m_real) return;
    // GetBackBuffer adds a ref; we hold that ref until Reset/destroy.
    m_real->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &m_pTrackedBackBuffer);
    EnsureShadow();
}

void Device9Proxy::ReleaseBackBufferReference()
{
    if (m_pTrackedBackBuffer)
    {
        m_pTrackedBackBuffer->Release();
        m_pTrackedBackBuffer = nullptr;
    }
}

HRESULT STDMETHODCALLTYPE Device9Proxy::QueryInterface(REFIID riid, void** ppvObj)
{
    if (!ppvObj) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IDirect3DDevice9)
    {
        *ppvObj = static_cast<IDirect3DDevice9*>(this);
        AddRef();
        return S_OK;
    }
    if (riid == IID_IDirect3DDevice9Ex)
    {
        if (!m_isEx) { *ppvObj = nullptr; return E_NOINTERFACE; }
        *ppvObj = static_cast<IDirect3DDevice9Ex*>(this);
        AddRef();
        return S_OK;
    }
    HRESULT hr = m_real->QueryInterface(riid, ppvObj);
    NVDM_TRACE_FIRST_N(8, "  Device9Proxy::QI(unknown IID) hr=0x%08lX -- bypass risk\n", hr);
    return hr;
}

ULONG STDMETHODCALLTYPE Device9Proxy::AddRef()  { return InterlockedIncrement(&m_refs); }
ULONG STDMETHODCALLTYPE Device9Proxy::Release()
{
    LONG r = InterlockedDecrement(&m_refs);
    if (r == 0)
    {
        if (m_real) { m_real->Release(); m_real = nullptr; }
        delete this;
    }
    return (ULONG)r;
}

// ---------------------------------------------------------------------------
// IDirect3DDevice9 — pure forwarders
// ---------------------------------------------------------------------------
HRESULT Device9Proxy::TestCooperativeLevel()                                          { return m_real->TestCooperativeLevel(); }
UINT    Device9Proxy::GetAvailableTextureMem()                                        { return m_real->GetAvailableTextureMem(); }
HRESULT Device9Proxy::EvictManagedResources()                                         { return m_real->EvictManagedResources(); }
HRESULT Device9Proxy::GetDirect3D(IDirect3D9** ppD3D9)                                { return m_real->GetDirect3D(ppD3D9); }
HRESULT Device9Proxy::GetDeviceCaps(D3DCAPS9* pCaps)                                  { return m_real->GetDeviceCaps(pCaps); }
HRESULT Device9Proxy::GetDisplayMode(UINT iSwapChain, D3DDISPLAYMODE* pMode)          { return m_real->GetDisplayMode(iSwapChain, pMode); }
HRESULT Device9Proxy::GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS* p)         { return m_real->GetCreationParameters(p); }
HRESULT Device9Proxy::SetCursorProperties(UINT X, UINT Y, IDirect3DSurface9* pBmp)    { return m_real->SetCursorProperties(X, Y, pBmp); }
void    Device9Proxy::SetCursorPosition(int X, int Y, DWORD Flags)                    { m_real->SetCursorPosition(X, Y, Flags); }
BOOL    Device9Proxy::ShowCursor(BOOL bShow)                                          { return m_real->ShowCursor(bShow); }
HRESULT Device9Proxy::CreateAdditionalSwapChain(D3DPRESENT_PARAMETERS* p, IDirect3DSwapChain9** pp) { return m_real->CreateAdditionalSwapChain(p, pp); }
HRESULT Device9Proxy::GetSwapChain(UINT iSwapChain, IDirect3DSwapChain9** pp)         { return m_real->GetSwapChain(iSwapChain, pp); }
UINT    Device9Proxy::GetNumberOfSwapChains()                                         { return m_real->GetNumberOfSwapChains(); }
HRESULT Device9Proxy::Reset(D3DPRESENT_PARAMETERS* p)
{
    // Old back buffer dies in Reset; release our tracking ref before forwarding.
    // SR weaver + intermediate texture also live in D3DPOOL_DEFAULT and must be
    // released before Reset; lazy-recreate on next RunSRWeave.
    ReleaseSRPipeline();
    ReleaseBackBufferReference();

    D3DPRESENT_PARAMETERS modified;
    UINT logicalW = 0, logicalH = 0;
    if (p)
    {
        modified = *p;
        ResolveAndDoubleSwapchainParams(&modified, modified.hDeviceWindow, &logicalW, &logicalH);
        p = &modified;
    }
    HRESULT hr = m_real->Reset(p);
    if (SUCCEEDED(hr))
    {
        if (logicalW > 0) SetLogicalBackBufferSize(logicalW, logicalH);
        StashBackBufferReference();
    }
    return hr;
}

HRESULT Device9Proxy::Present(CONST RECT* sr, CONST RECT* dr, HWND h, CONST RGNDATA* d)
{
    // Stage 4: composite captured eyes into the real BB before forwarding,
    // so the Present writes a SBS / T-B / mono image to the visible surface.
    CompositeAndPresent();
    return m_real->Present(sr, dr, h, d);
}
HRESULT Device9Proxy::GetBackBuffer(UINT iSC, UINT iBB, D3DBACKBUFFER_TYPE T, IDirect3DSurface9** pp)
{
    // Hand the game our shadow surface (logical size, format-matched to
    // the real BB) instead of the real BB itself. Game's draws land in
    // the shadow; CompositeAndPresent resolves them into the real BB at
    // Present time.
    if (iSC == 0 && iBB == 0 && T == D3DBACKBUFFER_TYPE_MONO && pp)
    {
        EnsureShadow();
        if (m_shadowBB)
        {
            m_shadowBB->AddRef();
            *pp = m_shadowBB;
            NVDM_TRACE_FIRST_N(4, "  d3d9 GetBackBuffer(0): handed shadow %p\n", m_shadowBB);
            return S_OK;
        }
    }
    return m_real->GetBackBuffer(iSC, iBB, T, pp);
}
HRESULT Device9Proxy::GetRasterStatus(UINT iSC, D3DRASTER_STATUS* p)                  { return m_real->GetRasterStatus(iSC, p); }
HRESULT Device9Proxy::SetDialogBoxMode(BOOL bEnable)                                  { return m_real->SetDialogBoxMode(bEnable); }
void    Device9Proxy::SetGammaRamp(UINT iSC, DWORD F, CONST D3DGAMMARAMP* p)          { m_real->SetGammaRamp(iSC, F, p); }
void    Device9Proxy::GetGammaRamp(UINT iSC, D3DGAMMARAMP* p)                         { m_real->GetGammaRamp(iSC, p); }
HRESULT Device9Proxy::CreateTexture(UINT W, UINT H, UINT L, DWORD U, D3DFORMAT F, D3DPOOL P, IDirect3DTexture9** pp, HANDLE* sh)            { return m_real->CreateTexture(W, H, L, U, F, P, pp, sh); }
HRESULT Device9Proxy::CreateVolumeTexture(UINT W, UINT H, UINT D, UINT L, DWORD U, D3DFORMAT F, D3DPOOL P, IDirect3DVolumeTexture9** pp, HANDLE* sh) { return m_real->CreateVolumeTexture(W, H, D, L, U, F, P, pp, sh); }
HRESULT Device9Proxy::CreateCubeTexture(UINT E, UINT L, DWORD U, D3DFORMAT F, D3DPOOL P, IDirect3DCubeTexture9** pp, HANDLE* sh)             { return m_real->CreateCubeTexture(E, L, U, F, P, pp, sh); }
HRESULT Device9Proxy::CreateVertexBuffer(UINT L, DWORD U, DWORD F, D3DPOOL P, IDirect3DVertexBuffer9** pp, HANDLE* sh)                       { return m_real->CreateVertexBuffer(L, U, F, P, pp, sh); }
HRESULT Device9Proxy::CreateIndexBuffer(UINT L, DWORD U, D3DFORMAT F, D3DPOOL P, IDirect3DIndexBuffer9** pp, HANDLE* sh)                     { return m_real->CreateIndexBuffer(L, U, F, P, pp, sh); }
HRESULT Device9Proxy::CreateRenderTarget(UINT W, UINT H, D3DFORMAT F, D3DMULTISAMPLE_TYPE M, DWORD MQ, BOOL Lk, IDirect3DSurface9** pp, HANDLE* sh) { return m_real->CreateRenderTarget(W, H, F, M, MQ, Lk, pp, sh); }
HRESULT Device9Proxy::CreateDepthStencilSurface(UINT W, UINT H, D3DFORMAT F, D3DMULTISAMPLE_TYPE M, DWORD MQ, BOOL Dc, IDirect3DSurface9** pp, HANDLE* sh) { return m_real->CreateDepthStencilSurface(W, H, F, M, MQ, Dc, pp, sh); }
HRESULT Device9Proxy::UpdateSurface(IDirect3DSurface9* sS, CONST RECT* sR, IDirect3DSurface9* dS, CONST POINT* dP) { return m_real->UpdateSurface(sS, sR, dS, dP); }
HRESULT Device9Proxy::UpdateTexture(IDirect3DBaseTexture9* sT, IDirect3DBaseTexture9* dT)                          { return m_real->UpdateTexture(sT, dT); }
HRESULT Device9Proxy::GetRenderTargetData(IDirect3DSurface9* sR, IDirect3DSurface9* dS)                            { return m_real->GetRenderTargetData(sR, dS); }
HRESULT Device9Proxy::GetFrontBufferData(UINT iSC, IDirect3DSurface9* dS)                                          { return m_real->GetFrontBufferData(iSC, dS); }
// ---------------------------------------------------------------------------
// Magic-header SBS surface tracking helpers (used by StretchRect below).
// ---------------------------------------------------------------------------
bool Device9Proxy::IsKnownStereoSurface(IDirect3DSurface9* s) const
{
    for (UINT i = 0; i < m_knownStereoSurfaceCount; ++i)
        if (m_knownStereoSurfaces[i] == s) return true;
    return false;
}

void Device9Proxy::MarkKnownStereoSurface(IDirect3DSurface9* s)
{
    if (IsKnownStereoSurface(s)) return;
    if (m_knownStereoSurfaceCount < kMaxKnownStereoSurfaces)
        m_knownStereoSurfaces[m_knownStereoSurfaceCount++] = s;
    // If full, silently drop — we'll just re-probe on each StretchRect for
    // surfaces that didn't fit. Acceptable: real games use 1-2 stereo surfaces.
}

HRESULT Device9Proxy::StretchRect(IDirect3DSurface9* sS, CONST RECT* sR,
                                  IDirect3DSurface9* dS, CONST RECT* dR,
                                  D3DTEXTUREFILTERTYPE F)
{
    // Magic-header SBS capture path. We're looking for the pattern where a
    // game / wrapper produces a (2W × H+1) surface tagged with
    // NVSTEREO_IMAGE_SIGNATURE and StretchRect's it to the swap-chain
    // backbuffer for the driver to present as stereo. We catch that here,
    // split into our per-eye surfaces, and let the downstream OutputMethod
    // composite normally — bypassing the wide-SBS-to-backbuffer blit so we
    // don't end up with the raw SBS image on screen.
    //
    // The destination check (dS == m_pTrackedBackBuffer) keeps us out of the
    // way of the game's intermediate stereo surface manipulations — only the
    // final blit-to-backbuffer triggers capture.
    if (sS && dS == m_pTrackedBackBuffer && m_pTrackedBackBuffer)
    {
        bool capture = false;
        UINT eyeW = 0, eyeH = 0;
        bool swap = m_magicHeaderSwapEyes;

        if (IsKnownStereoSurface(sS))
        {
            // Fast path — surface previously verified. Pull dims from desc.
            D3DSURFACE_DESC desc = {};
            if (SUCCEEDED(sS->GetDesc(&desc)) && desc.Width >= 2 && desc.Height >= 2)
            {
                eyeW = desc.Width / 2;
                eyeH = desc.Height - 1;
                capture = true;
            }
        }
        else
        {
            // Slow path — probe for magic. Once detected, cache the pointer
            // so subsequent frames skip the lock.
            NvDirectMode::MagicHeader::DetectResult r =
                NvDirectMode::MagicHeader::DetectStereoMagic(m_real, sS);
            if (r.hasMagic)
            {
                eyeW = r.eyeWidth;
                eyeH = r.eyeHeight;
                swap = r.swapEyes;
                m_magicHeaderActive   = true;
                m_magicHeaderSwapEyes = swap;
                MarkKnownStereoSurface(sS);
                capture = true;
                LOG_VERBOSE("  d3d9 StretchRect: magic-header SBS detected (src=%p, %ux%u eye, swap=%d) -> capture\n",
                            (void*)sS, eyeW, eyeH, swap);
            }
        }

        if (capture)
        {
            // Make sure our per-eye surfaces exist at the eye dimensions.
            // CaptureEye normally allocates them lazily based on m_shadowBB's
            // size, but in the magic-header path the shadow isn't involved.
            // If the slots are null OR the wrong size, (re)allocate.
            for (int e = 0; e < 2; ++e)
            {
                IDirect3DSurface9** slot = (e == 0) ? &m_leftEyeSurf : &m_rightEyeSurf;
                if (*slot)
                {
                    D3DSURFACE_DESC d = {};
                    if (SUCCEEDED((*slot)->GetDesc(&d)) &&
                        (d.Width != eyeW || d.Height != eyeH))
                    {
                        (*slot)->Release();
                        *slot = nullptr;
                    }
                }
                if (!*slot)
                {
                    D3DSURFACE_DESC src = {};
                    sS->GetDesc(&src);
                    HRESULT hr = m_real->CreateRenderTarget(
                        eyeW, eyeH, src.Format,
                        D3DMULTISAMPLE_NONE, 0, FALSE, slot, NULL);
                    if (FAILED(hr) || !*slot)
                    {
                        LOG_VERBOSE("  d3d9 StretchRect: magic-header eye RT alloc FAILED hr=0x%08lX\n", hr);
                        // Bail to legacy behaviour for this call.
                        return m_real->StretchRect(sS, sR, dS, dR, F);
                    }
                    LOG_VERBOSE("  d3d9 magic-header: alloc eyeSurf[%d]=%p (%ux%u fmt=%d)\n",
                                e, *slot, eyeW, eyeH, (int)src.Format);
                }
            }

            HRESULT hr = NvDirectMode::MagicHeader::SplitStereoSurface(
                m_real, sS, eyeW, eyeH, m_leftEyeSurf, m_rightEyeSurf, swap);
            if (FAILED(hr))
            {
                LOG_VERBOSE("  d3d9 magic-header: split FAILED hr=0x%08lX, falling back to passthrough\n", hr);
                return m_real->StretchRect(sS, sR, dS, dR, F);
            }
            // Skip the original blit — Present's OutputMethod composite will
            // overwrite the backbuffer using the per-eye surfaces. Returning
            // D3D_OK keeps the game happy.
            return D3D_OK;
        }
    }

    return m_real->StretchRect(sS, sR, dS, dR, F);
}
HRESULT Device9Proxy::ColorFill(IDirect3DSurface9* s, CONST RECT* r, D3DCOLOR c)                                   { return m_real->ColorFill(s, r, c); }
HRESULT Device9Proxy::CreateOffscreenPlainSurface(UINT W, UINT H, D3DFORMAT F, D3DPOOL P, IDirect3DSurface9** pp, HANDLE* sh)              { return m_real->CreateOffscreenPlainSurface(W, H, F, P, pp, sh); }
HRESULT Device9Proxy::SetRenderTarget(DWORD i, IDirect3DSurface9* p)
{
    HRESULT hr = m_real->SetRenderTarget(i, p);
    // Stage 3 v2 / shadow-RT: shadow is at logical (1x) size, so no
    // per-eye viewport clamp needed any more — game's natural rendering
    // goes straight into the shadow at the size the game expects.
    // BB-bound logging stays for diagnostics.
    if (i == 0 && p == m_shadowBB)
        NVDM_TRACE_FIRST_N(8, "  d3d9 SetRenderTarget BB(shadow=%p) bound (no clamp \xe2\x80\x94 shadow is 1x)\n",
                           m_shadowBB);
    return hr;
}
HRESULT Device9Proxy::GetRenderTarget(DWORD i, IDirect3DSurface9** pp)                                             { return m_real->GetRenderTarget(i, pp); }
HRESULT Device9Proxy::SetDepthStencilSurface(IDirect3DSurface9* p)                                                 { return m_real->SetDepthStencilSurface(p); }
HRESULT Device9Proxy::GetDepthStencilSurface(IDirect3DSurface9** pp)                                               { return m_real->GetDepthStencilSurface(pp); }
HRESULT Device9Proxy::BeginScene()                                                                                 { return m_real->BeginScene(); }
HRESULT Device9Proxy::EndScene()                                                                                   { return m_real->EndScene(); }
HRESULT Device9Proxy::Clear(DWORD C, CONST D3DRECT* pR, DWORD F, D3DCOLOR Co, float Z, DWORD S)                    { return m_real->Clear(C, pR, F, Co, Z, S); }
HRESULT Device9Proxy::SetTransform(D3DTRANSFORMSTATETYPE S, CONST D3DMATRIX* M)                                    { return m_real->SetTransform(S, M); }
HRESULT Device9Proxy::GetTransform(D3DTRANSFORMSTATETYPE S, D3DMATRIX* M)                                          { return m_real->GetTransform(S, M); }
HRESULT Device9Proxy::MultiplyTransform(D3DTRANSFORMSTATETYPE S, CONST D3DMATRIX* M)                               { return m_real->MultiplyTransform(S, M); }
HRESULT Device9Proxy::SetViewport(CONST D3DVIEWPORT9* p)                                                           { return m_real->SetViewport(p); }
HRESULT Device9Proxy::GetViewport(D3DVIEWPORT9* p)                                                                 { return m_real->GetViewport(p); }
HRESULT Device9Proxy::SetMaterial(CONST D3DMATERIAL9* p)                                                           { return m_real->SetMaterial(p); }
HRESULT Device9Proxy::GetMaterial(D3DMATERIAL9* p)                                                                 { return m_real->GetMaterial(p); }
HRESULT Device9Proxy::SetLight(DWORD i, CONST D3DLIGHT9* p)                                                        { return m_real->SetLight(i, p); }
HRESULT Device9Proxy::GetLight(DWORD i, D3DLIGHT9* p)                                                              { return m_real->GetLight(i, p); }
HRESULT Device9Proxy::LightEnable(DWORD i, BOOL b)                                                                 { return m_real->LightEnable(i, b); }
HRESULT Device9Proxy::GetLightEnable(DWORD i, BOOL* p)                                                             { return m_real->GetLightEnable(i, p); }
HRESULT Device9Proxy::SetClipPlane(DWORD i, CONST float* p)                                                        { return m_real->SetClipPlane(i, p); }
HRESULT Device9Proxy::GetClipPlane(DWORD i, float* p)                                                              { return m_real->GetClipPlane(i, p); }
HRESULT Device9Proxy::SetRenderState(D3DRENDERSTATETYPE S, DWORD V)                                                { return m_real->SetRenderState(S, V); }
HRESULT Device9Proxy::GetRenderState(D3DRENDERSTATETYPE S, DWORD* p)                                               { return m_real->GetRenderState(S, p); }
HRESULT Device9Proxy::CreateStateBlock(D3DSTATEBLOCKTYPE T, IDirect3DStateBlock9** pp)                             { return m_real->CreateStateBlock(T, pp); }
HRESULT Device9Proxy::BeginStateBlock()                                                                            { return m_real->BeginStateBlock(); }
HRESULT Device9Proxy::EndStateBlock(IDirect3DStateBlock9** pp)                                                     { return m_real->EndStateBlock(pp); }
HRESULT Device9Proxy::SetClipStatus(CONST D3DCLIPSTATUS9* p)                                                       { return m_real->SetClipStatus(p); }
HRESULT Device9Proxy::GetClipStatus(D3DCLIPSTATUS9* p)                                                             { return m_real->GetClipStatus(p); }
HRESULT Device9Proxy::GetTexture(DWORD S, IDirect3DBaseTexture9** pp)                                              { return m_real->GetTexture(S, pp); }
HRESULT Device9Proxy::SetTexture(DWORD S, IDirect3DBaseTexture9* p)                                                { return m_real->SetTexture(S, p); }
HRESULT Device9Proxy::GetTextureStageState(DWORD S, D3DTEXTURESTAGESTATETYPE T, DWORD* p)                          { return m_real->GetTextureStageState(S, T, p); }
HRESULT Device9Proxy::SetTextureStageState(DWORD S, D3DTEXTURESTAGESTATETYPE T, DWORD V)                           { return m_real->SetTextureStageState(S, T, V); }
HRESULT Device9Proxy::GetSamplerState(DWORD S, D3DSAMPLERSTATETYPE T, DWORD* p)                                    { return m_real->GetSamplerState(S, T, p); }
HRESULT Device9Proxy::SetSamplerState(DWORD S, D3DSAMPLERSTATETYPE T, DWORD V)                                     { return m_real->SetSamplerState(S, T, V); }
HRESULT Device9Proxy::ValidateDevice(DWORD* p)                                                                     { return m_real->ValidateDevice(p); }
HRESULT Device9Proxy::SetPaletteEntries(UINT N, CONST PALETTEENTRY* p)                                             { return m_real->SetPaletteEntries(N, p); }
HRESULT Device9Proxy::GetPaletteEntries(UINT N, PALETTEENTRY* p)                                                   { return m_real->GetPaletteEntries(N, p); }
HRESULT Device9Proxy::SetCurrentTexturePalette(UINT N)                                                             { return m_real->SetCurrentTexturePalette(N); }
HRESULT Device9Proxy::GetCurrentTexturePalette(UINT* p)                                                            { return m_real->GetCurrentTexturePalette(p); }
HRESULT Device9Proxy::SetScissorRect(CONST RECT* p)                                                                { return m_real->SetScissorRect(p); }
HRESULT Device9Proxy::GetScissorRect(RECT* p)                                                                      { return m_real->GetScissorRect(p); }
HRESULT Device9Proxy::SetSoftwareVertexProcessing(BOOL b)                                                          { return m_real->SetSoftwareVertexProcessing(b); }
BOOL    Device9Proxy::GetSoftwareVertexProcessing()                                                                { return m_real->GetSoftwareVertexProcessing(); }
HRESULT Device9Proxy::SetNPatchMode(float n)                                                                       { return m_real->SetNPatchMode(n); }
float   Device9Proxy::GetNPatchMode()                                                                              { return m_real->GetNPatchMode(); }
HRESULT Device9Proxy::DrawPrimitive(D3DPRIMITIVETYPE T, UINT SV, UINT PC)                                          { return m_real->DrawPrimitive(T, SV, PC); }
HRESULT Device9Proxy::DrawIndexedPrimitive(D3DPRIMITIVETYPE T, INT BV, UINT MV, UINT NV, UINT SI, UINT PC)         { return m_real->DrawIndexedPrimitive(T, BV, MV, NV, SI, PC); }
HRESULT Device9Proxy::DrawPrimitiveUP(D3DPRIMITIVETYPE T, UINT PC, CONST void* p, UINT St)                         { return m_real->DrawPrimitiveUP(T, PC, p, St); }
HRESULT Device9Proxy::DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE T, UINT MV, UINT NV, UINT PC, CONST void* iD, D3DFORMAT iF, CONST void* vD, UINT vS) { return m_real->DrawIndexedPrimitiveUP(T, MV, NV, PC, iD, iF, vD, vS); }
HRESULT Device9Proxy::ProcessVertices(UINT SS, UINT DI, UINT VC, IDirect3DVertexBuffer9* pDB, IDirect3DVertexDeclaration9* pVD, DWORD F)   { return m_real->ProcessVertices(SS, DI, VC, pDB, pVD, F); }
HRESULT Device9Proxy::CreateVertexDeclaration(CONST D3DVERTEXELEMENT9* pE, IDirect3DVertexDeclaration9** pp)       { return m_real->CreateVertexDeclaration(pE, pp); }
HRESULT Device9Proxy::SetVertexDeclaration(IDirect3DVertexDeclaration9* p)                                         { return m_real->SetVertexDeclaration(p); }
HRESULT Device9Proxy::GetVertexDeclaration(IDirect3DVertexDeclaration9** pp)                                       { return m_real->GetVertexDeclaration(pp); }
HRESULT Device9Proxy::SetFVF(DWORD F)                                                                              { return m_real->SetFVF(F); }
HRESULT Device9Proxy::GetFVF(DWORD* p)                                                                             { return m_real->GetFVF(p); }
HRESULT Device9Proxy::CreateVertexShader(CONST DWORD* p, IDirect3DVertexShader9** pp)                              { return m_real->CreateVertexShader(p, pp); }
HRESULT Device9Proxy::SetVertexShader(IDirect3DVertexShader9* p)                                                   { return m_real->SetVertexShader(p); }
HRESULT Device9Proxy::GetVertexShader(IDirect3DVertexShader9** pp)                                                 { return m_real->GetVertexShader(pp); }
HRESULT Device9Proxy::SetVertexShaderConstantF(UINT SR, CONST float* p, UINT C)                                    { return m_real->SetVertexShaderConstantF(SR, p, C); }
HRESULT Device9Proxy::GetVertexShaderConstantF(UINT SR, float* p, UINT C)                                          { return m_real->GetVertexShaderConstantF(SR, p, C); }
HRESULT Device9Proxy::SetVertexShaderConstantI(UINT SR, CONST int* p, UINT C)                                      { return m_real->SetVertexShaderConstantI(SR, p, C); }
HRESULT Device9Proxy::GetVertexShaderConstantI(UINT SR, int* p, UINT C)                                            { return m_real->GetVertexShaderConstantI(SR, p, C); }
HRESULT Device9Proxy::SetVertexShaderConstantB(UINT SR, CONST BOOL* p, UINT C)                                     { return m_real->SetVertexShaderConstantB(SR, p, C); }
HRESULT Device9Proxy::GetVertexShaderConstantB(UINT SR, BOOL* p, UINT C)                                           { return m_real->GetVertexShaderConstantB(SR, p, C); }
HRESULT Device9Proxy::SetStreamSource(UINT SN, IDirect3DVertexBuffer9* pSD, UINT OB, UINT St)                      { return m_real->SetStreamSource(SN, pSD, OB, St); }
HRESULT Device9Proxy::GetStreamSource(UINT SN, IDirect3DVertexBuffer9** pp, UINT* pO, UINT* pS)                    { return m_real->GetStreamSource(SN, pp, pO, pS); }
HRESULT Device9Proxy::SetStreamSourceFreq(UINT SN, UINT S)                                                         { return m_real->SetStreamSourceFreq(SN, S); }
HRESULT Device9Proxy::GetStreamSourceFreq(UINT SN, UINT* p)                                                        { return m_real->GetStreamSourceFreq(SN, p); }
HRESULT Device9Proxy::SetIndices(IDirect3DIndexBuffer9* p)                                                         { return m_real->SetIndices(p); }
HRESULT Device9Proxy::GetIndices(IDirect3DIndexBuffer9** pp)                                                       { return m_real->GetIndices(pp); }
HRESULT Device9Proxy::CreatePixelShader(CONST DWORD* p, IDirect3DPixelShader9** pp)                                { return m_real->CreatePixelShader(p, pp); }
HRESULT Device9Proxy::SetPixelShader(IDirect3DPixelShader9* p)                                                     { return m_real->SetPixelShader(p); }
HRESULT Device9Proxy::GetPixelShader(IDirect3DPixelShader9** pp)                                                   { return m_real->GetPixelShader(pp); }
HRESULT Device9Proxy::SetPixelShaderConstantF(UINT SR, CONST float* p, UINT C)                                     { return m_real->SetPixelShaderConstantF(SR, p, C); }
HRESULT Device9Proxy::GetPixelShaderConstantF(UINT SR, float* p, UINT C)                                           { return m_real->GetPixelShaderConstantF(SR, p, C); }
HRESULT Device9Proxy::SetPixelShaderConstantI(UINT SR, CONST int* p, UINT C)                                       { return m_real->SetPixelShaderConstantI(SR, p, C); }
HRESULT Device9Proxy::GetPixelShaderConstantI(UINT SR, int* p, UINT C)                                             { return m_real->GetPixelShaderConstantI(SR, p, C); }
HRESULT Device9Proxy::SetPixelShaderConstantB(UINT SR, CONST BOOL* p, UINT C)                                      { return m_real->SetPixelShaderConstantB(SR, p, C); }
HRESULT Device9Proxy::GetPixelShaderConstantB(UINT SR, BOOL* p, UINT C)                                            { return m_real->GetPixelShaderConstantB(SR, p, C); }
HRESULT Device9Proxy::DrawRectPatch(UINT H, CONST float* p, CONST D3DRECTPATCH_INFO* pI)                           { return m_real->DrawRectPatch(H, p, pI); }
HRESULT Device9Proxy::DrawTriPatch(UINT H, CONST float* p, CONST D3DTRIPATCH_INFO* pI)                             { return m_real->DrawTriPatch(H, p, pI); }
HRESULT Device9Proxy::DeletePatch(UINT H)                                                                          { return m_real->DeletePatch(H); }
HRESULT Device9Proxy::CreateQuery(D3DQUERYTYPE T, IDirect3DQuery9** pp)                                            { return m_real->CreateQuery(T, pp); }

// ---------------------------------------------------------------------------
// IDirect3DDevice9Ex extras — only reachable when m_isEx (QI gates the cast)
// ---------------------------------------------------------------------------
HRESULT Device9Proxy::SetConvolutionMonoKernel(UINT W, UINT H, float* r, float* c)                                                                  { return m_realEx->SetConvolutionMonoKernel(W, H, r, c); }
HRESULT Device9Proxy::ComposeRects(IDirect3DSurface9* pS, IDirect3DSurface9* pD, IDirect3DVertexBuffer9* pSR, UINT N, IDirect3DVertexBuffer9* pDR, D3DCOMPOSERECTSOP O, INT X, INT Y) { return m_realEx->ComposeRects(pS, pD, pSR, N, pDR, O, X, Y); }
HRESULT Device9Proxy::PresentEx(CONST RECT* sR, CONST RECT* dR, HWND h, CONST RGNDATA* pD, DWORD F)
{
    // Same composite as Present() — capture latest eye + StretchRect both
    // eyes (or fallback) into the real BB, then forward.
    CompositeAndPresent();
    return m_realEx->PresentEx(sR, dR, h, pD, F);
}
HRESULT Device9Proxy::GetGPUThreadPriority(INT* p)                                                                                                  { return m_realEx->GetGPUThreadPriority(p); }
HRESULT Device9Proxy::SetGPUThreadPriority(INT P)                                                                                                   { return m_realEx->SetGPUThreadPriority(P); }
HRESULT Device9Proxy::WaitForVBlank(UINT iSC)                                                                                                       { return m_realEx->WaitForVBlank(iSC); }
HRESULT Device9Proxy::CheckResourceResidency(IDirect3DResource9** pRA, UINT32 N)                                                                    { return m_realEx->CheckResourceResidency(pRA, N); }
HRESULT Device9Proxy::SetMaximumFrameLatency(UINT M)                                                                                                { return m_realEx->SetMaximumFrameLatency(M); }
HRESULT Device9Proxy::GetMaximumFrameLatency(UINT* p)                                                                                               { return m_realEx->GetMaximumFrameLatency(p); }
HRESULT Device9Proxy::CheckDeviceState(HWND h)                                                                                                      { return m_realEx->CheckDeviceState(h); }
HRESULT Device9Proxy::CreateRenderTargetEx(UINT W, UINT H, D3DFORMAT F, D3DMULTISAMPLE_TYPE M, DWORD MQ, BOOL L, IDirect3DSurface9** pp, HANDLE* sh, DWORD U)            { return m_realEx->CreateRenderTargetEx(W, H, F, M, MQ, L, pp, sh, U); }
HRESULT Device9Proxy::CreateOffscreenPlainSurfaceEx(UINT W, UINT H, D3DFORMAT F, D3DPOOL P, IDirect3DSurface9** pp, HANDLE* sh, DWORD U)                                 { return m_realEx->CreateOffscreenPlainSurfaceEx(W, H, F, P, pp, sh, U); }
HRESULT Device9Proxy::CreateDepthStencilSurfaceEx(UINT W, UINT H, D3DFORMAT F, D3DMULTISAMPLE_TYPE M, DWORD MQ, BOOL D, IDirect3DSurface9** pp, HANDLE* sh, DWORD U)     { return m_realEx->CreateDepthStencilSurfaceEx(W, H, F, M, MQ, D, pp, sh, U); }
HRESULT Device9Proxy::ResetEx(D3DPRESENT_PARAMETERS* pP, D3DDISPLAYMODEEX* pF)
{
    ReleaseSRPipeline();
    ReleaseBackBufferReference();

    D3DPRESENT_PARAMETERS modified;
    UINT logicalW = 0, logicalH = 0;
    if (pP)
    {
        modified = *pP;
        ResolveAndDoubleSwapchainParams(&modified, modified.hDeviceWindow, &logicalW, &logicalH);
        pP = &modified;
    }
    HRESULT hr = m_realEx->ResetEx(pP, pF);
    if (SUCCEEDED(hr))
    {
        if (logicalW > 0) SetLogicalBackBufferSize(logicalW, logicalH);
        StashBackBufferReference();
    }
    return hr;
}
HRESULT Device9Proxy::GetDisplayModeEx(UINT iSC, D3DDISPLAYMODEEX* p, D3DDISPLAYROTATION* pR)                                                       { return m_realEx->GetDisplayModeEx(iSC, p, pR); }

} // namespace NvDirectMode
