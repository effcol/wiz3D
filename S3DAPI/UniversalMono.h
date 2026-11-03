#pragma once

// Universal mono list: shader CRCs known to be engine-default UI pixel/vertex
// shaders that render the same thing to both eyes regardless of game. Seeded
// into g_ProfileData's VSCRCData / PSCRCData before per-game profile load, so
// a game profile can override by listing the CRC with its own settings.
//
// Keep this list conservative: only add CRCs that are confirmed as engine
// defaults across multiple titles. Game-specific UI shaders stay in that
// game's <Mono> block in BaseProfile.xml / UserProfile.xml.
//
// Shared across DX7/8/9/10/11 wrappers via S3DAPI.

void ApplyUniversalMonoDefaults();
