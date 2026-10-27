/*
 * Project : wiz3D
 * Copyright (C) wiz3D contributors 2026
 *
 * Implements the DX9 Katanga producer. See KatangaProducerDX9.h for the
 * protocol spec. Mirrors KatangaProducer.cpp for the mutex / mapped-file
 * / handle-publishing bits; diverges on the texture-create call (DX9
 * CreateTexture with pSharedHandle) and per-frame StretchRect blits.
 */
#include "KatangaProducerDX9.h"

namespace wiz3d {

namespace {

const wchar_t* const kKatangaMutexName       = L"KatangaSetupMutex";
const wchar_t* const kKatangaMappedFileName  = L"Local\\KatangaMappedFile";
constexpr DWORD      kSetupMutexWaitMs       = 1000;

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

KatangaProducerDX9::KatangaProducerDX9() = default;

KatangaProducerDX9::~KatangaProducerDX9()
{
    Shutdown();
}

bool KatangaProducerDX9::IsKatangaRunning()
{
    HANDLE h = OpenMutexW(SYNCHRONIZE, FALSE, kKatangaMutexName);
    if (!h) return false;
    CloseHandle(h);
    return true;
}

bool KatangaProducerDX9::TryInit()
{
    if (m_SetupMutex && m_MappedView) return true;
    if (m_InitAttempted && !m_SetupMutex) return false;

    m_InitAttempted = true;

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

bool KatangaProducerDX9::OnBackbufferReady(IDirect3DDevice9* device,
                                            UINT backbufferWidth, UINT backbufferHeight,
                                            D3DFORMAT backbufferFormat)
{
    if (!device || !m_MappedView || !m_SetupMutex) return false;

    ScopedKatangaMutex guard(m_SetupMutex);
    if (!guard.Acquired()) return false;

    // Drop any prior texture; publish HANDLE=0 so Katanga sees "no producer"
    // while we rebuild.
    if (m_SharedSurf) { m_SharedSurf->Release(); m_SharedSurf = nullptr; }
    if (m_SharedTex)
    {
        if (m_MappedView) *static_cast<PUINT>(m_MappedView) = 0;
        m_SharedTex->Release(); m_SharedTex = nullptr;
        m_SharedHandle = nullptr;
    }

    const UINT      wantW = backbufferWidth * 2;
    const UINT      wantH = backbufferHeight;
    const D3DFORMAT wantF = backbufferFormat;

    // DX9 shared handle: pSharedHandle must point at a HANDLE initialised to
    // nullptr. CreateTexture writes the KMT handle into it on success.
    HANDLE hShared = nullptr;
    HRESULT hr = device->CreateTexture(
        wantW, wantH, 1,
        D3DUSAGE_RENDERTARGET,
        wantF,
        D3DPOOL_DEFAULT,
        &m_SharedTex,
        &hShared);
    if (FAILED(hr) || !m_SharedTex || !hShared)
    {
        if (m_SharedTex) { m_SharedTex->Release(); m_SharedTex = nullptr; }
        return false;
    }

    // Grab the base-level surface -- that's what StretchRect writes to.
    if (FAILED(m_SharedTex->GetSurfaceLevel(0, &m_SharedSurf)))
    {
        m_SharedTex->Release(); m_SharedTex = nullptr;
        return false;
    }

    m_SharedHandle = hShared;
    m_SbsWidth     = wantW;
    m_SbsHeight    = wantH;
    m_SbsFormat    = wantF;

    // Publish the handle. PtrToUint truncates to 32 bits; KMT handles fit.
    *static_cast<PUINT>(m_MappedView) = PtrToUint(m_SharedHandle);
    return true;
}

bool KatangaProducerDX9::PublishFrame(IDirect3DDevice9* device,
                                      IDirect3DSurface9* leftEye, IDirect3DSurface9* rightEye,
                                      bool swapEyes)
{
    if (!device || !leftEye || !rightEye || !m_SharedSurf || !m_SetupMutex) return false;

    ScopedKatangaMutex guard(m_SetupMutex);
    if (!guard.Acquired()) return false;

    // Figure out single-eye dims from the shared surface (half-width).
    const UINT eyeW = m_SbsWidth / 2;
    const UINT eyeH = m_SbsHeight;

    RECT dstLeft  = { 0,       0, (LONG)eyeW,     (LONG)eyeH };
    RECT dstRight = { (LONG)eyeW, 0, (LONG)m_SbsWidth, (LONG)eyeH };

    IDirect3DSurface9* srcLeft  = swapEyes ? rightEye : leftEye;
    IDirect3DSurface9* srcRight = swapEyes ? leftEye  : rightEye;

    // StretchRect handles format conversion if needed (unlikely since we
    // match the backbuffer format). Point filter avoids per-eye blurring.
    HRESULT hr1 = device->StretchRect(srcLeft,  nullptr, m_SharedSurf, &dstLeft,  D3DTEXF_POINT);
    HRESULT hr2 = device->StretchRect(srcRight, nullptr, m_SharedSurf, &dstRight, D3DTEXF_POINT);
    return SUCCEEDED(hr1) && SUCCEEDED(hr2);
}

void KatangaProducerDX9::OnResize()
{
    // Called from DX9 wrapper's OnDeviceLost / before Reset. Drop GPU
    // resources; keep mutex + mapped file open so next OnBackbufferReady
    // can republish without re-opening the Katanga handles.
    if (m_SetupMutex && m_MappedView)
    {
        ScopedKatangaMutex guard(m_SetupMutex);
        if (guard.Acquired()) *static_cast<PUINT>(m_MappedView) = 0;
    }
    if (m_SharedSurf) { m_SharedSurf->Release(); m_SharedSurf = nullptr; }
    if (m_SharedTex)  { m_SharedTex->Release();  m_SharedTex  = nullptr; }
    m_SharedHandle = nullptr;
    m_SbsWidth = m_SbsHeight = 0;
    m_SbsFormat = D3DFMT_UNKNOWN;
}

void KatangaProducerDX9::Shutdown()
{
    if (m_SetupMutex && m_MappedView)
    {
        ScopedKatangaMutex guard(m_SetupMutex);
        if (guard.Acquired()) *static_cast<PUINT>(m_MappedView) = 0;
    }
    if (m_SharedSurf) { m_SharedSurf->Release(); m_SharedSurf = nullptr; }
    if (m_SharedTex)  { m_SharedTex->Release();  m_SharedTex  = nullptr; }
    m_SharedHandle = nullptr;
    m_SbsWidth = m_SbsHeight = 0;
    m_SbsFormat = D3DFMT_UNKNOWN;

    if (m_MappedView) { UnmapViewOfFile(m_MappedView); m_MappedView = nullptr; }
    if (m_MappedFile) { CloseHandle(m_MappedFile);     m_MappedFile = nullptr; }
    if (m_SetupMutex) { CloseHandle(m_SetupMutex);     m_SetupMutex = nullptr; }

    m_InitAttempted = false;
}

} // namespace wiz3d
