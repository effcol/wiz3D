// AmdQbProxy.cpp
// Fake atidxx32.dll (x86) / atidxx64.dll (x64)
//
// Implements the AMD Quad-Buffer Stereo COM interface so that AMD HD3D
// native games can enable their stereo render path on ANY GPU (AMD, NVIDIA,
// Intel).  Games render left eye into the top half and right eye into the
// bottom half of a doubled render target (top-and-bottom format).
//
// Shadow texture approach: the swap chain back buffer stays at display
// resolution.  A separate "shadow" texture at doubled height is returned
// by our hooked IDXGISwapChain::GetBuffer, so the game renders T&B into
// the shadow.  Our IDXGISwapChain::Present hook composites SBS from the
// shadow into the real (display-sized) BB before calling the original
// Present.  This avoids DXGI scaling issues that caused display bugs on
// NVIDIA (Hitman: Absolution, Tomb Raider 2013, GRID 2/Autosport).
//
// All output modes are half resolution per eye — each eye sees half the
// screen dimensions (SBS: half width, TAB: half height).
//
// D3D11 games: full SBS/TAB/Crosseyed compositor.
// D3D10 games: COM stubs work (game enters stereo mode) but no conversion —
//              TODO: add D3D10 compositor if needed.
//
// Config.xml (same folder as the DLL):
//   <AmdQbProxy>
//     <OutputMode Value="1"/>    <!-- 0=Top-and-Bottom, 1=SBS (default), 2=Crosseyed -->
//   </AmdQbProxy>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "..\Shared\version.h"
#include <new>
#include <stdio.h>
#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <unordered_map>
#include <vector>
#include <stdint.h>

// Local interfaces header — omits the AmdDxExtCreate/AmdDxExtCreate11 function
// declarations from the AMD SDK so our extern "C" exports don't cause C2732.
#include "AmdQbInterfaces.h"

#include <MinHook.h>

// SR-Lib simplified C-style factory API. Static lib with SR runtime 
// DLLs delay-loaded — see SR.hpp for the rationale and the vcxproj's
// DelayLoadDLLs entries that pair with it.
#include "SR.hpp"

// ============================================================
// Diagnostic log
// ============================================================
static HMODULE g_hSelf = nullptr;

