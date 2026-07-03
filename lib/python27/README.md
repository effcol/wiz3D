# lib/python27 — Vendored Python 2.7 (legacy build tooling)

**Status as of 2026-05-27 audit: actively referenced, but significantly rotted.**
**Python 2.7 reached end-of-life on 2020-01-01 — no upstream security or bug
fixes.** Retained because a handful of build/tooling `.bat` scripts still invoke
this interpreter; bulk-deleting it would break those workflows. Modernization
(port to Python 3 + system-installed or freshly-vendored interpreter) is the
recommended longer-term path. Best inferences below — treat workflow liveness
claims as guesses pending confirmation.

## What's in the tree

A full embedded CPython 2.7 distribution (~67 MB):

- `python.exe`, `pythonw.exe`
- `DLLs/`, `Lib/`, `include/`, `Scripts/`
- `Lib/site-packages/`: **wxPython 2.8** (`wx-2.8-msw-ansi`), **reportlab**,
  **pyPdf**, **PIL**, **Pygments**, **setuptools**

No project links `python27.lib` and no source `#include`s `Python.h` — Python
is invoked as a standalone interpreter only.

## Who invokes it (four workflows, varying liveness)

1. **`BuildSetup/.../SetBranchFolder.py`** — called from every
   `BuildAndDeploy_*Edition.bat` (Anaglyph, CMO, Lenovo, Taerim, YGT, iZ3D) and
   the `_BuildSetup.bat` variants. **Likely dead:**
   - Uses Python-2-only syntax (`file(...)` builtin), so it would not run on
     Python 3 without porting.
   - Parses SVN URLs (`svn://svn.neurok.ru/dev/driver/...`) to derive branch
     folder names. The repo is in **git** now, so whatever branch-name logic
     this script computed is almost certainly stale.

2. **`BuildSetup/GenerateListingPDF.bat`** — calls `generate_listing.py`
   (presumably reportlab-based). **The script is missing** — this `.bat` is
   already broken.

3. **`Metrics/cppcheck.bat`** — runs `cppcheck-htmlreport`, the Python helper
   that ships with cppcheck. Uses Python as a generic interpreter; would work
   on Python 3 unchanged.

4. **`ZLOg/GUI/ZLOgui.bat`** — launches `ZLOgui.pyw`, a **wxPython 2.8** log
   viewer GUI (`import wx`). This is what the vendored `wx-2.8-msw-ansi`
   site-package is for. Liveness unknown — depends on whether anyone still
   runs the GUI viewer over ZLog output.

## What to do (when someone gets to it)

Suggested cleanup pass, in order of confidence:

1. **Delete `GenerateListingPDF.bat`** — it calls a missing script.
2. **Confirm the `BuildAndDeploy_*Edition.bat` family is still in use.** If the
   branded-edition pipeline is defunct, those `.bat` files and
   `SetBranchFolder.py` can all go together.
3. **Decide ZLOgui's fate** — keep (port to Python 3 + wxPython 4 / Phoenix),
   rewrite, or delete.
4. **Switch `cppcheck.bat` to a system Python 3 invocation** (or port to a
   direct cppcheck XSLT, sidestepping Python entirely).
5. Once all four call sites are resolved, `lib/python27/` can be removed.
