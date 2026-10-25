/*
* wiz3D OSD renderer -- see OSDRenderer.h for the design notes.
*/
#include "StdAfx.h"
#include "OSDRenderer.h"
#include "ShaderHunter.h"                 // 0.5.0 hunt-mode OSD slot

#include "..\S3DAPI\GlobalData.h"

#include <d3dcompiler.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <algorithm>
#include <vector>

// Win32 defines DrawText as DrawTextW/DrawTextA macros; our TexSlot method
// has the same base name. Undef locally so it doesn't collide (we still use
// ::DrawTextW explicitly inside TexSlot::DrawTextLine below).
#ifdef DrawText
#undef DrawText
#endif

#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "d3dcompiler.lib")

namespace wiz3d
{

// -----------------------------------------------------------------------
// Shader source. Textured quad with alpha blend. Uses SV_VertexID so the
// vertex buffer only carries (pos.xy, uv.xy). The pos is in normalised
// device coordinates (NDC) built per-draw on the CPU side.
// -----------------------------------------------------------------------
static const char* k_osdShaderSrc = R"(
struct VSIn  { float2 pos : POSITION; float2 uv : TEXCOORD0; };
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };

VSOut VSMain(VSIn i) {
    VSOut o;
    o.pos = float4(i.pos, 0.0, 1.0);
    o.uv  = i.uv;
    return o;
}

Texture2D    osdTex : register(t0);
SamplerState samp   : register(s0);

float4 PSMain(VSOut i) : SV_Target {
    return osdTex.Sample(samp, i.uv);
}
)";

// -----------------------------------------------------------------------
// GDIFont
// -----------------------------------------------------------------------
// Create a GDI font with the given point size and weight. The face name
// is taken from a caller-supplied cstring so OSDRenderer can switch between
// "Inter" (preferred, matches SR Loom / Stereopticon) and "Arial"
// (fallback when the Inter TTFs aren't deployed) without duplicating this
// method per face.
static HFONT MakeFont(const wchar_t* face, int ptSize, int weight, int& outHeightPx)
{
    outHeightPx = -MulDiv(ptSize, 96, 72);
    return CreateFontW(
        outHeightPx, 0, 0, 0,
        weight,
        FALSE /*italic*/, FALSE /*underline*/, FALSE /*strikeout*/,
        DEFAULT_CHARSET,
        OUT_TT_PRECIS,
        CLIP_DEFAULT_PRECIS,
        // ANTIALIASED_QUALITY produces grayscale AA (R==G==B per glyph
        // pixel). Our outline compositor reads one channel as the coverage
        // mask, which only works with grayscale AA -- ClearType would write
        // different per-subpixel values and break the mask.
        ANTIALIASED_QUALITY,
        VARIABLE_PITCH | FF_SWISS,
        face);
}

bool OSDRenderer::GDIFont::Create(int ptSize, bool bold)
{
    // Legacy single-weight API: produces Arial. New code should prefer the
    // OSDRenderer-level LoadInterFontPrivate -> MakeFont path so the face
    // tracks the renderer's choice (Inter vs Arial fallback).
    font = MakeFont(L"Arial", ptSize, bold ? FW_BOLD : FW_NORMAL, heightPx);
    return font != nullptr;
}

void OSDRenderer::GDIFont::Destroy()
{
    if (font) { DeleteObject(font); font = nullptr; }
    heightPx = 0;
}

// -----------------------------------------------------------------------
// TexSlot — the GDI DIB + D3D11 texture pair backing one overlay element.
// -----------------------------------------------------------------------
bool OSDRenderer::TexSlot::Create(ID3D11Device* dev, int width, int height)
{
    w = width; h = height;

    // 1) Create the GDI memory DC + DIB section. BITMAPINFO is top-down
    // (negative height) so the GDI pixel layout matches D3D11's
    // top-left origin without a flip. 32-bit BGRA with the per-pixel
    // alpha preserved for alpha-blending the overlay.
    HDC screen = GetDC(NULL);
    dc = CreateCompatibleDC(screen);
    ReleaseDC(NULL, screen);
    if (!dc) return false;

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = w;
    bmi.bmiHeader.biHeight      = -h;      // top-down
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* pBits = nullptr;
    dib = CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, &pBits, NULL, 0);
    if (!dib || !pBits) { Destroy(); return false; }
    bits = (BYTE*)pBits;

    prev = (HBITMAP)SelectObject(dc, dib);
    SetBkMode(dc, TRANSPARENT);     // DrawTextW shouldn't fill behind glyphs

    // 2) Create the matching D3D11 dynamic texture + SRV. BGRA matches
    // the DIB's byte layout: dib[0]=B, dib[1]=G, dib[2]=R, dib[3]=A.
    D3D11_TEXTURE2D_DESC td = {};
    td.Width            = w;
    td.Height           = h;
    td.MipLevels        = 1;
    td.ArraySize        = 1;
    td.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage            = D3D11_USAGE_DYNAMIC;
    td.BindFlags        = D3D11_BIND_SHADER_RESOURCE;
    td.CPUAccessFlags   = D3D11_CPU_ACCESS_WRITE;
    HRESULT hr = dev->CreateTexture2D(&td, nullptr, &tex);
    if (FAILED(hr) || !tex) { Destroy(); return false; }

    D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.Format              = td.Format;
    sd.ViewDimension       = D3D11_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels = 1;
    hr = dev->CreateShaderResourceView(tex, &sd, &srv);
    if (FAILED(hr) || !srv) { Destroy(); return false; }

    Clear();
    dirty = true;
    return true;
}

void OSDRenderer::TexSlot::Destroy()
{
    if (srv)  { srv->Release();  srv  = nullptr; }
    if (tex)  { tex->Release();  tex  = nullptr; }
    if (dc && prev) { SelectObject(dc, prev); prev = nullptr; }
    if (dib)  { DeleteObject(dib); dib = nullptr; }
    if (dc)   { DeleteDC(dc);     dc  = nullptr; }
    bits = nullptr; w = h = 0; dirty = false;
    visibleW = visibleH = 0;
}

