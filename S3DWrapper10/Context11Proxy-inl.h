/*
* Project : wiz3D
* Copyright (C) Akshay Hegde 2026
*/

// What the Context11Proxy files share, as DX9's BaseStereoRenderer-inl.h does for its renderer files: the includes, the
// slot maxima, the frame state of the matrix detector and the shift helpers more than one file calls.

#pragma once

#include "Context11Proxy.h"
#include "../Shared/StereoMatrix.h"
#include "Device11Proxy.h"
#include "Texture2D11Proxy.h"
#include "RTV11Proxy.h"
#include "DSV11Proxy.h"
#include "Buffer11Proxy.h"
#include "SRV11Proxy.h"
#include "UAV11Proxy.h"
#include "Texture1D11Proxy.h"
#include "Texture3D11Proxy.h"
#include "proxy_factory.h"     // TryUnwrap* helpers
#include "EyeUnwrap11.h"       // the per-eye unwrap rules, shared with the deferred-context twin
#include "DeferredContext11.h" // command-list pairs
#include "AdapterFunctions.h"  // DDILog
#include "..\S3DAPI\ShaderProfileData.h"  // g_ProfileData matrix declarations
#include "SpriteDepth11.h"     // ClipSpaceSprites 3: the sprite-depth plan
#include "RightRecorder11.h"   // BatchRightEye
#include "GpuTimer11.h"        // CollectDebugInformation: GPU time of our work
#include <d3dcompiler.h>       // D3DDisassemble: the UAV slots a compute shader reads
#include <cctype>
#include <sstream>
#include <set>
#include <mutex>

// Slot-count maxima now live on Context11Proxy (see the header) so the
// eye-tracking arrays and the unwrap temporaries stay the same size.
using wiz3d::Context11Proxy;
static constexpr UINT kMaxSRVs       = Context11Proxy::kMaxSRVs;
static constexpr UINT kMaxSamplers   = Context11Proxy::kMaxSamplers;
static constexpr UINT kMaxCBs        = Context11Proxy::kMaxCBs;
static constexpr UINT kMaxRTVs       = Context11Proxy::kMaxRTVs;
static constexpr UINT kMaxUAVs       = Context11Proxy::kMaxUAVs;
static constexpr UINT kMaxVBs        = Context11Proxy::kMaxVBs;
static constexpr UINT kMaxSOBuffers  = Context11Proxy::kMaxSOBuffers;
static constexpr UINT kMaxClassInst  = Context11Proxy::kMaxClassInst;

