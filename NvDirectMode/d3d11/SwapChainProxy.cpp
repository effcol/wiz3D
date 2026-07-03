#include "SwapChainProxy.h"
#include "Device11Proxy.h"
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

#pragma comment(lib, "dxguid.lib")  // for IID_IDXGISwapChain et al

// Output-mode + swap-eyes flags (from dllmain.cpp via the C-linkage bridge).
extern "C" int NvDM_OutputIsTopBottom();
extern "C" int NvDM_SwapEyes();
extern "C" int NvDM_OutputMode();
extern "C" int NvDM_AnaglyphColour();
extern "C" int NvDM_AnaglyphMethod();

// ---------------------------------------------------------------------------
// Stage 4b composite pipeline: D3DCompile resolver + HLSL source
// ---------------------------------------------------------------------------
namespace {

typedef HRESULT (WINAPI *PFN_D3DCompile)(
    LPCVOID pSrcData, SIZE_T SrcDataSize, LPCSTR pSourceName,
    const D3D_SHADER_MACRO* pDefines, ID3DInclude* pInclude,
    LPCSTR pEntrypoint, LPCSTR pTarget, UINT Flags1, UINT Flags2,
    ID3DBlob** ppCode, ID3DBlob** ppErrorMsgs);

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
    if (!h)
    {
        LOG_VERBOSE("  EnsureD3DCompile: no d3dcompiler_*.dll found — composite path disabled\n");
        return false;
    }
    g_pfnD3DCompile = (PFN_D3DCompile)GetProcAddress(h, "D3DCompile");
    if (!g_pfnD3DCompile)
        LOG_VERBOSE("  EnsureD3DCompile: GetProcAddress(D3DCompile) failed in %p\n", h);
    return g_pfnD3DCompile != nullptr;
}

// Fullscreen-triangle VS — no vertex buffer needed, uses SV_VertexID. id 0
// gives uv=(0,0), id 1 gives uv=(2,0), id 2 gives uv=(0,2). The triangle
// covers [-1,1]x[-1,1] in NDC; the half outside the screen is clipped.
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

// OutputMode 4 — Line/Row Interleaved (passive 3D TVs).
// Each eye is its own logical-size texture so the sample is straight uv;
// even rows display LEFT eye, odd rows display RIGHT. Use point sampling
// from the caller side so half-row blending doesn't smear adjacent eyes.
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

// OutputMode 5 — Column Interleaved (rare; some old polarised monitors).
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

// OutputMode 6 — Checkerboard (DLP 3D projectors). XOR of pixel x and y parity.
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

// OutputMode 7 — Anaglyph (colour-filtered glasses). Each output channel is a
// linear combination of left- and right-eye RGB selected by AnaglyphColour x
// AnaglyphMethod (Dubois / Compromise / Color / HalfColor / Optimised / Grey /
// True). Six float4 rows of coefficients are uploaded to b2 by UpdateAnaglyphCB
// every Present (cheap; the table itself is constant — only the active pair
// of rows changes when the user picks a different AnaglyphColour/Method).
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

// Anaglyph colour-method matrix table — shared across all 4 NvDirectMode
// proxies (d3d9/d3d10/d3d11/opengl32) via NvDirectMode/anaglyph_matrices.h.
// Aliased into the anonymous namespace so the matrix-upload code below
// reads `kAnaglyphMatrices[colour][method]` without a namespace prefix.
using NvDirectMode::AnaglyphMatrix;
using NvDirectMode::kAnaglyphMatrices;

bool CompileShader(const char* src, size_t len, const char* entry, const char* target,
                   ID3DBlob** out)
{
    if (!g_pfnD3DCompile) return false;
    ID3DBlob* err = nullptr;
    HRESULT hr = g_pfnD3DCompile(src, len, entry, nullptr, nullptr,
                                  "main", target, 0, 0, out, &err);
    if (err) { err->Release(); err = nullptr; }
    return SUCCEEDED(hr) && *out != nullptr;
}

} // anonymous namespace

namespace NvDirectMode
{

// ---------------------------------------------------------------------------
// Stage 4 primary-swap-chain registry: NvApiProxy's eye-change callback
// runs as plain C-linkage with no `this` context, so we keep a global
// pointer to the most recently created SwapChainProxy. The callback
// dispatches into that primary's CaptureEye(). For multi-swap-chain
// games (rare), only the primary captures; secondary swap chains operate
// without per-eye capture (display whichever eye was last rendered, same
// as stage 3 v2 behaviour).
// ---------------------------------------------------------------------------
namespace
{
    SwapChainProxy* g_primarySwapChain = nullptr;
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
        // Skip SetActiveEye-driven capture if magic-header SBS path is live —
        // eye textures are populated from the SBS split there instead.
        if (g_primarySwapChain && !g_primarySwapChain->IsMagicHeaderActive())
            g_primarySwapChain->CaptureEye(oldEye);
        LeaveCriticalSection(&g_primaryLock);
    }
}

SwapChainProxy* SwapChainProxy::GetPrimary()
{
    EnsurePrimaryLock();
    EnterCriticalSection(&g_primaryLock);
    SwapChainProxy* p = g_primarySwapChain;
    LeaveCriticalSection(&g_primaryLock);
    return p;
}

SwapChainProxy::SwapChainProxy(IDXGISwapChain* real, Device11Proxy* parent)
    : m_real(real)
    , m_real1(nullptr)
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
    , m_srInterfaceDX11(nullptr)
    , m_srSBSTex(nullptr)
    , m_srSBSRTV(nullptr)
    , m_srSBSSRV(nullptr)
    , m_srSBSW(0)
    , m_srSBSH(0)
    , m_srSBSFmt(DXGI_FORMAT_UNKNOWN)
{
    if (m_real)
        m_real->QueryInterface(IID_IDXGISwapChain1, reinterpret_cast<void**>(&m_real1));

    // Register as the primary swap chain (replaces any previous).
    EnsurePrimaryLock();
    EnterCriticalSection(&g_primaryLock);
    g_primarySwapChain = this;
    LeaveCriticalSection(&g_primaryLock);

    // Lazily register the eye-change handler with NvApiProxy. Idempotent
    // — the eye_state module remembers we've registered.
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
}

