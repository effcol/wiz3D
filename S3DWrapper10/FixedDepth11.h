/*
* Project : wiz3D
* Copyright (C) wiz3D contributors 2026
*/

// FixedDepth / DepthFromCB — ShaderPatch11 clients. The right-eye twin of a VS
// adds a precomputed per-eye shift (cb[0].x) to clip-space x on every ret.
// Same DXBC plan for both primitives — they differ only in how BindFixedDepth
// vs BindDepthFromCB computes the constant at bind time.

#pragma once

#include "ShaderPatch11.h"

namespace wiz3d
{

// Adds cb[0].x to clip-space x before every ret. Caller fills cb[0].x with
// the per-eye depth shift. extraTemps = 0 (operates on the redirected
// SV_Position temp); cbRegisters = 1 (one float4 of plan constants).
ShaderPatchPlan FixedDepthPlan();

} // namespace wiz3d
