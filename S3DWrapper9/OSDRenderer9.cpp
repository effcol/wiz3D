/*
* wiz3D DX9 OSD renderer -- see OSDRenderer9.h for the design notes.
*/
#include "stdafx.h"
#include "OSDRenderer9.h"
#include "ShaderHunter9.h"               // 0.4.1 hunt-mode OSD slot
#include "..\S3DAPI\GlobalData.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <algorithm>
#include <vector>

#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

#ifdef DrawText
#undef DrawText
#endif

namespace wiz3d
{

// -----------------------------------------------------------------------
// Shared helpers (duplicated from the DX11 OSDRenderer to keep the two
// wrappers decoupled -- the DX11 one owns its own TexSlot plumbing).
// -----------------------------------------------------------------------
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
        // Grayscale AA so our outline compositor can use R==G==B as a
        // single-channel coverage mask (ClearType would write different
        // per-subpixel values and break the mask).
        ANTIALIASED_QUALITY,
        VARIABLE_PITCH | FF_SWISS,
        face);
}

// Load a single Inter TTF from this DLL's embedded RT_RCDATA resources
// into the process's private font space via AddFontMemResourceEx.
// 0.4.0+: replaces the on-disk Fonts/ folder requirement so S3DWrapperD3D9.dll
// ships standalone. Returns true on success and populates outHandle.
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

void OSDRenderer9::GDIFont::Destroy()
{
    if (font) { DeleteObject(font); font = nullptr; }
    heightPx = 0;
}

// -----------------------------------------------------------------------
// TexSlot -- DX9 variant uses IDirect3DTexture9 (DYNAMIC, D3DPOOL_DEFAULT)
// instead of the DX11 ID3D11Texture2D.
// -----------------------------------------------------------------------
bool OSDRenderer9::TexSlot::Create(IDirect3DDevice9* dev, int width, int height)
{
    w = width; h = height;

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
    SetBkMode(dc, TRANSPARENT);

    // DX9 texture: DYNAMIC + D3DPOOL_DEFAULT so we can Lock/Unlock cheaply
    // every frame if content changes. A8R8G8B8 matches our DIB's BGRA
    // layout (D3D names it A8R8G8B8 but the memory is Blue in byte 0).
    HRESULT hr = dev->CreateTexture(w, h, 1, D3DUSAGE_DYNAMIC,
                                     D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
                                     &tex, nullptr);
    if (FAILED(hr) || !tex) { Destroy(); return false; }

    Clear();
    dirty = true;
    return true;
}

void OSDRenderer9::TexSlot::Destroy()
{
    if (tex)  { tex->Release();  tex  = nullptr; }
    if (dc && prev) { SelectObject(dc, prev); prev = nullptr; }
    if (dib)  { DeleteObject(dib); dib = nullptr; }
    if (dc)   { DeleteDC(dc);     dc  = nullptr; }
    bits = nullptr; w = h = 0; dirty = false;
}

void OSDRenderer9::TexSlot::Clear()
{
    if (!bits) return;
    memset(bits, 0, (size_t)w * h * 4);
    dirty = true;
}

void OSDRenderer9::TexSlot::DrawRect(const RECT& r, COLORREF rgb, BYTE alpha)
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
}

