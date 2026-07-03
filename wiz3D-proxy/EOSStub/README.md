# EOSStub — a do-nothing replacement for the Epic Online Services SDK

`EOSStub` builds a drop-in replacement for `EOSSDK-Win32-Shipping.dll` (and
`EOSSDK-Win64-Shipping.dll`) in which **every export returns 0**. It disables
Epic Online Services entirely — auth, achievements, RTC, and crucially the Epic
overlay (`EOSOVH-*-Shipping.dll`) that the SDK loads on its own.

## Why

The EOS overlay crashes inside Simulated Reality weaving: its hooked-object
table chokes on the extra D3D swapchains the SR weaver creates (see
`project_tr2013_sr_dead_end`). We can't block the overlay from our game-folder
D3D proxies — EOSSDK is a **static import**, so it (and the overlay) load before
any of our other code runs. Replacing EOSSDK itself is the only intervention
point early enough, and because we *are* the imported module, every caller is
caught regardless of how it resolves a function (IAT, `GetProcAddress`, ordinal).

`0` is the correct "success" value: EOS uses `EOS_EResult` where
`EOS_Success == 0`, and `0` doubles as a NULL handle / `EOS_FALSE`.
`EOS_Platform_Create` returning NULL is the documented "no platform" path, so a
well-behaved game just skips EOS.

## Caveats

- **This disables ALL of EOS**, not only the overlay. Use it for titles where
  you don't care about EOS features (e.g. a Steam game with EOS bolted on).
- A game that *hard-requires* a valid platform handle, or that blocks waiting on
  an EOS callback that now never fires, may fail to start or hang. For those,
  use a pass-through shim (forward every export to the real DLL, intercept only
  `EOS_Platform_Create` to strip the overlay flags) instead.

## Use

1. Build `wiz3D-proxy-EOSStub` (Win32; the validated target. See below for x64).
2. In the game folder, back up the real DLL, e.g.
   `EOSSDK-Win32-Shipping.dll` → `EOSSDK-Win32-Shipping.dll.bak`.
3. Copy the built stub (`bin\Release\Win32\EOSSDK-Win32-Shipping.dll`) into the
   game folder.
4. Launch. An `EOSStub.log` next to the DLL confirms it attached.

This is an **opt-in, per-game tool** — it is deliberately *not* part of the
standard wiz3D release payload, since it would disable EOS for every game.

## Regenerating for a different EOSSDK version

The export set must match the EOSSDK the game ships, byte-for-byte (a missing
export means the game won't launch). `exports_x86.def` and
`eos_stubs.generated.cpp` are generated from a real EOSSDK by:

```powershell
.\generate_exports.ps1 -EosDll "path\to\game\EOSSDK-Win32-Shipping.dll"
```

**This project is Win32-only** — that's what was validated in-game (TR2013).
There is intentionally no x64 build: a 64-bit stub would be unverified. To
target a 64-bit game, run `generate_exports.ps1` against a real
`EOSSDK-Win64-Shipping.dll` (it emits `exports_x64.def`), then add x64
configurations back to the vcxproj. The script handles either architecture.
