/* wiz3D - ID3D11Texture2D proxy (Option B Stage 3a)
 *
 * Wraps an ID3D11Texture2D returned from Device11Proxy::CreateTexture2D.
 * At construction time, consults StereoHeuristic.h; if the texture meets
 * the doubling criteria the proxy calls the real device's CreateTexture2D
 * a second time with the same desc to allocate a right-eye sibling.
 * IsStereo() lets downstream code (the RTV/DSV/SRV/UAV proxies, and the
 * Stage-4 OMSet routing) tell which textures need per-eye handling.
 *
 * Mirrors S3DWrapper10/ResourceWrapper.cpp's CreateRightResource at the
 * COM layer. The Resource11Proxy / Stereo11Resource patterns from the
 * design doc collapse to a single concrete class here because the COM
 * surface is small enough not to need a template hierarchy.
 */

#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>

namespace wiz3d
{

class Device11Proxy;
bool ComputeOutputsPerEyeOn();   // FeatureComputeOutputsPerEye.cpp

class Texture2D11Proxy : public ID3D11Texture2D
{
public:
    // realLeft is the always-present main texture the game asked for; realRight
    // is the optional sibling created by the heuristic (nullable). Parent
    // Device11Proxy is used by GetDevice to preserve COM identity (return the
    // wrapped device, not the real one).
    Texture2D11Proxy(ID3D11Texture2D* realLeft, ID3D11Texture2D* realRight, Device11Proxy* parent);
    virtual ~Texture2D11Proxy();

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObj) override;
    ULONG   STDMETHODCALLTYPE AddRef() override                  { return InterlockedIncrement(&m_refs) - m_internalRefs; }
    ULONG   STDMETHODCALLTYPE Release() override;

    // ID3D11DeviceChild
    void    STDMETHODCALLTYPE GetDevice(ID3D11Device** ppDevice) override;
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT* pDataSize, void* pData) override                  { return m_realLeft->GetPrivateData(guid, pDataSize, pData); }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT DataSize, const void* pData) override              { return m_realLeft->SetPrivateData(guid, DataSize, pData); }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown* pData) override                { return m_realLeft->SetPrivateDataInterface(guid, pData); }

    // ID3D11Resource
    void    STDMETHODCALLTYPE GetType(D3D11_RESOURCE_DIMENSION* pResourceDimension) override                       { m_realLeft->GetType(pResourceDimension); }
    void    STDMETHODCALLTYPE SetEvictionPriority(UINT EvictionPriority) override                                  { m_realLeft->SetEvictionPriority(EvictionPriority); if (m_realRight) m_realRight->SetEvictionPriority(EvictionPriority); }
    UINT    STDMETHODCALLTYPE GetEvictionPriority() override                                                       { return m_realLeft->GetEvictionPriority(); }

    // ID3D11Texture2D
    void    STDMETHODCALLTYPE GetDesc(D3D11_TEXTURE2D_DESC* pDesc) override
    { m_realLeft->GetDesc(pDesc); if (pDesc && m_viewFormat != DXGI_FORMAT_UNKNOWN) pDesc->Format = m_viewFormat; }

    // <TypelessBackBuffer>: the swap chain's format a typeless stand-in back buffer reports, and its views default to.
    void             SetViewFormat(DXGI_FORMAT f) { m_viewFormat = f; }
    DXGI_FORMAT      ViewFormat() const           { return m_viewFormat; }

    // wiz3D accessors used by View11 proxies and Stage 4's per-eye routing.
    bool             IsStereo()      const { return m_realRight != nullptr; }
    ID3D11Texture2D* GetReal()       const { return m_realLeft;  }
    ID3D11Texture2D* GetRealRight()  const { return m_realRight; }
    Device11Proxy*   GetParent()     const { return m_parent;    }
    // The swap chain's presented pair: draws into it without a depth test are overlays (HUD, menus).
    void             MarkBackBuffer()      { m_backBuffer = true; }
    bool             IsBackBuffer()  const { return m_backBuffer; }
    // Our own holds (the swap chain's, views'): internal on the back buffer under PrivateBackBufferRefs, kept out of what
    // AddRef/Release return as D3D11's are; otherwise plain AddRef/Release. MakeRefInternal turns a held one internal.
    void             AddRefInternal();
    void             ReleaseInternal();
    void             MakeRefInternal();
    // A sampled-only right copy that no mirror has written holds nothing of the game's (a texture streamed past the
    // wrapper); the right eye samples the left until a mirror, or under ComputeOutputsPerEye a right UAV bind, fills it.
    void             MarkRightWritten()    { m_rightWritten = true; }
    bool             RightUnwritten() const
    {
        if (!m_realRight || m_rightWritten) return false;
        if (m_sampledOnly < 0)
        {
            D3D11_TEXTURE2D_DESC d = {};
            m_realLeft->GetDesc(&d);
            const UINT perEye = D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL |
                                (ComputeOutputsPerEyeOn() ? 0u : (UINT)D3D11_BIND_UNORDERED_ACCESS);
            m_sampledOnly = (d.BindFlags & perEye) ? 0 : 1;
        }
        return m_sampledOnly == 1;
    }

private:
    ID3D11Texture2D* m_realLeft;   // owned (released in dtor)
    ID3D11Texture2D* m_realRight;  // owned, nullable
    Device11Proxy*   m_parent;     // not owned
    LONG             m_refs;
    LONG             m_internalRefs = 0;   // of m_refs, the ones AddRef/Release leave out
    bool             m_backBuffer = false;
    bool             m_rightWritten = false;
    DXGI_FORMAT      m_viewFormat = DXGI_FORMAT_UNKNOWN;
    mutable signed char m_sampledOnly = -1;
};

} // namespace wiz3d
