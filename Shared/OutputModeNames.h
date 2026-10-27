/*
* Project : wiz3D
* Copyright (C) wiz3D contributors 2026
*
* Unified output-mode naming for every wiz3D wrapper. One canonical shortname
* per mode (e.g. "HalfSideBySide", "RowInterleaved"), one legacy DLL name
* kept for back-compat with iZ3D-era configs (e.g. "SideBySideOutput",
* "InterlacedOutput"), one numeric id used internally.
*
* The resolver below is pure-header, ASCII-only, zero-dependency so every
* consumer (S3DAPI, AmdQbProxy, NvDirectMode, each wrapper if it needs
* its own direct hook) can call into the same table.
*
* Priority at config read: a recognised string (canonical or legacy)
* overrides the numeric OutputMode. The caller writes the legacy DLL name
* back to its config state so DX9's plugin loader still finds the right
* plugin file even when the user wrote a canonical shortname in config.
* Unknown strings pass through unchanged, so DX9 user-added plugins and
* any mode we haven't yet registered keep working.
*
* The numeric ids are also unified across every modern wrapper (DX7-11,
* OGL, HD3D, 3DV Direct). HD3D and NvDirectMode used slightly different
* internal numbering in earlier builds (Anaglyph=7, SR_WEAVE=8 vs the
* OGL-origin numbering adopted here); the switch to this header requires
* those wrappers to renumber their enums to match. See wiz3D's 0.4.0
* release notes for the migration summary.
*/

#pragma once

#include <stddef.h>

