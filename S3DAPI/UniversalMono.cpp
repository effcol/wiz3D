#include "stdafx.h"
#include "UniversalMono.h"
#include "GlobalData.h"
#include "ShaderProfileData.h"

// Confirmed Unity UI Default pixel shader. HLSL signature:
//   sampler2D _MainTex;
//   float4 _TextureSampleAdd;
//   float4 _ClipRect;
// Compiled from Unity's built-in UI-Default.shader; stable across Unity
// 5.x through 2022.x. Shows up in every Unity game that uses the stock
// Canvas UI (Lara Croft GO, Monument Valley, Cuphead, countless mobile
// ports, etc.). Rendering it stereo splits the HUD across both eyes.
static const DWORD kUniversalMonoPS[] = {
    0x449010EA,
};

// Unity UI vertex shader CRCs vary across engine versions, so the PS
// entry above is the safe universal anchor. Add VS entries to a new
// kUniversalMonoVS[] array (and a matching loop below) only after
// confirming across at least three unrelated Unity titles.

void ApplyUniversalMonoDefaults()
{
    for (DWORD crc : kUniversalMonoPS) {
        ShaderProfileData& p = g_ProfileData.PSCRCData[crc];
        p.m_Multiplier = 0.0f;
        p.m_bSkipStereo = true;
    }
}
