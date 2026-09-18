#include "StdAfx.h"
#include "Device11Proxy.h"
#include "Context11Proxy.h"
#include "DXGIDeviceProxy.h"
#include "Texture2D11Proxy.h"
#include "RTV11Proxy.h"
#include "DSV11Proxy.h"
#include "Buffer11Proxy.h"
#include "SRV11Proxy.h"
#include "UAV11Proxy.h"
#include "Texture1D11Proxy.h"
#include "Texture3D11Proxy.h"
#include "StereoHeuristic.h"
#include "DxbcRebuild.h"                   // DxbcSelfTest
#include "ShaderModify11.h"                // TryModifyShaderForStereo
#include "..\S3DAPI\ReadData.h"            // ReadCurrentProfile
#include "..\S3DAPI\ShaderProfileData.h"   // g_ProfileData
#include "proxy_factory.h"     // for IID_wiz3D_Device11Proxy
#include "AdapterFunctions.h"  // DDILog

#include <dxgi.h>
#include <dxgi1_2.h>
#include <dxgi1_3.h>

#pragma comment(lib, "dxguid.lib")

// Diagnostic macros — keep call sites mostly identical to NvDirectMode source
// so the port is a clean diff. Both route to wiz3D_proxy.log via DDILog.
#define LOG_VERBOSE(fmt, ...)         DDILog(fmt, ##__VA_ARGS__)
#define NVDM_TRACE_FIRST_N(n, fmt, ...) do { static int s_n = 0; if (s_n < (n)) { DDILog(fmt, ##__VA_ARGS__); ++s_n; } } while(0)