void SwapChainProxy::ReleaseSRPipeline()
{
    if (m_srSBSSRV)        { m_srSBSSRV->Release();        m_srSBSSRV = nullptr; }
    if (m_srSBSRTV)        { m_srSBSRTV->Release();        m_srSBSRTV = nullptr; }
    if (m_srSBSTex)        { m_srSBSTex->Release();        m_srSBSTex = nullptr; }
    m_srSBSW = 0; m_srSBSH = 0; m_srSBSFmt = DXGI_FORMAT_UNKNOWN;

    if (m_srInterfaceDX11)
    {
        m_srInterfaceDX11->Delete();
        m_srInterfaceDX11 = nullptr;
    }
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

void SwapChainProxy::ReleaseShadowBB()
{
    if (m_shadowBB) { m_shadowBB->Release(); m_shadowBB = nullptr; }
    m_logicalW = 0;
    m_logicalH = 0;
    m_shadowFormat = DXGI_FORMAT_UNKNOWN;
}

void SwapChainProxy::ReleaseEyeFrames()
{
    // SRVs hold a ref on the underlying texture — release them first so
    // the texture refcount decrements correctly when we Release the slot.
    if (m_leftEyeSRV)    { m_leftEyeSRV->Release();    m_leftEyeSRV = nullptr; }
    if (m_rightEyeSRV)   { m_rightEyeSRV->Release();   m_rightEyeSRV = nullptr; }
    if (m_leftEyeFrame)  { m_leftEyeFrame->Release();  m_leftEyeFrame = nullptr; }
    if (m_rightEyeFrame) { m_rightEyeFrame->Release(); m_rightEyeFrame = nullptr; }
    m_lastSeenEye = NvDirectMode::kEyeMono;
}

void SwapChainProxy::EnsureEyeFrames()
{
    // Stage 4 v1.1: this function is now a no-op intentionally — eye
    // frames are allocated lazily *per eye* in CaptureEye(). Eager-
    // allocating both meant null != never-captured (it meant null !=
    // never-allocated), so a SwapEyes=1 display would end up copying
    // an empty-but-allocated right-eye texture to the real BB and
    // showing a black screen until the right-eye actually got captured.
}

void SwapChainProxy::CaptureEye(int eyeBeingLeft)
{
    if (!m_shadowBB || !m_parent) return;

    ID3D11Texture2D** slot = nullptr;
    if      (eyeBeingLeft == NvDirectMode::kEyeLeft)  slot = &m_leftEyeFrame;
    else if (eyeBeingLeft == NvDirectMode::kEyeRight) slot = &m_rightEyeFrame;
    else return; // MONO transitions don't need capturing — first real eye render starts fresh

    // Lazy per-eye allocation: only create this eye's texture on its
    // first capture. Means a non-null slot pointer means "this eye has
    // actual game-rendered content in it", which the display logic
    // relies on.
    if (!*slot)
    {
        ID3D11Device* dev = m_parent->GetReal();
        if (!dev) return;
        D3D11_TEXTURE2D_DESC td = {};
        td.Width            = m_logicalW;
        td.Height           = m_logicalH;
        td.MipLevels        = 1;
        td.ArraySize        = 1;
        td.Format           = m_shadowFormat;
        td.SampleDesc.Count = 1;
        td.Usage            = D3D11_USAGE_DEFAULT;
        td.BindFlags        = D3D11_BIND_SHADER_RESOURCE;
        HRESULT hr = dev->CreateTexture2D(&td, nullptr, slot);
        if (FAILED(hr) || !*slot)
        {
            LOG_VERBOSE("  CaptureEye(%d): per-eye CreateTexture2D FAILED hr=0x%08lX\n",
                        eyeBeingLeft, hr);
            return;
        }
        LOG_VERBOSE("  CaptureEye(%d): allocated eye texture=%p (%ux%u fmt=%d)\n",
                    eyeBeingLeft, *slot, m_logicalW, m_logicalH, (int)m_shadowFormat);
    }

    ID3D11DeviceContext* ctx = nullptr;
    if (m_parent->GetReal()) m_parent->GetReal()->GetImmediateContext(&ctx);
    if (ctx)
    {
        ctx->CopyResource(*slot, m_shadowBB);
        ctx->Release();
    }
    NVDM_TRACE_FIRST_N(8, "  CaptureEye(eye=%d): copied shadow=%p -> eyeFrame=%p\n",
                       eyeBeingLeft, m_shadowBB, *slot);
}

void SwapChainProxy::CaptureMagicHeaderSBS(ID3D11DeviceContext* pContext,
                                           ID3D11Resource* pSrc,
                                           UINT eyeWidth, UINT eyeHeight,
                                           bool swapEyes)
{
    if (!pContext || !pSrc || !m_parent) return;
    ID3D11Device* dev = m_parent->GetReal();
    if (!dev) return;

    // Pull source desc for Format / SampleDesc.
    ID3D11Texture2D* srcTex = nullptr;
    if (FAILED(pSrc->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&srcTex)) || !srcTex)
        return;
    D3D11_TEXTURE2D_DESC srcDesc = {};
    srcTex->GetDesc(&srcDesc);
    srcTex->Release();

    // (Re)allocate eye textures if missing or wrong size/format.
    for (int e = 0; e < 2; ++e)
    {
        ID3D11Texture2D** slot = (e == 0) ? &m_leftEyeFrame : &m_rightEyeFrame;
        if (*slot)
        {
            D3D11_TEXTURE2D_DESC d = {};
            (*slot)->GetDesc(&d);
            if (d.Width != eyeWidth || d.Height != eyeHeight || d.Format != srcDesc.Format)
            {
                (*slot)->Release();
                *slot = nullptr;
            }
        }
        if (!*slot)
        {
            D3D11_TEXTURE2D_DESC td = {};
            td.Width            = eyeWidth;
            td.Height           = eyeHeight;
            td.MipLevels        = 1;
            td.ArraySize        = 1;
            td.Format           = srcDesc.Format;
            td.SampleDesc.Count = 1;
            td.Usage            = D3D11_USAGE_DEFAULT;
            td.BindFlags        = D3D11_BIND_SHADER_RESOURCE;
            if (FAILED(dev->CreateTexture2D(&td, nullptr, slot)) || !*slot)
            {
                LOG_VERBOSE("  d3d11 magic-header: eye tex alloc FAILED (%ux%u)\n",
                            eyeWidth, eyeHeight);
                return;
            }
            LOG_VERBOSE("  d3d11 magic-header: alloc eyeFrame[%d]=%p (%ux%u)\n",
                        e, *slot, eyeWidth, eyeHeight);
        }
    }

    D3D11_BOX leftBox  = { 0,         0, 0, eyeWidth,     eyeHeight, 1 };
    D3D11_BOX rightBox = { eyeWidth,  0, 0, eyeWidth * 2, eyeHeight, 1 };
    ID3D11Texture2D* leftOnto  = swapEyes ? m_rightEyeFrame : m_leftEyeFrame;
    ID3D11Texture2D* rightOnto = swapEyes ? m_leftEyeFrame  : m_rightEyeFrame;
    pContext->CopySubresourceRegion(leftOnto,  0, 0, 0, 0, pSrc, 0, &leftBox);
    pContext->CopySubresourceRegion(rightOnto, 0, 0, 0, 0, pSrc, 0, &rightBox);

    if (!m_magicHeaderActive)
    {
        m_magicHeaderActive = true;
        m_logicalW = eyeWidth;
        m_logicalH = eyeHeight;
        m_shadowFormat = srcDesc.Format;
        LOG_VERBOSE("  d3d11 magic-header: ACTIVE (%ux%u eye, swap=%d)\n",
                    eyeWidth, eyeHeight, swapEyes);
    }
}