static void WriteLog(const char* msg)
{
    // Write next to the DLL (game directory) so the log is easy to find.
    wchar_t dir[MAX_PATH] = {};
    if (g_hSelf)
    {
        GetModuleFileNameW(g_hSelf, dir, MAX_PATH);
        wchar_t* p = wcsrchr(dir, L'\\');
        if (p) p[1] = L'\0';
    }

    // Draw OS cursor twice for SBS/TAB stereo so the pointer matches both eyes.
    // Cursor drawing is handled in the compositor Present path so we have a
    // valid HWND and can draw after the compositor finishes. The previous
    // implementation attempted to draw from the generic WriteLog function and
    // referenced undefined swap-chain state; move drawing into HookedPresent.
    if (!dir[0] && !GetTempPathW(MAX_PATH, dir)) return;

    wchar_t path[MAX_PATH];
    wcscpy_s(path, dir);
    wcscat_s(path, L"HD3D_atidxx.log");
    HANDLE h = CreateFileW(path, FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD written;
    WriteFile(h, msg, static_cast<DWORD>(strlen(msg)), &written, nullptr);
    CloseHandle(h);
}

// ============================================================
// Config
// ============================================================
enum OutputMode {
    OM_OU_HALF          = 0,  // Half Top-and-Bottom     (half res per eye, stacked)
    OM_SBS_HALF         = 1,  // Half Side-by-Side       (half res per eye, side by side, default)
    OM_SBS_FULL         = 2,  // Full Side-by-Side       (set game to half display width)
    OM_OU_FULL          = 3,  // Full Top-and-Bottom     (set game to half display height)
    OM_LINE_INTERLEAVED = 4,  // Line/Row Interleaved    (alternating horizontal rows, passive 3D TVs)
    OM_COL_INTERLEAVED  = 5,  // Column Interleaved      (alternating vertical columns)
    OM_CHECKERBOARD     = 6,  // Checkerboard            (alternating pixels, DLP projectors)
    OM_ANAGLYPH         = 7,  // Anaglyph                (colour-filtered stereo, requires glasses)
    OM_SR_WEAVE         = 8   // Simulated Reality Weave (Leia/Samsung Odyssey Moving Lightfield displays)
};
static OutputMode g_OutputMode = OM_SBS_HALF;
static bool g_bSwapEyes = false;

enum AnaglyphColour {
    AC_RED_CYAN      = 0,  // Red left / Cyan right  (most common)
    AC_GREEN_MAGENTA = 1,  // Green left / Magenta right
    AC_AMBER_BLUE    = 2,  // Amber left / Blue right (best colour rendition)
    AC_TRIOVIZ       = 3   // TriOviz/Inficolor 3D — magenta left, green right
};
static AnaglyphColour g_AnaglyphColour = AC_RED_CYAN;

enum AnaglyphMethod {
    AM_DUBOIS      = 0,  // Best for glasses viewing; minimises ghosting via spectral correction (default)
    AM_COMPROMISE  = 1,  // Best for mixed audiences; looks acceptable with AND without glasses
    AM_COLOR       = 2,  // Full colour, severe ghosting; avoid for glasses use
    AM_HALF_COLOR  = 3,  // Right eye keeps colour, left desaturated; reduced ghosting
    AM_OPTIMISED   = 4,  // Wimmer weighted-channel blend; good ghosting, less colour than Dubois
    AM_GREY        = 5,  // Both eyes greyscale; near-zero ghosting, no colour
    AM_TRUE        = 6   // Classic single-channel luma per eye; maximum ghosting, historical
};
static AnaglyphMethod g_AnaglyphMethod = AM_DUBOIS;

// Pull <Tag Value="N"/> ints out of the loaded config text. The needle is the
// exact element-and-attribute prefix "<Tag Value=\"", so we never match the tag
// name inside a comment or latch onto an unrelated downstream Value=" (the bare
// strstr(buf,"Tag") approach this replaced did both). Matches the same loose
// schema the NvDirectMode backends read, so the two stay in sync.
static int ReadConfigInt(const char* xml, const char* tag, int defaultValue)
{
    char needle[64];
    _snprintf_s(needle, sizeof(needle), _TRUNCATE, "<%s Value=\"", tag);
    const char* p = strstr(xml, needle);
    if (!p) return defaultValue;
    p += strlen(needle);
    return atoi(p);
}

static void LoadConfig()
{
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(g_hSelf, path, MAX_PATH);
    wchar_t* lastSlash = wcsrchr(path, L'\\');
    if (!lastSlash) return;
    size_t remaining = MAX_PATH - static_cast<size_t>(lastSlash - path + 1);
    wcscpy_s(lastSlash + 1, remaining, L"HD3D_Config.xml");

    HANDLE hFile = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return;

    // Read up to 16 KB into a heap buffer. The released wiz3D configs run to
    // tens of KB; the old fixed 4 KB stack buffer silently truncated them, so
    // any tag past the cut reverted to its default. 16 KB matches NvDirectMode.
    DWORD fileSize = GetFileSize(hFile, nullptr);
    if (fileSize == INVALID_FILE_SIZE || fileSize == 0 || fileSize > 16 * 1024)
    {
        CloseHandle(hFile);
        return;
    }
    char* buf = static_cast<char*>(malloc(fileSize + 1));
    if (!buf) { CloseHandle(hFile); return; }
    DWORD bytesRead = 0;
    if (!ReadFile(hFile, buf, fileSize, &bytesRead, nullptr))
    {
        free(buf);
        CloseHandle(hFile);
        return;
    }
    buf[bytesRead] = '\0';
    CloseHandle(hFile);

    // Load OutputMode
    {
        int mode = ReadConfigInt(buf, "OutputMode", g_OutputMode);
        if (mode >= OM_OU_HALF && mode <= OM_SR_WEAVE)
            g_OutputMode = static_cast<OutputMode>(mode);
    }

    // Some games crash if the SR runtime DLLs are loaded into their process
    // (e.g. Tomb Raider 2013 dies during init when SimulatedRealityCore loads).
    // For those, force the fallback to OM_SBS_HALF here at config time, before
    // any SR code paths run. New entries can be added as more games are tested.
    if (g_OutputMode == OM_SR_WEAVE)
    {
        wchar_t exePath[MAX_PATH] = {};
        if (GetModuleFileNameW(nullptr, exePath, MAX_PATH))
        {
            for (wchar_t* p = exePath; *p; ++p) *p = towlower(*p);
            const wchar_t* srBlacklist[] = {
                L"tombraider.exe",   // TR2013: SR DllMain side-effects crash the game
            };
            for (size_t i = 0; i < _countof(srBlacklist); ++i)
            {
                if (wcsstr(exePath, srBlacklist[i]))
                {
                    WriteLog("[AmdQbProxy] SR-incompatible exe detected; forcing OM_SBS_HALF\n");
                    g_OutputMode = OM_SBS_HALF;
                    break;
                }
            }
        }
    }
    // For SR-eligible games, SR DLLs are loaded lazily on first stereo present
    // via the delay-load mechanism — so non-SR systems (Linux/Proton, no Leia
    // runtime) won't load SR DLLs at all if the game never reaches a stereo
    // present, and will downgrade gracefully via SR-Lib's LoadLibrary probe
    // (CreateSRInterfaceDX11 returns E_NOINTERFACE) if they do.

    // Load SwapEyes setting
    g_bSwapEyes = (ReadConfigInt(buf, "SwapEyes", g_bSwapEyes ? 1 : 0) != 0);

    // Load AnaglyphColour
    {
        int col = ReadConfigInt(buf, "AnaglyphColour", g_AnaglyphColour);
        if (col >= AC_RED_CYAN && col <= AC_TRIOVIZ)
            g_AnaglyphColour = static_cast<AnaglyphColour>(col);
    }

    // Load AnaglyphMethod
    {
        int meth = ReadConfigInt(buf, "AnaglyphMethod", g_AnaglyphMethod);
        if (meth >= AM_DUBOIS && meth <= AM_TRUE)
            g_AnaglyphMethod = static_cast<AnaglyphMethod>(meth);
    }

    free(buf);
}

// ============================================================
// D3D11 state
// ============================================================
static ID3D11Device*        g_pDevice11  = nullptr;
static ID3D11DeviceContext* g_pContext    = nullptr;

// Compositor resources — recreated when swap chain dimensions change
static ID3D11Texture2D*          g_pStagingTex = nullptr;
static ID3D11ShaderResourceView* g_pStagingSRV = nullptr;
static UINT                      g_nStagingW   = 0;
static UINT                      g_nStagingH   = 0;
static DXGI_FORMAT               g_StagingFmt  = DXGI_FORMAT_UNKNOWN;

static ID3D11VertexShader*       g_pVS                = nullptr;
static ID3D11PixelShader*        g_pPS                = nullptr;
static ID3D11Buffer*             g_pCB                = nullptr;
static ID3D11SamplerState*       g_pSampler           = nullptr;  // linear (default)
static ID3D11SamplerState*       g_pPointSampler      = nullptr;  // point (interleaved modes)
static ID3D11PixelShader*        g_pLineInterleavedPS = nullptr;
static ID3D11PixelShader*        g_pColInterleavedPS  = nullptr;
static ID3D11PixelShader*        g_pCheckerboardPS    = nullptr;
static ID3D11PixelShader*        g_pAnaglyphPS        = nullptr;
static ID3D11PixelShader*        g_pAnaglyphTriovizPS = nullptr; // TriOviz variant with blend_RGB suppression
static ID3D11Buffer*             g_pAnaglyphCB        = nullptr;
static ID3D11DepthStencilState*  g_pDSState = nullptr;
static ID3D11RasterizerState*    g_pRSState = nullptr;
// NvDirectMode-style cursor handling: rely entirely on the OS hardware
// cursor drawn by DWM/the display driver post-Present. No software
// composite, no Win32 hooks, no doubled sprite. Previous implementation
// (~600 lines including SetCursor/ShowCursor/ClipCursor/SetCursorPos
// hooks, a WM_SETCURSOR window subclass, GPU-rendered cursor sprite per
// eye, and a blank-cursor fallback) was reliable on NVIDIA HD3D-driver
// setups but produced flicker / wrong position / disappeared cursor
// on AMD HD3D and SR-weave setups, plus added several hundred lines
// of game-specific cursor-hide-detection workarounds. Removed wholesale.

// SR (Simulated Reality / Leia) weave resources.  
// CreateSRInterfaceDX11 manages details internally.
static SimulatedReality::SRInterfaceDX11* g_pSRInterface = nullptr;
static ID3D11Texture2D*          g_pSRSBSTex     = nullptr;
static ID3D11RenderTargetView*   g_pSRSBSRTV     = nullptr;
static ID3D11ShaderResourceView* g_pSRSBSSRV     = nullptr;
static UINT                      g_nSRSBSW       = 0;
static UINT                      g_nSRSBSH       = 0;
static DXGI_FORMAT               g_SRSBSFmt      = DXGI_FORMAT_UNKNOWN;

// Shadow fixup resources — for engines (Hitman/EGO/Tomb Raider 2013) where the
// game only renders content into the TOP HALF of each eye's shadow slot. Before
// the compositor runs, we render-stretch each eye's top half to fill the full
// eye slot in this fixup texture, then rebind compositor input to fixup so all
// output paths (TAB, SBS, anaglyph, interleaved, SR weave) see "normal" content.
static ID3D11Texture2D*          g_pShadowFixupTex = nullptr;
static ID3D11RenderTargetView*   g_pShadowFixupRTV = nullptr;
static ID3D11ShaderResourceView* g_pShadowFixupSRV = nullptr;
static UINT                      g_nFixupW         = 0;
static UINT                      g_nFixupH         = 0;  // shadow height = origH * 2
static DXGI_FORMAT               g_FixupFmt        = DXGI_FORMAT_UNKNOWN;

static void CleanupSRWeaver()
{
	if (g_pSRInterface) { g_pSRInterface->Delete(); g_pSRInterface = nullptr; }
	if (g_pSRSBSSRV)    { g_pSRSBSSRV->Release();   g_pSRSBSSRV    = nullptr; }
	if (g_pSRSBSRTV)    { g_pSRSBSRTV->Release();   g_pSRSBSRTV    = nullptr; }
	if (g_pSRSBSTex)    { g_pSRSBSTex->Release();   g_pSRSBSTex    = nullptr; }
	g_nSRSBSW = 0; g_nSRSBSH = 0; g_SRSBSFmt = DXGI_FORMAT_UNKNOWN;
}

static void CleanupShadowFixup()
{
    if (g_pShadowFixupSRV) { g_pShadowFixupSRV->Release(); g_pShadowFixupSRV = nullptr; }
    if (g_pShadowFixupRTV) { g_pShadowFixupRTV->Release(); g_pShadowFixupRTV = nullptr; }
    if (g_pShadowFixupTex) { g_pShadowFixupTex->Release(); g_pShadowFixupTex = nullptr; }
    g_nFixupW = 0; g_nFixupH = 0; g_FixupFmt = DXGI_FORMAT_UNKNOWN;
}

// Release all compositor resources so they are recreated with the correct device.
static void ReleaseCompositorResources()
{
    CleanupSRWeaver();
    CleanupShadowFixup();
    if (g_pStagingSRV) { g_pStagingSRV->Release(); g_pStagingSRV = nullptr; }
    if (g_pStagingTex) { g_pStagingTex->Release(); g_pStagingTex = nullptr; }
    g_nStagingW = 0; g_nStagingH = 0; g_StagingFmt = DXGI_FORMAT_UNKNOWN;
    if (g_pVS)                { g_pVS->Release();                g_pVS                = nullptr; }
    if (g_pPS)                { g_pPS->Release();                g_pPS                = nullptr; }
    if (g_pCB)                { g_pCB->Release();                g_pCB                = nullptr; }
    if (g_pSampler)           { g_pSampler->Release();           g_pSampler           = nullptr; }
    if (g_pPointSampler)      { g_pPointSampler->Release();      g_pPointSampler      = nullptr; }
    if (g_pLineInterleavedPS) { g_pLineInterleavedPS->Release(); g_pLineInterleavedPS = nullptr; }
    if (g_pColInterleavedPS)  { g_pColInterleavedPS->Release();  g_pColInterleavedPS  = nullptr; }
    if (g_pCheckerboardPS)    { g_pCheckerboardPS->Release();    g_pCheckerboardPS    = nullptr; }
    if (g_pAnaglyphPS)        { g_pAnaglyphPS->Release();        g_pAnaglyphPS        = nullptr; }
    if (g_pAnaglyphTriovizPS) { g_pAnaglyphTriovizPS->Release(); g_pAnaglyphTriovizPS = nullptr; }
    if (g_pAnaglyphCB)        { g_pAnaglyphCB->Release();        g_pAnaglyphCB        = nullptr; }
    if (g_pDSState)           { g_pDSState->Release();           g_pDSState           = nullptr; }
    if (g_pRSState)           { g_pRSState->Release();           g_pRSState           = nullptr; }
}

// ============================================================
// Dynamic D3DCompile loading (avoids hard dependency on d3dcompiler_*.dll)
// ============================================================
typedef HRESULT (WINAPI *PFN_D3DCompile)(
    LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*,
    ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT,
    ID3DBlob**, ID3DBlob**);

static PFN_D3DCompile GetD3DCompile()
{
    static PFN_D3DCompile pfn = nullptr;
    if (pfn) return pfn;
    for (int v = 47; v >= 43; --v)
    {
        wchar_t name[32];
        swprintf_s(name, 32, L"d3dcompiler_%d.dll", v);
        HMODULE h = LoadLibraryW(name);
        if (h)
        {
            pfn = reinterpret_cast<PFN_D3DCompile>(GetProcAddress(h, "D3DCompile"));
            if (pfn) return pfn;
        }
    }
    return nullptr;
}

// ============================================================
// Shaders
// SBS conversion: source is over-under (left eye=top half, right eye=bottom half).
// Left  viewport (x: 0..W/2):  samples source UV v in [0.0, 0.5]
// Right viewport (x: W/2..W):  samples source UV v in [0.5, 1.0]
// Crosseyed swaps which eye is on which side.
// ============================================================
static const char s_VSSource[] =
    "void VSMain(uint vid:SV_VertexID,out float4 pos:SV_Position,out float2 uv:TEXCOORD0){"
    "  float2 v[3]={float2(-1,1),float2(3,1),float2(-1,-3)};"
    "  float2 t[3]={float2(0,0),float2(2,0),float2(0,2)};"
    "  pos=float4(v[vid],0,1);uv=t[vid];"
    "}";

static const char s_PSSource[] =
    "Texture2D<float4> src:register(t0);"
    "SamplerState smp:register(s0);"
    "cbuffer CB:register(b0){float uvOff;float uvScl;float2 pad;};"
    "float4 PSMain(float4 p:SV_Position,float2 uv:TEXCOORD0):SV_Target{"
    "  return src.Sample(smp,float2(uv.x,uv.y*uvScl+uvOff));"
    "}";

// Interleaved/checkerboard shaders — full-screen single draw.
// Shadow top half = left eye, bottom half = right eye (same T&B layout as all modes).
// SV_Position gives pixel centre (0.5, 1.5, …) so floor() gives the integer row/col.
// CB slot 0 (uvOff) is repurposed as a swap flag: 0.0 = normal, 1.0 = eyes swapped.
static const char s_PSLineInterleaved[] =
    "Texture2D<float4> src:register(t0);SamplerState smp:register(s0);"
    "cbuffer CB:register(b0){float swap;float uvScl;float2 pad;};"
    "float4 PSMain(float4 pos:SV_Position,float2 uv:TEXCOORD0):SV_Target{"
    "  float4 L=src.Sample(smp,float2(uv.x,uv.y*0.5));"
    "  float4 R=src.Sample(smp,float2(uv.x,uv.y*0.5+0.5));"
    "  bool ev=fmod(floor(pos.y),2)<1;"   // true on even rows (0,2,4,…)
    "  bool sl=(swap<0.5);"               // sl: left eye on even rows?
    "  return (ev==sl)?L:R;"
    "}";

static const char s_PSColInterleaved[] =
    "Texture2D<float4> src:register(t0);SamplerState smp:register(s0);"
    "cbuffer CB:register(b0){float swap;float uvScl;float2 pad;};"
    "float4 PSMain(float4 pos:SV_Position,float2 uv:TEXCOORD0):SV_Target{"
    "  float4 L=src.Sample(smp,float2(uv.x,uv.y*0.5));"
    "  float4 R=src.Sample(smp,float2(uv.x,uv.y*0.5+0.5));"
    "  bool ev=fmod(floor(pos.x),2)<1;"   // true on even columns (0,2,4,…)
    "  bool sl=(swap<0.5);"
    "  return (ev==sl)?L:R;"
    "}";

static const char s_PSCheckerboard[] =
    "Texture2D<float4> src:register(t0);SamplerState smp:register(s0);"
    "cbuffer CB:register(b0){float swap;float uvScl;float2 pad;};"
    "float4 PSMain(float4 pos:SV_Position,float2 uv:TEXCOORD0):SV_Target{"
    "  float4 L=src.Sample(smp,float2(uv.x,uv.y*0.5));"
    "  float4 R=src.Sample(smp,float2(uv.x,uv.y*0.5+0.5));"
    "  bool ev=fmod(floor(pos.x)+floor(pos.y),2)<1;" // checkerboard parity
    "  bool sl=(swap<0.5);"
    "  return (ev==sl)?L:R;"
    "}";

// Anaglyph shader: two 3x3 colour matrices (left-eye, right-eye) stored as 6 float4 rows in CB slot 2.
// Shadow layout: top half = left eye, bottom half = right eye (same as all other modes).
// result.R = dot(lR.xyz, left) + dot(rR.xyz, right), etc.  Output is clamped to [0,1].
static const char s_PSAnaglyph[] =
    "Texture2D<float4> src:register(t0);SamplerState smp:register(s0);"
    "cbuffer ACB:register(b2){float4 lR;float4 lG;float4 lB;float4 rR;float4 rG;float4 rB;};"
    "float4 PSMain(float4 pos:SV_Position,float2 uv:TEXCOORD0):SV_Target{"
    "  float3 L=src.Sample(smp,float2(uv.x,uv.y*0.5)).rgb;"
    "  float3 R=src.Sample(smp,float2(uv.x,uv.y*0.5+0.5)).rgb;"
    "  float3 a;"
    "  a.r=dot(lR.xyz,L)+dot(rR.xyz,R);"
    "  a.g=dot(lG.xyz,L)+dot(rG.xyz,R);"
    "  a.b=dot(lB.xyz,L)+dot(rB.xyz,R);"
    "  return float4(saturate(a),1.0);"
    "}";

// TriOviz/Inficolor 3D anaglyph shader with blend_RGB ghosting suppression.
// After the standard matrix composite, applies per-channel luminance suppression
// to reduce ghosting caused by TriOviz's non-complementary filter design.
// Matches SuperDepth3D.fx Inficolor_3D_Emulator post-processing pass:
//   blend_RGB = channel-dominance vector (positive = that channel dominates)
//   each channel is attenuated by 50% when it dominates (smoothstep ramp)
// Thresholds from SuperDepth3D: R=-0.250..0, G=-0.375..0, B=-0.500..0
// Reduce level 0.5 matches the default Inficolor_Reduce_RGB=0.5 from the
// working preset (vpforums post #281, Gravy/Ryan Goodenough settings).
static const char s_PSAnaglyph_Trioviz[] =
    "Texture2D<float4> src:register(t0);SamplerState smp:register(s0);"
    "cbuffer ACB:register(b2){float4 lR;float4 lG;float4 lB;float4 rR;float4 rG;float4 rB;};"
    "float4 PSMain(float4 pos:SV_Position,float2 uv:TEXCOORD0):SV_Target{"
    "  float3 L=src.Sample(smp,float2(uv.x,uv.y*0.5)).rgb;"
    "  float3 R=src.Sample(smp,float2(uv.x,uv.y*0.5+0.5)).rgb;"
    "  float3 a;"
    "  a.r=dot(lR.xyz,L)+dot(rR.xyz,R);"
    "  a.g=dot(lG.xyz,L)+dot(rG.xyz,R);"
    "  a.b=dot(lB.xyz,L)+dot(rB.xyz,R);"
    // blend_RGB ghosting suppression (SuperDepth3D Inficolor_3D_Emulator pass)
    "  float3 blend_RGB=float3(dot(a,float3(1,-1,-1)),dot(a,float3(-1,1,-1)),dot(a,float3(-1,-1,1)));"
    "  a.r*=lerp(1.0,lerp(1.0,0.5,smoothstep(-0.250,0.0,blend_RGB.r)),0.5);"
    "  a.g*=lerp(1.0,lerp(1.0,0.5,smoothstep(-0.375,0.0,blend_RGB.g)),0.5);"
    "  a.b*=lerp(1.0,lerp(1.0,0.5,smoothstep(-0.500,0.0,blend_RGB.b)),0.5);"
    "  return float4(saturate(a),1.0);"
    "}";

// 6 coefficients (float3 each) per combination: lR, lG, lB (left-eye output rows), rR, rG, rB (right-eye).
// Applied as: out.R = dot(lR, leftRGB) + dot(rR, rightRGB), etc.
// Order: [AnaglyphColour][AnaglyphMethod] = [RC/GM/AB][Dubois/Compromise/Color/HalfColor/Optimised/Grey/True]
struct AnaglyphMatrix { float lR[3], lG[3], lB[3], rR[3], rG[3], rB[3]; };

static const AnaglyphMatrix g_AnaglyphMatrices[4][7] =
{
    // ---- AC_RED_CYAN [0] ----
    {
        // AM_DUBOIS [0] — spectral least-squares fit (Dubois 2001); best ghosting suppression
        {{ 0.456f, 0.500f, 0.176f}, {-0.040f,-0.038f,-0.016f}, {-0.015f,-0.021f,-0.016f},
         {-0.043f,-0.088f,-0.002f}, { 0.378f, 0.734f, 0.018f}, {-0.072f,-0.113f, 1.226f}},
        // AM_COMPROMISE [1] — Ahtik 2011; average of Color/HalfColor/Dubois/Optimised with grey-preservation
        {{ 0.439f, 0.447f, 0.148f}, { 0.000f, 0.000f, 0.000f}, { 0.000f, 0.000f, 0.000f},
         { 0.000f, 0.000f, 0.000f}, { 0.095f, 0.934f,-0.005f}, {-0.018f,-0.028f, 1.057f}},
        // AM_COLOR [2] — direct channel passthrough; zero ghosting but severe colour fringing
        {{ 1.000f, 0.000f, 0.000f}, { 0.000f, 0.000f, 0.000f}, { 0.000f, 0.000f, 0.000f},
         { 0.000f, 0.000f, 0.000f}, { 0.000f, 1.000f, 0.000f}, { 0.000f, 0.000f, 1.000f}},
        // AM_HALF_COLOR [3] — left eye desaturated, right eye keeps colour
        {{ 0.299f, 0.587f, 0.114f}, { 0.000f, 0.000f, 0.000f}, { 0.000f, 0.000f, 0.000f},
         { 0.000f, 0.000f, 0.000f}, { 0.000f, 1.000f, 0.000f}, { 0.000f, 0.000f, 1.000f}},
        // AM_OPTIMISED [4] — Wimmer weighted-channel blend
        {{ 0.000f, 0.700f, 0.300f}, { 0.000f, 0.000f, 0.000f}, { 0.000f, 0.000f, 0.000f},
         { 0.000f, 0.000f, 0.000f}, { 0.000f, 1.000f, 0.000f}, { 0.000f, 0.000f, 1.000f}},
        // AM_GREY [5] — both eyes greyscale; near-zero ghosting, no colour
        {{ 0.299f, 0.587f, 0.114f}, { 0.000f, 0.000f, 0.000f}, { 0.000f, 0.000f, 0.000f},
         { 0.000f, 0.000f, 0.000f}, { 0.299f, 0.587f, 0.114f}, { 0.299f, 0.587f, 0.114f}},
        // AM_TRUE [6] — classic single-channel luma; maximum ghosting, historical
        {{ 0.299f, 0.587f, 0.114f}, { 0.000f, 0.000f, 0.000f}, { 0.000f, 0.000f, 0.000f},
         { 0.000f, 0.000f, 0.000f}, { 0.000f, 0.000f, 0.000f}, { 0.299f, 0.587f, 0.114f}},
    },
    // ---- AC_GREEN_MAGENTA [1] ----
    {
        // AM_DUBOIS [0] — spectral values approximated from Dubois's green/magenta filter measurements
        {{-0.063f,-0.162f, 0.042f}, { 0.285f, 0.665f, 0.150f}, {-0.015f,-0.027f, 0.021f},
         { 0.529f, 0.705f,-0.047f}, {-0.016f,-0.015f,-0.065f}, { 0.009f, 0.076f, 0.937f}},
        // AM_COMPROMISE [1] — computed via Ahtik methodology applied to G/M filter pair
        {{ 0.000f, 0.000f, 0.000f}, { 0.146f, 0.738f, 0.141f}, { 0.000f, 0.000f, 0.000f},
         { 0.882f, 0.176f,-0.012f}, { 0.000f, 0.000f, 0.000f}, { 0.002f, 0.019f, 0.984f}},
        // AM_COLOR [2]
        {{ 0.000f, 0.000f, 0.000f}, { 0.000f, 1.000f, 0.000f}, { 0.000f, 0.000f, 0.000f},
         { 1.000f, 0.000f, 0.000f}, { 0.000f, 0.000f, 0.000f}, { 0.000f, 0.000f, 1.000f}},
        // AM_HALF_COLOR [3] — left eye (green) desaturated; right eye (magenta=R+B) keeps colour
        {{ 0.000f, 0.000f, 0.000f}, { 0.299f, 0.587f, 0.114f}, { 0.000f, 0.000f, 0.000f},
         { 1.000f, 0.000f, 0.000f}, { 0.000f, 0.000f, 0.000f}, { 0.000f, 0.000f, 1.000f}},
        // AM_OPTIMISED [4] — Wimmer weighted-channel adapted for green/magenta
        {{ 0.000f, 0.000f, 0.000f}, { 0.000f, 0.700f, 0.300f}, { 0.000f, 0.000f, 0.000f},
         { 1.000f, 0.000f, 0.000f}, { 0.000f, 0.000f, 0.000f}, { 0.000f, 0.000f, 1.000f}},
        // AM_GREY [5]
        {{ 0.000f, 0.000f, 0.000f}, { 0.299f, 0.587f, 0.114f}, { 0.000f, 0.000f, 0.000f},
         { 0.299f, 0.587f, 0.114f}, { 0.000f, 0.000f, 0.000f}, { 0.299f, 0.587f, 0.114f}},
        // AM_TRUE [6] — left eye green luma, right eye red luma only (single magenta channel)
        {{ 0.000f, 0.000f, 0.000f}, { 0.299f, 0.587f, 0.114f}, { 0.000f, 0.000f, 0.000f},
         { 0.299f, 0.587f, 0.114f}, { 0.000f, 0.000f, 0.000f}, { 0.000f, 0.000f, 0.000f}},
    },
    // ---- AC_AMBER_BLUE [2] ----
    {
        // AM_DUBOIS [0] — spectral values approximated from Dubois's amber/blue filter measurements
        {{ 1.062f, 0.366f,-0.057f}, {-0.063f,-0.019f, 0.019f}, {-0.003f,-0.016f, 0.031f},
         {-0.390f,-0.350f, 0.055f}, { 0.468f, 0.246f, 0.000f}, {-0.018f, 0.102f, 0.902f}},
        // AM_COMPROMISE [1] — computed via Ahtik methodology applied to A/B filter pair
        {{ 0.840f, 0.238f, 0.014f}, { 0.059f, 0.642f, 0.033f}, { 0.000f, 0.000f, 0.000f},
         { 0.000f, 0.000f, 0.000f}, { 0.000f, 0.000f, 0.000f}, {-0.005f, 0.026f, 0.976f}},
        // AM_COLOR [2]
        {{ 1.000f, 0.000f, 0.000f}, { 0.000f, 1.000f, 0.000f}, { 0.000f, 0.000f, 0.000f},
         { 0.000f, 0.000f, 0.000f}, { 0.000f, 0.000f, 0.000f}, { 0.000f, 0.000f, 1.000f}},
        // AM_HALF_COLOR [3] — left eye (amber=R+G) desaturated into both channels; right eye keeps blue
        {{ 0.299f, 0.587f, 0.114f}, { 0.299f, 0.587f, 0.114f}, { 0.000f, 0.000f, 0.000f},
         { 0.000f, 0.000f, 0.000f}, { 0.000f, 0.000f, 0.000f}, { 0.000f, 0.000f, 1.000f}},
        // AM_OPTIMISED [4] — same as color for amber/blue (channels are already separated)
        {{ 1.000f, 0.000f, 0.000f}, { 0.000f, 1.000f, 0.000f}, { 0.000f, 0.000f, 0.000f},
         { 0.000f, 0.000f, 0.000f}, { 0.000f, 0.000f, 0.000f}, { 0.000f, 0.000f, 1.000f}},
        // AM_GREY [5]
        {{ 0.299f, 0.587f, 0.114f}, { 0.299f, 0.587f, 0.114f}, { 0.000f, 0.000f, 0.000f},
         { 0.000f, 0.000f, 0.000f}, { 0.000f, 0.000f, 0.000f}, { 0.299f, 0.587f, 0.114f}},
        // AM_TRUE [6] — amber luma in R+G, blue luma in B
        {{ 0.299f, 0.587f, 0.114f}, { 0.299f, 0.587f, 0.114f}, { 0.000f, 0.000f, 0.000f},
         { 0.000f, 0.000f, 0.000f}, { 0.000f, 0.000f, 0.000f}, { 0.299f, 0.587f, 0.114f}},
    },
    // ---- AC_TRIOVIZ [3] — TriOviz/Inficolor 3D ----
    // Physical glasses: magenta lens on LEFT eye, green lens on RIGHT eye.
    // (Confirmed by vpinball/vpinball#709 user report.)
    //
    // IMPORTANT: TriOviz is NOT a true anaglyph system. Both eyes see significant
    // amounts of all colours (VPX source comment: "not truly anaglyph since both
    // eyes see most of the colors"). Standard G/M Dubois produces ghosting because
    // it assumes complementary filters, which TriOviz does not have.
    //
    // VPX (vpinball/vpinball Anaglyph.cpp) has measured TriOviz filter data:
    //   Left (magenta) filter: R:(0.8561,0.0026,0.0500) G:(0.1827,0.1562,0.1148) B:(0.0097,0.0002,0.7355)
    //   Right (green) filter:  R:(0.5470,0.0229,0.0210) G:(0.0005,0.8109,0.2163) B:(0.0011,0.0031,0.3776)
    // No published Dubois-style least-squares fit for these specific filters exists.
    //
    // AM_COLOR [2] = SuperDepth3D.fx "Inficolor 3D Emulation Alpha": out.R=L.R, out.G=R.G, out.B=L.B
    // AM_HALF_COLOR [3] = SuperDepth3D.fx "Inficolor 3D Emulation Beta": out.R=L.R, out.G=luma(R), out.B=L.B
    // AM_DUBOIS [0] = Dubois 2001 G/M with L/R swapped; best available approximation.
    // Sources: vpinball/vpinball Anaglyph.cpp; BlueSkyDefender/Depth3D SuperDepth3D.fx;
    //          vpinball/vpinball#709; Kodi forum TamaraKama 2015-10-29.
    {
        // AM_DUBOIS [0] — VPX luminance filter from measured TriOviz filter data.
        // rgb2Yl=[0.4668,0.3955,0.1377]  rgb2Yr=[0.1767,0.7842,0.0391]  White->White verified.
        {{ 0.9417f,  0.7979f,  0.2778f}, {-0.2591f,-0.2195f,-0.0764f}, { 0.9417f,  0.7979f,  0.2778f},
         { 0.0583f, -0.7979f, -0.2778f}, { 0.2591f,  1.2195f,  0.0764f}, {-0.9417f,-0.7979f,  0.7222f}},
        // AM_COMPROMISE [1] — G/M compromise with L/R swapped
        {{ 0.882f, 0.176f,-0.012f}, { 0.000f, 0.000f, 0.000f}, { 0.002f, 0.019f, 0.984f},
         { 0.000f, 0.000f, 0.000f}, { 0.146f, 0.738f, 0.141f}, { 0.000f, 0.000f, 0.000f}},
        // AM_COLOR [2] — SuperDepth3D "Inficolor 3D Emulation Alpha": out.R=L.R, out.G=R.G, out.B=L.B
        {{ 1.000f, 0.000f, 0.000f}, { 0.000f, 0.000f, 0.000f}, { 1.000f, 0.000f, 0.000f},
         { 0.000f, 0.000f, 0.000f}, { 0.000f, 1.000f, 0.000f}, { 0.000f, 0.000f, 0.000f}},
        // AM_HALF_COLOR [3] — SuperDepth3D "Inficolor 3D Emulation Beta": out.R=L.R, out.G=luma(R), out.B=L.B
        {{ 1.000f, 0.000f, 0.000f}, { 0.000f, 0.000f, 0.000f}, { 1.000f, 0.000f, 0.000f},
         { 0.000f, 0.000f, 0.000f}, { 0.299f, 0.587f, 0.114f}, { 0.000f, 0.000f, 0.000f}},
        // AM_OPTIMISED [4] — same as AM_COLOR (no better data available)
        {{ 1.000f, 0.000f, 0.000f}, { 0.000f, 0.000f, 0.000f}, { 1.000f, 0.000f, 0.000f},
         { 0.000f, 0.000f, 0.000f}, { 0.000f, 1.000f, 0.000f}, { 0.000f, 0.000f, 0.000f}},
        // AM_GREY [5] — both eyes greyscale; near-zero ghosting, no colour
        {{ 0.000f, 0.000f, 0.000f}, { 0.299f, 0.587f, 0.114f}, { 0.000f, 0.000f, 0.000f},
         { 0.299f, 0.587f, 0.114f}, { 0.000f, 0.000f, 0.000f}, { 0.299f, 0.587f, 0.114f}},
        // AM_TRUE [6] — left magenta luma, right green luma
        {{ 0.000f, 0.000f, 0.000f}, { 0.299f, 0.587f, 0.114f}, { 0.000f, 0.000f, 0.000f},
         { 0.299f, 0.587f, 0.114f}, { 0.000f, 0.000f, 0.000f}, { 0.000f, 0.000f, 0.000f}},
    },
};

// Upload the selected anaglyph matrix pair to g_pAnaglyphCB (bound to slot b2).
// SwapEyes swaps which shadow half maps to which filter by exchanging l/r matrix rows.
static void UpdateAnaglyphCB()
{
    if (!g_pAnaglyphCB || !g_pContext) return;
    const AnaglyphMatrix& m = g_AnaglyphMatrices[g_AnaglyphColour][g_AnaglyphMethod];
    const float* rows[6];
    if (g_bSwapEyes)
    {
        // Shadow top=right eye, bottom=left eye — invert which rows feed which filter
        rows[0] = m.rR; rows[1] = m.rG; rows[2] = m.rB;
        rows[3] = m.lR; rows[4] = m.lG; rows[5] = m.lB;
    }
    else
    {
        rows[0] = m.lR; rows[1] = m.lG; rows[2] = m.lB;
        rows[3] = m.rR; rows[4] = m.rG; rows[5] = m.rB;
    }
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(g_pContext->Map(g_pAnaglyphCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return;
    float* p = static_cast<float*>(mapped.pData);
    for (int i = 0; i < 6; ++i)
    {
        p[i * 4 + 0] = rows[i][0];
        p[i * 4 + 1] = rows[i][1];
        p[i * 4 + 2] = rows[i][2];
        p[i * 4 + 3] = 0.0f;
    }
    g_pContext->Unmap(g_pAnaglyphCB, 0);
}

static bool EnsureShaders()
{
    if (g_pVS && g_pPS && g_pCB && g_pSampler) return true;
    if (!g_pDevice11) return false;

    auto D3DCompileFn = GetD3DCompile();
    if (!D3DCompileFn) return false;

    if (!g_pVS)
    {
        ID3DBlob *blob = nullptr, *err = nullptr;
        D3DCompileFn(s_VSSource, strlen(s_VSSource), nullptr, nullptr, nullptr,
                     "VSMain", "vs_5_0", 0, 0, &blob, &err);
        if (blob)
        {
            g_pDevice11->CreateVertexShader(blob->GetBufferPointer(),
                                            blob->GetBufferSize(), nullptr, &g_pVS);
            blob->Release();
        }
        if (err) err->Release();
    }
    if (!g_pVS) return false;

    if (!g_pPS)
    {
        ID3DBlob *blob = nullptr, *err = nullptr;
        D3DCompileFn(s_PSSource, strlen(s_PSSource), nullptr, nullptr, nullptr,
                     "PSMain", "ps_5_0", 0, 0, &blob, &err);
        if (blob)
        {
            g_pDevice11->CreatePixelShader(blob->GetBufferPointer(),
                                           blob->GetBufferSize(), nullptr, &g_pPS);
            blob->Release();
        }
        if (err) err->Release();
    }
    if (!g_pPS) return false;

    if (!g_pCB)
    {
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth      = 16;
        bd.Usage          = D3D11_USAGE_DYNAMIC;
        bd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        g_pDevice11->CreateBuffer(&bd, nullptr, &g_pCB);
    }
    if (!g_pCB) return false;

    if (!g_pSampler)
    {
        D3D11_SAMPLER_DESC sd = {};
        sd.Filter         = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
        sd.AddressU       = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.AddressV       = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.AddressW       = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
        sd.MaxLOD         = D3D11_FLOAT32_MAX;
        g_pDevice11->CreateSamplerState(&sd, &g_pSampler);
    }
    if (!g_pSampler) return false;

    if (!g_pPointSampler)
    {
        D3D11_SAMPLER_DESC sd = {};
        sd.Filter         = D3D11_FILTER_MIN_MAG_MIP_POINT;
        sd.AddressU       = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.AddressV       = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.AddressW       = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
        sd.MaxLOD         = D3D11_FLOAT32_MAX;
        g_pDevice11->CreateSamplerState(&sd, &g_pPointSampler);
    }

    auto CompilePS = [&](const char* src, ID3D11PixelShader** ppPS) {
        if (*ppPS) return;
        ID3DBlob *blob = nullptr, *err = nullptr;
        D3DCompileFn(src, strlen(src), nullptr, nullptr, nullptr,
                     "PSMain", "ps_5_0", 0, 0, &blob, &err);
        if (blob) { g_pDevice11->CreatePixelShader(blob->GetBufferPointer(),
                                                    blob->GetBufferSize(), nullptr, ppPS);
                    blob->Release(); }
        if (err) err->Release();
    };
    CompilePS(s_PSLineInterleaved, &g_pLineInterleavedPS);
    CompilePS(s_PSColInterleaved,  &g_pColInterleavedPS);
    CompilePS(s_PSCheckerboard,    &g_pCheckerboardPS);
    CompilePS(s_PSAnaglyph,        &g_pAnaglyphPS);
    CompilePS(s_PSAnaglyph_Trioviz, &g_pAnaglyphTriovizPS);

    if (!g_pAnaglyphCB)
    {
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth      = 96;  // 6 float4 rows
        bd.Usage          = D3D11_USAGE_DYNAMIC;
        bd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        g_pDevice11->CreateBuffer(&bd, nullptr, &g_pAnaglyphCB);
    }

    if (!g_pDSState)
    {
        D3D11_DEPTH_STENCIL_DESC dsd = {};
        dsd.DepthEnable    = FALSE;
        dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        g_pDevice11->CreateDepthStencilState(&dsd, &g_pDSState);
    }

    if (!g_pRSState)
    {
        D3D11_RASTERIZER_DESC rsd = {};
        rsd.FillMode        = D3D11_FILL_SOLID;
        rsd.CullMode        = D3D11_CULL_NONE;
        rsd.DepthClipEnable = FALSE;
        g_pDevice11->CreateRasterizerState(&rsd, &g_pRSState);
    }

    return true;
}



static bool EnsureStagingTexture(UINT W, UINT H, DXGI_FORMAT Fmt)
{
    if (g_pStagingTex && g_nStagingW == W && g_nStagingH == H && g_StagingFmt == Fmt)
        return true;

    if (g_pStagingSRV) { g_pStagingSRV->Release(); g_pStagingSRV = nullptr; }
    if (g_pStagingTex) { g_pStagingTex->Release(); g_pStagingTex = nullptr; }

    D3D11_TEXTURE2D_DESC td = {};
    td.Width            = W;
    td.Height           = H;
    td.MipLevels        = 1;
    td.ArraySize        = 1;
    td.Format           = Fmt;
    td.SampleDesc.Count = 1;
    td.Usage            = D3D11_USAGE_DEFAULT;
    td.BindFlags        = D3D11_BIND_SHADER_RESOURCE;
    td.MiscFlags        = 0;
    if (FAILED(g_pDevice11->CreateTexture2D(&td, nullptr, &g_pStagingTex)))
        return false;

    D3D11_SHADER_RESOURCE_VIEW_DESC svd = {};
    svd.Format                  = Fmt;
    svd.ViewDimension           = D3D11_SRV_DIMENSION_TEXTURE2D;
    svd.Texture2D.MipLevels     = 1;
    if (FAILED(g_pDevice11->CreateShaderResourceView(g_pStagingTex, &svd, &g_pStagingSRV)))
    {
        g_pStagingTex->Release(); g_pStagingTex = nullptr;
        return false;
    }

    g_nStagingW  = W;
    g_nStagingH  = H;
    g_StagingFmt = Fmt;
    return true;
}

static int  g_nStereoMode   = 0;       // 0=unknown, 1=full-height, 2=half-height
static float g_fUvScl        = 0.5f;   // UV scale (0.5=full, 0.25=half)
static ID3D11Texture2D* g_pReadbackTex = nullptr;  // 1x1 STAGING for detection
// Per-game overrides: some game executables deliberately render stereo in a
// non-standard way that confuses our auto-detection.  Allow forcing full-height
// mode for known titles (Sniper Elite series) to avoid regressions.
static bool g_bForceFullHeightForSniper = false;
static bool g_bSniper3Special = false; // special-case handling for Sniper Elite III
static bool g_bForceEgoEngine = false; // force EGO/DiRT/Grid handling (separate list)
static bool g_bHitmanAbsolution = false; // Hitman Absolution: 3D content in top half of each eye slot only
static bool g_bTombRaider2013   = false; // Tomb Raider 2013 (TombRaider.exe): placeholder for game-specific fixes

static void UpdateCB(float uvYOffset)
{
    D3D11_MAPPED_SUBRESOURCE m = {};
    if (SUCCEEDED(g_pContext->Map(g_pCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
    {
        float* p = static_cast<float*>(m.pData);
        p[0] = uvYOffset; p[1] = g_fUvScl; p[2] = p[3] = 0.0f;
        g_pContext->Unmap(g_pCB, 0);
    }
}

// ============================================================
// Present hook
// ============================================================
static bool g_bStereoActive       = false;
static bool g_bQbsCreated         = false;
static bool g_bHooked             = false;

// Expose small control APIs so other in-tree proxies (NVAPI proxy) can
// drive our compositor without duplicating heavy internals.
extern "C" {
__declspec(dllexport) void AmdQbProxy_SetStereoActive(bool active)
{
    g_bStereoActive = active;
    char buf[128];
    wsprintfA(buf, "[AmdQbProxy] External SetStereoActive -> %d\n", (int)active);
    WriteLog(buf);
}

__declspec(dllexport) int AmdQbProxy_IsStereoActive()
{
    return g_bStereoActive ? 1 : 0;
}

__declspec(dllexport) void AmdQbProxy_SetOutputMode(int mode)
{
    if (mode >= 0 && mode <= static_cast<int>(OM_SR_WEAVE)) g_OutputMode = static_cast<OutputMode>(mode);
    char buf[128];
    wsprintfA(buf, "[AmdQbProxy] External SetOutputMode -> %d\n", mode);
    WriteLog(buf);
}

__declspec(dllexport) void AmdQbProxy_SetSeparation(float sep)
{
    // Stub: store as basic log; detailed per-handle convergence/separation
    // can be implemented by extending proxy state if needed.
    char buf[128];
    wsprintfA(buf, "[AmdQbProxy] External SetSeparation -> %f\n", sep);
    WriteLog(buf);
}

__declspec(dllexport) void AmdQbProxy_SetConvergence(float conv)
{
    char buf[128];
    wsprintfA(buf, "[AmdQbProxy] External SetConvergence -> %f\n", conv);
    WriteLog(buf);
}
} // extern "C"

typedef HRESULT (STDMETHODCALLTYPE *PFN_Present)(IDXGISwapChain*, UINT, UINT);
static PFN_Present g_pfnOrigPresent = nullptr;

typedef HRESULT (STDMETHODCALLTYPE *PFN_Present1)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
static PFN_Present1 g_pfnOrigPresent1 = nullptr;

// Per-swap-chain shadow render target.  The real swap chain back buffer stays
// at display resolution; the shadow texture is doubled in height.  Games see
// the shadow via the hooked IDXGISwapChain::GetBuffer; the compositor draws
// SBS from the shadow into the real (display-sized) BB before Present.
struct ShadowData {
    ID3D11Texture2D*          pTex;     // shadow texture (gameW or W) x (2*origH)
    ID3D11ShaderResourceView* pSRV;     // SRV for compositor input
    UINT                      origH;    // original (non-doubled) height
    UINT                      gameW;    // game's intended width (non-zero in full SBS mode only)
    void Release() {
        if (pSRV) { pSRV->Release(); pSRV = nullptr; }
        if (pTex) { pTex->Release(); pTex = nullptr; }
        origH = 0;
        gameW = 0;
    }
};
static std::unordered_map<IDXGISwapChain*, ShadowData> g_scShadow;

static bool g_bPresentLogOnce = false;
static int  g_nPresentCount  = 0;    // Diagnostic: count Present calls

// --- GetBuffer hook: returns the doubled shadow texture instead of the real BB ---
typedef HRESULT (STDMETHODCALLTYPE *PFN_GetBuffer)(IDXGISwapChain*, UINT, REFIID, void**);
static PFN_GetBuffer g_pfnOrigGetBuffer = nullptr;

// --- ResizeBuffers hook ---
typedef HRESULT (STDMETHODCALLTYPE *PFN_ResizeBuffers)(
    IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
static PFN_ResizeBuffers g_pfnOrigResizeBuffers = nullptr;

// --- CreateSwapChain hook: creates shadow render target for stereo ---
typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateSwapChain)(
    IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
static PFN_CreateSwapChain g_pfnOrigCreateSwapChain = nullptr;

// --- GetDesc hook: reports doubled height to game for stereo swap chains ---
typedef HRESULT (STDMETHODCALLTYPE *PFN_GetDesc)(IDXGISwapChain*, DXGI_SWAP_CHAIN_DESC*);
static PFN_GetDesc g_pfnOrigGetDesc = nullptr;

// --- GetDesc1 hook: DXGI 1.1 version ---
typedef HRESULT (STDMETHODCALLTYPE *PFN_GetDesc1)(IDXGISwapChain1*, DXGI_SWAP_CHAIN_DESC1*);
static PFN_GetDesc1 g_pfnOrigGetDesc1 = nullptr;

// --- ResizeTarget hook: prevents doubled height leaking to display mode ---
typedef HRESULT (STDMETHODCALLTYPE *PFN_ResizeTarget)(IDXGISwapChain*, const DXGI_MODE_DESC*);
static PFN_ResizeTarget g_pfnOrigResizeTarget = nullptr;

// Helper: make a format typeless so the shadow supports both UNORM and SRGB RTVs.
static DXGI_FORMAT MakeTypeless(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return DXGI_FORMAT_R8G8B8A8_TYPELESS;
    case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8A8_TYPELESS;
    case DXGI_FORMAT_R10G10B10A2_UNORM: return DXGI_FORMAT_R10G10B10A2_TYPELESS;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return DXGI_FORMAT_R16G16B16A16_TYPELESS;
    default: return f;
    }
}

// Helper: create (or recreate) the shadow texture for a stereo swap chain.
// gameW: non-zero only in full SBS mode — shadow uses game width, not real BB width.
static bool CreateShadow(IDXGISwapChain* pSC, ID3D11Device* pDev,
                          UINT W, UINT origH, DXGI_FORMAT fmt, UINT gameW = 0)
{
    auto& sd = g_scShadow[pSC];
    sd.Release();

    UINT shadowW = gameW ? gameW : W;

    D3D11_TEXTURE2D_DESC td = {};
    td.Width            = shadowW;
    td.Height           = origH * 2;
    td.MipLevels        = 1;
    td.ArraySize        = 1;
    td.Format           = fmt;   // Use typed format so games can create RTVs with DXGI_FORMAT_UNKNOWN
    td.SampleDesc.Count = 1;
    td.Usage            = D3D11_USAGE_DEFAULT;
    td.BindFlags        = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(pDev->CreateTexture2D(&td, nullptr, &sd.pTex)))
    {
        g_scShadow.erase(pSC);
        return false;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC svd = {};
    svd.Format                    = fmt;   // typed format for shader reads
    svd.ViewDimension             = D3D11_SRV_DIMENSION_TEXTURE2D;
    svd.Texture2D.MipLevels       = 1;
    if (FAILED(pDev->CreateShaderResourceView(sd.pTex, &svd, &sd.pSRV)))
    {
        sd.Release();
        g_scShadow.erase(pSC);
        return false;
    }
    sd.origH = origH;
    sd.gameW = gameW;

    char buf[200];
    wsprintfA(buf, "[AmdQbProxy] Shadow created: %ux%u (origH=%u fmt=%u gameW=%u)\n",
              shadowW, origH * 2, origH, fmt, gameW);
    WriteLog(buf);
    return true;
}

static HRESULT STDMETHODCALLTYPE HookedCreateSwapChain(
    IDXGIFactory* pFactory, IUnknown* pDevice,
    DXGI_SWAP_CHAIN_DESC* pDesc, IDXGISwapChain** ppSC)
{
    UINT origH = 0;
    UINT gameW = 0;  // non-zero in full SBS fullscreen mode

    bool fullSBSMode = (g_OutputMode == OM_SBS_FULL);

    if (pDesc && g_bStereoActive)
    {
        origH = pDesc->BufferDesc.Height;
        if (fullSBSMode && !pDesc->Windowed)
        {
            gameW = pDesc->BufferDesc.Width;
            char buf[200];
            wsprintfA(buf, "[AmdQbProxy] CreateSwapChain: %ux%u (fullSBS, doubling width -> %u)\n",
                      gameW, origH, gameW * 2);
            WriteLog(buf);
        }
        else
        {
            char buf[160];
            wsprintfA(buf, "[AmdQbProxy] CreateSwapChain: %ux%u (stereo, shadow mode)\n",
                      pDesc->BufferDesc.Width, origH);
            WriteLog(buf);
        }
    }
    else if (pDesc)
    {
        char buf[128];
        wsprintfA(buf, "[AmdQbProxy] CreateSwapChain: %ux%u (no stereo)\n",
                  pDesc->BufferDesc.Width, pDesc->BufferDesc.Height);
        WriteLog(buf);
    }

    // For full SBS: pass a modified desc with doubled width to the driver so the
    // real BB is 2x game width.  We copy instead of modifying pDesc so the
    // caller's struct stays untouched (modifying it breaks games that read it back).
    DXGI_SWAP_CHAIN_DESC descForDriver;
    DXGI_SWAP_CHAIN_DESC* pDescToPass = pDesc;
    if (gameW > 0 && pDesc)
    {
        descForDriver = *pDesc;
        descForDriver.BufferDesc.Width = gameW * 2;
        pDescToPass = &descForDriver;
    }

    HRESULT hr = g_pfnOrigCreateSwapChain(pFactory, pDevice, pDescToPass, ppSC);
    if (SUCCEEDED(hr) && ppSC && *ppSC && origH > 0)
    {
        IDXGISwapChain* pSC = *ppSC;
        ID3D11Device* pDev = nullptr;
        pDevice->QueryInterface(__uuidof(ID3D11Device),
                                reinterpret_cast<void**>(&pDev));
        if (pDev)
        {
            DXGI_SWAP_CHAIN_DESC desc = {};
            pSC->GetDesc(&desc);
            if (CreateShadow(pSC, pDev, desc.BufferDesc.Width, origH,
                             desc.BufferDesc.Format, gameW))
            {
                // NOTE: Do NOT modify pDesc->BufferDesc.Height here.
                // On real AMD hardware the driver doubles the BB internally
                // but leaves the caller's DXGI_SWAP_CHAIN_DESC struct
                // unchanged.  Games discover the doubled height through
                // GetDesc() (hooked) and GetBuffer() (returns shadow).
                // Modifying pDesc broke Sniper Elite V2 and other Rebellion
                // engine games that compute viewport offsets from the
                // original pDesc value.

                char buf[300];
                wsprintfA(buf, "[AmdQbProxy] CreateSwapChain: shadow %ux%u OK (origH=%u gameW=%u swapEff=%u flags=0x%X windowed=%d)\n",
                          desc.BufferDesc.Width, origH * 2, origH, gameW,
                          desc.SwapEffect, desc.Flags, desc.Windowed);
                WriteLog(buf);
            }
            else
            {
                WriteLog("[AmdQbProxy] CreateSwapChain: shadow creation FAILED\n");
            }
            pDev->Release();
        }
    }
    return hr;
}

// --- ResizeBuffers hook: recreates shadow when game resizes ---
static HRESULT STDMETHODCALLTYPE HookedResizeBuffers(
    IDXGISwapChain* pSC, UINT BufferCount, UINT Width, UINT Height,
    DXGI_FORMAT NewFormat, UINT SwapChainFlags)
{
    // Release old shadow before ResizeBuffers (buffer refs are invalidated)
    auto it = g_scShadow.find(pSC);
    bool wasStereo = (it != g_scShadow.end());
    UINT oldOrigH  = wasStereo ? it->second.origH : 0;
    UINT oldGameW  = wasStereo ? it->second.gameW : 0;
    if (wasStereo) it->second.Release();

    // For full SBS: track game's intended width and double it for the real call.
    UINT callWidth = Width;
    UINT newGameW  = 0;
    if (g_bStereoActive && oldGameW > 0)
    {
        // Preserve game width: if Width==0 keep oldGameW, else use what game passed.
        newGameW = (Width > 0) ? Width : oldGameW;
        if (Width > 0)
            callWidth = Width * 2;  // real BB is 2x game width
        // Width==0 means "keep current" — real BB already doubled, so callWidth stays 0.
    }

    if (g_bStereoActive && Height > 0)
    {
        // Guard: if game passes the doubled shadow height, use origH instead
        if (wasStereo && Height == oldOrigH * 2)
        {
            char buf[160];
            wsprintfA(buf, "[AmdQbProxy] ResizeBuffers: %ux%u (shadow height, using origH=%u)\n",
                      Width, Height, oldOrigH);
            WriteLog(buf);
            Height = oldOrigH;
        }
        char buf[200];
        wsprintfA(buf, "[AmdQbProxy] ResizeBuffers: %ux%u -> real %ux%u (stereo%s)\n",
                  Width, Height, callWidth, Height, oldGameW > 0 ? ", fullSBS" : "");
        WriteLog(buf);
    }
    else if (Width > 0 || Height > 0)
    {
        char buf[128];
        wsprintfA(buf, "[AmdQbProxy] ResizeBuffers: %ux%u (no stereo)\n",
                  Width, Height);
        WriteLog(buf);
    }

    HRESULT hr = g_pfnOrigResizeBuffers(pSC, BufferCount, callWidth, Height,
                                         NewFormat, SwapChainFlags);

    // Recreate shadow after successful resize
    if (SUCCEEDED(hr) && g_bStereoActive && Height > 0)
    {
        ID3D11Device* pDev = nullptr;
        pSC->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&pDev));
        if (pDev)
        {
            DXGI_SWAP_CHAIN_DESC desc = {};
            pSC->GetDesc(&desc);
            DXGI_FORMAT fmt = (NewFormat != DXGI_FORMAT_UNKNOWN)
                              ? NewFormat : desc.BufferDesc.Format;
            CreateShadow(pSC, pDev, desc.BufferDesc.Width, Height, fmt, newGameW);
            pDev->Release();
        }
    }
    return hr;
}

// --- GetBuffer hook: return the doubled shadow texture instead of the real BB ---
static HRESULT STDMETHODCALLTYPE HookedGetBuffer(
    IDXGISwapChain* pSC, UINT Buffer, REFIID riid, void** ppSurface)
{
    if (Buffer == 0 && ppSurface)
    {
        auto it = g_scShadow.find(pSC);
        if (it != g_scShadow.end() && it->second.pTex)
            return it->second.pTex->QueryInterface(riid, ppSurface);
    }
    return g_pfnOrigGetBuffer(pSC, Buffer, riid, ppSurface);
}

// --- GetDesc hook: report doubled height for stereo swap chains ---
// On real AMD HD3D hardware the driver doubles the actual swap chain BB height,
// so GetDesc reports the doubled value.  Our shadow approach keeps the real BB
// at display resolution but must report doubled height so games set up their
// viewports / post-processing to cover the full T&B shadow texture.
static HRESULT STDMETHODCALLTYPE HookedGetDesc(
    IDXGISwapChain* pSC, DXGI_SWAP_CHAIN_DESC* pDesc)
{
    HRESULT hr = g_pfnOrigGetDesc(pSC, pDesc);
    if (SUCCEEDED(hr) && pDesc)
    {
        auto it = g_scShadow.find(pSC);
        if (it != g_scShadow.end() && it->second.origH > 0)
        {
            pDesc->BufferDesc.Height = it->second.origH * 2;
            if (it->second.gameW > 0)
                pDesc->BufferDesc.Width = it->second.gameW;
            static bool s_bLogOnce = false;
            if (!s_bLogOnce)
            {
                s_bLogOnce = true;
                char buf[200];
                wsprintfA(buf, "[AmdQbProxy] GetDesc: reporting %ux%u (origH=%u gameW=%u)\n",
                          pDesc->BufferDesc.Width, pDesc->BufferDesc.Height,
                          it->second.origH, it->second.gameW);
                WriteLog(buf);
            }
        }
    }
    return hr;
}

// --- GetDesc1 hook: DXGI 1.1 version ---
static HRESULT STDMETHODCALLTYPE HookedGetDesc1(
    IDXGISwapChain1* pSC1, DXGI_SWAP_CHAIN_DESC1* pDesc)
{
    HRESULT hr = g_pfnOrigGetDesc1(pSC1, pDesc);
    if (SUCCEEDED(hr) && pDesc)
    {
        auto it = g_scShadow.find(static_cast<IDXGISwapChain*>(pSC1));
        if (it != g_scShadow.end() && it->second.origH > 0)
        {
            pDesc->Height = it->second.origH * 2;
            if (it->second.gameW > 0)
                pDesc->Width = it->second.gameW;
        }
    }
    return hr;
}

// --- ResizeTarget hook: halve doubled height to prevent bad display mode changes ---
// Games that read our doubled GetDesc height may pass it to ResizeTarget, which
// would attempt to change the display mode to an impossibly tall resolution.
static HRESULT STDMETHODCALLTYPE HookedResizeTarget(
    IDXGISwapChain* pSC, const DXGI_MODE_DESC* pNewTargetParameters)
{
    if (pNewTargetParameters)
    {
        auto it = g_scShadow.find(pSC);
        if (it != g_scShadow.end())
        {
            bool needModify = false;
            DXGI_MODE_DESC modified = *pNewTargetParameters;

            // Halve doubled height the game may have read back from our hooked GetDesc.
            if (it->second.origH > 0 && modified.Height == it->second.origH * 2)
            {
                modified.Height = it->second.origH;
                needModify = true;
            }

            // Double game width in full SBS mode so the display mode matches the real BB.
            if (it->second.gameW > 0 && modified.Width == it->second.gameW)
            {
                modified.Width = it->second.gameW * 2;
                needModify = true;
            }

            if (needModify)
            {
                char buf[200];
                wsprintfA(buf, "[AmdQbProxy] ResizeTarget: %ux%u -> %ux%u\n",
                          pNewTargetParameters->Width, pNewTargetParameters->Height,
                          modified.Width, modified.Height);
                WriteLog(buf);
                return g_pfnOrigResizeTarget(pSC, &modified);
            }
        }
    }
    return g_pfnOrigResizeTarget(pSC, pNewTargetParameters);
}

// Lazily create a shadow-fixup texture matching the shadow's dimensions.
// shadowH is the full shadow height (origH * 2). Returned texture has the
// same size and format as the shadow itself.
static bool EnsureShadowFixup(UINT W, UINT shadowH, DXGI_FORMAT Fmt)
{
    if (g_pShadowFixupTex && g_nFixupW == W && g_nFixupH == shadowH && g_FixupFmt == Fmt)
        return true;

    CleanupShadowFixup();

    D3D11_TEXTURE2D_DESC td = {};
    td.Width      = W;
    td.Height     = shadowH;
    td.MipLevels  = 1;
    td.ArraySize  = 1;
    td.Format     = Fmt;
    td.SampleDesc = { 1, 0 };
    td.Usage      = D3D11_USAGE_DEFAULT;
    td.BindFlags  = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(g_pDevice11->CreateTexture2D(&td, nullptr, &g_pShadowFixupTex))) return false;

    D3D11_RENDER_TARGET_VIEW_DESC rtvd = {};
    rtvd.Format        = Fmt;
    rtvd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    if (FAILED(g_pDevice11->CreateRenderTargetView(g_pShadowFixupTex, &rtvd, &g_pShadowFixupRTV))) return false;

    D3D11_SHADER_RESOURCE_VIEW_DESC srvd = {};
    srvd.Format              = Fmt;
    srvd.ViewDimension       = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvd.Texture2D.MipLevels = 1;
    if (FAILED(g_pDevice11->CreateShaderResourceView(g_pShadowFixupTex, &srvd, &g_pShadowFixupSRV))) return false;

    g_nFixupW = W; g_nFixupH = shadowH; g_FixupFmt = Fmt;
    return true;
}

static bool EnsureSRSBSTexture(UINT W, UINT H, DXGI_FORMAT Fmt)
{
    if (g_pSRSBSTex && g_nSRSBSW == W * 2 && g_nSRSBSH == H && g_SRSBSFmt == Fmt)
        return true;

    if (g_pSRSBSSRV) { g_pSRSBSSRV->Release(); g_pSRSBSSRV = nullptr; }
    if (g_pSRSBSRTV) { g_pSRSBSRTV->Release(); g_pSRSBSRTV = nullptr; }
    if (g_pSRSBSTex) { g_pSRSBSTex->Release(); g_pSRSBSTex = nullptr; }
    g_nSRSBSW = 0; g_nSRSBSH = 0; g_SRSBSFmt = DXGI_FORMAT_UNKNOWN;

    D3D11_TEXTURE2D_DESC td = {};
    td.Width          = W * 2;
    td.Height         = H;
    td.MipLevels      = 1;
    td.ArraySize      = 1;
    td.Format         = Fmt;
    td.SampleDesc     = { 1, 0 };
    td.Usage          = D3D11_USAGE_DEFAULT;
    td.BindFlags      = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(g_pDevice11->CreateTexture2D(&td, nullptr, &g_pSRSBSTex))) return false;

    D3D11_RENDER_TARGET_VIEW_DESC rtvd = {};
    rtvd.Format        = Fmt;
    rtvd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    if (FAILED(g_pDevice11->CreateRenderTargetView(g_pSRSBSTex, &rtvd, &g_pSRSBSRTV))) return false;

    D3D11_SHADER_RESOURCE_VIEW_DESC srvd = {};
    srvd.Format                    = Fmt;
    srvd.ViewDimension             = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvd.Texture2D.MipLevels       = 1;
    if (FAILED(g_pDevice11->CreateShaderResourceView(g_pSRSBSTex, &srvd, &g_pSRSBSSRV))) return false;

    g_nSRSBSW  = W * 2;
    g_nSRSBSH  = H;
    g_SRSBSFmt = Fmt;

    // Bind the (new) SRV once here rather than every Present — the SRV only
    // changes when this function recreates the SBS texture.
    if (g_pSRInterface)
        g_pSRInterface->SetInputTexture(g_pSRSBSSRV);

    return true;
}

static bool EnsureSRWeaver(HWND hWnd)
{
    if (g_pSRInterface) return true;

    // CreateSRInterfaceDX11 manages the SR lifecycle internally and
    // probes runtime availability via LoadLibrary before touching any of the
    // delay-loaded SR DLLs — returns E_NOINTERFACE when the runtime is absent
    // (the soft path that lets us downgrade to Half SBS instead of crashing),
    // or another failing HRESULT for server-down / no-SR-display / etc.
    ID3D11DeviceContext* pCtx = nullptr;
    g_pDevice11->GetImmediateContext(&pCtx);
    HRESULT hr = SimulatedReality::CreateSRInterfaceDX11(pCtx, hWnd, &g_pSRInterface);
    pCtx->Release();

    if (FAILED(hr) || !g_pSRInterface)
    {
        // Sticky downgrade: once SR is unavailable for this session, fall back
        // to Half SBS so we don't retry every frame. 
        static bool s_bLoggedSRFail = false;
        if (!s_bLoggedSRFail) { s_bLoggedSRFail = true;
            char buf[220];
            wsprintfA(buf, "[AmdQbProxy] SR: CreateSRInterfaceDX11 failed (hr=0x%08X hWnd=%p); downgrading OM_SR_WEAVE -> OM_SBS_HALF\n", (unsigned)hr, hWnd);
            WriteLog(buf);
        }
        g_OutputMode = OM_SBS_HALF;
        if (g_pSRInterface) { g_pSRInterface->Delete(); g_pSRInterface = nullptr; }
        return false;
    }

    char buf[200];
    wsprintfA(buf, "[AmdQbProxy] SR: DX11 weaver ready (hWnd=%p sri=%p)\n", hWnd, g_pSRInterface);
    WriteLog(buf);
    return true;
}

static HRESULT STDMETHODCALLTYPE HookedPresent(IDXGISwapChain* pSC, UINT SyncInterval, UINT Flags)
{
    // Heartbeat: count ALL Present calls, even early-returned ones
    static int s_nHeartbeat = 0;
    s_nHeartbeat++;
    if (s_nHeartbeat == 50 || s_nHeartbeat == 100 || s_nHeartbeat == 500)
    {
        char buf[200];
        wsprintfA(buf, "[AmdQbProxy] Present heartbeat #%d: SC=%p stereo=%d dev=%p\n",
                  s_nHeartbeat, pSC, (int)g_bStereoActive, g_pDevice11);
        WriteLog(buf);
    }

    if (!g_bStereoActive || !g_pContext || !g_pDevice11)
        return g_pfnOrigPresent(pSC, SyncInterval, Flags);

    // One-time: confirm which per-game flags are active so the log is definitive.
    {
        static bool s_bFlagsLogged = false;
        if (!s_bFlagsLogged)
        {
            s_bFlagsLogged = true;
            char buf[200];
            wsprintfA(buf, "[AmdQbProxy] First stereo present: hitman=%d sniper=%d ego=%d mode=%d\n",
                      (int)g_bHitmanAbsolution, (int)g_bForceFullHeightForSniper,
                      (int)g_bForceEgoEngine, (int)g_OutputMode);
            WriteLog(buf);
        }
    }

    // Get the REAL back buffer (at display resolution) via original GetBuffer,
    // bypassing our hook which would return the shadow texture.
    ID3D11Texture2D* pBB = nullptr;
    if (!g_pfnOrigGetBuffer ||
        FAILED(g_pfnOrigGetBuffer(pSC, 0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&pBB))))
        return g_pfnOrigPresent(pSC, SyncInterval, Flags);

    D3D11_TEXTURE2D_DESC bbDesc;
    pBB->GetDesc(&bbDesc);

    // Skip MSAA back buffers — they need ResolveSubresource first
    if (bbDesc.SampleDesc.Count > 1)
    {
        pBB->Release();
        return g_pfnOrigPresent(pSC, SyncInterval, Flags);
    }

    UINT W = bbDesc.Width, H = bbDesc.Height;

    // Look up per-swap-chain shadow data
    auto itShadow = g_scShadow.find(pSC);
    UINT origH = (itShadow != g_scShadow.end()) ? itShadow->second.origH : 0;
    ID3D11ShaderResourceView* pShadowSRV = (itShadow != g_scShadow.end()) ? itShadow->second.pSRV : nullptr;
    ID3D11Texture2D*          pShadowTex = (itShadow != g_scShadow.end()) ? itShadow->second.pTex : nullptr;

    // Diagnostic: log the first several Present calls to diagnose multi-swapchain issues
    g_nPresentCount++;
    if (g_nPresentCount <= 10)
    {
        DXGI_SWAP_CHAIN_DESC scDesc = {};
        pSC->GetDesc(&scDesc);
        char buf[400];
        wsprintfA(buf, "[AmdQbProxy] Present #%d: SC=%p BB=%ux%u fmt=%u origH=%u windowed=%d swapEff=%u flags=0x%X outWnd=%p\n",
                  g_nPresentCount, pSC, W, H, bbDesc.Format, origH,
                  scDesc.Windowed, scDesc.SwapEffect, scDesc.Flags, scDesc.OutputWindow);
        WriteLog(buf);
    }

    // Only composite swap chains that have a stereo shadow texture.
    if (origH == 0 || !pShadowTex || !pShadowSRV)
    {
        pBB->Release();
        return g_pfnOrigPresent(pSC, SyncInterval, Flags);
    }

    if (!g_bPresentLogOnce)
    {
        g_bPresentLogOnce = true;
        char buf[300];
        wsprintfA(buf, "[AmdQbProxy] Present: realBB %ux%u fmt=%u mode=%d origH=%u shadow=%ux%u bind=0x%X\n",
                  W, H, bbDesc.Format, g_OutputMode, origH,
                  W, origH * 2, bbDesc.BindFlags);
        WriteLog(buf);
    }

    // --- One-time: log display mode for diagnostic ---
    {
        static bool s_bDisplayLogDone = false;
        if (!s_bDisplayLogDone && origH > 0)
        {
            s_bDisplayLogDone = true;
            DEVMODE dm = {};
            dm.dmSize = sizeof(dm);
            EnumDisplaySettingsW(NULL, ENUM_CURRENT_SETTINGS, &dm);
            DXGI_SWAP_CHAIN_DESC rtDesc = {};
            pSC->GetDesc(&rtDesc);
            char rtBuf[300];
            wsprintfA(rtBuf, "[AmdQbProxy] Display: %ux%u@%uHz realBB=%ux%u shadow=%ux%u windowed=%d\n",
                      dm.dmPelsWidth, dm.dmPelsHeight, dm.dmDisplayFrequency,
                      rtDesc.BufferDesc.Width, rtDesc.BufferDesc.Height,
                      W, origH * 2, rtDesc.Windowed);
            WriteLog(rtBuf);
        }
    }

    // --- Device-from-BB check: ensure compositor uses the device that owns the BB ---
    {
        ID3D11Device* pDevBB = nullptr;
        pBB->GetDevice(&pDevBB);
        if (pDevBB && pDevBB != g_pDevice11)
        {
            static bool s_bMismatchLogOnce = false;
            if (!s_bMismatchLogOnce)
            {
                s_bMismatchLogOnce = true;
                char buf[256];
                wsprintfA(buf, "[AmdQbProxy] Device mismatch! BB device=%p, g_pDevice11=%p — switching\n",
                          pDevBB, g_pDevice11);
                WriteLog(buf);
            }
            ReleaseCompositorResources();
            if (g_pReadbackTex) { g_pReadbackTex->Release(); g_pReadbackTex = nullptr; }
            g_nStereoMode = 0; g_fUvScl = 0.5f;  // reset detection for new device
            if (g_pContext)  { g_pContext->Release();  g_pContext  = nullptr; }
            if (g_pDevice11) { g_pDevice11->Release(); g_pDevice11 = nullptr; }
            g_pDevice11 = pDevBB;   // take ownership of the ref from GetDevice
            g_pDevice11->GetImmediateContext(&g_pContext);
        }
        else if (pDevBB)
        {
            pDevBB->Release();  // same device, release the extra ref
        }
    }

    if (!EnsureShaders())
    {
        WriteLog("[AmdQbProxy] Compositor FAILED: shaders not ready\n");
        pBB->Release();
        return g_pfnOrigPresent(pSC, SyncInterval, Flags);
    }

    // Shadow texture already has the game's T&B content (game renders to it via
    // our hooked GetBuffer).  No copy needed — we read the shadow directly as SRV.
    UINT shadowH = origH * 2;  // Shadow texture height

    // --- Auto-detect content presence (reads from shadow) ---
    {
        // Per-process exception checks (one-time)
        static bool s_bSniperCheckDone = false;
        if (!s_bSniperCheckDone)
        {
            s_bSniperCheckDone = true;
            wchar_t procPath[MAX_PATH] = {};
            if (GetModuleFileNameW(nullptr, procPath, MAX_PATH))
            {
                // lowercase for simple case-insensitive substring search
                for (wchar_t* p = procPath; *p; ++p) *p = towlower(*p);
                {
                    char buf[MAX_PATH + 64];
                    int n = WideCharToMultiByte(CP_UTF8, 0, procPath, -1, nullptr, 0, nullptr, nullptr);
                    char ascii[MAX_PATH] = {};
                    if (n > 0 && n < (int)sizeof(ascii))
                        WideCharToMultiByte(CP_UTF8, 0, procPath, -1, ascii, n, nullptr, nullptr);
                    wsprintfA(buf, "[AmdQbProxy] Detection: procPath=%s\n", ascii);
                    WriteLog(buf);
                }
                // Special-case Sniper Elite III: use a tailored detection path
                if (wcsstr(procPath, L"sniperelite3.exe"))
                {
                    g_bSniper3Special = true;
                    WriteLog("[AmdQbProxy] Sniper Elite III detected (explicit)\n");
                }

                // Match known Sniper/ZA executables (lowercased procPath)
                const wchar_t* sniperNames[] = {
                    L"sniperelitev2.exe",
                    L"sniperelite4_dx11.exe",
                    L"sniperelite4_dx12.exe",
                    L"nza.exe",
                    L"nza2.exe",
                    L"zat.exe",
                    // L"dirt3_game.exe",
                    // L"dirt3.exe"
                };
                for (size_t si = 0; si < _countof(sniperNames); ++si)
                {
                    if (wcsstr(procPath, sniperNames[si]))
                    {
                        g_bForceFullHeightForSniper = true;
                        WriteLog("[AmdQbProxy] Forcing full-height mode for sniper executable\n");
                        break;
                    }
                }

                // EGO / DiRT / GRID engine family override (separate list)
                const wchar_t* egoNames[] = {
                    L"grid2.exe",
                    L"grid_autosport.exe",
                    L"dirt2_game.exe",
                    L"dirt3.exe",
                    L"dirt3_game.exe",
                    L"drt.exe",
                    L"dirt4.exe",
                    L"showdown_avx.exe",
                    L"showdown.exe"
                };
                for (size_t ei = 0; ei < _countof(egoNames); ++ei)
                {
                    if (wcsstr(procPath, egoNames[ei]))
                    {
                        g_bForceEgoEngine = true;
                        WriteLog("[AmdQbProxy] EGO engine title detected: applying EGO overrides\n");
                        break;
                    }
                }

                // Hitman Absolution (Glacier engine): renders each eye's 3D into only
                // the top half of its allocated T&B shadow area; needs separate sampling.
                // Executable is HMA.exe (not hitmanabsolution.exe).
                if (wcsstr(procPath, L"hma.exe"))
                {
                    g_bHitmanAbsolution = true;
                    WriteLog("[AmdQbProxy] Hitman Absolution detected: half-height-per-eye sampling\n");
                }

                // Tomb Raider 2013 (Crystal Dynamics / Square Enix).
                // Placeholder for game-specific compositor fixes — logic to be added once
                // the exact rendering layout is diagnosed.
                if (wcsstr(procPath, L"tombraider.exe"))
                {
                    g_bTombRaider2013 = true;
                    WriteLog("[AmdQbProxy] Tomb Raider 2013 detected\n");
                }
            }
        }
        static UINT s_lastW = 0, s_lastOrigH = 0;
        static int  s_nDetectFrame = 0;
        static bool s_bHalfDetectDone = false;
        if (W != s_lastW || origH != s_lastOrigH)
        {
            s_lastW = W; s_lastOrigH = origH;
            g_nStereoMode = 0;
            g_fUvScl = 0.5f;
            s_nDetectFrame = 0;
            s_bHalfDetectDone = false;
            if (g_pReadbackTex) { g_pReadbackTex->Release(); g_pReadbackTex = nullptr; }
        }
        ++s_nDetectFrame;
        if (g_nStereoMode == 0 && s_nDetectFrame >= 3 && s_nDetectFrame <= 120 && origH > 0)
        {
            if (!g_pReadbackTex)
            {
                D3D11_TEXTURE2D_DESC rd = {};
                rd.Width = 1; rd.Height = 1; rd.MipLevels = 1; rd.ArraySize = 1;
                rd.Format = bbDesc.Format; rd.SampleDesc.Count = 1;
                rd.Usage  = D3D11_USAGE_STAGING;
                rd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                g_pDevice11->CreateTexture2D(&rd, nullptr, &g_pReadbackTex);
            }
            if (g_pReadbackTex)
            {
                DWORD rgbMask = 0x00FFFFFF;
                if (bbDesc.Format == DXGI_FORMAT_R10G10B10A2_UNORM ||
                    bbDesc.Format == DXGI_FORMAT_R10G10B10A2_UINT)
                    rgbMask = 0x3FFFFFFF;

                UINT contentY = origH / 4;
                D3D11_BOX box1 = { W / 2, contentY, 0, W / 2 + 1, contentY + 1, 1 };
                g_pContext->CopySubresourceRegion(g_pReadbackTex, 0, 0, 0, 0,
                                                   pShadowTex, 0, &box1);
                D3D11_MAPPED_SUBRESOURCE mapped = {};
                DWORD contentPx = 0;
                if (SUCCEEDED(g_pContext->Map(g_pReadbackTex, 0, D3D11_MAP_READ, 0, &mapped)))
                {
                    contentPx = (*static_cast<DWORD*>(mapped.pData)) & rgbMask;
                    g_pContext->Unmap(g_pReadbackTex, 0);
                }
                if (contentPx != 0)
                {
                    if (g_bSniper3Special)
                    {
                        // Sniper Elite III: historic behavior requires a slightly
                        // different detection sequence.  Instead of immediately
                        // switching to full-height, allow the half-height check to
                        // run but reduce its aggressiveness by requiring 3 matching
                        // samples and a brighter threshold.  We simply log here and
                        // let the subsequent block (half-height detection) decide.
                        char dbuf[200];
                        wsprintfA(dbuf, "[AmdQbProxy] Sniper3 special: observed content=0x%08X frame=%d\n",
                                  contentPx, s_nDetectFrame);
                        WriteLog(dbuf);
                    }
                    else
                    {
                        // Default: treat as full-height game initially
                        g_nStereoMode = 1;
                        g_fUvScl = 0.5f;
                        char dbuf[200];
                        wsprintfA(dbuf, "[AmdQbProxy] Stereo detect: FULL height (content=0x%08X frame=%d)\n",
                                  contentPx, s_nDetectFrame);
                        WriteLog(dbuf);
                        if (g_bForceFullHeightForSniper)
                        {
                            WriteLog("[AmdQbProxy] Sniper override active: keeping full-height mode\n");
                        }
                    }
                    g_pReadbackTex->Release(); g_pReadbackTex = nullptr;
                }
            }
        }

        // Half-height T&B detection: some games (TR 2013, EGO engine) render
        // T&B at origH into the shadow instead of 2*origH.  Both eyes are packed
        // into [0, origH) and the bottom half [origH, 2*origH) is a stale copy.
        // Detect by comparing corresponding pixels in the top and bottom halves.
        if (g_nStereoMode == 1 && !s_bHalfDetectDone && s_nDetectFrame >= 10 && origH > 0)
        {
            s_bHalfDetectDone = true;
            if (!g_pReadbackTex)
            {
                D3D11_TEXTURE2D_DESC rd = {};
                rd.Width = 1; rd.Height = 1; rd.MipLevels = 1; rd.ArraySize = 1;
                rd.Format = bbDesc.Format; rd.SampleDesc.Count = 1;
                rd.Usage  = D3D11_USAGE_STAGING;
                rd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                g_pDevice11->CreateTexture2D(&rd, nullptr, &g_pReadbackTex);
            }
            if (g_pReadbackTex)
            {
                DWORD rgbMask = 0x00FFFFFF;
                if (bbDesc.Format == DXGI_FORMAT_R10G10B10A2_UNORM ||
                    bbDesc.Format == DXGI_FORMAT_R10G10B10A2_UINT)
                    rgbMask = 0x3FFFFFFF;

                // Compare multiple pixels at corresponding positions
                // in the top half [0, origH) and bottom half [origH, 2*origH).
                // Use 3 samples to reduce false positives (some games have
                // identical scanlines in a couple of locations). For Sniper Elite
                // III (special-case), demand a stronger brightness threshold.
                UINT testY[3] = { origH / 4, origH / 2, origH * 3 / 4 };
                bool allMatch = true;
                DWORD topPx[3] = {}, botPx[3] = {};
                for (int i = 0; i < 3 && allMatch; ++i)
                {
                    // Top half pixel
                    D3D11_BOX boxT = { W / 2, testY[i], 0, W / 2 + 1, testY[i] + 1, 1 };
                    g_pContext->CopySubresourceRegion(g_pReadbackTex, 0, 0, 0, 0,
                                                       pShadowTex, 0, &boxT);
                    D3D11_MAPPED_SUBRESOURCE mapped = {};
                    if (SUCCEEDED(g_pContext->Map(g_pReadbackTex, 0, D3D11_MAP_READ, 0, &mapped)))
                    {
                        topPx[i] = (*static_cast<DWORD*>(mapped.pData)) & rgbMask;
                        g_pContext->Unmap(g_pReadbackTex, 0);
                    }
                    // Bottom half pixel at same relative position
                    D3D11_BOX boxB = { W / 2, origH + testY[i], 0, W / 2 + 1, origH + testY[i] + 1, 1 };
                    g_pContext->CopySubresourceRegion(g_pReadbackTex, 0, 0, 0, 0,
                                                       pShadowTex, 0, &boxB);
                    if (SUCCEEDED(g_pContext->Map(g_pReadbackTex, 0, D3D11_MAP_READ, 0, &mapped)))
                    {
                        botPx[i] = (*static_cast<DWORD*>(mapped.pData)) & rgbMask;
                        g_pContext->Unmap(g_pReadbackTex, 0);
                    }
                    // Reject near-black pixels to avoid false positives
                    // (e.g. Hitman Absolution dark loading screens: 0x00030002 sum=5)
                    BYTE cR = topPx[i] & 0xFF;
                    BYTE cG = (topPx[i] >> 8) & 0xFF;
                    BYTE cB = (topPx[i] >> 16) & 0xFF;
                    UINT bright = (UINT)cR + cG + cB;
                    UINT minBright = g_bSniper3Special ? 96u : 48u; // Sniper3 needs brighter samples
                    if (topPx[i] != botPx[i] || bright < minBright)
                        allMatch = false;
                }
                if (allMatch)
                {
                    g_nStereoMode = 2;  // half-height T&B
                    g_fUvScl = 0.25f;   // each eye occupies 1/4 of shadow UV
                    char dbuf[300];
                    wsprintfA(dbuf, "[AmdQbProxy] Half-height T&B detected: top(%u,%u)=0x%08X=bot(%u,%u)=0x%08X  top(%u,%u)=0x%08X=bot(%u,%u)=0x%08X\n",
                              W/2, testY[0], topPx[0], W/2, origH+testY[0], botPx[0],
                              W/2, testY[1], topPx[1], W/2, origH+testY[1], botPx[1]);
                    WriteLog(dbuf);
                }
                else
                {
                    char dbuf[300];
                    wsprintfA(dbuf, "[AmdQbProxy] Half-height check: NO (top0=0x%08X bot0=0x%08X top1=0x%08X bot1=0x%08X)\n",
                              topPx[0], botPx[0], topPx[1], botPx[1]);
                    WriteLog(dbuf);
                }
                g_pReadbackTex->Release(); g_pReadbackTex = nullptr;
            }
        }
    }

    // --- One-time T&B layout diagnostic (delayed to frame 60 for real content) ---
    {
        static bool s_bDiagDone = false;
        if (!s_bDiagDone && g_nStereoMode != 0 && origH > 0 && g_nPresentCount >= 60)
        {
            s_bDiagDone = true;
            ID3D11Texture2D* pDiag = nullptr;
            D3D11_TEXTURE2D_DESC rd = {};
            rd.Width = 1; rd.Height = 1; rd.MipLevels = 1; rd.ArraySize = 1;
            rd.Format = bbDesc.Format; rd.SampleDesc.Count = 1;
            rd.Usage  = D3D11_USAGE_STAGING;
            rd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            g_pDevice11->CreateTexture2D(&rd, nullptr, &pDiag);
            if (pDiag)
            {
                UINT py[5] = { origH/4, origH*3/4, origH, origH+origH/4, origH+origH*3/4 };
                const char* lbl[5] = { "topL", "botL", "split", "topR", "botR" };
                char line[600];
                wsprintfA(line, "[AmdQbProxy] T&B diag (W=%u origH=%u shadowH=%u fmt=%u):",
                          W, origH, shadowH, bbDesc.Format);
                WriteLog(line);
                for (int i = 0; i < 5; ++i)
                {
                    D3D11_BOX box = { W/2, py[i], 0, W/2+1, py[i]+1, 1 };
                    g_pContext->CopySubresourceRegion(pDiag, 0, 0, 0, 0,
                                                      pShadowTex, 0, &box);
                    D3D11_MAPPED_SUBRESOURCE mapped = {};
                    DWORD px = 0;
                    if (SUCCEEDED(g_pContext->Map(pDiag, 0, D3D11_MAP_READ, 0, &mapped)))
                    {
                        px = *static_cast<DWORD*>(mapped.pData);
                        g_pContext->Unmap(pDiag, 0);
                    }
                    wsprintfA(line, " %s(%u,%u)=0x%08X", lbl[i], W/2, py[i], px);
                    WriteLog(line);
                }
                WriteLog("\n");
                pDiag->Release();
            }
        }
    }

    // Create RTV for back buffer (we will overwrite it with SBS output)
    ID3D11RenderTargetView* pRTV = nullptr;
    HRESULT hrRTV = g_pDevice11->CreateRenderTargetView(pBB, nullptr, &pRTV);
    if (FAILED(hrRTV))
    {
        static bool s_bRTVFailLog = false;
        if (!s_bRTVFailLog)
        {
            s_bRTVFailLog = true;
            char buf[200];
            wsprintfA(buf, "[AmdQbProxy] CreateRTV FAILED hr=0x%08X bind=0x%X\n",
                      (unsigned)hrRTV, bbDesc.BindFlags);
            WriteLog(buf);
        }
        pBB->Release();
        return g_pfnOrigPresent(pSC, SyncInterval, Flags);
    }
    pBB->Release();

    // ---- Save render state ----
    ID3D11RenderTargetView*   pOldRTV  = nullptr;
    ID3D11DepthStencilView*   pOldDSV  = nullptr;
    UINT                      numVPs   = 1;
    D3D11_VIEWPORT            oldVP    = {};
    ID3D11VertexShader*       pOldVS   = nullptr;
    ID3D11PixelShader*        pOldPS   = nullptr;
    ID3D11InputLayout*        pOldIL   = nullptr;
    D3D11_PRIMITIVE_TOPOLOGY  oldTopo  = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ID3D11Buffer*             pOldCB   = nullptr;
    ID3D11ShaderResourceView* pOldSRV  = nullptr;
    ID3D11SamplerState*       pOldSmp  = nullptr;
    ID3D11DepthStencilState*  pOldDS   = nullptr; UINT oldSRef = 0;
    ID3D11RasterizerState*    pOldRS   = nullptr;
    ID3D11BlendState*         pOldBS   = nullptr; FLOAT oldBF[4] = {}; UINT oldSM = 0;

    g_pContext->OMGetRenderTargets(1, &pOldRTV, &pOldDSV);
    g_pContext->RSGetViewports(&numVPs, &oldVP);
    g_pContext->VSGetShader(&pOldVS, nullptr, nullptr);
    g_pContext->PSGetShader(&pOldPS, nullptr, nullptr);
    g_pContext->IAGetInputLayout(&pOldIL);
    g_pContext->IAGetPrimitiveTopology(&oldTopo);
    g_pContext->PSGetConstantBuffers(0, 1, &pOldCB);
    g_pContext->PSGetShaderResources(0, 1, &pOldSRV);
    g_pContext->PSGetSamplers(0, 1, &pOldSmp);
    g_pContext->OMGetDepthStencilState(&pOldDS, &oldSRef);
    g_pContext->RSGetState(&pOldRS);
    g_pContext->OMGetBlendState(&pOldBS, oldBF, &oldSM);

    // ---- Set compositor state ----
    // Set RTV first to unbind the shadow from any output slots,
    // then bind the shadow SRV as input.
    g_pContext->IASetInputLayout(nullptr);
    g_pContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_pContext->VSSetShader(g_pVS, nullptr, 0);
    g_pContext->PSSetShader(g_pPS, nullptr, 0);
    g_pContext->OMSetRenderTargets(1, &pRTV, nullptr);
    g_pContext->PSSetShaderResources(0, 1, &pShadowSRV);
    g_pContext->PSSetSamplers(0, 1, &g_pSampler);
    g_pContext->OMSetDepthStencilState(g_pDSState, 0);
    g_pContext->RSSetState(g_pRSState);
    g_pContext->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);

    // One-time compositor diagnostics
    static bool s_bCompositorLogOnce = false;
    if (!s_bCompositorLogOnce)
    {
        s_bCompositorLogOnce = true;
        char buf[300];
        wsprintfA(buf, "[AmdQbProxy] Compositor OK: shadow=%p srv=%p vs=%p ps=%p cb=%p rtv=%p realBB=%ux%u\n",
                  pShadowTex, pShadowSRV, g_pVS, g_pPS, g_pCB, pRTV, W, H);
        WriteLog(buf);
    }

    // Clear back buffer before compositing
    float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    g_pContext->ClearRenderTargetView(pRTV, black);

    // --- One-time post-clear + post-copy verification diagnostic ---
    {
        static bool s_bVerifyDone = false;
        if (!s_bVerifyDone && g_nPresentCount >= 60)
        {
            s_bVerifyDone = true;
            ID3D11Texture2D* pVerify = nullptr;
            D3D11_TEXTURE2D_DESC vd = {};
            vd.Width = 1; vd.Height = 1; vd.MipLevels = 1; vd.ArraySize = 1;
            vd.Format = bbDesc.Format; vd.SampleDesc.Count = 1;
            vd.Usage  = D3D11_USAGE_STAGING;
            vd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            g_pDevice11->CreateTexture2D(&vd, nullptr, &pVerify);
            if (pVerify)
            {
                // Verify shadow content: read center pixel from shadow texture
                DWORD shPx = 0xDEADBEEF;
                D3D11_BOX bSh = { W/2, origH/2, 0, W/2+1, origH/2+1, 1 };
                g_pContext->CopySubresourceRegion(pVerify, 0, 0, 0, 0,
                                                   pShadowTex, 0, &bSh);
                D3D11_MAPPED_SUBRESOURCE mapped = {};
                if (SUCCEEDED(g_pContext->Map(pVerify, 0, D3D11_MAP_READ, 0, &mapped)))
                {
                    shPx = *static_cast<DWORD*>(mapped.pData);
                    g_pContext->Unmap(pVerify, 0);
                }

                // Verify clear: read center pixel from real BB after ClearRTV
                ID3D11Texture2D* pClrBB = nullptr;
                DWORD clrPx = 0xDEADBEEF;
                if (g_pfnOrigGetBuffer &&
                    SUCCEEDED(g_pfnOrigGetBuffer(pSC, 0, __uuidof(ID3D11Texture2D),
                              reinterpret_cast<void**>(&pClrBB))))
                {
                    D3D11_BOX bClr = { W/2, H/2, 0, W/2+1, H/2+1, 1 };
                    g_pContext->CopySubresourceRegion(pVerify, 0, 0, 0, 0,
                                                       pClrBB, 0, &bClr);
                    if (SUCCEEDED(g_pContext->Map(pVerify, 0, D3D11_MAP_READ, 0, &mapped)))
                    {
                        clrPx = *static_cast<DWORD*>(mapped.pData);
                        g_pContext->Unmap(pVerify, 0);
                    }
                    pClrBB->Release();
                }

                char buf[300];
                wsprintfA(buf, "[AmdQbProxy] Verify: shadow(%u,%u)=0x%08X  postClear(%u,%u)=0x%08X\n",
                          W/2, origH/2, shPx, W/2, H/2, clrPx);
                WriteLog(buf);
                pVerify->Release();
            }
        }
    }

    // ============================================================
    // [DISABLED — kept for future reference. Universal pre-compositor "shadow
    //  fixup" pass that render-stretched each eye's top-half content to fill
    //  its full eye slot in g_pShadowFixupTex, then redirected compositor
    //  input to the fixup tex. Disabled because the assumption that affected
    //  games (Hitman/EGO/TR2013) put each eye's content in the top half of
    //  its slot didn't hold up — the actual layouts differ per game and the
    //  fixup ended up cropping real content instead of recovering it.
    //  Per-output special cases (Hitman/EGO branches in the SBS compositor)
    //  are restored below as the working approach.]
    //
    // bool needShadowFixup = (g_bHitmanAbsolution || g_bForceEgoEngine || g_bTombRaider2013);
    // bool fixupReady = needShadowFixup && EnsureShadowFixup(W, origH * 2, bbDesc.Format);
    // if (fixupReady) {
    //     // [draws and pointer swap — see git history if reviving]
    // }
    // ============================================================
    bool crosseyed       = g_bSwapEyes;
    bool tabMode         = (g_OutputMode == OM_OU_HALF || g_OutputMode == OM_OU_FULL);
    bool fullResSBS      = (g_OutputMode == OM_SBS_FULL);
    bool fullResTab      = (g_OutputMode == OM_OU_FULL);
    bool interleavedMode = (g_OutputMode == OM_LINE_INTERLEAVED ||
                            g_OutputMode == OM_COL_INTERLEAVED  ||
                            g_OutputMode == OM_CHECKERBOARD);
    bool anaglyphMode    = (g_OutputMode == OM_ANAGLYPH);
    bool srWeaveMode     = (g_OutputMode == OM_SR_WEAVE);

    // Compositor: the shadow texture holds the game's T&B content (left eye=top,
    // right eye=bottom).  The pixel shader samples each eye's half and draws it
    // into the real (display-sized) BB.
    //
    // Half-resolution modes: each eye sees half the screen dimensions
    //   SBS: each eye = W/2 wide, H tall (letterboxed if needed)
    //   TAB: each eye = W wide, H/2 tall
    //
    // Full-resolution modes: each eye sees full screen dimensions
    //   SBS Full: each eye = W/2 wide, H tall (stretched to full width for display)
    //   TAB Full: each eye = W wide, H/2 tall (stretched to full height for display)
    //
    // Interleaved modes: game runs at full res; compositor picks per-pixel from each
    //   eye half of the shadow based on row/column/checkerboard parity.

    // One-time log
    {
        static bool s_bVPLogOnce = false;
        if (!s_bVPLogOnce)
        {
            s_bVPLogOnce = true;
            char buf[200];
            wsprintfA(buf, "[AmdQbProxy] Viewport: mode=%d realBB=%ux%u origH=%u shadow=%ux%u\n",
                      g_OutputMode, W, H, origH, W, shadowH);
            WriteLog(buf);
        }
    }

    if (anaglyphMode)
    {
        // Anaglyph: single full-screen draw using colour-matrix shader.
        // SwapEyes is handled inside UpdateAnaglyphCB by swapping l/r matrix rows.
        if (g_pAnaglyphPS && g_pAnaglyphCB)
        {
            UpdateAnaglyphCB();
            D3D11_VIEWPORT vp = {};
            vp.Width    = static_cast<float>(W);
            vp.Height   = static_cast<float>(H);
            vp.MaxDepth = 1.0f;
            g_pContext->RSSetViewports(1, &vp);
            // Use TriOviz-specific shader (with blend_RGB ghosting suppression) when AC_TRIOVIZ is selected
            ID3D11PixelShader* anaPS = (g_AnaglyphColour == AC_TRIOVIZ && g_pAnaglyphTriovizPS)
                                       ? g_pAnaglyphTriovizPS : g_pAnaglyphPS;
            g_pContext->PSSetShader(anaPS, nullptr, 0);
            g_pContext->PSSetConstantBuffers(2, 1, &g_pAnaglyphCB);
            g_pContext->Draw(3, 0);
            g_pContext->PSSetShader(g_pPS, nullptr, 0);
        }
    }
    else if (interleavedMode)
    {
        // Single full-screen draw; dedicated PS picks left/right eye per pixel.
        // Point sampler used to guarantee no cross-eye blending at row/column edges.
        // CB slot 0 (uvOff) repurposed as swap flag: 0.0 = normal, 1.0 = eyes swapped.
        ID3D11PixelShader* pIntPS =
            (g_OutputMode == OM_LINE_INTERLEAVED) ? g_pLineInterleavedPS :
            (g_OutputMode == OM_COL_INTERLEAVED)  ? g_pColInterleavedPS  :
                                                     g_pCheckerboardPS;
        if (pIntPS && g_pPointSampler)
        {
            D3D11_VIEWPORT vp = {};
            vp.Width    = static_cast<float>(W);
            vp.Height   = static_cast<float>(H);
            vp.MaxDepth = 1.0f;
            g_pContext->RSSetViewports(1, &vp);
            g_pContext->PSSetShader(pIntPS, nullptr, 0);
            g_pContext->PSSetSamplers(0, 1, &g_pPointSampler);
            UpdateCB(crosseyed ? 1.0f : 0.0f);  // swap flag in uvOff slot
            g_pContext->PSSetConstantBuffers(0, 1, &g_pCB);
            g_pContext->Draw(3, 0);
            // Restore default PS/sampler (full state restore happens at Present end)
            g_pContext->PSSetShader(g_pPS, nullptr, 0);
            g_pContext->PSSetSamplers(0, 1, &g_pSampler);
        }
    }
    else if (tabMode)
    {
        if (fullResTab)
        {
            // TAB Full-Resolution: each eye stretched to full height
            // Left eye (full display height, right eye rendered over)
            {
                D3D11_VIEWPORT vp = {};
                vp.TopLeftX = 0.0f;
                vp.TopLeftY = 0.0f;
                vp.Width    = static_cast<float>(W);
                vp.Height   = static_cast<float>(H);
                vp.MaxDepth = 1.0f;
                g_pContext->RSSetViewports(1, &vp);
                UpdateCB(crosseyed ? g_fUvScl : 0.0f);  // sample based on swap setting
                g_pContext->PSSetConstantBuffers(0, 1, &g_pCB);
                g_pContext->Draw(3, 0);
            }
        }
        else
        {
            // TAB Half-Resolution: standard top/bottom split
            // Left eye (top half of display)
            {
                D3D11_VIEWPORT vp = {};
                vp.TopLeftX = 0.0f;
                vp.TopLeftY = 0.0f;
                vp.Width    = static_cast<float>(W);
                vp.Height   = static_cast<float>(H / 2);
                vp.MaxDepth = 1.0f;
                g_pContext->RSSetViewports(1, &vp);
                UpdateCB(0.0f);  // sample top half of shadow
                g_pContext->PSSetConstantBuffers(0, 1, &g_pCB);
                g_pContext->Draw(3, 0);
            }
            // Right eye (bottom half of display)
            {
                D3D11_VIEWPORT vp = {};
                vp.TopLeftX = 0.0f;
                vp.TopLeftY = static_cast<float>(H / 2);
                vp.Width    = static_cast<float>(W);
                vp.Height   = static_cast<float>(H / 2);
                vp.MaxDepth = 1.0f;
                g_pContext->RSSetViewports(1, &vp);
                UpdateCB(g_fUvScl);  // sample bottom half (or second quarter for half-height)
                g_pContext->PSSetConstantBuffers(0, 1, &g_pCB);
                g_pContext->Draw(3, 0);
            }
        }
    }
    else if (srWeaveMode)
    {
        // SR Weave: blit each eye from the T&B shadow into a side-by-side texture,
        // then hand it to the Leia DX11 weaver which writes to the currently-bound
        // RTV (pRTV, already targeting the real BB).
        DXGI_SWAP_CHAIN_DESC scDescSR = {};
        pSC->GetDesc(&scDescSR);
        HWND hWndSR = scDescSR.OutputWindow;

        bool srOK  = EnsureSRWeaver(hWndSR);
        bool sbsOK = srOK && EnsureSRSBSTexture(W, H, bbDesc.Format);

        // One-time setup log
        static bool s_bSRSetupLogged = false;
        if (!s_bSRSetupLogged)
        {
            s_bSRSetupLogged = true;
            char buf[300];
            wsprintfA(buf, "[AmdQbProxy] SR weave setup: weaverOK=%d sbsOK=%d hWnd=%p BB=%ux%u origH=%u shadow=%p SBSTex=%p SBSSRV=%p fmt=%u\n",
                      (int)srOK, (int)sbsOK, hWndSR, W, H, origH, pShadowTex, g_pSRSBSTex, g_pSRSBSSRV, bbDesc.Format);
            WriteLog(buf);
        }

        if (sbsOK)
        {
            // Unbind the shadow SRV from PS slot 0 (compositor flow bound it earlier).
            // We need NumViews=1 with a nullptr SRV — NumViews=0 is a no-op.
            ID3D11ShaderResourceView* pNullSRV = nullptr;
            g_pContext->PSSetShaderResources(0, 1, &pNullSRV);

            // Copy left eye (top half of shadow) into left half of SBS texture,
            // right eye (bottom half of shadow) into right half. Swap if crosseyed.
            D3D11_BOX srcLeft  = { 0, 0,      0, W,     origH, 1 };
            D3D11_BOX srcRight = { 0, origH,  0, W, origH * 2, 1 };
            g_pContext->CopySubresourceRegion(g_pSRSBSTex, 0, 0, 0, 0, pShadowTex, 0, crosseyed ? &srcRight : &srcLeft);
            g_pContext->CopySubresourceRegion(g_pSRSBSTex, 0, W, 0, 0, pShadowTex, 0, crosseyed ? &srcLeft  : &srcRight);

            // Full-BB viewport so weave() writes the whole frame.
            D3D11_VIEWPORT vp = {};
            vp.Width    = static_cast<float>(W);
            vp.Height   = static_cast<float>(H);
            vp.MaxDepth = 1.0f;
            g_pContext->RSSetViewports(1, &vp);

            // Hand off to SR weaver. SetInputTexture was called once in
            // EnsureSRSBSTexture when the SRV was created.
            g_pSRInterface->Weave();

            // One-time success log on first weave
            static bool s_bWeaveOnceLog = false;
            if (!s_bWeaveOnceLog)
            {
                s_bWeaveOnceLog = true;
                WriteLog("[AmdQbProxy] SR weave: first Weave() called\n");
            }
        }
    }
    else
    {
        // SBS modes: left viewport = one eye, right viewport = other eye
        bool halfH = (g_nStereoMode == 2);
        bool ego   = g_bForceEgoEngine;

        if (g_bHitmanAbsolution)
        {
            // Hitman Absolution: each eye's content lives in the TOP HALF of its shadow
            // slot (left eye rows [0, origH/2), right eye rows [origH, origH+origH/2)).
            // Sample with uvScl=0.25 and uvOff 0.0 / 0.5 to stretch that half-height
            // content to fill the full SBS viewport.
            static bool s_bHitmanPathLogged = false;
            if (!s_bHitmanPathLogged)
            {
                s_bHitmanPathLogged = true;
                char buf[200];
                wsprintfA(buf, "[AmdQbProxy] Hitman compositor path: uvScl=0.25, origH=%u, shadow=%ux%u\n",
                          origH, W, origH * 2);
                WriteLog(buf);
            }
            float savedUvScl = g_fUvScl;
            g_fUvScl = 0.25f;
            // Left eye
            {
                D3D11_VIEWPORT vp = {};
                vp.TopLeftX = 0.0f;
                vp.TopLeftY = 0.0f;
                vp.Width    = static_cast<float>(W / 2);
                vp.Height   = static_cast<float>(H);
                vp.MaxDepth = 1.0f;
                g_pContext->RSSetViewports(1, &vp);
                UpdateCB(crosseyed ? 0.5f : 0.0f);
                g_pContext->PSSetConstantBuffers(0, 1, &g_pCB);
                g_pContext->Draw(3, 0);
            }
            // Right eye
            {
                D3D11_VIEWPORT vp = {};
                vp.TopLeftX = static_cast<float>(W / 2);
                vp.TopLeftY = 0.0f;
                vp.Width    = static_cast<float>(W / 2);
                vp.Height   = static_cast<float>(H);
                vp.MaxDepth = 1.0f;
                g_pContext->RSSetViewports(1, &vp);
                UpdateCB(crosseyed ? 0.0f : 0.5f);
                g_pContext->PSSetConstantBuffers(0, 1, &g_pCB);
                g_pContext->Draw(3, 0);
            }
            g_fUvScl = savedUvScl;
        }
        else if (fullResSBS)
        {
            // SBS Full-Resolution: real BB is 2x game width (e.g. 3840 for 1920 game).
            // Shadow is game-width x 2H (T&B). Each half-viewport is exactly game-width
            // wide, and NDC [-1,1] maps to [0,1] UV_x in each half — so the full shadow
            // width is sampled per eye with no squishing. UV_y selects top/bottom half.
            // Left eye (left half of display)
            {
                D3D11_VIEWPORT vp = {};
                vp.TopLeftX = 0.0f;
                vp.TopLeftY = 0.0f;
                vp.Width    = static_cast<float>(W / 2);
                vp.Height   = static_cast<float>(H);
                vp.MaxDepth = 1.0f;
                g_pContext->RSSetViewports(1, &vp);
                UpdateCB(crosseyed ? g_fUvScl : 0.0f);
                g_pContext->PSSetConstantBuffers(0, 1, &g_pCB);
                g_pContext->Draw(3, 0);
            }
            // Right eye (right half of display)
            {
                D3D11_VIEWPORT vp = {};
                vp.TopLeftX = static_cast<float>(W / 2);
                vp.TopLeftY = 0.0f;
                vp.Width    = static_cast<float>(W / 2);
                vp.Height   = static_cast<float>(H);
                vp.MaxDepth = 1.0f;
                g_pContext->RSSetViewports(1, &vp);
                UpdateCB(crosseyed ? 0.0f : g_fUvScl);
                g_pContext->PSSetConstantBuffers(0, 1, &g_pCB);
                g_pContext->Draw(3, 0);
            }
        }
        else if (ego)
        {
            // EGO-engine workaround: put left eye in top half, right eye in bottom
            // half of the BB to avoid vertical stretching of the top-half content.
            // Left eye (top half)
            {
                D3D11_VIEWPORT vp = {};
                vp.TopLeftX = 0.0f;
                vp.TopLeftY = 0.0f;
                vp.Width    = static_cast<float>(W / 2);
                vp.Height   = static_cast<float>(H / 2);
                vp.MaxDepth = 1.0f;
                g_pContext->RSSetViewports(1, &vp);
                UpdateCB(crosseyed ? g_fUvScl : 0.0f);
                g_pContext->PSSetConstantBuffers(0, 1, &g_pCB);
                g_pContext->Draw(3, 0);
            }
            // Right eye (bottom half) — EGO fix: show bottom half on the right side
            {
                D3D11_VIEWPORT vp = {};
                vp.TopLeftX = static_cast<float>(W / 2);
                vp.TopLeftY = static_cast<float>(H / 2);
                vp.Width    = static_cast<float>(W / 2);
                vp.Height   = static_cast<float>(H / 2);
                vp.MaxDepth = 1.0f;
                g_pContext->RSSetViewports(1, &vp);
                UpdateCB(crosseyed ? 0.0f : g_fUvScl);
                g_pContext->PSSetConstantBuffers(0, 1, &g_pCB);
                g_pContext->Draw(3, 0);
            }
        }
        else
        {
            // Default SBS half-resolution: respect half-height letterbox if detected
            float vpH  = halfH ? static_cast<float>(H / 2) : static_cast<float>(H);
            float vpY  = halfH ? static_cast<float>(H / 4) : 0.0f;
            // Left eye
            {
                D3D11_VIEWPORT vp = {};
                vp.TopLeftX = 0.0f;
                vp.TopLeftY = vpY;
                vp.Width    = static_cast<float>(W / 2);
                vp.Height   = vpH;
                vp.MaxDepth = 1.0f;
                g_pContext->RSSetViewports(1, &vp);
                UpdateCB(crosseyed ? g_fUvScl : 0.0f);
                g_pContext->PSSetConstantBuffers(0, 1, &g_pCB);
                g_pContext->Draw(3, 0);
            }
            // Right eye
            {
                D3D11_VIEWPORT vp = {};
                vp.TopLeftX = static_cast<float>(W / 2);
                vp.TopLeftY = vpY;
                vp.Width    = static_cast<float>(W / 2);
                vp.Height   = vpH;
                vp.MaxDepth = 1.0f;
                g_pContext->RSSetViewports(1, &vp);
                UpdateCB(crosseyed ? 0.0f : g_fUvScl);
                g_pContext->PSSetConstantBuffers(0, 1, &g_pCB);
                g_pContext->Draw(3, 0);
            }
        }
    }

    // --- Post-compositor diagnostic: verify Draw calls actually wrote to BB ---
    {
        static bool s_bPostCompDone = false;
        if (!s_bPostCompDone && g_nPresentCount >= 60)
        {
            s_bPostCompDone = true;

            // Check device health first
            HRESULT hrRemoved = g_pDevice11->GetDeviceRemovedReason();
            char hbuf[200];
            wsprintfA(hbuf, "[AmdQbProxy] PostComp: DeviceRemovedReason=0x%08X\n", (unsigned)hrRemoved);
            WriteLog(hbuf);

            // Flush GPU to ensure Draw calls are complete
            g_pContext->Flush();

            // Read back from REAL BB (not shadow) to see what the compositor produced
            ID3D11Texture2D* pPostBB = nullptr;
            if (g_pfnOrigGetBuffer &&
                SUCCEEDED(g_pfnOrigGetBuffer(pSC, 0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&pPostBB))))
            {
                D3D11_TEXTURE2D_DESC postDesc;
                pPostBB->GetDesc(&postDesc);

                ID3D11Texture2D* pRead = nullptr;
                D3D11_TEXTURE2D_DESC rd = {};
                rd.Width = 1; rd.Height = 1; rd.MipLevels = 1; rd.ArraySize = 1;
                rd.Format = postDesc.Format; rd.SampleDesc.Count = 1;
                rd.Usage  = D3D11_USAGE_STAGING;
                rd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                g_pDevice11->CreateTexture2D(&rd, nullptr, &pRead);
                if (pRead)
                {
                    // Sample 4 points: left-center, right-center, top-left corner, center
                    UINT px[4] = { W/4,   W*3/4, 10,  W/2 };
                    UINT py[4] = { H/2,   H/2,   10,  H/2 };
                    const char* lbl[4] = { "Lctr", "Rctr", "TLcrnr", "center" };
                    char line[600];
                    wsprintfA(line, "[AmdQbProxy] PostComp BB readback (W=%u H=%u fmt=%u):", postDesc.Width, postDesc.Height, postDesc.Format);
                    WriteLog(line);
                    for (int i = 0; i < 4; ++i)
                    {
                        D3D11_BOX box = { px[i], py[i], 0, px[i]+1, py[i]+1, 1 };
                        g_pContext->CopySubresourceRegion(pRead, 0, 0, 0, 0, pPostBB, 0, &box);
                        D3D11_MAPPED_SUBRESOURCE mapped = {};
                        DWORD val = 0;
                        if (SUCCEEDED(g_pContext->Map(pRead, 0, D3D11_MAP_READ, 0, &mapped)))
                        {
                            val = *static_cast<DWORD*>(mapped.pData);
                            g_pContext->Unmap(pRead, 0);
                        }
                        wsprintfA(line, " %s(%u,%u)=0x%08X", lbl[i], px[i], py[i], val);
                        WriteLog(line);
                    }
                    WriteLog("\n");
                    pRead->Release();
                }
                pPostBB->Release();
            }
        }
    }

    // Cursor compositing block removed: the OS hardware cursor is drawn by
    // DWM/the display driver AFTER our Present, so it appears as a single
    // sprite on top of the composited stereo output naturally — exactly
    // the behaviour NvDirectMode relies on.

    // ---- Restore render state ----
    g_pContext->OMSetRenderTargets(1, &pOldRTV, pOldDSV);
    g_pContext->RSSetViewports(numVPs, &oldVP);
    g_pContext->VSSetShader(pOldVS, nullptr, 0);
    g_pContext->PSSetShader(pOldPS, nullptr, 0);
    g_pContext->IASetInputLayout(pOldIL);
    g_pContext->IASetPrimitiveTopology(oldTopo);
    g_pContext->PSSetConstantBuffers(0, 1, &pOldCB);
    g_pContext->PSSetShaderResources(0, 1, &pOldSRV);
    g_pContext->PSSetSamplers(0, 1, &pOldSmp);
    g_pContext->OMSetDepthStencilState(pOldDS, oldSRef);
    g_pContext->RSSetState(pOldRS);
    g_pContext->OMSetBlendState(pOldBS, oldBF, oldSM);

    if (pOldRTV)  pOldRTV->Release();
    if (pOldDSV)  pOldDSV->Release();
    if (pOldVS)   pOldVS->Release();
    if (pOldPS)   pOldPS->Release();
    if (pOldIL)   pOldIL->Release();
    if (pOldCB)   pOldCB->Release();
    if (pOldSRV)  pOldSRV->Release();
    if (pOldSmp)  pOldSmp->Release();
    if (pOldDS)   pOldDS->Release();
    if (pOldRS)   pOldRS->Release();
    if (pOldBS)   pOldBS->Release();
    pRTV->Release();

    // GPU-based cursor compositing implemented above; remove GDI fallback to avoid double-draw.

    return g_pfnOrigPresent(pSC, SyncInterval, Flags);
}