void OSDRenderer::TexSlot::Clear()
{
    if (!bits) return;
    memset(bits, 0, (size_t)w * h * 4);   // fully transparent black
    dirty = true;
    visibleW = visibleH = 0;
}

// GDI AlphaBlend-style rect fill with explicit alpha (DIB is 32-bit).
// Writes BGRA with (B, G, R, alpha) ordering. Overwrites existing pixels
// in the rect (not accumulated) -- callers that want blending should Clear
// first.
void OSDRenderer::TexSlot::DrawRect(const RECT& r, COLORREF rgb, BYTE alpha)
{
    if (!bits) return;
    const BYTE R = GetRValue(rgb), G = GetGValue(rgb), B = GetBValue(rgb);
    const int x0 = (std::max)(0L, (long)r.left),   x1 = (std::min)((long)w, (long)r.right);
    const int y0 = (std::max)(0L, (long)r.top),    y1 = (std::min)((long)h, (long)r.bottom);
    for (int y = y0; y < y1; ++y) {
        BYTE* row = bits + (size_t)y * w * 4;
        for (int x = x0; x < x1; ++x) {
            BYTE* p = row + x * 4;
            p[0] = B; p[1] = G; p[2] = R; p[3] = alpha;
        }
    }
    dirty = true;
    if (x1 > visibleW) visibleW = x1;
    if (y1 > visibleH) visibleH = y1;
}

// Draw text with a 2-pixel black outline (matches the DX9 / legacy-DX10
// OSD's BMFont outlineThickness=2 look) and no backing panel.
//
// Technique: use a MONOCHROME glyph mask approach so the outline is
// correctly alpha-blended independently of GDI's "alpha-byte-is-just-
// padding" quirk on 32-bit DIBs.
//   1) Scratch step: temporarily draw the text to the DIB with GDI in
//      WHITE on BLACK (GDI anti-aliasing produces per-pixel grey values
//      that *are* the glyph coverage mask). Pixels GDI touches are
//      lum>0; pixels GDI leaves alone stay at lum=0.
//   2) Snapshot the mask into a small heap buffer (one byte per pixel).
//   3) Clear the rect back to all-zero.
//   4) For every pixel in the rect: if ANY surrounding mask pixel in
//      a 2px radius is non-zero, that pixel is "outlined" -- composite
//      black with alpha = max-of-neighbours. Then if its own mask is
//      non-zero, overwrite with fill colour at alpha = own-mask-value.
//      The result is a 2px black halo around the fill, with proper AA.
void OSDRenderer::TexSlot::DrawTextLine(const wchar_t* text, const RECT& r, HFONT font,
                                        COLORREF rgb, UINT flags)
{
    if (!dc || !bits || !font || !text) return;

    HFONT prevFont = (HFONT)SelectObject(dc, font);

    const int x0 = (std::max)(0L, (long)r.left),   x1 = (std::min)((long)w, (long)r.right);
    const int y0 = (std::max)(0L, (long)r.top),    y1 = (std::min)((long)h, (long)r.bottom);
    const int rw = x1 - x0, rh = y1 - y0;
    if (rw <= 0 || rh <= 0) { SelectObject(dc, prevFont); return; }

    // 1) Zero the rect so GDI starts from a known baseline.
    for (int y = y0; y < y1; ++y) {
        BYTE* row = bits + (size_t)y * w * 4;
        for (int x = x0; x < x1; ++x) {
            BYTE* p = row + x * 4;
            p[0] = p[1] = p[2] = p[3] = 0;
        }
    }

    // 2) Draw the text in white; GDI's anti-aliasing writes grey values
    //    into R==G==B that serve as our coverage mask.
    SetTextColor(dc, RGB(255, 255, 255));
    RECT rc = r;
    // Previously applied a 94% x-scale world transform here as a
    // publication-tightness trick. It was visibly garbling narrow glyphs
    // -- user 2026-10-18 reported "wiz3D" rendering as "wz3D" in the
    // hotkey-help overlay, which lined up with sub-pixel sampling across
    // "iz" at 15 px font + 0.94x. Also clipped the right side of longer
    // body strings because DrawTextW's rect clips in logical coords that
    // the transform shrinks. Slot widths (720 Status / 720 HotkeyHelp)
    // are already sized to hold the worst-case content at 1:1, so the
    // squeeze is pure liability now.
    ::DrawTextW(dc, text, -1, &rc, flags);

    // 3) Snapshot the mask (R channel is enough since R==G==B after
    //    white-on-black AA).
    std::vector<BYTE> mask((size_t)rw * rh, 0);
    for (int y = 0; y < rh; ++y) {
        const BYTE* row = bits + (size_t)(y + y0) * w * 4 + (size_t)x0 * 4;
        for (int x = 0; x < rw; ++x) mask[(size_t)y * rw + x] = row[x * 4 + 2]; // R
    }

    // 4) Composite outline + fill into the DIB.
    const BYTE Rt = GetRValue(rgb), Gt = GetGValue(rgb), Bt = GetBValue(rgb);
    const int kOutline = 2;
    for (int y = 0; y < rh; ++y) {
        BYTE* row = bits + (size_t)(y + y0) * w * 4 + (size_t)x0 * 4;
        for (int x = 0; x < rw; ++x) {
            // Outline: max mask value within a 2-pixel radius (excluding
            // the centre, which the fill owns).
            BYTE outAlpha = 0;
            for (int dy = -kOutline; dy <= kOutline && outAlpha < 255; ++dy) {
                const int ny = y + dy;
                if (ny < 0 || ny >= rh) continue;
                for (int dx = -kOutline; dx <= kOutline && outAlpha < 255; ++dx) {
                    const int nx = x + dx;
                    if (nx < 0 || nx >= rw) continue;
                    if (dx * dx + dy * dy > kOutline * kOutline) continue;
                    const BYTE m = mask[(size_t)ny * rw + nx];
                    if (m > outAlpha) outAlpha = m;
                }
            }
            BYTE fillAlpha = mask[(size_t)y * rw + x];

            BYTE* p = row + x * 4;
            if (fillAlpha > 0) {
                p[0] = Bt; p[1] = Gt; p[2] = Rt; p[3] = fillAlpha;
            } else if (outAlpha > 0) {
                p[0] = p[1] = p[2] = 0;
                p[3] = outAlpha;
            } else {
                p[0] = p[1] = p[2] = p[3] = 0;
            }
        }
    }

    SelectObject(dc, prevFont);
    dirty = true;
    if (x1 > visibleW) visibleW = x1;
    if (y1 > visibleH) visibleH = y1;
}