void SwapChainProxy::EnsureShadowBB()
{
    if (m_shadowBB || !m_real || !m_parent) return;

    // Stage 3 v2: shadow allocated at the *logical* (one-eye) size.
    // Previous v1 doubled the shadow to provide per-eye viewport routing
    // inside one texture, but that broke games whose deferred rendering
    // queries the BB texture's GetDesc and sizes G-buffers / post-process
    // RTs accordingly — TR's "shaders broken with line down the middle"
    // was exactly this (game allocated 7680-wide G-buffer but our viewport
    // clamp restricted draws to the LEFT half, leaving the RIGHT half as
    // sampled garbage for the post-process pass).
    //
    // With a 1x shadow the game sees a normal-sized BB everywhere
    // (swap chain GetDesc, texture GetDesc, all consistent). For mono
    // games this Just Works. For genuine Direct Mode games the latest-
    // rendered eye lands in the shadow each frame and we blit it to the
    // real BB — a shutter-glasses-style flicker on a 2D display, but it
    // IS correct stereo output. Per-eye CAPTURE for proper SBS/T-B
    // composite is stage 4.
    DXGI_SWAP_CHAIN_DESC desc = {};
    if (FAILED(m_real->GetDesc(&desc))) return;
    m_logicalW = desc.BufferDesc.Width;
    m_logicalH = desc.BufferDesc.Height;
    m_shadowFormat = desc.BufferDesc.Format;
    if (m_logicalW == 0 || m_logicalH == 0)
    {
        LOG_VERBOSE("  EnsureShadowBB: degenerate logical size %ux%u — skipping shadow alloc\n",
                    m_logicalW, m_logicalH);
        return;
    }

    m_parent->SetLogicalBackBufferSize(m_logicalW, m_logicalH);

    ID3D11Device* dev = m_parent->GetReal();
    if (!dev) return;

    D3D11_TEXTURE2D_DESC td = {};
    td.Width            = m_logicalW;
    td.Height           = m_logicalH;
    td.MipLevels        = 1;
    td.ArraySize        = 1;
    td.Format           = m_shadowFormat;
    td.SampleDesc.Count = 1;
    td.SampleDesc.Quality = 0;
    td.Usage            = D3D11_USAGE_DEFAULT;
    td.BindFlags        = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    HRESULT hr = dev->CreateTexture2D(&td, nullptr, &m_shadowBB);
    if (FAILED(hr) || !m_shadowBB)
    {
        LOG_VERBOSE("  EnsureShadowBB: CreateTexture2D(%ux%u fmt=%d) FAILED hr=0x%08lX\n",
                    m_logicalW, m_logicalH, (int)m_shadowFormat, hr);
        m_shadowBB = nullptr;
        return;
    }
    LOG_VERBOSE("  EnsureShadowBB: shadow=%p (%ux%u 1x logical, fmt=%d)\n",
                m_shadowBB, m_logicalW, m_logicalH, (int)m_shadowFormat);
}

