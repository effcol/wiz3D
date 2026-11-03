/*
* Project : wiz3D
* Copyright (C) Akshay Hegde 2026
*/

// Deferred contexts in pairs, after DX12's dual record: every call goes to the game's context with left reals and to a
// right twin with right copies; FinishCommandList pairs the two lists (Ryse streams textures through one).

#include "StdAfx.h"
#include "Device11Proxy.h"
#include "DeferredContext11.h"
#include "EyeUnwrap11.h"
#include "Buffer11Proxy.h"
#include "Texture2D11Proxy.h"
#include "proxy_factory.h"                 // TryUnwrap*
#include "ProfileFeature.h"                // EntryValue, FeatureLog
#include "StereoHeuristic.h"               // IsBlockCompressedFormat
#include "Context11Proxy-inl.h"            // BoundShaderCRC
#include "ShaderHunter.h"                  // 0.5.0 deferred-context hunt plumbing
#include "..\S3DAPI\ShaderProfileData.h"   // g_ProfileData
#include <d3d11_3.h>
#include <unordered_set>

namespace wiz3d
{
namespace
{

// {5C2F7A61-3D84-4B9E-A1C7-0E6B92D4F318}
const GUID IID_wiz3D_DeferredPair11 = { 0x5c2f7a61, 0x3d84, 0x4b9e, { 0xa1, 0xc7, 0x0e, 0x6b, 0x92, 0xd4, 0xf3, 0x18 } };
// {8E41B0D3-62F5-4C17-9A2E-D57C08B1E64A}
const GUID IID_wiz3D_CommandListPair11 = { 0x8e41b0d3, 0x62f5, 0x4c17, { 0x9a, 0x2e, 0xd5, 0x7c, 0x08, 0xb1, 0xe6, 0x4a } };

volatile LONG s_pairs = 0, s_rightFailed = 0, s_rightMapFailed = 0;
volatile LONG s_finished = 0, s_rightKept = 0, s_rightDropped = 0, s_executed = 0;

const UINT kMaxSRVs = 128, kMaxCBs = 16, kMaxRTVs = 8, kMaxUAVs = 64, kMaxVBs = 32, kMaxSO = 4;

ID3D11Resource* LeftRes(ID3D11Resource* p)  { return UnwrapResourceForEye(p, false); }
ID3D11Resource* RightRes(ID3D11Resource* p) { return p ? RightEyeResource(p) : nullptr; }
ID3D11Buffer*   RightBuf(ID3D11Buffer* p)   { return static_cast<ID3D11Buffer*>(RightRes(p)); }
template <class V> V* ViewFor(V* p, bool right) { return static_cast<V*>(UnwrapViewForEye(p, right)); }

// A tiled-resource barrier names a resource or a view.
ID3D11DeviceChild* LeftChild(ID3D11DeviceChild* p)
{
    if (!p) return nullptr;
    ID3D11Resource* r = nullptr;
    if (SUCCEEDED(p->QueryInterface(__uuidof(ID3D11Resource), reinterpret_cast<void**>(&r))) && r)
    {
        r->Release();
        return LeftRes(r);
    }
    ID3D11View* v = nullptr;
    if (SUCCEEDED(p->QueryInterface(__uuidof(ID3D11View), reinterpret_cast<void**>(&v))) && v)
    {
        v->Release();
        return UnwrapViewForEye(v, false);
    }
    return p;
}

// A mapping's extent: a buffer's width, or a texture subresource's rows (block rows when compressed) and slices.
void MapExtent(ID3D11Resource* real, UINT sub, UINT& rows, UINT& slices, UINT& bufferBytes)
{
    rows = slices = 1;
    bufferBytes = 0;
    D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    real->GetType(&dim);
    if (dim == D3D11_RESOURCE_DIMENSION_BUFFER)
    {
        D3D11_BUFFER_DESC d = {};
        static_cast<ID3D11Buffer*>(real)->GetDesc(&d);
        bufferBytes = d.ByteWidth;
    }
    else if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D)
    {
        D3D11_TEXTURE2D_DESC d = {};
        static_cast<ID3D11Texture2D*>(real)->GetDesc(&d);
        const UINT mip = d.MipLevels ? sub % d.MipLevels : 0;
        const UINT h = (d.Height >> mip) ? (d.Height >> mip) : 1;
        rows = IsBlockCompressedFormat(d.Format) ? (h + 3) / 4 : h;
    }
    else if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE3D)
    {
        D3D11_TEXTURE3D_DESC d = {};
        static_cast<ID3D11Texture3D*>(real)->GetDesc(&d);
        const UINT mip = d.MipLevels ? sub % d.MipLevels : 0;
        const UINT h = (d.Height >> mip) ? (d.Height >> mip) : 1;
        rows = IsBlockCompressedFormat(d.Format) ? (h + 3) / 4 : h;
        slices = (d.Depth >> mip) ? (d.Depth >> mip) : 1;
    }
}

// The two finished lists; the right is null when its recording did no right-eye work.
class CommandListPair11 : public ID3D11CommandList
{
public:
    CommandListPair11(ID3D11CommandList* left, ID3D11CommandList* right, Device11Proxy* device,
                      std::vector<Buffer11Proxy*>&& written)
        : m_left(left), m_right(right), m_device(device), m_written(std::move(written))
    {
        if (m_device) m_device->AddRef();
    }
    virtual ~CommandListPair11()
    {
        if (m_left) m_left->Release();
        if (m_right) m_right->Release();
        for (Buffer11Proxy* b : m_written) b->Release();
        if (m_device) m_device->Release();
    }

    ID3D11CommandList* Left() const  { return m_left; }
    ID3D11CommandList* Right() const { return m_right; }
    const std::vector<Buffer11Proxy*>& Written() const { return m_written; }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D11DeviceChild) || riid == __uuidof(ID3D11CommandList) ||
            riid == IID_wiz3D_CommandListPair11)
        {
            *ppv = static_cast<ID3D11CommandList*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&m_refs); }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const LONG r = InterlockedDecrement(&m_refs);
        if (r == 0) delete this;
        return (ULONG)r;
    }
    void STDMETHODCALLTYPE GetDevice(ID3D11Device** pp) override
    {
        if (!pp) return;
        *pp = m_device;
        if (m_device) m_device->AddRef();
    }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID g, UINT* n, void* d) override            { return m_left->GetPrivateData(g, n, d); }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID g, UINT n, const void* d) override       { return m_left->SetPrivateData(g, n, d); }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID g, const IUnknown* d) override  { return m_left->SetPrivateDataInterface(g, d); }
    UINT    STDMETHODCALLTYPE GetContextFlags() override                                      { return m_left->GetContextFlags(); }

private:
    volatile LONG               m_refs = 1;
    ID3D11CommandList*          m_left;
    ID3D11CommandList*          m_right;
    Device11Proxy*              m_device;
    std::vector<Buffer11Proxy*> m_written;   // one reference each
};

CommandListPair11* TryUnwrapPair(ID3D11CommandList* p)
{
    if (!p) return nullptr;
    IUnknown* probe = nullptr;
    if (FAILED(p->QueryInterface(IID_wiz3D_CommandListPair11, reinterpret_cast<void**>(&probe))) || !probe) return nullptr;
    probe->Release();
    return static_cast<CommandListPair11*>(static_cast<ID3D11CommandList*>(probe));
}

