/* wiz3D - ID3D11Buffer proxy (Option B Stage 3c.1)
 *
 * Passthrough wrap of ID3D11Buffer. Carries a sticky "ever bound to a
 * vertex-pipeline stage" flag the Stage 4c projection-matrix heuristic
 * uses to filter PS-only / CS-only CBs out of the eye-shift modify path.
 *
 * Buffers are not stereo-doubled — there's no analogue to a right-eye RT
 * for a buffer (Stage 4c modifies CB bytes per eye via Map/Unmap instead).
 * So unlike Texture2D11Proxy this is just identity + metadata, single real
 * pointer.
 */

#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>

namespace wiz3d
{

class Device11Proxy;

class Buffer11Proxy : public ID3D11Buffer
{
public:
    Buffer11Proxy(ID3D11Buffer* real, Device11Proxy* parent);
    virtual ~Buffer11Proxy();

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObj) override;
    ULONG   STDMETHODCALLTYPE AddRef() override                  { return InterlockedIncrement(&m_refs); }
    ULONG   STDMETHODCALLTYPE Release() override;

    // ID3D11DeviceChild
    void    STDMETHODCALLTYPE GetDevice(ID3D11Device** ppDevice) override;
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT* pDataSize, void* pData) override                  { return m_real->GetPrivateData(guid, pDataSize, pData); }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT DataSize, const void* pData) override              { return m_real->SetPrivateData(guid, DataSize, pData); }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown* pData) override                { return m_real->SetPrivateDataInterface(guid, pData); }

    // ID3D11Resource
    void    STDMETHODCALLTYPE GetType(D3D11_RESOURCE_DIMENSION* pResourceDimension) override                       { m_real->GetType(pResourceDimension); }
    void    STDMETHODCALLTYPE SetEvictionPriority(UINT EvictionPriority) override                                  { m_real->SetEvictionPriority(EvictionPriority); }
    UINT    STDMETHODCALLTYPE GetEvictionPriority() override                                                       { return m_real->GetEvictionPriority(); }

    // ID3D11Buffer
    void    STDMETHODCALLTYPE GetDesc(D3D11_BUFFER_DESC* pDesc) override                                           { m_real->GetDesc(pDesc); }

    // wiz3D accessors
    ID3D11Buffer*  GetReal()       const { return m_real;   }
    Device11Proxy* GetParent()     const { return m_parent; }

    // Right-eye CB sibling, so draw duplication switches eyes by rebinding
    // rather than rewriting a shared buffer. Null unless gInfo.DuplicateDraws.
    ID3D11Buffer*  GetRealRight()  const { return m_realRight; }
    void           SetRealRight(ID3D11Buffer* p) { m_realRight = p; }

    // Stage 4c.1: sticky tag — set when *SetConstantBuffers fires on a
    // vertex-pipeline stage (VS / GS / HS / DS). Map/Unmap consults this
    // to decide whether to record + eye-shift the captured CB bytes.
    // Once set, never cleared — the buffer is eligible for stereo math
    // for its lifetime. m_evictionPriority field is the closest existing
    // metadata slot, but a dedicated bool keeps semantics clean.
    bool IsVSBound() const { return m_vsBound; }
    void TagVSBound()      { m_vsBound = true; }

private:
    ID3D11Buffer*  m_real;      // owned (released in dtor)
    ID3D11Buffer*  m_realRight; // owned, null unless a stereo CB sibling
    Device11Proxy* m_parent;    // not owned
    LONG           m_refs;
    bool           m_vsBound;
};

} // namespace wiz3d