namespace wiz3d
{

// Stage 4c: walk the captured CB bytes 64-byte (4x4 float) at a time. If the
// chunk matches the D3D row-major perspective projection pattern, apply the
// configured eye-shift to the m[2][0] element so the right eye renders with
// a horizontally-offset projection.
//
// D3D row-major perspective projection layout (HLSL default mul order):
//   m[0][0]=xScale   m[0][1]=0     m[0][2]=0       m[0][3]=0
//   m[1][0]=0        m[1][1]=yScale m[1][2]=0      m[1][3]=0
//   m[2][0]=0        m[2][1]=0     m[2][2]=zFactor m[2][3]=1
//   m[3][0]=0        m[3][1]=0     m[3][2]=zOffset m[3][3]=0
//
// As floats[0..15], we check floats[11]==1 (m[2][3]) and floats[15]==0
// (m[3][3]) — distinguishes perspective projections from identity, view,
// world, and orthographic. m[0][0] != 0 too (some xScale). When matched,
// add eyeShift to floats[8] (m[2][0]). This is the standard "horizontal
// off-axis projection" stereo trick used by iZ3D and 3DMigoto for view-
// shift without parallax errors.
// Stage 4e.2: targeted modifier. matrices[] enumerates known matrix start
// registers (matrixRegister) + their transposed flag. Each register is
// 16 bytes (4 floats). Non-transposed (HLSL row_major): matrix spans
// 4 consecutive registers row-by-row; m[2][0] is at the (register+2)*16
// byte offset, component 0. Transposed (HLSL default column_major): same
// 4 registers but stored column-by-column; m[2][0] is at register*16 + 8.
// We also need m[0][0] (xScale) to scale the shift by — at offset 0 of
// the first register in both layouts.
struct EyeShiftMatrix
{
    DWORD matrixRegister;   // register index inside CB
    BOOL  matrixIsTransposed;
    // Set for matrices declared <Inverse Value="1"/> in BaseProfile.xml: the CB
    // holds P^-1 (deferred passes reconstructing position), not P.
    BOOL  matrixIsInverse;
    // Recalled from the buffer instead of confirmed by the bound shader, so the
    // contents get a shape check before we trust them.
    BOOL  matrixFromLearned;
    // >= 0: not a matrix but a camera view's x translation, this component of matrixRegister, moved by -viewSign * the
    // eye offset (ViewMatrices).
    int   viewComponent = -1;
    float viewSign = 0.f;
    // The viewport is folded in (ViewportMatrices): world to pixel, or pixel to world with matrixIsInverse.
    bool  viewport = false;
};

// A tracked binding holds a reference, as the runtime does for a bound object (new one taken before the old dropped).
template<class T> static void HoldRef(T*& slot, T* p)
{
    if (p) p->AddRef();
    if (slot) slot->Release();
    slot = p;
}
template<class T> static void DropRef(T*& slot)
{
    if (slot) { slot->Release(); slot = nullptr; }
}

// CollectDebugInformation: adds the ticks spent in its scope to a FrameTiming part; does nothing otherwise.
struct ScopedTicks
{
    LONGLONG* acc;
    LARGE_INTEGER t0;
    explicit ScopedTicks(LONGLONG& part) : acc(gInfo.CollectDebugInformation ? &part : nullptr) { if (acc) QueryPerformanceCounter(&t0); }
    ~ScopedTicks() { if (acc) { LARGE_INTEGER t1; QueryPerformanceCounter(&t1); *acc += t1.QuadPart - t0.QuadPart; } }
};

// <BatchRightEye>: a state set the game makes reaches the right eye's recorder too (kept only while it records).
#define MIRROR(CALL)  do { if (m_mirror) m_mirror->CALL; } while (0)
#define MIRROR1(CALL) do { if (m_mirror1) m_mirror1->CALL; } while (0)

// The frame state the files share, defined in Context11Proxy.cpp.
extern float s_camP00;
extern bool  s_uiProjectionParallax;
extern unsigned s_scaledByCamera;
extern unsigned s_lightOnlyKept;
extern std::map<int, std::pair<unsigned, float>> s_cleanP00Votes;
extern float    s_sceneP00;
extern float    s_referenceP00;
extern float    s_sceneZRow[2];
extern unsigned s_sceneZRowFrame;
extern unsigned s_sceneP00Votes, s_cleanP00Total;
extern unsigned s_frameId;
extern wiz3d::StereoMatrixState s_mat;

// Helpers more than one file calls: the profile and CRC lookups and the shift math (Context11Proxy_Main.cpp,
// Context11Proxy_Utils.cpp).
void  ClearBlockTargets();   // the per-frame union of ring block targets (SyncBoundConstants)
DWORD BoundShaderCRC(Device11Proxy* parent, void* shader);
float ProfileShaderMultiplier(Device11Proxy* parent, void* vs);
// Per-shader ConvergenceShift from the active profile (<VertexShader CRC=... ConvergenceShift="...">).
// Returns 0 when no profile entry applies, matching DX9's m_CurrentConvergenceShift reset default.
float ProfileShaderConvergenceShift(Device11Proxy* parent, void* vs);
// Effective per-shader convergence in world units. Mirrors DX9's inline matrix shift
// (BaseStereoRenderer-inl.h:47 / 109 / 165): new_conv = 1 / (1/ZPS + shift). Returns the global
// StereoConvergence() when shift==0.
float ResolveShaderConvergence(Device11Proxy* parent, void* vs);
float EyeShiftB(float eyeShift);
float StereoConvergence();
float EyeOffsetM41(float eyeShift);
bool  ZRowOf(const float* f, bool transposed, float& m22, float& m23);
bool  CameraFamily(const float* f, bool transposed);
void  ApplyTargetedEyeShiftToCB(unsigned char* data, size_t byteCount,
                                float eyeShift,
                                float conv,
                                const std::vector<EyeShiftMatrix>& matrices,
                                bool fullColumn,
                                unsigned* outShifted = nullptr,
                                unsigned* outRejected = nullptr,
                                unsigned* outInvPatched = nullptr,
                                const void* srcBuffer = nullptr);

} // namespace wiz3d
