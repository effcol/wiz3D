# Vulkan

🚧 **In flight (0.5.0).** Akshay's Vulkan Stereo-Injector wrapper is being prepped for PR around 2026-11-08. Pre-release scope:

- Stereo renders in **any deployed Vulkan game** that uses standard descriptor-set layouts (no crashes; shadows / reflections may still need per-game shader fixes).
- New per-game profile support alongside the DX11 Feature Engine update that lands in the same PR.
- Known-good emulator targets: BotW (Cemu), Tears of the Kingdom (Ryubing), Ocarina of Time Remake (Lichii), Demon's Souls (KytyPS5).
- Known-good retail target in Akshay's own testing: Red Dead Redemption 2.

## Install path

`Stereo-Injector/Vulkan/` (final arch/subfolder layout TBD until the PR lands).

## Environment variables needed for some emulators

Some Vulkan emulators load their Vulkan loader in a way that skips drop-in proxy DLLs next to the exe. For those, an env-var override is needed — otherwise the emulator uses the system Vulkan loader and our proxy never gets a chance:

| Emulator / target | Reason | Status |
|---|---|---|
| Lichii — Ocarina of Time Remake | Uses system Vulkan loader by default | Env var required (details once PR lands) |
| KytyPS5 — Demon's Souls | Uses system Vulkan loader by default | Env var required; also needs a stereo-mode shader-precompilation pass to avoid first-run stutter |

Long term, the [Stereopticon](https://github.com/effcol/Stereopticon) launcher will set those env vars per-game. Short term, additional install instructions will document the env-var dance per emulator.

## Performance note

Akshay reports **200 FPS in Demon's Souls** with 4× MFG + DLSS Quality + stereo enabled on his test rig. Vulkan + the emulator's compute-based approach scales well with modern GPUs.

## Games

### Known targets (pending Akshay's PR)

| Game / target | Engine | Status | Notes |
|---|---|---|---|
| Red Dead Redemption 2 | RAGE | 🧪 | Akshay's primary test target. |
| Doom (2016) | idTech 6 | 🧪 | Pending our test. |
| Doom Eternal | idTech 7 | 🧪 | Pending our test. |
| Cemu — Breath of the Wild | — | 🧪 |  |
| Ryubing — Tears of the Kingdom | — | 🧪 |  |
| Lichii — Ocarina of Time Remake | — | 🧪 | Env var required. |
| KytyPS5 — Demon's Souls | — | 🧪 | Env var required + stereo-mode shader precompile. |

Please report additional Vulkan titles via [GitHub Issues](https://github.com/effcol/WiZ3D/issues).
