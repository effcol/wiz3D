/* wiz3D - ID3D10Buffer proxy implementation (Option B for DX10) */

#include "StdAfx.h"
#include "Buffer10Proxy.h"
#include "Device10Proxy.h"
#include "proxy_factory.h"
#include "../S3DAPI/GlobalData.h"
#include <vector>

#pragma comment(lib, "dxguid.lib")

namespace wiz3d
{

// 4c10: same projection-matrix-shaped 4x4 float scan as Context11Proxy's
// ApplyEyeShiftToCB. Duplicated here rather than shared because the DX11
// version is static inside Context11Proxy.cpp; keeping it local keeps the
// DX10 path independent of the DX11 .obj.
static void ApplyEyeShiftToCB10(unsigned char* data, size_t byteCount, float eyeShift)
{
    if (eyeShift == 0.f) return;
    constexpr size_t kMat4Bytes = 16 * sizeof(float);
    if (byteCount < kMat4Bytes) return;
    for (size_t off = 0; off + kMat4Bytes <= byteCount; off += 4)
    {
        float* f = reinterpret_cast<float*>(data + off);
        if (f[11] != 1.f) continue;
        if (f[15] != 0.f) continue;
        if (f[0]  == 0.f) continue;
        if (f[5]  == 0.f) continue;
        f[8] += eyeShift * f[0];
    }
}

// Stage 4e DX10: targeted CB modifier driven by analyzer data. matrixRegister
// is the constant register index inside the CB (each register = 16 bytes / 4
// floats). matrixIsTransposed picks row- vs column-major slot layout — see
// Context11Proxy's ApplyTargetedEyeShiftToCB for the geometry.
struct EyeShiftMatrix10
{
    DWORD matrixRegister;
    BOOL  matrixIsTransposed;
};

static void ApplyTargetedEyeShiftToCB10(unsigned char* data, size_t byteCount,
                                        float eyeShift,
                                        const std::vector<EyeShiftMatrix10>& matrices)
{
    if (eyeShift == 0.f || matrices.empty()) return;
    constexpr size_t kRegBytes  = 16;
    constexpr size_t kMat4Bytes = 4 * kRegBytes;
    for (const auto& m : matrices)
    {
        size_t base = size_t(m.matrixRegister) * kRegBytes;
        if (base + kMat4Bytes > byteCount) continue;
        float* f = reinterpret_cast<float*>(data + base);
        float xScale = f[0];
        if (xScale == 0.f) continue;
        if (m.matrixIsTransposed) f[2] += eyeShift * xScale;
        else                      f[8] += eyeShift * xScale;
    }
}

Buffer10Proxy::Buffer10Proxy(ID3D10Buffer* real, Device10Proxy* parent)
    : m_real(real)
    , m_parent(parent)
    , m_refs(1)
    , m_vsBound(false)
    , m_activeMapValid(false)
    , m_activeMapData(nullptr)
    , m_activeMapByteWidth(0)
    , m_activeMapType(D3D10_MAP_WRITE_DISCARD)
{
    // Remaining Option A (DX11-shape) fields default-initialized in the header.
}

Buffer10Proxy::~Buffer10Proxy()
{
    if (m_realRightMoved) { m_realRightMoved->Release(); m_realRightMoved = nullptr; }
    if (m_realRight)      { m_realRight->Release();      m_realRight      = nullptr; }
    if (m_real)           { m_real->Release();           m_real           = nullptr; }
}

// Option A: lazy-create the right-eye sibling buffer with the same desc as the
// game's left buffer. Called by Device10Proxy::EditRightCBs (Cycle 2) when a
// per-draw right-eye edit needs its own buffer to bind rather than rewrite the
// left in-place. variant 0 is the standard sibling; variant 1 is a second
// sibling for passes that see the right camera with its origin moved.
ID3D10Buffer* Buffer10Proxy::RightCopy(int variant)
{
    ID3D10Buffer*& slot = (variant & 1) ? m_realRightMoved : m_realRight;
    if (slot) return slot;
    if (!m_real || !m_parent) return nullptr;

    D3D10_BUFFER_DESC desc;
    m_real->GetDesc(&desc);
    // Siblings never participate in the game's own Map/Unmap shadow pipeline,
    // they receive bytes via the eye-shift path. Keep the same usage/bind so
    // binding at the exact same slot works, but drop any STAGING requirement
    // games sometimes bake in -- siblings should be DEFAULT for GPU-side edits.

    ID3D10Device* dev = m_parent->GetReal();
    if (!dev) return nullptr;

    ID3D10Buffer* sibling = nullptr;
    if (FAILED(dev->CreateBuffer(&desc, nullptr, &sibling)) || !sibling)
        return nullptr;

    slot = sibling;
    return slot;
}

ULONG STDMETHODCALLTYPE Buffer10Proxy::Release()
{
    LONG r = InterlockedDecrement(&m_refs);
    if (r == 0) delete this;
    return (ULONG)r;
}

HRESULT STDMETHODCALLTYPE Buffer10Proxy::QueryInterface(REFIID riid, void** ppvObj)
{
    if (!ppvObj) return E_POINTER;
    if (riid == IID_IUnknown        ||
        riid == IID_ID3D10DeviceChild ||
        riid == IID_ID3D10Resource  ||
        riid == IID_ID3D10Buffer)
    {
        *ppvObj = static_cast<ID3D10Buffer*>(this);
        AddRef();
        return S_OK;
    }
    if (riid == IID_wiz3D_Buffer10Proxy)
    {
        *ppvObj = static_cast<IUnknown*>(static_cast<ID3D10Buffer*>(this));
        AddRef();
        return S_OK;
    }
    return m_real->QueryInterface(riid, ppvObj);
}

void STDMETHODCALLTYPE Buffer10Proxy::GetDevice(ID3D10Device** ppDevice)
{
    if (!ppDevice) return;
    if (m_parent)
    {
        *ppDevice = static_cast<ID3D10Device*>(m_parent);
        m_parent->AddRef();
        return;
    }
    m_real->GetDevice(ppDevice);
}

HRESULT STDMETHODCALLTYPE Buffer10Proxy::Map(D3D10_MAP MapType, UINT MapFlags, void** ppData)
{
    HRESULT hr = m_real->Map(MapType, MapFlags, ppData);
    if (FAILED(hr) || !ppData || !*ppData) return hr;

    m_activeMapValid = false;
    if (!gInfo.UseCOMWrapReplay) return hr;
    if (!m_parent || !m_parent->IsPresentHookActive()) return hr;
    if (!m_vsBound) return hr;

    if (MapType != D3D10_MAP_WRITE_DISCARD &&
        MapType != D3D10_MAP_WRITE &&
        MapType != D3D10_MAP_WRITE_NO_OVERWRITE &&
        MapType != D3D10_MAP_READ_WRITE)
        return hr;

    D3D10_BUFFER_DESC desc;
    m_real->GetDesc(&desc);
    if ((desc.BindFlags & D3D10_BIND_CONSTANT_BUFFER) == 0) return hr;
    if (desc.ByteWidth == 0) return hr;

    m_activeMapValid     = true;
    m_activeMapData      = *ppData;
    m_activeMapByteWidth = desc.ByteWidth;
    m_activeMapType      = MapType;
    return hr;
}

void STDMETHODCALLTYPE Buffer10Proxy::Unmap()
{
    if (m_activeMapValid && m_activeMapData && m_activeMapByteWidth && m_parent)
    {
        std::vector<unsigned char> bytes(m_activeMapByteWidth);
        memcpy(bytes.data(), m_activeMapData, m_activeMapByteWidth);
        D3D10_MAP mt = m_activeMapType;

        // Stage 4e DX10: consult the analyzer for the bound VS shader. If
        // it has known projection matrices in any CB slot that this buffer
        // is bound to (per Device10Proxy's m_boundVSCBs snapshot), build
        // a precise target list. Empty → falls back to the m[2][3]==1 /
        // m[3][3]==0 heuristic at replay time.
        std::vector<EyeShiftMatrix10> targets;
        if (ID3D10VertexShader* vs = m_parent->GetBoundVS())
        {
            if (const ShaderAnalysis11Result* info = m_parent->LookupShaderProjection(vs))
            {
                if (info->parsed)
                {
                    for (UINT slot = 0; slot < Device10Proxy::kMaxVSCBSlots; ++slot)
                    {
                        if (m_parent->GetBoundVSCB(slot) != static_cast<ID3D10Buffer*>(this))
                            continue;
                        auto cbIt = info->projection.matrixData.cb.find(slot);
                        if (cbIt == info->projection.matrixData.cb.end()) continue;
                        for (const auto& pmd : cbIt->second)
                        {
                            if (pmd.incorrectProjection) continue;
                            EyeShiftMatrix10 em;
                            em.matrixRegister     = pmd.matrixRegister;
                            em.matrixIsTransposed = pmd.matrixIsTransposed;
                            targets.push_back(em);
                        }
                    }
                }
            }
        }

        // Keep this proxy alive across the frame; the closure holds a ref.
        AddRef();
        Buffer10Proxy* self = this;
        m_parent->PushFrameCommand(
            [self, mt, bytes, targets]() {
                if (self->m_parent &&
                    self->m_parent->ActiveEye() == Device10Proxy::Eye::Right)
                {
                    void* mapped = nullptr;
                    if (SUCCEEDED(self->m_real->Map(mt, 0, &mapped)) && mapped)
                    {
                        memcpy(mapped, bytes.data(), bytes.size());
                        float eyeShift = wiz3D_GetEffectiveEyeShift();
                        if (!targets.empty())
                        {
                            ApplyTargetedEyeShiftToCB10(
                                static_cast<unsigned char*>(mapped),
                                bytes.size(), eyeShift, targets);
                        }
                        else
                        {
                            ApplyEyeShiftToCB10(static_cast<unsigned char*>(mapped),
                                                bytes.size(), eyeShift);
                        }
                        self->m_real->Unmap();
                    }
                }
                self->Release();
            });
    }
    m_activeMapValid = false;
    m_real->Unmap();
}

} // namespace wiz3d