void OSDRenderer::TexSlot::Upload(ID3D11DeviceContext* ctx)
{
    if (!dirty || !tex || !bits) return;
    D3D11_MAPPED_SUBRESOURCE m = {};
    if (FAILED(ctx->Map(tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)) || !m.pData) return;
    const size_t rowBytes = (size_t)w * 4;
    for (int y = 0; y < h; ++y) {
        memcpy((BYTE*)m.pData + (size_t)y * m.RowPitch, bits + (size_t)y * rowBytes, rowBytes);
    }
    ctx->Unmap(tex, 0);
    dirty = false;
}

// -----------------------------------------------------------------------
// OSDRenderer
// -----------------------------------------------------------------------
OSDRenderer::OSDRenderer() {}
OSDRenderer::~OSDRenderer() { ReleaseAll(); }

bool OSDRenderer::EnsureShaders(ID3D11Device* dev)
{
    if (m_vs && m_ps && m_il && m_vb && m_sampler && m_blend && m_raster && m_depth) return true;

    // Compile VS + PS.
    ID3DBlob* vsBlob = nullptr, * psBlob = nullptr, * err = nullptr;
    HRESULT hr = D3DCompile(k_osdShaderSrc, strlen(k_osdShaderSrc),
                             "wiz3D_osd_vs", nullptr, nullptr, "VSMain", "vs_4_0",
                             0, 0, &vsBlob, &err);
    if (err) { err->Release(); err = nullptr; }
    if (FAILED(hr) || !vsBlob) return false;

    hr = D3DCompile(k_osdShaderSrc, strlen(k_osdShaderSrc),
                     "wiz3D_osd_ps", nullptr, nullptr, "PSMain", "ps_4_0",
                     0, 0, &psBlob, &err);
    if (err) { err->Release(); err = nullptr; }
    if (FAILED(hr) || !psBlob) { vsBlob->Release(); return false; }

    hr = dev->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &m_vs);
    if (FAILED(hr)) { vsBlob->Release(); psBlob->Release(); return false; }
    hr = dev->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &m_ps);
    if (FAILED(hr)) { vsBlob->Release(); psBlob->Release(); return false; }

    const D3D11_INPUT_ELEMENT_DESC ied[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    hr = dev->CreateInputLayout(ied, 2, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &m_il);
    vsBlob->Release(); psBlob->Release();
    if (FAILED(hr)) return false;

    // Dynamic vertex buffer: 6 verts (two triangles), 16 bytes each.
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth      = 6 * 16;
    bd.Usage          = D3D11_USAGE_DYNAMIC;
    bd.BindFlags      = D3D11_BIND_VERTEX_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(dev->CreateBuffer(&bd, nullptr, &m_vb))) return false;

    // Linear sampler, clamp. Point would make ClearType AA transitions jaggy.
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter   = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD   = D3D11_FLOAT32_MAX;
    if (FAILED(dev->CreateSamplerState(&sd, &m_sampler))) return false;

    // Standard "over" alpha blend: Src.rgb * Src.a + Dst.rgb * (1 - Src.a).
    D3D11_BLEND_DESC blendDesc = {};
    blendDesc.RenderTarget[0].BlendEnable           = TRUE;
    blendDesc.RenderTarget[0].SrcBlend              = D3D11_BLEND_SRC_ALPHA;
    blendDesc.RenderTarget[0].DestBlend             = D3D11_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].BlendOp               = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].SrcBlendAlpha         = D3D11_BLEND_ONE;
    blendDesc.RenderTarget[0].DestBlendAlpha        = D3D11_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].BlendOpAlpha          = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(dev->CreateBlendState(&blendDesc, &m_blend))) return false;

    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    if (FAILED(dev->CreateRasterizerState(&rd, &m_raster))) return false;

    D3D11_DEPTH_STENCIL_DESC dsd = {};
    dsd.DepthEnable   = FALSE;
    dsd.StencilEnable = FALSE;
    if (FAILED(dev->CreateDepthStencilState(&dsd, &m_depth))) return false;

    return true;
}

