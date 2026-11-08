# Compatibility

Overview of which games work with which path. For the full per-game listings see each API's own page:

**Stereo-Injector pages**

- [DirectX 7](API-DirectX-7)
- [DirectX 8](API-DirectX-8)
- [DirectX 9](API-DirectX-9) — large list (~250 titles)
- [DirectX 10](API-DirectX-10)
- [DirectX 11](API-DirectX-11)
- [DirectX 12](API-DirectX-12) — coming soon
- [Vulkan](API-Vulkan) — in flight
- [3D Vision Automatic-aware games](Injector-3D-Vision-Automatic-Aware)

**Native-stereo pages**

- [OpenGL (Quad-Buffer)](Native-OpenGL-Quad-Buffer)
- [HD3D (AMD)](Native-HD3D)
- [3D Vision Direct (NVIDIA)](Native-3D-Vision-Direct)

**Status key** (used across all pages)

| Status | Meaning |
|---|---|
| ✅ Working | Stereo renders correctly; any per-game fix is shipped in `BaseProfile.xml`. |
| 🟡 Partial / Mostly | Stereo works but has rendering artefacts (shadows, reflections, UI depth). Usually improved by a per-game ShaderFix. |
| 🔴 Not working | Crashes, hangs, or stereo is fundamentally wrong (wrong eye content, no depth). |
| 🧪 Untested / Pending | Awaiting a test report. |

**What's in each cell**

- **Install Path** — the release folder that applies to this game.
- **iZ3D Profile** — `✓` if the shipped `BaseProfile.xml` has a block inherited from the original iZ3D profile set, `✕` if not. Independent of working/not-working; a game without a profile can still work out of the box if nothing in it conflicts with the defaults.
- **Notes** — gotchas (Admin install, env-var setup, known-broken effects).

## How to add a row

1. Fork this wiki, or raise an issue with your test report.
2. Row shape: `Game | Install Path | iZ3D Profile (✓/✕) | Testing | Notes`.
3. For "Testing" — if the title already needed a per-game profile, say which section of `BaseProfile.xml` or whether it was a `UserProfile.xml` override.
4. **Please include**: wiz3D version tested, GPU + driver, OS, output-mode value, screenshot if you can.

Reports via GitHub issue (preferred — easier to triage) or Discord.
