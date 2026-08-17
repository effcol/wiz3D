/* NvDirectMode - ID3D11DeviceContext proxy
 *
 * Stage 1b-i: passthrough wrapper. Stage 1b-iv hooks OMSetRenderTargets +
 * OMSetRenderTargetsAndUnorderedAccessViews to redirect viewport per active
 * eye when a wrapped backbuffer RTV is bound at slot 0.
 */

#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9types.h>   // D3DCOLORVALUE for the DXGI_RGBA shim below
#ifndef _DXGI_RGBA_DEFINED
#define _DXGI_RGBA_DEFINED
typedef D3DCOLORVALUE DXGI_RGBA;     // bundled lib/d3d10 dxgitype.h still
                                     // shadows the SDK header for DXGI_RGBA.
#endif
#include <d3d11_3.h>  // ID3D11DeviceContext3 (inherits Context2/Context1/Context)
#include <functional>
#include <vector>
#include <string>
#include <map>
#include <algorithm>

namespace wiz3d
{

// Stage 4b.4: RAII wrapper for COM pointers captured inside frame-recording
// lambdas. The lambda needs to keep the pointer alive across the frame even
// if the game releases its own ref, AND std::function requires the captured
// state to be copy-constructible (so we Release-on-destruct rather than
// move-only). Copy-ctor AddRefs, dtor Releases, so a std::vector<ComRefHolder>
// can be captured by value into a lambda and the ref count stays balanced.
struct ComRefHolder
{
    IUnknown* p;

    ComRefHolder() : p(nullptr) {}
    explicit ComRefHolder(IUnknown* x) : p(x) { if (p) p->AddRef(); }
    ComRefHolder(const ComRefHolder& o) : p(o.p) { if (p) p->AddRef(); }
    ComRefHolder(ComRefHolder&& o) noexcept : p(o.p) { o.p = nullptr; }
    ComRefHolder& operator=(const ComRefHolder& o)
    {
        if (this != &o)
        {
            if (p) p->Release();
            p = o.p;
            if (p) p->AddRef();
        }
        return *this;
    }
    ComRefHolder& operator=(ComRefHolder&& o) noexcept
    {
        if (this != &o)
        {
            if (p) p->Release();
            p = o.p;
            o.p = nullptr;
        }
        return *this;
    }
    ~ComRefHolder() { if (p) p->Release(); }
};

class Device11Proxy;

class Context11Proxy : public ID3D11DeviceContext3
{
public:
    // D3D11 spec maxima. Undersizing makes the runtime read past our unwrap
    // arrays into stack garbage and AV (Metro 2033 / MP3 / De Blob, May 2026).
    static constexpr UINT kMaxSRVs      = 128; // D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT
    static constexpr UINT kMaxSamplers  = 16;  // D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT
    static constexpr UINT kMaxCBs       = 15;  // D3D11_1_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT
    static constexpr UINT kMaxRTVs      = 8;   // D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT
    static constexpr UINT kMaxUAVs      = 64;  // D3D11_1_UAV_SLOT_COUNT
    static constexpr UINT kMaxVBs       = 32;  // D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT
    static constexpr UINT kMaxSOBuffers = 4;   // D3D11_SO_BUFFER_SLOT_COUNT
    static constexpr UINT kMaxClassInst = 253; // D3D11_SHADER_MAX_INTERFACES

    Context11Proxy(ID3D11DeviceContext* real, Device11Proxy* parent);
    virtual ~Context11Proxy();

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObj) override;
    ULONG   STDMETHODCALLTYPE AddRef() override                                  { return InterlockedIncrement(&m_refs); }
    ULONG   STDMETHODCALLTYPE Release() override
    {
        LONG r = InterlockedDecrement(&m_refs);
        if (r == 0) { if (m_real) { m_real->Release(); m_real = nullptr; } delete this; }
        return (ULONG)r;
    }

