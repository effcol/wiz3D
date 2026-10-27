/* wiz3D - IDXGISwapChain / IDXGISwapChain1 proxy (Option B Stage 4b.2)
 *
 * Lightweight COM-vtable wrap of IDXGISwapChain (+ IDXGISwapChain1 via QI).
 * Most methods passthrough. The Present and Present1 entries fire a frame-
 * end callback on the parent Device11Proxy's immediate Context11Proxy so the
 * recording machinery has a flush + replay trigger.
 *
 * Stage 4b.2 ships the class + IID; the factory hook that actually produces
 * the proxies at CreateSwapChain time lands in 4b.3.
 *
 * Minimal vs the existing DXGISwapChainWrapper (ATL-based, for the legacy
 * DDI path) and NvDirectMode/d3d11/SwapChainProxy (1300+ lines with shadow-
 * RT + magic-header capture). The Option B path doesn't need either — we
 * keep the real BB at native size and SBS-composite into it at Present time
 * (Stage 4d).
 */

#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9types.h>   // D3DCOLORVALUE for the DXGI_RGBA shim below
#ifndef _DXGI_RGBA_DEFINED
#define _DXGI_RGBA_DEFINED
typedef D3DCOLORVALUE DXGI_RGBA;     // see DXGIDeviceProxy.h — bundled
                                     // lib/d3d10 dxgitype.h shadows the
                                     // SDK header and is missing this.
#endif
#include <dxgi.h>
#include <dxgi1_2.h>
#include <dxgi1_5.h>   // IDXGISwapChain2/3/4
#include "ShiftFinder11.h"           // 0.4.1 AutoFocus / LaserSight image correlator
#include "..\Shared\KatangaProducer.h" // 0.5.0 Katanga shared-texture producer
#include "..\Shared\NV3DOutput.h"      // 0.5.0 NV3D-Lib shutter-glass output

// SR-Lib (ThirdPartyLibs/SR-Lib_v2.0.0): lightfield weave for Simulated
// Reality displays. Forward-declared so only the .cpp includes SR.hpp.
namespace SimulatedReality { class SRInterfaceDX11; }
struct ID3DDeviceContextState;   // d3d11_1.h, included by the .cpp

namespace wiz3d
{

class Device11Proxy;
class Texture2D11Proxy;
class OSDRenderer;
class LaserSightRenderer;

// IDXGISwapChain4, so QI for SwapChain2/3/4 returns `this`: a game handed the real chain would Present and GetBuffer
// past the proxy, mixing real and wrapped back buffers.
class SwapChain11Proxy : public IDXGISwapChain4
{
public:
    // real    : the underlying IDXGISwapChain from the real DXGI factory.
    // parent  : the Device11Proxy this swap chain belongs to (not AddRef'd
    //           by us — parent always outlives the swap chain because
    //           Release-cascade from game holds it alive).
    // real1   : optional QI'd IDXGISwapChain1 (Win8+). Nullable; methods on
    //           the IDXGISwapChain1 surface return E_NOINTERFACE when this
    //           is null.
    SwapChain11Proxy(IDXGISwapChain* real, IDXGISwapChain1* real1, Device11Proxy* parent);
    virtual ~SwapChain11Proxy();

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObj) override;
    ULONG   STDMETHODCALLTYPE AddRef() override                                  { return InterlockedIncrement(&m_refs); }
    ULONG   STDMETHODCALLTYPE Release() override;