HRESULT STDMETHODCALLTYPE SwapChainProxy::QueryInterface(REFIID riid, void** ppvObj)
{
    if (!ppvObj) return E_POINTER;
    if (riid == IID_IUnknown ||
        riid == IID_IDXGIObject ||
        riid == IID_IDXGIDeviceSubObject ||
        riid == IID_IDXGISwapChain)
    {
        *ppvObj = static_cast<IDXGISwapChain*>(this);
        AddRef();
        return S_OK;
    }
    if (riid == IID_IDXGISwapChain1 && m_real1)
    {
        *ppvObj = static_cast<IDXGISwapChain1*>(this);
        AddRef();
        return S_OK;
    }
    NVDM_TRACE_FIRST_N(8, "  SwapChainProxy::QI(unknown/higher IID) -> E_NOINTERFACE\n");
    *ppvObj = nullptr;
    return E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE SwapChainProxy::Present(UINT SyncInterval, UINT Flags)
{
    NVDM_TRACE_FIRST_N(4, "  SwapChainProxy::Present(SyncInterval=%u, Flags=0x%X)\n", SyncInterval, Flags);
    CaptureAndPresentBlit();
    return m_real->Present(SyncInterval, Flags);
}

HRESULT STDMETHODCALLTYPE SwapChainProxy::Present1(UINT SyncInterval, UINT Flags,
                                                   const DXGI_PRESENT_PARAMETERS* pPresentParameters)
{
    NVDM_TRACE_FIRST_N(4, "  SwapChainProxy::Present1(SyncInterval=%u, Flags=0x%X)\n", SyncInterval, Flags);
    if (!m_real1) return E_NOINTERFACE;
    CaptureAndPresentBlit();
    return m_real1->Present1(SyncInterval, Flags, pPresentParameters);
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

    ID3D11Device* dev = m_parent->GetReal();
    if (!dev) return false;

    ID3DBlob* vsBlob = nullptr;
    ID3DBlob* psSbsBlob = nullptr;
    ID3DBlob* psTbBlob = nullptr;
    ID3DBlob* psLineBlob = nullptr;
    ID3DBlob* psColBlob = nullptr;
    ID3DBlob* psCheckerBlob = nullptr;
    ID3DBlob* psAnaglyphBlob = nullptr;
    bool ok = true;
    ok = ok && CompileShader(kCompositeVS,                sizeof(kCompositeVS) - 1,                "vs", "vs_4_0", &vsBlob);
    ok = ok && CompileShader(kCompositePS_SBS,            sizeof(kCompositePS_SBS) - 1,            "ps", "ps_4_0", &psSbsBlob);
    ok = ok && CompileShader(kCompositePS_TB,             sizeof(kCompositePS_TB) - 1,             "ps", "ps_4_0", &psTbBlob);
    ok = ok && CompileShader(kCompositePS_LineInterleaved,sizeof(kCompositePS_LineInterleaved) - 1,"ps", "ps_4_0", &psLineBlob);
    ok = ok && CompileShader(kCompositePS_ColInterleaved, sizeof(kCompositePS_ColInterleaved) - 1, "ps", "ps_4_0", &psColBlob);
    ok = ok && CompileShader(kCompositePS_Checkerboard,   sizeof(kCompositePS_Checkerboard) - 1,   "ps", "ps_4_0", &psCheckerBlob);
    ok = ok && CompileShader(kCompositePS_Anaglyph,       sizeof(kCompositePS_Anaglyph) - 1,       "ps", "ps_4_0", &psAnaglyphBlob);
    if (!ok)
    {
        LOG_VERBOSE("  EnsureCompositeShaders: D3DCompile failed\n");
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
        hr = dev->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &m_compositeVS);
    if (SUCCEEDED(hr) && !m_compositePS_SBS)
        hr = dev->CreatePixelShader(psSbsBlob->GetBufferPointer(), psSbsBlob->GetBufferSize(), nullptr, &m_compositePS_SBS);
    if (SUCCEEDED(hr) && !m_compositePS_TB)
        hr = dev->CreatePixelShader(psTbBlob->GetBufferPointer(), psTbBlob->GetBufferSize(), nullptr, &m_compositePS_TB);
    if (SUCCEEDED(hr) && !m_compositePS_Line)
        hr = dev->CreatePixelShader(psLineBlob->GetBufferPointer(), psLineBlob->GetBufferSize(), nullptr, &m_compositePS_Line);
    if (SUCCEEDED(hr) && !m_compositePS_Col)
        hr = dev->CreatePixelShader(psColBlob->GetBufferPointer(), psColBlob->GetBufferSize(), nullptr, &m_compositePS_Col);
    if (SUCCEEDED(hr) && !m_compositePS_Checker)
        hr = dev->CreatePixelShader(psCheckerBlob->GetBufferPointer(), psCheckerBlob->GetBufferSize(), nullptr, &m_compositePS_Checker);
    if (SUCCEEDED(hr) && !m_compositePS_Anaglyph)
        hr = dev->CreatePixelShader(psAnaglyphBlob->GetBufferPointer(), psAnaglyphBlob->GetBufferSize(), nullptr, &m_compositePS_Anaglyph);
    vsBlob->Release(); psSbsBlob->Release(); psTbBlob->Release();
    psLineBlob->Release(); psColBlob->Release(); psCheckerBlob->Release(); psAnaglyphBlob->Release();
    if (FAILED(hr)) { ReleaseCompositePipeline(); return false; }

    if (!m_anaglyphCB)
    {
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth      = 6 * 16; // 6 float4 rows
        bd.Usage          = D3D11_USAGE_DYNAMIC;
        bd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        dev->CreateBuffer(&bd, nullptr, &m_anaglyphCB);
    }

    if (!m_compositeSampler)
    {
        D3D11_SAMPLER_DESC sd = {};
        sd.Filter   = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
        sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MinLOD   = 0;
        sd.MaxLOD   = D3D11_FLOAT32_MAX;
        dev->CreateSamplerState(&sd, &m_compositeSampler);
    }
    if (!m_compositeSamplerPoint)
    {
        D3D11_SAMPLER_DESC sd = {};
        sd.Filter   = D3D11_FILTER_MIN_MAG_MIP_POINT;
        sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MinLOD   = 0;
        sd.MaxLOD   = D3D11_FLOAT32_MAX;
        dev->CreateSamplerState(&sd, &m_compositeSamplerPoint);
    }
    if (!m_compositeRS)
    {
        D3D11_RASTERIZER_DESC rd = {};
        rd.FillMode        = D3D11_FILL_SOLID;
        rd.CullMode        = D3D11_CULL_NONE;
        rd.DepthClipEnable = TRUE;
        dev->CreateRasterizerState(&rd, &m_compositeRS);
    }
    if (!m_compositeBlend)
    {
        D3D11_BLEND_DESC bd = {};
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        dev->CreateBlendState(&bd, &m_compositeBlend);
    }
    if (!m_compositeDSS)
    {
        D3D11_DEPTH_STENCIL_DESC dsd = {};
        dsd.DepthEnable   = FALSE;
        dsd.StencilEnable = FALSE;
        dev->CreateDepthStencilState(&dsd, &m_compositeDSS);
    }

    bool full = m_compositeVS && m_compositePS_SBS && m_compositePS_TB &&
                m_compositePS_Line && m_compositePS_Col && m_compositePS_Checker &&
                m_compositePS_Anaglyph && m_anaglyphCB &&
                m_compositeSampler && m_compositeSamplerPoint &&
                m_compositeRS && m_compositeBlend && m_compositeDSS;
    LOG_VERBOSE("  EnsureCompositeShaders: %s\n", full ? "OK" : "PARTIAL");
    return full;
}

// Some games crash if the SR runtime DLLs are loaded into their process.
// Blacklist them here so SR mode downgrades to SBS without ever touching
// the runtime. AmdQbProxy maintains an identical list for the same reason
// — TR2013 is the canonical example: the screen's microlens engages on
// weave(), then the SR runtime's async camera/eye-tracker subsystem
// spawns its threads and something inside TR (anti-tamper, EOS Overlay
// injection, or one of TR's other injected DLLs) doesn't tolerate them.
// AmdQbProxy hit the same wall first and accepted it; we follow.
// If a game turns out to crash only when actually entering SR weave,
// add its exe name here.
static bool IsSRIncompatibleExe()
{
    wchar_t exePath[MAX_PATH] = {};
    if (!GetModuleFileNameW(nullptr, exePath, MAX_PATH)) return false;
    for (wchar_t* p = exePath; *p; ++p) *p = (wchar_t)towlower(*p);
    static const wchar_t* const kBlacklist[] = {
        // TR2013: deep-diagnosed; the crash is inside CreateDX11Weaver itself,
        // BEFORE setWindowHandle or ctx->initialize ever run. EOSOVH (Epic
        // overlay) is in TR's process from its static EOSSDK import and hooks
        // something the SR weaver constructor touches. Neither the two-step
        // HWND-defer pattern nor reordering ctx->initialize() helped — both
        // landed on the same EOSOVH+0x18060 strlen-loop crash. SBS fallback
        // was the answer until/unless we neutralise the overlay itself.
        // RESOLVED 2026-06-26: EOSSDK is a static import and loads the overlay
        // wiz3D-proxy/EOSStub project (a hard-stub EOSSDK whose exports all
        // return 0) disables EOS - and thus the overlay - entirely. Confirmed
        // in-game: with the stub in the game folder, TR runs SR weave on an SR
        // device, no crash. This entry STAYS as the safe SBS default for users
        // without the stub; the SR path is opt-in via EOSStub + ForceSRWeave=1.
        // See project_tr2013_sr_dead_end / project_eosstub memories.
        L"tombraider.exe",
    };
    for (auto entry : kBlacklist)
        if (wcsstr(exePath, entry)) return true;
    return false;
}

bool SwapChainProxy::EnsureSRSBSTexture()
{
    if (!m_parent) return false;
    ID3D11Device* dev = m_parent->GetReal();
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

    D3D11_TEXTURE2D_DESC td = {};
    td.Width            = wantW;
    td.Height           = wantH;
    td.MipLevels        = 1;
    td.ArraySize        = 1;
    td.Format           = wantFmt;
    td.SampleDesc.Count = 1;
    td.Usage            = D3D11_USAGE_DEFAULT;
    td.BindFlags        = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &m_srSBSTex))) return false;

    D3D11_RENDER_TARGET_VIEW_DESC rtvd = {};
    rtvd.Format        = wantFmt;
    rtvd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    if (FAILED(dev->CreateRenderTargetView(m_srSBSTex, &rtvd, &m_srSBSRTV))) return false;

    D3D11_SHADER_RESOURCE_VIEW_DESC srvd = {};
    srvd.Format              = wantFmt;
    srvd.ViewDimension       = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvd.Texture2D.MipLevels = 1;
    if (FAILED(dev->CreateShaderResourceView(m_srSBSTex, &srvd, &m_srSBSSRV))) return false;

    m_srSBSW = wantW; m_srSBSH = wantH; m_srSBSFmt = wantFmt;

    // Bind SBS SRV to the SR interface once per texture creation (not per frame).
    if (m_srInterfaceDX11)
        m_srInterfaceDX11->SetInputTexture(m_srSBSSRV);

    return true;
}