namespace wiz3d
{

// Vendor id drives per-vendor config sections; 0 is an acceptable fallback.
static DWORD GetVendorIdFromDevice(ID3D11Device* dev)
{
    DWORD vendor = 0;
    IDXGIDevice* dxgiDev = nullptr;
    if (dev && SUCCEEDED(dev->QueryInterface(__uuidof(IDXGIDevice),
                                             reinterpret_cast<void**>(&dxgiDev))) && dxgiDev)
    {
        IDXGIAdapter* adapter = nullptr;
        if (SUCCEEDED(dxgiDev->GetAdapter(&adapter)) && adapter)
        {
            DXGI_ADAPTER_DESC desc;
            if (SUCCEEDED(adapter->GetDesc(&desc))) vendor = desc.VendorId;
            adapter->Release();
        }
        dxgiDev->Release();
    }
    return vendor;
}

Device11Proxy::Device11Proxy(ID3D11Device* real)
    : m_real(real)
    , m_real1(nullptr)
    , m_real2(nullptr)
    , m_real3(nullptr)
    , m_ctxProxy(nullptr)
    , m_dxgiDeviceProxy(nullptr)
    , m_refs(1)
    , m_logicalWidth(0)
    , m_logicalHeight(0)
    , m_pBackBufferResource(nullptr)
{
    InitializeCriticalSection(&m_rtvSetLock);
    InitializeCriticalSection(&m_dxgiCacheLock);
    InitializeCriticalSection(&m_shaderProjLock);
    InitializeCriticalSection(&m_realToProxyLock);
    // Cache Device1/2/3 upgrades. QI failures leave the pointer null; we
    // refuse to claim the corresponding IID in our own QI when null.
    if (m_real)
    {
        if (FAILED(m_real->QueryInterface(__uuidof(ID3D11Device1), reinterpret_cast<void**>(&m_real1)))) m_real1 = nullptr;
        if (FAILED(m_real->QueryInterface(__uuidof(ID3D11Device2), reinterpret_cast<void**>(&m_real2)))) m_real2 = nullptr;
        if (FAILED(m_real->QueryInterface(__uuidof(ID3D11Device3), reinterpret_cast<void**>(&m_real3)))) m_real3 = nullptr;
        LOG_VERBOSE("  Device11Proxy ctor: real=%p real1=%p real2=%p real3=%p\n",
                    m_real, m_real1, m_real2, m_real3);
    }

    // Populate g_ProfileData. Only D3DDeviceWrapper (legacy DDI) and DX9 ever
    // called this, so under COM-wrap every per-game profile — mono shader
    // lists, matrix declarations — silently did nothing.
    static bool s_profileRead = false;
    if (!s_profileRead)
    {
        s_profileRead = true;
        // ReadConfig bails early when g_docConfig is null (it is, under
        // COM-wrap), so DefaultProfile presets never load and a game profile
        // without its own <Presets> would leave separation at zero. Keep the
        // camera state we already had; we only want the shader/matrix data.
        DataInput savedInput = gInfo.Input;
        ReadCurrentProfile(GetVendorIdFromDevice(m_real));
        // Restoring unconditionally also discarded presets a profile really did
        // supply, pinning every game to the built-in 0.16 / 0.5. Keep what the
        // profile gave us; fall back only when it gave nothing usable.
        const CameraPreset* loaded = gInfo.Input.GetActivePreset();
        if (!loaded || loaded->StereoBase == 0.f)
            gInfo.Input = savedInput;
        // Method 0 shifts via dp4 with the stereo projection's _31/_41 skew, which
        // only the DX9 renderer computes. Method 2 is x += sep*(w - conv), which
        // needs just separation and convergence — the values we actually have.
        if (gInfo.ModifyShadersDX11 && gInfo.VertexShaderModificationMethod != 2)
        {
            DDILog("  Device11Proxy: ModifyShadersDX11 forces VertexShaderModificationMethod 2 (was %d)\n",
                   (int)gInfo.VertexShaderModificationMethod);
            gInfo.VertexShaderModificationMethod = 2;
        }
        const CameraPreset* act = gInfo.Input.GetActivePreset();
        DDILog("  Device11Proxy: preset StereoBase=%.6f One_div_ZPS=%.6f (converge=%.2f units)\n",
               act ? act->StereoBase : 0.f, act ? act->One_div_ZPS : 0.f,
               (act && act->One_div_ZPS != 0.f) ? 1.f / act->One_div_ZPS : 0.f);
        DDILog("  Device11Proxy: ReadCurrentProfile done, VS=%u PS=%u GS=%u entries,"
               " eyeShift=%.6f\n",
               (unsigned)g_ProfileData.VSCRCData.size(),
               (unsigned)g_ProfileData.PSCRCData.size(),
               (unsigned)g_ProfileData.GSCRCData.size(),
               wiz3D_GetEffectiveEyeShift());
    }
}

Device11Proxy::~Device11Proxy()
{
    if (m_dxgiDeviceProxy)
    {
        // Detach parent first so any outstanding game-held refs on the
        // proxy can no longer call back into a destructed Device11Proxy
        // (their QI(ID3D11Device) returns E_NOINTERFACE).
        m_dxgiDeviceProxy->DetachParent();
        m_dxgiDeviceProxy->Release();
        m_dxgiDeviceProxy = nullptr;
    }
    if (m_real3) { m_real3->Release(); m_real3 = nullptr; }
    if (m_real2) { m_real2->Release(); m_real2 = nullptr; }
    if (m_real1) { m_real1->Release(); m_real1 = nullptr; }
    DeleteCriticalSection(&m_rtvSetLock);
    DeleteCriticalSection(&m_dxgiCacheLock);
    DeleteCriticalSection(&m_shaderProjLock);
    DeleteCriticalSection(&m_realToProxyLock);
}

void Device11Proxy::RegisterRealToProxy(void* realLeft, Texture2D11Proxy* proxy)
{
    if (!realLeft || !proxy) return;
    EnterCriticalSection(&m_realToProxyLock);
    m_realToProxy[realLeft] = proxy;
    LeaveCriticalSection(&m_realToProxyLock);
}

void Device11Proxy::UnregisterRealToProxy(void* realLeft)
{
    if (!realLeft) return;
    EnterCriticalSection(&m_realToProxyLock);
    m_realToProxy.erase(realLeft);
    LeaveCriticalSection(&m_realToProxyLock);
}

Texture2D11Proxy* Device11Proxy::LookupProxyByReal(void* realLeft) const
{
    if (!realLeft) return nullptr;
    auto* self = const_cast<Device11Proxy*>(this);
    EnterCriticalSection(&self->m_realToProxyLock);
    auto it = m_realToProxy.find(realLeft);
    Texture2D11Proxy* p = (it == m_realToProxy.end()) ? nullptr : it->second;
    LeaveCriticalSection(&self->m_realToProxyLock);
    return p;
}

void Device11Proxy::StoreShaderProjection(void* shaderPtr, const ShaderAnalysis11Result& info)
{
    if (!shaderPtr) return;
    EnterCriticalSection(&m_shaderProjLock);
    m_shaderProjections[shaderPtr] = info;
    LeaveCriticalSection(&m_shaderProjLock);
}

const ShaderAnalysis11Result* Device11Proxy::LookupShaderProjection(void* shaderPtr) const
{
    if (!shaderPtr) return nullptr;
    auto* self = const_cast<Device11Proxy*>(this);
    EnterCriticalSection(&self->m_shaderProjLock);
    auto it = m_shaderProjections.find(shaderPtr);
    const ShaderAnalysis11Result* p = (it == m_shaderProjections.end()) ? nullptr : &it->second;
    LeaveCriticalSection(&self->m_shaderProjLock);
    return p;
}

const Device11Proxy::ModifiedVS* Device11Proxy::LookupModifiedVS(ID3D11VertexShader* original) const
{
    if (!original) return nullptr;
    auto* self = const_cast<Device11Proxy*>(this);
    EnterCriticalSection(&self->m_shaderProjLock);
    auto it = m_modifiedVS.find(original);
    const ModifiedVS* p = (it == m_modifiedVS.end()) ? nullptr : &it->second;
    LeaveCriticalSection(&self->m_shaderProjLock);
    return p;
}

namespace {

template<typename TShader, typename CreateFn>
HRESULT AnalyzeAndCreate(const char* tag, Device11Proxy* self, CreateFn createReal,
                         const void* pBytecode, SIZE_T byteLength,
                         ID3D11ClassLinkage* pClassLinkage, TShader** ppShader)
{
    HRESULT hr = createReal(pBytecode, byteLength, pClassLinkage, ppShader);
    if (FAILED(hr) || !ppShader || !*ppShader) return hr;

    // One-time proof, against real game bytecode, that our checksum matches the
    // compiler's and that a rebuilt container is byte-identical to the original.
    static bool s_selfTested = false;
    if (!s_selfTested && pBytecode && byteLength > 32)
    {
        s_selfTested = true;
        DxbcSelfTest(pBytecode, byteLength);
    }

    ShaderAnalysis11Result info;
    const bool analyzed = AnalyzeShader11(pBytecode, byteLength, info);
    if (analyzed && !info.projection.matrixData.cb.empty())
    {
        NVDM_TRACE_FIRST_N(32,
            "  Device11Proxy::Create%sShader: CRC=0x%08lX shader=%p matrices in %u CB(s)\n",
            tag, info.crc32, *ppShader,
            (unsigned)info.projection.matrixData.cb.size());
    }
    else
    {
        NVDM_TRACE_FIRST_N(16,
            "  Device11Proxy::Create%sShader: CRC=0x%08lX shader=%p (no projection found, parsed=%d)\n",
            tag, info.crc32, *ppShader, (int)info.parsed);
    }

    // Store EVERY successfully-parsed result, including ones with no
    // projection matrices (Aug 2026). Previously we only stored shaders that
    // had matrices, which meant LookupShaderProjection returned null for two
    // very different situations: "analyzed, definitively has no projection"
    // and "never analyzed at all". Context11Proxy::Unmap could not tell them
    // apart, so a shader we had positively cleared still fell through to the
    // heuristic CB scan. Storing the negative result makes that distinction
    // available at the call site.
    // Store on CRC alone too: pixel shaders always fail analysis (no
    // SV_Position output) but BaseProfile.xml still matches them by CRC.
    // parsed stays false there, so analyzerKnows is unaffected.
    if (analyzed || info.crc32)
        self->StoreShaderProjection(*ppShader, info);
    return hr;
}

} // namespace

HRESULT STDMETHODCALLTYPE Device11Proxy::CreateVertexShader(
    const void* pShaderBytecode, SIZE_T BytecodeLength,
    ID3D11ClassLinkage* pClassLinkage, ID3D11VertexShader** ppVertexShader)
{
    HRESULT hr = AnalyzeAndCreate<ID3D11VertexShader>("Vertex", this,
        [this](const void* b, SIZE_T n, ID3D11ClassLinkage* cl, ID3D11VertexShader** pp) {
            return m_real->CreateVertexShader(b, n, cl, pp);
        },
        pShaderBytecode, BytecodeLength, pClassLinkage, ppVertexShader);

    if (gInfo.ModifyShadersDX11 && SUCCEEDED(hr) && ppVertexShader && *ppVertexShader)
        TryBuildModifiedVS(pShaderBytecode, BytecodeLength, pClassLinkage, *ppVertexShader);
    return hr;
}

// Builds the self-shifting variant of a vertex shader and keeps it beside the
// original. Only shaders the analyzer found no projection matrix in are worth
// modifying — the rest are already handled by targeted CB patching.
void Device11Proxy::TryBuildModifiedVS(const void* bytecode, SIZE_T byteLength,
                                       ID3D11ClassLinkage* linkage, ID3D11VertexShader* original)
{
    const ShaderAnalysis11Result* info = LookupShaderProjection(original);
    if (!info || !info->parsed) { ++m_vsModSkippedUnparsed; return; }
    if (!info->projection.matrixData.cb.empty()) { ++m_vsModSkippedHasMatrix; return; }
    if (gInfo.ModifyShadersMaxCount && m_vsModOk >= gInfo.ModifyShadersMaxCount) return;

    std::vector<BYTE> blob;
    ModifiedShaderData mdata;
    if (!TryModifyShaderForStereo(bytecode, byteLength, info->posRegister, false, blob, mdata))
    {
        ++m_vsModFailed;
        NVDM_TRACE_FIRST_N(8, "  TryBuildModifiedVS: CRC=0x%08lX modify FAILED\n", info->crc32);
        return;
    }

    ID3D11VertexShader* modified = nullptr;
    HRESULT hr = m_real->CreateVertexShader(blob.data(), blob.size(), linkage, &modified);
    if (FAILED(hr) || !modified)
    {
        ++m_vsModRejected;
        NVDM_TRACE_FIRST_N(8,
            "  TryBuildModifiedVS: CRC=0x%08lX runtime REJECTED modified shader hr=0x%08lX\n",
            info->crc32, hr);
        return;
    }

    ++m_vsModOk;
    DumpShaderPair(bytecode, byteLength, blob.data(), blob.size(), m_vsModOk, info->crc32);
    // Index is the bisection handle: with ModifyShadersMaxCount set, only
    // shaders below that index are modified, so the last one logged before a
    // crash narrows the culprit.
    DDILog("  TryBuildModifiedVS[%u]: CRC=0x%08lX OK cb=%u dp4Reg=%u (orig=%zu mod=%zu bytes)\n",
           m_vsModOk, info->crc32, mdata.CBIndex, mdata.dp4VectorRegister, byteLength, blob.size());

    EnterCriticalSection(&m_shaderProjLock);
    ModifiedVS& slot = m_modifiedVS[original];
    if (slot.shader) slot.shader->Release();   // shader pointer reuse after a Release
    slot.shader = modified;
    slot.data   = mdata;
    LeaveCriticalSection(&m_shaderProjLock);
}

void Device11Proxy::LogShaderModStats() const
{
    DDILog("  VS modification: ok=%u failed=%u rejected=%u skipped(hasMatrix)=%u skipped(unparsed)=%u\n",
           m_vsModOk, m_vsModFailed, m_vsModRejected,
           m_vsModSkippedHasMatrix, m_vsModSkippedUnparsed);
}

HRESULT STDMETHODCALLTYPE Device11Proxy::CreatePixelShader(
    const void* pShaderBytecode, SIZE_T BytecodeLength,
    ID3D11ClassLinkage* pClassLinkage, ID3D11PixelShader** ppPixelShader)
{
    static unsigned s_psDump = 0;
    DumpShaderBytecode("ps", pShaderBytecode, BytecodeLength, s_psDump++);
    return AnalyzeAndCreate<ID3D11PixelShader>("Pixel", this,
        [this](const void* b, SIZE_T n, ID3D11ClassLinkage* cl, ID3D11PixelShader** pp) {
            return m_real->CreatePixelShader(b, n, cl, pp);
        },
        pShaderBytecode, BytecodeLength, pClassLinkage, ppPixelShader);
}

HRESULT STDMETHODCALLTYPE Device11Proxy::CreateComputeShader(
    const void* pShaderBytecode, SIZE_T BytecodeLength,
    ID3D11ClassLinkage* pClassLinkage, ID3D11ComputeShader** ppComputeShader)
{
    static unsigned s_csDump = 0;
    DumpShaderBytecode("cs", pShaderBytecode, BytecodeLength, s_csDump++);
    return m_real->CreateComputeShader(pShaderBytecode, BytecodeLength,
                                       pClassLinkage, ppComputeShader);
}

HRESULT STDMETHODCALLTYPE Device11Proxy::CreateGeometryShader(
    const void* pShaderBytecode, SIZE_T BytecodeLength,
    ID3D11ClassLinkage* pClassLinkage, ID3D11GeometryShader** ppGeometryShader)
{
    return AnalyzeAndCreate<ID3D11GeometryShader>("Geometry", this,
        [this](const void* b, SIZE_T n, ID3D11ClassLinkage* cl, ID3D11GeometryShader** pp) {
            return m_real->CreateGeometryShader(b, n, cl, pp);
        },
        pShaderBytecode, BytecodeLength, pClassLinkage, ppGeometryShader);
}

HRESULT STDMETHODCALLTYPE Device11Proxy::CreateHullShader(
    const void* pShaderBytecode, SIZE_T BytecodeLength,
    ID3D11ClassLinkage* pClassLinkage, ID3D11HullShader** ppHullShader)
{
    return AnalyzeAndCreate<ID3D11HullShader>("Hull", this,
        [this](const void* b, SIZE_T n, ID3D11ClassLinkage* cl, ID3D11HullShader** pp) {
            return m_real->CreateHullShader(b, n, cl, pp);
        },
        pShaderBytecode, BytecodeLength, pClassLinkage, ppHullShader);
}

HRESULT STDMETHODCALLTYPE Device11Proxy::CreateDomainShader(
    const void* pShaderBytecode, SIZE_T BytecodeLength,
    ID3D11ClassLinkage* pClassLinkage, ID3D11DomainShader** ppDomainShader)
{
    return AnalyzeAndCreate<ID3D11DomainShader>("Domain", this,
        [this](const void* b, SIZE_T n, ID3D11ClassLinkage* cl, ID3D11DomainShader** pp) {
            return m_real->CreateDomainShader(b, n, cl, pp);
        },
        pShaderBytecode, BytecodeLength, pClassLinkage, ppDomainShader);
}

void Device11Proxy::RegisterBackBufferTexture(void* pTextureLike)
{
    m_pBackBufferResource = pTextureLike;
}

bool Device11Proxy::IsBackBufferResource(ID3D11Resource* p) const
{
    // ID3D11Texture2D -> ID3D11Resource is single-inheritance: same address.
    return p && static_cast<void*>(p) == m_pBackBufferResource;
}

void Device11Proxy::TrackBackBufferRTV(ID3D11RenderTargetView* rtv)
{
    if (!rtv) return;
    EnterCriticalSection(&m_rtvSetLock);
    m_backBufferRTVs.insert(rtv);
    LeaveCriticalSection(&m_rtvSetLock);
}

bool Device11Proxy::IsBackBufferRTV(ID3D11RenderTargetView* rtv) const
{
    if (!rtv) return false;
    // const_cast is fine here — only mutating the lock, the set is read-only
    // through this path.
    auto* self = const_cast<Device11Proxy*>(this);
    EnterCriticalSection(&self->m_rtvSetLock);
    bool found = m_backBufferRTVs.find(rtv) != m_backBufferRTVs.end();
    LeaveCriticalSection(&self->m_rtvSetLock);
    return found;
}

HRESULT STDMETHODCALLTYPE Device11Proxy::CreateRenderTargetView(
    ID3D11Resource* pResource, const D3D11_RENDER_TARGET_VIEW_DESC* pDesc,
    ID3D11RenderTargetView** ppRTView)
{
    // Stage 3b: probe pResource for a Texture2D11Proxy via the private IID.
    // On hit, unwrap to the real left-eye texture for the runtime's
    // CreateRTV; if the proxy also has a right-eye sibling, allocate a
    // matching right-eye RTV and bundle both into an RTV11Proxy returned
    // to the game.
    Texture2D11Proxy* texProxy   = TryUnwrapTexture2D(pResource);
    Buffer11Proxy*    bufProxy   = TryUnwrapBuffer(pResource);
    // Real-pointer fallback: De Blob and similar games QI our wrapped
    // texture for IDXGIResource, then QI that back to ID3D11Resource — which
    // returns the m_realLeft pointer. The probe above doesn't recognise raw
    // real pointers as ours. LookupProxyByReal closes that bypass.
    if (!texProxy && !bufProxy && pResource)
        texProxy = LookupProxyByReal(pResource);
    ID3D11Resource*   realToUse  = texProxy ? static_cast<ID3D11Resource*>(texProxy->GetReal())
                                 : bufProxy ? static_cast<ID3D11Resource*>(bufProxy->GetReal())
                                            : pResource;
    ID3D11Resource*   realRight  = texProxy ? static_cast<ID3D11Resource*>(texProxy->GetRealRight())
                                            : nullptr;

    HRESULT hr = m_real->CreateRenderTargetView(realToUse, pDesc, ppRTView);
    if (SUCCEEDED(hr) && ppRTView && *ppRTView)
    {
        // BB-RTV tracking still operates on the unwrapped game pointer so
        // the legacy back-buffer flow continues to work for shutdown logic.
        if (IsBackBufferResource(pResource))
        {
            TrackBackBufferRTV(*ppRTView);
            LOG_VERBOSE("  Device11Proxy::CreateRenderTargetView: BB-derived rtv=%p (resource=%p tracked)\n",
                        *ppRTView, pResource);
        }
        else
        {
            NVDM_TRACE_FIRST_N(8, "  Device11Proxy::CreateRenderTargetView: non-BB rtv=%p (resource=%p, our BB=%p)\n",
                               *ppRTView, pResource, m_pBackBufferResource);
        }

        // Always wrap RTVs that came from a Texture2D11Proxy so identity
        // tracking flows through the wrapper. Right-eye sibling created only
        // if the underlying resource was stereo.
        if (texProxy)
        {
            ID3D11RenderTargetView* realRightRTV = nullptr;
            if (realRight)
            {
                HRESULT hr2 = m_real->CreateRenderTargetView(realRight, pDesc, &realRightRTV);
                if (FAILED(hr2)) realRightRTV = nullptr;   // fall through to mono RTV
            }
            auto* rtvProxy = new RTV11Proxy(*ppRTView, realRightRTV, this);
            *ppRTView = static_cast<ID3D11RenderTargetView*>(rtvProxy);
            NVDM_TRACE_FIRST_N(16,
                "  Device11Proxy::CreateRenderTargetView: -> RTV11Proxy=%p stereo=%d\n",
                rtvProxy, (int)rtvProxy->IsStereo());
        }
    }
    return hr;
}

void Device11Proxy::SetImmediateContextProxy(Context11Proxy* ctxProxy)
{
    m_ctxProxy = ctxProxy;
}

HRESULT STDMETHODCALLTYPE Device11Proxy::QueryInterface(REFIID riid, void** ppvObj)
{
    if (!ppvObj) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_ID3D11Device)
    {
        *ppvObj = static_cast<ID3D11Device*>(this);
        AddRef();
        return S_OK;
    }

