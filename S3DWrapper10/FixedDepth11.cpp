/*
* Project : wiz3D
* Copyright (C) wiz3D contributors 2026
*/

#include "StdAfx.h"
#include "FixedDepth11.h"
#include <d3d10umddi.h>

namespace wiz3d
{

ShaderPatchPlan FixedDepthPlan()
{
    using namespace DxbcEmit;
    ShaderPatchPlan plan;
    plan.redirectOutput = ShaderPatchPlan::kPositionOutput;
    plan.extraTemps = 0;
    plan.cbRegisters = 1;
    plan.beforeRet = [](std::vector<uint32_t>& o, const PatchRegs& r)
    {
        if ((r.outMask & 0x1) == 0) return false;   // x not written
        // out.x += cb[0].x  (plan constant = eyeShift * depth, precomputed by caller)
        Ins(o, D3D10_SB_OPCODE_ADD, {
            Dst(kTemp, r.outTemp, 0x1),
            Src(kTemp, r.outTemp, 0, 0, 0, 0),
            Cb(r.cbSlot, 0, 0, 0, 0, 0)
        });
        return true;
    };
    return plan;
}

} // namespace wiz3d
