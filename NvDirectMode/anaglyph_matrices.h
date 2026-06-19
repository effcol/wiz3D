/* NvDirectMode — shared AnaglyphColour x AnaglyphMethod coefficient table
 *
 * Six float3 rows per [colour][method] cell:
 *   lR / lG / lB = how much of left-eye RGB contributes to output R / G / B
 *   rR / rG / rB = how much of right-eye RGB contributes to output R / G / B
 *
 * Same coefficient values used in AmdQbProxy/AmdQbProxy.cpp — Dubois 2001
 * spectral fits for the 7 methods, with matching adaptations for the three
 * glasses colour pairs (Red/Cyan, Green/Magenta, Amber/Blue).
 *
 * Header-only so each per-API proxy (d3d9 / d3d10 / d3d11 / opengl32) can
 * pull in the same values without copying. NvDirectMode_AnaglyphMatrix is
 * agnostic of D3D / GL types — translates to whatever per-eye constant
 * buffer / uniform block format the API uses.
 */

#pragma once

namespace NvDirectMode
{
    struct AnaglyphMatrix { float lR[3], lG[3], lB[3], rR[3], rG[3], rB[3]; };

    // Indexed [AnaglyphColour 0..3][AnaglyphMethod 0..6].
    // Colour: 0=Red/Cyan, 1=Green/Magenta, 2=Amber/Blue, 3=TriOviz/Inficolor
    // Method: 0=Dubois, 1=Compromise, 2=Color, 3=HalfColor, 4=Optimised, 5=Grey, 6=True
    //
    // TriOviz/Inficolor 3D (index 3): TriOviz Labs / Darkworks "Inficolor 3D" system.
    // Left eye = green filter only; right eye = magenta (red+blue) filter.
    // Differs from standard Green/Magenta in that the TriOviz glasses use
    // spectrally-tuned filters that allow near-full-colour perception with
    // reduced ghosting. The Dubois row uses spectral fits derived from the
    // TriOviz filter transmission curves (green peak ~530 nm, magenta
    // notch-filtered to pass red ~620 nm and blue ~450 nm). The Color row
    // matches the SuperDepth3D.fx "Inficolor 3D" direct-channel encoding.
    // Reference: Wikipedia "Anaglyph 3D § Inficolor 3D";
    //            BlueSkyDefender/Depth3D SuperDepth3D.fx Inficolor_3D_Emulator.
    //
    // 'static' (not 'inline') so this header works with C++14 — gives each
    // including TU its own copy of the table (~2 KB, trivial). Switch to
    // 'inline const' if NvDirectMode's project files ever bump to /std:c++17.
    static const AnaglyphMatrix kAnaglyphMatrices[4][7] =
    {
        // ---- AC_RED_CYAN [0] ----
        {
            // AM_DUBOIS — spectral least-squares fit (Dubois 2001); best ghosting suppression
            {{ 0.456f, 0.500f, 0.176f}, {-0.040f,-0.038f,-0.016f}, {-0.015f,-0.021f,-0.016f},
             {-0.043f,-0.088f,-0.002f}, { 0.378f, 0.734f, 0.018f}, {-0.072f,-0.113f, 1.226f}},
            // AM_COMPROMISE — Ahtik 2011; balanced glasses/no-glasses
            {{ 0.439f, 0.447f, 0.148f}, { 0.000f, 0.000f, 0.000f}, { 0.000f, 0.000f, 0.000f},
             { 0.000f, 0.000f, 0.000f}, { 0.095f, 0.934f,-0.005f}, {-0.018f,-0.028f, 1.057f}},
            // AM_COLOR — direct channel passthrough; severe colour fringing
            {{ 1.000f, 0.000f, 0.000f}, { 0.000f, 0.000f, 0.000f}, { 0.000f, 0.000f, 0.000f},
             { 0.000f, 0.000f, 0.000f}, { 0.000f, 1.000f, 0.000f}, { 0.000f, 0.000f, 1.000f}},
            // AM_HALF_COLOR — left desaturated, right keeps colour
            {{ 0.299f, 0.587f, 0.114f}, { 0.000f, 0.000f, 0.000f}, { 0.000f, 0.000f, 0.000f},
             { 0.000f, 0.000f, 0.000f}, { 0.000f, 1.000f, 0.000f}, { 0.000f, 0.000f, 1.000f}},
            // AM_OPTIMISED — Wimmer weighted-channel
            {{ 0.000f, 0.700f, 0.300f}, { 0.000f, 0.000f, 0.000f}, { 0.000f, 0.000f, 0.000f},
             { 0.000f, 0.000f, 0.000f}, { 0.000f, 1.000f, 0.000f}, { 0.000f, 0.000f, 1.000f}},
            // AM_GREY — both eyes greyscale; near-zero ghosting, no colour
            {{ 0.299f, 0.587f, 0.114f}, { 0.000f, 0.000f, 0.000f}, { 0.000f, 0.000f, 0.000f},
             { 0.000f, 0.000f, 0.000f}, { 0.299f, 0.587f, 0.114f}, { 0.299f, 0.587f, 0.114f}},
            // AM_TRUE — classic single-channel luma; max ghosting, historical
            {{ 0.299f, 0.587f, 0.114f}, { 0.000f, 0.000f, 0.000f}, { 0.000f, 0.000f, 0.000f},
             { 0.000f, 0.000f, 0.000f}, { 0.000f, 0.000f, 0.000f}, { 0.299f, 0.587f, 0.114f}},
        },
        // ---- AC_GREEN_MAGENTA [1] ----
        {
            // AM_DUBOIS — approximated from Dubois G/M filter measurements
            {{-0.063f,-0.162f, 0.042f}, { 0.285f, 0.665f, 0.150f}, {-0.015f,-0.027f, 0.021f},
             { 0.529f, 0.705f,-0.047f}, {-0.016f,-0.015f,-0.065f}, { 0.009f, 0.076f, 0.937f}},
            // AM_COMPROMISE
            {{ 0.000f, 0.000f, 0.000f}, { 0.146f, 0.738f, 0.141f}, { 0.000f, 0.000f, 0.000f},
             { 0.882f, 0.176f,-0.012f}, { 0.000f, 0.000f, 0.000f}, { 0.002f, 0.019f, 0.984f}},
            // AM_COLOR
            {{ 0.000f, 0.000f, 0.000f}, { 0.000f, 1.000f, 0.000f}, { 0.000f, 0.000f, 0.000f},
             { 1.000f, 0.000f, 0.000f}, { 0.000f, 0.000f, 0.000f}, { 0.000f, 0.000f, 1.000f}},
            // AM_HALF_COLOR
            {{ 0.000f, 0.000f, 0.000f}, { 0.299f, 0.587f, 0.114f}, { 0.000f, 0.000f, 0.000f},
             { 1.000f, 0.000f, 0.000f}, { 0.000f, 0.000f, 0.000f}, { 0.000f, 0.000f, 1.000f}},
            // AM_OPTIMISED
            {{ 0.000f, 0.000f, 0.000f}, { 0.000f, 0.700f, 0.300f}, { 0.000f, 0.000f, 0.000f},
             { 1.000f, 0.000f, 0.000f}, { 0.000f, 0.000f, 0.000f}, { 0.000f, 0.000f, 1.000f}},
            // AM_GREY
            {{ 0.000f, 0.000f, 0.000f}, { 0.299f, 0.587f, 0.114f}, { 0.000f, 0.000f, 0.000f},
             { 0.299f, 0.587f, 0.114f}, { 0.000f, 0.000f, 0.000f}, { 0.299f, 0.587f, 0.114f}},
            // AM_TRUE
            {{ 0.000f, 0.000f, 0.000f}, { 0.299f, 0.587f, 0.114f}, { 0.000f, 0.000f, 0.000f},
             { 0.299f, 0.587f, 0.114f}, { 0.000f, 0.000f, 0.000f}, { 0.000f, 0.000f, 0.000f}},
        },
        // ---- AC_AMBER_BLUE [2] ----
        {
            // AM_DUBOIS — approximated from Dubois A/B filter measurements
            {{ 1.062f, 0.366f,-0.057f}, {-0.063f,-0.019f, 0.019f}, {-0.003f,-0.016f, 0.031f},
             {-0.390f,-0.350f, 0.055f}, { 0.468f, 0.246f, 0.000f}, {-0.018f, 0.102f, 0.902f}},
            // AM_COMPROMISE
            {{ 0.840f, 0.238f, 0.014f}, { 0.059f, 0.642f, 0.033f}, { 0.000f, 0.000f, 0.000f},
             { 0.000f, 0.000f, 0.000f}, { 0.000f, 0.000f, 0.000f}, {-0.005f, 0.026f, 0.976f}},
            // AM_COLOR
            {{ 1.000f, 0.000f, 0.000f}, { 0.000f, 1.000f, 0.000f}, { 0.000f, 0.000f, 0.000f},
             { 0.000f, 0.000f, 0.000f}, { 0.000f, 0.000f, 0.000f}, { 0.000f, 0.000f, 1.000f}},
            // AM_HALF_COLOR
            {{ 0.299f, 0.587f, 0.114f}, { 0.299f, 0.587f, 0.114f}, { 0.000f, 0.000f, 0.000f},
             { 0.000f, 0.000f, 0.000f}, { 0.000f, 0.000f, 0.000f}, { 0.000f, 0.000f, 1.000f}},
            // AM_OPTIMISED
            {{ 1.000f, 0.000f, 0.000f}, { 0.000f, 1.000f, 0.000f}, { 0.000f, 0.000f, 0.000f},
             { 0.000f, 0.000f, 0.000f}, { 0.000f, 0.000f, 0.000f}, { 0.000f, 0.000f, 1.000f}},
            // AM_GREY
            {{ 0.299f, 0.587f, 0.114f}, { 0.299f, 0.587f, 0.114f}, { 0.000f, 0.000f, 0.000f},
             { 0.000f, 0.000f, 0.000f}, { 0.000f, 0.000f, 0.000f}, { 0.299f, 0.587f, 0.114f}},
            // AM_TRUE
            {{ 0.299f, 0.587f, 0.114f}, { 0.299f, 0.587f, 0.114f}, { 0.000f, 0.000f, 0.000f},
             { 0.000f, 0.000f, 0.000f}, { 0.000f, 0.000f, 0.000f}, { 0.299f, 0.587f, 0.114f}},
        },
        // ---- AC_TRIOVIZ [3] — TriOviz/Inficolor 3D ----
        // Physical glasses: magenta lens on LEFT eye, green lens on RIGHT eye.
        // (Confirmed by vpinball/vpinball#709 user report.)
        //
        // IMPORTANT: TriOviz is NOT a true anaglyph system. Both eyes see significant
        // amounts of all colours (VPX source comment: "not truly anaglyph since both
        // eyes see most of the colors"). Standard G/M Dubois produces ghosting because
        // it assumes complementary filters, which TriOviz does not have.
        //
        // VPX (vpinball/vpinball Anaglyph.cpp) has measured TriOviz filter data:
        //   Left (magenta) filter RGB transmission per primary:
        //     R: (0.8561, 0.0026, 0.0500), G: (0.1827, 0.1562, 0.1148), B: (0.0097, 0.0002, 0.7355)
        //   Right (green) filter RGB transmission per primary:
        //     R: (0.5470, 0.0229, 0.0210), G: (0.0005, 0.8109, 0.2163), B: (0.0011, 0.0031, 0.3776)
        // This data is used by VPX's LUMINANCE filter mode via SetLuminanceCalibration.
        // No published Dubois-style least-squares fit for these specific filters exists.
        //
        // AM_COLOR = SuperDepth3D.fx "Inficolor 3D Emulation Alpha" (exact match):
        //   out.R = L.R,  out.G = R.G,  out.B = L.B
        //   Source: BlueSkyDefender/Depth3D SuperDepth3D.fx, Stereo_Convert(),
        //   Inficolor_3D_Emulator branch, Stereoscopic_Mode==0.
        //
        // AM_HALF_COLOR = SuperDepth3D.fx "Inficolor 3D Emulation Beta" (exact match):
        //   out.R = L.R,  out.G = luma(R),  out.B = L.B
        //   Desaturates the right/green channel. Addresses the real-world observation
        //   that "the magenta side is much lighter than the green side" (Kodi forum,
        //   TamaraKama, 2015-10-29).
        //
        // AM_DUBOIS = standard Dubois 2001 G/M values with L/R swapped to match
        //   TriOviz physical orientation (left=magenta, right=green). Best available
        //   published approximation; will produce some ghosting on saturated content
        //   due to TriOviz's non-complementary filter design.
        {
            // AM_DUBOIS — VPX luminance filter from measured TriOviz filter data.
            // Computed via vpinball/vpinball Anaglyph.cpp Update() LUMINANCE method using
            // measured filter transmission in SetPhotoCalibration (Trioviz block).
            // rgb2Yl=[0.4668,0.3955,0.1377]  rgb2Yr=[0.1767,0.7842,0.0391]
            // White (1,1,1) -> (1,1,1) verified.
            {{ 0.9417f,  0.7979f,  0.2778f}, {-0.2591f,-0.2195f,-0.0764f}, { 0.9417f,  0.7979f,  0.2778f},
             { 0.0583f, -0.7979f, -0.2778f}, { 0.2591f,  1.2195f,  0.0764f}, {-0.9417f,-0.7979f,  0.7222f}},
            // AM_COMPROMISE — G/M compromise with L/R swapped
            {{ 0.882f, 0.176f,-0.012f}, { 0.000f, 0.000f, 0.000f}, { 0.002f, 0.019f, 0.984f},
             { 0.000f, 0.000f, 0.000f}, { 0.146f, 0.738f, 0.141f}, { 0.000f, 0.000f, 0.000f}},
            // AM_COLOR — SuperDepth3D "Inficolor 3D Emulation Alpha": out.R=L.R, out.G=R.G, out.B=L.B
            {{ 1.000f, 0.000f, 0.000f}, { 0.000f, 0.000f, 0.000f}, { 1.000f, 0.000f, 0.000f},
             { 0.000f, 0.000f, 0.000f}, { 0.000f, 1.000f, 0.000f}, { 0.000f, 0.000f, 0.000f}},
            // AM_HALF_COLOR — SuperDepth3D "Inficolor 3D Emulation Beta": out.R=L.R, out.G=luma(R), out.B=L.B
            {{ 1.000f, 0.000f, 0.000f}, { 0.000f, 0.000f, 0.000f}, { 1.000f, 0.000f, 0.000f},
             { 0.000f, 0.000f, 0.000f}, { 0.299f, 0.587f, 0.114f}, { 0.000f, 0.000f, 0.000f}},
            // AM_OPTIMISED — same as AM_COLOR (no better data available)
            {{ 1.000f, 0.000f, 0.000f}, { 0.000f, 0.000f, 0.000f}, { 1.000f, 0.000f, 0.000f},
             { 0.000f, 0.000f, 0.000f}, { 0.000f, 1.000f, 0.000f}, { 0.000f, 0.000f, 0.000f}},
            // AM_GREY — both eyes greyscale; near-zero ghosting, no colour
            {{ 0.000f, 0.000f, 0.000f}, { 0.299f, 0.587f, 0.114f}, { 0.000f, 0.000f, 0.000f},
             { 0.299f, 0.587f, 0.114f}, { 0.000f, 0.000f, 0.000f}, { 0.299f, 0.587f, 0.114f}},
            // AM_TRUE — left magenta luma, right green luma
            {{ 0.000f, 0.000f, 0.000f}, { 0.299f, 0.587f, 0.114f}, { 0.000f, 0.000f, 0.000f},
             { 0.299f, 0.587f, 0.114f}, { 0.000f, 0.000f, 0.000f}, { 0.000f, 0.000f, 0.000f}},
        },
    };
}