bool SwapChainProxy::EnsureSRWeaver()
{
    if (m_srBlacklistedOrFailed) return false;
    if (m_srInterfaceDX11) return true;
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
                LOG_VERBOSE("  d3d11 EnsureSRWeaver: exe is SR-blacklisted but ForceSRWeave=1 — attempting anyway (diagnostic)\n");
                s_isBlacklisted = false;
            }
            else
            {
                LOG_VERBOSE("  d3d11 EnsureSRWeaver: exe is SR-blacklisted; falling back to SBS (set ForceSRWeave=1 to override)\n");
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

    ID3D11Device* dev = m_parent->GetReal();
    if (!dev) 
    {
        m_srBlacklistedOrFailed = true; 
        return false; 
    }
    ID3D11DeviceContext* immCtx = nullptr;
    dev->GetImmediateContext(&immCtx);
    if (!immCtx)
    {
        m_srBlacklistedOrFailed = true;
        return false; 
    }

    // SR-Lib: single-call init — creates SRContext + weaver internally.
    // Returns E_NOINTERFACE when the delay-loaded SR runtime DLLs are
    // absent, or a failure HRESULT if no SR display is connected.
    SimulatedReality::SRInterfaceDX11* srInterface = nullptr;
    HRESULT hr = SimulatedReality::CreateSRInterfaceDX11(immCtx, hWnd, &srInterface);
    immCtx->Release();
    if (FAILED(hr) || !srInterface)
    {
        LOG_VERBOSE("  d3d11 EnsureSRWeaver: CreateSRInterfaceDX11 failed (hr=0x%08lX hWnd=%p)\n",
                    hr, (void*)hWnd);
        m_srBlacklistedOrFailed = true;
        return false;
    }

    m_srInterfaceDX11 = srInterface;
    NvDM_Log("  d3d11 EnsureSRWeaver: ready (hWnd=%p srInterface=%p)\n",
             (void*)hWnd, (void*)srInterface);
    return true;
}