bool OSDRenderer::Init(ID3D11Device* device)
{
    if (m_initDone)   return true;
    if (m_initFailed) return false;
    if (!device)      return false;

    m_dev = device;

    if (!EnsureShaders(device)) { ReleaseAll(); m_initFailed = true; return false; }

    // Inter v4 is embedded as RT_RCDATA and loaded here for an SR-Loom /
    // Stereopticon visual match, but 2026-10-18 field build hit SPACE
    // being substituted with a right-arrow glyph in every rendered string
    // (user screenshot: "wiz3D→Hotkeys", "Toggle→stereo→on→/→off", …).
    // Inter v4 ships GSUB rules (contextual alternates / stylistic sets)
    // that GDI's shaper is honoring on this Windows build; the space
    // glyph gets substituted with a visible-whitespace symbol. Until we
    // either strip GSUB from the embedded TTFs or move to a shaper that
    // lets us disable features, fall back to Arial unconditionally.
    // The DLL still ships the TTFs (resource-embedded, free) so flipping
    // back is one edit when Inter rendering is sorted.
    if (false && LoadInterFontPrivate())
        m_fontFace = L"Inter";

    int px = 0;
    m_fontStatus.font   = MakeFont(m_fontFace, 20, FW_SEMIBOLD, px); m_fontStatus.heightPx   = px;
    m_fontFps.font      = MakeFont(m_fontFace, 16, FW_SEMIBOLD, px); m_fontFps.heightPx      = px;
    m_fontHelp.font     = MakeFont(m_fontFace, 22, FW_BOLD,     px); m_fontHelp.heightPx     = px;
    m_fontHelpBody.font = MakeFont(m_fontFace, 15, FW_NORMAL,   px); m_fontHelpBody.heightPx = px;
    if (!m_fontStatus.font || !m_fontFps.font || !m_fontHelp.font || !m_fontHelpBody.font) {
        ReleaseAll(); m_initFailed = true; return false;
    }

    // Fixed slot sizes picked to fit the maximum content.
    // Status slot is wider than it visually needs to be: paired with the 94%
    // horizontal squeeze in DrawTextLine, we want enough DIB room to hold the
    // worst-case status string ("wiz3D | Preset 2 | Sep 100% | Conv 1000 units
    // [Eyes Swapped]") without clipping right-side glyphs.
    if (!m_slots[(int)Slot::Status].Create(device, 720, 48))      { ReleaseAll(); m_initFailed = true; return false; }
    if (!m_slots[(int)Slot::Fps].Create(device, 180, 36))         { ReleaseAll(); m_initFailed = true; return false; }
    if (!m_slots[(int)Slot::HotkeyHelp].Create(device, 720, 520)) { ReleaseAll(); m_initFailed = true; return false; }
    // 0.5.0 ShaderHunter: two-line bottom-centre strip. 820x72 fits
    // "HUNT  VS:0x12345678  draws:999999  cycle 999/9999" on line 1 plus the
    // hotkey legend on line 2 at the status font size.
    if (!m_slots[(int)Slot::Hunter].Create(device, 820, 72))      { ReleaseAll(); m_initFailed = true; return false; }
    // 0.4.1 save-toast: bottom-right. Width fits a typical filename
    //   "0x12345678_VS_20261021_225834.txt" (34 chars) at the FPS font size
    //   plus a small side margin.
    if (!m_slots[(int)Slot::SaveToast].Create(device, 480, 72))   { ReleaseAll(); m_initFailed = true; return false; }

    m_initDone = true;
    return true;
}

void OSDRenderer::ReleaseAll()
{
    for (int i = 0; i < (int)Slot::_Count; ++i) m_slots[i].Destroy();
    m_fontHelpBody.Destroy();
    m_fontHelp.Destroy();
    m_fontFps.Destroy();
    m_fontStatus.Destroy();
    UnloadInterFontPrivate();
    m_fontFace = L"Arial";
    if (m_depth)   { m_depth->Release();   m_depth   = nullptr; }
    if (m_raster)  { m_raster->Release();  m_raster  = nullptr; }
    if (m_blend)   { m_blend->Release();   m_blend   = nullptr; }
    if (m_sampler) { m_sampler->Release(); m_sampler = nullptr; }
    if (m_vb)      { m_vb->Release();      m_vb      = nullptr; }
    if (m_il)      { m_il->Release();      m_il      = nullptr; }
    if (m_ps)      { m_ps->Release();      m_ps      = nullptr; }
    if (m_vs)      { m_vs->Release();      m_vs      = nullptr; }
    m_initDone = false;
    m_initFailed = false;
    m_lastStatus[0] = m_lastFps[0] = L'\0';
    m_helpTextRendered = false;
    m_dev = nullptr;
}

// Load Inter TTFs from the DLL's embedded RT_RCDATA resources into the
// process's private font space. The TTFs are bound to resource IDs in
// S3DWrapper10/app.rc so no on-disk Fonts/ folder needs to ship with the
// wrapper. Returns true if at least one TTF loaded; the caller falls back
// to Arial if none did.
//
// 0.4.0+: Inter TTFs are embedded in S3DWrapperD3D10.dll as RT_RCDATA
// resources (IDs IDR_INTER_REGULAR/SEMIBOLD/BOLD, defined in resource.h and
// bound to the .ttf files in app.rc). AddFontMemResourceEx registers each
// TTF from memory and returns an opaque HANDLE we feed to
// RemoveFontMemResourceEx at teardown. Dropped the on-disk Fonts/ folder
// dependency: deployment is now a single DLL per leaf.
//
// Proton note: Wine's GDI32 implements AddFontMemResourceEx correctly, so
// this works under Proton too -- D3D11 goes via DXVK; OSD rendering stays
// on GDI's side of the fence.
static bool LoadRCDATAFontIntoMem(HMODULE hMod, LPCWSTR resName, HANDLE& outHandle)
{
    outHandle = nullptr;
    HRSRC hRes = ::FindResourceW(hMod, resName, RT_RCDATA);
    if (!hRes) return false;
    DWORD size = ::SizeofResource(hMod, hRes);
    HGLOBAL hData = ::LoadResource(hMod, hRes);
    if (!hData || !size) return false;
    void* data = ::LockResource(hData);
    if (!data) return false;
    DWORD numFonts = 0;
    outHandle = ::AddFontMemResourceEx(data, size, nullptr, &numFonts);
    return outHandle != nullptr && numFonts > 0;
}

bool OSDRenderer::LoadInterFontPrivate()
{
    HMODULE hMod = nullptr;
    // Resolve this DLL's handle so FindResource reads from our own module.
    if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              reinterpret_cast<LPCWSTR>(&LoadRCDATAFontIntoMem), &hMod))
        return false;

    static const LPCWSTR kResNames[3] = {
        L"INTER_REGULAR", L"INTER_SEMIBOLD", L"INTER_BOLD"
    };
    m_interLoadedCount = 0;
    for (int i = 0; i < 3; ++i)
    {
        if (LoadRCDATAFontIntoMem(hMod, kResNames[i], m_interMemHandles[i]))
            m_interLoadedCount++;
        else
            m_interMemHandles[i] = nullptr;
    }
    return m_interLoadedCount > 0;
}