// Draw text with a 2-pixel black outline (matches DX9/legacy-DX10 BMFont
// outlineThickness=2 look), no backing panel. Mirrors the DX11
// OSDRenderer::TexSlot::DrawTextLine implementation.
void OSDRenderer9::TexSlot::DrawTextLine(const wchar_t* text, const RECT& r, HFONT font,
                                         COLORREF rgb, UINT flags)
{
    if (!dc || !bits || !font || !text) return;

    HFONT prevFont = (HFONT)SelectObject(dc, font);

    const int x0 = (std::max)(0L, (long)r.left),   x1 = (std::min)((long)w, (long)r.right);
    const int y0 = (std::max)(0L, (long)r.top),    y1 = (std::min)((long)h, (long)r.bottom);
    const int rw = x1 - x0, rh = y1 - y0;
    if (rw <= 0 || rh <= 0) { SelectObject(dc, prevFont); return; }

    for (int y = y0; y < y1; ++y) {
        BYTE* row = bits + (size_t)y * w * 4;
        for (int x = x0; x < x1; ++x) {
            BYTE* p = row + x * 4;
            p[0] = p[1] = p[2] = p[3] = 0;
        }
    }

    SetTextColor(dc, RGB(255, 255, 255));
    RECT rc = r;
    // Previously squeezed glyphs to 94% horizontal via SetWorldTransform.
    // Dropped: it was garbling narrow glyphs (user 2026-10-18 report of
    // "wiz3D" rendering as "wz3D" at 15 px) and clipping the right side
    // of longer body strings in the help overlay. See the DX11
    // OSDRenderer.cpp comment for the full rationale.
    ::DrawTextW(dc, text, -1, &rc, flags);

    std::vector<BYTE> mask((size_t)rw * rh, 0);
    for (int y = 0; y < rh; ++y) {
        const BYTE* row = bits + (size_t)(y + y0) * w * 4 + (size_t)x0 * 4;
        for (int x = 0; x < rw; ++x) mask[(size_t)y * rw + x] = row[x * 4 + 2];
    }

    const BYTE Rt = GetRValue(rgb), Gt = GetGValue(rgb), Bt = GetBValue(rgb);
    const int kOutline = 2;
    for (int y = 0; y < rh; ++y) {
        BYTE* row = bits + (size_t)(y + y0) * w * 4 + (size_t)x0 * 4;
        for (int x = 0; x < rw; ++x) {
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
}

void OSDRenderer9::TexSlot::Upload()
{
    if (!dirty || !tex || !bits) return;
    D3DLOCKED_RECT lr = {};
    if (FAILED(tex->LockRect(0, &lr, nullptr, D3DLOCK_DISCARD))) return;
    const size_t rowBytes = (size_t)w * 4;
    for (int y = 0; y < h; ++y) {
        memcpy((BYTE*)lr.pBits + (size_t)y * lr.Pitch, bits + (size_t)y * rowBytes, rowBytes);
    }
    tex->UnlockRect(0);
    dirty = false;
}

// -----------------------------------------------------------------------
// OSDRenderer9
// -----------------------------------------------------------------------
OSDRenderer9::OSDRenderer9() {}
OSDRenderer9::~OSDRenderer9() { ReleaseAll(); }

bool OSDRenderer9::LoadInterFontPrivate()
{
    HMODULE hMod = nullptr;
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

void OSDRenderer9::UnloadInterFontPrivate()
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

bool OSDRenderer9::Init(IDirect3DDevice9* device)
{
    if (m_initDone)   return true;
    if (m_initFailed) return false;
    if (!device)      return false;

    m_dev = device;

    // See the matching DX11 OSDRenderer::Initialize comment: Inter v4's
    // GSUB rules get GDI to substitute SPACE with a right-arrow glyph on
    // the 2026-10-18 field build, so we fall back to Arial until the
    // Inter embedding is reworked. The TTFs stay as RT_RCDATA for the
    // one-line flip-back.
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

    // Status slot widened to 720 to match the DX11 OSD. Pairs with the 94%
    // horizontal squeeze in DrawTextLine so the worst-case status string
    // fits without clipping.
    if (!m_slots[(int)Slot::Status].Create(device, 720, 48))      { ReleaseAll(); m_initFailed = true; return false; }
    if (!m_slots[(int)Slot::Fps].Create(device, 180, 36))         { ReleaseAll(); m_initFailed = true; return false; }
    if (!m_slots[(int)Slot::HotkeyHelp].Create(device, 720, 520)) { ReleaseAll(); m_initFailed = true; return false; }
    // 0.4.1 ShaderHunter DX9: two-line bottom-centre strip. Same 820x72
    // footprint as the DX11 slot so the OSD looks the same across APIs.
    if (!m_slots[(int)Slot::Hunter].Create(device, 820, 72))      { ReleaseAll(); m_initFailed = true; return false; }
    // 0.4.1 save-toast — mirror of DX11.
    if (!m_slots[(int)Slot::SaveToast].Create(device, 480, 72))   { ReleaseAll(); m_initFailed = true; return false; }

    m_initDone = true;
    return true;
}

void OSDRenderer9::OnDeviceLost()
{
    // D3D9 Reset() invalidates D3DPOOL_DEFAULT resources. Drop just the
    // textures; the GDI/DIB side survives, and Init re-creates the
    // textures on the next Render call.
    for (int i = 0; i < (int)Slot::_Count; ++i) {
        if (m_slots[i].tex) { m_slots[i].tex->Release(); m_slots[i].tex = nullptr; }
        m_slots[i].dirty = true;    // force upload after re-create
    }
    m_initDone = false;
}

void OSDRenderer9::ReleaseAll()
{
    for (int i = 0; i < (int)Slot::_Count; ++i) m_slots[i].Destroy();
    m_fontHelpBody.Destroy();
    m_fontHelp.Destroy();
    m_fontFps.Destroy();
    m_fontStatus.Destroy();
    UnloadInterFontPrivate();
    m_fontFace = L"Arial";
    m_initDone = false;
    m_initFailed = false;
    m_lastStatus[0] = m_lastFps[0] = L'\0';
    m_helpTextRendered = false;
    m_dev = nullptr;
}

void OSDRenderer9::NoteSettingChanged()
{
    m_statusFadeDeadline = GetTickCount64() + 2000;
}

void OSDRenderer9::TickFrame()
{
    ULONGLONG now = GetTickCount64();
    ++m_frameCountSinceSample;
    if (m_lastFpsSample == 0) m_lastFpsSample = now;
    if (now - m_lastFpsSample >= 250) {
        const double seconds = (double)(now - m_lastFpsSample) / 1000.0;
        const double instantFps = seconds > 0 ? (double)m_frameCountSinceSample / seconds : 0.0;
        m_fpsEMA = (m_fpsEMA == 0.f) ? (float)instantFps
                                     : (0.6f * (float)instantFps + 0.4f * m_fpsEMA);
        m_frameCountSinceSample = 0;
        m_lastFpsSample = now;
    }

    const CameraPreset* p = gInfo.Input.GetActivePreset();
    const float stereoBase  = p ? p->StereoBase   : 0.f;
    const float oneDivZPS   = p ? p->One_div_ZPS  : 0.f;
    const bool  swapEyes    = gInfo.Input.SwapEyes != 0;
    const unsigned preset   = gInfo.Input.ActivePreset;
    if (m_lastActivePreset == (unsigned)-1) {
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

bool OSDRenderer9::WantStatus() const
{
    // ShowOSD tri-state (see OSDRenderer::WantStatus for the full note):
    //   2 = always-on, 1 = popup-only (default), 0 = fully off.
    if (gInfo.ShowOSD >= 2) return true;
    if (gInfo.ShowOSD == 0) return false;
    return GetTickCount64() < m_statusFadeDeadline;
}

bool OSDRenderer9::WantFps() const
{
    return gInfo.Input.ShowFPS != 0;
}

bool OSDRenderer9::WantHotkeyHelp() const
{
    return gInfo.Input.ShowHotKeyOSD != 0;
}

bool OSDRenderer9::WantHunter() const
{
    return wiz3d::dx9::HunterIsActive();
}

// Save-toast survives exiting hunt mode so a user who presses F3 then
// Numpad 0 still sees the confirmation. Mirrors DX11 behaviour.
bool OSDRenderer9::WantSaveToast() const
{
    const wiz3d::dx9::HunterState s = wiz3d::dx9::HunterGetState();
    return s.flashDeadline != 0 && s.flashLeaf[0] != '\0';
}

// Two-line hunter status, rendered bottom-centre while hunt mode is active.
// Mirror of OSDRenderer::BuildHunterText on DX11.
void OSDRenderer9::BuildHunterText(wchar_t* line1, size_t cap1,
                                   wchar_t* line2, size_t cap2) const
{
    const wiz3d::dx9::HunterState s = wiz3d::dx9::HunterGetState();
    if (!s.active) { line1[0] = L'\0'; line2[0] = L'\0'; return; }
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
                     (s.hiddenStage == wiz3d::dx9::HuntStage::VS ? "VS" : "PS"),
                     s.hiddenCRC, s.drawsForHidden, s.cycleIdx, s.cycleSize);
    }
    _snwprintf_s(line2, cap2, _TRUNCATE,
                 L"Num1 prev   Num2 next   Num3 capture   Num0 exit");
}

// 0.4.1 save-toast — bottom-right "Shader saved" / "<leaf>". Mirror of DX11.
void OSDRenderer9::BuildSaveToastText(wchar_t* line1, size_t cap1,
                                      wchar_t* line2, size_t cap2) const
{
    const wiz3d::dx9::HunterState s = wiz3d::dx9::HunterGetState();
    if (s.flashDeadline == 0 || s.flashLeaf[0] == '\0')
    {
        line1[0] = L'\0';
        line2[0] = L'\0';
        return;
    }
    _snwprintf_s(line1, cap1, _TRUNCATE, L"Shader saved");
    _snwprintf_s(line2, cap2, _TRUNCATE, L"%hs", s.flashLeaf);
}

// Status: "wiz3D | Preset N | Sep X% | Conv Yunits [| AutoFocus] [| EyesSwapped]"
// Mirror of DX11 OSDRenderer::BuildStatusText -- content layer kept in sync so
// the user sees the same strip across DX9 and DX11 StereoInjector paths.
void OSDRenderer9::BuildStatusText(wchar_t* out, size_t cap) const
{
    const CameraPreset* p = gInfo.Input.GetActivePreset();
    const int presetIdx = (int)gInfo.Input.ActivePreset + 1;
    const float sepPercent = p ? (p->StereoBase / 0.008f) : 0.f;
    const float convUnits  = (p && p->One_div_ZPS != 0.f) ? (1.f / p->One_div_ZPS) : 0.f;
    // Convergence at 2 decimals (see DX11 OSDRenderer::BuildStatusText).
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

void OSDRenderer9::BuildFpsText(wchar_t* out, size_t cap) const
{
    _snwprintf_s(out, cap, _TRUNCATE, L"%.0f fps", m_fpsEMA);
}

// Draw a textured screen-space quad with D3DFVF_XYZRHW + D3DFVF_TEX1.
// (x, y, w, h) are in pixels in the real BB coordinate system; vertices
// go straight to the rasteriser (no world/view/projection transform).
void OSDRenderer9::DrawQuad(IDirect3DDevice9* device, IDirect3DTexture9* tex,
                            float x, float y, float w, float h)
{
    if (!tex || !device) return;

    struct TLVert { float x, y, z, rhw; float u, v; };
    const TLVert verts[6] = {
        { x,     y,     0.f, 1.f, 0.f, 0.f }, // TL
        { x + w, y,     0.f, 1.f, 1.f, 0.f }, // TR
        { x,     y + h, 0.f, 1.f, 0.f, 1.f }, // BL
        { x,     y + h, 0.f, 1.f, 0.f, 1.f }, // BL
        { x + w, y,     0.f, 1.f, 1.f, 0.f }, // TR
        { x + w, y + h, 0.f, 1.f, 1.f, 1.f }, // BR
    };

    device->SetTexture(0, tex);
    device->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
    device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, verts, sizeof(TLVert));
    device->SetTexture(0, nullptr);
}

void OSDRenderer9::Render(IDirect3DDevice9* device, UINT bbW, UINT bbH,
                          int originX, int originY)
{
    if (!device || bbW == 0 || bbH == 0) return;
    const float oX = (float)originX;
    const float oY = (float)originY;
    if (m_initFailed) return;
    if (!m_initDone && !Init(device)) return;

    // 0.4.2 DIAG: log the first 40 Render calls so we can see whether the
    // caller is alternating between left and right RTs each frame.
    // Persistent file handle (mirrors ShaderHunter9::HLog) because the
    // previous per-call _wfopen_s pattern wasn't producing output -- most
    // likely a share-mode fight against zlog's own handle on the same
    // path. Keeping the handle open side-steps that.
    static int   s_dbgRenders = 0;
    static FILE* s_dbgFp      = nullptr;
    if (s_dbgRenders < 40) {
        if (!s_dbgFp) {
            wchar_t logPath[MAX_PATH] = {};
            if (GetModuleFileNameW(nullptr, logPath, MAX_PATH)) {
                if (wchar_t* bs = wcsrchr(logPath, L'\\')) {
                    *(bs + 1) = L'\0';
                    lstrcatW(logPath, L"wiz3D_Logs");
                    CreateDirectoryW(logPath, nullptr);
                    lstrcatW(logPath, L"\\wiz3D_proxy.log");
                    s_dbgFp = _wfopen(logPath, L"a");
                }
            }
        }
        if (s_dbgFp) {
            IDirect3DSurface9* curRT = nullptr;
            device->GetRenderTarget(0, &curRT);
            fprintf(s_dbgFp, "[OSD9DIAG] Render #%d  RT=%p  bb=%ux%u\n",
                    s_dbgRenders, (void*)curRT, bbW, bbH);
            fflush(s_dbgFp);
            if (curRT) curRT->Release();
        }
        s_dbgRenders++;
    }

    // Re-create textures if a Reset() dropped them (OnDeviceLost).
    for (int i = 0; i < (int)Slot::_Count; ++i) {
        if (!m_slots[i].tex) {
            HRESULT hr = device->CreateTexture(m_slots[i].w, m_slots[i].h, 1,
                                                D3DUSAGE_DYNAMIC,
                                                D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
                                                &m_slots[i].tex, nullptr);
            if (FAILED(hr) || !m_slots[i].tex) return;
            m_slots[i].dirty = true;
        }
    }

    const bool wantStatus = WantStatus();
    const bool wantFps    = WantFps();
    const bool wantHelp   = WantHotkeyHelp();
    const bool wantHunter = WantHunter();
    const bool wantToast  = WantSaveToast();
    if (!wantStatus && !wantFps && !wantHelp && !wantHunter && !wantToast) return;

    if (wantStatus) {
        wchar_t text[128];
        BuildStatusText(text, _countof(text));
        if (wcscmp(text, m_lastStatus) != 0) {
            TexSlot& s = m_slots[(int)Slot::Status];
            s.Clear();
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
        RECT title = { 20, 20, s.w - 20, 60 };
        s.DrawTextLine(L"wiz3D Hotkeys", title, m_fontHelp.font, RGB(255, 255, 255),
                       DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
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
            // ShaderHunter cycle/capture/exit keys live on the hunter OSD
            // line 2 while hunt mode is active; only the entry hotkey needs
            // to appear in the global menu (user 2026-10-21 feedback).
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
    if (!wantHelp && m_helpTextRendered) m_helpTextRendered = false;

    // 0.4.1 ShaderHunter DX9: two-line DIB. Re-rendered only when either
    // line changed (cycle move or draw-count tick).
    if (wantHunter) {
        wchar_t line1[96];
        wchar_t line2[128];
        BuildHunterText(line1, _countof(line1), line2, _countof(line2));
        if (wcscmp(line1, m_lastHunter1) != 0 || wcscmp(line2, m_lastHunter2) != 0) {
            TexSlot& s = m_slots[(int)Slot::Hunter];
            s.Clear();
            RECT line1Rect = { 12, 4,  s.w - 12, 36 };
            RECT line2Rect = { 12, 38, s.w - 12, 68 };
            s.DrawTextLine(line1, line1Rect, m_fontStatus.font, RGB(255, 255, 255),
                           DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
            s.DrawTextLine(line2, line2Rect, m_fontFps.font,    RGB(200, 200, 200),
                           DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
            wcscpy_s(m_lastHunter1, line1);
            wcscpy_s(m_lastHunter2, line2);
        }
    }

    // 0.4.1 save-toast — bottom-right slot.
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
        m_slots[(int)Slot::SaveToast].Clear();
        m_lastToast1[0] = L'\0';
        m_lastToast2[0] = L'\0';
    }

    if (wantStatus) m_slots[(int)Slot::Status].Upload();
    if (wantFps)    m_slots[(int)Slot::Fps].Upload();
    if (wantHelp)   m_slots[(int)Slot::HotkeyHelp].Upload();
    if (wantHunter) m_slots[(int)Slot::Hunter].Upload();
    if (wantToast)  m_slots[(int)Slot::SaveToast].Upload();

    // Save state via a state block so we can restore the game's render state.
    IDirect3DStateBlock9* sb = nullptr;
    if (FAILED(device->CreateStateBlock(D3DSBT_ALL, &sb)) || !sb) return;

    // Minimal fixed-function state for alpha-blended textured quads.
    device->SetVertexShader(nullptr);
    device->SetPixelShader(nullptr);
    device->SetRenderState(D3DRS_LIGHTING, FALSE);
    device->SetRenderState(D3DRS_ZENABLE, FALSE);
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    device->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    device->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    device->SetTextureStageState(0, D3DTSS_COLOROP,   D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    device->SetTextureStageState(0, D3DTSS_ALPHAOP,   D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    device->SetSamplerState(0, D3DSAMP_ADDRESSU,  D3DTADDRESS_CLAMP);
    device->SetSamplerState(0, D3DSAMP_ADDRESSV,  D3DTADDRESS_CLAMP);

    if (wantHelp) {
        TexSlot& s = m_slots[(int)Slot::HotkeyHelp];
        float dw = (float)(std::min<UINT>)(bbW > 40 ? bbW - 40 : (UINT)s.w, (UINT)s.w);
        float dh = (float)(std::min<UINT>)(bbH > 40 ? bbH - 40 : (UINT)s.h, (UINT)s.h);
        float dx = ((float)bbW - dw) * 0.5f;
        float dy = ((float)bbH - dh) * 0.5f;
        DrawQuad(device, s.tex, oX + dx, oY + dy, dw, dh);
    }
    if (wantStatus) {
        TexSlot& s = m_slots[(int)Slot::Status];
        DrawQuad(device, s.tex, oX + 20.f, oY + 20.f, (float)s.w, (float)s.h);
    }
    if (wantFps) {
        TexSlot& s = m_slots[(int)Slot::Fps];
        DrawQuad(device, s.tex, oX + (float)bbW - 20.f - (float)s.w, oY + 20.f, (float)s.w, (float)s.h);
    }
    if (wantHunter) {
        TexSlot& s = m_slots[(int)Slot::Hunter];
        float dw = (float)(std::min<UINT>)(bbW > 40 ? bbW - 40 : (UINT)s.w, (UINT)s.w);
        float dx = ((float)bbW - dw) * 0.5f;
        float dy = (float)bbH - (float)s.h - 20.f;
        if (dy < 0) dy = 0;
        DrawQuad(device, s.tex, oX + dx, oY + dy, dw, (float)s.h);
    }
    // Save-toast: bottom-right (mirror of DX11).
    if (wantToast) {
        TexSlot& s = m_slots[(int)Slot::SaveToast];
        float dx = (float)bbW - 20.f - (float)s.w;
        float dy = (float)bbH - (float)s.h - 20.f;
        if (dx < 0) dx = 0;
        if (dy < 0) dy = 0;
        DrawQuad(device, s.tex, oX + dx, oY + dy, (float)s.w, (float)s.h);
    }

    sb->Apply();
    sb->Release();
}

}  // namespace wiz3d
