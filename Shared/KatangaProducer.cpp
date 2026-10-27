/*
 * Project : wiz3D
 * Copyright (C) wiz3D contributors 2026
 *
 * Implements the Katanga shared-texture producer. See KatangaProducer.h
 * header for the protocol spec and licensing posture.
 */
#include "KatangaProducer.h"
#include <dxgi.h>

namespace wiz3d {

namespace {

const wchar_t* const kKatangaMutexName       = L"KatangaSetupMutex";
const wchar_t* const kKatangaMappedFileName  = L"Local\\KatangaMappedFile";
constexpr DWORD      kSetupMutexWaitMs       = 1000;

// Guard the setup-mutex acquire/release so an exception or early-return
// can't leave Katanga blocked.
class ScopedKatangaMutex
{
public:
    explicit ScopedKatangaMutex(HANDLE mutex)
        : m_Mutex(mutex)
        , m_Acquired(WAIT_OBJECT_0 == WaitForSingleObject(mutex, kSetupMutexWaitMs))
    {}
    ~ScopedKatangaMutex() { if (m_Acquired) ReleaseMutex(m_Mutex); }
    bool Acquired() const { return m_Acquired; }
private:
    HANDLE m_Mutex;
    bool   m_Acquired;
};

} // namespace

KatangaProducer::KatangaProducer() = default;

KatangaProducer::~KatangaProducer()
{
    Shutdown();
}

bool KatangaProducer::IsKatangaRunning()
{
    // Non-destructive probe: open the mutex, close it immediately. Only
    // tells us whether Katanga has created it (i.e. is running), without
    // actually waiting on it or taking any resources.
    HANDLE h = OpenMutexW(SYNCHRONIZE, FALSE, kKatangaMutexName);
    if (!h) return false;
    CloseHandle(h);
    return true;
}

bool KatangaProducer::TryInit()
{
    if (m_SetupMutex && m_MappedView) return true;        // already initialised
    if (m_InitAttempted && !m_SetupMutex) return false;   // failed previously, don't retry hot path

    m_InitAttempted = true;

    // Katanga-first startup: if the mutex isn't there, Katanga isn't
    // running. The caller falls back without us touching anything else.
    m_SetupMutex = OpenMutexW(SYNCHRONIZE, FALSE, kKatangaMutexName);
    if (!m_SetupMutex) return false;

    m_MappedFile = OpenFileMappingW(FILE_MAP_WRITE, FALSE, kKatangaMappedFileName);
    if (!m_MappedFile)
    {
        CloseHandle(m_SetupMutex); m_SetupMutex = nullptr;
        return false;
    }

    m_MappedView = MapViewOfFile(m_MappedFile, FILE_MAP_WRITE, 0, 0, sizeof(UINT));
    if (!m_MappedView)
    {
        CloseHandle(m_MappedFile); m_MappedFile = nullptr;
        CloseHandle(m_SetupMutex); m_SetupMutex = nullptr;
        return false;
    }

    return true;
}

DXGI_FORMAT KatangaProducer::StripSrgb(DXGI_FORMAT fmt)
{
    // Katanga's own code (InProc_DX11.cpp:179-194) strips exactly these two.
    switch (fmt)
    {
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
        default: return fmt;
    }
}

bool KatangaProducer::OnBackbufferReady(ID3D11Device* device, const D3D11_TEXTURE2D_DESC& bbDesc)
{
    if (!device || !m_MappedView || !m_SetupMutex) return false;

    // Serialise texture (re)creation against Katanga's render loop. If
    // Katanga is mid-draw it'll finish, release the mutex, we take it,
    // swap the texture handle in the mapped file, release, Katanga picks
    // up the new HANDLE on its next frame.
    ScopedKatangaMutex guard(m_SetupMutex);
    if (!guard.Acquired()) return false;

    // Drop any prior texture before publishing a new HANDLE.
    if (m_SharedTex)
    {
        if (m_MappedView) *static_cast<PUINT>(m_MappedView) = 0;
        m_SharedTex->Release(); m_SharedTex = nullptr;
        m_SharedHandle = nullptr;
    }

    D3D11_TEXTURE2D_DESC d = bbDesc;
    d.Width       = bbDesc.Width * 2;   // wide SBS: 2W x H
    d.Format      = StripSrgb(bbDesc.Format);
    d.BindFlags  |= D3D11_BIND_SHADER_RESOURCE;
    d.CPUAccessFlags = 0;
    // Legacy KMT MISC_SHARED (NOT _NTHANDLE). Katanga's OpenSharedResource
    // on the consumer side expects the KMT handle, not the NT handle.
    d.MiscFlags   = D3D11_RESOURCE_MISC_SHARED;

    HRESULT hr = device->CreateTexture2D(&d, nullptr, &m_SharedTex);
    if (FAILED(hr) || !m_SharedTex) return false;

    m_SbsWidth  = d.Width;
    m_SbsHeight = d.Height;
    m_SbsFormat = d.Format;

    IDXGIResource* dxgi = nullptr;
    hr = m_SharedTex->QueryInterface(__uuidof(IDXGIResource), reinterpret_cast<void**>(&dxgi));
    if (SUCCEEDED(hr) && dxgi)
    {
        dxgi->GetSharedHandle(&m_SharedHandle);
        dxgi->Release();
    }
    if (!m_SharedHandle)
    {
        m_SharedTex->Release(); m_SharedTex = nullptr;
        return false;
    }

    // Publish via mapped file. 32-bit HANDLE per Windows IPC convention
    // (KMT handles fit in 32 bits on both x86 and x64).
    *static_cast<PUINT>(m_MappedView) = PtrToUint(m_SharedHandle);
    return true;
}

bool KatangaProducer::PublishFrame(ID3D11DeviceContext* ctx, ID3D11Texture2D* wideSbsSrc)
{
    if (!ctx || !wideSbsSrc || !m_SharedTex || !m_SetupMutex) return false;

    ScopedKatangaMutex guard(m_SetupMutex);
    if (!guard.Acquired()) return false;

    // Full-resource copy of the wrapper's wide-SBS composite into the
    // shared texture Katanga reads from. Format + dims must match -- the
    // wrapper is responsible for creating its composite at GetSbs{W,H,Format}.
    ctx->CopyResource(m_SharedTex, wideSbsSrc);
    return true;
}

void KatangaProducer::Shutdown()
{
    // Publish HANDLE=0 so Katanga sees "no producer" cleanly before we
    // tear anything down. Grab the mutex if we still have it, so Katanga
    // isn't mid-draw on a texture we're about to free.
    if (m_SetupMutex && m_MappedView)
    {
        ScopedKatangaMutex guard(m_SetupMutex);
        if (guard.Acquired())
            *static_cast<PUINT>(m_MappedView) = 0;
    }

    if (m_SharedTex)   { m_SharedTex->Release();   m_SharedTex   = nullptr; }
    m_SharedHandle = nullptr;
    m_SbsWidth = m_SbsHeight = 0;
    m_SbsFormat = DXGI_FORMAT_UNKNOWN;

    if (m_MappedView)  { UnmapViewOfFile(m_MappedView); m_MappedView  = nullptr; }
    if (m_MappedFile)  { CloseHandle(m_MappedFile);     m_MappedFile  = nullptr; }
    if (m_SetupMutex)  { CloseHandle(m_SetupMutex);     m_SetupMutex  = nullptr; }

    m_InitAttempted = false;  // allow re-init if the user re-launches Katanga
}

} // namespace wiz3d
