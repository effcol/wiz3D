/* NvDirectMode - IDXGISwapChain / IDXGISwapChain1 proxy
 *
 * Stage 1b-i: wraps IDXGISwapChain (back-buffer doubling lands in 1b-iii,
 * BB-pointer tracking that 1b-iv's OMSetRenderTargets routing needs hangs
 * off this proxy).
 *
 * Stage 2 Half 1: extended to IDXGISwapChain1 so the dxgi.dll factory
 * wrap can return a typed wrapper for `IDXGIFactory2::CreateSwapChainForHwnd`
 * et al. The IDXGISwapChain1-level methods passthrough to the real
 * IDXGISwapChain1 obtained via QI at construction time; if the real swap
 * chain doesn't expose IDXGISwapChain1 (Win7 without platform update), our
 * QI returns E_NOINTERFACE for that level so the game falls back to the
 * IDXGISwapChain interface (which we always wrap).
 *
 * Stage 3 shadow-RT: the real swap-chain BB now stays at the game's
 * requested (one-eye) size — no more desc-doubling. The doubled per-eye
 * rendering surface is allocated as a side ID3D11Texture2D ("shadow BB")
 * the first time the game calls GetBuffer(0), and is the texture we hand
 * back to the game. Game's draws land in our shadow texture; at Present
 * time we CopySubresourceRegion the active eye's half into the real BB
 * and forward to the real Present. This fixes:
 *   - GetDesc returning a doubled size (game was placing UI off-screen)
 *   - Fullscreen mode failing because the doubled BackBufferWidth wasn't
 *     a valid display mode
 *   - The whole-game-squashed-into-half-window appearance in non-stereo
 *     preview (BB is now native size; we display ONE eye in mono)
 */

#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <d3d11.h>

namespace SimulatedReality
{
    class SRInterfaceDX11;  // forward declaration to avoid including SR.hpp in the header
}

namespace NvDirectMode
{

class Device11Proxy;

class SwapChainProxy : public IDXGISwapChain1
{
public:
    explicit SwapChainProxy(IDXGISwapChain* real, Device11Proxy* parent);
    virtual ~SwapChainProxy();

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObj) override;
    ULONG   STDMETHODCALLTYPE AddRef() override                                  { return InterlockedIncrement(&m_refs); }
    ULONG   STDMETHODCALLTYPE Release() override
    {
        LONG r = InterlockedDecrement(&m_refs);
        if (r == 0)
        {
            if (m_real1) { m_real1->Release(); m_real1 = nullptr; }
            if (m_real)  { m_real->Release();  m_real  = nullptr; }
            delete this;
        }
        return (ULONG)r;
    }