    // Private IID for cross-DLL identification (dxgi.dll's factory wrap
    // QIs incoming `pDevice` to detect a Device11Proxy). Returned as an
    // IUnknown* — the caller is expected to cast back via known type.
    if (riid == IID_wiz3D_Device11Proxy)
    {
        *ppvObj = static_cast<IUnknown*>(static_cast<ID3D11Device*>(this));
        AddRef();
        return S_OK;
    }

    // Stage 2: IDXGIDevice family routes through DXGIDeviceProxy so the
    // game's `device->QI(IDXGIDevice) ... ->QI(ID3D11Device)` round-trip
    // returns to *this* Device11Proxy rather than the unwrapped real
    // device. Without this, refcount math diverges and the CRT free()
    // crashes when the second Release fires (Tomb Raider 2013 etc.).
    if (riid == IID_IDXGIDevice  ||
        riid == IID_IDXGIDevice1 ||
        riid == IID_IDXGIDevice2 ||
        riid == IID_IDXGIDevice3)
    {
        DXGIDeviceProxy* dp = GetOrCreateDXGIDeviceProxyAddRef();
        if (!dp) return E_NOINTERFACE;
        // Route through the proxy's QI to get the right interface cast,
        // which also AddRefs. Drop the AddRef we got from the cache helper.
        HRESULT hr = dp->QueryInterface(riid, ppvObj);
        dp->Release();
        return hr;
    }

