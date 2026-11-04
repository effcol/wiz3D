/* wiz3D - IDXGISwapChain / IDXGISwapChain1 proxy for DX10 (Option B Stage 4d-equivalent)
 *
 * Mirror of SwapChain11Proxy but holding a Device10Proxy* parent and using
 * ID3D10* composite resources. The IDXGISwapChain interface itself is the
 * same for DX10 and DX11 (it's all DXGI), so the wrapper structure copies
 * directly; only the BB siblings, composite shader pipeline, and the parent
 * device differ.
 *
 * The IDXGISwapChain1 surface was originally dropped for DX10, but
 * CreateSwapChainForHwnd (DXGI 1.2+) returns IDXGISwapChain1 directly even
 * on DX10 devices, so the Factory2 swap-chain hook needs to wrap and return
 * an IDXGISwapChain1. Methods on the IDXGISwapChain1 surface return
 * E_NOINTERFACE when m_real1 is null.
 */

#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9types.h>   // D3DCOLORVALUE for the DXGI_RGBA shim below
#ifndef _DXGI_RGBA_DEFINED
#define _DXGI_RGBA_DEFINED
typedef D3DCOLORVALUE DXGI_RGBA;
#endif
#include <d3d10.h>
#include <dxgi.h>
#include <dxgi1_2.h>

namespace wiz3d
{

class Device10Proxy;
class Texture2D10Proxy;

class SwapChain10Proxy : public IDXGISwapChain1
{
public:
    // real    : the underlying IDXGISwapChain from the real DXGI factory.
    // parent  : the Device10Proxy this swap chain belongs to (AddRef'd).
    // real1   : optional QI'd IDXGISwapChain1 (Win8+ / Factory2 path).
    //           Nullable; methods on the IDXGISwapChain1 surface return
    //           E_NOINTERFACE when this is null.
    SwapChain10Proxy(IDXGISwapChain* real, Device10Proxy* parent);
    SwapChain10Proxy(IDXGISwapChain* real, IDXGISwapChain1* real1, Device10Proxy* parent);
    virtual ~SwapChain10Proxy();

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

    IDXGISwapChain* GetReal()    const { return m_real;   }
    Device10Proxy*  GetParent()  const { return m_parent; }

    // DX10 parity with SwapChain11Proxy::EmulateFullscreen. Enabled when the
    // Factory wrap path creates the chain windowed though the game asked for
    // fullscreen (DisableFullscreenModeEmulation=0, or legacy shutter output).
    // The chain then fakes fullscreen state back to the game via GetDesc /
    // GetFullscreenState while running windowed underneath -- avoids the
    // Crysis-Warhead-class alt-tab + fullscreen-exclusive-toggle thrash.
    void            EmulateFullscreen() { m_emulateFullscreen = true; m_gameFullscreen = TRUE; }

private:
    void    OnPresentBoundaryPre();
    void    OnPresentBoundaryPost();

    HRESULT EnsureStereoBackBuffer();
    HRESULT EnsureComposite();
    void    ReleaseStereoBackBuffer();
    void    ReleaseComposite();
    void    DoComposite();

    IDXGISwapChain*  m_real;
    IDXGISwapChain1* m_real1;    // optional, owned, nullable
    Device10Proxy*   m_parent;
    LONG             m_refs;

    ID3D10Texture2D*          m_leftBB;
    ID3D10Texture2D*          m_rightBB;
    Texture2D10Proxy*         m_wrappedBB;
    ID3D10ShaderResourceView* m_leftSRV;
    ID3D10ShaderResourceView* m_rightSRV;
    ID3D10RenderTargetView*   m_realBBRTV;
    UINT                      m_bbWidth;
    UINT                      m_bbHeight;
    DXGI_FORMAT               m_bbFormat;

    ID3D10VertexShader*       m_compositeVS;
    ID3D10PixelShader*        m_compositePS;
    ID3D10SamplerState*       m_compositeSampler;
    ID3D10RasterizerState*    m_compositeRaster;
    ID3D10BlendState*         m_compositeBlend;
    ID3D10DepthStencilState*  m_compositeDepthStencil;

    // Fullscreen-emulation state (DX11-parity). m_emulateFullscreen is set
    // once by wiz3D_EmulateFullscreen10 at swap-chain wrap time; m_gameFullscreen
    // tracks the fullscreen state the game last asked for, so Get*Desc/
    // GetFullscreenState can report that to the game even though the real
    // chain stays windowed.
    bool                      m_emulateFullscreen = false;
    BOOL                      m_gameFullscreen    = FALSE;
};

} // namespace wiz3d