void OSDRenderer::UnloadInterFontPrivate()
{
    for (int i = 0; i < 3; ++i)
    {
        if (m_interMemHandles[i])
        {
            ::RemoveFontMemResourceEx(m_interMemHandles[i]);
            m_interMemHandles[i] = nullptr;
        }
    }
    m_interLoadedCount = 0;
}

void OSDRenderer::NoteSettingChanged()
{
    // 2 seconds like DX9 BaseStereoRenderer_Main.cpp m_nOSDShowTimeLeft.
    m_statusFadeDeadline = GetTickCount64() + 2000;
}

void OSDRenderer::TickFrame()
{
    ULONGLONG now = GetTickCount64();
    ++m_frameCountSinceSample;
    if (m_lastFpsSample == 0) m_lastFpsSample = now;
    if (now - m_lastFpsSample >= 250) {
        const double seconds = (double)(now - m_lastFpsSample) / 1000.0;
        const double instantFps = seconds > 0 ? (double)m_frameCountSinceSample / seconds : 0.0;
        // Simple EMA so the number stops flickering.
        m_fpsEMA = (m_fpsEMA == 0.f) ? (float)instantFps
                                     : (0.6f * (float)instantFps + 0.4f * m_fpsEMA);
        m_frameCountSinceSample = 0;
        m_lastFpsSample = now;
    }

    // Detect hotkey-driven setting changes vs the last-frame snapshot and
    // auto-trigger the 2s status pop. Avoids wiring a callback through
    // the shared KeyboardHook thread for every hotkey.
    const CameraPreset* p = gInfo.Input.GetActivePreset();
    const float stereoBase  = p ? p->StereoBase   : 0.f;
    const float oneDivZPS   = p ? p->One_div_ZPS  : 0.f;
    const bool  swapEyes    = gInfo.Input.SwapEyes != 0;
    const unsigned preset   = gInfo.Input.ActivePreset;
    if (m_lastActivePreset == (unsigned)-1) {
        // Prime the snapshot on first tick without firing the pop.
        m_lastActivePreset = preset;
        m_lastStereoBase   = stereoBase;
        m_lastOneDivZPS    = oneDivZPS;
        m_lastSwapEyes     = swapEyes;
    } else if (preset != m_lastActivePreset ||
               stereoBase != m_lastStereoBase ||
               oneDivZPS  != m_lastOneDivZPS  ||
               swapEyes   != m_lastSwapEyes) {
        NoteSettingChanged();
        m_lastActivePreset = preset;
        m_lastStereoBase   = stereoBase;
        m_lastOneDivZPS    = oneDivZPS;
        m_lastSwapEyes     = swapEyes;
    }
}

bool OSDRenderer::WantStatus() const
{
    // ShowOSD is a tri-state (0 = off, 1 = popup-only, 2 = always-on):
    //   2 -- strip renders every frame.
    //   1 -- strip only during the 2s fade after a hotkey-driven setting
    //        change (NoteSettingChanged sets m_statusFadeDeadline).
    //   0 -- strip is fully off, even for the popup.
    if (gInfo.ShowOSD >= 2) return true;
    if (gInfo.ShowOSD == 0) return false;
    return GetTickCount64() < m_statusFadeDeadline;
}

bool OSDRenderer::WantFps() const
{
    return gInfo.Input.ShowFPS != 0;
}

bool OSDRenderer::WantHotkeyHelp() const
{
    return gInfo.Input.ShowHotKeyOSD != 0;
}

// Status: "wiz3D | Preset N | Sep X% | Conv Yunits [| AutoFocus] [| EyesSwapped]"
void OSDRenderer::BuildStatusText(wchar_t* out, size_t cap) const
{
    const CameraPreset* p = gInfo.Input.GetActivePreset();
    const int presetIdx = (int)gInfo.Input.ActivePreset + 1;  // 1-based for user
    const float sepPercent = p ? (p->StereoBase / 0.008f) : 0.f;  // StereoBase = percent * 0.008
    const float convUnits  = (p && p->One_div_ZPS != 0.f) ? (1.f / p->One_div_ZPS) : 0.f;
    // Convergence shown at 2 decimals so a 4 px-per-press hotkey tap is
    // visible even at 4K BB where each step moves convUnits by ~0.02 units
    // (one decimal would collapse 3-4 consecutive presses onto the same
    // "2.0" reading before the user saw any number change).
    const bool af = p && p->AutoFocusEnable;
    if (af && gInfo.Input.SwapEyes) {
        _snwprintf_s(out, cap, _TRUNCATE,
                     L"wiz3D   |   Preset %d   |   Sep %.0f%%   |   Conv %.2f   |   AutoFocus   |   Eyes Swapped",
                     presetIdx, sepPercent, convUnits);
    } else if (af) {
        _snwprintf_s(out, cap, _TRUNCATE,
                     L"wiz3D   |   Preset %d   |   Sep %.0f%%   |   Conv %.2f   |   AutoFocus",
                     presetIdx, sepPercent, convUnits);
    } else if (gInfo.Input.SwapEyes) {
        _snwprintf_s(out, cap, _TRUNCATE,
                     L"wiz3D   |   Preset %d   |   Sep %.0f%%   |   Conv %.2f   |   Eyes Swapped",
                     presetIdx, sepPercent, convUnits);
    } else {
        _snwprintf_s(out, cap, _TRUNCATE,
                     L"wiz3D   |   Preset %d   |   Sep %.0f%%   |   Conv %.2f",
                     presetIdx, sepPercent, convUnits);
    }
}

void OSDRenderer::BuildFpsText(wchar_t* out, size_t cap) const
{
    _snwprintf_s(out, cap, _TRUNCATE, L"%.0f fps", m_fpsEMA);
}