    // Claim Device1/2/3 with `this` so games QI'ing for the higher Device
    // versions land on our proxy instead of getting an unwrapped real device
    // back. The base ID3D11Device methods dispatch through the same vtable
    // slots regardless of which Device1+ view the caller holds, so existing
    // overrides apply automatically. Methods unique to Device1+ dispatch
    // through m_real1/2/3. Closes the dominant DX10/11 right-eye bypass
    // identified May 2026 via the per-frame trace.
    if (riid == __uuidof(ID3D11Device1) && m_real1)
    {
        *ppvObj = static_cast<ID3D11Device1*>(this);
        AddRef();
        return S_OK;
    }
    if (riid == __uuidof(ID3D11Device2) && m_real2)
    {
        *ppvObj = static_cast<ID3D11Device2*>(this);
        AddRef();
        return S_OK;
    }
    if (riid == __uuidof(ID3D11Device3) && m_real3)
    {
        *ppvObj = static_cast<ID3D11Device3*>(this);
        AddRef();
        return S_OK;
    }
    // Device4/5, vendor IIDs, etc.: pass through unwrapped. Identity won't
    // be preserved for these; extend if a game needs it.
    HRESULT hr = m_real->QueryInterface(riid, ppvObj);
    char iidName[64];
    FormatGUID(riid, iidName, sizeof(iidName));
    NVDM_TRACE_FIRST_N(16,
        "  Device11Proxy::QI(%s) hr=0x%08lX -- bypass risk\n", iidName, hr);
    return hr;
}

