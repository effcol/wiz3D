/* wiz3D - ID3D11DeviceContext proxy
 * Each stereo draw and dispatch runs again for the right eye, with right-eye copies bound and constants patched. */

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
#include "ProfileFeature.h"
#include "ShaderPatch11.h"   // right-eye shader twins
#include "ShaderModifier11.h" // wiz3d::ModifiedVS11 (TryBuildModifiedVS Phase 2)

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
class Buffer11Proxy;
class Texture2D11Proxy;
class RightRecorder11;
class GpuTimer11;
struct EyeShiftMatrix;
struct ShaderAnalysis11Result;

class Context11Proxy : public ID3D11DeviceContext3
{
public:
    // D3D11 spec maxima: smaller unwrap arrays let the runtime read past them into stack garbage.
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

    // Called by ~Device11Proxy before it Releases this context, so if the
    // Release doesn't drop the final ref (over-ref'd by the game), our own
    // destructor won't later try to notify a freed Device11Proxy via
    // OnContextProxyDestroyed. Pairs with ~Device11Proxy's cleanup path.
    void DetachFromParent();

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
    void STDMETHODCALLTYPE Begin(ID3D11Asynchronous* pAsync) override                                                                                                                              { FlushRightBatch(kFlushQuery); m_real->Begin(pAsync); }
    void STDMETHODCALLTYPE End(ID3D11Asynchronous* pAsync) override                                                                                                                                { FlushRightBatch(kFlushQuery); m_real->End(pAsync); }
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
    void STDMETHODCALLTYPE SetResourceMinLOD(ID3D11Resource* pResource, FLOAT MinLOD) override;
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
    void STDMETHODCALLTYPE VSGetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer** ppConstantBuffers) override                                                                        { GetTrackedCBs(ST_VS, StartSlot, NumBuffers, ppConstantBuffers); }
    void STDMETHODCALLTYPE PSGetShaderResources(UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView** ppShaderResourceViews) override                                                          { GetTrackedSRVs(ST_PS, StartSlot, NumViews, ppShaderResourceViews); }
    void STDMETHODCALLTYPE PSGetShader(ID3D11PixelShader** ppPixelShader, ID3D11ClassInstance** ppClassInstances, UINT* pNumClassInstances) override                                               { m_real->PSGetShader(ppPixelShader, ppClassInstances, pNumClassInstances); }
    void STDMETHODCALLTYPE PSGetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState** ppSamplers) override                                                                               { m_real->PSGetSamplers(StartSlot, NumSamplers, ppSamplers); }
    void STDMETHODCALLTYPE VSGetShader(ID3D11VertexShader** ppVertexShader, ID3D11ClassInstance** ppClassInstances, UINT* pNumClassInstances) override                                             { m_real->VSGetShader(ppVertexShader, ppClassInstances, pNumClassInstances); }
    void STDMETHODCALLTYPE PSGetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer** ppConstantBuffers) override                                                                        { GetTrackedCBs(ST_PS, StartSlot, NumBuffers, ppConstantBuffers); }
    void STDMETHODCALLTYPE IAGetInputLayout(ID3D11InputLayout** ppInputLayout) override                                                                                                            { m_real->IAGetInputLayout(ppInputLayout); }
    void STDMETHODCALLTYPE IAGetVertexBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer** ppVertexBuffers, UINT* pStrides, UINT* pOffsets) override                                            { m_real->IAGetVertexBuffers(StartSlot, NumBuffers, ppVertexBuffers, pStrides, pOffsets); }
    void STDMETHODCALLTYPE IAGetIndexBuffer(ID3D11Buffer** pIndexBuffer, DXGI_FORMAT* Format, UINT* Offset) override                                                                               { m_real->IAGetIndexBuffer(pIndexBuffer, Format, Offset); }
    void STDMETHODCALLTYPE GSGetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer** ppConstantBuffers) override                                                                        { GetTrackedCBs(ST_GS, StartSlot, NumBuffers, ppConstantBuffers); }
    void STDMETHODCALLTYPE GSGetShader(ID3D11GeometryShader** ppGeometryShader, ID3D11ClassInstance** ppClassInstances, UINT* pNumClassInstances) override                                         { m_real->GSGetShader(ppGeometryShader, ppClassInstances, pNumClassInstances); }
    void STDMETHODCALLTYPE IAGetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY* pTopology) override                                                                                                    { m_real->IAGetPrimitiveTopology(pTopology); }
    void STDMETHODCALLTYPE VSGetShaderResources(UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView** ppShaderResourceViews) override                                                          { GetTrackedSRVs(ST_VS, StartSlot, NumViews, ppShaderResourceViews); }
    void STDMETHODCALLTYPE VSGetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState** ppSamplers) override                                                                               { m_real->VSGetSamplers(StartSlot, NumSamplers, ppSamplers); }
    void STDMETHODCALLTYPE GetPredication(ID3D11Predicate** ppPredicate, BOOL* pPredicateValue) override                                                                                           { m_real->GetPredication(ppPredicate, pPredicateValue); }
    void STDMETHODCALLTYPE GSGetShaderResources(UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView** ppShaderResourceViews) override                                                          { GetTrackedSRVs(ST_GS, StartSlot, NumViews, ppShaderResourceViews); }
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
    void STDMETHODCALLTYPE HSGetShaderResources(UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView** ppShaderResourceViews) override                                                          { GetTrackedSRVs(ST_HS, StartSlot, NumViews, ppShaderResourceViews); }
    void STDMETHODCALLTYPE HSGetShader(ID3D11HullShader** ppHullShader, ID3D11ClassInstance** ppClassInstances, UINT* pNumClassInstances) override                                                 { m_real->HSGetShader(ppHullShader, ppClassInstances, pNumClassInstances); }
    void STDMETHODCALLTYPE HSGetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState** ppSamplers) override                                                                               { m_real->HSGetSamplers(StartSlot, NumSamplers, ppSamplers); }
    void STDMETHODCALLTYPE HSGetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer** ppConstantBuffers) override                                                                        { GetTrackedCBs(ST_HS, StartSlot, NumBuffers, ppConstantBuffers); }
    void STDMETHODCALLTYPE DSGetShaderResources(UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView** ppShaderResourceViews) override                                                          { GetTrackedSRVs(ST_DS, StartSlot, NumViews, ppShaderResourceViews); }
    void STDMETHODCALLTYPE DSGetShader(ID3D11DomainShader** ppDomainShader, ID3D11ClassInstance** ppClassInstances, UINT* pNumClassInstances) override                                             { m_real->DSGetShader(ppDomainShader, ppClassInstances, pNumClassInstances); }
    void STDMETHODCALLTYPE DSGetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState** ppSamplers) override                                                                               { m_real->DSGetSamplers(StartSlot, NumSamplers, ppSamplers); }
    void STDMETHODCALLTYPE DSGetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer** ppConstantBuffers) override                                                                        { GetTrackedCBs(ST_DS, StartSlot, NumBuffers, ppConstantBuffers); }
    void STDMETHODCALLTYPE CSGetShaderResources(UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView** ppShaderResourceViews) override                                                          { GetTrackedSRVs(ST_CS, StartSlot, NumViews, ppShaderResourceViews); }
    void STDMETHODCALLTYPE CSGetUnorderedAccessViews(UINT StartSlot, UINT NumUAVs, ID3D11UnorderedAccessView** ppUnorderedAccessViews) override                                                    { GetTrackedUAVs(StartSlot, NumUAVs, ppUnorderedAccessViews); }
    void STDMETHODCALLTYPE CSGetShader(ID3D11ComputeShader** ppComputeShader, ID3D11ClassInstance** ppClassInstances, UINT* pNumClassInstances) override                                           { m_real->CSGetShader(ppComputeShader, ppClassInstances, pNumClassInstances); }
    void STDMETHODCALLTYPE CSGetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState** ppSamplers) override                                                                               { m_real->CSGetSamplers(StartSlot, NumSamplers, ppSamplers); }
    void STDMETHODCALLTYPE CSGetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer** ppConstantBuffers) override                                                                        { GetTrackedCBs(ST_CS, StartSlot, NumBuffers, ppConstantBuffers); }
    void STDMETHODCALLTYPE ClearState() override;
    void STDMETHODCALLTYPE Flush() override                                                                                                                                                        { FlushRightBatch(kFlushState); m_real->Flush(); }
    D3D11_DEVICE_CONTEXT_TYPE STDMETHODCALLTYPE GetType() override                                                                                                                                 { return m_real->GetType(); }
    UINT STDMETHODCALLTYPE GetContextFlags() override                                                                                                                                              { return m_real->GetContextFlags(); }
    HRESULT STDMETHODCALLTYPE FinishCommandList(BOOL RestoreDeferredContextState, ID3D11CommandList** ppCommandList) override                                                                      { return m_real->FinishCommandList(RestoreDeferredContextState, ppCommandList); }

    // ----- ID3D11DeviceContext1 — claim Context1 in QI with `this` so games
    // accessing the immediate context via Device1::GetImmediateContext1 land
    // on our proxy, not the unwrapped real Context1. New methods passthrough
    // through m_real1 — none have per-eye logic yet, so games using these
    // (ClearView, DiscardView, *SetConstantBuffers1) will skip per-eye state
    // for those calls. Future stage can fill in record-for-replay.
    void STDMETHODCALLTYPE CopySubresourceRegion1(ID3D11Resource* pDstResource, UINT DstSubresource, UINT DstX, UINT DstY, UINT DstZ, ID3D11Resource* pSrcResource, UINT SrcSubresource, const D3D11_BOX* pSrcBox, UINT CopyFlags) override;
    void STDMETHODCALLTYPE UpdateSubresource1(ID3D11Resource* pDstResource, UINT DstSubresource, const D3D11_BOX* pDstBox, const void* pSrcData, UINT SrcRowPitch, UINT SrcDepthPitch, UINT CopyFlags) override;
    void STDMETHODCALLTYPE DiscardResource(ID3D11Resource* pResource) override;
    void STDMETHODCALLTYPE DiscardView(ID3D11View* pResourceView) override;
    // Same clobber risk as VSSetConstantBuffers — a Device1+ game reaching the
    // modified shader's slot through this entry point must not unbind us.
    void STDMETHODCALLTYPE VSSetConstantBuffers1(UINT StartSlot, UINT NumBuffers, ID3D11Buffer* const* ppConstantBuffers, const UINT* pFirstConstant, const UINT* pNumConstants) override;
    void STDMETHODCALLTYPE HSSetConstantBuffers1(UINT StartSlot, UINT NumBuffers, ID3D11Buffer* const* ppConstantBuffers, const UINT* pFirstConstant, const UINT* pNumConstants) override;
    void STDMETHODCALLTYPE DSSetConstantBuffers1(UINT StartSlot, UINT NumBuffers, ID3D11Buffer* const* ppConstantBuffers, const UINT* pFirstConstant, const UINT* pNumConstants) override;
    void STDMETHODCALLTYPE GSSetConstantBuffers1(UINT StartSlot, UINT NumBuffers, ID3D11Buffer* const* ppConstantBuffers, const UINT* pFirstConstant, const UINT* pNumConstants) override;
    void STDMETHODCALLTYPE PSSetConstantBuffers1(UINT StartSlot, UINT NumBuffers, ID3D11Buffer* const* ppConstantBuffers, const UINT* pFirstConstant, const UINT* pNumConstants) override;
    void STDMETHODCALLTYPE CSSetConstantBuffers1(UINT StartSlot, UINT NumBuffers, ID3D11Buffer* const* ppConstantBuffers, const UINT* pFirstConstant, const UINT* pNumConstants) override;
    void STDMETHODCALLTYPE VSGetConstantBuffers1(UINT StartSlot, UINT NumBuffers, ID3D11Buffer** ppConstantBuffers, UINT* pFirstConstant, UINT* pNumConstants) override                           { GetTrackedCBs(ST_VS, StartSlot, NumBuffers, ppConstantBuffers, pFirstConstant, pNumConstants); }
    void STDMETHODCALLTYPE HSGetConstantBuffers1(UINT StartSlot, UINT NumBuffers, ID3D11Buffer** ppConstantBuffers, UINT* pFirstConstant, UINT* pNumConstants) override                           { GetTrackedCBs(ST_HS, StartSlot, NumBuffers, ppConstantBuffers, pFirstConstant, pNumConstants); }
    void STDMETHODCALLTYPE DSGetConstantBuffers1(UINT StartSlot, UINT NumBuffers, ID3D11Buffer** ppConstantBuffers, UINT* pFirstConstant, UINT* pNumConstants) override                           { GetTrackedCBs(ST_DS, StartSlot, NumBuffers, ppConstantBuffers, pFirstConstant, pNumConstants); }
    void STDMETHODCALLTYPE GSGetConstantBuffers1(UINT StartSlot, UINT NumBuffers, ID3D11Buffer** ppConstantBuffers, UINT* pFirstConstant, UINT* pNumConstants) override                           { GetTrackedCBs(ST_GS, StartSlot, NumBuffers, ppConstantBuffers, pFirstConstant, pNumConstants); }
    void STDMETHODCALLTYPE PSGetConstantBuffers1(UINT StartSlot, UINT NumBuffers, ID3D11Buffer** ppConstantBuffers, UINT* pFirstConstant, UINT* pNumConstants) override                           { GetTrackedCBs(ST_PS, StartSlot, NumBuffers, ppConstantBuffers, pFirstConstant, pNumConstants); }
    void STDMETHODCALLTYPE CSGetConstantBuffers1(UINT StartSlot, UINT NumBuffers, ID3D11Buffer** ppConstantBuffers, UINT* pFirstConstant, UINT* pNumConstants) override                           { GetTrackedCBs(ST_CS, StartSlot, NumBuffers, ppConstantBuffers, pFirstConstant, pNumConstants); }
    void STDMETHODCALLTYPE SwapDeviceContextState(ID3DDeviceContextState* pState, ID3DDeviceContextState** ppPreviousState) override;
    void STDMETHODCALLTYPE ClearView(ID3D11View* pView, const FLOAT Color[4], const D3D11_RECT* pRect, UINT NumRects) override;
    void STDMETHODCALLTYPE DiscardView1(ID3D11View* pResourceView, const D3D11_RECT* pRects, UINT NumRects) override;

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

    // Arms recording into m_frameCommands once our swap chain's Present runs, so the recording is flushed each frame.
    // It never arms while gInfo.DuplicateDraws is set, which keeps the whole record-and-replay path dormant.
    void SetPresentHookActive(bool active);
    bool IsPresentHookActive() const { return m_presentHookActive; }

    // Once per Present: ages the matrix detector and the shared-UAV twins, ends the features' frame and, with
    // CollectDebugInformation, logs this frame's counts every 60th frame that drew anything.
    void EndFrame();
    // CollectDebugInformation: the swap chain's part of the frame (output pass, real Present, frames the GPU is behind).
    void NoteFrameTiming(LONGLONG outputTicks, LONGLONG presentTicks, unsigned gpuBehind);

    // <BatchRightEye>: the right eye's recorded draws run now, before a pass change, a write to something they read, a
    // dispatch, a query or the frame's composite (the reason is counted for the frame report).
    enum BatchFlush { kFlushTargets, kFlushWrite, kFlushCompute, kFlushPresent, kFlushUnbatched, kFlushState, kFlushQuery,
                      kFlushMap, kFlushUpdate, kFlushReasons };
    void FlushRightBatch(BatchFlush reason);
    // CollectDebugInformation: the swap chain's output pass as a GPU span (GpuTimer11).
    UINT GpuOutputBegin();
    void GpuOutputEnd(UINT begin);

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
    // Under <RawResources>: a real left texture the game reached past the wrapper, as its proxy; else p as given.
    ID3D11Resource* GameResource(ID3D11Resource* p) const;
    void DoClearRenderTargetView(
        ID3D11RenderTargetView* pRenderTargetView, const FLOAT ColorRGBA[4]);
    void DoClearDepthStencilView(
        ID3D11DepthStencilView* pDepthStencilView, UINT ClearFlags, FLOAT Depth, UINT8 Stencil);

    // ---- Draw duplication (gInfo.DuplicateDraws): each *Set* call resolves both eyes' real pointers once and parks
    // them here, so switching eyes mid-draw is an array hand-off with no unwrapping.
    enum StageIdx { ST_VS = 0, ST_PS, ST_GS, ST_HS, ST_DS, ST_CS, ST_COUNT };

    // CS UAVs resolve their eye at set time, so a duplicated dispatch needs the
    // game's wrapped pointers kept to rebind against the right eye.
    struct UAVEyeSlots
    {
        ID3D11UnorderedAccessView* left[kMaxUAVs];
        ID3D11UnorderedAccessView* right[kMaxUAVs];
        ID3D11UnorderedAccessView* game[kMaxUAVs];   // what the game bound, answered back by the getter
        UINT high;
        bool anyStereo;
    };
    UAVEyeSlots m_csUAVSlots = {};
    void TrackCSUAVs(UINT StartSlot, UINT NumUAVs, ID3D11UnorderedAccessView* const* pp, const UINT* initialCounts);
    // The game's initial counts at the last bind, valid until the first dispatch after it (-1 = keep the counter).
    UINT m_csInitCount[kMaxUAVs];
    bool m_csInitFresh[kMaxUAVs] = {};
    // A right dispatch's twin of each shared UAV, copied before the left dispatch, so the right reads what the left read
    // and writes nothing the left eye owns (PrepareSharedUavTwins, BindCSUAVs).
    struct UavTwin
    {
        ID3D11Resource*            src = nullptr;   // not owned; with the descs, identifies a reused address
        ID3D11Resource*            res = nullptr;
        ID3D11UnorderedAccessView* uav = nullptr;
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {};
        UINT64                     bytes = 0;
        unsigned                   lastFrame = 0;
    };
    std::map<ID3D11UnorderedAccessView*, UavTwin> m_uavTwins;
    ID3D11UnorderedAccessView* m_csTwin[kMaxUAVs] = {};
    UINT                       m_csTwinCount[kMaxUAVs];
    bool                       m_csTwinsPrepared = false;   // the arrays above belong to this dispatch
    void PrepareSharedUavTwins();
    ID3D11UnorderedAccessView* TwinFor(ID3D11UnorderedAccessView* v);
    void ReleaseUavTwins(unsigned olderThan);
    unsigned m_twinCopiesThisFrame = 0, m_twinFailedThisFrame = 0, m_twinCounterUnknownThisFrame = 0;
    unsigned m_twinWriteOnlyThisFrame = 0;
    UINT64   m_twinBytesThisFrame = 0;
    void BindCSUAVs(bool right);
    bool BeginRightEyeDispatch();
    void EndRightEyeDispatch();

    struct SRVEyeSlots
    {
        ID3D11ShaderResourceView* left[kMaxSRVs];
        ID3D11ShaderResourceView* right[kMaxSRVs];
        ID3D11ShaderResourceView* game[kMaxSRVs];   // what the game bound, answered back by the getter
        UINT high;       // highest slot ever bound + 1 — bounds every rebind
        bool anyStereo;  // some slot in [0,high) resolves differently per eye
    };
    struct CBEyeSlots
    {
        ID3D11Buffer* left[kMaxCBs];
        ID3D11Buffer* right[kMaxCBs];
        ID3D11Buffer* game[kMaxCBs];   // what the game bound, answered back by the getter
        // The wrapper behind left[]/right[], so a draw can ask whether the constants it is about to consume were
        // actually eye-shifted. Each slot holds a reference, as the runtime does for a bound object.
        Buffer11Proxy* proxy[kMaxCBs];
        // D3D11.1 partial binds (*SetConstantBuffers1): the constant range per slot (num 0 = whole buffer) and the
        // buffer's constant count, so a rebind through the 11.1 entry can spell out whole-buffer slots.
        UINT first[kMaxCBs];
        UINT num[kMaxCBs];
        UINT size[kMaxCBs];
        // The write sequence, offset, shader set and shift each eye's real buffer last received for this slot's block,
        // so SyncBoundConstants writes a block once per change.
        unsigned syncSeq[2][kMaxCBs];
        UINT     syncOff[2][kMaxCBs];
        DWORD    syncCrc[kMaxCBs];
        float    syncShift[kMaxCBs];
        unsigned syncHash[kMaxCBs];   // the right version's block bytes (ring): rewritten only when they or the union change
        UINT high;
        bool anyStereo;
        bool anyRange;
    };

    // Every tracked pointer holds a reference (TrackSRVs, TrackCBs, TrackOM, TrackCSUAVs), dropped by ResetEyeTracking:
    // a game may release an object while it is bound, and the right eye still binds it.
    SRVEyeSlots m_srvSlots[ST_COUNT] = {};
    CBEyeSlots  m_cbSlots[ST_COUNT] = {};

    ID3D11RenderTargetView* m_rtvLeft[kMaxRTVs] = {};
    ID3D11RenderTargetView* m_rtvRight[kMaxRTVs] = {};
    UINT                    m_numRTVs;
    ID3D11DepthStencilView* m_dsvLeft = nullptr;
    ID3D11DepthStencilView* m_dsvRight = nullptr;
    // Decides on its own whether a draw is duplicated — a pass writing a mono
    // target issues once, as StereoCommandBuffer does with bStereoDraw.
    bool                    m_omAnyStereo;
    // Set while UAVs are bound through the OM: such a draw is duplicated only with a stereo target, and BindEye
    // re-issues the UAVs with it (a plain OMSetRenderTargets would unbind them).
    bool                    m_omHasUAVs;
    // Depth-stencil bound but mono: the right pass depth-tests against values
    // the left pass wrote, which can reject its geometry.
    bool                    m_omMonoDSV;
    // Overlay state: the swap chain's back buffer is the first target and the depth test is off (null state = on).
    bool                    m_omBackBuffer = false;
    bool                    m_depthTestOn  = true;
    bool                    m_depthWriteOn = true;
    D3D11_PRIMITIVE_TOPOLOGY m_topology    = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    UINT                     m_drawVertices = 0;   // the draw's vertex (index) count per instance, UINT_MAX = indirect

    // Game-facing OM bindings, mirroring whatever is currently bound; each holds a reference, so OMGetRenderTargets
    // can hand them out after the game released its own.
    ID3D11RenderTargetView* m_rtvGame[kMaxRTVs] = {};
    ID3D11DepthStencilView* m_dsvGame           = nullptr;
    UINT                    m_numRTVGame        = 0;
    void RecordGameFacingOM(UINT NumViews, ID3D11RenderTargetView* const* ppRTVs,
                            ID3D11DepthStencilView* pDSV);

    void TrackSRVs(StageIdx stage, UINT StartSlot, UINT NumViews,
                   ID3D11ShaderResourceView* const* ppSRVs);
    // State getters answer with what the game bound (our proxies), never the real objects behind them: an engine
    // saving and restoring state through them would otherwise rebind the real left resources past the right eye.
    void GetTrackedCBs(StageIdx stage, UINT StartSlot, UINT NumBuffers, ID3D11Buffer** pp,
                       UINT* pFirst = nullptr, UINT* pNum = nullptr);
    void GetTrackedSRVs(StageIdx stage, UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView** pp);
    void GetTrackedUAVs(UINT StartSlot, UINT NumUAVs, ID3D11UnorderedAccessView** pp);
    unsigned m_getCBsThisFrame = 0, m_getSRVsThisFrame = 0, m_getUAVsThisFrame = 0;
    void TrackCBs(StageIdx stage, UINT StartSlot, UINT NumBuffers, ID3D11Buffer* const* ppCBs,
                  const UINT* pFirst = nullptr, const UINT* pNum = nullptr);
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

    // OM-UAV binding, AddRef'd, so BindEye can re-issue it per eye — a plain
    // OMSetRenderTargets would silently unbind the UAVs.
    void ClearOmUavRefs();
    bool  m_omUavBound = false;
    UINT  m_omUavStart = 0;
    UINT  m_omUavCount = 0;
    ID3D11UnorderedAccessView* m_omUavGame[kMaxUAVs] = {};
    UINT  m_omUavInit[kMaxUAVs] = {};
    bool  m_omUavHasInit = false;
    // Right siblings take the recorded initial counts exactly once per game
    // bind; re-passing them on every eye switch would reset append counters.
    bool  m_omUavRightInited = false;

    // Frame report counts: draws issued twice vs once (mono targets), and why.
    unsigned m_drawsDuplicatedThisFrame = 0;
    unsigned m_drawsMonoThisFrame       = 0;
    unsigned m_drawsUavSkippedThisFrame = 0;
    unsigned m_drawsUavDupThisFrame     = 0;
    unsigned m_drawsMonoDsvThisFrame    = 0;
    unsigned m_drawsNoRightRTVThisFrame = 0;
    // Depth-only draws (no RTV bound), and draws with no right-eye target of any kind: the latter lose content.
    unsigned m_drawsDepthOnlyThisFrame     = 0;
    unsigned m_drawsNoRightTargetThisFrame = 0;
    unsigned m_dispatchesDuplicatedThisFrame = 0;
    // Right dispatches whose shared UAVs were twinned (BindCSUAVs), by compute shader CRC.
    unsigned m_sharedUavDispatchesThisFrame = 0;
    unsigned m_sharedUavSlotsThisFrame      = 0;
    std::map<DWORD, unsigned> m_sharedUavByCS;
    std::vector<unsigned char> m_drawPatchScratch;

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

    // The frame report's counts (EndFrame); no rendering decision reads them.
    unsigned             m_drawsThisFrame     = 0;
    unsigned             m_dispatchesThisFrame = 0;
    unsigned             m_cmdListsThisFrame  = 0;
    // CB writes with matrix targets vs without (duplication copies those through, the replay blind-scans them).
    unsigned             m_cbTargetedThisFrame   = 0;
    unsigned             m_cbUntargetedThisFrame = 0;
    // Within the targeted patches: individual matrices actually shifted, vs
    // rejected by the ortho / shadow-map guards in ShouldSkipProjectionMatrix.
    unsigned             m_cbMatShiftedThisFrame  = 0;
    unsigned             m_cbMatRejectedThisFrame = 0;
    // DetectInverseMatrix: scans, hits, inverses patched, learned inverses re-applied, and the UpdateSubresource
    // mirror's CB writes (seen / patched).
    unsigned             m_invScansThisFrame     = 0;
    unsigned             m_invHitsThisFrame      = 0;
    unsigned             m_invPatchedThisFrame   = 0;
    unsigned             m_invLearnedThisFrame   = 0;
    unsigned             m_updCBWritesThisFrame  = 0;
    unsigned             m_updCBPatchedThisFrame = 0;
    float                m_invBestMissThisFrame  = 1e30f;
    // Pixel and compute shader camera matrices for the right eye: buffers checked, inverses and previous cameras patched,
    // by shader CRC; and what each (shader, slot, register) was found to hold, kept for its later draws and dispatches.
    unsigned             m_invDrawChecksThisFrame  = 0;
    unsigned             m_invDrawPatchedThisFrame = 0;
    unsigned             m_fwdDrawPatchedThisFrame = 0;
    std::map<DWORD, unsigned> m_invDrawByPS;
    // Overlay draws and the right copies they restored; plain constants restored for shaders reading no matrix there;
    // learned forward shifts withheld at write time (the draw patch serves the consumer instead).
    unsigned             m_overlayDrawsThisFrame     = 0;
    unsigned             m_overlayRestoredThisFrame  = 0;
    unsigned             m_plainRestoredThisFrame    = 0;
    unsigned             m_learnedWithheldThisFrame  = 0;
    struct PsCameraMatrix { bool inverse; bool transposed; bool viewport = false; };
    std::map<unsigned long long, PsCameraMatrix> m_psCameraMatrices;
    // Why a CB write had no targets: no VS bound, VS unparsed, VS without matrices, buffer at no VS slot, at a VS slot
    // without a matrix; and the bindings dropped by state resets.
    unsigned             m_untargetedWhy[5] = {};
    unsigned             m_bindResetsThisFrame = 0;
    // Draw-level stereo coverage: why a draw's geometry could not be corrected.
    unsigned             m_drawsVSUnparsedThisFrame     = 0;
    unsigned             m_drawsVSNoMatrixThisFrame     = 0;
    unsigned             m_drawsVSNeverShiftedThisFrame = 0;
    unsigned             m_drawsVSStaleShiftThisFrame   = 0;
    unsigned             m_drawsVSNoSiblingCBThisFrame  = 0;
    // Draws per VS CRC for geometry that reached the right eye uncorrected.
    std::map<DWORD, unsigned> m_uncorrectedVSDraws;
    // CollectDebugInformation: this frame's CPU time by part, in QueryPerformanceCounter ticks; summed over the report
    // window with the frame interval, then logged per frame (Context11Proxy_Utils.cpp).
    struct FrameTiming
    {
        LONGLONG leftDraws, coverageTally, gameCalls, rightConstants, rightBinds, rightFeatures, rightCalls, rightTeardown,
                 drawDone, maps, mapDriver, mapRead, mapBytes, updates, tracking, dispatches, rightFlush, syncBuild, syncApply,
                 recordBegin, output, present, frame;
    };
    LONGLONG    m_timeScratch = 0;   // a ScopedTicks target for work no row reports
    FrameTiming m_time = {}, m_timeWindow = {};
    // <BatchRightEye>: the recorder (RightRecorder11) takes the game's state sets while it records (m_mirror); a right-eye
    // draw records into it with m_real swapped to it, and FlushRightBatch replays it on the immediate context.
    RightRecorder11*      m_recorder = nullptr;
    GpuTimer11*           m_gpuTimer = nullptr;   // CollectDebugInformation
    std::string           PassLabel() const;      // the bound left target, for the GPU row
    ID3D11DeviceContext*  m_mirror = nullptr;      // the recorder outside a right-eye draw
    ID3D11DeviceContext1* m_mirror1 = nullptr;
    ID3D11DeviceContext*  m_immediate = nullptr;   // the real context while a right-eye draw records
    ID3D11DeviceContext1* m_immediate1 = nullptr;
    bool     m_rightPending = false, m_rightRecording = false, m_soBound = false, m_syncSkipGeometry = false;
    unsigned m_batchedDrawsThisFrame = 0, m_unbatchedDrawsThisFrame = 0, m_batchFlushes[kFlushReasons] = {};
    unsigned m_updReplayTex = 0, m_updReplayBuf = 0, m_updReplayCB = 0, m_updReplayBufBinds = 0;   // update replays by target
    D3D11_VIEWPORT m_gameVp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
    UINT           m_gameVpCount = 0;   // the game's last RSSetViewports (0 after ClearState)
    D3D11_RECT     m_gameSc[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
    UINT           m_gameScCount = 0;   // the game's last RSSetScissorRects (0 after ClearState)
    std::vector<unsigned char> m_cbScratch;   // a CB write's bytes (Unmap, UpdateSubresource), reused
    // Game buffers a recorded draw reads in place (LightViewPasses' left constants), a ring by its bound range (n 0 =
    // the whole buffer): writing one replays the recording.
    struct PendingLeft { Buffer11Proxy* b; size_t off, n; };
    std::vector<PendingLeft> m_pendingLeftCBs;
    bool RightBatchOn() const;
    void BeginRightRecording();
    void EndRightRecording();
    void StopRightBatch(const char* why);
    bool PendingLeftCB(Buffer11Proxy* b) const;
    bool PendingLeftRange(ID3D11Buffer* real, size_t off, size_t n) const;
    bool IAOnlyUnread(ID3D11Buffer* real) const;   // a vertex/index-only buffer no recorded draw can read
    ID3D11DeviceContext* RightWriter();
    LONGLONG    m_frameStart = 0, m_frameMax = 0;
    unsigned    m_timedFrames = 0, m_gpuBehindSum = 0, m_gpuBehindMax = 0;
    // CollectDebugInformation: driver Maps over 0.1 ms in the report window, by resource kind (which ones wait on the GPU).
    struct SlowMaps { LONGLONG ticks = 0; unsigned count = 0; };
    std::map<std::string, SlowMaps> m_slowMaps;
    void NoteSlowMap(ID3D11Resource* real, D3D11_MAP type, LONGLONG t0, bool right);
    void AccumulateFrameTiming();
    void TallyDrawCoverage();
    void TeachBoundMatrices(bool screenSpace);
    void LogFrameStats(unsigned frame);
    void PatchBoundCBsForDraw();
    void PatchCameraMatrices(StageIdx stage);
    void UnshiftOverlayCBs();
    void RestoreRightCB(Buffer11Proxy* b);
    bool WriteWholeRightCB(Buffer11Proxy* b, const std::vector<unsigned char>& bytes);
    // ConstantBlocks: bound constant blocks (a small buffer whole, or a ring's 11.1 range) rebuilt for the right eye at
    // the draw or dispatch, and a ring's ranges copied to the real left buffer.
    void SyncBoundConstants(bool right, bool plainOnly, bool fromDispatch);
    bool WriteRingRange(ID3D11Buffer* real, size_t off, const unsigned char* p, size_t n);
    void MergeRightRing(Buffer11Proxy* b, size_t off, unsigned char* p, size_t n, float eyeShift);
    bool ClassifyDeclaredMatrix(const ShaderAnalysis11Result* info, UINT slot, DWORD reg, const float* f, EyeShiftMatrix& out);
    bool BareProjectionKept(StageIdx st, const ShaderAnalysis11Result* info, UINT slot, const float* f,
                            const EyeShiftMatrix& em);
    bool ClassifyViewMatrix(const ShaderAnalysis11Result* info, UINT slot, size_t off, const unsigned char* block, size_t n,
                            EyeShiftMatrix& out);
    // A GPU copy into a ring bypasses the shadow: the copied range is read back from the real buffer a frame later
    // (no stall) and the shadow takes it; until then its pages are foreign and left alone.
    struct RingReadback { Buffer11Proxy* b; size_t off, n; unsigned frame; };
    std::vector<RingReadback> m_ringReadbacks;
    std::map<UINT, ID3D11Buffer*> m_ringStaging;   // readback staging buffers by size
    void NoteRingCopy(Buffer11Proxy* b, size_t off, size_t n);
    void ServiceRingReadbacks();
    bool RingReadBack(ID3D11Buffer* b, size_t off, size_t n, unsigned char* out);
    void BeginDraw(UINT vertexCount);
    // ClipSpaceSprites: a vertex shader passing NDC positions through (w = 1) gets, for the right eye only, a geometry
    // shader that moves x by the eye offset at the vertex's own depth; one per such vertex shader, null = unbuildable.
    std::map<DWORD, ID3D11GeometryShader*> m_clipSpaceGS;
    ID3D11Buffer* m_clipSpaceCB = nullptr;
    bool          m_clipSpaceBound = false;
    float         m_clipSpaceK[4] = {};
    unsigned      m_clipSpaceDrawsThisFrame = 0, m_clipSpaceNoCameraThisFrame = 0;
    ID3D11GeometryShader* ClipSpaceShiftShader(const ShaderAnalysis11Result* info);
    // Right-eye shader twins (ShaderPatch11) by stage, plan tag and game CRC (null shader = unpatchable). One is bound for
    // one right draw or dispatch with its constants in m_twinCB; UnbindTwin restores the game's shader and slot binding.
    struct ShaderTwin { ID3D11DeviceChild* shader = nullptr; UINT slot = 0; };
    std::map<unsigned long long, ShaderTwin> m_twins;
    ID3D11Buffer*       m_twinCB = nullptr;
    int                 m_twinStage = -1;            // the stage a twin is bound on, -1 = none
    ID3D11DeviceChild*  m_twinSavedShader = nullptr;
    ID3D11Buffer*       m_twinSavedCB = nullptr;
    UINT                m_twinSavedSlot = 0, m_twinSavedFirst = 0, m_twinSavedNum = 0;
    float               m_twinConst[16] = {};        // the bound twin's constants
    UINT                m_twinConstCount = 0;
    DWORD               m_twinCrc = 0;
    int                 m_twinTag = 0;
    bool UnbindTwin();
    // ClipSpaceSprites 3, the first twin client: sprites that read their world position (SpriteDepth11).
    unsigned            m_spriteTwinDrawsThisFrame = 0;
    bool BindSpriteDepth(const ShaderAnalysis11Result* info);
    bool SpriteDepthRow(const ShaderAnalysis11Result* info, float row[4]);
    // ClipSpaceRays: the same for a pass-through vertex shader carrying a world ray per vertex; the geometry shader
    // moves the ray by the eye offset at infinity, the position stays (the sky quad keeps covering the screen).
    std::map<DWORD, ID3D11GeometryShader*> m_clipSpaceRayGS;
    bool          m_clipSpaceRays = false;
    unsigned      m_clipSpaceRayDrawsThisFrame = 0;
    ID3D11GeometryShader* ClipSpaceRayShader(const ShaderAnalysis11Result* info);
    // TiledLightVolumes: a light volume's view-space outputs moved into the right camera's view, and a private copy of the
    // pixel stage's cb0 with the tile rays' x offset moved by the eye shift.
    std::map<DWORD, ID3D11GeometryShader*> m_lightVolumeGS;
    ID3D11Buffer* m_lightVolumePsCB = nullptr;
    UINT          m_lightVolumePsCBSize = 0;
    unsigned      m_lightVolumeDrawsThisFrame = 0, m_lightVolumeRefusedThisFrame = 0;
    bool          m_lightVolumeBound = false;
    ID3D11GeometryShader* LightVolumeShader(const ShaderAnalysis11Result* info, int kind);
    // ClipSpaceSprites 1: a copy of the right depth buffer and its shader view, per right DSV, for sprites without a z.
    struct ClipSpaceDepth { ID3D11Texture2D* tex; ID3D11ShaderResourceView* srv; UINT w, h; unsigned frame; };
    std::map<ID3D11DepthStencilView*, ClipSpaceDepth> m_clipSpaceDepth;
    bool ClipSpaceDepthViews(ClipSpaceDepth& out);
    bool SceneZRowFromBoundVS(float& m22, float& m23);

    // The bound vertex shader (a lookup key, not AddRef'd) and its constant buffers (referenced; the draw path queries
    // them): Unmap shifts only the registers that shader reads as projection matrices in the buffer being written.
    static constexpr UINT kMaxVSCBSlots = 15;
    ID3D11VertexShader*  m_boundVS;
    ID3D11Buffer*        m_boundVSCBs[kMaxVSCBSlots];

    // The same snapshot for the pixel stage: BaseProfile.xml keys hand-written matrix declarations on <PixelShader CRC>,
    // so the bound PS and its CB slots must be known.
    static constexpr UINT kMaxPSCBSlots = 15;
    ID3D11PixelShader*   m_boundPS;
    ID3D11Buffer*        m_boundPSCBs[kMaxPSCBSlots];

    // Analysis of the bound vertex [0], pixel [1] and compute [2] shader, taken when the game sets them.
    const ShaderAnalysis11Result* m_boundInfo[3] = {};
    // The bound domain shader and its analysis: tessellated geometry projects there, not in the vertex shader.
    ID3D11DomainShader*           m_boundDS     = nullptr;
    const ShaderAnalysis11Result* m_boundDSInfo = nullptr;
    // The game's geometry and hull shaders (keys only): a right draw adds its own geometry shader only where the game
    // has none (ClipSpaceSprites).
    ID3D11GeometryShader*         m_boundGS     = nullptr;
    ID3D11HullShader*             m_boundHS     = nullptr;
    void TrackStageShader(StageIdx stage, void* shader);
    void PatchStageCBsForDraw(StageIdx stage, void* shader, const ShaderAnalysis11Result* info, float eyeShift, float conv);
    unsigned                      m_dsBuffersPatchedThisFrame = 0;

    // TryBuildModifiedVS DX11 Phase 2: per-eye binding of modified vertex
    // shaders produced by Device11Proxy. On the right-eye replay of a Draw
    // for a shader with a modified sibling, we temporarily swap in the
    // modified VS and bind our per-eye-info CB to the register the modified
    // shader expects to read eye data from, then restore the game's VS so
    // subsequent closures see consistent state. The eye-info CB is a 16-
    // byte D3D11_USAGE_DYNAMIC buffer created lazily on first use; its
    // contents mirror the DX10 DDI legacy ConstantBufferWrapper method 2
    // layout: [sep, -(1/ZPS + convShift)^2, 0, StereoZNear].
    ID3D11Buffer* m_eyeInfoCB = nullptr;
    bool          BeginRightEyeShaderSwap(ID3D11VertexShader* vsAtDraw,
                                          ID3D11VertexShader*& outOrig, UINT& outSlot);
    void          EndRightEyeShaderSwap(ID3D11VertexShader* orig);
    void          UpdateEyeInfoCB(const wiz3d::ModifiedVS11& mod);

    // The game's per-game behaviour (ProfileFeature.h), built from its profile on first use.
    ProfileFeatures m_features;
    bool            m_featuresBuilt = false;
    ProfileFeatures& Features();
    // The features' edits of the bound shaders' noted cbuffers, for a right draw or dispatch (EditRightConstants).
    void EditRightCBs(bool compute);
    bool OutputAtPresentAspect(bool compute) const;
    void BindRightCB(StageIdx st, UINT slot, ID3D11Buffer* b);
    void BindLeftSRVs(bool compute);
    unsigned m_leftSRVsThisFrame = 0;
    std::vector<UINT> m_substitutedCSSRVs;   // slots a feature replaced for the current right dispatch
    std::vector<unsigned char> m_editScratch;
    bool                       m_editAtDispatch = false, m_editMovedOrigin = false, m_editRetry = false;
    const ShaderAnalysis11Result* m_editShader = nullptr;
    // Every write to a CB's right sibling counts here, so a feature's draw-time rewrite knows when it was overwritten.
    void NoteRightConstants(Buffer11Proxy* b);
    // CB writes this frame that took a flipped eye shift (inside an x-flipped target).
    unsigned m_flippedWritesThisFrame = 0;

public:
    // ---- What profile features use (ProfileFeature.h) ----------------------
    ID3D11DeviceContext* Real() const   { return m_real; }
    Device11Proxy*       Parent() const { return m_parent; }
    // BatchRightEye: a state set a feature makes around the left draw goes to Mirror() too (null when not recording);
    // inside a recorded right-eye draw nothing can be read back, so the game's viewports come from GameViewports.
    ID3D11DeviceContext* Mirror() const { return m_mirror; }
    bool                 Recording() const { return m_rightRecording; }
    UINT                 GameViewports(D3D11_VIEWPORT* out) const
    { for (UINT i = 0; i < m_gameVpCount; ++i) out[i] = m_gameVp[i]; return m_gameVpCount; }
    UINT                 GameScissorRects(D3D11_RECT* out) const
    { for (UINT i = 0; i < m_gameScCount; ++i) out[i] = m_gameSc[i]; return m_gameScCount; }
    // The geometry stage's binding at a slot as the drawing eye has it (BindEye rebinds a stage holding a stereo view).
    ID3D11Buffer* GSConstantBufferNow(UINT slot) const;
    ID3D11ShaderResourceView* GSResourceNow(UINT slot) const;
    // The analysis of the bound vertex (pixel = false) or pixel shader, null if unknown.
    const ShaderAnalysis11Result* BoundShader(bool pixel) const { return m_boundInfo[pixel ? 1 : 0]; }
    // The game's constant buffer / shader resources at a vertex or pixel shader slot (left = the game's, right = the
    // right eye's), null when none.
    Buffer11Proxy*            BoundCB(bool pixel, UINT slot) const;
    ID3D11ShaderResourceView* LeftSRV(bool pixel, UINT slot) const;
    ID3D11ShaderResourceView* RightSRV(bool pixel, UINT slot) const;
    UINT                      NumTargets() const { return m_numRTVs; }
    // The bound pass draws an overlay (HUD, menu): into the back buffer, without a depth test.
    bool                      OverlayPass() const { return m_omBackBuffer && !m_depthTestOn; }
    ID3D11RenderTargetView*   LeftTarget(UINT i) const { return i < kMaxRTVs ? m_rtvLeft[i] : nullptr; }
    ID3D11DepthStencilView*   LeftDepth() const { return m_dsvLeft; }
    // The game's own geometry stage (GS, HS or DS) is bound; the draw's primitive topology and vertex count.
    bool                      GeometryStageBound() const { return m_boundGS || m_boundHS || m_boundDS; }
    D3D11_PRIMITIVE_TOPOLOGY  Topology() const { return m_topology; }
    UINT                      DrawVertices() const { return m_drawVertices; }
    // The right copy reads the left eye's VS/HS/DS/GS constants (a light's view is the same for both eyes).
    void                      BindLeftGeometryConstants();
    // Writes a whole CB's right sibling (a feature's rewrite of the game's last bytes); counts as a right write.
    bool                      WriteRightConstants(Buffer11Proxy* b, const unsigned char* data, size_t size, int variant = 0);
    // Over every feature: the eye shift's sign, the scene camera's p00 (0 = unknown), a light's-view pass.
    float                     EyeShiftSign() const;
    float                     CameraP00() const;
    bool                      LightViewPass() const;
    bool                      VertexCameraMatrices() const;
    bool                      RingKeepsShifts() const;
    bool                      KeepBareProjections() const;
    bool                      WholeBufferRanges() const;
    bool                      ViewMatrices() const;
    bool                      UiProjectionParallax() const;
    // Over every feature: the bound camera and the right camera's move (ProfileFeature::BoundCameraMove).
    bool                      BoundCameraMove(CameraMove& out);
    // During EditRightConstants: the edit is for a dispatch (else a draw), and the buffer the bound shaders read as a
    // noted cbuffer (null = none).
    bool                      EditAtDispatch() const { return m_editAtDispatch; }
    // During EditRightConstants: the shader whose cbuffer is edited ($Globals is laid out per shader).
    const ShaderAnalysis11Result* EditingShader() const { return m_editShader; }
    Buffer11Proxy*            BoundNamedCB(const std::string& cbuffer) const;
    // A bound shader carries a feature's tag; the pass sees the right camera with its origin moved; a feature lacked
    // what it needs (the edit is redone at the next binding rather than kept).
    bool                      BoundShadersTagged(const std::string& name) const;
    bool                      MovedOrigin() const { return m_editMovedOrigin; }
    void                      RetryEdit() { m_editRetry = true; }
    // The bound compute shader; the SRV its right dispatch reads at a slot; one bound in its place for this right
    // dispatch only (the left one is put back after it).
    const ShaderAnalysis11Result* BoundCompute() const { return m_boundInfo[2]; }
    ID3D11ShaderResourceView* ComputeSRVForRight(UINT slot) const;
    void                      SubstituteComputeSRV(UINT slot, ID3D11ShaderResourceView* v);
    // ClipSpaceSprites, at a right draw: the depth shift's geometry shader bound for a pass-through vertex shader
    // (true = bound); taken off after the draw; this frame's counts (shifted draws, draws without a scene camera).
    bool                      BindClipSpaceShift(int mode);   // 2 = no depth read: a z of 0 is at infinity
    void                      UnbindClipSpaceShift();
    bool                      BindClipSpaceRayShift();
    unsigned                  ClipSpaceRayCount() { const unsigned n = m_clipSpaceRayDrawsThisFrame; m_clipSpaceRayDrawsThisFrame = 0; return n; }
    // <TiledLightVolumes>: kind 1 point, 2 spot, 3 box; rayReg = the pixel shader's cb0 register of x/z = px * .x + .z.
    bool                      BindLightVolumeShift(int kind, UINT rayReg);
    void                      UnbindLightVolumeShift();
    void                      LightVolumeCounts(unsigned& shifted, unsigned& refused)
    { shifted = m_lightVolumeDrawsThisFrame; refused = m_lightVolumeRefusedThisFrame; m_lightVolumeDrawsThisFrame = m_lightVolumeRefusedThisFrame = 0; }
    void                      ClipSpaceCounts(unsigned& shifted, unsigned& noCamera)
    { shifted = m_clipSpaceDrawsThisFrame; noCamera = m_clipSpaceNoCameraThisFrame; m_clipSpaceDrawsThisFrame = m_clipSpaceNoCameraThisFrame = 0; }
    unsigned                  SpriteTwinCount() { const unsigned n = m_spriteTwinDrawsThisFrame; m_spriteTwinDrawsThisFrame = 0; return n; }
    // Shader twins' plan tags (ShaderPatch11 clients).
    enum TwinTag { kTwinSpriteDepth = 1, kTwinFogVolume = 2 };
    // A twin of the bound `st` shader under `plan` (built once per tag and CRC), bound for the coming right draw or
    // dispatch with `count` floats (16 at most) in its cbuffer; false = none (unpatchable, no code kept).
    bool                      BindTwin(StageIdx st, const ShaderAnalysis11Result* info, int tag, const ShaderPatchPlan& plan,
                                       const float* consts, UINT count);
    // The same for the bound pixel shader, from a feature's OnRightDraw.
    bool                      BindPixelTwin(int tag, const ShaderPatchPlan& plan, const float* consts, UINT count)
    { return BindTwin(ST_PS, m_boundInfo[1], tag, plan, consts, count); }
    unsigned                  FlippedWritesThisFrame() const { return m_flippedWritesThisFrame; }
private:

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
    };
    std::vector<ActiveMap> m_activeMaps;
};

} // namespace wiz3d