// Present1 hook (IDXGISwapChain1::Present1, vtable slot 22) — same as Present but DXGI 1.1 API.
// Some games (Hitman: Absolution) use Present for loading then switch to Present1 for rendering.
static HRESULT STDMETHODCALLTYPE HookedPresent1(IDXGISwapChain1* pSC1, UINT SyncInterval, UINT PresentFlags,
                                                 const DXGI_PRESENT_PARAMETERS* pPresentParameters)
{
    // Run the full compositor through HookedPresent (which calls g_pfnOrigPresent at the end).
    // Present and Present1 are functionally equivalent for our purposes — the extra
    // DXGI_PRESENT_PARAMETERS only affect dirty-rect optimisations.
    return HookedPresent(pSC1, SyncInterval, PresentFlags);
}

static void InstallPresentHook()
{
    if (g_bHooked || !g_pDevice11) return;

    // Create a throwaway window to host the dummy swap chain
    HWND hWnd = CreateWindowW(L"STATIC", nullptr, WS_POPUP, 0, 0, 8, 8,
                              nullptr, nullptr, nullptr, nullptr);
    if (!hWnd) return;

    IDXGIDevice* pDXGIDevice = nullptr;
    if (FAILED(g_pDevice11->QueryInterface(__uuidof(IDXGIDevice),
                                           reinterpret_cast<void**>(&pDXGIDevice))))
    { DestroyWindow(hWnd); return; }

    IDXGIAdapter* pAdapter = nullptr;
    pDXGIDevice->GetAdapter(&pAdapter);
    pDXGIDevice->Release();
    if (!pAdapter) { DestroyWindow(hWnd); return; }

    IDXGIFactory* pFactory = nullptr;
    pAdapter->GetParent(__uuidof(IDXGIFactory), reinterpret_cast<void**>(&pFactory));
    pAdapter->Release();
    if (!pFactory) { DestroyWindow(hWnd); return; }

    DXGI_SWAP_CHAIN_DESC scd    = {};
    scd.BufferCount             = 1;
    scd.BufferDesc.Width        = 8;
    scd.BufferDesc.Height       = 8;
    scd.BufferDesc.Format       = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferUsage             = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow            = hWnd;
    scd.SampleDesc.Count        = 1;
    scd.Windowed                = TRUE;

    IDXGISwapChain* pDummy = nullptr;
    if (SUCCEEDED(pFactory->CreateSwapChain(g_pDevice11, &scd, &pDummy)))
    {
        // IDXGISwapChain::Present is vtable slot 8 (0-indexed)
        void** vtable    = *reinterpret_cast<void***>(pDummy);
        void*  presentFn = vtable[8];

        if (MH_CreateHook(presentFn,
                          reinterpret_cast<LPVOID>(&HookedPresent),
                          reinterpret_cast<LPVOID*>(&g_pfnOrigPresent)) == MH_OK)
            MH_EnableHook(presentFn);

        // IDXGISwapChain1::Present1 is vtable slot 22 (DXGI 1.1)
        // Some games switch from Present to Present1 after initial setup.
        {
            IDXGISwapChain1* pDummy1 = nullptr;
            if (SUCCEEDED(pDummy->QueryInterface(__uuidof(IDXGISwapChain1),
                                                  reinterpret_cast<void**>(&pDummy1))))
            {
                void** vtable1     = *reinterpret_cast<void***>(pDummy1);
                void*  present1Fn  = vtable1[22];
                if (present1Fn != presentFn)
                {
                    if (MH_CreateHook(present1Fn,
                                      reinterpret_cast<LPVOID>(&HookedPresent1),
                                      reinterpret_cast<LPVOID*>(&g_pfnOrigPresent1)) == MH_OK)
                    {
                        MH_EnableHook(present1Fn);
                        WriteLog("[AmdQbProxy] IDXGISwapChain1::Present1 hook installed\n");
                    }
                }
                // IDXGISwapChain1::GetDesc1 is vtable slot 18
                void* getDesc1Fn = vtable1[18];
                if (MH_CreateHook(getDesc1Fn,
                                  reinterpret_cast<LPVOID>(&HookedGetDesc1),
                                  reinterpret_cast<LPVOID*>(&g_pfnOrigGetDesc1)) == MH_OK)
                {
                    MH_EnableHook(getDesc1Fn);
                    WriteLog("[AmdQbProxy] IDXGISwapChain1::GetDesc1 hook installed\n");
                }

                pDummy1->Release();
            }
        }

        // IDXGISwapChain::ResizeBuffers is vtable slot 13
        void* resizeFn = vtable[13];
        if (MH_CreateHook(resizeFn,
                          reinterpret_cast<LPVOID>(&HookedResizeBuffers),
                          reinterpret_cast<LPVOID*>(&g_pfnOrigResizeBuffers)) == MH_OK)
        {
            MH_EnableHook(resizeFn);
            WriteLog("[AmdQbProxy] IDXGISwapChain::ResizeBuffers hook installed\n");
        }

        // IDXGISwapChain::GetBuffer is vtable slot 9
        void* getBufferFn = vtable[9];
        if (MH_CreateHook(getBufferFn,
                          reinterpret_cast<LPVOID>(&HookedGetBuffer),
                          reinterpret_cast<LPVOID*>(&g_pfnOrigGetBuffer)) == MH_OK)
        {
            MH_EnableHook(getBufferFn);
            WriteLog("[AmdQbProxy] IDXGISwapChain::GetBuffer hook installed\n");
        }

        // IDXGISwapChain::GetDesc is vtable slot 12
        void* getDescFn = vtable[12];
        if (MH_CreateHook(getDescFn,
                          reinterpret_cast<LPVOID>(&HookedGetDesc),
                          reinterpret_cast<LPVOID*>(&g_pfnOrigGetDesc)) == MH_OK)
        {
            MH_EnableHook(getDescFn);
            WriteLog("[AmdQbProxy] IDXGISwapChain::GetDesc hook installed\n");
        }

        // IDXGISwapChain::ResizeTarget is vtable slot 14
        void* resizeTargetFn = vtable[14];
        if (MH_CreateHook(resizeTargetFn,
                          reinterpret_cast<LPVOID>(&HookedResizeTarget),
                          reinterpret_cast<LPVOID*>(&g_pfnOrigResizeTarget)) == MH_OK)
        {
            MH_EnableHook(resizeTargetFn);
            WriteLog("[AmdQbProxy] IDXGISwapChain::ResizeTarget hook installed\n");
        }

        g_bHooked = true;
        WriteLog("[AmdQbProxy] IDXGISwapChain::Present hook installed\n");
        pDummy->Release();

        // Hook IDXGIFactory::CreateSwapChain (vtable[10]) to create shadow for stereo
        {
            void** factoryVtbl = *reinterpret_cast<void***>(pFactory);
            void*  createSCFn  = factoryVtbl[10];
            if (MH_CreateHook(createSCFn,
                              reinterpret_cast<LPVOID>(&HookedCreateSwapChain),
                              reinterpret_cast<LPVOID*>(&g_pfnOrigCreateSwapChain)) == MH_OK)
            {
                MH_EnableHook(createSCFn);
                WriteLog("[AmdQbProxy] IDXGIFactory::CreateSwapChain hook installed\n");
            }
        }
    }

    pFactory->Release();
    DestroyWindow(hWnd);
}