bool SwapChainProxy::RunSRWeave()
{
    NVDM_TRACE_FIRST_N(5, "  d3d11 RunSRWeave: entry tid=%lu shaders=%d leftSRV=%p rightSRV=%p\n",
                       GetCurrentThreadId(),
                       (int)EnsureCompositeShaders(),
                       (void*)m_leftEyeSRV, (void*)m_rightEyeSRV);

    if (!EnsureCompositeShaders()) return false;
    if (!EnsureSRWeaver())         return false;
    if (!EnsureSRSBSTexture())     return false;
    if (!m_leftEyeSRV || !m_rightEyeSRV)
    {
        NVDM_TRACE_FIRST_N(5, "  d3d11 RunSRWeave: ABORT — eye SRVs not ready (L=%p R=%p)\n",
                           (void*)m_leftEyeSRV, (void*)m_rightEyeSRV);
        return false;
    }

    ID3D11Device* dev = m_parent ? m_parent->GetReal() : nullptr;
    if (!dev) return false;
    ID3D11DeviceContext* ctx = nullptr;
    dev->GetImmediateContext(&ctx);
    if (!ctx) return false;

    // ---- Step A: render the SBS composite into m_srSBSTex (2W × H) using
    //              the existing SBS pixel shader.
    bool swap = (NvDM_SwapEyes() != 0);
    ID3D11ShaderResourceView* leftSRV  = swap ? m_rightEyeSRV : m_leftEyeSRV;
    ID3D11ShaderResourceView* rightSRV = swap ? m_leftEyeSRV  : m_rightEyeSRV;
    ID3D11ShaderResourceView* srvs[2] = { leftSRV, rightSRV };

    D3D11_VIEWPORT vp = {};
    vp.TopLeftX = 0; vp.TopLeftY = 0;
    vp.Width    = (FLOAT)m_srSBSW;
    vp.Height   = (FLOAT)m_srSBSH;
    vp.MinDepth = 0; vp.MaxDepth = 1;

    ctx->OMSetRenderTargets(1, &m_srSBSRTV, nullptr);
    ctx->RSSetViewports(1, &vp);
    ctx->RSSetState(m_compositeRS);
    ctx->OMSetBlendState(m_compositeBlend, nullptr, 0xFFFFFFFF);
    ctx->OMSetDepthStencilState(m_compositeDSS, 0);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11Buffer* nullVB = nullptr; UINT zero = 0;
    ctx->IASetVertexBuffers(0, 1, &nullVB, &zero, &zero);
    ctx->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
    ctx->VSSetShader(m_compositeVS, nullptr, 0);
    ctx->GSSetShader(nullptr, nullptr, 0);
    ctx->HSSetShader(nullptr, nullptr, 0);
    ctx->DSSetShader(nullptr, nullptr, 0);
    ctx->PSSetShader(m_compositePS_SBS, nullptr, 0);
    ctx->PSSetShaderResources(0, 2, srvs);
    ctx->PSSetSamplers(0, 1, &m_compositeSampler);
    ctx->Draw(3, 0);

    // Detach inputs so the next pass doesn't read from a stale SRV.
    ID3D11ShaderResourceView* nullSRV[2] = { nullptr, nullptr };
    ctx->PSSetShaderResources(0, 2, nullSRV);

    // ---- Step B: bind real BB as RT and call Weave() — the SR-Lib
    //              interface already has the SBS SRV from SetInputTexture
    //              (bound once in EnsureSRSBSTexture).
    if (!m_realBBRTV)
    {
        ID3D11Texture2D* realBB = nullptr;
        if (FAILED(m_real->GetBuffer(0, IID_ID3D11Texture2D,
                                      reinterpret_cast<void**>(&realBB))) || !realBB)
        { ctx->Release(); return false; }
        HRESULT hr = dev->CreateRenderTargetView(realBB, nullptr, &m_realBBRTV);
        realBB->Release();
        if (FAILED(hr) || !m_realBBRTV) { ctx->Release(); return false; }
    }
    ctx->OMSetRenderTargets(1, &m_realBBRTV, nullptr);
    D3D11_VIEWPORT vpBB = {};
    vpBB.Width    = (FLOAT)m_logicalW;
    vpBB.Height   = (FLOAT)m_logicalH;
    vpBB.MinDepth = 0; vpBB.MaxDepth = 1;
    ctx->RSSetViewports(1, &vpBB);

    NVDM_TRACE_FIRST_N(5, "  d3d11 RunSRWeave: weave call - srInterface=%p SBS=%ux%u rtv=%p tid=%lu\n",
                       (void*)m_srInterfaceDX11, m_srSBSW, m_srSBSH,
                       (void*)m_realBBRTV, GetCurrentThreadId());
    m_srInterfaceDX11->Weave();
    NVDM_TRACE_FIRST_N(5, "  d3d11 RunSRWeave: weave returned OK\n");

    // Lifetime heartbeat — log every Nth weave so we can see how long SR
    // stayed alive before the crash. Cheap (just atomic inc + modulo).
    static volatile long s_weaveCount = 0;
    long n = _InterlockedIncrement(&s_weaveCount);
    if (n == 60 || n == 180 || n == 600 || n == 1800)
        NvDM_Log("  d3d11 RunSRWeave: heartbeat #%ld (SR still alive)\n", n);

    ctx->Release();
    return true;
}