    // ID3D11DeviceChild
    void    STDMETHODCALLTYPE GetDevice(ID3D11Device** ppDevice) override;  // returns wrapped parent
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT* pDataSize, void* pData) override         { return m_real->GetPrivateData(guid, pDataSize, pData); }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT DataSize, const void* pData) override     { return m_real->SetPrivateData(guid, DataSize, pData); }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown* pData) override       { return m_real->SetPrivateDataInterface(guid, pData); }

    // ID3D11DeviceContext — input assembler / shader stages / rasteriser / output merger / draws
    void    STDMETHODCALLTYPE VSSetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer* const* ppConstantBuffers) override;
    void    STDMETHODCALLTYPE PSSetShaderResources(UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView* const* ppShaderResourceViews) override;
    void    STDMETHODCALLTYPE PSSetShader(ID3D11PixelShader* pPixelShader, ID3D11ClassInstance* const* ppClassInstances, UINT NumClassInstances) override;
    void    STDMETHODCALLTYPE PSSetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState* const* ppSamplers) override;
    void    STDMETHODCALLTYPE VSSetShader(ID3D11VertexShader* pVertexShader, ID3D11ClassInstance* const* ppClassInstances, UINT NumClassInstances) override;
    void    STDMETHODCALLTYPE DrawIndexed(UINT IndexCount, UINT StartIndexLocation, INT BaseVertexLocation) override;
    void    STDMETHODCALLTYPE Draw(UINT VertexCount, UINT StartVertexLocation) override;
    HRESULT STDMETHODCALLTYPE Map(ID3D11Resource* pResource, UINT Subresource, D3D11_MAP MapType, UINT MapFlags, D3D11_MAPPED_SUBRESOURCE* pMappedResource) override;
    void    STDMETHODCALLTYPE Unmap(ID3D11Resource* pResource, UINT Subresource) override;
    void    STDMETHODCALLTYPE PSSetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer* const* ppConstantBuffers) override;
    void    STDMETHODCALLTYPE IASetInputLayout(ID3D11InputLayout* pInputLayout) override;
    void    STDMETHODCALLTYPE IASetVertexBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer* const* ppVertexBuffers, const UINT* pStrides, const UINT* pOffsets) override;
    void    STDMETHODCALLTYPE IASetIndexBuffer(ID3D11Buffer* pIndexBuffer, DXGI_FORMAT Format, UINT Offset) override;
    void    STDMETHODCALLTYPE DrawIndexedInstanced(UINT IndexCountPerInstance, UINT InstanceCount, UINT StartIndexLocation, INT BaseVertexLocation, UINT StartInstanceLocation) override;
    void    STDMETHODCALLTYPE DrawInstanced(UINT VertexCountPerInstance, UINT InstanceCount, UINT StartVertexLocation, UINT StartInstanceLocation) override;
    void    STDMETHODCALLTYPE GSSetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer* const* ppConstantBuffers) override;
    void    STDMETHODCALLTYPE GSSetShader(ID3D11GeometryShader* pShader, ID3D11ClassInstance* const* ppClassInstances, UINT NumClassInstances) override;
    void    STDMETHODCALLTYPE IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY Topology) override;
    void    STDMETHODCALLTYPE VSSetShaderResources(UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView* const* ppShaderResourceViews) override;
    void    STDMETHODCALLTYPE VSSetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState* const* ppSamplers) override;
    void STDMETHODCALLTYPE Begin(ID3D11Asynchronous* pAsync) override                                                                                                                              { m_real->Begin(pAsync); }
    void STDMETHODCALLTYPE End(ID3D11Asynchronous* pAsync) override                                                                                                                                { m_real->End(pAsync); }
    HRESULT STDMETHODCALLTYPE GetData(ID3D11Asynchronous* pAsync, void* pData, UINT DataSize, UINT GetDataFlags) override                                                                          { return m_real->GetData(pAsync, pData, DataSize, GetDataFlags); }
    void    STDMETHODCALLTYPE SetPredication(ID3D11Predicate* pPredicate, BOOL PredicateValue) override;
    void    STDMETHODCALLTYPE GSSetShaderResources(UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView* const* ppShaderResourceViews) override;
    void    STDMETHODCALLTYPE GSSetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState* const* ppSamplers) override;
    void STDMETHODCALLTYPE OMSetRenderTargets(UINT NumViews, ID3D11RenderTargetView* const* ppRenderTargetViews, ID3D11DepthStencilView* pDepthStencilView) override;
    void STDMETHODCALLTYPE OMSetRenderTargetsAndUnorderedAccessViews(UINT NumRTVs, ID3D11RenderTargetView* const* ppRenderTargetViews, ID3D11DepthStencilView* pDepthStencilView, UINT UAVStartSlot, UINT NumUAVs, ID3D11UnorderedAccessView* const* ppUnorderedAccessViews, const UINT* pUAVInitialCounts) override;
    void    STDMETHODCALLTYPE OMSetBlendState(ID3D11BlendState* pBlendState, const FLOAT BlendFactor[4], UINT SampleMask) override;
    void    STDMETHODCALLTYPE OMSetDepthStencilState(ID3D11DepthStencilState* pDepthStencilState, UINT StencilRef) override;
    void    STDMETHODCALLTYPE SOSetTargets(UINT NumBuffers, ID3D11Buffer* const* ppSOTargets, const UINT* pOffsets) override;
    void    STDMETHODCALLTYPE DrawAuto() override;
    void    STDMETHODCALLTYPE DrawIndexedInstancedIndirect(ID3D11Buffer* pBufferForArgs, UINT AlignedByteOffsetForArgs) override;
    void    STDMETHODCALLTYPE DrawInstancedIndirect(ID3D11Buffer* pBufferForArgs, UINT AlignedByteOffsetForArgs) override;
    void    STDMETHODCALLTYPE Dispatch(UINT ThreadGroupCountX, UINT ThreadGroupCountY, UINT ThreadGroupCountZ) override;
    void    STDMETHODCALLTYPE DispatchIndirect(ID3D11Buffer* pBufferForArgs, UINT AlignedByteOffsetForArgs) override;
    void    STDMETHODCALLTYPE RSSetState(ID3D11RasterizerState* pRasterizerState) override;
    void    STDMETHODCALLTYPE RSSetViewports(UINT NumViewports, const D3D11_VIEWPORT* pViewports) override;
    void    STDMETHODCALLTYPE RSSetScissorRects(UINT NumRects, const D3D11_RECT* pRects) override;
    // Intercepted out-of-line for the NV magic-header SBS capture path.
    void STDMETHODCALLTYPE CopySubresourceRegion(ID3D11Resource* pDstResource, UINT DstSubresource, UINT DstX, UINT DstY, UINT DstZ, ID3D11Resource* pSrcResource, UINT SrcSubresource, const D3D11_BOX* pSrcBox) override;
    void STDMETHODCALLTYPE CopyResource(ID3D11Resource* pDstResource, ID3D11Resource* pSrcResource) override;
    void    STDMETHODCALLTYPE UpdateSubresource(ID3D11Resource* pDstResource, UINT DstSubresource, const D3D11_BOX* pDstBox, const void* pSrcData, UINT SrcRowPitch, UINT SrcDepthPitch) override;
    void    STDMETHODCALLTYPE CopyStructureCount(ID3D11Buffer* pDstBuffer, UINT DstAlignedByteOffset, ID3D11UnorderedAccessView* pSrcView) override;
    void    STDMETHODCALLTYPE ClearRenderTargetView(ID3D11RenderTargetView* pRenderTargetView, const FLOAT ColorRGBA[4]) override;
    void STDMETHODCALLTYPE ClearUnorderedAccessViewUint(ID3D11UnorderedAccessView* pUnorderedAccessView, const UINT Values[4]) override;
    void STDMETHODCALLTYPE ClearUnorderedAccessViewFloat(ID3D11UnorderedAccessView* pUnorderedAccessView, const FLOAT Values[4]) override;
    void    STDMETHODCALLTYPE ClearDepthStencilView(ID3D11DepthStencilView* pDepthStencilView, UINT ClearFlags, FLOAT Depth, UINT8 Stencil) override;
    void STDMETHODCALLTYPE GenerateMips(ID3D11ShaderResourceView* pShaderResourceView) override;
    void STDMETHODCALLTYPE SetResourceMinLOD(ID3D11Resource* pResource, FLOAT MinLOD) override                                                                                                     { m_real->SetResourceMinLOD(pResource, MinLOD); }
    FLOAT STDMETHODCALLTYPE GetResourceMinLOD(ID3D11Resource* pResource) override                                                                                                                  { return m_real->GetResourceMinLOD(pResource); }
    void    STDMETHODCALLTYPE ResolveSubresource(ID3D11Resource* pDstResource, UINT DstSubresource, ID3D11Resource* pSrcResource, UINT SrcSubresource, DXGI_FORMAT Format) override;
    void STDMETHODCALLTYPE ExecuteCommandList(ID3D11CommandList* pCommandList, BOOL RestoreContextState) override;
    void    STDMETHODCALLTYPE HSSetShaderResources(UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView* const* ppShaderResourceViews) override;
    void    STDMETHODCALLTYPE HSSetShader(ID3D11HullShader* pHullShader, ID3D11ClassInstance* const* ppClassInstances, UINT NumClassInstances) override;
    void    STDMETHODCALLTYPE HSSetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState* const* ppSamplers) override;
    void    STDMETHODCALLTYPE HSSetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer* const* ppConstantBuffers) override;
    void    STDMETHODCALLTYPE DSSetShaderResources(UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView* const* ppShaderResourceViews) override;
    void    STDMETHODCALLTYPE DSSetShader(ID3D11DomainShader* pDomainShader, ID3D11ClassInstance* const* ppClassInstances, UINT NumClassInstances) override;
    void    STDMETHODCALLTYPE DSSetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState* const* ppSamplers) override;
    void    STDMETHODCALLTYPE DSSetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer* const* ppConstantBuffers) override;
    void    STDMETHODCALLTYPE CSSetShaderResources(UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView* const* ppShaderResourceViews) override;
    void    STDMETHODCALLTYPE CSSetUnorderedAccessViews(UINT StartSlot, UINT NumUAVs, ID3D11UnorderedAccessView* const* ppUnorderedAccessViews, const UINT* pUAVInitialCounts) override;
    void    STDMETHODCALLTYPE CSSetShader(ID3D11ComputeShader* pComputeShader, ID3D11ClassInstance* const* ppClassInstances, UINT NumClassInstances) override;
    void    STDMETHODCALLTYPE CSSetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState* const* ppSamplers) override;
    void    STDMETHODCALLTYPE CSSetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer* const* ppConstantBuffers) override;
    void STDMETHODCALLTYPE VSGetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer** ppConstantBuffers) override                                                                        { m_real->VSGetConstantBuffers(StartSlot, NumBuffers, ppConstantBuffers); }
    void STDMETHODCALLTYPE PSGetShaderResources(UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView** ppShaderResourceViews) override                                                          { m_real->PSGetShaderResources(StartSlot, NumViews, ppShaderResourceViews); }
    void STDMETHODCALLTYPE PSGetShader(ID3D11PixelShader** ppPixelShader, ID3D11ClassInstance** ppClassInstances, UINT* pNumClassInstances) override                                               { m_real->PSGetShader(ppPixelShader, ppClassInstances, pNumClassInstances); }
    void STDMETHODCALLTYPE PSGetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState** ppSamplers) override                                                                               { m_real->PSGetSamplers(StartSlot, NumSamplers, ppSamplers); }
    void STDMETHODCALLTYPE VSGetShader(ID3D11VertexShader** ppVertexShader, ID3D11ClassInstance** ppClassInstances, UINT* pNumClassInstances) override                                             { m_real->VSGetShader(ppVertexShader, ppClassInstances, pNumClassInstances); }
    void STDMETHODCALLTYPE PSGetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer** ppConstantBuffers) override                                                                        { m_real->PSGetConstantBuffers(StartSlot, NumBuffers, ppConstantBuffers); }
    void STDMETHODCALLTYPE IAGetInputLayout(ID3D11InputLayout** ppInputLayout) override                                                                                                            { m_real->IAGetInputLayout(ppInputLayout); }
    void STDMETHODCALLTYPE IAGetVertexBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer** ppVertexBuffers, UINT* pStrides, UINT* pOffsets) override                                            { m_real->IAGetVertexBuffers(StartSlot, NumBuffers, ppVertexBuffers, pStrides, pOffsets); }
    void STDMETHODCALLTYPE IAGetIndexBuffer(ID3D11Buffer** pIndexBuffer, DXGI_FORMAT* Format, UINT* Offset) override                                                                               { m_real->IAGetIndexBuffer(pIndexBuffer, Format, Offset); }
    void STDMETHODCALLTYPE GSGetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer** ppConstantBuffers) override                                                                        { m_real->GSGetConstantBuffers(StartSlot, NumBuffers, ppConstantBuffers); }
    void STDMETHODCALLTYPE GSGetShader(ID3D11GeometryShader** ppGeometryShader, ID3D11ClassInstance** ppClassInstances, UINT* pNumClassInstances) override                                         { m_real->GSGetShader(ppGeometryShader, ppClassInstances, pNumClassInstances); }
    void STDMETHODCALLTYPE IAGetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY* pTopology) override                                                                                                    { m_real->IAGetPrimitiveTopology(pTopology); }
    void STDMETHODCALLTYPE VSGetShaderResources(UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView** ppShaderResourceViews) override                                                          { m_real->VSGetShaderResources(StartSlot, NumViews, ppShaderResourceViews); }
    void STDMETHODCALLTYPE VSGetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState** ppSamplers) override                                                                               { m_real->VSGetSamplers(StartSlot, NumSamplers, ppSamplers); }
    void STDMETHODCALLTYPE GetPredication(ID3D11Predicate** ppPredicate, BOOL* pPredicateValue) override                                                                                           { m_real->GetPredication(ppPredicate, pPredicateValue); }
    void STDMETHODCALLTYPE GSGetShaderResources(UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView** ppShaderResourceViews) override                                                          { m_real->GSGetShaderResources(StartSlot, NumViews, ppShaderResourceViews); }
    void STDMETHODCALLTYPE GSGetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState** ppSamplers) override                                                                               { m_real->GSGetSamplers(StartSlot, NumSamplers, ppSamplers); }
    // Must hand back the proxies the game gave us, not m_real's views: games
    // save/restore OM state, and a leaked real RTV rebinds as untrackable.
    void STDMETHODCALLTYPE OMGetRenderTargets(UINT NumViews, ID3D11RenderTargetView** ppRenderTargetViews, ID3D11DepthStencilView** ppDepthStencilView) override;
    void STDMETHODCALLTYPE OMGetRenderTargetsAndUnorderedAccessViews(UINT NumRTVs, ID3D11RenderTargetView** ppRenderTargetViews, ID3D11DepthStencilView** ppDepthStencilView, UINT UAVStartSlot, UINT NumUAVs, ID3D11UnorderedAccessView** ppUnorderedAccessViews) override;
    void STDMETHODCALLTYPE OMGetBlendState(ID3D11BlendState** ppBlendState, FLOAT BlendFactor[4], UINT* pSampleMask) override                                                                      { m_real->OMGetBlendState(ppBlendState, BlendFactor, pSampleMask); }
    void STDMETHODCALLTYPE OMGetDepthStencilState(ID3D11DepthStencilState** ppDepthStencilState, UINT* pStencilRef) override                                                                       { m_real->OMGetDepthStencilState(ppDepthStencilState, pStencilRef); }
    void STDMETHODCALLTYPE SOGetTargets(UINT NumBuffers, ID3D11Buffer** ppSOTargets) override                                                                                                      { m_real->SOGetTargets(NumBuffers, ppSOTargets); }
    void STDMETHODCALLTYPE RSGetState(ID3D11RasterizerState** ppRasterizerState) override                                                                                                          { m_real->RSGetState(ppRasterizerState); }
    void STDMETHODCALLTYPE RSGetViewports(UINT* pNumViewports, D3D11_VIEWPORT* pViewports) override                                                                                                { m_real->RSGetViewports(pNumViewports, pViewports); }
    void STDMETHODCALLTYPE RSGetScissorRects(UINT* pNumRects, D3D11_RECT* pRects) override                                                                                                         { m_real->RSGetScissorRects(pNumRects, pRects); }
    void STDMETHODCALLTYPE HSGetShaderResources(UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView** ppShaderResourceViews) override                                                          { m_real->HSGetShaderResources(StartSlot, NumViews, ppShaderResourceViews); }
    void STDMETHODCALLTYPE HSGetShader(ID3D11HullShader** ppHullShader, ID3D11ClassInstance** ppClassInstances, UINT* pNumClassInstances) override                                                 { m_real->HSGetShader(ppHullShader, ppClassInstances, pNumClassInstances); }
    void STDMETHODCALLTYPE HSGetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState** ppSamplers) override                                                                               { m_real->HSGetSamplers(StartSlot, NumSamplers, ppSamplers); }
    void STDMETHODCALLTYPE HSGetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer** ppConstantBuffers) override                                                                        { m_real->HSGetConstantBuffers(StartSlot, NumBuffers, ppConstantBuffers); }
    void STDMETHODCALLTYPE DSGetShaderResources(UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView** ppShaderResourceViews) override                                                          { m_real->DSGetShaderResources(StartSlot, NumViews, ppShaderResourceViews); }
    void STDMETHODCALLTYPE DSGetShader(ID3D11DomainShader** ppDomainShader, ID3D11ClassInstance** ppClassInstances, UINT* pNumClassInstances) override                                             { m_real->DSGetShader(ppDomainShader, ppClassInstances, pNumClassInstances); }
    void STDMETHODCALLTYPE DSGetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState** ppSamplers) override                                                                               { m_real->DSGetSamplers(StartSlot, NumSamplers, ppSamplers); }
    void STDMETHODCALLTYPE DSGetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer** ppConstantBuffers) override                                                                        { m_real->DSGetConstantBuffers(StartSlot, NumBuffers, ppConstantBuffers); }
    void STDMETHODCALLTYPE CSGetShaderResources(UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView** ppShaderResourceViews) override                                                          { m_real->CSGetShaderResources(StartSlot, NumViews, ppShaderResourceViews); }
    void STDMETHODCALLTYPE CSGetUnorderedAccessViews(UINT StartSlot, UINT NumUAVs, ID3D11UnorderedAccessView** ppUnorderedAccessViews) override                                                    { m_real->CSGetUnorderedAccessViews(StartSlot, NumUAVs, ppUnorderedAccessViews); }
    void STDMETHODCALLTYPE CSGetShader(ID3D11ComputeShader** ppComputeShader, ID3D11ClassInstance** ppClassInstances, UINT* pNumClassInstances) override                                           { m_real->CSGetShader(ppComputeShader, ppClassInstances, pNumClassInstances); }
    void STDMETHODCALLTYPE CSGetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState** ppSamplers) override                                                                               { m_real->CSGetSamplers(StartSlot, NumSamplers, ppSamplers); }
    void STDMETHODCALLTYPE CSGetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer** ppConstantBuffers) override                                                                        { m_real->CSGetConstantBuffers(StartSlot, NumBuffers, ppConstantBuffers); }
    void STDMETHODCALLTYPE ClearState() override;
    void STDMETHODCALLTYPE Flush() override                                                                                                                                                        { m_real->Flush(); }
    D3D11_DEVICE_CONTEXT_TYPE STDMETHODCALLTYPE GetType() override                                                                                                                                 { return m_real->GetType(); }
    UINT STDMETHODCALLTYPE GetContextFlags() override                                                                                                                                              { return m_real->GetContextFlags(); }
    HRESULT STDMETHODCALLTYPE FinishCommandList(BOOL RestoreDeferredContextState, ID3D11CommandList** ppCommandList) override                                                                      { return m_real->FinishCommandList(RestoreDeferredContextState, ppCommandList); }

    // ----- ID3D11DeviceContext1 — claim Context1 in QI with `this` so games
    // accessing the immediate context via Device1::GetImmediateContext1 land
    // on our proxy, not the unwrapped real Context1. New methods passthrough
    // through m_real1 — none have per-eye logic yet, so games using these
    // (ClearView, DiscardView, *SetConstantBuffers1) will skip per-eye state
    // for those calls. Future stage can fill in record-for-replay.
    void STDMETHODCALLTYPE CopySubresourceRegion1(ID3D11Resource* pDstResource, UINT DstSubresource, UINT DstX, UINT DstY, UINT DstZ, ID3D11Resource* pSrcResource, UINT SrcSubresource, const D3D11_BOX* pSrcBox, UINT CopyFlags) override { if (m_real1) m_real1->CopySubresourceRegion1(pDstResource, DstSubresource, DstX, DstY, DstZ, pSrcResource, SrcSubresource, pSrcBox, CopyFlags); }
    void STDMETHODCALLTYPE UpdateSubresource1(ID3D11Resource* pDstResource, UINT DstSubresource, const D3D11_BOX* pDstBox, const void* pSrcData, UINT SrcRowPitch, UINT SrcDepthPitch, UINT CopyFlags) override { if (m_real1) m_real1->UpdateSubresource1(pDstResource, DstSubresource, pDstBox, pSrcData, SrcRowPitch, SrcDepthPitch, CopyFlags); }
    void STDMETHODCALLTYPE DiscardResource(ID3D11Resource* pResource) override                                                                                                                    { if (m_real1) m_real1->DiscardResource(pResource); }
    void STDMETHODCALLTYPE DiscardView(ID3D11View* pResourceView) override                                                                                                                        { if (m_real1) m_real1->DiscardView(pResourceView); }
    // Same clobber risk as VSSetConstantBuffers — a Device1+ game reaching the
    // modified shader's slot through this entry point must not unbind us.
    void STDMETHODCALLTYPE VSSetConstantBuffers1(UINT StartSlot, UINT NumBuffers, ID3D11Buffer* const* ppConstantBuffers, const UINT* pFirstConstant, const UINT* pNumConstants) override         { if (m_real1) m_real1->VSSetConstantBuffers1(StartSlot, NumBuffers, ppConstantBuffers, pFirstConstant, pNumConstants); if (m_modVSShader && m_modVSCBIndex >= StartSlot && m_modVSCBIndex < StartSlot + NumBuffers) BindStereoShiftCB(m_activeEye == Eye::Right); }
    void STDMETHODCALLTYPE HSSetConstantBuffers1(UINT StartSlot, UINT NumBuffers, ID3D11Buffer* const* ppConstantBuffers, const UINT* pFirstConstant, const UINT* pNumConstants) override         { if (m_real1) m_real1->HSSetConstantBuffers1(StartSlot, NumBuffers, ppConstantBuffers, pFirstConstant, pNumConstants); }
    void STDMETHODCALLTYPE DSSetConstantBuffers1(UINT StartSlot, UINT NumBuffers, ID3D11Buffer* const* ppConstantBuffers, const UINT* pFirstConstant, const UINT* pNumConstants) override         { if (m_real1) m_real1->DSSetConstantBuffers1(StartSlot, NumBuffers, ppConstantBuffers, pFirstConstant, pNumConstants); }
    void STDMETHODCALLTYPE GSSetConstantBuffers1(UINT StartSlot, UINT NumBuffers, ID3D11Buffer* const* ppConstantBuffers, const UINT* pFirstConstant, const UINT* pNumConstants) override         { if (m_real1) m_real1->GSSetConstantBuffers1(StartSlot, NumBuffers, ppConstantBuffers, pFirstConstant, pNumConstants); }
    void STDMETHODCALLTYPE PSSetConstantBuffers1(UINT StartSlot, UINT NumBuffers, ID3D11Buffer* const* ppConstantBuffers, const UINT* pFirstConstant, const UINT* pNumConstants) override         { if (m_real1) m_real1->PSSetConstantBuffers1(StartSlot, NumBuffers, ppConstantBuffers, pFirstConstant, pNumConstants); }
    void STDMETHODCALLTYPE CSSetConstantBuffers1(UINT StartSlot, UINT NumBuffers, ID3D11Buffer* const* ppConstantBuffers, const UINT* pFirstConstant, const UINT* pNumConstants) override         { if (m_real1) m_real1->CSSetConstantBuffers1(StartSlot, NumBuffers, ppConstantBuffers, pFirstConstant, pNumConstants); }
    void STDMETHODCALLTYPE VSGetConstantBuffers1(UINT StartSlot, UINT NumBuffers, ID3D11Buffer** ppConstantBuffers, UINT* pFirstConstant, UINT* pNumConstants) override                           { if (m_real1) m_real1->VSGetConstantBuffers1(StartSlot, NumBuffers, ppConstantBuffers, pFirstConstant, pNumConstants); }
    void STDMETHODCALLTYPE HSGetConstantBuffers1(UINT StartSlot, UINT NumBuffers, ID3D11Buffer** ppConstantBuffers, UINT* pFirstConstant, UINT* pNumConstants) override                           { if (m_real1) m_real1->HSGetConstantBuffers1(StartSlot, NumBuffers, ppConstantBuffers, pFirstConstant, pNumConstants); }
    void STDMETHODCALLTYPE DSGetConstantBuffers1(UINT StartSlot, UINT NumBuffers, ID3D11Buffer** ppConstantBuffers, UINT* pFirstConstant, UINT* pNumConstants) override                           { if (m_real1) m_real1->DSGetConstantBuffers1(StartSlot, NumBuffers, ppConstantBuffers, pFirstConstant, pNumConstants); }
    void STDMETHODCALLTYPE GSGetConstantBuffers1(UINT StartSlot, UINT NumBuffers, ID3D11Buffer** ppConstantBuffers, UINT* pFirstConstant, UINT* pNumConstants) override                           { if (m_real1) m_real1->GSGetConstantBuffers1(StartSlot, NumBuffers, ppConstantBuffers, pFirstConstant, pNumConstants); }
    void STDMETHODCALLTYPE PSGetConstantBuffers1(UINT StartSlot, UINT NumBuffers, ID3D11Buffer** ppConstantBuffers, UINT* pFirstConstant, UINT* pNumConstants) override                           { if (m_real1) m_real1->PSGetConstantBuffers1(StartSlot, NumBuffers, ppConstantBuffers, pFirstConstant, pNumConstants); }
    void STDMETHODCALLTYPE CSGetConstantBuffers1(UINT StartSlot, UINT NumBuffers, ID3D11Buffer** ppConstantBuffers, UINT* pFirstConstant, UINT* pNumConstants) override                           { if (m_real1) m_real1->CSGetConstantBuffers1(StartSlot, NumBuffers, ppConstantBuffers, pFirstConstant, pNumConstants); }
    void STDMETHODCALLTYPE SwapDeviceContextState(ID3DDeviceContextState* pState, ID3DDeviceContextState** ppPreviousState) override                                                              { if (m_real1) m_real1->SwapDeviceContextState(pState, ppPreviousState); }
    void STDMETHODCALLTYPE ClearView(ID3D11View* pView, const FLOAT Color[4], const D3D11_RECT* pRect, UINT NumRects) override                                                                    { if (m_real1) m_real1->ClearView(pView, Color, pRect, NumRects); }
    void STDMETHODCALLTYPE DiscardView1(ID3D11View* pResourceView, const D3D11_RECT* pRects, UINT NumRects) override                                                                              { if (m_real1) m_real1->DiscardView1(pResourceView, pRects, NumRects); }

    // ----- ID3D11DeviceContext2 — tiled-resource APIs + annotations
    HRESULT STDMETHODCALLTYPE UpdateTileMappings(ID3D11Resource* pTiledResource, UINT NumTiledResourceRegions, const D3D11_TILED_RESOURCE_COORDINATE* pTiledResourceRegionStartCoordinates, const D3D11_TILE_REGION_SIZE* pTiledResourceRegionSizes, ID3D11Buffer* pTilePool, UINT NumRanges, const UINT* pRangeFlags, const UINT* pTilePoolStartOffsets, const UINT* pRangeTileCounts, UINT Flags) override { return m_real2 ? m_real2->UpdateTileMappings(pTiledResource, NumTiledResourceRegions, pTiledResourceRegionStartCoordinates, pTiledResourceRegionSizes, pTilePool, NumRanges, pRangeFlags, pTilePoolStartOffsets, pRangeTileCounts, Flags) : E_NOINTERFACE; }
    HRESULT STDMETHODCALLTYPE CopyTileMappings(ID3D11Resource* pDestTiledResource, const D3D11_TILED_RESOURCE_COORDINATE* pDestRegionStartCoordinate, ID3D11Resource* pSourceTiledResource, const D3D11_TILED_RESOURCE_COORDINATE* pSourceRegionStartCoordinate, const D3D11_TILE_REGION_SIZE* pTileRegionSize, UINT Flags) override { return m_real2 ? m_real2->CopyTileMappings(pDestTiledResource, pDestRegionStartCoordinate, pSourceTiledResource, pSourceRegionStartCoordinate, pTileRegionSize, Flags) : E_NOINTERFACE; }
    void STDMETHODCALLTYPE CopyTiles(ID3D11Resource* pTiledResource, const D3D11_TILED_RESOURCE_COORDINATE* pTileRegionStartCoordinate, const D3D11_TILE_REGION_SIZE* pTileRegionSize, ID3D11Buffer* pBuffer, UINT64 BufferStartOffsetInBytes, UINT Flags) override { if (m_real2) m_real2->CopyTiles(pTiledResource, pTileRegionStartCoordinate, pTileRegionSize, pBuffer, BufferStartOffsetInBytes, Flags); }
    void STDMETHODCALLTYPE UpdateTiles(ID3D11Resource* pDestTiledResource, const D3D11_TILED_RESOURCE_COORDINATE* pDestTileRegionStartCoordinate, const D3D11_TILE_REGION_SIZE* pDestTileRegionSize, const void* pSourceTileData, UINT Flags) override { if (m_real2) m_real2->UpdateTiles(pDestTiledResource, pDestTileRegionStartCoordinate, pDestTileRegionSize, pSourceTileData, Flags); }
    HRESULT STDMETHODCALLTYPE ResizeTilePool(ID3D11Buffer* pTilePool, UINT64 NewSizeInBytes) override                                                                                              { return m_real2 ? m_real2->ResizeTilePool(pTilePool, NewSizeInBytes) : E_NOINTERFACE; }
    void STDMETHODCALLTYPE TiledResourceBarrier(ID3D11DeviceChild* pBeforeBarrier, ID3D11DeviceChild* pAfterBarrier) override                                                                      { if (m_real2) m_real2->TiledResourceBarrier(pBeforeBarrier, pAfterBarrier); }
    BOOL STDMETHODCALLTYPE IsAnnotationEnabled() override                                                                                                                                          { return m_real2 ? m_real2->IsAnnotationEnabled() : FALSE; }
    void STDMETHODCALLTYPE SetMarkerInt(LPCWSTR pLabel, INT Data) override                                                                                                                         { if (m_real2) m_real2->SetMarkerInt(pLabel, Data); }
    void STDMETHODCALLTYPE BeginEventInt(LPCWSTR pLabel, INT Data) override                                                                                                                        { if (m_real2) m_real2->BeginEventInt(pLabel, Data); }
    void STDMETHODCALLTYPE EndEvent() override                                                                                                                                                     { if (m_real2) m_real2->EndEvent(); }

    // ----- ID3D11DeviceContext3 — Flush1 + protected-state
    void STDMETHODCALLTYPE Flush1(D3D11_CONTEXT_TYPE ContextType, HANDLE hEvent) override                                                                                                          { if (m_real3) m_real3->Flush1(ContextType, hEvent); }
    void STDMETHODCALLTYPE SetHardwareProtectionState(BOOL HwProtectionEnable) override                                                                                                            { if (m_real3) m_real3->SetHardwareProtectionState(HwProtectionEnable); }
    void STDMETHODCALLTYPE GetHardwareProtectionState(BOOL* pHwProtectionEnable) override                                                                                                          { if (m_real3) m_real3->GetHardwareProtectionState(pHwProtectionEnable); }

    // Accessors for 1b-iv routing logic
    ID3D11DeviceContext* GetReal()  const { return m_real; }
    Device11Proxy*       GetParent() const { return m_parent; }

    // Stage 3 helpers — exposed so the shared OMSet hook can mark whether
    // the BB-RTV is currently bound (RSSetViewports needs to know).
    void SetCurrentBBBound(bool b) { m_currentBBBound = b; }
    bool GetCurrentBBBound() const { return m_currentBBBound; }

    // Stage 4a: active-eye state. Default is left. Stage 4d (SBS composite
    // at swap-chain Present) flips this to right between L/R passes so that
    // OMSetRenderTargets picks the right-eye real RTV/DSV when binding a
    // stereo-doubled view. Left and right textures were allocated as a pair
    // by Texture2D11Proxy's constructor (Stage 3b); 4a just decides which
    // one gets bound at draw time.
    enum class Eye { Left = 0, Right = 1 };
    void SetActiveEye(Eye e) { m_activeEye = e; }
    Eye  GetActiveEye() const { return m_activeEye; }

    // Stage 4b.1: frame-recording INFRASTRUCTURE — the storage vector,
    // a clear hook, and a replay-with-eye method. iZ3D's StereoCommandBuffer
    // pattern ported to COM: each recorded entry is a std::function that
    // re-invokes the proxy-level call so the closure re-runs OUR intercept
    // logic — which picks the eye-appropriate real handle based on
    // m_activeEye at REPLAY time, not capture time. That's how setting
    // m_activeEye=Right before ReplayFrameCommands lets the same recorded
    // OMSet bind the right RTV.
    void ClearFrameCommands();
    void ReplayFrameCommands(Eye eye);

    // Stage 4b.4: gate for whether OMSet/state-setters actually push into
    // m_frameCommands. Set true the first time SwapChain11Proxy's Present
    // hook fires on our parent's swap chain (so we know the vector will be
    // flushed each frame). Stays false for games whose swap chain bypasses
    // us (e.g. D3D11CreateDevice + factory->CreateSwapChain), so we never
    // accumulate an unbounded recording. The 4b.8 factory hook will close
    // that gap; until then it's safer to skip recording than to leak.
    // Refuses to arm while gInfo.DuplicateDraws is set — one gate keeps the
    // whole record-and-replay path dormant without touching ~40 state setters.
    void SetPresentHookActive(bool active);
    bool IsPresentHookActive() const { return m_presentHookActive; }

    // Diagnostic (Aug 2026): per-frame tally of work the game issued on THIS
    // (immediate) context, logged periodically and reset each Present. Answers
    // "is the recording empty because the game drew nothing, or because the
    // draws went somewhere we don't see?" — cmdLists > 0 means deferred
    // contexts are in play and the right eye can never be correct via replay.
    // Counts game-issued calls only: replay closures invoke m_real directly
    // rather than re-entering these overrides, so they don't inflate the tally.
    void LogAndResetFrameDrawStats();

