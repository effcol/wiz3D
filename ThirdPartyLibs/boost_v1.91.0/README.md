# Boost 1.91.0 — minimal vendored drop

This is a **deliberately minimal** Boost drop, and it is shaped differently from
the other libraries under `ThirdPartyLibs/`. Read this before adding to it.

## Why Boost is special here

Boost is enormous (the full distribution is hundreds of MB across thousands of
compiled objects), but this codebase uses almost none of that:

- **The production code uses Boost header-only.** `shared_ptr`, `unordered_map`,
  `lexical_cast`, `pool`, `graph`, `property_tree`, `tokenizer`, `variant`, etc.
  are all header-only. `BOOST_ALL_NO_LIB` is defined globally (see
  `Directory.Build.props`) to disable Boost's `#pragma comment(lib, ...)`
  auto-link, so nothing in the shipping driver links a compiled Boost lib.
- **Exactly one compiled Boost lib is actually linked:** `unit_test_framework`,
  and only by the dormant-but-kept **TestsPack** regression suite (see the
  `project-testspack-history` note). Even there, no transitive closure libs
  (filesystem, chrono, thread, atomic, ...) were needed — the framework lib is
  self-contained for this usage in 1.91.

So vendoring the whole Boost binary set would be hundreds of MB of dead weight to
satisfy a single lib used by a handful of non-shipping test projects.

## What's in this drop

```
boost_v1.91.0/
  Include/boost/   Full header tree (8,283 files), faithful to the vcpkg export.
                   Arch-independent — one copy serves both Win32 and x64.
  lib/             FLAT folder with ONLY the compiled lib we link:
                     boost_unit_test_framework-vc145-mt-x64-1_91.lib       (Release x64)
                     boost_unit_test_framework-vc145-mt-gd-x64-1_91.lib    (Debug   x64)
                     boost_unit_test_framework-vc145-mt-x32-1_91.lib       (Release x86)
                     boost_unit_test_framework-vc145-mt-gd-x32-1_91.lib    (Debug   x86)
  LICENSE          Boost Software License 1.0 (permissive).
  README.md        This file.
```

### Why the lib folder is flat (no `arch/config` subtree)

Every other vendored lib uses an `lib/<arch>/<config>/` layout because their
filenames don't distinguish variants. Boost's filenames already encode **every**
axis — toolset (`vc145`), threading (`mt`), debug tag (`gd`), address model
(`x32`/`x64`), and version (`1_91`) — so subfolders would be pure redundancy.
A single flat folder of self-describing names is clearer and visibly signals
"this drop is different / minimal."

`ThirdPartyLibs.props` selects the right variant **by name**, not by path:
`$(BoostLib)` builds the filename from `$(_Arch32)` (Win32→`x32`, x64→`x64`) and
adds the `-gd-` infix in Debug. Final Release uses the plain Release lib.

## How this was produced

Classic-mode vcpkg used purely as a build factory (the project itself is no
longer on vcpkg — `vcpkg.json` was removed once Boost migrated):

1. `vcpkg install <22 boost-* ports>:<triplet>` for `x64-windows-static` and
   `x86-windows-static`. The 22 top-level ports pull a ~81-package closure.
2. `vcpkg export ... --raw` to get a faithful artifact.
3. Copy the **headers** from the x64 export (arch-independent) into `Include/`.
4. Copy **only** `unit_test_framework` (Debug+Release, both arches) into `lib/`.

The 22 ports map to actual `#include <boost/...>` usage: algorithm, bind,
circular-buffer, core, filesystem, function, graph, lexical-cast, pool,
property-tree, ptr-container, serialization, smart-ptr, static-assert, test,
thread, throw-exception, tokenizer, type-traits, unordered, utility, variant.

## If you need another compiled Boost lib

Don't vendor the whole binary set. Add just the lib(s) the linker actually asks
for: drop the named `.lib` files into this flat `lib/` folder and add a name
property in `ThirdPartyLibs.props` following the `$(BoostLib)` pattern. Discover
the real need by building and reading the `LNK2019`/`LNK2001` symbols, exactly
as the rest of the `ThirdPartyLibs` migration was done.

## Deviation note

This headers-faithful-but-libs-minimal shape is an intentional exception to the
"faithful drop on disk, exact-use wiring" convention used elsewhere. The full
header tree is kept faithful (Boost headers cross-include heavily and can't be
safely subsetted), but the compiled libs are pruned to exact use because
`BOOST_ALL_NO_LIB` makes the non-linkage an explicit architectural decision, not
an accident — and the alternative is hundreds of MB of unused binaries.