void SwapChainProxy::UpdateAnaglyphCB()
{
    if (!m_anaglyphCB || !m_parent) return;
    ID3D11Device* dev = m_parent->GetReal();
    if (!dev) return;
    ID3D11DeviceContext* ctx = nullptr;
    dev->GetImmediateContext(&ctx);
    if (!ctx) return;

    int colour = NvDM_AnaglyphColour();
    int method = NvDM_AnaglyphMethod();
    if (colour < 0 || colour > 2) colour = 0;
    if (method < 0 || method > 6) method = 0;
    const AnaglyphMatrix& m = kAnaglyphMatrices[colour][method];

    // SwapEyes is already applied at the SRV-binding level (left/right SRVs
    // are swapped at slot 0/1 in RunCompositePass), so the matrix rows go
    // through unchanged here.
    const float* rows[6] = { m.lR, m.lG, m.lB, m.rR, m.rG, m.rB };

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (SUCCEEDED(ctx->Map(m_anaglyphCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)) && mapped.pData)
    {
        float* p = static_cast<float*>(mapped.pData);
        for (int i = 0; i < 6; ++i)
        {
            p[i * 4 + 0] = rows[i][0];
            p[i * 4 + 1] = rows[i][1];
            p[i * 4 + 2] = rows[i][2];
            p[i * 4 + 3] = 0.0f;
        }
        ctx->Unmap(m_anaglyphCB, 0);
    }
    ctx->Release();
}

bool SwapChainProxy::RunCompositePass()
{
    // Need both eyes captured + shaders + RTV available.
    if (!m_leftEyeFrame || !m_rightEyeFrame) return false;
    if (!EnsureCompositeShaders()) return false;

    ID3D11Device* dev = m_parent ? m_parent->GetReal() : nullptr;
    if (!dev) return false;

    // Lazy SRV creation, one per eye texture (texture pointer stable
    // across captures — CopyResource doesn't change the resource).
    if (!m_leftEyeSRV)
    {
        D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format              = m_shadowFormat;
        sd.ViewDimension       = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        if (FAILED(dev->CreateShaderResourceView(m_leftEyeFrame, &sd, &m_leftEyeSRV)))
            return false;
    }
    if (!m_rightEyeSRV)
    {
        D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format              = m_shadowFormat;
        sd.ViewDimension       = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        if (FAILED(dev->CreateShaderResourceView(m_rightEyeFrame, &sd, &m_rightEyeSRV)))
            return false;
    }

    // Lazy RTV for the real BB.
    if (!m_realBBRTV)
    {
        ID3D11Texture2D* realBB = nullptr;
        if (FAILED(m_real->GetBuffer(0, IID_ID3D11Texture2D,
                                      reinterpret_cast<void**>(&realBB))) || !realBB)
            return false;
        HRESULT hr = dev->CreateRenderTargetView(realBB, nullptr, &m_realBBRTV);
        realBB->Release();
        if (FAILED(hr) || !m_realBBRTV) return false;
    }

    ID3D11DeviceContext* ctx = nullptr;
    dev->GetImmediateContext(&ctx);
    if (!ctx) return false;

    // SwapEyes flips which captured eye lands in which display half.
    bool swap = (NvDM_SwapEyes() != 0);
    ID3D11ShaderResourceView* leftSRV  = swap ? m_rightEyeSRV : m_leftEyeSRV;
    ID3D11ShaderResourceView* rightSRV = swap ? m_leftEyeSRV  : m_rightEyeSRV;
    ID3D11ShaderResourceView* srvs[2] = { leftSRV, rightSRV };

    // Pick the right pixel shader + sampler for the requested OutputMode.
    // Modes 0/3 -> T-B; 1/2 -> SBS; 4 -> Line; 5 -> Col; 6 -> Checker;
    // 7 -> Anaglyph; 8 -> Simulated Reality weave (separate weave-into-real-BB
    // pipeline; falls back to SBS if SR runtime missing or weaver create fails).
    int mode = NvDM_OutputMode();
    if (mode == 8)
    {
        if (RunSRWeave()) return true;
        // SR unavailable on this system (runtime missing, server down, or
        // weaver create failed). m_srBlacklistedOrFailed is now sticky;
        // fall through to SBS for the rest of the session.
        NVDM_TRACE_FIRST_N(1, "  RunCompositePass: SR weave unavailable, falling back to SBS\n");
    }
    ID3D11PixelShader* ps = m_compositePS_SBS;
    ID3D11SamplerState* sampler = m_compositeSampler;
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
        case 8:         ps = m_compositePS_SBS;       modeTag = "SBS (SR-fallback)"; break;
        default:        ps = m_compositePS_SBS;       modeTag = "SBS (fallback)";    break;
    }
    if (needsAnaglyphCB) UpdateAnaglyphCB();

    // Composite pass: write the SBS / T-B / interleaved / etc. framebuffer
    // into the real BB. We do NOT save/restore the game's pipeline state —
    // game rebinds everything on its next frame's first render. (Trade-off
    // documented in the class comment; a save/restore wrapper can be added
    // if a game proves to depend on state persisting across Present.)
    D3D11_VIEWPORT vp = {};
    vp.TopLeftX = 0; vp.TopLeftY = 0;
    vp.Width  = (FLOAT)m_logicalW;
    vp.Height = (FLOAT)m_logicalH;
    vp.MinDepth = 0; vp.MaxDepth = 1;

    ctx->OMSetRenderTargets(1, &m_realBBRTV, nullptr);
    ctx->RSSetViewports(1, &vp);
    ctx->RSSetState(m_compositeRS);
    ctx->OMSetBlendState(m_compositeBlend, nullptr, 0xFFFFFFFF);
    ctx->OMSetDepthStencilState(m_compositeDSS, 0);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11Buffer* nullVB = nullptr; UINT zero = 0;
    ctx->IASetVertexBuffers(0, 1, &nullVB, &zero, &zero);
    ctx->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
    ctx->VSSetShader(m_compositeVS, nullptr, 0);
    ctx->GSSetShader(nullptr, nullptr, 0);
    ctx->HSSetShader(nullptr, nullptr, 0);
    ctx->DSSetShader(nullptr, nullptr, 0);
    ctx->PSSetShader(ps, nullptr, 0);
    ctx->PSSetShaderResources(0, 2, srvs);
    ctx->PSSetSamplers(0, 1, &sampler);
    if (needsAnaglyphCB) ctx->PSSetConstantBuffers(2, 1, &m_anaglyphCB);
    ctx->Draw(3, 0);

    // Drop our SRV bindings so the game's next frame doesn't read from
    // a stale slot. RTV/blend/DSS will all be replaced by game's first
    // OMSet/blend/DSS calls so we leave them.
    ID3D11ShaderResourceView* nullSRV[2] = { nullptr, nullptr };
    ctx->PSSetShaderResources(0, 2, nullSRV);
    if (needsAnaglyphCB)
    {
        ID3D11Buffer* nullCB = nullptr;
        ctx->PSSetConstantBuffers(2, 1, &nullCB);
    }

    ctx->Release();
    NVDM_TRACE_FIRST_N(4, "  RunCompositePass: %s (mode=%d swap=%d) -> realBB rtv=%p\n",
                       modeTag, mode, (int)swap, m_realBBRTV);
    return true;
}

