# lib/vld — Visual Leak Detector (dormant)

**Status as of 2026-05-27 audit: dormant but intentionally retained.**

This folder vendors the headers (`include/vld.h`, `include/vld_def.h`) and runtime
config (`vld.ini`) for [Visual Leak Detector](https://github.com/KindDragon/vld),
an MSVC memory leak detector. VLD is **not currently active** in any build, but
the wiring is kept on purpose so a developer can quickly re-enable it.

## What's in the tree

- `include/vld.h`, `include/vld_def.h` — public headers (auto-link `vld_x86.lib`
  / `vld_x64.lib` via `#pragma comment(lib, ...)` when included).
- `vld.ini` — runtime configuration (report mode, callstack depth, etc.).
- **No `.lib` binaries.** If VLD is re-enabled, the matching `vld_x86.lib` /
  `vld_x64.lib` must be re-vendored under `lib/vld/lib/{x86,x64}/`, and that
  path added to the consuming project's `AdditionalLibraryDirectories`.

## How it's referenced (and why it's dormant)

Every `#include <vld.h>` in the source tree is **commented out**:

- `S3DAPI/dllmain.cpp` — `//#include <vld.h>`
- `S3DWrapper9/S3DWrapperD3D9.cpp` — `//#include <vld.h>`
- `S3DWrapper10/S3DWrapperD3D10.cpp` — `//#include <vld.h>`

The companion `VLDSetReportHook(...)` calls are also commented out. To turn VLD
on for a wrapper DLL: re-vendor the `.lib` files, uncomment the `#include`, and
uncomment the `VLDSetReportHook` line in that DLL's entry point.

## Active leak detection today

A leak-checking path is still wired up — it just uses the **MSVC CRT debug
heap**, not VLD:

```cpp
_CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
_CrtSetReportHookW2(_CRT_RPTHOOK_INSTALL, zlog::VldReportHook);
```

The hook function `zlog::VldReportHook` (defined in `ZLOg/ZLOg.cpp`) keeps its
VLD-era name as a historical fossil — it's actually routing CRT leak reports
through the project's log channel, not VLD reports.

## Modern alternatives

If wiring fresh leak tooling, VLD is no longer the modern default. Stronger
options on Windows in 2026:

- **MSVC AddressSanitizer** (`/fsanitize=address`) for memory *corruption* bugs
  (UAF, OOB) — note: LeakSanitizer is **not available** on MSVC ASan.
- **UMDH + `gflags +ust`** (Windows SDK Debugging Tools) for process-level
  leak hunting against a running host game — the best fit for the
  injected-wrapper architecture.
- **Dr. Memory** for an easier-onramp Valgrind-style checker.