// ============================================================
// Fake AMD COM interfaces
// ============================================================

// IAmdDxExtQuadBufferStereo implementation
class FakeAmdDxExtQbStereo : public IAmdDxExtQuadBufferStereo
{
    ULONG m_ref;
public:
    FakeAmdDxExtQbStereo() : m_ref(1) {}

    unsigned int AddRef()  override { return ++m_ref; }
    unsigned int Release() override
    {
        ULONG r = --m_ref;
        if (r == 0)
        {
            WriteLog("[AmdQbProxy] FakeAmdDxExtQbStereo destroyed\n");
            delete this;
        }
        return r;
    }

    HRESULT EnableQuadBufferStereo(BOOL enable) override
    {
        g_bStereoActive = (enable != FALSE);
        if (g_bStereoActive)
        {
            WriteLog("[AmdQbProxy] EnableQuadBufferStereo(TRUE) - installing Present hook\n");
            InstallPresentHook();
        }
        else
        {
            WriteLog("[AmdQbProxy] EnableQuadBufferStereo(FALSE) - stereo disabled\n");
        }
        return S_OK;
    }

    // Game uses this to determine where the right eye starts in the back buffer.
    // The shadow texture is 2*origH tall; we report origH as the split point.
    UINT GetLineOffset(IDXGISwapChain* pSC) override
    {
        UINT offset = 0;
        if (pSC)
        {
            auto it = g_scShadow.find(pSC);
            if (it != g_scShadow.end())
                offset = it->second.origH;
        }
        if (!offset && pSC)
        {
            DXGI_SWAP_CHAIN_DESC desc = {};
            pSC->GetDesc(&desc);
            offset = desc.BufferDesc.Height;
        }
        static int s_nLogCount = 0;
        if (++s_nLogCount <= 10 || (s_nLogCount % 5000) == 0)
        {
            char buf[128];
            wsprintfA(buf, "[AmdQbProxy] GetLineOffset(SC=%p) -> %u (call #%d)\n", pSC, offset, s_nLogCount);
            WriteLog(buf);
        }
        return offset;
    }