    // IDXGIObject
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID Name, UINT DataSize, const void* pData) override          { return m_real->SetPrivateData(Name, DataSize, pData); }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID Name, const IUnknown* pUnk) override             { return m_real->SetPrivateDataInterface(Name, pUnk); }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID Name, UINT* pDataSize, void* pData) override              { return m_real->GetPrivateData(Name, pDataSize, pData); }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void** ppParent) override                                 { return m_real->GetParent(riid, ppParent); }

    // IDXGIDeviceSubObject
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void** ppDevice) override;

    // IDXGISwapChain
    HRESULT STDMETHODCALLTYPE Present(UINT SyncInterval, UINT Flags) override;
    HRESULT STDMETHODCALLTYPE GetBuffer(UINT Buffer, REFIID riid, void** ppSurface) override;
    HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL Fullscreen, IDXGIOutput* pTarget) override;
    HRESULT STDMETHODCALLTYPE GetFullscreenState(BOOL* pFullscreen, IDXGIOutput** ppTarget) override;
    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC* pDesc) override;
    HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags) override;
    HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC* pNewTargetParameters) override                { return m_real->ResizeTarget(pNewTargetParameters); }
    HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput** ppOutput) override                             { return m_real->GetContainingOutput(ppOutput); }
    HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS* pStats) override                       { return m_real->GetFrameStatistics(pStats); }
    HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT* pLastPresentCount) override                            { return m_real->GetLastPresentCount(pLastPresentCount); }

    // IDXGISwapChain1
    HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_SWAP_CHAIN_DESC1* pDesc) override;
    HRESULT STDMETHODCALLTYPE GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pDesc) override;
    HRESULT STDMETHODCALLTYPE GetHwnd(HWND* pHwnd) override                                                    { return m_real1 ? m_real1->GetHwnd(pHwnd) : E_NOINTERFACE; }
    HRESULT STDMETHODCALLTYPE GetCoreWindow(REFIID refiid, void** ppUnk) override                              { return m_real1 ? m_real1->GetCoreWindow(refiid, ppUnk) : E_NOINTERFACE; }
    HRESULT STDMETHODCALLTYPE Present1(UINT SyncInterval, UINT PresentFlags, const DXGI_PRESENT_PARAMETERS* pPresentParameters) override;
    BOOL    STDMETHODCALLTYPE IsTemporaryMonoSupported() override                                              { return m_real1 ? m_real1->IsTemporaryMonoSupported() : FALSE; }
    HRESULT STDMETHODCALLTYPE GetRestrictToOutput(IDXGIOutput** ppRestrictToOutput) override                   { return m_real1 ? m_real1->GetRestrictToOutput(ppRestrictToOutput) : E_NOINTERFACE; }
    HRESULT STDMETHODCALLTYPE SetBackgroundColor(const DXGI_RGBA* pColor) override                             { return m_real1 ? m_real1->SetBackgroundColor(pColor) : E_NOINTERFACE; }
    HRESULT STDMETHODCALLTYPE GetBackgroundColor(DXGI_RGBA* pColor) override                                   { return m_real1 ? m_real1->GetBackgroundColor(pColor) : E_NOINTERFACE; }
    HRESULT STDMETHODCALLTYPE SetRotation(DXGI_MODE_ROTATION Rotation) override                                { return m_real1 ? m_real1->SetRotation(Rotation) : E_NOINTERFACE; }
    HRESULT STDMETHODCALLTYPE GetRotation(DXGI_MODE_ROTATION* pRotation) override                              { return m_real1 ? m_real1->GetRotation(pRotation) : E_NOINTERFACE; }

    // IDXGISwapChain2 — nullable real2, same convention as m_real1 above.
    HRESULT STDMETHODCALLTYPE SetSourceSize(UINT Width, UINT Height) override                                 { return m_real2 ? m_real2->SetSourceSize(Width, Height) : E_NOINTERFACE; }
    HRESULT STDMETHODCALLTYPE GetSourceSize(UINT* pWidth, UINT* pHeight) override                             { return m_real2 ? m_real2->GetSourceSize(pWidth, pHeight) : E_NOINTERFACE; }
    HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT MaxLatency) override                                { return m_real2 ? m_real2->SetMaximumFrameLatency(MaxLatency) : E_NOINTERFACE; }
    HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT* pMaxLatency) override                              { return m_real2 ? m_real2->GetMaximumFrameLatency(pMaxLatency) : E_NOINTERFACE; }
    HANDLE  STDMETHODCALLTYPE GetFrameLatencyWaitableObject() override                                        { return m_real2 ? m_real2->GetFrameLatencyWaitableObject() : nullptr; }
    HRESULT STDMETHODCALLTYPE SetMatrixTransform(const DXGI_MATRIX_3X2_F* pMatrix) override                   { return m_real2 ? m_real2->SetMatrixTransform(pMatrix) : E_NOINTERFACE; }
    HRESULT STDMETHODCALLTYPE GetMatrixTransform(DXGI_MATRIX_3X2_F* pMatrix) override                         { return m_real2 ? m_real2->GetMatrixTransform(pMatrix) : E_NOINTERFACE; }

    // IDXGISwapChain3
    UINT    STDMETHODCALLTYPE GetCurrentBackBufferIndex() override                                            { return m_real3 ? m_real3->GetCurrentBackBufferIndex() : 0; }
    HRESULT STDMETHODCALLTYPE CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE ColorSpace, UINT* pColorSpaceSupport) override { return m_real3 ? m_real3->CheckColorSpaceSupport(ColorSpace, pColorSpaceSupport) : E_NOINTERFACE; }
    HRESULT STDMETHODCALLTYPE SetColorSpace1(DXGI_COLOR_SPACE_TYPE ColorSpace) override                       { return m_real3 ? m_real3->SetColorSpace1(ColorSpace) : E_NOINTERFACE; }
    HRESULT STDMETHODCALLTYPE ResizeBuffers1(UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT Format, UINT SwapChainFlags, const UINT* pCreationNodeMask, IUnknown* const* ppPresentQueue) override;

    // IDXGISwapChain4
    HRESULT STDMETHODCALLTYPE SetHDRMetaData(DXGI_HDR_METADATA_TYPE Type, UINT Size, void* pMetaData) override { return m_real4 ? m_real4->SetHDRMetaData(Type, Size, pMetaData) : E_NOINTERFACE; }

    // wiz3D accessors
    IDXGISwapChain* GetReal()    const { return m_real;   }
    Device11Proxy*  GetParent()  const { return m_parent; }
    // DisableFullscreenModeEmulation=0 created this chain windowed though the game asked for fullscreen.
    void            EmulateFullscreen() { m_emulateFullscreen = true; m_gameFullscreen = TRUE; }
    // 0.4.1 AutoFocus / LaserSight: LaserSightRenderer pulls the measured
    // current-shift pixels from here to position the crosshair on the
    // feature AutoFocus has locked onto. Returns nullptr before the first
    // EnsureStereoBackBuffer completes.
    wiz3d::ShiftFinder11* GetShiftFinder() { return m_shiftFinder.IsInitializedSuccessfully() ? &m_shiftFinder : nullptr; }