DXGIDeviceProxy* Device11Proxy::GetOrCreateDXGIDeviceProxyAddRef()
{
    EnterCriticalSection(&m_dxgiCacheLock);
    if (m_dxgiDeviceProxy)
    {
        m_dxgiDeviceProxy->AddRef();
        DXGIDeviceProxy* hit = m_dxgiDeviceProxy;
        LeaveCriticalSection(&m_dxgiCacheLock);
        return hit;
    }

    // Build the proxy by QI'ing each level on the real device. r0 is
    // mandatory; higher levels are best-effort.
    IDXGIDevice*  r0 = nullptr;
    IDXGIDevice1* r1 = nullptr;
    IDXGIDevice2* r2 = nullptr;
    IDXGIDevice3* r3 = nullptr;
    HRESULT hr0 = m_real->QueryInterface(IID_IDXGIDevice,  reinterpret_cast<void**>(&r0));
    if (FAILED(hr0) || !r0)
    {
        LeaveCriticalSection(&m_dxgiCacheLock);
        return nullptr;
    }
    m_real->QueryInterface(IID_IDXGIDevice1, reinterpret_cast<void**>(&r1));
    m_real->QueryInterface(IID_IDXGIDevice2, reinterpret_cast<void**>(&r2));
    m_real->QueryInterface(IID_IDXGIDevice3, reinterpret_cast<void**>(&r3));

    // ctor sets m_refs=1 — that's the cache's strong ref. AddRef once more
    // for the caller so we return with two outstanding refs (cache + caller).
    m_dxgiDeviceProxy = new DXGIDeviceProxy(this, r0, r1, r2, r3);
    m_dxgiDeviceProxy->AddRef();
    DXGIDeviceProxy* result = m_dxgiDeviceProxy;
    LeaveCriticalSection(&m_dxgiCacheLock);
    return result;
}

void STDMETHODCALLTYPE Device11Proxy::GetImmediateContext(ID3D11DeviceContext** ppImmediateContext)
{
    if (!ppImmediateContext) return;
    if (m_ctxProxy)
    {
        // Context11Proxy publicly inherits from ID3D11DeviceContext so this
        // is a real upcast — static_cast keeps /W4 + warnings-as-errors happy
        // (reinterpret_cast between related types triggers C4946).
        *ppImmediateContext = static_cast<ID3D11DeviceContext*>(m_ctxProxy);
        m_ctxProxy->AddRef();
        return;
    }
    // Fallback: game calls GetImmediateContext before we've stashed a proxy
    // (shouldn't happen via the standard D3D11CreateDevice path, but defensive).
    m_real->GetImmediateContext(ppImmediateContext);
}

// GetImmediateContext1/2/3: return our wrapped Context cast as the higher
// interface. Context11Proxy was extended to implement ID3D11DeviceContext3
// so the cast is valid. Required so games using Device1+ get a wrapped
// Context — without this they'd get the real unwrapped Context1+ and all
// state-setter calls would bypass our record-for-replay logic.
void STDMETHODCALLTYPE Device11Proxy::GetImmediateContext1(ID3D11DeviceContext1** ppImmediateContext)
{
    if (!ppImmediateContext) return;
    if (m_ctxProxy)
    {
        *ppImmediateContext = static_cast<ID3D11DeviceContext1*>(m_ctxProxy);
        m_ctxProxy->AddRef();
        return;
    }
    if (m_real1) m_real1->GetImmediateContext1(ppImmediateContext);
    else *ppImmediateContext = nullptr;
}

