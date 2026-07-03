#include "SwapChainProxy.h"
#include "Device10Proxy.h"
#include "eye_state.h"
#include "log.h"
#include "../anaglyph_matrices.h"

// SR-Lib — static library wrapping the Simulated Reality SDK.
// All SR import libs are merged into SR-mt[d].lib; the final DLL must
// delay-load the SR runtime DLLs (see SR.hpp header comment for the list).
//
// NOTE: SR-Lib does not yet have a DX10 interface (SRInterfaceDX10 /
// CreateSRInterfaceDX10). This file mirrors the DX11 SR-Lib pattern as
// a prototype — it will not compile until SR-Lib adds DX10 support.
// The d3d10 project is excluded from the solution build in the meantime.
#include "SR.hpp"

#include <ctype.h>
#include <stdlib.h>

#include <d3dcommon.h>
#include <string.h>

#pragma comment(lib, "dxguid.lib")

extern "C" int NvDM_OutputIsTopBottom();
extern "C" int NvDM_SwapEyes();
extern "C" int NvDM_OutputMode();
extern "C" int NvDM_AnaglyphColour();
extern "C" int NvDM_AnaglyphMethod();
extern "C" int NvDM_ForceSRWeave();

// ---------------------------------------------------------------------------
// d3dcompiler resolver + composite shader source (same shaders as d3d11;
// vs_4_0 / ps_4_0 are valid for both d3d10 and d3d11)
// ---------------------------------------------------------------------------
namespace {

typedef HRESULT (WINAPI *PFN_D3DCompile)(
    LPCVOID, SIZE_T, LPCSTR,
    const D3D_SHADER_MACRO*, ID3DInclude*,
    LPCSTR, LPCSTR, UINT, UINT,
    ID3DBlob**, ID3DBlob**);

static PFN_D3DCompile g_pfnD3DCompile = nullptr;
static volatile LONG g_d3dCompileResolved = 0;

bool EnsureD3DCompile()
{
    if (g_pfnD3DCompile) return true;
    if (InterlockedCompareExchange(&g_d3dCompileResolved, 1, 0) != 0)
        return g_pfnD3DCompile != nullptr;
    HMODULE h = LoadLibraryW(L"d3dcompiler_47.dll");
    if (!h) h = LoadLibraryW(L"d3dcompiler_46.dll");
    if (!h) h = LoadLibraryW(L"d3dcompiler_43.dll");
    if (!h) return false;
    g_pfnD3DCompile = (PFN_D3DCompile)GetProcAddress(h, "D3DCompile");
    return g_pfnD3DCompile != nullptr;
}

const char kCompositeVS[] =
    "struct VS_OUT { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
    "VS_OUT main(uint id : SV_VertexID) {\n"
    "  VS_OUT o;\n"
    "  o.uv = float2((id == 1) ? 2.0 : 0.0, (id == 2) ? 2.0 : 0.0);\n"
    "  o.pos = float4(o.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);\n"
    "  return o;\n"
    "}\n";

const char kCompositePS_SBS[] =
    "Texture2D leftTex  : register(t0);\n"
    "Texture2D rightTex : register(t1);\n"
    "SamplerState sLinear : register(s0);\n"
    "struct VS_OUT { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
    "float4 main(VS_OUT i) : SV_Target {\n"
    "  if (i.uv.x < 0.5)\n"
    "    return leftTex.Sample(sLinear, float2(i.uv.x * 2.0, i.uv.y));\n"
    "  else\n"
    "    return rightTex.Sample(sLinear, float2((i.uv.x - 0.5) * 2.0, i.uv.y));\n"
    "}\n";

const char kCompositePS_TB[] =
    "Texture2D leftTex  : register(t0);\n"
    "Texture2D rightTex : register(t1);\n"
    "SamplerState sLinear : register(s0);\n"
    "struct VS_OUT { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
    "float4 main(VS_OUT i) : SV_Target {\n"
    "  if (i.uv.y < 0.5)\n"
    "    return leftTex.Sample(sLinear, float2(i.uv.x, i.uv.y * 2.0));\n"
    "  else\n"
    "    return rightTex.Sample(sLinear, float2(i.uv.x, (i.uv.y - 0.5) * 2.0));\n"
    "}\n";

// OutputMode 4 — Line/Row Interleaved
const char kCompositePS_LineInterleaved[] =
    "Texture2D leftTex  : register(t0);\n"
    "Texture2D rightTex : register(t1);\n"
    "SamplerState sPoint : register(s0);\n"
    "struct VS_OUT { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
    "float4 main(VS_OUT i) : SV_Target {\n"
    "  float4 L = leftTex.Sample(sPoint, i.uv);\n"
    "  float4 R = rightTex.Sample(sPoint, i.uv);\n"
    "  bool ev = fmod(floor(i.pos.y), 2) < 1;\n"
    "  return ev ? L : R;\n"
    "}\n";

// OutputMode 5 — Column Interleaved
const char kCompositePS_ColInterleaved[] =
    "Texture2D leftTex  : register(t0);\n"
    "Texture2D rightTex : register(t1);\n"
    "SamplerState sPoint : register(s0);\n"
    "struct VS_OUT { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
    "float4 main(VS_OUT i) : SV_Target {\n"
    "  float4 L = leftTex.Sample(sPoint, i.uv);\n"
    "  float4 R = rightTex.Sample(sPoint, i.uv);\n"
    "  bool ev = fmod(floor(i.pos.x), 2) < 1;\n"
    "  return ev ? L : R;\n"
    "}\n";

// OutputMode 6 — Checkerboard
const char kCompositePS_Checkerboard[] =
    "Texture2D leftTex  : register(t0);\n"
    "Texture2D rightTex : register(t1);\n"
    "SamplerState sPoint : register(s0);\n"
    "struct VS_OUT { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
    "float4 main(VS_OUT i) : SV_Target {\n"
    "  float4 L = leftTex.Sample(sPoint, i.uv);\n"
    "  float4 R = rightTex.Sample(sPoint, i.uv);\n"
    "  bool ev = fmod(floor(i.pos.x) + floor(i.pos.y), 2) < 1;\n"
    "  return ev ? L : R;\n"
    "}\n";

// OutputMode 7 — Anaglyph (matrix coefficients in CB slot b2)
const char kCompositePS_Anaglyph[] =
    "Texture2D leftTex  : register(t0);\n"
    "Texture2D rightTex : register(t1);\n"
    "SamplerState sLinear : register(s0);\n"
    "cbuffer ACB : register(b2) { float4 lR; float4 lG; float4 lB; float4 rR; float4 rG; float4 rB; };\n"
    "struct VS_OUT { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
    "float4 main(VS_OUT i) : SV_Target {\n"
    "  float3 L = leftTex.Sample(sLinear, i.uv).rgb;\n"
    "  float3 R = rightTex.Sample(sLinear, i.uv).rgb;\n"
    "  float3 a;\n"
    "  a.r = dot(lR.xyz, L) + dot(rR.xyz, R);\n"
    "  a.g = dot(lG.xyz, L) + dot(rG.xyz, R);\n"
    "  a.b = dot(lB.xyz, L) + dot(rB.xyz, R);\n"
    "  return float4(saturate(a), 1.0);\n"
    "}\n";

bool CompileShader(const char* src, size_t len, const char* tag, const char* target, ID3DBlob** out)
{
    if (!g_pfnD3DCompile) return false;
    ID3DBlob* err = nullptr;
    HRESULT hr = g_pfnD3DCompile(src, len, tag, nullptr, nullptr,
                                  "main", target, 0, 0, out, &err);
    if (err) { err->Release(); err = nullptr; }
    return SUCCEEDED(hr) && *out != nullptr;
}

} // anonymous namespace