    // IDXGIObject
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID Name, UINT DataSize, const void* pData) override        { return m_real->SetPrivateData(Name, DataSize, pData); }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID Name, const IUnknown* pUnknown) override       { return m_real->SetPrivateDataInterface(Name, pUnknown); }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID Name, UINT* pDataSize, void* pData) override            { return m_real->GetPrivateData(Name, pDataSize, pData); }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void** ppParent) override                               { return m_real->GetParent(riid, ppParent); }

    // IDXGIDeviceSubObject
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void** ppDevice) override                               { return m_real->GetDevice(riid, ppDevice); }

    // IDXGISwapChain
    HRESULT STDMETHODCALLTYPE Present(UINT SyncInterval, UINT Flags) override;
    HRESULT STDMETHODCALLTYPE GetBuffer(UINT Buffer, REFIID riid, void** ppSurface) override;
    HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL Fullscreen, IDXGIOutput* pTarget) override             { return m_real->SetFullscreenState(Fullscreen, pTarget); }
    HRESULT STDMETHODCALLTYPE GetFullscreenState(BOOL* pFullscreen, IDXGIOutput** ppTarget) override         { return m_real->GetFullscreenState(pFullscreen, ppTarget); }
    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC* pDesc) override                                  { return m_real->GetDesc(pDesc); }
    HRESULT STDMETHODCALLTYPE ResizeBuffers(
        UINT BufferCount, UINT Width, UINT Height,
        DXGI_FORMAT NewFormat, UINT SwapChainFlags) override;
    HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC* pNewTargetParameters) override              { return m_real->ResizeTarget(pNewTargetParameters); }
    HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput** ppOutput) override                           { return m_real->GetContainingOutput(ppOutput); }
    HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS* pStats) override                     { return m_real->GetFrameStatistics(pStats); }
    HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT* pLastPresentCount) override                          { return m_real->GetLastPresentCount(pLastPresentCount); }

    // IDXGISwapChain1
    HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_SWAP_CHAIN_DESC1* pDesc) override                                { return m_real1 ? m_real1->GetDesc1(pDesc) : E_NOINTERFACE; }
    HRESULT STDMETHODCALLTYPE GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pDesc) override             { return m_real1 ? m_real1->GetFullscreenDesc(pDesc) : E_NOINTERFACE; }
    HRESULT STDMETHODCALLTYPE GetHwnd(HWND* pHwnd) override                                                  { return m_real1 ? m_real1->GetHwnd(pHwnd) : E_NOINTERFACE; }
    HRESULT STDMETHODCALLTYPE GetCoreWindow(REFIID riid, void** ppUnk) override                              { return m_real1 ? m_real1->GetCoreWindow(riid, ppUnk) : E_NOINTERFACE; }
    HRESULT STDMETHODCALLTYPE Present1(UINT SyncInterval, UINT Flags,
                                       const DXGI_PRESENT_PARAMETERS* pPresentParameters) override;
    BOOL    STDMETHODCALLTYPE IsTemporaryMonoSupported() override                                            { return m_real1 ? m_real1->IsTemporaryMonoSupported() : FALSE; }
    HRESULT STDMETHODCALLTYPE GetRestrictToOutput(IDXGIOutput** ppRestrictToOutput) override                 { return m_real1 ? m_real1->GetRestrictToOutput(ppRestrictToOutput) : E_NOINTERFACE; }
    HRESULT STDMETHODCALLTYPE SetBackgroundColor(const DXGI_RGBA* pColor) override                           { return m_real1 ? m_real1->SetBackgroundColor(pColor) : E_NOINTERFACE; }
    HRESULT STDMETHODCALLTYPE GetBackgroundColor(DXGI_RGBA* pColor) override                                 { return m_real1 ? m_real1->GetBackgroundColor(pColor) : E_NOINTERFACE; }
    HRESULT STDMETHODCALLTYPE SetRotation(DXGI_MODE_ROTATION Rotation) override                              { return m_real1 ? m_real1->SetRotation(Rotation) : E_NOINTERFACE; }
    HRESULT STDMETHODCALLTYPE GetRotation(DXGI_MODE_ROTATION* pRotation) override                            { return m_real1 ? m_real1->GetRotation(pRotation) : E_NOINTERFACE; }

    // Accessors for 1b-iii / 1b-iv
    IDXGISwapChain* GetReal() const { return m_real; }
    Device11Proxy*  GetParent() const { return m_parent; }

    // Stage 4: invoked by the eye-change handler (registered with
    // NvApiProxy). Captures the *current* shadow content into the
    // texture for `eyeBeingLeft` (the eye whose render just finished).
    // Public so the static callback dispatcher can call into us.
    void CaptureEye(int eyeBeingLeft);

    // Magic-header SBS capture — Context11Proxy::CopyResource calls this
    // when it detects a (2W × H+1) source tagged with NVSTEREO_IMAGE_SIGNATURE.
    // Allocates / resizes m_leftEyeFrame and m_rightEyeFrame to (eyeWidth ×
    // eyeHeight) and splits the source via CopySubresourceRegion. Composite /
    // SR-weave then reads from the eye textures normally.
    void CaptureMagicHeaderSBS(ID3D11DeviceContext* pContext,
                               ID3D11Resource* pSrc,
                               UINT eyeWidth, UINT eyeHeight, bool swapEyes);

    bool IsMagicHeaderActive() const { return m_magicHeaderActive; }
    static SwapChainProxy* GetPrimary();