bool OSDRenderer::WantHunter() const
{
    return ShaderHunter::IsActive();
}

// Save-toast lives independently of hunt mode so the toast still shows if
// the user exits hunt mode between pressing F3 and the deadline expiring.
bool OSDRenderer::WantSaveToast() const
{
    const ShaderHunter::State s = ShaderHunter::GetState();
    return s.flashDeadline != 0 && s.flashLeaf[0] != '\0';
}

// Two-line hunter status, rendered bottom-centre when hunt mode is on.
//   line 1: HUNT  VS:0xABC12345  draws:847  cycle 23/142
//           — OR "HUNT  none selected  (N CRCs drawn last frame)" when no
//              shader has been cycled onto yet.
//   line 2: Num1 prev   Num2 next   Num3 capture   Num0 exit (always).
// Pulled from the ShaderHunter's atomic snapshot — no locks held while
// GDI renders, so a cycle change mid-frame is fine.
void OSDRenderer::BuildHunterText(wchar_t* line1, size_t cap1,
                                  wchar_t* line2, size_t cap2) const
{
    const ShaderHunter::State s = ShaderHunter::GetState();
    if (!s.active)
    {
        line1[0] = L'\0';
        line2[0] = L'\0';
        return;
    }
    if (s.hiddenCRC == 0)
    {
        _snwprintf_s(line1, cap1, _TRUNCATE,
                     L"HUNT  none selected  (%zu CRCs drawn last frame)",
                     s.cycleSize);
    }
    else
    {
        _snwprintf_s(line1, cap1, _TRUNCATE,
                     L"HUNT  %hs:0x%08X  draws:%u  cycle %zu/%zu",
                     (s.hiddenStage == HuntStage::VS ? "VS" : "PS"),
                     s.hiddenCRC, s.drawsForHidden, s.cycleIdx, s.cycleSize);
    }
    _snwprintf_s(line2, cap2, _TRUNCATE,
                 L"Num1 prev   Num2 next   Num3 capture   Num0 exit");
}

// 0.4.1 save-toast: bottom-right, two lines like "Shader saved" / "<leaf>".
// Rendered as its own slot (SaveToast) so it stays visible even when hunt
// mode is off (the user may have exited hunt mode in the 2 s window between
// pressing F3 and the toast expiring).
void OSDRenderer::BuildSaveToastText(wchar_t* line1, size_t cap1,
                                     wchar_t* line2, size_t cap2) const
{
    const ShaderHunter::State s = ShaderHunter::GetState();
    if (s.flashDeadline == 0 || s.flashLeaf[0] == '\0')
    {
        line1[0] = L'\0';
        line2[0] = L'\0';
        return;
    }
    _snwprintf_s(line1, cap1, _TRUNCATE, L"Shader saved");
    _snwprintf_s(line2, cap2, _TRUNCATE, L"%hs", s.flashLeaf);
}

// Draw a textured quad. (x, y) and (w, h) are in pixels in the real BB
// coordinate system; converted to NDC per draw. Samples the full texture
// (uv 0..1) regardless of the slot's visible extent -- the slot's
// transparent pixels alpha-blend to a no-op over the game content.
void OSDRenderer::DrawQuad(ID3D11DeviceContext* ctx,
                           ID3D11ShaderResourceView* srv,
                           float x, float y, float w, float h,
                           UINT bbW, UINT bbH)
{
    if (!srv || !m_vb) return;

    // Pixel -> NDC. Pixel origin top-left, NDC origin centre, Y up.
    const float x0 = (x / (float)bbW)         * 2.f - 1.f;
    const float x1 = ((x + w) / (float)bbW)    * 2.f - 1.f;
    const float y0 = 1.f - (y / (float)bbH)         * 2.f;        // top edge
    const float y1 = 1.f - ((y + h) / (float)bbH)   * 2.f;        // bottom edge

    struct V { float x, y; float u, v; };
    V verts[6] = {
        { x0, y0, 0.f, 0.f },   // TL
        { x1, y0, 1.f, 0.f },   // TR
        { x0, y1, 0.f, 1.f },   // BL
        { x0, y1, 0.f, 1.f },   // BL
        { x1, y0, 1.f, 0.f },   // TR
        { x1, y1, 1.f, 1.f },   // BR
    };
    D3D11_MAPPED_SUBRESOURCE m = {};
    if (FAILED(ctx->Map(m_vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)) || !m.pData) return;
    memcpy(m.pData, verts, sizeof(verts));
    ctx->Unmap(m_vb, 0);

    UINT stride = sizeof(V), offset = 0;
    ctx->IASetVertexBuffers(0, 1, &m_vb, &stride, &offset);
    ctx->IASetInputLayout(m_il);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(m_vs, nullptr, 0);
    ctx->GSSetShader(nullptr, nullptr, 0);
    ctx->HSSetShader(nullptr, nullptr, 0);
    ctx->DSSetShader(nullptr, nullptr, 0);
    ctx->PSSetShader(m_ps, nullptr, 0);
    ctx->PSSetShaderResources(0, 1, &srv);
    ctx->PSSetSamplers(0, 1, &m_sampler);
    ctx->Draw(6, 0);
    // Clear the SRV binding so the caller's next PSSetShaderResources on
    // the same slot doesn't trip a D3D11 read/write hazard warning.
    ID3D11ShaderResourceView* nullSRV = nullptr;
    ctx->PSSetShaderResources(0, 1, &nullSRV);
}