namespace NvDirectMode
{

// Primary-swap-chain registry for the eye-change callback dispatcher.
namespace
{
    SwapChainProxy*  g_primarySwapChain = nullptr;
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
        // Skip SetActiveEye-driven shadow-capture if the game is using the
        // magic-header SBS path — those eye textures are already populated
        // from the SBS split. See d3d9 OnEyeChange for the same gating.
        if (g_primarySwapChain && !g_primarySwapChain->IsMagicHeaderActive())
            g_primarySwapChain->CaptureEye(oldEye);
        LeaveCriticalSection(&g_primaryLock);
    }
}

SwapChainProxy::SwapChainProxy(IDXGISwapChain* real, Device10Proxy* parent)
    : m_real(real)
    , m_parent(parent)
    , m_refs(1)
    , m_shadowBB(nullptr)
    , m_logicalW(0)
    , m_logicalH(0)
    , m_shadowFormat(DXGI_FORMAT_UNKNOWN)
    , m_leftEyeFrame(nullptr)
    , m_rightEyeFrame(nullptr)
    , m_lastSeenEye(NvDirectMode::kEyeMono)
    , m_magicHeaderActive(false)
    , m_compositeVS(nullptr)
    , m_compositePS_SBS(nullptr)
    , m_compositePS_TB(nullptr)
    , m_compositePS_Line(nullptr)
    , m_compositePS_Col(nullptr)
    , m_compositePS_Checker(nullptr)
    , m_compositePS_Anaglyph(nullptr)
    , m_anaglyphCB(nullptr)
    , m_compositeSampler(nullptr)
    , m_compositeSamplerPoint(nullptr)
    , m_compositeRS(nullptr)
    , m_compositeBlend(nullptr)
    , m_compositeDSS(nullptr)
    , m_leftEyeSRV(nullptr)
    , m_rightEyeSRV(nullptr)
    , m_realBBRTV(nullptr)
    , m_srBlacklistedOrFailed(false)
    , m_srInterfaceDX10(nullptr)
    , m_srSBSTex(nullptr)
    , m_srSBSRTV(nullptr)
    , m_srSBSSRV(nullptr)
    , m_srSBSW(0)
    , m_srSBSH(0)
    , m_srSBSFmt(DXGI_FORMAT_UNKNOWN)
{
    EnsurePrimaryLock();
    EnterCriticalSection(&g_primaryLock);
    g_primarySwapChain = this;
    LeaveCriticalSection(&g_primaryLock);
    NvDirectMode::RegisterEyeChangeHandler(&OnEyeChange);
}

SwapChainProxy::~SwapChainProxy()
{
    EnsurePrimaryLock();
    EnterCriticalSection(&g_primaryLock);
    if (g_primarySwapChain == this) g_primarySwapChain = nullptr;
    LeaveCriticalSection(&g_primaryLock);

    ReleaseSRPipeline();
    ReleaseCompositePipeline();
    ReleaseEyeFrames();
    ReleaseShadowBB();
    if (m_real) { m_real->Release(); m_real = nullptr; }
}

void SwapChainProxy::ReleaseShadowBB()
{
    if (m_shadowBB) { m_shadowBB->Release(); m_shadowBB = nullptr; }
    m_logicalW = 0;
    m_logicalH = 0;
    m_shadowFormat = DXGI_FORMAT_UNKNOWN;
}

void SwapChainProxy::ReleaseEyeFrames()
{
    if (m_leftEyeSRV)    { m_leftEyeSRV->Release();    m_leftEyeSRV = nullptr; }
    if (m_rightEyeSRV)   { m_rightEyeSRV->Release();   m_rightEyeSRV = nullptr; }
    if (m_leftEyeFrame)  { m_leftEyeFrame->Release();  m_leftEyeFrame = nullptr; }
    if (m_rightEyeFrame) { m_rightEyeFrame->Release(); m_rightEyeFrame = nullptr; }
    m_lastSeenEye = NvDirectMode::kEyeMono;
}