private:
    // Stage 4b.4: private "Do" versions of state-setting methods that the
    // public ID3D11DeviceContext overrides delegate to. The recorded
    // lambdas also call these Do* helpers directly, so replay re-runs the
    // eye-aware unwrap logic with whatever m_activeEye is set to at replay
    // time without triggering another record (no infinite recursion).
    void DoOMSetRenderTargets(UINT NumViews,
                              ID3D11RenderTargetView* const* ppRenderTargetViews,
                              ID3D11DepthStencilView* pDepthStencilView);
    void DoOMSetRenderTargetsAndUnorderedAccessViews(
        UINT NumRTVs, ID3D11RenderTargetView* const* ppRenderTargetViews,
        ID3D11DepthStencilView* pDepthStencilView,
        UINT UAVStartSlot, UINT NumUAVs,
        ID3D11UnorderedAccessView* const* ppUnorderedAccessViews,
        const UINT* pUAVInitialCounts);

    // Stage 4b.6: eye-aware Copy/Update/Resolve/Clear helpers. Same shape as
    // DoOMSetRenderTargets — pick the left- or right-eye real handle per
    // wrapped resource/view depending on m_activeEye. Recording closures
    // invoke these directly so replay re-runs the eye selection with the
    // active eye set by ReplayFrameCommands.
    void DoCopyResource(ID3D11Resource* pDstResource, ID3D11Resource* pSrcResource);
    void DoCopySubresourceRegion(
        ID3D11Resource* pDstResource, UINT DstSubresource, UINT DstX, UINT DstY,
        UINT DstZ, ID3D11Resource* pSrcResource, UINT SrcSubresource,
        const D3D11_BOX* pSrcBox);
    void DoUpdateSubresource(
        ID3D11Resource* pDstResource, UINT DstSubresource, const D3D11_BOX* pDstBox,
        const void* pSrcData, UINT SrcRowPitch, UINT SrcDepthPitch);
    void DoResolveSubresource(
        ID3D11Resource* pDstResource, UINT DstSubresource,
        ID3D11Resource* pSrcResource, UINT SrcSubresource, DXGI_FORMAT Format);
    void DoClearRenderTargetView(
        ID3D11RenderTargetView* pRenderTargetView, const FLOAT ColorRGBA[4]);
    void DoClearDepthStencilView(
        ID3D11DepthStencilView* pDepthStencilView, UINT ClearFlags, FLOAT Depth, UINT8 Stencil);

    // ---- Draw duplication (gInfo.DuplicateDraws) ------------------------
    // Each *Set* call resolves BOTH eyes' real pointers once and parks them
    // here, so switching eyes mid-draw is an array hand-off with no unwrapping.
    enum StageIdx { ST_VS = 0, ST_PS, ST_GS, ST_HS, ST_DS, ST_CS, ST_COUNT };

    // CS UAVs resolve their eye at set time, so a duplicated dispatch needs the
    // game's wrapped pointers kept to rebind against the right eye.
    struct UAVEyeSlots
    {
        ID3D11UnorderedAccessView* left[kMaxUAVs];
        ID3D11UnorderedAccessView* right[kMaxUAVs];
        UINT high;
        bool anyStereo;
    };
    UAVEyeSlots m_csUAVSlots = {};
    void TrackCSUAVs(UINT StartSlot, UINT NumUAVs, ID3D11UnorderedAccessView* const* pp);
    void BindCSUAVs(bool right);
    bool BeginRightEyeDispatch();
    void EndRightEyeDispatch();

    struct SRVEyeSlots
    {
        ID3D11ShaderResourceView* left[kMaxSRVs];
        ID3D11ShaderResourceView* right[kMaxSRVs];
        UINT high;       // highest slot ever bound + 1 — bounds every rebind
        bool anyStereo;  // some slot in [0,high) resolves differently per eye
    };
    struct CBEyeSlots
    {
        ID3D11Buffer* left[kMaxCBs];
        ID3D11Buffer* right[kMaxCBs];
        UINT high;
        bool anyStereo;
    };

    SRVEyeSlots m_srvSlots[ST_COUNT];
    CBEyeSlots  m_cbSlots[ST_COUNT];

    ID3D11RenderTargetView* m_rtvLeft[kMaxRTVs];
    ID3D11RenderTargetView* m_rtvRight[kMaxRTVs];
    UINT                    m_numRTVs;
    ID3D11DepthStencilView* m_dsvLeft;
    ID3D11DepthStencilView* m_dsvRight;
    // Decides on its own whether a draw is duplicated — a pass writing a mono
    // target issues once, as StereoCommandBuffer does with bStereoDraw.
    bool                    m_omAnyStereo;
    // Set while UAVs are bound through the OM. BindEye restores with a plain
    // OMSetRenderTargets, which would unbind them, so we skip duplication.
    bool                    m_omHasUAVs;
    // Depth-stencil bound but mono: the right pass depth-tests against values
    // the left pass wrote, which can reject its geometry.
    bool                    m_omMonoDSV;

    // Game-facing OM bindings, mirroring whatever is currently bound. Safe to
    // return un-AddRef'd from storage because D3D holds a ref on bound views
    // and every OMSet overwrites this.
    ID3D11RenderTargetView* m_rtvGame[kMaxRTVs] = {};
    ID3D11DepthStencilView* m_dsvGame           = nullptr;
    UINT                    m_numRTVGame        = 0;
    void RecordGameFacingOM(UINT NumViews, ID3D11RenderTargetView* const* ppRTVs,
                            ID3D11DepthStencilView* pDSV);

    void TrackSRVs(StageIdx stage, UINT StartSlot, UINT NumViews,
                   ID3D11ShaderResourceView* const* ppSRVs);
    void TrackCBs(StageIdx stage, UINT StartSlot, UINT NumBuffers,
                  ID3D11Buffer* const* ppCBs);
    void TrackOM(UINT NumViews, ID3D11RenderTargetView* const* ppRTVs,
                 ID3D11DepthStencilView* pDSV);
    void ResetEyeTracking();

    // Rebind tracked slots that differ between eyes; called twice per
    // duplicated draw, once to switch and once to switch back.
    void BindEye(bool right);
    void BindStageSRVs(StageIdx stage, bool right);
    void BindStageCBs(StageIdx stage, bool right);

    // Bracket a duplicated draw. Begin returns false when duplication is off,
    // the targets are mono, or we are already inside the right-eye pass.
    bool BeginRightEyeDraw();
    void EndRightEyeDraw();

    // Frame summary tally: draws issued twice vs left single (mono targets).
    unsigned m_drawsDuplicatedThisFrame = 0;
    unsigned m_drawsMonoThisFrame       = 0;
    unsigned m_drawsUavSkippedThisFrame = 0;
    unsigned m_drawsMonoDsvThisFrame    = 0;
    unsigned m_drawsNoRightRTVThisFrame = 0;
    // Depth-only draws (no RTV bound), and draws with no right-eye target of
    // any kind — the latter is the one that means content is actually lost.
    unsigned m_dispatchesDuplicatedThisFrame = 0;
    unsigned m_drawsDepthOnlyThisFrame     = 0;
    unsigned m_drawsNoRightTargetThisFrame = 0;

    ID3D11DeviceContext* m_real;
    // Cached upgrades of m_real for Context1/2/3 dispatch (claimed in QI
    // with `this`; new-version method overrides route through these).
    ID3D11DeviceContext1* m_real1;
    ID3D11DeviceContext2* m_real2;
    ID3D11DeviceContext3* m_real3;
    Device11Proxy*       m_parent;
    LONG                 m_refs;
    bool                 m_currentBBBound;       // last OMSet had BB-RTV at slot 0
    Eye                  m_activeEye;            // Stage 4a: which eye OMSet binds
    bool                 m_presentHookActive;    // Stage 4b.4 safety gate

    // Diagnostic counters backing LogAndResetFrameDrawStats(). Not part of
    // any rendering decision — purely for the wiz3D_proxy.log summary.
    unsigned             m_drawsThisFrame     = 0;
    unsigned             m_dispatchesThisFrame = 0;
    unsigned             m_cmdListsThisFrame  = 0;
    // Per-eye CB patch policy tally (see gInfo.DisableBlindCBScan):
    //   targeted — analyzer named the exact matrix register(s) to shift
    //   blind    — fell back to the whole-buffer heuristic scan
    //   skipped  — analyzer positively cleared the buffer, left untouched
    unsigned             m_cbTargetedThisFrame = 0;
    unsigned             m_cbBlindThisFrame    = 0;
    unsigned             m_cbSkippedThisFrame  = 0;
    // Within the targeted patches: individual matrices actually shifted, vs
    // rejected by the ortho / shadow-map guards in ShouldSkipProjectionMatrix.
    unsigned             m_cbMatShiftedThisFrame  = 0;
    unsigned             m_cbMatRejectedThisFrame = 0;
    // Draw-level stereo coverage: why a draw's geometry could not be corrected.
    unsigned             m_drawsVSUnparsedThisFrame     = 0;
    unsigned             m_drawsVSNoMatrixThisFrame     = 0;
    unsigned             m_drawsVSNeverShiftedThisFrame = 0;
    void TallyDrawCoverage();
    void BeginDraw(UINT vertexCount);
    // Draws per modified-shader CRC, so the ones the scene really uses are known.
    std::map<DWORD, unsigned> m_modVSDraws;
    // Stage 4f: dynamic vertex/index buffer write replays. `skipped` counts
    // writes we declined to snapshot — either over the size cap, or mapped
    // WRITE_NO_OVERWRITE, where a whole-buffer rewrite would corrupt the left
    // eye's pending draws (see the map-type check in Map). Those draws still
    // replay, but from stale geometry.
    unsigned             m_dynBufReplaysThisFrame = 0;
    unsigned             m_dynBufSkippedThisFrame = 0;

    // Stage 4e.2: VS binding snapshot for targeted CB stereo math. m_boundVS
    // is the game's vertex shader pointer most recently set via VSSetShader.
    // m_boundVSCBs[i] is the wrapped buffer pointer set at VS CB slot i via
    // VSSetConstantBuffers. Neither is AddRef'd — the game owns lifetime,
    // and at worst the analyzer cache miss falls us back to the heuristic
    // scan. Used inside Unmap to ask Device11Proxy::LookupShaderProjection
    // whether the bound VS has a known projection-matrix register in the CB
    // being mapped, so per-eye writes target only that register.
