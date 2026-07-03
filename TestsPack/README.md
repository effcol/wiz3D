# TestsPack — iZ3D-era test suite (historical / study reference)

This folder is the original iZ3D automated test suite. **It is retained as
documentation, not as a live test harness.** None of it is run today, and there
is no plan to revive it — but it is kept (and the C++ parts kept *building*)
because it is the clearest surviving record of how many subsystems were
*expected* to behave, and that intent isn't captured anywhere else in the code.

## Status: dormant, but possibly useful for study

In its day this was a strong regression suite — automated runs (likely across a
batch of games) streaming results to an iZ3D TeamCity CI server. That world is
gone:

- the TeamCity server and `iz3d.com` are defunct (the tests emit TeamCity
  service messages to nobody),
- the game test rigs and the people who knew how to drive them are gone,
- the scripted parts depend on EOL tooling (Python 2.7, AutoIt3) that isn't
  vendored here.

So treat these as **read-only history**. Their value now is documentary:

- They are a readable spec of expected subsystem behavior (resource lifetimes,
  hook semantics, shader analysis/modification, command-buffer merging, the
  allocator, the Control Center UI ↔ profile-XML mapping).
- If you're reverse-engineering "what was this supposed to do?", the relevant
  test here is a legitimate source to read alongside the code.

See also the `project-testspack-history` engineering note for the original
"keep this as history" decision and more on CCTest's role.

## What builds, and the Boost dependency

The C++ projects still compile under the current toolchain (v145 / Win10 SDK),
and the solution keeps them building so they don't silently rot. Six of them
link Boost's `unit_test_framework` — the **only** compiled Boost lib used
anywhere in this codebase, vendored minimally for exactly this purpose. See
`ThirdPartyLibs/boost_v1.91.0/README.md` for that story. They **compile and
link**, but "passing" is meaningless without the (gone) runtime environment.

The aggregate runner is `Testing/Testing.vcxproj` at the repo root (not in this
folder); it pulls in the C++ subsystem tests.

## Contents

| Tool | Area | Kind |
|------|------|------|
| `CCTest` | Control Center GUI ↔ `BaseProfile.xml`/`UserProfile.xml` mapping | **Scripted** (AutoIt3 + Python 2.7) — dormant, no interpreters vendored |
| `D3D8ResourceLeak` | D3D8 wrapper resource ref-count / leak checks | C++ (boost::test) |
| `D3D9ResourceLeak` | D3D9 wrapper resource ref-count / leak checks | C++ (boost::test) |
| `HookAPITest` | API hooking infrastructure (MadCHook/MinHook-era) | C++ (boost::test) |
| `SmallObjectAllocatorTest` | small-object allocator | C++ (boost::test) |
| `DX10CommandBufferTest` | D3D10 command-buffer merge logic | C++ (boost::test) |
| `DX9ShaderAnalyzingTest` | D3D9 shader analysis | C++ (boost::test) |
| `DX9ShaderModifyingTest` | D3D9 shader modification (e.g. matrix detection) | C++ |
| `DX9OutputTest` | D3D9 output / texture path | C++ |
| `DX10ShaderAnalyzingTest` | D3D10 shader analysis | C++ |
| `DX10OutputTest` | D3D10 output path | C++ |
| `ConvertPerfomanceTest` | conversion performance (name misspelled in original) | C++ |

### CCTest, specifically

`CCTest/CCTest.au3` (with `Autofocus.py`, `hotkeys.py`, `InGameSettings.py`,
`testing.py`) is a readable spec of what the C# WinForms Control Center was
supposed to do: how user-facing features map to `BaseProfile.xml` /
`UserProfile.xml` attributes, and what the profile schema namespace
(`http://schemas.iz3d.com/baseprofile/2007`) looked like. It is the single best
artifact for understanding the old Control Center, even though it can't run.

## If you want to actually run these

You'd be rebuilding the harness, not just clicking Run: stand up a result sink
(or strip the TeamCity reporting), supply the runtime environment each test
assumes (a driver build, target apps/games, and for CCTest the Control Center
plus AutoIt3 and a Python 2.7 interpreter). That's a project in itself — see the
`project-testspack-history` note before going down that road.