void SwapChainProxy::ReleaseCompositePipeline()
{
    if (m_realBBRTV)             { m_realBBRTV->Release();             m_realBBRTV = nullptr; }
    if (m_leftEyeSRV)            { m_leftEyeSRV->Release();            m_leftEyeSRV = nullptr; }
    if (m_rightEyeSRV)           { m_rightEyeSRV->Release();           m_rightEyeSRV = nullptr; }
    if (m_compositeDSS)          { m_compositeDSS->Release();          m_compositeDSS = nullptr; }
    if (m_compositeBlend)        { m_compositeBlend->Release();        m_compositeBlend = nullptr; }
    if (m_compositeRS)           { m_compositeRS->Release();           m_compositeRS = nullptr; }
    if (m_compositeSamplerPoint) { m_compositeSamplerPoint->Release(); m_compositeSamplerPoint = nullptr; }
    if (m_compositeSampler)      { m_compositeSampler->Release();      m_compositeSampler = nullptr; }
    if (m_anaglyphCB)            { m_anaglyphCB->Release();            m_anaglyphCB = nullptr; }
    if (m_compositePS_Anaglyph)  { m_compositePS_Anaglyph->Release();  m_compositePS_Anaglyph = nullptr; }
    if (m_compositePS_Checker)   { m_compositePS_Checker->Release();   m_compositePS_Checker = nullptr; }
    if (m_compositePS_Col)       { m_compositePS_Col->Release();       m_compositePS_Col = nullptr; }
    if (m_compositePS_Line)      { m_compositePS_Line->Release();      m_compositePS_Line = nullptr; }
    if (m_compositePS_TB)        { m_compositePS_TB->Release();        m_compositePS_TB = nullptr; }
    if (m_compositePS_SBS)       { m_compositePS_SBS->Release();       m_compositePS_SBS = nullptr; }
    if (m_compositeVS)           { m_compositeVS->Release();           m_compositeVS = nullptr; }
}

void SwapChainProxy::EnsureShadowBB()
{
    if (m_shadowBB || !m_real || !m_parent) return;

    DXGI_SWAP_CHAIN_DESC desc = {};
    if (FAILED(m_real->GetDesc(&desc))) return;
    m_logicalW = desc.BufferDesc.Width;
    m_logicalH = desc.BufferDesc.Height;
    m_shadowFormat = desc.BufferDesc.Format;
    if (m_logicalW == 0 || m_logicalH == 0) return;

    m_parent->SetLogicalBackBufferSize(m_logicalW, m_logicalH);

    ID3D10Device* dev = m_parent->GetReal();
    if (!dev) return;

    D3D10_TEXTURE2D_DESC td = {};
    td.Width            = m_logicalW;
    td.Height           = m_logicalH;
    td.MipLevels        = 1;
    td.ArraySize        = 1;
    td.Format           = m_shadowFormat;
    td.SampleDesc.Count = 1;
    td.Usage            = D3D10_USAGE_DEFAULT;
    td.BindFlags        = D3D10_BIND_RENDER_TARGET | D3D10_BIND_SHADER_RESOURCE;

    HRESULT hr = dev->CreateTexture2D(&td, nullptr, &m_shadowBB);
    if (FAILED(hr) || !m_shadowBB)
    {
        LOG_VERBOSE("  d3d10 EnsureShadowBB: CreateTexture2D(%ux%u) FAILED hr=0x%08lX\n",
                    m_logicalW, m_logicalH, hr);
        m_shadowBB = nullptr;
        return;
    }
    LOG_VERBOSE("  d3d10 EnsureShadowBB: shadow=%p (%ux%u 1x logical, fmt=%d)\n",
                m_shadowBB, m_logicalW, m_logicalH, (int)m_shadowFormat);
}

void SwapChainProxy::CaptureEye(int eyeBeingLeft)
{
    if (!m_shadowBB || !m_parent) return;

    ID3D10Texture2D** slot = nullptr;
    if      (eyeBeingLeft == NvDirectMode::kEyeLeft)  slot = &m_leftEyeFrame;
    else if (eyeBeingLeft == NvDirectMode::kEyeRight) slot = &m_rightEyeFrame;
    else return;

    if (!*slot)
    {
        ID3D10Device* dev = m_parent->GetReal();
        if (!dev) return;
        D3D10_TEXTURE2D_DESC td = {};
        td.Width            = m_logicalW;
        td.Height           = m_logicalH;
        td.MipLevels        = 1;
        td.ArraySize        = 1;
        td.Format           = m_shadowFormat;
        td.SampleDesc.Count = 1;
        td.Usage            = D3D10_USAGE_DEFAULT;
        td.BindFlags        = D3D10_BIND_SHADER_RESOURCE;
        if (FAILED(dev->CreateTexture2D(&td, nullptr, slot)) || !*slot) return;
        LOG_VERBOSE("  d3d10 CaptureEye(%d): allocated eye texture=%p\n", eyeBeingLeft, *slot);
    }

    ID3D10Device* dev = m_parent->GetReal();
    if (!dev) return;
    dev->CopyResource(*slot, m_shadowBB);
    NVDM_TRACE_FIRST_N(8, "  d3d10 CaptureEye(eye=%d): copied shadow=%p -> eyeFrame=%p\n",
                       eyeBeingLeft, m_shadowBB, *slot);
}

SwapChainProxy* SwapChainProxy::GetPrimary()
{
    EnsurePrimaryLock();
    EnterCriticalSection(&g_primaryLock);
    SwapChainProxy* p = g_primarySwapChain;
    LeaveCriticalSection(&g_primaryLock);
    return p;
}