    // Return real DXGI output modes so the game considers HD3D available.
    // Uses CreateDXGIFactory instead of g_pDevice11 to avoid dangling-pointer
    // crashes when the game destroys/recreates the D3D11 device mid-init.
    HRESULT GetDisplayModeList(DXGI_FORMAT format, UINT flags,
                               UINT* pNum, DXGI_MODE_DESC* pModes) override
    {
        if (!pNum) return E_INVALIDARG;

        IDXGIFactory* pFactory = nullptr;
        if (FAILED(CreateDXGIFactory(__uuidof(IDXGIFactory),
                                     reinterpret_cast<void**>(&pFactory))))
        { *pNum = 0; return S_OK; }

        IDXGIAdapter* pAdapter = nullptr;
        HRESULT hr = pFactory->EnumAdapters(0, &pAdapter);
        pFactory->Release();
        if (FAILED(hr)) { *pNum = 0; return S_OK; }

        IDXGIOutput* pOutput = nullptr;
        hr = pAdapter->EnumOutputs(0, &pOutput);
        pAdapter->Release();
        if (FAILED(hr)) { *pNum = 0; return S_OK; }

        DXGI_FORMAT fmt = (format != DXGI_FORMAT_UNKNOWN) ? format
                                                          : DXGI_FORMAT_R8G8B8A8_UNORM;
        hr = pOutput->GetDisplayModeList(fmt, flags, pNum, pModes);
        pOutput->Release();

        char buf[256];
        wsprintfA(buf, "[AmdQbProxy] GetDisplayModeList(fmt=%u flags=0x%X): %u modes\n",
                  (UINT)fmt, flags, *pNum);
        WriteLog(buf);

        // Diagnostic dump: when the game requests the actual mode list (pModes
        // non-null), log each entry once per unique (format, flags, count) tuple.
        // Helps diagnose game-side stereo validation that rejects our forwarded
        // DXGI mode list (e.g. Thief looking for a 120Hz stereo refresh rate
        // that real AMD HD3D would advertise but we don't synthesize).
        if (pModes && *pNum > 0)
        {
            static UINT s_lastFmt   = 0xFFFFFFFFu;
            static UINT s_lastFlags = 0xFFFFFFFFu;
            static UINT s_lastNum   = 0xFFFFFFFFu;
            if ((UINT)fmt != s_lastFmt || flags != s_lastFlags || *pNum != s_lastNum)
            {
                s_lastFmt = (UINT)fmt; s_lastFlags = flags; s_lastNum = *pNum;
                for (UINT i = 0; i < *pNum; ++i)
                {
                    const DXGI_MODE_DESC& m = pModes[i];
                    UINT hz_x100 = m.RefreshRate.Denominator
                        ? (m.RefreshRate.Numerator * 100u) / m.RefreshRate.Denominator
                        : 0u;
                    wsprintfA(buf, "[AmdQbProxy]   mode[%u]: %ux%u @ %u.%02uHz fmt=%u scan=%u scale=%u\n",
                              i, m.Width, m.Height, hz_x100 / 100u, hz_x100 % 100u,
                              (UINT)m.Format, (UINT)m.ScanlineOrdering, (UINT)m.Scaling);
                    WriteLog(buf);
                }
            }
        }

        return hr;
    }
};