void OSDRenderer::Render(ID3D11DeviceContext* ctx,
                         ID3D11RenderTargetView* rtv,
                         UINT bbW, UINT bbH)
{
    if (!ctx || !rtv || bbW == 0 || bbH == 0) return;
    if (!m_initDone) return;      // caller must have called Init() with a real device first

    const bool wantStatus = WantStatus();
    const bool wantFps    = WantFps();
    const bool wantHelp   = WantHotkeyHelp();
    const bool wantHunter = WantHunter();
    const bool wantToast  = WantSaveToast();
    if (!wantStatus && !wantFps && !wantHelp && !wantHunter && !wantToast) return;

    // Update DIBs where content changed.
    if (wantStatus) {
        wchar_t text[128];
        BuildStatusText(text, _countof(text));
        if (wcscmp(text, m_lastStatus) != 0) {
            TexSlot& s = m_slots[(int)Slot::Status];
            s.Clear();
            // No backing panel -- the 2px black outline from DrawTextLine
            // gives readability over any background, matching the DX9 /
            // legacy-DX10 BMFont outlineThickness=2 look.
            RECT textRect = { 12, 4, s.w - 12, s.h - 4 };
            s.DrawTextLine(text, textRect, m_fontStatus.font, RGB(255, 255, 255),
                       DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
            wcscpy_s(m_lastStatus, text);
        }
    }

    if (wantFps) {
        wchar_t text[32];
        BuildFpsText(text, _countof(text));
        if (wcscmp(text, m_lastFps) != 0) {
            TexSlot& s = m_slots[(int)Slot::Fps];
            s.Clear();
            RECT textRect = { 8, 2, s.w - 8, s.h - 2 };
            s.DrawTextLine(text, textRect, m_fontFps.font, RGB(255, 255, 255),
                       DT_RIGHT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
            wcscpy_s(m_lastFps, text);
        }
    }

    if (wantHelp && !m_helpTextRendered) {
        TexSlot& s = m_slots[(int)Slot::HotkeyHelp];
        s.Clear();
        // Hotkey help: no backing panel. Each line gets the 2px outline
        // via DrawTextLine so the list stays readable over any game scene.
        RECT title = { 20, 20, s.w - 20, 60 };
        s.DrawTextLine(L"wiz3D Hotkeys", title, m_fontHelp.font, RGB(255, 255, 255),
                   DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
        // Body: two columns. Keep it tight so it fits in the 720x520 slot.
        // Only the ENTRY hotkey for ShaderHunter is listed here — the cycle/
        // capture/exit keys are already shown on the hunter OSD line 2 while
        // hunt mode is on, and only act there anyway. Listing them in the
        // global help was crowding three ShaderHunter: rows into the menu
        // (user 2026-10-21: "shader names were layered on top of each other
        // making them hard to read" / "you don't need to show the shaderlog
        // shortcut keys, only the one to enter the mode, as those buttons
        // only work when you're inside the mode").
        static const wchar_t* lines[] = {
            L"Numpad *",             L"Toggle stereo on / off",
            L"Numpad +  /  -",       L"Separation (depth strength)",
            L"Shift + Numpad +/-",   L"Convergence (where screen plane sits)",
            L"Numpad 7 / 8 / 9",     L"Preset Low / Default / Strong",
            L"Ctrl + F8",            L"Swap left / right eyes",
            L"Numpad /",             L"Toggle auto-focus",
            L"Ctrl + Numpad /",      L"Toggle laser-sight crosshair",
            L"PrtScn",               L"Stereo screenshot",
            L"Shift + F1",           L"Toggle this hotkey list",
            L"Ctrl + F1",            L"Toggle OSD strip + FPS",
            L"Numpad 0",             L"ShaderHunter: enter / exit hunt mode",
            L"Ctrl + F5",            L"Reload BaseProfile.xml (live)",
        };
        int row = 72;
        const int rowH = 32;
        const int keyX = 32, valueX = 240;
        for (int i = 0; i < _countof(lines); i += 2) {
            RECT k = { keyX,   row, valueX - 8,  row + rowH };
            RECT v = { valueX, row, s.w - 20,    row + rowH };
            s.DrawTextLine(lines[i],     k, m_fontHelpBody.font, RGB(180, 220, 255),
                       DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
            s.DrawTextLine(lines[i + 1], v, m_fontHelpBody.font, RGB(230, 230, 230),
                       DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
            row += rowH;
        }
        m_helpTextRendered = true;
    }
    if (!wantHelp && m_helpTextRendered) m_helpTextRendered = false;    // next show re-renders

    // 0.5.0 ShaderHunter: two-line DIB. Re-rendered when either line changed
    // (hunted CRC, draw count, or cycle position moved). The line-2 legend is
    // constant text so it only prints on first activation of the slot.
    if (wantHunter) {
        wchar_t line1[96];
        wchar_t line2[128];
        BuildHunterText(line1, _countof(line1), line2, _countof(line2));
        if (wcscmp(line1, m_lastHunter1) != 0 || wcscmp(line2, m_lastHunter2) != 0) {
            TexSlot& s = m_slots[(int)Slot::Hunter];
            s.Clear();
            RECT line1Rect = { 12, 4,  s.w - 12, 36 };
            RECT line2Rect = { 12, 38, s.w - 12, 68 };
            // Line 1: status font, white. Line 2: FPS font, dim grey (legend).
            s.DrawTextLine(line1, line1Rect, m_fontStatus.font, RGB(255, 255, 255),
                       DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
            s.DrawTextLine(line2, line2Rect, m_fontFps.font,    RGB(200, 200, 200),
                       DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
            wcscpy_s(m_lastHunter1, line1);
            wcscpy_s(m_lastHunter2, line2);
        }
    }

    // 0.4.1 save-toast: build + upload two-line DIB (bottom-right). Updated
    // when either line changes. The leaf changes once per capture; line 1
    // ("Shader saved") is constant per toast life.
    if (wantToast) {
        wchar_t t1[32], t2[160];
        BuildSaveToastText(t1, _countof(t1), t2, _countof(t2));
        if (wcscmp(t1, m_lastToast1) != 0 || wcscmp(t2, m_lastToast2) != 0) {
            TexSlot& s = m_slots[(int)Slot::SaveToast];
            s.Clear();
            RECT line1Rect = { 12, 4,  s.w - 12, 36 };
            RECT line2Rect = { 12, 38, s.w - 12, 68 };
            s.DrawTextLine(t1, line1Rect, m_fontStatus.font, RGB(255, 255, 255),
                       DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
            s.DrawTextLine(t2, line2Rect, m_fontFps.font,    RGB(200, 220, 255),
                       DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
            wcscpy_s(m_lastToast1, t1);
            wcscpy_s(m_lastToast2, t2);
        }
    } else if (m_lastToast1[0] != L'\0') {
        // Toast just expired — clear the slot so a later re-trigger re-renders.
        m_slots[(int)Slot::SaveToast].Clear();
        m_lastToast1[0] = L'\0';
        m_lastToast2[0] = L'\0';
    }

    // Upload any dirty DIBs.
    if (wantStatus) m_slots[(int)Slot::Status].Upload(ctx);
    if (wantFps)    m_slots[(int)Slot::Fps].Upload(ctx);
    if (wantHelp)   m_slots[(int)Slot::HotkeyHelp].Upload(ctx);
    if (wantHunter) m_slots[(int)Slot::Hunter].Upload(ctx);
    if (wantToast)  m_slots[(int)Slot::SaveToast].Upload(ctx);

    // State setup once for all draws.
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    D3D11_VIEWPORT vp = {};
    vp.Width = (float)bbW; vp.Height = (float)bbH;
    vp.MinDepth = 0.f; vp.MaxDepth = 1.f;
    ctx->RSSetViewports(1, &vp);
    ctx->RSSetState(m_raster);
    const FLOAT blendFactor[4] = { 1, 1, 1, 1 };
    ctx->OMSetBlendState(m_blend, blendFactor, 0xFFFFFFFFu);
    ctx->OMSetDepthStencilState(m_depth, 0);

    // 0.4.1 OSD position offsets (legacy DX10 D3DDeviceWrapper.cpp:830-850).
    //  - FrustumAdjustMode + BlackAreaWidth add comfort bars at the LEFT /
    //    RIGHT edges; shift the top-left Status X right and the top-right
    //    FPS X left by that width so the text clears them. SeparationMode=1
    //    doubles the shift (half-SBS: each eye has its own bar).
    //  - FixNone16x9TextPosition shifts top-anchored text DOWN by the top
    //    letterbox height, when the game render is 16:9 but the BB is taller.
    //    Hunter (bottom-anchored) shifts UP by the matching bottom letterbox.
    float barX = 0.f;
    if (gInfo.Input.StereoActive && gInfo.FrustumAdjustMode)
    {
        const float width = gInfo.BlackAreaWidth * (float)bbW;
        barX = (gInfo.SeparationMode == 1) ? 2.f * width : width;
    }
    float letterboxY = 0.f;
    if (gInfo.FixNone16x9TextPosition && bbW > 0)
    {
        const float render16x9H = (float)bbW / (16.f / 9.f);
        letterboxY = (std::max<float>)(0.f, ((float)bbH - render16x9H) * 0.5f);
    }

    // Draw order: hotkey help underneath (full-screen dim), then status, then FPS.
    if (wantHelp) {
        TexSlot& s = m_slots[(int)Slot::HotkeyHelp];
        // Centred, max 720x520 pixels. Scale down proportionally only if BB
        // is smaller than the slot (rare -- 480p-era displays). Otherwise
        // 1:1 pixel mapping. Centred placement already clears both comfort
        // bars and letterbox so no offsets apply here.
        float dw = (float)(std::min<UINT>)(bbW > 40 ? bbW - 40 : (UINT)s.w, (UINT)s.w);
        float dh = (float)(std::min<UINT>)(bbH > 40 ? bbH - 40 : (UINT)s.h, (UINT)s.h);
        float dx = ((float)bbW - dw) * 0.5f;
        float dy = ((float)bbH - dh) * 0.5f;
        DrawQuad(ctx, s.srv, dx, dy, dw, dh, bbW, bbH);
    }
    if (wantStatus) {
        TexSlot& s = m_slots[(int)Slot::Status];
        DrawQuad(ctx, s.srv, 20.f + barX, 20.f + letterboxY, (float)s.w, (float)s.h, bbW, bbH);
    }
    if (wantFps) {
        TexSlot& s = m_slots[(int)Slot::Fps];
        DrawQuad(ctx, s.srv, (float)bbW - 20.f - barX - (float)s.w, 20.f + letterboxY, (float)s.w, (float)s.h, bbW, bbH);
    }
    // 0.5.0 ShaderHunter: bottom-centre strip. Fits inside bb width with
    // 20px side margins when bb is wider than the slot; otherwise shrinks
    // to fit (keeps the status visible even at 480p). Letterbox lifts the
    // strip clear of the bottom bar when FixNone16x9TextPosition is on.
    if (wantHunter) {
        TexSlot& s = m_slots[(int)Slot::Hunter];
        float dw = (float)(std::min<UINT>)(bbW > 40 ? bbW - 40 : (UINT)s.w, (UINT)s.w);
        float dx = ((float)bbW - dw) * 0.5f;
        float dy = (float)bbH - (float)s.h - 20.f - letterboxY;
        if (dy < 0) dy = 0;
        DrawQuad(ctx, s.srv, dx, dy, dw, (float)s.h, bbW, bbH);
    }
    // 0.4.1 save-toast: bottom-right corner (clears the right-side comfort
    // bar + bottom letterbox like FPS does). Sits above the hunter strip
    // when both are visible (the hunter strip is centred, the toast is far
    // right — no visual collision at any sensible BB width).
    if (wantToast) {
        TexSlot& s = m_slots[(int)Slot::SaveToast];
        float dx = (float)bbW - 20.f - barX - (float)s.w;
        float dy = (float)bbH - (float)s.h - 20.f - letterboxY;
        if (dx < 0) dx = 0;
        if (dy < 0) dy = 0;
        DrawQuad(ctx, s.srv, dx, dy, (float)s.w, (float)s.h, bbW, bbH);
    }
}

}  // namespace wiz3d