private:
    void EnsureShadowBB();
    void EnsureEyeFrames();
    void ReleaseShadowBB();
    void ReleaseEyeFrames();

    // Called from Present / Present1 — captures current shadow as the
    // latest-rendered eye, then copies the configured display eye(s) to
    // the real backbuffer.
    void CaptureAndPresentBlit();

    IDXGISwapChain*  m_real;
    IDXGISwapChain1* m_real1;   // null if real doesn't expose IDXGISwapChain1
    Device11Proxy*   m_parent;  // not AddRef'd; device outlives swap chain
    LONG             m_refs;

    // Stage 3 shadow-RT state. m_shadowBB is the (1x logical) per-frame
    // RT we hand the game in place of the real BB. Allocated on first
    // GetBuffer(0); released in dtor / ResizeBuffers.
    ID3D11Texture2D* m_shadowBB;
    UINT             m_logicalW;     // one-eye width  (= real BB width)
    UINT             m_logicalH;     // one-eye height (= real BB height)
    DXGI_FORMAT      m_shadowFormat;

    // Stage 4 per-eye capture textures. Lazily allocated on first eye
    // change. m_lastSeenEye is the eye whose render is currently in the
    // shadow (so Present knows which slot to capture into).
    ID3D11Texture2D* m_leftEyeFrame;
    ID3D11Texture2D* m_rightEyeFrame;
    int              m_lastSeenEye;  // 1=R 2=L 3=MONO; default MONO

    // Magic-header SBS state: when set, eye textures are being populated
    // from CopyResource intercepts instead of CaptureEye/SetActiveEye.
    bool             m_magicHeaderActive;

    // Stage 4b composite pipeline. Lazy-compiled the first time both
    // eye frames have content; reused thereafter. Released in dtor /
    // ResizeBuffers (RTV is invalidated when the real BB changes).
    ID3D11VertexShader*       m_compositeVS;
    ID3D11PixelShader*        m_compositePS_SBS;
    ID3D11PixelShader*        m_compositePS_TB;
    // OutputMode 4-7 — line/column interleaved, checkerboard, anaglyph.
    // Same VS as SBS/TB; shaders sample left/right SRVs (t0/t1) and
    // pick per-pixel based on screen-space pixel coordinates.
    ID3D11PixelShader*        m_compositePS_Line;
    ID3D11PixelShader*        m_compositePS_Col;
    ID3D11PixelShader*        m_compositePS_Checker;
    ID3D11PixelShader*        m_compositePS_Anaglyph;
    ID3D11Buffer*             m_anaglyphCB;     // 6 float4 rows of colour-matrix coefficients (slot b2)
    ID3D11SamplerState*       m_compositeSampler;       // linear (SBS / TB / anaglyph)
    ID3D11SamplerState*       m_compositeSamplerPoint;  // point   (interleaved / checker — avoids row blending)
    ID3D11RasterizerState*    m_compositeRS;
    ID3D11BlendState*         m_compositeBlend;
    ID3D11DepthStencilState*  m_compositeDSS;
    ID3D11ShaderResourceView* m_leftEyeSRV;     // view onto m_leftEyeFrame
    ID3D11ShaderResourceView* m_rightEyeSRV;    // view onto m_rightEyeFrame
    ID3D11RenderTargetView*   m_realBBRTV;      // RTV for the real DXGI back buffer

    bool EnsureCompositeShaders();
    void ReleaseCompositePipeline();   // shaders+states+SRVs+RTV
    bool RunCompositePass();           // returns true if composite ran (and replaces single-eye blit)
    void UpdateAnaglyphCB();           // upload current AnaglyphColour x AnaglyphMethod matrix pair to b2

    // OutputMode 8 — Simulated Reality weave (Leia / Samsung Odyssey ML displays).
    // SR runtime DLLs are delay-loaded (vcxproj's DelayLoadDLLs); first call to
    // EnsureSRWeaver attempts CreateSRInterfaceDX11 from SR-Lib. If the runtime
    // isn't installed or no SR display is connected, the HRESULT fails and the
    // wrap downgrades OutputMode to SBS for the remainder of the session.
    //
    // Pipeline: composite SBS-shader writes to m_srSBSTex (2W × H intermediate),
    // SetInputTexture(m_srSBSSRV) binds once on texture creation, Weave() writes
    // the weaved frame to the currently-bound RTV (we bind m_realBBRTV first).
    bool EnsureSRWeaver();
    bool EnsureSRSBSTexture();
    void ReleaseSRPipeline();
    bool RunSRWeave();
    // Sticky flag — set if SR runtime missing OR interface create failed OR
    // the game is on the SR-incompatible blacklist. Once set the rest of the
    // session falls back to SBS without re-trying.
    bool                               m_srBlacklistedOrFailed;
    SimulatedReality::SRInterfaceDX11* m_srInterfaceDX11;  
                                                   
    ID3D11Texture2D*          m_srSBSTex;
    ID3D11RenderTargetView*   m_srSBSRTV;
    ID3D11ShaderResourceView* m_srSBSSRV;
    UINT                      m_srSBSW;            // == m_logicalW * 2
    UINT                      m_srSBSH;            // == m_logicalH
    DXGI_FORMAT               m_srSBSFmt;
};

} // namespace NvDirectMode