// IAmdDxExt implementation
class FakeAmdDxExt : public IAmdDxExt
{
    ULONG m_ref;
public:
    FakeAmdDxExt() : m_ref(1) {}

    unsigned int AddRef()  override { return ++m_ref; }
    unsigned int Release() override
    {
        ULONG r = --m_ref;
        if (r == 0) delete this;
        return r;
    }

    HRESULT GetVersion(AmdDxExtVersion* pVer) override
    {
        if (pVer) { pVer->majorVersion = 1; pVer->minorVersion = 1; }
        return S_OK;
    }

    IAmdDxExtInterface* GetExtInterface(unsigned int iface) override
    {
        if (iface == AmdDxExtQuadBufferStereoID)
        {
            g_bQbsCreated = true;
            InstallPresentHook();  // Install hooks early for games that skip EnableQBS
            WriteLog("[AmdQbProxy] GetExtInterface(QuadBufferStereo) -> OK\n");
            return new (std::nothrow) FakeAmdDxExtQbStereo();
        }
        char buf[80];
        wsprintfA(buf, "[AmdQbProxy] GetExtInterface(%u) -> NULL\n", iface);
        WriteLog(buf);
        return nullptr;
    }

    // Remaining IAmdDxExt methods — not needed for stereo
    HRESULT IaSetPrimitiveTopology(unsigned int) override                { return E_NOTIMPL; }
    HRESULT IaGetPrimitiveTopology(AmdDxExtPrimitiveTopology*) override  { return E_NOTIMPL; }
    HRESULT SetSingleSampleRead(ID3D10Resource*, BOOL) override          { return E_NOTIMPL; }
    HRESULT SetSingleSampleRead11(ID3D11Resource*, BOOL) override        { return E_NOTIMPL; }
};