void STDMETHODCALLTYPE Device11Proxy::GetImmediateContext2(ID3D11DeviceContext2** ppImmediateContext)
{
    if (!ppImmediateContext) return;
    if (m_ctxProxy)
    {
        *ppImmediateContext = static_cast<ID3D11DeviceContext2*>(m_ctxProxy);
        m_ctxProxy->AddRef();
        return;
    }
    if (m_real2) m_real2->GetImmediateContext2(ppImmediateContext);
    else *ppImmediateContext = nullptr;
}

void STDMETHODCALLTYPE Device11Proxy::GetImmediateContext3(ID3D11DeviceContext3** ppImmediateContext)
{
    if (!ppImmediateContext) return;
    if (m_ctxProxy)
    {
        *ppImmediateContext = static_cast<ID3D11DeviceContext3*>(m_ctxProxy);
        m_ctxProxy->AddRef();
        return;
    }
    if (m_real3) m_real3->GetImmediateContext3(ppImmediateContext);
    else *ppImmediateContext = nullptr;
}

// ---------------------------------------------------------------------------
// Deferred-context creation — diagnostic passthrough (Aug 2026).
//
// Behaviour is UNCHANGED: we still hand the game the real deferred context.
// The logging exists to answer a specific open question on Max Payne 3, where
// the per-frame recording that feeds the right-eye replay comes back nearly
// empty (19 commands / 2 draws for a full gameplay frame, when the real frame
// is hundreds of draws). If the game builds its scene on deferred contexts,
// those draws never pass through Context11Proxy at all, so the right-eye
// sibling textures are never written — which would explain a right eye that
// is black (MSAA on) or depth-garbage green (MSAA off) rather than merely
// mis-shifted.
//
// Note this is a REPORTING hook only. Wrapping deferred contexts is not a
// drop-in fix: a command list bakes its resource bindings at record time, so
// replaying one for the right eye would just redraw the left. If these lines
// fire, the replay design needs rework, not a patch here.
// ---------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE Device11Proxy::CreateDeferredContext(
    UINT ContextFlags, ID3D11DeviceContext** ppDeferredContext)
{
    HRESULT hr = m_real->CreateDeferredContext(ContextFlags, ppDeferredContext);
    NVDM_TRACE_FIRST_N(8,
        "  Device11Proxy::CreateDeferredContext(flags=0x%X) hr=0x%08lX ctx=%p"
        " -- UNWRAPPED: draws on this context bypass right-eye recording\n",
        ContextFlags, hr,
        ((SUCCEEDED(hr) && ppDeferredContext) ? (void*)*ppDeferredContext : nullptr));
    return hr;
}

HRESULT STDMETHODCALLTYPE Device11Proxy::CreateDeferredContext1(
    UINT ContextFlags, ID3D11DeviceContext1** ppDeferredContext)
{
    HRESULT hr = m_real1 ? m_real1->CreateDeferredContext1(ContextFlags, ppDeferredContext)
                         : E_NOINTERFACE;
    NVDM_TRACE_FIRST_N(8,
        "  Device11Proxy::CreateDeferredContext1(flags=0x%X) hr=0x%08lX ctx=%p"
        " -- UNWRAPPED: draws on this context bypass right-eye recording\n",
        ContextFlags, hr,
        ((SUCCEEDED(hr) && ppDeferredContext) ? (void*)*ppDeferredContext : nullptr));
    return hr;
}

HRESULT STDMETHODCALLTYPE Device11Proxy::CreateDeferredContext2(
    UINT ContextFlags, ID3D11DeviceContext2** ppDeferredContext)
{
    HRESULT hr = m_real2 ? m_real2->CreateDeferredContext2(ContextFlags, ppDeferredContext)
                         : E_NOINTERFACE;
    NVDM_TRACE_FIRST_N(8,
        "  Device11Proxy::CreateDeferredContext2(flags=0x%X) hr=0x%08lX ctx=%p"
        " -- UNWRAPPED: draws on this context bypass right-eye recording\n",
        ContextFlags, hr,
        ((SUCCEEDED(hr) && ppDeferredContext) ? (void*)*ppDeferredContext : nullptr));
    return hr;
}

HRESULT STDMETHODCALLTYPE Device11Proxy::CreateDeferredContext3(
    UINT ContextFlags, ID3D11DeviceContext3** ppDeferredContext)
{
    HRESULT hr = m_real3 ? m_real3->CreateDeferredContext3(ContextFlags, ppDeferredContext)
                         : E_NOINTERFACE;
    NVDM_TRACE_FIRST_N(8,
        "  Device11Proxy::CreateDeferredContext3(flags=0x%X) hr=0x%08lX ctx=%p"
        " -- UNWRAPPED: draws on this context bypass right-eye recording\n",
        ContextFlags, hr,
        ((SUCCEEDED(hr) && ppDeferredContext) ? (void*)*ppDeferredContext : nullptr));
    return hr;
}

HRESULT STDMETHODCALLTYPE Device11Proxy::CreateBuffer(
    const D3D11_BUFFER_DESC* pDesc,
    const D3D11_SUBRESOURCE_DATA* pInitialData,
    ID3D11Buffer** ppBuffer)
{
    // Stage 3c.1: always wrap so downstream Try*Unwrap calls see our proxy
    // identity even for buffers that aren't constant buffers. Buffers don't
    // get stereo doubling (no right-eye sibling) — the wrap is purely for
    // identity + the VS-binding tag bit Stage 4c.1 consults.
    HRESULT hr = m_real->CreateBuffer(pDesc, pInitialData, ppBuffer);
    if (FAILED(hr) || !ppBuffer || !*ppBuffer) return hr;
    auto* bufProxy = new Buffer11Proxy(*ppBuffer, this);

    // Right-eye sibling for CBs and UAV-capable buffers. Alloc failure is
    // non-fatal: the sibling stays null and that buffer's data stays shared.
    if (gInfo.DuplicateDraws && pDesc &&
        (pDesc->BindFlags & (D3D11_BIND_CONSTANT_BUFFER |
                             D3D11_BIND_UNORDERED_ACCESS)) != 0)
    {
        ID3D11Buffer* right = nullptr;
        if (SUCCEEDED(m_real->CreateBuffer(pDesc, pInitialData, &right)) && right)
        {
            bufProxy->SetRealRight(right);
            NVDM_TRACE_FIRST_N(8,
                "  Device11Proxy::CreateBuffer: right-eye CB sibling ok (size=%u usage=%d)\n",
                pDesc->ByteWidth, (int)pDesc->Usage);
        }
        else
            NVDM_TRACE_FIRST_N(8,
                "  Device11Proxy::CreateBuffer: right-eye CB sibling alloc FAILED"
                " (size=%u) -- constants stay mono for this buffer\n",
                pDesc->ByteWidth);
    }

    *ppBuffer = static_cast<ID3D11Buffer*>(bufProxy);
    return hr;
}