void SwapChainProxy::CaptureMagicHeaderSBS(ID3D10Resource* pSrc,
                                           UINT eyeWidth, UINT eyeHeight,
                                           bool swapEyes)
{
    if (!pSrc || !m_parent) return;
    ID3D10Device* dev = m_parent->GetReal();
    if (!dev) return;

    // Pull desc from source (Texture2D-typed) — we need Format / SampleDesc
    // for the eye-texture allocations.
    ID3D10Texture2D* srcTex = nullptr;
    if (FAILED(pSrc->QueryInterface(__uuidof(ID3D10Texture2D), (void**)&srcTex)) || !srcTex)
        return;
    D3D10_TEXTURE2D_DESC srcDesc = {};
    srcTex->GetDesc(&srcDesc);
    srcTex->Release();

    // (Re)allocate eye textures if missing or wrong size.
    for (int e = 0; e < 2; ++e)
    {
        ID3D10Texture2D** slot = (e == 0) ? &m_leftEyeFrame : &m_rightEyeFrame;
        if (*slot)
        {
            D3D10_TEXTURE2D_DESC d = {};
            (*slot)->GetDesc(&d);
            if (d.Width != eyeWidth || d.Height != eyeHeight || d.Format != srcDesc.Format)
            {
                (*slot)->Release();
                *slot = nullptr;
            }
        }
        if (!*slot)
        {
            D3D10_TEXTURE2D_DESC td = {};
            td.Width            = eyeWidth;
            td.Height           = eyeHeight;
            td.MipLevels        = 1;
            td.ArraySize        = 1;
            td.Format           = srcDesc.Format;
            td.SampleDesc.Count = 1;
            td.Usage            = D3D10_USAGE_DEFAULT;
            td.BindFlags        = D3D10_BIND_SHADER_RESOURCE;
            if (FAILED(dev->CreateTexture2D(&td, nullptr, slot)) || !*slot)
            {
                LOG_VERBOSE("  d3d10 magic-header: eye tex alloc FAILED (%ux%u)\n",
                            eyeWidth, eyeHeight);
                return;
            }
            LOG_VERBOSE("  d3d10 magic-header: alloc eyeFrame[%d]=%p (%ux%u)\n",
                        e, *slot, eyeWidth, eyeHeight);
        }
    }

    D3D10_BOX leftBox  = { 0,         0, 0, eyeWidth,     eyeHeight, 1 };
    D3D10_BOX rightBox = { eyeWidth,  0, 0, eyeWidth * 2, eyeHeight, 1 };
    ID3D10Texture2D* leftOnto  = swapEyes ? m_rightEyeFrame : m_leftEyeFrame;
    ID3D10Texture2D* rightOnto = swapEyes ? m_leftEyeFrame  : m_rightEyeFrame;
    dev->CopySubresourceRegion(leftOnto,  0, 0, 0, 0, pSrc, 0, &leftBox);
    dev->CopySubresourceRegion(rightOnto, 0, 0, 0, 0, pSrc, 0, &rightBox);

    if (!m_magicHeaderActive)
    {
        m_magicHeaderActive = true;
        m_logicalW = eyeWidth;
        m_logicalH = eyeHeight;
        m_shadowFormat = srcDesc.Format;
        LOG_VERBOSE("  d3d10 magic-header: ACTIVE (%ux%u eye, swap=%d)\n",
                    eyeWidth, eyeHeight, swapEyes);
    }
}

bool SwapChainProxy::EnsureCompositeShaders()
{
    if (!m_parent) return false;
    if (m_compositeVS && m_compositePS_SBS && m_compositePS_TB &&
        m_compositePS_Line && m_compositePS_Col && m_compositePS_Checker &&
        m_compositePS_Anaglyph && m_anaglyphCB &&
        m_compositeSampler && m_compositeSamplerPoint &&
        m_compositeRS && m_compositeBlend && m_compositeDSS)
        return true;
    if (!EnsureD3DCompile()) return false;

    ID3D10Device* dev = m_parent->GetReal();
    if (!dev) return false;

    ID3DBlob* vsBlob = nullptr;
    ID3DBlob* psSbsBlob = nullptr;
    ID3DBlob* psTbBlob = nullptr;
    ID3DBlob* psLineBlob = nullptr;
    ID3DBlob* psColBlob = nullptr;
    ID3DBlob* psCheckerBlob = nullptr;
    ID3DBlob* psAnaglyphBlob = nullptr;
    bool ok = true;
    ok = ok && CompileShader(kCompositeVS,                 sizeof(kCompositeVS) - 1,                 "vs",        "vs_4_0", &vsBlob);
    ok = ok && CompileShader(kCompositePS_SBS,             sizeof(kCompositePS_SBS) - 1,             "sbs",       "ps_4_0", &psSbsBlob);
    ok = ok && CompileShader(kCompositePS_TB,              sizeof(kCompositePS_TB) - 1,              "tb",        "ps_4_0", &psTbBlob);
    ok = ok && CompileShader(kCompositePS_LineInterleaved, sizeof(kCompositePS_LineInterleaved) - 1, "line",      "ps_4_0", &psLineBlob);
    ok = ok && CompileShader(kCompositePS_ColInterleaved,  sizeof(kCompositePS_ColInterleaved) - 1,  "col",       "ps_4_0", &psColBlob);
    ok = ok && CompileShader(kCompositePS_Checkerboard,    sizeof(kCompositePS_Checkerboard) - 1,    "checker",   "ps_4_0", &psCheckerBlob);
    ok = ok && CompileShader(kCompositePS_Anaglyph,        sizeof(kCompositePS_Anaglyph) - 1,        "anaglyph",  "ps_4_0", &psAnaglyphBlob);
    if (!ok)
    {
        if (vsBlob)         vsBlob->Release();
        if (psSbsBlob)      psSbsBlob->Release();
        if (psTbBlob)       psTbBlob->Release();
        if (psLineBlob)     psLineBlob->Release();
        if (psColBlob)      psColBlob->Release();
        if (psCheckerBlob)  psCheckerBlob->Release();
        if (psAnaglyphBlob) psAnaglyphBlob->Release();
        return false;
    }

    HRESULT hr = S_OK;
    if (!m_compositeVS)
        hr = dev->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &m_compositeVS);
    if (SUCCEEDED(hr) && !m_compositePS_SBS)
        hr = dev->CreatePixelShader(psSbsBlob->GetBufferPointer(), psSbsBlob->GetBufferSize(), &m_compositePS_SBS);
    if (SUCCEEDED(hr) && !m_compositePS_TB)
        hr = dev->CreatePixelShader(psTbBlob->GetBufferPointer(), psTbBlob->GetBufferSize(), &m_compositePS_TB);
    if (SUCCEEDED(hr) && !m_compositePS_Line)
        hr = dev->CreatePixelShader(psLineBlob->GetBufferPointer(), psLineBlob->GetBufferSize(), &m_compositePS_Line);
    if (SUCCEEDED(hr) && !m_compositePS_Col)
        hr = dev->CreatePixelShader(psColBlob->GetBufferPointer(), psColBlob->GetBufferSize(), &m_compositePS_Col);
    if (SUCCEEDED(hr) && !m_compositePS_Checker)
        hr = dev->CreatePixelShader(psCheckerBlob->GetBufferPointer(), psCheckerBlob->GetBufferSize(), &m_compositePS_Checker);
    if (SUCCEEDED(hr) && !m_compositePS_Anaglyph)
        hr = dev->CreatePixelShader(psAnaglyphBlob->GetBufferPointer(), psAnaglyphBlob->GetBufferSize(), &m_compositePS_Anaglyph);
    vsBlob->Release(); psSbsBlob->Release(); psTbBlob->Release();
    psLineBlob->Release(); psColBlob->Release(); psCheckerBlob->Release(); psAnaglyphBlob->Release();
    if (FAILED(hr)) { ReleaseCompositePipeline(); return false; }

    if (!m_anaglyphCB)
    {
        D3D10_BUFFER_DESC bd = {};
        bd.ByteWidth      = 6 * 16; // 6 float4 rows
        bd.Usage          = D3D10_USAGE_DYNAMIC;
        bd.BindFlags      = D3D10_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D10_CPU_ACCESS_WRITE;
        dev->CreateBuffer(&bd, nullptr, &m_anaglyphCB);
    }

    if (!m_compositeSampler)
    {
        D3D10_SAMPLER_DESC sd = {};
        sd.Filter   = D3D10_FILTER_MIN_MAG_LINEAR_MIP_POINT;
        sd.AddressU = D3D10_TEXTURE_ADDRESS_CLAMP;
        sd.AddressV = D3D10_TEXTURE_ADDRESS_CLAMP;
        sd.AddressW = D3D10_TEXTURE_ADDRESS_CLAMP;
        sd.MinLOD = 0; sd.MaxLOD = D3D10_FLOAT32_MAX;
        dev->CreateSamplerState(&sd, &m_compositeSampler);
    }
    if (!m_compositeSamplerPoint)
    {
        D3D10_SAMPLER_DESC sd = {};
        sd.Filter   = D3D10_FILTER_MIN_MAG_MIP_POINT;
        sd.AddressU = D3D10_TEXTURE_ADDRESS_CLAMP;
        sd.AddressV = D3D10_TEXTURE_ADDRESS_CLAMP;
        sd.AddressW = D3D10_TEXTURE_ADDRESS_CLAMP;
        sd.MinLOD = 0; sd.MaxLOD = D3D10_FLOAT32_MAX;
        dev->CreateSamplerState(&sd, &m_compositeSamplerPoint);
    }
    if (!m_compositeRS)
    {
        D3D10_RASTERIZER_DESC rd = {};
        rd.FillMode = D3D10_FILL_SOLID;
        rd.CullMode = D3D10_CULL_NONE;
        rd.DepthClipEnable = TRUE;
        dev->CreateRasterizerState(&rd, &m_compositeRS);
    }
    if (!m_compositeBlend)
    {
        D3D10_BLEND_DESC bd = {};
        bd.RenderTargetWriteMask[0] = D3D10_COLOR_WRITE_ENABLE_ALL;
        dev->CreateBlendState(&bd, &m_compositeBlend);
    }
    if (!m_compositeDSS)
    {
        D3D10_DEPTH_STENCIL_DESC dsd = {};
        dsd.DepthEnable   = FALSE;
        dsd.StencilEnable = FALSE;
        dev->CreateDepthStencilState(&dsd, &m_compositeDSS);
    }
    bool full = m_compositeVS && m_compositePS_SBS && m_compositePS_TB &&
                m_compositePS_Line && m_compositePS_Col && m_compositePS_Checker &&
                m_compositePS_Anaglyph && m_anaglyphCB &&
                m_compositeSampler && m_compositeSamplerPoint &&
                m_compositeRS && m_compositeBlend && m_compositeDSS;
    LOG_VERBOSE("  d3d10 EnsureCompositeShaders: %s\n", full ? "OK" : "PARTIAL");
    return full;
}

