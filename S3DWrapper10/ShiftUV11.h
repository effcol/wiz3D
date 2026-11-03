/*
* Project : wiz3D
* Copyright (C) wiz3D contributors 2026
*/

// ShiftUV — ShaderPatch11 client. The right-eye twin of a VS adds a per-eye
// constant to one component of a chosen output register, letting UI /
// overlay / text shaders simulate depth without a clip-space position patch.

#pragma once

#include "ShaderPatch11.h"

namespace wiz3d
{

// Adds cb[0].x to output register `outputReg`'s component (0 = x, 1 = y, ...)
// before every ret. Caller fills cb[0].x with the per-eye UV offset.
ShaderPatchPlan ShiftUVPlan(unsigned outputReg, unsigned component);

} // namespace wiz3d