HRESULT STDMETHODCALLTYPE Device11Proxy::CreateTexture2D(
    const D3D11_TEXTURE2D_DESC* pDesc,
    const D3D11_SUBRESOURCE_DATA* pInitialData,
    ID3D11Texture2D** ppTexture2D)
{
    HRESULT hr = m_real->CreateTexture2D(pDesc, pInitialData, ppTexture2D);
    if (FAILED(hr) || !ppTexture2D || !*ppTexture2D) return hr;

    // Stage 3b: consult the heuristic on the just-allocated texture. If it
    // qualifies for stereo doubling, allocate a right-eye sibling via the
    // real device using the SAME desc but no initial data — the right-eye
    // is wiz3D's stereo replica and gets its content via per-eye writes in
    // Stage 4. Mirrors ResourceWrapper::CreateRightResource semantics.
    //
    // We always wrap, even for mono resources, so that:
    //   (a) GetDevice() returns our wrapped device (COM identity preserved)
    //   (b) TryUnwrapTexture2D detects our texture in downstream Create*View
    //       methods regardless of stereo status
    //   (c) Stage 4 can attach state-tracking metadata here later.
    SIZE bbSize = { (LONG)m_logicalWidth, (LONG)m_logicalHeight };
    const SIZE* pBBSize = (m_logicalWidth > 0 && m_logicalHeight > 0) ? &bbSize : nullptr;

    ID3D11Texture2D* realLeft  = *ppTexture2D;
    ID3D11Texture2D* realRight = nullptr;

    if (ShouldDoubleTexture2D(pDesc, pBBSize))
    {
        HRESULT hr2 = m_real->CreateTexture2D(pDesc, nullptr, &realRight);
        if (FAILED(hr2))
        {
            // Right-eye allocation failed (OOM, format restrictions, etc.).
            // Continue mono for this resource — game still works, just no
            // stereo for this RT.
            realRight = nullptr;
            NVDM_TRACE_FIRST_N(4, "  Device11Proxy::CreateTexture2D: right-eye alloc failed hr=0x%08lX (mono fallback)\n", hr2);
        }
    }

    auto* texProxy = new Texture2D11Proxy(realLeft, realRight, this);
    *ppTexture2D = static_cast<ID3D11Texture2D*>(texProxy);
    // Register the real → proxy mapping so CreateRTV/DSV/SRV/UAV can recover
    // the proxy when the game passes a raw real pointer (most often via the
    // IDXGIResource QI escape hatch). Unregistered in ~Texture2D11Proxy.
    RegisterRealToProxy(realLeft, texProxy);
    NVDM_TRACE_FIRST_N(32,
        "  Device11Proxy::CreateTexture2D: %ux%u fmt=%d bind=0x%X -> Texture2D11Proxy=%p stereo=%d\n",
        (unsigned)(pDesc ? pDesc->Width : 0),
        (unsigned)(pDesc ? pDesc->Height : 0),
        (int)(pDesc ? pDesc->Format : DXGI_FORMAT_UNKNOWN),
        (unsigned)(pDesc ? pDesc->BindFlags : 0),
        texProxy, (int)texProxy->IsStereo());
    return hr;
}