// ============================================================
// Exports
// Using extern "C" is safe here because our local AmdQbInterfaces.h does NOT
// forward-declare AmdDxExtCreate / AmdDxExtCreate11, so there is no C2732
// linkage-specification conflict.
// ============================================================
extern "C"
{
    // D3D10 path: return COM stubs.  No compositor (D3D10 TODO).
    HRESULT __cdecl AmdDxExtCreate(ID3D10Device* /*pDevice*/, IAmdDxExt** ppExt)
    {
        if (!ppExt) return E_INVALIDARG;
        *ppExt = new (std::nothrow) FakeAmdDxExt();
        return *ppExt ? S_OK : E_OUTOFMEMORY;
    }

    // D3D11 path: store device for compositor + return COM stubs.
    HRESULT __cdecl AmdDxExtCreate11(ID3D11Device* pDevice, IAmdDxExt** ppExt)
    {
        if (!ppExt) return E_INVALIDARG;

        char buf[128];
        wsprintfA(buf, "[AmdQbProxy] AmdDxExtCreate11 called (pDevice=%p)\n", pDevice);
        WriteLog(buf);

        if (!pDevice)
        {
            WriteLog("[AmdQbProxy]   pDevice is NULL, skipping device setup\n");
        }
        else if (g_pDevice11 != pDevice)
        {
            WriteLog("[AmdQbProxy]   new device, releasing old\n");
            ReleaseCompositorResources();
            if (g_pReadbackTex) { g_pReadbackTex->Release(); g_pReadbackTex = nullptr; }
            g_nStereoMode = 0; g_fUvScl = 0.5f;
            if (g_pContext)  { g_pContext->Release();  g_pContext  = nullptr; }
            if (g_pDevice11) { g_pDevice11->Release(); g_pDevice11 = nullptr; }
            g_pDevice11 = pDevice;
            g_pDevice11->AddRef();
            g_pDevice11->GetImmediateContext(&g_pContext);
            WriteLog("[AmdQbProxy]   device stored OK\n");
        }

        *ppExt = new (std::nothrow) FakeAmdDxExt();
        WriteLog("[AmdQbProxy]   returning FakeAmdDxExt\n");
        return *ppExt ? S_OK : E_OUTOFMEMORY;
    }
}