namespace wiz3d {

enum OutputModeId
{
    OUTMODE_UNKNOWN              = -1,
    OUTMODE_IZ3D_DUAL            = 0,   // iZ3D dual-panel hardware (legacy)
    OUTMODE_HALF_SIDE_BY_SIDE    = 1,   // default
    OUTMODE_FULL_SIDE_BY_SIDE    = 2,   // game renders at half display width
    OUTMODE_HALF_TOP_AND_BOTTOM  = 3,
    OUTMODE_FULL_TOP_AND_BOTTOM  = 4,   // game renders at half display height
    OUTMODE_ANAGLYPH             = 5,   // Dubois Red/Cyan
    OUTMODE_ROW_INTERLEAVED      = 6,   // horizontal rows (passive 3D TVs)
    OUTMODE_COLUMN_INTERLEAVED   = 7,
    OUTMODE_CHECKERBOARD         = 8,   // DLP-Link projectors
    OUTMODE_SR_WEAVE             = 9,   // Simulated Reality (Leia/Samsung Odyssey 3D)
    OUTMODE_STEREO_MIRROR        = 10,  // second-display mirror
    OUTMODE_3DVISION_GLASSES     = 11,  // 0.5.0: NVIDIA 3D Vision shutter glasses auto-detect cascade
                                        //        (NV3D-Lib + Vision Restoration + ShutterOutput). Slot
                                        //        reserved; full wiring deferred until after team sync.
    OUTMODE_KATANGA              = 12,  // 0.5.0: produces a shared wide-SBS DX11 texture Katanga picks
                                        //        up and displays. bo3b's open-source stereo viewer.
                                        //        See project_katanga_output memory for the protocol.
};

struct OutputModeEntry
{
    int         mode;             // one of the OUTMODE_* ids
    const char* canonicalName;    // user-facing shortname
    const char* legacyDll;        // iZ3D-era OutputMethodDll value for back-compat
    int         legacySubmode;    // per-DLL submode index passed to CreateOutputDXn()
                                  // ctor (NOT the global `mode` id). Each shipped
                                  // output DLL has its own switch over 0..N; 0 is
                                  // the DLL's default. See EnumOutputModes() in
                                  // each OutputMethods/<DLL>/Output_dx9.cpp.
};

// 0.4.1 rename: short canonical names first (what OutputModeToCanonical
// returns, what the config comments show), legacy long-form names kept as
// alias rows below so pre-0.4.1 configs still parse. Legacy DLL column is
// the exact filename DX9's plugin loader looks up. Case-insensitive matching
// across the whole table.
//
// 0.4.3 note on DLLs that don't ship (TopAndBottomOutput, ColumnInterleavedOutput):
// these never had their own DLL in the shipped OutputMethods/ folder; the
// matching mode is served by a SUBMODE of a sibling DLL (TopAndBottomOutput
// -> SideBySideOutput submode 1 "Over/Under"; ColumnInterleavedOutput ->
// InterlacedOutput submode 1 "Vertical (Optimized)"). The unified refactor
// in 0.4.1 wired these to the wrong DLL + no submode, so e.g. selecting
// HalfTAB tried to load a missing DLL and selecting SBS passed OUTMODE_* id
// 1 (which happened to collide with SideBySideOutput's submode 1 = Over/Under),
// which is why SBS rendered as TAB. See kOutputModeTable rows below for the
// correct DLL + submode pairing.
static const OutputModeEntry kOutputModeTable[] =
{
    // --- Canonical short names (0.4.1+). These are what the config comments show. ---
    // 0.4.3: Full-SBS and Full-TAB alias to the SideBySideOutput DLL at the
    // same submode as their Half counterparts. The DLL is a surface blitter
    // that operates on whatever backbuffer size it finds -- user picks a
    // 2x-wide / 2x-tall display mode to get the actual "Full" presentation.
    // The DX11 Stereo-Injector and 3DV-Direct compositors use the same
    // UV-sampled approach; DX9 matches by alias rather than a dedicated DLL.
    { OUTMODE_HALF_SIDE_BY_SIDE,    "HalfSBS",                "SideBySideOutput",            0 },
    { OUTMODE_FULL_SIDE_BY_SIDE,    "FullSBS",                "SideBySideOutput",            0 },
    { OUTMODE_HALF_TOP_AND_BOTTOM,  "HalfTAB",                "SideBySideOutput",            1 },
    { OUTMODE_FULL_TOP_AND_BOTTOM,  "FullTAB",                "SideBySideOutput",            1 },
    { OUTMODE_ANAGLYPH,             "Anaglyph",               "AnaglyphOutput",              0 },
    { OUTMODE_ROW_INTERLEAVED,      "RowIL",                  "InterlacedOutput",            0 },
    { OUTMODE_COLUMN_INTERLEAVED,   "ColumnIL",               "InterlacedOutput",            1 },
    // 0.4.3: Checker aliases to DLP3DOutput (the dedicated DLP 3D-Ready
    // output DLL that produces the checkerboard pattern DLP-Link glasses
    // sync on). S3DOutput submode 10 also has a Checkerboard but it's
    // #ifndef FINAL_RELEASE-gated so unavailable in shipped builds.
    { OUTMODE_CHECKERBOARD,         "Checker",                "DLP3DOutput",                 0 },
    { OUTMODE_SR_WEAVE,             "SRWeave",                "SimulatedRealityWeaveOutput", 0 },
    { OUTMODE_STEREO_MIRROR,        "Mirror",                 "StereoMirrorOutput",          0 },
    { OUTMODE_IZ3D_DUAL,            "iZ3D",                   "S3DOutput",                   0 },
    // 0.5.0: 3D Vision Glasses output -- reserved, implementation deferred
    // pending bo3b + Akshay sync. See project_v043_in_flight_hotfix for the
    // planned cascade; slot kept in the enum so the config schema is stable.
    { OUTMODE_3DVISION_GLASSES,     "3DVisionGlasses",        "ShutterOutput",               0 },
    // 0.5.0: Katanga output -- produce a shared wide-SBS DX11 texture bo3b's
    // Katanga app picks up and displays. No Output DLL load; the wrapper
    // handles the shared-handle + sync primitive directly. The DLL field
    // here is a last-resort fallback to SideBySideOutput if the shared-
    // handle creation fails (user still sees stereo on-screen as SBS).
    { OUTMODE_KATANGA,              "Katanga",                "SideBySideOutput",            0 },
    // --- Ultra-short bare-mode aliases (user convenience). "SBS" means
    //     HalfSBS, "TAB" means HalfTAB -- picking the common half variants.
    { OUTMODE_HALF_SIDE_BY_SIDE,    "SBS",                    "SideBySideOutput",            0 },
    { OUTMODE_HALF_TOP_AND_BOTTOM,  "TAB",                    "SideBySideOutput",            1 },
    // --- Legacy long-form aliases (pre-0.4.1 names kept recognised). ---
    { OUTMODE_HALF_SIDE_BY_SIDE,    "HalfSideBySide",         "SideBySideOutput",            0 },
    { OUTMODE_FULL_SIDE_BY_SIDE,    "FullSideBySide",         "SideBySideOutput",            0 },
    { OUTMODE_HALF_TOP_AND_BOTTOM,  "HalfTopAndBottom",       "SideBySideOutput",            1 },
    { OUTMODE_FULL_TOP_AND_BOTTOM,  "FullTopAndBottom",       "SideBySideOutput",            1 },
    { OUTMODE_ROW_INTERLEAVED,      "RowInterleaved",         "InterlacedOutput",            0 },
    { OUTMODE_COLUMN_INTERLEAVED,   "ColumnInterleaved",      "InterlacedOutput",            1 },
    { OUTMODE_CHECKERBOARD,         "Checkerboard",           "DLP3DOutput",                 0 },
    { OUTMODE_SR_WEAVE,             "SimulatedRealityWeave",  "SimulatedRealityWeaveOutput", 0 },
    { OUTMODE_STEREO_MIRROR,        "StereoMirror",           "StereoMirrorOutput",          0 },
    { OUTMODE_IZ3D_DUAL,            "iZ3DDualPanel",          "S3DOutput",                   0 },
    { OUTMODE_3DVISION_GLASSES,     "3DVisionShutter",        "ShutterOutput",               0 },
    { OUTMODE_3DVISION_GLASSES,     "ShutterGlasses",         "ShutterOutput",               0 },
    // --- Pre-0.4.1 wrong-DLL aliases (recognise so users with stale configs
    //     still land on the right DLL + submode after a 0.4.3 upgrade). ---
    { OUTMODE_HALF_TOP_AND_BOTTOM,  "TopAndBottomOutput",     "SideBySideOutput",            1 },
    { OUTMODE_COLUMN_INTERLEAVED,   "ColumnInterleavedOutput","InterlacedOutput",            1 },
};

// Hardware-specific legacy plugin DLL names, kept recognised so the resolver
// doesn't mangle them even though we don't assign them canonical shortnames
// or inline-wrapper support. Users with the matching hardware keep using
// their existing DX9 plugin configs; new wrappers see "unknown" and leave
// the state alone, so the DX9 plugin loader still finds the DLL.
static const char* const kLegacyHardwarePlugins[] =
{
    "CMOShutterOutput",  "iZShutterOutput",  "ShutterOutput",     "S3DShutter",
    "ZalmanOutput",      "VR920Output",      "Z800Output",        "LenovoOutput",
    "DualProjectionOutput", "TaerimOutput",  "AvitridOutput",
    "S3D120HzProjectorsOutput", "3DDLPOutput", "ATI3DDLPOutput",
    "AMDHD3DOutput",     "S3DMarkedOutput",
};

// Case-insensitive ASCII C-string compare (names in config are ASCII).
inline int OutputModes_StrICmp(const char* a, const char* b)
{
    while (*a && *b)
    {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return (int)(unsigned char)ca - (int)(unsigned char)cb;
        ++a; ++b;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

// Returns an OUTMODE_* id if `name` matches a canonical shortname OR a
// legacy DLL name in the table. Returns OUTMODE_UNKNOWN for null, empty,
// unrecognised, or hardware-plugin names (callers check IsLegacyHardwarePlugin
// if they want to distinguish those).
inline int ResolveOutputModeName(const char* name)
{
    if (!name || !*name) return OUTMODE_UNKNOWN;
    for (size_t i = 0; i < sizeof(kOutputModeTable) / sizeof(kOutputModeTable[0]); ++i)
    {
        const OutputModeEntry& e = kOutputModeTable[i];
        if (OutputModes_StrICmp(name, e.canonicalName) == 0 ||
            OutputModes_StrICmp(name, e.legacyDll)     == 0)
            return e.mode;
    }
    return OUTMODE_UNKNOWN;
}

// Returns the legacy DLL filename stem (no ".dll" suffix) for an OUTMODE_*
// id, or nullptr for an unregistered id. Used by DX9's plugin loader so a
// user who wrote a canonical shortname in config still gets the right
// plugin DLL loaded.
inline const char* OutputModeToLegacyDll(int mode)
{
    for (size_t i = 0; i < sizeof(kOutputModeTable) / sizeof(kOutputModeTable[0]); ++i)
        if (kOutputModeTable[i].mode == mode) return kOutputModeTable[i].legacyDll;
    return nullptr;
}

// Returns the canonical shortname for an OUTMODE_* id, or nullptr.
inline const char* OutputModeToCanonical(int mode)
{
    for (size_t i = 0; i < sizeof(kOutputModeTable) / sizeof(kOutputModeTable[0]); ++i)
        if (kOutputModeTable[i].mode == mode) return kOutputModeTable[i].canonicalName;
    return nullptr;
}

// Returns the per-DLL submode index for an OUTMODE_* id -- the value the
// shipped OutputMethods/<DLL>/Output_dx9.cpp constructor expects as its
// `mode` arg. Different from the global OUTMODE_* id: e.g. HalfTAB is
// global id 3, but is submode 1 of SideBySideOutput.dll (Over/Under).
// Returns 0 (the DLL's default submode) for unregistered ids, which is
// also the right answer for every DLL whose EnumOutputModes only lists
// a "Default" case.
inline int OutputModeToLegacySubmode(int mode)
{
    for (size_t i = 0; i < sizeof(kOutputModeTable) / sizeof(kOutputModeTable[0]); ++i)
        if (kOutputModeTable[i].mode == mode) return kOutputModeTable[i].legacySubmode;
    return 0;
}

// Returns true if `name` matches a recognised legacy hardware plugin DLL
// name (CMOShutter, Zalman, VR920, etc.). These pass through the resolver
// unchanged so the DX9 plugin pipeline still loads them.
inline bool IsLegacyHardwarePlugin(const char* name)
{
    if (!name || !*name) return false;
    for (size_t i = 0; i < sizeof(kLegacyHardwarePlugins) / sizeof(kLegacyHardwarePlugins[0]); ++i)
        if (OutputModes_StrICmp(name, kLegacyHardwarePlugins[i]) == 0)
            return true;
    return false;
}

}  // namespace wiz3d
