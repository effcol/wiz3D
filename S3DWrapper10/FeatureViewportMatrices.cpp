/*
* Project : wiz3D
* Copyright (C) Akshay Hegde 2026
*/

// <ViewportMatrices Value="1"/>: camera matrices with the viewport folded in (world to pixel, pixel to world) are declared
// camera matrices too, and take the eye offset scaled to pixels (Context11Proxy::ClassifyDeclaredMatrix).

#include "StdAfx.h"
#include "ProfileFeature.h"
#include "AdapterFunctions.h"   // DDILog
#include "../Shared/StereoMatrix.h"
#include <cmath>
#include <mutex>
#include <set>

namespace wiz3d
{
namespace
{

std::mutex                    s_logLock;
std::set<unsigned long long>  s_logged[2];   // shader registers already logged as a miss, as a match
unsigned                      s_forward = 0, s_inverse = 0, s_lost = 0, s_matched = 0;

// F canonical row-vector (x column 0, w column 3), a forward projection to the viewport: the viewport's scale on x and y is
// those columns' part along the w column, and what is left of x is the focal in pixels. sx: the x scale, for the log.
bool ViewportShape(const float* F, float scene, float& focal, float& wn, float& sx, float& p00)
{
    const float W[3] = { F[3], F[7], F[11] };
    const float ww = W[0] * W[0] + W[1] * W[1] + W[2] * W[2];
    if (!(ww > 1e-12f)) return false;
    float X[3] = { F[0], F[4], F[8] }, Y[3] = { F[1], F[5], F[9] };
    sx = (X[0] * W[0] + X[1] * W[1] + X[2] * W[2]) / ww;
    const float sy = (Y[0] * W[0] + Y[1] * W[1] + Y[2] * W[2]) / ww;
    if (!(sx > 0.25f) || !(sy > 0.25f)) return false;   // NDC leans on w by its jitter only
    for (int k = 0; k < 3; ++k) { X[k] -= sx * W[k]; Y[k] -= sy * W[k]; }
    const float xn = sqrtf(X[0] * X[0] + X[1] * X[1] + X[2] * X[2]), yn = sqrtf(Y[0] * Y[0] + Y[1] * Y[1] + Y[2] * Y[2]);
    wn = sqrtf(ww);
    p00 = xn / (wn * sx);
    // The camera's right and up axes, and the scene camera's focal through a viewport at the origin.
    if (!(xn > 1e-6f) || !(yn > 1e-6f) || fabsf(X[0] * Y[0] + X[1] * Y[1] + X[2] * Y[2]) > 0.01f * xn * yn) return false;
    if (!(scene > 0.f) || fabsf(p00 - scene) > 0.002f * scene) return false;
    focal = xn / wn;
    return true;
}

// The forward projection of a declared matrix: itself, or an unprojection's inverse.
bool ForwardOf(const float* f, bool transposed, bool inverse, float* F)
{
    CanonicalizeMat4(f, transposed, F);
    if (!AllFinite16(F)) return false;
    if (!inverse) return true;
    if (!LooksLikeUnprojection(F)) return false;
    D3DXMATRIX c(F), inv;
    if (!D3DXMatrixInverse(&inv, nullptr, &c)) return false;
    memcpy(F, (const float*)inv, sizeof(float) * 16);
    return AllFinite16(F);
}

class ViewportMatricesFeature : public ProfileFeature
{
public:
    void OnFrameEnd(Context11Proxy&, unsigned frame, bool report) override
    {
        if (report)
            DDILog("  [frame %u] viewport matrices (this frame): world-to-pixel shifted %u, pixel-to-world shifted %u, shape lost"
                   " %u | shader registers matched so far %u\n", frame, s_forward, s_inverse, s_lost, s_matched);
        s_forward = s_inverse = s_lost = 0;
    }
};

} // namespace

bool ViewportMatricesOn()
{
    static const bool s_on = ProfileSwitchOn("ViewportMatrices");
    return s_on;
}

bool ViewportFocal(const float* f, bool transposed, bool inverse, float scene, float& focal, float& wn)
{
    float F[16], sx, p00;
    return ForwardOf(f, transposed, inverse, F) && ViewportShape(F, scene, focal, wn, sx, p00);
}

int ClassifyViewportMatrix(const float* f, float scene, bool rowsOnly, bool& transposed, DWORD crc, UINT slot, DWORD reg)
{
    int found = 0;
    float bestSx = 0.f, bestP00 = 0.f;
    for (int o = rowsOnly ? 1 : 0; o < 2 && !found; ++o)
        for (int inv = 0; inv < 2 && !found; ++inv)
        {
            float F[16], focal = 0.f, wn = 0.f, sx = 0.f, p00 = 0.f;
            if (!ForwardOf(f, o == 1, inv == 1, F)) continue;
            const bool ok = ViewportShape(F, scene, focal, wn, sx, p00);
            if (sx > bestSx) { bestSx = sx; bestP00 = p00; }
            if (ok) { found = inv ? 2 : 1; transposed = o == 1; }
        }
    // Each shader register once as a match and once as a near miss (its x leaning on w as a viewport's does).
    if (!found && !(bestSx > 0.25f)) return 0;
    const unsigned long long key = ((unsigned long long)crc << 32) | (slot << 16) | reg;
    std::lock_guard<std::mutex> g(s_logLock);
    if (!s_logged[found ? 1 : 0].insert(key).second) return found;
    if (found) ++s_matched;
    FeatureLog("  ViewportMatrices: CRC=0x%08lX cb%u r%lu %s | viewport x scale %.2f, focal p00 %.4f (scene %.4f)\n", crc, slot,
               reg, found == 1 ? "world to pixel, shifted" : found == 2 ? "pixel to world, shifted" : "not the scene camera's",
               bestSx, bestP00, scene);
    return found;
}

void CountViewportPatch(bool inverse, bool ok)
{
    if (!ok) ++s_lost;
    else if (inverse) ++s_inverse;
    else ++s_forward;
}

std::unique_ptr<ProfileFeature> MakeViewportMatrices(const ProfileEntry* e)
{
    if (!e || EntryValue(*e, "Value") == "0") return nullptr;
    return std::unique_ptr<ProfileFeature>(new ViewportMatricesFeature());
}

} // namespace wiz3d