HRESULT STDMETHODCALLTYPE Device11Proxy::CreateDepthStencilView(
    ID3D11Resource* pResource, const D3D11_DEPTH_STENCIL_VIEW_DESC* pDesc,
    ID3D11DepthStencilView** ppDepthStencilView)
{
    // Stage 3b: same unwrap-then-rewrap pattern as CreateRenderTargetView.
    Texture2D11Proxy* texProxy   = TryUnwrapTexture2D(pResource);
    Buffer11Proxy*    bufProxy   = TryUnwrapBuffer(pResource);  // DSV-on-buffer is exotic but legal
    if (!texProxy && !bufProxy && pResource)
        texProxy = LookupProxyByReal(pResource);  // raw-pointer fallback (see CreateRTV comment)
    ID3D11Resource*   realToUse  = texProxy ? static_cast<ID3D11Resource*>(texProxy->GetReal())
                                 : bufProxy ? static_cast<ID3D11Resource*>(bufProxy->GetReal())
                                            : pResource;
    ID3D11Resource*   realRight  = texProxy ? static_cast<ID3D11Resource*>(texProxy->GetRealRight())
                                            : nullptr;

    HRESULT hr = m_real->CreateDepthStencilView(realToUse, pDesc, ppDepthStencilView);
    if (FAILED(hr) || !ppDepthStencilView || !*ppDepthStencilView) return hr;

    if (texProxy)
    {
        ID3D11DepthStencilView* realRightDSV = nullptr;
        if (realRight)
        {
            HRESULT hr2 = m_real->CreateDepthStencilView(realRight, pDesc, &realRightDSV);
            if (FAILED(hr2)) realRightDSV = nullptr;
        }
        auto* dsvProxy = new DSV11Proxy(*ppDepthStencilView, realRightDSV, this);
        *ppDepthStencilView = static_cast<ID3D11DepthStencilView*>(dsvProxy);
        NVDM_TRACE_FIRST_N(16,
            "  Device11Proxy::CreateDepthStencilView: -> DSV11Proxy=%p stereo=%d\n",
            dsvProxy, (int)dsvProxy->IsStereo());
    }
    else
    {
        NVDM_TRACE_FIRST_N(8, "  Device11Proxy::CreateDepthStencilView: dsv=%p (unwrapped resource=%p)\n",
                           *ppDepthStencilView, pResource);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE Device11Proxy::CreateTexture1D(
    const D3D11_TEXTURE1D_DESC* pDesc,
    const D3D11_SUBRESOURCE_DATA* pInitialData,
    ID3D11Texture1D** ppTexture1D)
{
    // Stage 3c.2: always-wrap identity (no stereo doubling for Tex1D).
    HRESULT hr = m_real->CreateTexture1D(pDesc, pInitialData, ppTexture1D);
    if (FAILED(hr) || !ppTexture1D || !*ppTexture1D) return hr;
    auto* texProxy = new Texture1D11Proxy(*ppTexture1D, this);
    *ppTexture1D = static_cast<ID3D11Texture1D*>(texProxy);
    return hr;
}

HRESULT STDMETHODCALLTYPE Device11Proxy::CreateTexture3D(
    const D3D11_TEXTURE3D_DESC* pDesc,
    const D3D11_SUBRESOURCE_DATA* pInitialData,
    ID3D11Texture3D** ppTexture3D)
{
    // Stage 3c.2: always-wrap identity (no stereo doubling for Tex3D).
    HRESULT hr = m_real->CreateTexture3D(pDesc, pInitialData, ppTexture3D);
    if (FAILED(hr) || !ppTexture3D || !*ppTexture3D) return hr;
    auto* texProxy = new Texture3D11Proxy(*ppTexture3D, this);
    *ppTexture3D = static_cast<ID3D11Texture3D*>(texProxy);
    return hr;
}

HRESULT STDMETHODCALLTYPE Device11Proxy::CreateShaderResourceView(
    ID3D11Resource* pResource, const D3D11_SHADER_RESOURCE_VIEW_DESC* pDesc,
    ID3D11ShaderResourceView** ppSRView)
{
    // Stage 3c.2: unwrap input resource and wrap the resulting SRV. If the
    // source resource carries a right-eye sibling, build a parallel SRV
    // against the right-eye real resource so 4e can route per-eye binding.
    Texture2D11Proxy* tex2Proxy = TryUnwrapTexture2D(pResource);
    Texture1D11Proxy* tex1Proxy = TryUnwrapTexture1D(pResource);
    Texture3D11Proxy* tex3Proxy = TryUnwrapTexture3D(pResource);
    Buffer11Proxy*    bufProxy  = TryUnwrapBuffer(pResource);
    if (!tex2Proxy && !tex1Proxy && !tex3Proxy && !bufProxy && pResource)
        tex2Proxy = LookupProxyByReal(pResource);  // raw-pointer fallback

    ID3D11Resource* realLeftRes  = tex2Proxy ? static_cast<ID3D11Resource*>(tex2Proxy->GetReal())
                                 : tex1Proxy ? static_cast<ID3D11Resource*>(tex1Proxy->GetReal())
                                 : tex3Proxy ? static_cast<ID3D11Resource*>(tex3Proxy->GetReal())
                                 : bufProxy  ? static_cast<ID3D11Resource*>(bufProxy->GetReal())
                                             : pResource;
    ID3D11Resource* realRightRes = tex2Proxy ? static_cast<ID3D11Resource*>(tex2Proxy->GetRealRight())
                                 : bufProxy  ? static_cast<ID3D11Resource*>(bufProxy->GetRealRight())
                                             : nullptr;

    HRESULT hr = m_real->CreateShaderResourceView(realLeftRes, pDesc, ppSRView);
    if (FAILED(hr) || !ppSRView || !*ppSRView) return hr;

    ID3D11ShaderResourceView* realRightSRV = nullptr;
    if (realRightRes)
    {
        HRESULT hr2 = m_real->CreateShaderResourceView(realRightRes, pDesc, &realRightSRV);
        if (FAILED(hr2)) realRightSRV = nullptr;
    }

    auto* srvProxy = new SRV11Proxy(*ppSRView, realRightSRV, this);
    *ppSRView = static_cast<ID3D11ShaderResourceView*>(srvProxy);
    return hr;
}

HRESULT STDMETHODCALLTYPE Device11Proxy::CreateUnorderedAccessView(
    ID3D11Resource* pResource, const D3D11_UNORDERED_ACCESS_VIEW_DESC* pDesc,
    ID3D11UnorderedAccessView** ppUAView)
{
    // Stage 3c.2: same unwrap-input/wrap-output pattern as SRV.
    Texture2D11Proxy* tex2Proxy = TryUnwrapTexture2D(pResource);
    Texture1D11Proxy* tex1Proxy = TryUnwrapTexture1D(pResource);
    Texture3D11Proxy* tex3Proxy = TryUnwrapTexture3D(pResource);
    Buffer11Proxy*    bufProxy  = TryUnwrapBuffer(pResource);
    if (!tex2Proxy && !tex1Proxy && !tex3Proxy && !bufProxy && pResource)
        tex2Proxy = LookupProxyByReal(pResource);  // raw-pointer fallback

    ID3D11Resource* realLeftRes  = tex2Proxy ? static_cast<ID3D11Resource*>(tex2Proxy->GetReal())
                                 : tex1Proxy ? static_cast<ID3D11Resource*>(tex1Proxy->GetReal())
                                 : tex3Proxy ? static_cast<ID3D11Resource*>(tex3Proxy->GetReal())
                                 : bufProxy  ? static_cast<ID3D11Resource*>(bufProxy->GetReal())
                                             : pResource;
    ID3D11Resource* realRightRes = tex2Proxy ? static_cast<ID3D11Resource*>(tex2Proxy->GetRealRight())
                                 : bufProxy  ? static_cast<ID3D11Resource*>(bufProxy->GetRealRight())
                                             : nullptr;

    HRESULT hr = m_real->CreateUnorderedAccessView(realLeftRes, pDesc, ppUAView);
    if (FAILED(hr) || !ppUAView || !*ppUAView) return hr;

    ID3D11UnorderedAccessView* realRightUAV = nullptr;
    if (realRightRes)
    {
        HRESULT hr2 = m_real->CreateUnorderedAccessView(realRightRes, pDesc, &realRightUAV);
        if (FAILED(hr2)) realRightUAV = nullptr;
    }

    auto* uavProxy = new UAV11Proxy(*ppUAView, realRightUAV, this);
    *ppUAView = static_cast<ID3D11UnorderedAccessView*>(uavProxy);
    return hr;
}

} // namespace wiz3d