public:
    // Every live context, so the frame summary can show work happening on a
    // device that never presents through a swap chain we wrapped.
    static std::string DescribeAllContexts();
private:
    static constexpr UINT kMaxVSCBSlots = 15;
    ID3D11VertexShader*  m_boundVS;
    ID3D11Buffer*        m_boundVSCBs[kMaxVSCBSlots];

    // Same snapshot for the pixel stage. BaseProfile.xml keys its hand-written
    // matrix declarations on <PixelShader CRC=...>, so the deferred-lighting
    // fix needs to know which PS and which PS CB slot is being written.
    static constexpr UINT kMaxPSCBSlots = 15;
    ID3D11PixelShader*   m_boundPS;
    ID3D11Buffer*        m_boundPSCBs[kMaxPSCBSlots];

    // Self-shifting VS support. m_modVSShader is the modified variant of the
    // currently bound shader (null when there isn't one, which is the signal that
    // no per-eye CB is needed); the two CBs hold its shift constants and are
    // rebuilt only when separation changes. Not AddRef'd — Device11Proxy owns it.
    ID3D11VertexShader* m_modVSShader  = nullptr;
    UINT                m_modVSCBIndex = 0;
    ID3D11Buffer* m_stereoCBLeft  = nullptr;
    ID3D11Buffer* m_stereoCBRight = nullptr;
    float         m_stereoCBSep   = 0.f;
    float         m_stereoCBConv  = 0.f;
    bool EnsureStereoShiftCBs();
    void BindStereoShiftCB(bool right);
    void ReleaseStereoShiftCBs();

    // Stage 4b.1: per-frame command record. Stage 4b.8 + 4d flush + replay
    // before each Present. Only populated when m_presentHookActive is true.
    std::vector<std::function<void()>> m_frameCommands;

    // Stage 4b.5: active-map tracking. D3D11's Map() returns a pointer the
    // game writes into; only at Unmap() is the data committed to GPU. We can
    // only capture the bytes once the writes are done — i.e. at Unmap, BEFORE
    // we forward Unmap to the real context. So Map stashes the mapped pointer
    // plus the resource's byte size, and Unmap memcpy's that many bytes into
    // a snapshot which the replay closure re-issues. Buffer-only for now;
    // texture map replay would need pitch/slice metadata (deferred to 4b.6+).
    struct ActiveMap
    {
        ID3D11Resource* resource;       // game's pointer (Map keeps it pinned)
        UINT            subresource;
        D3D11_MAP       mapType;
        void*           mappedData;
        UINT            byteWidth;
        // false ⇒ dynamic vertex/index buffer: replay the write verbatim, with
        // no projection-matrix patching (shifting geometry would double-apply
        // the stereo offset on top of the CB shift).
        bool            isConstantBuffer;
    };
    std::vector<ActiveMap> m_activeMaps;
};

} // namespace wiz3d