// Mirror of the d3d11 blacklist. Keep aligned manually.
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

void SwapChainProxy::ReleaseSRPipeline()
{
    if (m_srSBSSRV) { m_srSBSSRV->Release(); m_srSBSSRV = nullptr; }
    if (m_srSBSRTV) { m_srSBSRTV->Release(); m_srSBSRTV = nullptr; }
    if (m_srSBSTex) { m_srSBSTex->Release(); m_srSBSTex = nullptr; }
    m_srSBSW = 0; m_srSBSH = 0; m_srSBSFmt = DXGI_FORMAT_UNKNOWN;

    if (m_srInterfaceDX10)
    {
        m_srInterfaceDX10->Delete();
        m_srInterfaceDX10 = nullptr;
    }
}

bool SwapChainProxy::EnsureSRSBSTexture()
{
    if (!m_parent) return false;
    ID3D10Device* dev = m_parent->GetReal();
    if (!dev) return false;
    if (m_logicalW == 0 || m_logicalH == 0) return false;

    UINT wantW = m_logicalW * 2;
    UINT wantH = m_logicalH;
    DXGI_FORMAT wantFmt = m_shadowFormat;
    if (m_srSBSTex && m_srSBSW == wantW && m_srSBSH == wantH && m_srSBSFmt == wantFmt)
        return true;

    if (m_srSBSSRV) { m_srSBSSRV->Release(); m_srSBSSRV = nullptr; }
    if (m_srSBSRTV) { m_srSBSRTV->Release(); m_srSBSRTV = nullptr; }
    if (m_srSBSTex) { m_srSBSTex->Release(); m_srSBSTex = nullptr; }

    D3D10_TEXTURE2D_DESC td = {};
    td.Width            = wantW;
    td.Height           = wantH;
    td.MipLevels        = 1;
    td.ArraySize        = 1;
    td.Format           = wantFmt;
    td.SampleDesc.Count = 1;
    td.Usage            = D3D10_USAGE_DEFAULT;
    td.BindFlags        = D3D10_BIND_RENDER_TARGET | D3D10_BIND_SHADER_RESOURCE;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &m_srSBSTex))) return false;

    D3D10_RENDER_TARGET_VIEW_DESC rtvd = {};
    rtvd.Format        = wantFmt;
    rtvd.ViewDimension = D3D10_RTV_DIMENSION_TEXTURE2D;
    if (FAILED(dev->CreateRenderTargetView(m_srSBSTex, &rtvd, &m_srSBSRTV))) return false;

    D3D10_SHADER_RESOURCE_VIEW_DESC srvd = {};
    srvd.Format              = wantFmt;
    srvd.ViewDimension       = D3D10_SRV_DIMENSION_TEXTURE2D;
    srvd.Texture2D.MipLevels = 1;
    if (FAILED(dev->CreateShaderResourceView(m_srSBSTex, &srvd, &m_srSBSSRV))) return false;

    m_srSBSW = wantW; m_srSBSH = wantH; m_srSBSFmt = wantFmt;

    // Bind SBS SRV to the SR interface once per texture creation (not per frame).
    // NOTE: SetInputTexture signature TBD — assuming DX11-like SRV input for now.
    if (m_srInterfaceDX10)
        m_srInterfaceDX10->SetInputTexture(m_srSBSSRV);

    return true;
}

