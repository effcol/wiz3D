/*
* Project : wiz3D
* Copyright (C) wiz3D contributors 2026
*
* LeiaSR panel-info query helper.
*
* Reads the recommended views-texture dimensions from the LeiaSR Displays
* runtime (SimulatedRealityDisplays.dll + SimulatedRealityCore.dll). These
* are the SBS input texture dimensions the SR weaver wants on this exact
* panel and calibration. If the SR runtime isn't installed, the query
* returns false and the caller falls back to backbuffer-sized weave.
*
* Loaded lazily at runtime via LoadLibraryW + GetProcAddress -- no vendored
* LeiaSR SDK dependency, no link-time import, no delay-load config needed.
* Companion to the existing SR-Lib shim, which doesn't expose this
* information; see task #35 for the planned upstream PR to bo3b/SR-lib.
*/

#pragma once

namespace wiz3d
{

// Queries SimulatedRealityDisplays for the recommended views (SBS input)
// texture dimensions. outWidthSBS is the TOTAL side-by-side width (both
// eyes combined), i.e. 2 * per-eye width. Returns false and leaves the
// output ints unchanged if the SR runtime is absent or any step fails.
bool QueryRecommendedSRViewsSize(int& outWidthSBS, int& outHeight);

// Returns the panel-native per-eye dimensions the SR weave should target.
// Preferred source is the LeiaSR Displays runtime (QueryRecommendedSRViewsSize/2);
// if that is absent, falls back to the Windows monitor the given hwnd lives on
// (MonitorFromWindow + GetMonitorInfo). This way sub-native fullscreen games
// still get a lenticular-aligned weave even on SR panels without the Displays
// SDK installed. Returns false only when both sources fail (hwnd invalid, no
// monitor info, no SR runtime). outPanelW / outPanelH are the PANEL dims (one
// eye's worth), not the SBS width.
bool QueryPanelNativeSize(void* hwnd, int& outPanelW, int& outPanelH);

}  // namespace wiz3d