void SwapChainProxy::CaptureAndPresentBlit()
{
    if (!m_shadowBB || !m_real || !m_parent) return;

    // What's currently in the shadow is the latest-eye render. Capture
    // it into the appropriate eye slot so the composite pass / fallback
    // blit can see both eyes.
    int currentEye = NvDirectMode::GetActiveEye();
    if (currentEye == NvDirectMode::kEyeLeft || currentEye == NvDirectMode::kEyeRight)
    {
        CaptureEye(currentEye);   // self-allocates the slot on first call
        m_lastSeenEye = currentEye;
    }

    // Stage 4b: if both eyes are captured AND shaders compile, run the
    // SBS / T-B composite shader pass writing directly to the real BB.
    if (RunCompositePass())
        return;

    // Fallback: only one eye captured (or shaders unavailable). Blit
    // the single-eye captured frame, falling back through SwapEyes
    // preference and finally the shadow itself for fully-mono games.
    ID3D11Texture2D* fallbackBB = nullptr;
    if (FAILED(m_real->GetBuffer(0, IID_ID3D11Texture2D,
                                  reinterpret_cast<void**>(&fallbackBB))) || !fallbackBB)
        return;

    ID3D11DeviceContext* ctx = nullptr;
    if (m_parent->GetReal()) m_parent->GetReal()->GetImmediateContext(&ctx);
    if (!ctx) { fallbackBB->Release(); return; }

    bool swap = NvDM_SwapEyes() != 0;
    ID3D11Texture2D* leftSrc  = swap ? m_rightEyeFrame : m_leftEyeFrame;
    ID3D11Texture2D* rightSrc = swap ? m_leftEyeFrame  : m_rightEyeFrame;
    ID3D11Texture2D* displaySrc = leftSrc ? leftSrc : (rightSrc ? rightSrc : m_shadowBB);

    ctx->CopyResource(fallbackBB, displaySrc);
    ctx->Release();

    NVDM_TRACE_FIRST_N(4, "  CaptureAndPresentBlit (fallback): currentEye=%d displaySrc=%s (%p) -> realBB=%p\n",
                       currentEye,
                       (displaySrc == m_leftEyeFrame  ? "leftEye"  :
                        displaySrc == m_rightEyeFrame ? "rightEye" : "shadow"),
                       displaySrc, fallbackBB);

    fallbackBB->Release();
}

HRESULT STDMETHODCALLTYPE SwapChainProxy::GetBuffer(UINT Buffer, REFIID riid, void** ppSurface)
{
    if (Buffer == 0)
    {
        EnsureShadowBB();
        if (m_shadowBB && ppSurface)
        {
            // Hand the game our shadow texture in place of the real BB.
            // QI for whatever interface flavour was requested
            // (ID3D11Texture2D / IDXGISurface / etc).
            HRESULT hr = m_shadowBB->QueryInterface(riid, ppSurface);
            if (SUCCEEDED(hr) && *ppSurface && m_parent)
            {
                m_parent->RegisterBackBufferTexture(*ppSurface);
                LOG_VERBOSE("  GetBuffer(0): handed shadow %p (as %p via QI) registered on dev=%p\n",
                            m_shadowBB, *ppSurface, m_parent);
            }
            else
            {
                NVDM_TRACE_FIRST_N(4, "  GetBuffer(0): shadow QI hr=0x%08lX  surface=%p\n",
                                   hr, ppSurface ? *ppSurface : nullptr);
            }
            return hr;
        }
    }
    HRESULT hr = m_real->GetBuffer(Buffer, riid, ppSurface);
    NVDM_TRACE_FIRST_N(4, "  GetBuffer(idx=%u) hr=0x%08lX surface=%p (passthrough — non-zero buffer)\n",
                       Buffer, hr, ppSurface ? *ppSurface : NULL);
    return hr;
}

HRESULT STDMETHODCALLTYPE SwapChainProxy::ResizeBuffers(
    UINT BufferCount, UINT Width, UINT Height,
    DXGI_FORMAT NewFormat, UINT SwapChainFlags)
{
    // Game thinks the swap chain is at logical (one-eye) size so it asks
    // us to resize to W x H. Real swap chain agrees — pass straight
    // through. Shadow BB is dropped here; next GetBuffer(0) re-allocates
    // it at 2W x H against the new size.
    LOG_VERBOSE("  SwapChainProxy::ResizeBuffers(BufferCount=%u, %ux%u, fmt=%d, flags=0x%X)\n",
                BufferCount, Width, Height, (int)NewFormat, SwapChainFlags);
    // Real BB pointer becomes invalid after ResizeBuffers; drop our RTV
    // and SRVs (eye frames may also need re-allocation if format changed).
    if (m_realBBRTV) { m_realBBRTV->Release(); m_realBBRTV = nullptr; }
    ReleaseEyeFrames();
    ReleaseShadowBB();
    return m_real->ResizeBuffers(BufferCount, Width, Height, NewFormat, SwapChainFlags);
}

// NOTE: ResizeTarget / SetFullscreenState etc are inline passthroughs in
// the header — the real swap chain knows the same dimensions the game
// thinks it has now (no doubling), so no overrides needed.

} // namespace NvDirectMode