// ============================================================
// DllMain
// ============================================================
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        g_hSelf = hModule;
        DisableThreadLibraryCalls(hModule);
        LoadConfig();
        MH_Initialize();   // still needed for the IDXGIFactory::CreateSwapChain hook
        WriteLog("[AmdQbProxy] DLL_PROCESS_ATTACH: atidxx loaded OK (wiz3D " DISPLAYED_VERSION ")\n");
        break;

    case DLL_PROCESS_DETACH:
        // lpReserved != NULL means the process is exiting — DLL unload order is
        // undefined and the SR runtime DLLs may already be gone. Calling
        // SRInterfaceDX11::Delete() on a dead vtable crashes (DiRT Rally exit
        // crash). Let the OS reclaim memory in that case.
        if (lpReserved != nullptr)
        {
            MH_Uninitialize();
            break;
        }
        MH_Uninitialize();
        CleanupSRWeaver();
        CleanupShadowFixup();
        for (auto& kv : g_scShadow) kv.second.Release();
        g_scShadow.clear();
        if (g_pReadbackTex){ g_pReadbackTex->Release();g_pReadbackTex = nullptr; }
        if (g_pStagingSRV) { g_pStagingSRV->Release(); g_pStagingSRV = nullptr; }
        if (g_pStagingTex) { g_pStagingTex->Release(); g_pStagingTex = nullptr; }
        if (g_pVS)         { g_pVS->Release();         g_pVS = nullptr; }
        if (g_pPS)         { g_pPS->Release();         g_pPS = nullptr; }
        if (g_pCB)         { g_pCB->Release();         g_pCB = nullptr; }
        if (g_pSampler)    { g_pSampler->Release();    g_pSampler = nullptr; }
        if (g_pDSState)    { g_pDSState->Release();    g_pDSState = nullptr; }
        if (g_pRSState)    { g_pRSState->Release();    g_pRSState = nullptr; }
        if (g_pContext)    { g_pContext->Release();    g_pContext = nullptr; }
        if (g_pDevice11)   { g_pDevice11->Release();   g_pDevice11 = nullptr; }
        break;
    }
    return TRUE;
}