bool SwapChainProxy::EnsureSRWeaver()
{
    if (m_srBlacklistedOrFailed) return false;
    if (m_srInterfaceDX10) return true;
    if (!m_parent || !m_real) return false;

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
                LOG_VERBOSE("  d3d10 EnsureSRWeaver: exe is SR-blacklisted but ForceSRWeave=1 — attempting anyway (diagnostic)\n");
                s_isBlacklisted = false;
            }
            else
            {
                LOG_VERBOSE("  d3d10 EnsureSRWeaver: exe is SR-blacklisted; falling back to SBS (set ForceSRWeave=1 to override)\n");
            }
        }
    }
    if (s_isBlacklisted) { m_srBlacklistedOrFailed = true; return false; }

    // HWND from the swap chain's output window.
    DXGI_SWAP_CHAIN_DESC scDesc = {};
    if (FAILED(m_real->GetDesc(&scDesc)))
    {
        m_srBlacklistedOrFailed = true;
        return false;
    }
    HWND hWnd = scDesc.OutputWindow;

    ID3D10Device* dev = m_parent->GetReal();
    if (!dev)
    { 
        m_srBlacklistedOrFailed = true; 
        return false; 
    }

    // SR-Lib: single-call init — creates SRContext + weaver internally.
    // NOTE: CreateSRInterfaceDX10 does not exist yet in SR-Lib — this is
    // a prototype mirroring the DX11 pattern. Signature assumed to match
    // DX11 (device, HWND, out-pointer).
    HRESULT hr = SimulatedReality::CreateSRInterfaceDX10(dev, hWnd, &m_srInterfaceDX10);
    if (FAILED(hr) || !m_srInterfaceDX10)
    {
        LOG_VERBOSE("  d3d10 EnsureSRWeaver: CreateSRInterfaceDX10 failed (hr=0x%08lX hWnd=%p dev=%p)\n",
                    hr, (void*)hWnd, (void*)dev);
        m_srBlacklistedOrFailed = true;
        return false;
    }

    LOG_VERBOSE("  d3d10 EnsureSRWeaver: ready (hWnd=%p srInterface=%p)\n",
                (void*)hWnd, (void*)m_srInterfaceDX10);
    return true;
}

