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

namespace wiz3d
{

class Device11Proxy;
class Texture2D11Proxy;

// Derives from IDXGISwapChain4 so QI for SwapChain2/3/4 can return `this`.
// Handing back the real chain let Win11-era games (GTA V) Present and
// GetBuffer behind our back, mixing real and wrapped back buffers.
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
    HRESULT STDMETHODCALLTYPE GetFullscreenState(BOOL* pFullscreen, IDXGIOutput** ppTarget) override           { return m_real->GetFullscreenState(pFullscreen, ppTarget); }
    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC* pDesc) override                                    { return m_real->GetDesc(pDesc); }
    HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags) override;
    HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC* pNewTargetParameters) override                { return m_real->ResizeTarget(pNewTargetParameters); }
    HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput** ppOutput) override                             { return m_real->GetContainingOutput(ppOutput); }
    HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS* pStats) override                       { return m_real->GetFrameStatistics(pStats); }
    HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT* pLastPresentCount) override                            { return m_real->GetLastPresentCount(pLastPresentCount); }

    // IDXGISwapChain1
    HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_SWAP_CHAIN_DESC1* pDesc) override                                  { return m_real1 ? m_real1->GetDesc1(pDesc) : E_NOINTERFACE; }
    HRESULT STDMETHODCALLTYPE GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pDesc) override               { return m_real1 ? m_real1->GetFullscreenDesc(pDesc) : E_NOINTERFACE; }
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

private:
    // Stage 4b.8: Present / Present1 split into pre + post hooks. Pre runs
    // the right-eye replay over frame N's recorded command stream so both
    // eyes' textures are populated before the real Present flips the BB.
    // Post clears the recording vector and re-arms the hook for frame N+1.
    void OnPresentBoundaryPre();
    void OnPresentBoundaryPost();

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

    IDXGISwapChain*  m_real;     // owned (released in dtor)
    IDXGISwapChain1* m_real1;    // optional, owned, nullable
    // QI'd in ctor from m_real; null on older runtimes, in which case the
    // matching interface methods return E_NOINTERFACE.
    IDXGISwapChain2* m_real2;
    IDXGISwapChain3* m_real3;
    IDXGISwapChain4* m_real4;
    Device11Proxy*   m_parent;   // AddRef'd in ctor, released in dtor
    LONG             m_refs;

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
    UINT                      m_bbWidth;
    UINT                      m_bbHeight;
    DXGI_FORMAT               m_bbFormat;

    // Composite pipeline state. Once compiled they're reusable across
    // frames; cleared in dtor + ResizeBuffers (the BB format/size may
    // change, but the shaders themselves are independent of those).
    ID3D11VertexShader*       m_compositeVS;
    ID3D11PixelShader*        m_compositePS;
    ID3D11SamplerState*       m_compositeSampler;
    ID3D11RasterizerState*    m_compositeRaster;
    ID3D11BlendState*         m_compositeBlend;
    ID3D11DepthStencilState*  m_compositeDepthStencil;
};

} // namespace wiz3d