#define PAIR_BOTH(call) do { m_left->call; if (m_right) m_right->call; } while (0)

// Every shader stage's binds: resources per eye, shaders and samplers shared (they are the real objects already).
#define PAIR_STAGE(St, Shader)                                                                                                 \
    void STDMETHODCALLTYPE St##SetShaderResources(UINT s, UINT n, ID3D11ShaderResourceView* const* pp) override               \
    { SetSRVs(&ID3D11DeviceContext::St##SetShaderResources, s, n, pp); }                                                     \
    void STDMETHODCALLTYPE St##SetShader(Shader* sh, ID3D11ClassInstance* const* ci, UINT nci) override                        \
    { PAIR_BOTH(St##SetShader(sh, ci, nci)); }                                                                                 \
    void STDMETHODCALLTYPE St##SetSamplers(UINT s, UINT n, ID3D11SamplerState* const* pp) override                             \
    { PAIR_BOTH(St##SetSamplers(s, n, pp)); }                                                                                  \
    void STDMETHODCALLTYPE St##SetConstantBuffers(UINT s, UINT n, ID3D11Buffer* const* pp) override                            \
    { SetCBs(&ID3D11DeviceContext::St##SetConstantBuffers, s, n, pp); }                                                      \
    void STDMETHODCALLTYPE St##SetConstantBuffers1(UINT s, UINT n, ID3D11Buffer* const* pp, const UINT* f, const UINT* c) override \
    { SetCBs1(&ID3D11DeviceContext1::St##SetConstantBuffers1, s, n, pp, f, c); }                                             \
    void STDMETHODCALLTYPE St##GetShaderResources(UINT s, UINT n, ID3D11ShaderResourceView** pp) override                      \
    { m_left->St##GetShaderResources(s, n, pp); }                                                                              \
    void STDMETHODCALLTYPE St##GetShader(Shader** sh, ID3D11ClassInstance** ci, UINT* nci) override                            \
    { m_left->St##GetShader(sh, ci, nci); }                                                                                    \
    void STDMETHODCALLTYPE St##GetSamplers(UINT s, UINT n, ID3D11SamplerState** pp) override                                   \
    { m_left->St##GetSamplers(s, n, pp); }                                                                                     \
    void STDMETHODCALLTYPE St##GetConstantBuffers(UINT s, UINT n, ID3D11Buffer** pp) override                                  \
    { m_left->St##GetConstantBuffers(s, n, pp); }                                                                              \
    void STDMETHODCALLTYPE St##GetConstantBuffers1(UINT s, UINT n, ID3D11Buffer** pp, UINT* f, UINT* c) override               \
    { if (m_left1) m_left1->St##GetConstantBuffers1(s, n, pp, f, c); }

// A deferred context and its right-eye twin: draws and dispatches reach the twin only with a right-eye output bound,
// writes always (the right copy, or a shared resource replayed in order); getters answer from the game's context.
class DeferredPair11 : public ID3D11DeviceContext3
{
public:
    DeferredPair11(ID3D11DeviceContext* left, ID3D11DeviceContext* right, Device11Proxy* device)
        : m_left(left), m_right(right), m_device(device)
    {
        Query(m_left, m_left1, m_left2, m_left3);
        Query(m_right, m_right1, m_right2, m_right3);
        if (m_device) m_device->AddRef();
    }
    virtual ~DeferredPair11()
    {
        ReleaseAll(m_left, m_left1, m_left2, m_left3);
        ReleaseAll(m_right, m_right1, m_right2, m_right3);
        for (Buffer11Proxy* b : m_written) b->Release();
        if (m_device) m_device->Release();
    }

    // IUnknown: the context interfaces are ours; annotations pass to the game's context (markers only); nothing else does.
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D11DeviceChild) || riid == __uuidof(ID3D11DeviceContext) ||
            riid == IID_wiz3D_DeferredPair11)
            *ppv = static_cast<ID3D11DeviceContext*>(this);
        else if (riid == __uuidof(ID3D11DeviceContext1) && m_left1) *ppv = static_cast<ID3D11DeviceContext1*>(this);
        else if (riid == __uuidof(ID3D11DeviceContext2) && m_left2) *ppv = static_cast<ID3D11DeviceContext2*>(this);
        else if (riid == __uuidof(ID3D11DeviceContext3) && m_left3) *ppv = static_cast<ID3D11DeviceContext3*>(this);
        if (*ppv) { AddRef(); return S_OK; }
        if (riid == __uuidof(ID3DUserDefinedAnnotation)) return m_left->QueryInterface(riid, ppv);
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&m_refs); }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const LONG r = InterlockedDecrement(&m_refs);
        if (r == 0) delete this;
        return (ULONG)r;
    }

    // ID3D11DeviceChild
    void STDMETHODCALLTYPE GetDevice(ID3D11Device** pp) override
    {
        if (!pp) return;
        *pp = m_device;
        if (m_device) m_device->AddRef();
    }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID g, UINT* n, void* d) override            { return m_left->GetPrivateData(g, n, d); }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID g, UINT n, const void* d) override       { return m_left->SetPrivateData(g, n, d); }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID g, const IUnknown* d) override  { return m_left->SetPrivateDataInterface(g, d); }

    PAIR_STAGE(VS, ID3D11VertexShader)
    PAIR_STAGE(HS, ID3D11HullShader)
    PAIR_STAGE(DS, ID3D11DomainShader)
    PAIR_STAGE(GS, ID3D11GeometryShader)
    PAIR_STAGE(PS, ID3D11PixelShader)
    PAIR_STAGE(CS, ID3D11ComputeShader)

    // Input assembler, rasteriser, output-merger state
    void STDMETHODCALLTYPE IASetInputLayout(ID3D11InputLayout* p) override                   { PAIR_BOTH(IASetInputLayout(p)); }
    void STDMETHODCALLTYPE IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY t) override       { PAIR_BOTH(IASetPrimitiveTopology(t)); }
    void STDMETHODCALLTYPE IASetVertexBuffers(UINT s, UINT n, ID3D11Buffer* const* pp, const UINT* strides, const UINT* offsets) override
    {
        ID3D11Buffer* l[kMaxVBs] = {};
        ID3D11Buffer* r[kMaxVBs] = {};
        if (n > kMaxVBs) n = kMaxVBs;
        for (UINT i = 0; pp && i < n; ++i) { l[i] = UnwrapBuf(pp[i]); r[i] = RightBuf(pp[i]); }
        m_left->IASetVertexBuffers(s, n, pp ? l : nullptr, strides, offsets);
        if (m_right) m_right->IASetVertexBuffers(s, n, pp ? r : nullptr, strides, offsets);
    }
    void STDMETHODCALLTYPE IASetIndexBuffer(ID3D11Buffer* p, DXGI_FORMAT f, UINT o) override
    {
        m_left->IASetIndexBuffer(UnwrapBuf(p), f, o);
        if (m_right) m_right->IASetIndexBuffer(RightBuf(p), f, o);
    }
    void STDMETHODCALLTYPE SOSetTargets(UINT n, ID3D11Buffer* const* pp, const UINT* offsets) override
    {
        ID3D11Buffer* l[kMaxSO] = {};
        ID3D11Buffer* r[kMaxSO] = {};
        if (n > kMaxSO) n = kMaxSO;
        m_soRight = false;
        for (UINT i = 0; pp && i < n; ++i)
        {
            l[i] = UnwrapBuf(pp[i]);
            r[i] = RightBuf(pp[i]);
            if (r[i] != l[i]) m_soRight = true;
        }
        m_left->SOSetTargets(n, pp ? l : nullptr, offsets);
        if (m_right) m_right->SOSetTargets(n, pp ? r : nullptr, offsets);
        UpdateOutputs();
    }
    void STDMETHODCALLTYPE RSSetState(ID3D11RasterizerState* p) override                     { PAIR_BOTH(RSSetState(p)); }
    void STDMETHODCALLTYPE RSSetViewports(UINT n, const D3D11_VIEWPORT* v) override          { PAIR_BOTH(RSSetViewports(n, v)); }
    void STDMETHODCALLTYPE RSSetScissorRects(UINT n, const D3D11_RECT* r) override           { PAIR_BOTH(RSSetScissorRects(n, r)); }
    void STDMETHODCALLTYPE OMSetBlendState(ID3D11BlendState* p, const FLOAT f[4], UINT m) override { PAIR_BOTH(OMSetBlendState(p, f, m)); }
    void STDMETHODCALLTYPE OMSetDepthStencilState(ID3D11DepthStencilState* p, UINT ref) override   { PAIR_BOTH(OMSetDepthStencilState(p, ref)); }
    void STDMETHODCALLTYPE OMSetRenderTargets(UINT n, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv) override
    {
        ID3D11RenderTargetView* l[kMaxRTVs] = {};
        ID3D11RenderTargetView* r[kMaxRTVs] = {};
        if (n > kMaxRTVs) n = kMaxRTVs;
        SetTargets(n, rtvs, dsv, l, r);
        m_left->OMSetRenderTargets(n, rtvs ? l : nullptr, ViewFor(dsv, false));
        if (m_right) m_right->OMSetRenderTargets(n, rtvs ? r : nullptr, ViewFor(dsv, true));
        UpdateOutputs();
    }
    void STDMETHODCALLTYPE OMSetRenderTargetsAndUnorderedAccessViews(UINT nRTV, ID3D11RenderTargetView* const* rtvs,
        ID3D11DepthStencilView* dsv, UINT uavStart, UINT nUAV, ID3D11UnorderedAccessView* const* uavs, const UINT* counts) override
    {
        ID3D11RenderTargetView* l[kMaxRTVs] = {};
        ID3D11RenderTargetView* r[kMaxRTVs] = {};
        ID3D11UnorderedAccessView* lu[kMaxUAVs] = {};
        ID3D11UnorderedAccessView* ru[kMaxUAVs] = {};
        const bool keepRTVs = nRTV == D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL;
        const bool keepUAVs = nUAV == D3D11_KEEP_UNORDERED_ACCESS_VIEWS;
        if (!keepRTVs)
        {
            if (nRTV > kMaxRTVs) nRTV = kMaxRTVs;
            SetTargets(nRTV, rtvs, dsv, l, r);
        }
        if (!keepUAVs)
        {
            if (uavStart >= kMaxUAVs) nUAV = 0;
            else if (nUAV > kMaxUAVs - uavStart) nUAV = kMaxUAVs - uavStart;
            for (UINT i = 0; i < kMaxUAVs; ++i) m_omUavRight[i] = false;
            for (UINT i = 0; uavs && i < nUAV; ++i)
            {
                lu[i] = UnwrapUAVForEye(uavs[i], false);
                ru[i] = UnwrapUAVForEye(uavs[i], true);
                m_omUavRight[uavStart + i] = ru[i] != lu[i];
            }
        }
        m_left->OMSetRenderTargetsAndUnorderedAccessViews(nRTV, (!keepRTVs && rtvs) ? l : rtvs, keepRTVs ? dsv : ViewFor(dsv, false),
                                                          uavStart, nUAV, (!keepUAVs && uavs) ? lu : uavs, counts);
        if (m_right)
            m_right->OMSetRenderTargetsAndUnorderedAccessViews(nRTV, (!keepRTVs && rtvs) ? r : rtvs, keepRTVs ? dsv : ViewFor(dsv, true),
                                                               uavStart, nUAV, (!keepUAVs && uavs) ? ru : uavs, counts);
        UpdateOutputs();
    }
    void STDMETHODCALLTYPE CSSetUnorderedAccessViews(UINT s, UINT n, ID3D11UnorderedAccessView* const* pp, const UINT* counts) override
    {
        ID3D11UnorderedAccessView* l[kMaxUAVs] = {};
        ID3D11UnorderedAccessView* r[kMaxUAVs] = {};
        if (s >= kMaxUAVs) n = 0;
        else if (n > kMaxUAVs - s) n = kMaxUAVs - s;
        for (UINT i = 0; i < n; ++i)
        {
            l[i] = pp ? UnwrapUAVForEye(pp[i], false) : nullptr;
            r[i] = pp ? UnwrapUAVForEye(pp[i], true) : nullptr;
            m_csUavRight[s + i] = r[i] != l[i];
        }
        m_left->CSSetUnorderedAccessViews(s, n, pp ? l : nullptr, counts);
        if (m_right) m_right->CSSetUnorderedAccessViews(s, n, pp ? r : nullptr, counts);
        m_csRight = false;
        for (UINT i = 0; i < kMaxUAVs; ++i) if (m_csUavRight[i]) m_csRight = true;
    }

    // 0.5.0 ShaderHunter wiring for deferred contexts. Design doc risk #3
    // originally excluded these from the hunt path on lock-cost grounds; we
    // now plumb them through so a game like Fallout Shelter (Unity, uses
    // deferred contexts for UI) exposes every shader to Numpad 1/2 cycling
    // and the on/off flash. GetShader queries are O(1) COM-interface returns
    // with an AddRef — one Release per draw at hunt time, zero cost when off.
    DWORD HuntVS_()
    {
        if (!wiz3d::ShaderHunter::IsActive()) return 0;
        ID3D11VertexShader* vs = nullptr;
        m_left->VSGetShader(&vs, nullptr, nullptr);
        if (!vs) return 0;
        const DWORD crc = wiz3d::BoundShaderCRC(m_device, vs);
        vs->Release();
        return crc;
    }
    DWORD HuntPS_()
    {
        if (!wiz3d::ShaderHunter::IsActive()) return 0;
        ID3D11PixelShader* ps = nullptr;
        m_left->PSGetShader(&ps, nullptr, nullptr);
        if (!ps) return 0;
        const DWORD crc = wiz3d::BoundShaderCRC(m_device, ps);
        ps->Release();
        return crc;
    }

    // Draws and dispatches. Hunter notes every draw's bound VS+PS and skips
    // on the flash cadence (ShouldSkipDraw returns true 15/30 frames while
    // the hunted CRC is bound). Fast path (!IsActive) returns 0 instantly
    // and the skip is a short-circuit false.
    void STDMETHODCALLTYPE Draw(UINT c, UINT s) override
    {
        const DWORD v = HuntVS_(), p = HuntPS_();
        wiz3d::ShaderHunter::NoteDraw(v, p);
        if (wiz3d::ShaderHunter::ShouldSkipDraw(v, p)) return;
        m_left->Draw(c, s); if (RightDraw()) m_right->Draw(c, s);
    }
    void STDMETHODCALLTYPE DrawIndexed(UINT c, UINT s, INT b) override
    {
        const DWORD v = HuntVS_(), p = HuntPS_();
        wiz3d::ShaderHunter::NoteDraw(v, p);
        if (wiz3d::ShaderHunter::ShouldSkipDraw(v, p)) return;
        m_left->DrawIndexed(c, s, b); if (RightDraw()) m_right->DrawIndexed(c, s, b);
    }
    void STDMETHODCALLTYPE DrawInstanced(UINT c, UINT n, UINT s, UINT si) override
    {
        const DWORD v = HuntVS_(), p = HuntPS_();
        wiz3d::ShaderHunter::NoteDraw(v, p);
        if (wiz3d::ShaderHunter::ShouldSkipDraw(v, p)) return;
        m_left->DrawInstanced(c, n, s, si); if (RightDraw()) m_right->DrawInstanced(c, n, s, si);
    }
    void STDMETHODCALLTYPE DrawIndexedInstanced(UINT c, UINT n, UINT s, INT b, UINT si) override
    {
        const DWORD v = HuntVS_(), p = HuntPS_();
        wiz3d::ShaderHunter::NoteDraw(v, p);
        if (wiz3d::ShaderHunter::ShouldSkipDraw(v, p)) return;
        m_left->DrawIndexedInstanced(c, n, s, b, si); if (RightDraw()) m_right->DrawIndexedInstanced(c, n, s, b, si);
    }
    void STDMETHODCALLTYPE DrawAuto() override
    {
        const DWORD v = HuntVS_(), p = HuntPS_();
        wiz3d::ShaderHunter::NoteDraw(v, p);
        if (wiz3d::ShaderHunter::ShouldSkipDraw(v, p)) return;
        m_left->DrawAuto(); if (RightDraw()) m_right->DrawAuto();
    }
    void STDMETHODCALLTYPE DrawInstancedIndirect(ID3D11Buffer* a, UINT o) override
    {
        const DWORD v = HuntVS_(), p = HuntPS_();
        wiz3d::ShaderHunter::NoteDraw(v, p);
        if (wiz3d::ShaderHunter::ShouldSkipDraw(v, p)) return;
        m_left->DrawInstancedIndirect(UnwrapBuf(a), o); if (RightDraw()) m_right->DrawInstancedIndirect(RightBuf(a), o);
    }
    void STDMETHODCALLTYPE DrawIndexedInstancedIndirect(ID3D11Buffer* a, UINT o) override
    {
        const DWORD v = HuntVS_(), p = HuntPS_();
        wiz3d::ShaderHunter::NoteDraw(v, p);
        if (wiz3d::ShaderHunter::ShouldSkipDraw(v, p)) return;
        m_left->DrawIndexedInstancedIndirect(UnwrapBuf(a), o); if (RightDraw()) m_right->DrawIndexedInstancedIndirect(RightBuf(a), o);
    }
    void STDMETHODCALLTYPE Dispatch(UINT x, UINT y, UINT z) override                         { m_left->Dispatch(x, y, z); if (RightDispatch()) m_right->Dispatch(x, y, z); }
    void STDMETHODCALLTYPE DispatchIndirect(ID3D11Buffer* a, UINT o) override                { m_left->DispatchIndirect(UnwrapBuf(a), o); if (RightDispatch()) m_right->DispatchIndirect(RightBuf(a), o); }

    // Writes
    void STDMETHODCALLTYPE UpdateSubresource(ID3D11Resource* dst, UINT sub, const D3D11_BOX* box, const void* data, UINT rp, UINT dp) override
    {
        m_left->UpdateSubresource(LeftRes(dst), sub, box, data, rp, dp);
        if (m_right) m_right->UpdateSubresource(RightRes(dst), sub, box, data, rp, dp);
        NoteWrite(dst);
    }
    void STDMETHODCALLTYPE UpdateSubresource1(ID3D11Resource* dst, UINT sub, const D3D11_BOX* box, const void* data, UINT rp, UINT dp, UINT flags) override
    {
        if (!m_left1) return;
        m_left1->UpdateSubresource1(LeftRes(dst), sub, box, data, rp, dp, flags);
        if (m_right1) m_right1->UpdateSubresource1(RightRes(dst), sub, box, data, rp, dp, flags);
        NoteWrite(dst);
    }
    void STDMETHODCALLTYPE CopyResource(ID3D11Resource* dst, ID3D11Resource* src) override
    {
        m_left->CopyResource(LeftRes(dst), LeftRes(src));
        if (m_right)
        {
            if (HasRight(dst)) m_right->CopyResource(RightRes(dst), RightRes(src));
            else m_right->CopyResource(LeftRes(dst), LeftRes(src));
        }
        NoteWrite(dst);
    }
    void STDMETHODCALLTYPE CopySubresourceRegion(ID3D11Resource* dst, UINT ds, UINT x, UINT y, UINT z, ID3D11Resource* src, UINT ss, const D3D11_BOX* box) override
    {
        m_left->CopySubresourceRegion(LeftRes(dst), ds, x, y, z, LeftRes(src), ss, box);
        if (m_right)
        {
            if (HasRight(dst)) m_right->CopySubresourceRegion(RightRes(dst), ds, x, y, z, RightRes(src), ss, box);
            else m_right->CopySubresourceRegion(LeftRes(dst), ds, x, y, z, LeftRes(src), ss, box);
        }
        NoteWrite(dst);
    }
    void STDMETHODCALLTYPE CopySubresourceRegion1(ID3D11Resource* dst, UINT ds, UINT x, UINT y, UINT z, ID3D11Resource* src, UINT ss, const D3D11_BOX* box, UINT flags) override
    {
        if (!m_left1) return;
        m_left1->CopySubresourceRegion1(LeftRes(dst), ds, x, y, z, LeftRes(src), ss, box, flags);
        if (m_right1)
        {
            if (HasRight(dst)) m_right1->CopySubresourceRegion1(RightRes(dst), ds, x, y, z, RightRes(src), ss, box, flags);
            else m_right1->CopySubresourceRegion1(LeftRes(dst), ds, x, y, z, LeftRes(src), ss, box, flags);
        }
        NoteWrite(dst);
    }
    void STDMETHODCALLTYPE CopyStructureCount(ID3D11Buffer* dst, UINT o, ID3D11UnorderedAccessView* src) override
    {
        m_left->CopyStructureCount(UnwrapBuf(dst), o, UnwrapUAVForEye(src, false));
        if (m_right)
        {
            if (HasRight(dst)) m_right->CopyStructureCount(RightBuf(dst), o, UnwrapUAVForEye(src, true));
            else m_right->CopyStructureCount(UnwrapBuf(dst), o, UnwrapUAVForEye(src, false));
        }
        NoteWrite(dst);
    }
    void STDMETHODCALLTYPE ResolveSubresource(ID3D11Resource* dst, UINT ds, ID3D11Resource* src, UINT ss, DXGI_FORMAT f) override
    {
        m_left->ResolveSubresource(LeftRes(dst), ds, LeftRes(src), ss, f);
        if (m_right)
        {
            if (HasRight(dst)) m_right->ResolveSubresource(RightRes(dst), ds, RightRes(src), ss, f);
            else m_right->ResolveSubresource(LeftRes(dst), ds, LeftRes(src), ss, f);
        }
        NoteWrite(dst);
    }
    void STDMETHODCALLTYPE GenerateMips(ID3D11ShaderResourceView* v) override
    {
        ID3D11ShaderResourceView* l = ViewFor(v, false);
        ID3D11ShaderResourceView* r = ViewFor(v, true);
        m_left->GenerateMips(l);
        if (m_right) m_right->GenerateMips(r);
        if (r != l) m_rightWork = true;
    }
    HRESULT STDMETHODCALLTYPE Map(ID3D11Resource* res, UINT sub, D3D11_MAP type, UINT flags, D3D11_MAPPED_SUBRESOURCE* pm) override
    {
        const HRESULT hr = m_left->Map(LeftRes(res), sub, type, flags, pm);
        if (SUCCEEDED(hr) && pm && m_right) m_maps.push_back({ res, sub, type, *pm });
        return hr;
    }
    void STDMETHODCALLTYPE Unmap(ID3D11Resource* res, UINT sub) override
    {
        for (size_t i = 0; i < m_maps.size(); ++i)
        {
            if (m_maps[i].res != res || m_maps[i].sub != sub) continue;
            CopyMapToRight(m_maps[i]);
            m_maps.erase(m_maps.begin() + i);
            break;
        }
        m_left->Unmap(LeftRes(res), sub);
        NoteWrite(res);
    }

    // Clears: a right-eye view gets its own; a shared one is cleared once, as the twin draws nothing into it.
    void STDMETHODCALLTYPE ClearRenderTargetView(ID3D11RenderTargetView* v, const FLOAT c[4]) override
    {
        m_left->ClearRenderTargetView(ViewFor(v, false), c);
        if (m_right && ViewFor(v, true) != ViewFor(v, false)) { m_right->ClearRenderTargetView(ViewFor(v, true), c); m_rightWork = true; }
    }
    void STDMETHODCALLTYPE ClearDepthStencilView(ID3D11DepthStencilView* v, UINT f, FLOAT d, UINT8 s) override
    {
        m_left->ClearDepthStencilView(ViewFor(v, false), f, d, s);
        if (m_right && ViewFor(v, true) != ViewFor(v, false)) { m_right->ClearDepthStencilView(ViewFor(v, true), f, d, s); m_rightWork = true; }
    }
    void STDMETHODCALLTYPE ClearUnorderedAccessViewUint(ID3D11UnorderedAccessView* v, const UINT c[4]) override
    {
        m_left->ClearUnorderedAccessViewUint(UnwrapUAVForEye(v, false), c);
        if (m_right && UnwrapUAVForEye(v, true) != UnwrapUAVForEye(v, false)) { m_right->ClearUnorderedAccessViewUint(UnwrapUAVForEye(v, true), c); m_rightWork = true; }
    }
    void STDMETHODCALLTYPE ClearUnorderedAccessViewFloat(ID3D11UnorderedAccessView* v, const FLOAT c[4]) override
    {
        m_left->ClearUnorderedAccessViewFloat(UnwrapUAVForEye(v, false), c);
        if (m_right && UnwrapUAVForEye(v, true) != UnwrapUAVForEye(v, false)) { m_right->ClearUnorderedAccessViewFloat(UnwrapUAVForEye(v, true), c); m_rightWork = true; }
    }
    void STDMETHODCALLTYPE ClearView(ID3D11View* v, const FLOAT c[4], const D3D11_RECT* r, UINT n) override
    {
        if (!m_left1) return;
        m_left1->ClearView(UnwrapViewForEye(v, false), c, r, n);
        if (m_right1 && UnwrapViewForEye(v, true) != UnwrapViewForEye(v, false)) { m_right1->ClearView(UnwrapViewForEye(v, true), c, r, n); m_rightWork = true; }
    }
    void STDMETHODCALLTYPE DiscardResource(ID3D11Resource* p) override
    {
        if (!m_left1) return;
        m_left1->DiscardResource(LeftRes(p));
        if (m_right1) m_right1->DiscardResource(RightRes(p));
    }
    void STDMETHODCALLTYPE DiscardView(ID3D11View* v) override
    {
        if (!m_left1) return;
        m_left1->DiscardView(UnwrapViewForEye(v, false));
        if (m_right1) m_right1->DiscardView(UnwrapViewForEye(v, true));
    }
    void STDMETHODCALLTYPE DiscardView1(ID3D11View* v, const D3D11_RECT* r, UINT n) override
    {
        if (!m_left1) return;
        m_left1->DiscardView1(UnwrapViewForEye(v, false), r, n);
        if (m_right1) m_right1->DiscardView1(UnwrapViewForEye(v, true), r, n);
    }
    void STDMETHODCALLTYPE SetResourceMinLOD(ID3D11Resource* p, FLOAT lod) override
    {
        m_left->SetResourceMinLOD(LeftRes(p), lod);
        if (m_right) m_right->SetResourceMinLOD(RightRes(p), lod);
    }
    FLOAT STDMETHODCALLTYPE GetResourceMinLOD(ID3D11Resource* p) override                    { return m_left->GetResourceMinLOD(LeftRes(p)); }

    // Queries run in the game's list only; predication applies to both.
    void    STDMETHODCALLTYPE Begin(ID3D11Asynchronous* a) override                          { m_left->Begin(a); }
    void    STDMETHODCALLTYPE End(ID3D11Asynchronous* a) override                            { m_left->End(a); }
    HRESULT STDMETHODCALLTYPE GetData(ID3D11Asynchronous* a, void* d, UINT n, UINT f) override { return m_left->GetData(a, d, n, f); }
    void    STDMETHODCALLTYPE SetPredication(ID3D11Predicate* p, BOOL v) override            { PAIR_BOTH(SetPredication(p, v)); }
    void    STDMETHODCALLTYPE GetPredication(ID3D11Predicate** p, BOOL* v) override          { m_left->GetPredication(p, v); }

    // Getters
    void STDMETHODCALLTYPE IAGetInputLayout(ID3D11InputLayout** p) override                  { m_left->IAGetInputLayout(p); }
    void STDMETHODCALLTYPE IAGetVertexBuffers(UINT s, UINT n, ID3D11Buffer** pp, UINT* st, UINT* o) override { m_left->IAGetVertexBuffers(s, n, pp, st, o); }
    void STDMETHODCALLTYPE IAGetIndexBuffer(ID3D11Buffer** p, DXGI_FORMAT* f, UINT* o) override { m_left->IAGetIndexBuffer(p, f, o); }
    void STDMETHODCALLTYPE IAGetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY* t) override      { m_left->IAGetPrimitiveTopology(t); }
    void STDMETHODCALLTYPE OMGetRenderTargets(UINT n, ID3D11RenderTargetView** r, ID3D11DepthStencilView** d) override { m_left->OMGetRenderTargets(n, r, d); }
    void STDMETHODCALLTYPE OMGetRenderTargetsAndUnorderedAccessViews(UINT nr, ID3D11RenderTargetView** r, ID3D11DepthStencilView** d, UINT us, UINT nu, ID3D11UnorderedAccessView** u) override
    { m_left->OMGetRenderTargetsAndUnorderedAccessViews(nr, r, d, us, nu, u); }
    void STDMETHODCALLTYPE OMGetBlendState(ID3D11BlendState** p, FLOAT f[4], UINT* m) override { m_left->OMGetBlendState(p, f, m); }
    void STDMETHODCALLTYPE OMGetDepthStencilState(ID3D11DepthStencilState** p, UINT* r) override { m_left->OMGetDepthStencilState(p, r); }
    void STDMETHODCALLTYPE SOGetTargets(UINT n, ID3D11Buffer** pp) override                  { m_left->SOGetTargets(n, pp); }
    void STDMETHODCALLTYPE RSGetState(ID3D11RasterizerState** p) override                    { m_left->RSGetState(p); }
    void STDMETHODCALLTYPE RSGetViewports(UINT* n, D3D11_VIEWPORT* v) override               { m_left->RSGetViewports(n, v); }
    void STDMETHODCALLTYPE RSGetScissorRects(UINT* n, D3D11_RECT* r) override                { m_left->RSGetScissorRects(n, r); }
    void STDMETHODCALLTYPE CSGetUnorderedAccessViews(UINT s, UINT n, ID3D11UnorderedAccessView** pp) override { m_left->CSGetUnorderedAccessViews(s, n, pp); }

    // Recording
    void STDMETHODCALLTYPE ClearState() override                                             { PAIR_BOTH(ClearState()); ResetOutputs(); }
    void STDMETHODCALLTYPE Flush() override                                                  { m_left->Flush(); }
    D3D11_DEVICE_CONTEXT_TYPE STDMETHODCALLTYPE GetType() override                           { return m_left->GetType(); }
    UINT STDMETHODCALLTYPE GetContextFlags() override                                        { return m_left->GetContextFlags(); }
    void STDMETHODCALLTYPE ExecuteCommandList(ID3D11CommandList* list, BOOL restore) override
    {
        CommandListPair11* p = TryUnwrapPair(list);
        ID3D11CommandList* left = p ? p->Left() : list;
        m_left->ExecuteCommandList(left, restore);
        if (m_right) m_right->ExecuteCommandList((p && p->Right()) ? p->Right() : left, restore);
        if (p && p->Right()) m_rightWork = true;
        if (p) for (Buffer11Proxy* b : p->Written()) NoteBuffer(b);
        if (!restore) ResetOutputs();
    }
    HRESULT STDMETHODCALLTYPE FinishCommandList(BOOL restore, ID3D11CommandList** pp) override
    {
        InterlockedIncrement(&s_finished);
        m_maps.clear();
        ID3D11CommandList* l = nullptr;
        ID3D11CommandList* r = nullptr;
        const HRESULT hr = m_left->FinishCommandList(restore, pp ? &l : nullptr);
        if (m_right && FAILED(m_right->FinishCommandList(restore, &r))) r = nullptr;
        if (!restore) ResetOutputs();
        const bool rightWork = m_rightWork;
        m_rightWork = false;
        std::vector<Buffer11Proxy*> written;
        written.swap(m_written);
        m_writtenSet.clear();
        if (FAILED(hr) || !pp || !l)
        {
            if (l) l->Release();
            if (r) r->Release();
            for (Buffer11Proxy* b : written) b->Release();
            if (pp) *pp = nullptr;
            return hr;
        }
        if (r && !rightWork) { r->Release(); r = nullptr; InterlockedIncrement(&s_rightDropped); }
        else if (r) InterlockedIncrement(&s_rightKept);
        *pp = new CommandListPair11(l, r, m_device, std::move(written));
        return hr;
    }
    void STDMETHODCALLTYPE SwapDeviceContextState(ID3DDeviceContextState* s, ID3DDeviceContextState** prev) override
    { if (m_left1) m_left1->SwapDeviceContextState(s, prev); }

    // D3D11.2 tiled resources and annotations: the game's list only.
    HRESULT STDMETHODCALLTYPE UpdateTileMappings(ID3D11Resource* t, UINT nr, const D3D11_TILED_RESOURCE_COORDINATE* c, const D3D11_TILE_REGION_SIZE* sz,
        ID3D11Buffer* pool, UINT n, const UINT* rf, const UINT* off, const UINT* cnt, UINT f) override
    { return m_left2 ? m_left2->UpdateTileMappings(LeftRes(t), nr, c, sz, UnwrapBuf(pool), n, rf, off, cnt, f) : E_NOINTERFACE; }
    HRESULT STDMETHODCALLTYPE CopyTileMappings(ID3D11Resource* d, const D3D11_TILED_RESOURCE_COORDINATE* dc, ID3D11Resource* s,
        const D3D11_TILED_RESOURCE_COORDINATE* sc, const D3D11_TILE_REGION_SIZE* sz, UINT f) override
    { return m_left2 ? m_left2->CopyTileMappings(LeftRes(d), dc, LeftRes(s), sc, sz, f) : E_NOINTERFACE; }
    void STDMETHODCALLTYPE CopyTiles(ID3D11Resource* t, const D3D11_TILED_RESOURCE_COORDINATE* c, const D3D11_TILE_REGION_SIZE* sz,
        ID3D11Buffer* b, UINT64 o, UINT f) override
    { if (m_left2) m_left2->CopyTiles(LeftRes(t), c, sz, UnwrapBuf(b), o, f); }
    void STDMETHODCALLTYPE UpdateTiles(ID3D11Resource* t, const D3D11_TILED_RESOURCE_COORDINATE* c, const D3D11_TILE_REGION_SIZE* sz,
        const void* d, UINT f) override
    { if (m_left2) m_left2->UpdateTiles(LeftRes(t), c, sz, d, f); }
    HRESULT STDMETHODCALLTYPE ResizeTilePool(ID3D11Buffer* pool, UINT64 n) override         { return m_left2 ? m_left2->ResizeTilePool(UnwrapBuf(pool), n) : E_NOINTERFACE; }
    void STDMETHODCALLTYPE TiledResourceBarrier(ID3D11DeviceChild* b, ID3D11DeviceChild* a) override { if (m_left2) m_left2->TiledResourceBarrier(LeftChild(b), LeftChild(a)); }
    BOOL STDMETHODCALLTYPE IsAnnotationEnabled() override                                    { return m_left2 ? m_left2->IsAnnotationEnabled() : FALSE; }
    void STDMETHODCALLTYPE SetMarkerInt(LPCWSTR l, INT d) override                           { if (m_left2) m_left2->SetMarkerInt(l, d); }
    void STDMETHODCALLTYPE BeginEventInt(LPCWSTR l, INT d) override                          { if (m_left2) m_left2->BeginEventInt(l, d); }
    void STDMETHODCALLTYPE EndEvent() override                                               { if (m_left2) m_left2->EndEvent(); }

    // D3D11.3
    void STDMETHODCALLTYPE Flush1(D3D11_CONTEXT_TYPE t, HANDLE e) override                   { if (m_left3) m_left3->Flush1(t, e); }
    void STDMETHODCALLTYPE SetHardwareProtectionState(BOOL on) override                      { if (m_left3) m_left3->SetHardwareProtectionState(on); }
    void STDMETHODCALLTYPE GetHardwareProtectionState(BOOL* on) override                     { if (m_left3) m_left3->GetHardwareProtectionState(on); }

private:
    struct OpenMap { ID3D11Resource* res; UINT sub; D3D11_MAP type; D3D11_MAPPED_SUBRESOURCE m; };

    static void Query(ID3D11DeviceContext* c, ID3D11DeviceContext1*& c1, ID3D11DeviceContext2*& c2, ID3D11DeviceContext3*& c3)
    {
        c1 = nullptr; c2 = nullptr; c3 = nullptr;
        if (!c) return;
        if (FAILED(c->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&c1)))) c1 = nullptr;
        if (FAILED(c->QueryInterface(__uuidof(ID3D11DeviceContext2), reinterpret_cast<void**>(&c2)))) c2 = nullptr;
        if (FAILED(c->QueryInterface(__uuidof(ID3D11DeviceContext3), reinterpret_cast<void**>(&c3)))) c3 = nullptr;
    }
    static void ReleaseAll(ID3D11DeviceContext* c, ID3D11DeviceContext1* c1, ID3D11DeviceContext2* c2, ID3D11DeviceContext3* c3)
    {
        if (c3) c3->Release();
        if (c2) c2->Release();
        if (c1) c1->Release();
        if (c) c->Release();
    }

    static bool HasRight(ID3D11Resource* p) { return p && RightRes(p) != LeftRes(p); }

    typedef void (STDMETHODCALLTYPE ID3D11DeviceContext::*SrvFn)(UINT, UINT, ID3D11ShaderResourceView* const*);
    typedef void (STDMETHODCALLTYPE ID3D11DeviceContext::*CbFn)(UINT, UINT, ID3D11Buffer* const*);
    typedef void (STDMETHODCALLTYPE ID3D11DeviceContext1::*Cb1Fn)(UINT, UINT, ID3D11Buffer* const*, const UINT*, const UINT*);

    void SetSRVs(SrvFn fn, UINT s, UINT n, ID3D11ShaderResourceView* const* pp)
    {
        ID3D11ShaderResourceView* l[kMaxSRVs] = {};
        ID3D11ShaderResourceView* r[kMaxSRVs] = {};
        if (n > kMaxSRVs) n = kMaxSRVs;
        for (UINT i = 0; pp && i < n; ++i) { l[i] = UnwrapSRVForEye(pp[i], false); r[i] = UnwrapSRVForEye(pp[i], true); }
        (m_left->*fn)(s, n, pp ? l : nullptr);
        if (m_right) (m_right->*fn)(s, n, pp ? r : nullptr);
    }
    void SetCBs(CbFn fn, UINT s, UINT n, ID3D11Buffer* const* pp)
    {
        ID3D11Buffer* l[kMaxCBs] = {};
        ID3D11Buffer* r[kMaxCBs] = {};
        if (n > kMaxCBs) n = kMaxCBs;
        for (UINT i = 0; pp && i < n; ++i) { l[i] = UnwrapBuf(pp[i]); r[i] = RightBuf(pp[i]); }
        (m_left->*fn)(s, n, pp ? l : nullptr);
        if (m_right) (m_right->*fn)(s, n, pp ? r : nullptr);
    }
    void SetCBs1(Cb1Fn fn, UINT s, UINT n, ID3D11Buffer* const* pp, const UINT* first, const UINT* num)
    {
        if (!m_left1) return;
        ID3D11Buffer* l[kMaxCBs] = {};
        ID3D11Buffer* r[kMaxCBs] = {};
        if (n > kMaxCBs) n = kMaxCBs;
        for (UINT i = 0; pp && i < n; ++i) { l[i] = UnwrapBuf(pp[i]); r[i] = RightBuf(pp[i]); }
        (m_left1->*fn)(s, n, pp ? l : nullptr, first, num);
        if (m_right1) (m_right1->*fn)(s, n, pp ? r : nullptr, first, num);
    }

    // Render targets and depth: each eye's views, and which of them the right eye owns.
    void SetTargets(UINT n, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv,
                    ID3D11RenderTargetView** l, ID3D11RenderTargetView** r)
    {
        for (UINT i = 0; i < kMaxRTVs; ++i) m_rtvRight[i] = false;
        for (UINT i = 0; rtvs && i < n; ++i)
        {
            l[i] = ViewFor(rtvs[i], false);
            r[i] = ViewFor(rtvs[i], true);
            m_rtvRight[i] = r[i] != l[i];
        }
        m_dsvRight = ViewFor(dsv, true) != ViewFor(dsv, false);
    }
    void UpdateOutputs()
    {
        m_omRight = m_dsvRight || m_soRight;
        for (UINT i = 0; i < kMaxRTVs; ++i) if (m_rtvRight[i]) m_omRight = true;
        for (UINT i = 0; i < kMaxUAVs; ++i) if (m_omUavRight[i]) m_omRight = true;
    }
    void ResetOutputs()
    {
        for (UINT i = 0; i < kMaxRTVs; ++i) m_rtvRight[i] = false;
        for (UINT i = 0; i < kMaxUAVs; ++i) m_omUavRight[i] = m_csUavRight[i] = false;
        m_dsvRight = m_soRight = m_omRight = m_csRight = false;
    }

    // A draw writing only shared outputs stays in the game's list: the twin drawing into them with right-eye inputs
    // would leave the right eye's pixels where the left reads them.
    bool RightDraw()
    {
        if (!m_right || !m_omRight) return false;
        m_rightWork = true;
        return true;
    }
    bool RightDispatch()
    {
        if (!m_right || !m_csRight) return false;
        m_rightWork = true;
        return true;
    }

    // A written resource: a right copy written makes the right half worth running; a written buffer's bytes change for
    // the immediate context when the list runs.
    void NoteWrite(ID3D11Resource* dst)
    {
        if (!dst) return;
        if (m_right && HasRight(dst))
        {
            m_rightWork = true;
            if (Texture2D11Proxy* t = TryUnwrapTexture2D(dst)) t->MarkRightWritten();
        }
        if (Buffer11Proxy* b = TryUnwrapBuffer(dst)) NoteBuffer(b);
    }
    void NoteBuffer(Buffer11Proxy* b)
    {
        if (!m_writtenSet.insert(b).second) return;
        b->AddRef();
        m_written.push_back(b);
    }

    // The game wrote the left mapping; the twin maps its own (the right copy, or the shared resource renamed in its
    // list) and takes the same bytes.
    void CopyMapToRight(const OpenMap& om)
    {
        ID3D11Resource* l = LeftRes(om.res);
        ID3D11Resource* r = RightRes(om.res);
        D3D11_MAPPED_SUBRESOURCE rm = {};
        if (FAILED(m_right->Map(r, om.sub, om.type, 0, &rm)) || !rm.pData || !om.m.pData)
        {
            InterlockedIncrement(&s_rightMapFailed);
            return;
        }
        UINT rows = 1, slices = 1, bufferBytes = 0;
        MapExtent(l, om.sub, rows, slices, bufferBytes);
        if (bufferBytes) memcpy(rm.pData, om.m.pData, bufferBytes);
        else
        {
            const UINT row = om.m.RowPitch < rm.RowPitch ? om.m.RowPitch : rm.RowPitch;
            for (UINT z = 0; z < slices; ++z)
                for (UINT y = 0; y < rows; ++y)
                    memcpy(static_cast<BYTE*>(rm.pData) + (size_t)z * rm.DepthPitch + (size_t)y * rm.RowPitch,
                           static_cast<const BYTE*>(om.m.pData) + (size_t)z * om.m.DepthPitch + (size_t)y * om.m.RowPitch, row);
        }
        m_right->Unmap(r, om.sub);
    }

    volatile LONG          m_refs = 1;
    ID3D11DeviceContext*   m_left;
    ID3D11DeviceContext1*  m_left1 = nullptr;
    ID3D11DeviceContext2*  m_left2 = nullptr;
    ID3D11DeviceContext3*  m_left3 = nullptr;
    ID3D11DeviceContext*   m_right;   // null: the twin could not be created, and the pair forwards alone
    ID3D11DeviceContext1*  m_right1 = nullptr;
    ID3D11DeviceContext2*  m_right2 = nullptr;
    ID3D11DeviceContext3*  m_right3 = nullptr;
    Device11Proxy*         m_device;
    bool                   m_rightWork = false;
    std::vector<Buffer11Proxy*>        m_written;   // one reference each, handed to the finished pair
    std::unordered_set<Buffer11Proxy*> m_writtenSet;
    std::vector<OpenMap>   m_maps;
    bool m_rtvRight[kMaxRTVs] = {};
    bool m_omUavRight[kMaxUAVs] = {};
    bool m_csUavRight[kMaxUAVs] = {};
    bool m_dsvRight = false, m_soRight = false, m_omRight = false, m_csRight = false;
};

#undef PAIR_STAGE
#undef PAIR_BOTH

} // namespace

bool DeferredContextsOn()
{
    if (!gInfo.DuplicateDraws) return false;
    const auto it = g_ProfileData.FeatureEntries.find("DeferredContexts");
    return it != g_ProfileData.FeatureEntries.end() && EntryValue(it->second, "Value") != "0";
}

// Off: the call the game made, on the real device's interface of that version.
static HRESULT CreateRealDeferredContext(ID3D11Device* real, UINT flags, REFIID riid, void** ppContext)
{
    if (riid == __uuidof(ID3D11DeviceContext))
        return real->CreateDeferredContext(flags, reinterpret_cast<ID3D11DeviceContext**>(ppContext));
    HRESULT hr = E_NOINTERFACE;
    if (riid == __uuidof(ID3D11DeviceContext1))
    {
        ID3D11Device1* d = nullptr;
        if (SUCCEEDED(real->QueryInterface(__uuidof(ID3D11Device1), reinterpret_cast<void**>(&d))) && d)
        { hr = d->CreateDeferredContext1(flags, reinterpret_cast<ID3D11DeviceContext1**>(ppContext)); d->Release(); }
    }
    else if (riid == __uuidof(ID3D11DeviceContext2))
    {
        ID3D11Device2* d = nullptr;
        if (SUCCEEDED(real->QueryInterface(__uuidof(ID3D11Device2), reinterpret_cast<void**>(&d))) && d)
        { hr = d->CreateDeferredContext2(flags, reinterpret_cast<ID3D11DeviceContext2**>(ppContext)); d->Release(); }
    }
    else if (riid == __uuidof(ID3D11DeviceContext3))
    {
        ID3D11Device3* d = nullptr;
        if (SUCCEEDED(real->QueryInterface(__uuidof(ID3D11Device3), reinterpret_cast<void**>(&d))) && d)
        { hr = d->CreateDeferredContext3(flags, reinterpret_cast<ID3D11DeviceContext3**>(ppContext)); d->Release(); }
    }
    return hr;
}

HRESULT CreateDeferredContextFor(Device11Proxy* device, UINT flags, REFIID riid, void** ppContext)
{
    ID3D11Device* real = device->GetReal();
    if (!DeferredContextsOn() || !ppContext) return CreateRealDeferredContext(real, flags, riid, ppContext);
    *ppContext = nullptr;
    ID3D11DeviceContext* left = nullptr;
    HRESULT hr = real->CreateDeferredContext(flags, &left);
    if (FAILED(hr) || !left) return hr;
    ID3D11DeviceContext* right = nullptr;
    if (FAILED(real->CreateDeferredContext(flags, &right)) || !right)
    {
        right = nullptr;
        InterlockedIncrement(&s_rightFailed);
    }
    DeferredPair11* pair = new DeferredPair11(left, right, device);
    const LONG n = InterlockedIncrement(&s_pairs);
    if (n <= 16)
        FeatureLog("  deferred context #%ld (flags 0x%X) records a right-eye twin: left %p, right %p\n", n, flags,
                   static_cast<void*>(left), static_cast<void*>(right));
    hr = pair->QueryInterface(riid, ppContext);
    pair->Release();
    return hr;
}

void ExecuteCommandListPair(ID3D11DeviceContext* real, ID3D11CommandList* list, BOOL restore)
{
    CommandListPair11* p = DeferredContextsOn() ? TryUnwrapPair(list) : nullptr;
    if (!p) { real->ExecuteCommandList(list, restore); return; }
    InterlockedIncrement(&s_executed);
    real->ExecuteCommandList(p->Left(), restore);
    if (p->Right()) real->ExecuteCommandList(p->Right(), restore);
}

void ExecuteCommandListRight(ID3D11DeviceContext* real, ID3D11CommandList* list, BOOL restore)
{
    CommandListPair11* p = DeferredContextsOn() ? TryUnwrapPair(list) : nullptr;
    real->ExecuteCommandList(p ? (p->Right() ? p->Right() : p->Left()) : list, restore);
}

const std::vector<Buffer11Proxy*>& CommandListWrittenBuffers(ID3D11CommandList* list)
{
    static const std::vector<Buffer11Proxy*> none;
    CommandListPair11* p = DeferredContextsOn() ? TryUnwrapPair(list) : nullptr;
    return p ? p->Written() : none;
}

DeferredStats TakeDeferredStats()
{
    DeferredStats s;
    s.pairs           = s_pairs;
    s.rightFailed     = s_rightFailed;
    s.finished        = InterlockedExchange(&s_finished, 0);
    s.rightKept       = InterlockedExchange(&s_rightKept, 0);
    s.rightDropped    = InterlockedExchange(&s_rightDropped, 0);
    s.executed        = InterlockedExchange(&s_executed, 0);
    s.rightMapFailed  = InterlockedExchange(&s_rightMapFailed, 0);
    return s;
}

} // namespace wiz3d
