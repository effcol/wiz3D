/*
* Project : wiz3D
* Copyright (C) wiz3D contributors 2026
*/

#include "StdAfx.h"
#include "ShiftUV11.h"
#include <d3d10umddi.h>

namespace wiz3d
{

ShaderPatchPlan ShiftUVPlan(unsigned outputReg, unsigned component)
{
    using namespace DxbcEmit;
    ShaderPatchPlan plan;
    plan.redirectOutput = (int)outputReg;
    plan.extraTemps = 0;
    plan.cbRegisters = 1;
    const uint32_t mask = (1u << component);
    const unsigned comp = component;
    plan.beforeRet = [mask, comp](std::vector<uint32_t>& o, const PatchRegs& r)
    {
        if ((r.outMask & mask) == 0) return false;
        // out.<component> += cb[0].x
        Ins(o, D3D10_SB_OPCODE_ADD, {
            Dst(kTemp, r.outTemp, mask),
            Src(kTemp, r.outTemp, comp, comp, comp, comp),
            Cb(r.cbSlot, 0, 0, 0, 0, 0)
        });
        return true;
    };
    return plan;
}

} // namespace wiz3d
