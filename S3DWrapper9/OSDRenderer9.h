/*
* wiz3D OSD renderer — DX9 StereoInjector variant.
*
* Mirrors S3DWrapper10/OSDRenderer.h (the DX11 version). Replaces the
* legacy iZ3D Font library path (which has been dead at runtime for a
* long time: OSDFont.fnt was shipped by the original InnoSetup installer
* into %ALLUSERSPROFILE%\Font\, which the current zip-release pipeline
* doesn't populate -- CFont::Init returns silently on fopen failure).
*
* Rendering: GDI DrawTextW with ClearType into a 32-bit BGRA DIB section
* (shared pattern with the DX11 renderer), uploaded to a DYNAMIC
* IDirect3DTexture9 (D3DPOOL_DEFAULT) and drawn as pre-transformed
* XYZRHW quads with D3DFVF_XYZRHW | D3DFVF_TEX1.
*
* The content layer (status / FPS / hotkey help text, snapshot-change
* detection, fade timing) is identical to the DX11 version so the user
* sees the same look across DX9 and DX11 StereoInjector paths.
*
* Three overlay elements:
*   - Status line (top-left): pops for 2s on any stereo-value change.
*   - FPS counter (top-right): visible when gInfo.Input.ShowFPS=1.
*   - Hotkey help (centred): visible when gInfo.Input.ShowHotKeyOSD=1.
*
* Inter font loading (Fonts/Inter-*.ttf) mirrors the DX11 renderer and
* falls back to Arial when the TTFs aren't deployed.
*/
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>

namespace wiz3d
{

class OSDRenderer9
{
public:
    OSDRenderer9();
    ~OSDRenderer9();

    bool Init(IDirect3DDevice9* device);
    void ReleaseAll();
    // Deferred to a device-dispose + reinit after a Reset(); textures in
    // D3DPOOL_DEFAULT must be re-created.
    void OnDeviceLost();

    // originX/originY shift every overlay element by that many pixels in
    // the destination render target. bbW/bbH describe the per-eye drawable
    // extent (not the full SBS wide RT). Non-wide callers pass origin=0,
    // bbW/bbH = device backbuffer; wide-SBS callers call Render twice, once
    // per eye-view-rect, with origin = eyeRect.{left,top} and bbW/bbH =
    // eyeRect.{right - left, bottom - top}. XYZRHW vertices bypass the
    // vertex-shader / viewport, so the overlay has to add origin itself.
    void Render(IDirect3DDevice9* device, UINT bbW, UINT bbH,
                int originX = 0, int originY = 0);

    void NoteSettingChanged();
    void TickFrame();

private:
    enum class Slot { Status = 0, Fps, HotkeyHelp, Hunter, SaveToast, _Count };

    struct GDIFont {
        HFONT font     = nullptr;
        int   heightPx = 0;
        void  Destroy();
    };

    struct TexSlot {
        int                  w = 0, h = 0;
        HDC                  dc = nullptr;
        HBITMAP              dib = nullptr;
        HBITMAP              prev = nullptr;
        BYTE*                bits = nullptr;
        IDirect3DTexture9*   tex = nullptr;
        bool                 dirty = false;

        bool  Create(IDirect3DDevice9* dev, int width, int height);
        void  Destroy();
        void  Clear();
        void  DrawRect(const RECT& r, COLORREF rgb, BYTE alpha);
        void  DrawTextLine(const wchar_t* text, const RECT& r, HFONT font,
                           COLORREF rgb, UINT flags);
        void  Upload();
    };

    bool  LoadInterFontPrivate();
    void  UnloadInterFontPrivate();
    void  BuildStatusText(wchar_t* out, size_t cap) const;
    void  BuildFpsText(wchar_t* out, size_t cap) const;
    void  BuildHunterText(wchar_t* line1, size_t cap1,
                          wchar_t* line2, size_t cap2) const;
    void  BuildSaveToastText(wchar_t* line1, size_t cap1,
                             wchar_t* line2, size_t cap2) const;
    void  DrawQuad(IDirect3DDevice9* device, IDirect3DTexture9* tex,
                   float x, float y, float w, float h);
    bool  WantStatus() const;
    bool  WantFps() const;
    bool  WantHotkeyHelp() const;
    bool  WantHunter() const;
    bool  WantSaveToast() const;

    GDIFont m_fontStatus;
    GDIFont m_fontFps;
    GDIFont m_fontHelp;
    GDIFont m_fontHelpBody;
    const wchar_t* m_fontFace = L"Arial";
    // 0.4.0+: Inter TTFs embedded as RT_RCDATA in S3DWrapperD3D9.dll;
    // AddFontMemResourceEx returns opaque handles for RemoveFontMemResourceEx.
    HANDLE  m_interMemHandles[3] = {};
    int     m_interLoadedCount = 0;

    TexSlot m_slots[(int)Slot::_Count];

    ULONGLONG m_lastFpsSample = 0;
    unsigned  m_frameCountSinceSample = 0;
    float     m_fpsEMA = 0.f;
    ULONGLONG m_statusFadeDeadline = 0;

    unsigned  m_lastActivePreset = (unsigned)-1;
    float     m_lastStereoBase   = -1.f;
    float     m_lastOneDivZPS    = -1.f;
    bool      m_lastSwapEyes     = false;

    wchar_t   m_lastStatus[128] = {};
    wchar_t   m_lastFps[32]     = {};
    wchar_t   m_lastHunter1[96]  = {};
    wchar_t   m_lastHunter2[128] = {};
    wchar_t   m_lastToast1[32]   = {};
    wchar_t   m_lastToast2[160]  = {};
    bool      m_helpTextRendered = false;

    bool              m_initDone   = false;
    bool              m_initFailed = false;
    IDirect3DDevice9* m_dev        = nullptr;   // not AddRef'd
};

}  // namespace wiz3d