private:
    // Stage 4b.8: Present / Present1 split into pre + post hooks. Pre runs
    // the right-eye replay over frame N's recorded command stream so both
    // eyes' textures are populated before the real Present flips the BB.
    // Post clears the recording vector and re-arms the hook for frame N+1.
    void OnPresentBoundaryPre();
    void OnPresentBoundaryPost();
    // Both Present entries; with CollectDebugInformation the output pass and the real call are timed for the frame report.
    HRESULT PresentFrame(bool present1, UINT SyncInterval, UINT Flags, const DXGI_PRESENT_PARAMETERS* pParams);
    // CollectDebugInformation: frames whose end the GPU has not reached yet, from a ring of event queries.
    unsigned GpuFramesBehind();
    static const unsigned kFrameEvents = 8;
    ID3D11Query* m_frameEvents[kFrameEvents] = {};
    unsigned     m_frameEventNext = 0;
    // Hotkey-changed stereo settings are written once they settle for 5 s, as the DX9 wrapper does at each Present.
    void PersistSettings();
    DataInput m_savedInput;
    ULONGLONG m_writeSettingsAt = 0;
    // The screenshot hotkey (or the autotests' MakeScreenshot): both eyes before the output pass, as DX9 writes them.
    void WriteScreenshot();

    // Stage 4d: stereo BB sibling + SBS composite. EnsureStereoBackBuffer
    // allocates wiz3D-side LEFT + RIGHT BB textures (matching real BB desc
    // + BIND_RENDER_TARGET + BIND_SHADER_RESOURCE) that the game's draws
    // (left) and the recorded replay (right) write into. EnsureComposite
    // lazily compiles the fullscreen-triangle VS/PS that samples L + R and
    // writes SBS-formatted output into the real BB. DoComposite runs that
    // pass at Pre-Present time, after the right-eye replay sweep.
    HRESULT EnsureStereoBackBuffer();
    HRESULT EnsureComposite();
    void    ReleaseStereoBackBuffer();
    void    ReleaseComposite();
    void    DoComposite();

    // -----------------------------------------------------------------
    // Output mode. The composite picks a pixel shader per mode and
    // routes to one of several destinations. Mode numbering matches
    // S3DWrapperOGL and NvDirectMode so the same wiz3D_Config.xml
    // <OutputMode Value="N"/> works across all our modern paths.
    //
    // Modes 1 and 2 share the SBS shader -- "Full SBS" is "Half SBS"
    // with the game rendering at half display width, same for TB.
    // Modes 5-8 are added by follow-up commits; the SR weave path
    // (mode 9) remains the existing SR-Lib integration.
    // -----------------------------------------------------------------
    enum OutputMode {
        OM_IZ3D_DUAL          = 0,  // iZ3D dual-panel (legacy); unsupported on DX11, falls back to Half SBS
        OM_HALF_SBS           = 1,  // Half Side-by-Side (default)
        OM_FULL_SBS           = 2,  // Full Side-by-Side -- game must render at half display width
        OM_HALF_TB            = 3,  // Half Top-and-Bottom
        OM_FULL_TB            = 4,  // Full Top-and-Bottom -- game must render at half display height
        OM_ANAGLYPH           = 5,  // Anaglyph Red/Cyan (Dubois) -- follow-up commit
        OM_ROW_INTERLEAVED   = 6,  // Row interleaved (passive 3D TVs) -- follow-up commit
        OM_COLUMN_INTERLEAVED = 7,  // Column interleaved -- follow-up commit
        OM_CHECKERBOARD       = 8,  // Checkerboard (DLP-Link projectors) -- follow-up commit
        OM_SR_WEAVE           = 9,  // Simulated Reality Weave (Leia/Samsung Odyssey 3D)
        OM_3DVISION_GLASSES   = 11, // 0.5.0: NV3D-Lib DIRECT-mode shutter glasses (Vision Restoration coop)
        OM_KATANGA            = 12, // 0.5.0: produce shared wide-SBS DX11 tex, Katanga app displays it
    };
    // Reads gInfo.OutputMode and the legacy OutputMethodDll string, returning
    // the enum. OutputMethodDll=="SimulatedRealityWeaveOutput" overrides to
    // OM_SR_WEAVE for backward compat with existing SR configs. Mode 0 and
    // out-of-range values fall back to OM_HALF_SBS.
    OutputMode SelectedOutputMode() const;

    // The composite pass by destination: the compositor output draws into
    // the real BB, the SR weave path into a 2WxH offscreen texture the
    // weaver reads. The chosen OutputMode picks which pixel shader runs.
    void    DrawCompositePass(ID3D11DeviceContext* ctx,
                              ID3D11RenderTargetView* rtv,
                              UINT width, UINT height,
                              OutputMode mode);
    // The output pass's own context state (D3D11.1), swapped in around it so the game's state comes back untouched.
    bool    EnsureCompositeState();
    ID3DDeviceContextState* m_compositeState = nullptr;
    bool                    m_compositeStateFailed = false;

    // Simulated Reality weave output (lightfield displays), chosen by the <OutputMethodDll> key the DX9 path uses: the
    // weaver reads the offscreen SBS and writes into the real BB; any failure falls back to plain SBS for good.
    bool    WeaveSelected() const;
    bool    EnsureSRWeaver();
    HRESULT EnsureSRSBSTexture();
    bool    RunSRWeave(ID3D11DeviceContext* ctx);
    void    ReleaseSRWeave(bool releaseWeaver);

    // Katanga output -- producer for bo3b's open-source stereo viewer. Produces a
    // 2W x H shared DX11 texture (D3D11_RESOURCE_MISC_SHARED legacy KMT) that
    // Katanga opens and displays. Falls back to Half-SBS on the real BB if
    // Katanga isn't running. See Shared/KatangaProducer.h for the protocol.
    HRESULT EnsureKatangaSBSTexture();
    bool    RunKatanga(ID3D11DeviceContext* ctx);
    void    ReleaseKatanga();

    // NV3D-Lib output -- hands the compositor's wide SBS source to NV3D::InterfaceDX11
    // which drives shutter glasses via its own D3D9Ex DIRECT-mode presenter.
    // Reuses m_katangaSBSTex for the SBS source: a session picks EITHER Katanga
    // OR NV3D-Lib, never both, so sharing the compositor target is safe.
    // See Shared/NV3DOutput.h for the Vision Restoration cooperation story.
    bool    RunNV3DGlasses(ID3D11DeviceContext* ctx);
    void    ReleaseNV3DGlasses();

    IDXGISwapChain*  m_real;     // owned (released in dtor)
    IDXGISwapChain1* m_real1;    // optional, owned, nullable
    // QI'd in ctor from m_real; null on older runtimes, in which case the
    // matching interface methods return E_NOINTERFACE.
    IDXGISwapChain2* m_real2;
    IDXGISwapChain3* m_real3;
    IDXGISwapChain4* m_real4;
    Device11Proxy*   m_parent;   // AddRef'd in ctor, released in dtor
    LONG             m_refs;
    // The game reads back the fullscreen state it last set: shown a windowed chain, Metro 2033 Redux rebuilt it every
    // second frame (issue #34).
    bool             m_emulateFullscreen = false;
    BOOL             m_gameFullscreen    = FALSE;

    // Stage 4d resources. All AddRef'd by us, released in dtor /
    // ReleaseStereoBackBuffer / ReleaseComposite. m_wrappedBB is the
    // single Texture2D11Proxy returned by GetBuffer(0) — its left/right
    // reals are m_leftBB / m_rightBB. m_realBBRTV is an RTV on the REAL
    // swap-chain back buffer (the composite destination).
    ID3D11Texture2D*          m_leftBB;
    ID3D11Texture2D*          m_rightBB;
    Texture2D11Proxy*         m_wrappedBB;
    ID3D11ShaderResourceView* m_leftSRV;
    ID3D11ShaderResourceView* m_rightSRV;
    ID3D11RenderTargetView*   m_realBBRTV;
    UINT                      m_bbWidth;      // Game-visible BB dims (what GetDesc returns to the game,
    UINT                      m_bbHeight;     // what m_leftBB/m_rightBB are sized to for game rendering).
    DXGI_FORMAT               m_bbFormat;
    // Real swap-chain BB dims. Equal to m_bbWidth/m_bbHeight except in
    // SR_Weave mode: there we resize the real BB to LeiaSR's recommended
    // panel-native dims so the weave output lands at lenslet pixel pitch
    // instead of being scaled downstream by DWM / display.
    UINT                      m_realBBW = 0;
    UINT                      m_realBBH = 0;

    // Composite pipeline state. Once compiled they're reusable across
    // frames; cleared in dtor + ResizeBuffers (the BB format/size may
    // change, but the shaders themselves are independent of those).
    // One PS per OutputMode group that uses the same sampling pattern:
    // SBS/TB are linear-sampled, Anaglyph adds a Dubois CB (follow-up),
    // Line/Col/Checker use point sampling (follow-up).
    ID3D11VertexShader*       m_compositeVS;
    ID3D11PixelShader*        m_compositePS_SBS              = nullptr;  // modes 1 (Half SBS) + 2 (Full SBS)
    ID3D11PixelShader*        m_compositePS_TB               = nullptr;  // modes 3 (Half TB)  + 4 (Full TB)
    ID3D11PixelShader*        m_compositePS_Anaglyph         = nullptr;  // mode 5 (Dubois Red/Cyan)
    ID3D11PixelShader*        m_compositePS_RowInterleaved  = nullptr;  // mode 6 (row-polarised displays)
    ID3D11PixelShader*        m_compositePS_ColInterleaved   = nullptr;  // mode 7 (column-polarised displays)
    ID3D11PixelShader*        m_compositePS_Checkerboard     = nullptr;  // mode 8 (DLP-Link projectors)
    ID3D11SamplerState*       m_compositeSampler;
    // Point sampler for the interleaved + checkerboard modes where
    // screen-space pixel parity selects the eye and any linear filtering
    // would smear eye boundaries.
    ID3D11SamplerState*       m_compositeSamplerPoint        = nullptr;
    ID3D11RasterizerState*    m_compositeRaster;
    ID3D11BlendState*         m_compositeBlend;
    ID3D11DepthStencilState*  m_compositeDepthStencil;

    // SR weave state: the weaver survives ResizeBuffers (it is bound to the context and window, not the BB); the SBS
    // input texture follows the BB's size and format.
    SimulatedReality::SRInterfaceDX11* m_srInterface = nullptr;
    bool                      m_srFailed  = false;
    ID3D11Texture2D*          m_srSBSTex  = nullptr;
    ID3D11RenderTargetView*   m_srSBSRTV  = nullptr;
    ID3D11ShaderResourceView* m_srSBSSRV  = nullptr;
    UINT                      m_srSBSW    = 0;
    UINT                      m_srSBSH    = 0;
    DXGI_FORMAT               m_srSBSFmt  = DXGI_FORMAT_UNKNOWN;

    // Katanga output state. m_Katanga owns the shared texture + Katanga-side
    // mapped file + setup mutex (see Shared/KatangaProducer). m_katangaSBS* is
    // our side's wide-SBS scratch that the compositor fills before we hand
    // the result to Katanga via CopyResource.
    wiz3d::KatangaProducer    m_Katanga;

    // NV3D-Lib output. Owns an NV3D::InterfaceDX11 (hidden D3D9Ex FSE
    // popup); SBS source is the Katanga one shared above.
    wiz3d::NV3DOutputDX11     m_NV3D;
    ID3D11Texture2D*          m_katangaSBSTex = nullptr;
    ID3D11RenderTargetView*   m_katangaSBSRTV = nullptr;
    UINT                      m_katangaSBSW   = 0;
    UINT                      m_katangaSBSH   = 0;
    DXGI_FORMAT               m_katangaSBSFmt = DXGI_FORMAT_UNKNOWN;

    // OSD overlay (ShowOSD / ShowFPS / ShowHotKeyOSD) — self-contained
    // GDI-based text renderer. Lazily initialised at first composite;
    // released in dtor. Independent of the stereo composite path, drawn
    // after it over the real BB.
    OSDRenderer*              m_osd = nullptr;

    // Laser sight overlay (Ctrl+Numpad / hotkey). Lazy-created on first
    // frame that has LaserSightEnable=1; released in dtor.
    LaserSightRenderer*       m_laser = nullptr;

    // 0.4.1 AutoFocus / LaserSight image correlator. Shared infra lives in
    // S3DAPI/ImageProcessing (worker thread + SSE converters); ShiftFinder11
    // only owns the DX11 staging texture and the per-eye copy boxes. Driven
    // from DoComposite once m_leftBB/m_rightBB are populated; its measured
    // shift feeds Device11Proxy::m_autoFocus::UpdateZPS and the laser-sight
    // crosshair position.
    wiz3d::ShiftFinder11      m_shiftFinder;
    UINT                      m_shiftFrame = 0;
    // Snapshot of the active preset's stereo state used to spot user-driven
    // changes (hotkey tap on separation / convergence / preset slot) and
    // invalidate the AutoFocus hold-shift so a fresh adjustment settles.
    unsigned                  m_sfLastPreset = 0xFFFFFFFFu;
    float                     m_sfLastSep    = -1.f;
    float                     m_sfLastConv   = 0.f;
};

} // namespace wiz3d