bool SwapChainProxy::RunSRWeave()
{
    NVDM_TRACE_FIRST_N(5, "  d3d10 RunSRWeave: entry tid=%lu shaders=%d leftSRV=%p rightSRV=%p\n",
                       GetCurrentThreadId(),
                       (int)EnsureCompositeShaders(),
                       (void*)m_leftEyeSRV, (void*)m_rightEyeSRV);

    if (!EnsureCompositeShaders()) return false;
    if (!EnsureSRWeaver())         return false;
    if (!EnsureSRSBSTexture())     return false;
    if (!m_leftEyeSRV || !m_rightEyeSRV)
    {
        NVDM_TRACE_FIRST_N(5, "  d3d10 RunSRWeave: ABORT — eye SRVs not ready (L=%p R=%p)\n",
                           (void*)m_leftEyeSRV, (void*)m_rightEyeSRV);
        return false;
    }

    ID3D10Device* dev = m_parent ? m_parent->GetReal() : nullptr;
    if (!dev) return false;

    // Step A: render SBS composite into m_srSBSTex (2W × H) using existing SBS shader.
    bool swap = (NvDM_SwapEyes() != 0);
    ID3D10ShaderResourceView* leftSRV  = swap ? m_rightEyeSRV : m_leftEyeSRV;
    ID3D10ShaderResourceView* rightSRV = swap ? m_leftEyeSRV  : m_rightEyeSRV;
    ID3D10ShaderResourceView* srvs[2] = { leftSRV, rightSRV };

    D3D10_VIEWPORT vp = {};
    vp.TopLeftX = 0; vp.TopLeftY = 0;
    vp.Width    = m_srSBSW;
    vp.Height   = m_srSBSH;
    vp.MinDepth = 0; vp.MaxDepth = 1;

    dev->OMSetRenderTargets(1, &m_srSBSRTV, nullptr);
    dev->RSSetViewports(1, &vp);
    dev->RSSetState(m_compositeRS);
    float blendFactor[4] = { 0, 0, 0, 0 };
    dev->OMSetBlendState(m_compositeBlend, blendFactor, 0xFFFFFFFF);
    dev->OMSetDepthStencilState(m_compositeDSS, 0);
    dev->IASetInputLayout(nullptr);
    dev->IASetPrimitiveTopology(D3D10_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D10Buffer* nullVB = nullptr; UINT zero = 0;
    dev->IASetVertexBuffers(0, 1, &nullVB, &zero, &zero);
    dev->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
    dev->VSSetShader(m_compositeVS);
    dev->GSSetShader(nullptr);
    dev->PSSetShader(m_compositePS_SBS);
    dev->PSSetShaderResources(0, 2, srvs);
    dev->PSSetSamplers(0, 1, &m_compositeSampler);
    dev->Draw(3, 0);

    ID3D10ShaderResourceView* nullSRV[2] = { nullptr, nullptr };
    dev->PSSetShaderResources(0, 2, nullSRV);

    // Step B: bind real BB as RT and call Weave() — the SR-Lib interface
    // already has the SBS SRV from SetInputTexture (bound once in EnsureSRSBSTexture).
    if (!m_realBBRTV)
    {
        ID3D10Texture2D* realBB = nullptr;
        if (FAILED(m_real->GetBuffer(0, __uuidof(ID3D10Texture2D),
                                      reinterpret_cast<void**>(&realBB))) || !realBB)
            return false;
        HRESULT hr = dev->CreateRenderTargetView(realBB, nullptr, &m_realBBRTV);
        realBB->Release();
        if (FAILED(hr) || !m_realBBRTV) return false;
    }
    dev->OMSetRenderTargets(1, &m_realBBRTV, nullptr);
    D3D10_VIEWPORT vpBB = {};
    vpBB.Width    = m_logicalW;
    vpBB.Height   = m_logicalH;
    vpBB.MinDepth = 0; vpBB.MaxDepth = 1;
    dev->RSSetViewports(1, &vpBB);

    NVDM_TRACE_FIRST_N(5, "  d3d10 RunSRWeave: weave call - srInterface=%p SBS=%ux%u rtv=%p tid=%lu\n",
                       (void*)m_srInterfaceDX10, m_srSBSW, m_srSBSH,
                       (void*)m_realBBRTV, GetCurrentThreadId());
    m_srInterfaceDX10->Weave();
    NVDM_TRACE_FIRST_N(5, "  d3d10 RunSRWeave: weave returned OK\n");

    static volatile long s_weaveCount = 0;
    long n = _InterlockedIncrement(&s_weaveCount);
    if (n == 60 || n == 180 || n == 600 || n == 1800)
        NvDM_Log("  d3d10 RunSRWeave: heartbeat #%ld (SR still alive)\n", n);

    return true;
}

void SwapChainProxy::UpdateAnaglyphCB()
{
    if (!m_anaglyphCB) return;
    int colour = NvDM_AnaglyphColour();
    int method = NvDM_AnaglyphMethod();
    if (colour < 0 || colour > 2) colour = 0;
    if (method < 0 || method > 6) method = 0;
    const NvDirectMode::AnaglyphMatrix& m = NvDirectMode::kAnaglyphMatrices[colour][method];
    const float* rows[6] = { m.lR, m.lG, m.lB, m.rR, m.rG, m.rB };

    void* pData = nullptr;
    if (SUCCEEDED(m_anaglyphCB->Map(D3D10_MAP_WRITE_DISCARD, 0, &pData)) && pData)
    {
        float* p = static_cast<float*>(pData);
        for (int i = 0; i < 6; ++i)
        {
            p[i * 4 + 0] = rows[i][0];
            p[i * 4 + 1] = rows[i][1];
            p[i * 4 + 2] = rows[i][2];
            p[i * 4 + 3] = 0.0f;
        }
        m_anaglyphCB->Unmap();
    }
}

bool SwapChainProxy::RunCompositePass()
{
    if (!m_leftEyeFrame || !m_rightEyeFrame) return false;
    if (!EnsureCompositeShaders()) return false;

    // Mode 8 = SR weave. Try the LeiaSR pipeline first; if the runtime
    // is missing, blacklisted, or fails, fall through to the SBS shader
    // path below (modeTag will report "SBS (SR-fallback)" in that case).
    if (NvDM_OutputMode() == 8 && RunSRWeave())
        return true;

    ID3D10Device* dev = m_parent ? m_parent->GetReal() : nullptr;
    if (!dev) return false;

    if (!m_leftEyeSRV)
    {
        D3D10_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format              = m_shadowFormat;
        sd.ViewDimension       = D3D10_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        if (FAILED(dev->CreateShaderResourceView(m_leftEyeFrame, &sd, &m_leftEyeSRV))) return false;
    }
    if (!m_rightEyeSRV)
    {
        D3D10_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format              = m_shadowFormat;
        sd.ViewDimension       = D3D10_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        if (FAILED(dev->CreateShaderResourceView(m_rightEyeFrame, &sd, &m_rightEyeSRV))) return false;
    }
    if (!m_realBBRTV)
    {
        ID3D10Texture2D* realBB = nullptr;
        if (FAILED(m_real->GetBuffer(0, __uuidof(ID3D10Texture2D),
                                      reinterpret_cast<void**>(&realBB))) || !realBB)
            return false;
        HRESULT hr = dev->CreateRenderTargetView(realBB, nullptr, &m_realBBRTV);
        realBB->Release();
        if (FAILED(hr) || !m_realBBRTV) return false;
    }

    bool swap = (NvDM_SwapEyes() != 0);
    ID3D10ShaderResourceView* leftSRV  = swap ? m_rightEyeSRV : m_leftEyeSRV;
    ID3D10ShaderResourceView* rightSRV = swap ? m_leftEyeSRV  : m_rightEyeSRV;
    ID3D10ShaderResourceView* srvs[2] = { leftSRV, rightSRV };

    int mode = NvDM_OutputMode();
    ID3D10PixelShader* ps = m_compositePS_SBS;
    ID3D10SamplerState* sampler = m_compositeSampler;
    bool needsAnaglyphCB = false;
    const char* modeTag = "SBS";
    switch (mode)
    {
        case 0: case 3: ps = m_compositePS_TB;        modeTag = "T-B";          break;
        case 1: case 2: ps = m_compositePS_SBS;       modeTag = "SBS";          break;
        case 4:         ps = m_compositePS_Line;      modeTag = "LineInterl";   sampler = m_compositeSamplerPoint; break;
        case 5:         ps = m_compositePS_Col;       modeTag = "ColInterl";    sampler = m_compositeSamplerPoint; break;
        case 6:         ps = m_compositePS_Checker;   modeTag = "Checkerboard"; sampler = m_compositeSamplerPoint; break;
        case 7:         ps = m_compositePS_Anaglyph;  modeTag = "Anaglyph";     needsAnaglyphCB = true; break;
        case 8: /* SR weave NYI */
                        ps = m_compositePS_SBS;       modeTag = "SBS (SR-fallback)"; break;
        default:        ps = m_compositePS_SBS;       modeTag = "SBS (fallback)";    break;
    }
    if (needsAnaglyphCB) UpdateAnaglyphCB();

    D3D10_VIEWPORT vp = {};
    vp.Width  = m_logicalW;
    vp.Height = m_logicalH;
    vp.MinDepth = 0; vp.MaxDepth = 1;

    dev->OMSetRenderTargets(1, &m_realBBRTV, nullptr);
    dev->RSSetViewports(1, &vp);
    dev->RSSetState(m_compositeRS);
    float blendFactor[4] = { 0, 0, 0, 0 };
    dev->OMSetBlendState(m_compositeBlend, blendFactor, 0xFFFFFFFF);
    dev->OMSetDepthStencilState(m_compositeDSS, 0);
    dev->IASetInputLayout(nullptr);
    dev->IASetPrimitiveTopology(D3D10_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D10Buffer* nullVB = nullptr; UINT zero = 0;
    dev->IASetVertexBuffers(0, 1, &nullVB, &zero, &zero);
    dev->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
    dev->VSSetShader(m_compositeVS);
    dev->GSSetShader(nullptr);
    dev->PSSetShader(ps);
    dev->PSSetShaderResources(0, 2, srvs);
    dev->PSSetSamplers(0, 1, &sampler);
    if (needsAnaglyphCB) dev->PSSetConstantBuffers(2, 1, &m_anaglyphCB);
    dev->Draw(3, 0);

    ID3D10ShaderResourceView* nullSRV[2] = { nullptr, nullptr };
    dev->PSSetShaderResources(0, 2, nullSRV);
    if (needsAnaglyphCB)
    {
        ID3D10Buffer* nullCB = nullptr;
        dev->PSSetConstantBuffers(2, 1, &nullCB);
    }

    NVDM_TRACE_FIRST_N(4, "  d3d10 RunCompositePass: %s (mode=%d swap=%d) -> realBB rtv=%p\n",
                       modeTag, mode, (int)swap, m_realBBRTV);
    return true;
}

void SwapChainProxy::CaptureAndPresentBlit()
{
    if (!m_shadowBB || !m_real || !m_parent) return;

    int currentEye = NvDirectMode::GetActiveEye();
    if (currentEye == NvDirectMode::kEyeLeft || currentEye == NvDirectMode::kEyeRight)
    {
        CaptureEye(currentEye);
        m_lastSeenEye = currentEye;
    }

    if (RunCompositePass()) return;

    // Fallback: single-eye copy
    ID3D10Texture2D* realBB = nullptr;
    if (FAILED(m_real->GetBuffer(0, __uuidof(ID3D10Texture2D),
                                  reinterpret_cast<void**>(&realBB))) || !realBB)
        return;

    ID3D10Device* dev = m_parent->GetReal();
    bool swap = NvDM_SwapEyes() != 0;
    ID3D10Texture2D* leftSrc  = swap ? m_rightEyeFrame : m_leftEyeFrame;
    ID3D10Texture2D* rightSrc = swap ? m_leftEyeFrame  : m_rightEyeFrame;
    ID3D10Texture2D* displaySrc = leftSrc ? leftSrc : (rightSrc ? rightSrc : m_shadowBB);
    if (dev) dev->CopyResource(realBB, displaySrc);
    NVDM_TRACE_FIRST_N(4, "  d3d10 CaptureAndPresentBlit (fallback): currentEye=%d displaySrc=%s\n",
                       currentEye,
                       (displaySrc == m_leftEyeFrame  ? "leftEye"  :
                        displaySrc == m_rightEyeFrame ? "rightEye" : "shadow"));
    realBB->Release();
}

HRESULT STDMETHODCALLTYPE SwapChainProxy::Present(UINT SyncInterval, UINT Flags)
{
    NVDM_TRACE_FIRST_N(4, "  d3d10 SwapChainProxy::Present(SyncInterval=%u, Flags=0x%X)\n", SyncInterval, Flags);
    CaptureAndPresentBlit();
    return m_real->Present(SyncInterval, Flags);
}

HRESULT STDMETHODCALLTYPE SwapChainProxy::QueryInterface(REFIID riid, void** ppvObj)
{
    if (!ppvObj) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IDXGIObject ||
        riid == IID_IDXGIDeviceSubObject || riid == IID_IDXGISwapChain)
    {
        *ppvObj = static_cast<IDXGISwapChain*>(this);
        AddRef();
        return S_OK;
    }
    NVDM_TRACE_FIRST_N(8, "  d3d10/SwapChainProxy::QI(unknown/higher IID) -> E_NOINTERFACE\n");
    *ppvObj = nullptr;
    return E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE SwapChainProxy::GetBuffer(UINT Buffer, REFIID riid, void** ppSurface)
{
    if (Buffer == 0)
    {
        EnsureShadowBB();
        if (m_shadowBB && ppSurface)
        {
            HRESULT hr = m_shadowBB->QueryInterface(riid, ppSurface);
            if (SUCCEEDED(hr) && *ppSurface && m_parent)
            {
                m_parent->RegisterBackBufferTexture(*ppSurface);
                LOG_VERBOSE("  d3d10/SwapChainProxy::GetBuffer(0): handed shadow %p (as %p) on parent=%p\n",
                            m_shadowBB, *ppSurface, m_parent);
            }
            return hr;
        }
    }
    HRESULT hr = m_real->GetBuffer(Buffer, riid, ppSurface);
    NVDM_TRACE_FIRST_N(4, "  d3d10/SwapChainProxy::GetBuffer(idx=%u) hr=0x%08lX (passthrough)\n", Buffer, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE SwapChainProxy::ResizeBuffers(
    UINT BufferCount, UINT Width, UINT Height,
    DXGI_FORMAT NewFormat, UINT SwapChainFlags)
{
    LOG_VERBOSE("  d3d10/SwapChainProxy::ResizeBuffers(%ux%u, fmt=%d, flags=0x%X)\n",
                Width, Height, (int)NewFormat, SwapChainFlags);
    if (m_realBBRTV) { m_realBBRTV->Release(); m_realBBRTV = nullptr; }
    ReleaseEyeFrames();
    ReleaseShadowBB();
    return m_real->ResizeBuffers(BufferCount, Width, Height, NewFormat, SwapChainFlags);
}

} // namespace NvDirectMode
