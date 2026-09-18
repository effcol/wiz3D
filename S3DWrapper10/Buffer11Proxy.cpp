/* wiz3D - ID3D11Buffer proxy implementation (Option B Stage 3c.1) */

#include "StdAfx.h"
#include "Buffer11Proxy.h"
#include "Device11Proxy.h"
#include "proxy_factory.h"     // IID_wiz3D_Buffer11Proxy

#pragma comment(lib, "dxguid.lib")

namespace wiz3d
{

Buffer11Proxy::Buffer11Proxy(ID3D11Buffer* real, Device11Proxy* parent)
    : m_real(real)
    , m_realRight(nullptr)
    , m_parent(parent)
    , m_refs(1)
    , m_vsBound(false)
{
}

Buffer11Proxy::~Buffer11Proxy()
{
    if (m_realRight) { m_realRight->Release(); m_realRight = nullptr; }
    if (m_real) { m_real->Release(); m_real = nullptr; }
}

ULONG STDMETHODCALLTYPE Buffer11Proxy::Release()
{
    LONG r = InterlockedDecrement(&m_refs);
    if (r == 0) delete this;
    return (ULONG)r;
}

HRESULT STDMETHODCALLTYPE Buffer11Proxy::QueryInterface(REFIID riid, void** ppvObj)
{
    if (!ppvObj) return E_POINTER;
    if (riid == IID_IUnknown        ||
        riid == IID_ID3D11DeviceChild ||
        riid == IID_ID3D11Resource  ||
        riid == IID_ID3D11Buffer)
    {
        *ppvObj = static_cast<ID3D11Buffer*>(this);
        AddRef();
        return S_OK;
    }
    if (riid == IID_wiz3D_Buffer11Proxy)
    {
        *ppvObj = static_cast<IUnknown*>(static_cast<ID3D11Buffer*>(this));
        AddRef();
        return S_OK;
    }
    return m_real->QueryInterface(riid, ppvObj);
}

void STDMETHODCALLTYPE Buffer11Proxy::GetDevice(ID3D11Device** ppDevice)
{
    if (!ppDevice) return;
    if (m_parent)
    {
        *ppDevice = static_cast<ID3D11Device*>(m_parent);
        m_parent->AddRef();
        return;
    }
    m_real->GetDevice(ppDevice);
}

} // namespace wiz3d
