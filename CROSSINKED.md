# CrossInked

A fork of [CrossInk](https://github.com/uxjulia/CrossInk) (itself a fork of
[CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader))
for the Xteink X3/X4, adding features tuned for a large, series-heavy,
Project-Gutenberg-inclusive personal library.

## What's new in CrossInked

| # | Feature | Status | Where |
|---|---------|--------|-------|
| 1 | **Series-gap markers** in the file browser | ✅ implemented, unit-tested + visually verified | `FileBrowserActivity.cpp`, `NaturalSort.*` |
| 2 | **`start`-only guide reference fix** (Gutenberg books open at chapter 1, not front matter) | ✅ implemented, integration-tested | `ContentOpfParser.*`, `Epub.cpp` |
| 3 | **Remember last-browsed folder** for "Browse Files" | ✅ implemented, persistence verified | `HomeActivity.cpp`, `FileBrowserActivity.cpp`, `CrossPointState`, `JsonSettingsIO` |
| 4 | **Jump-to-letter-group** on hold in the browser | ✅ implemented, unit-tested | `FileBrowserActivity.cpp`, `NaturalSort.*` |
| 5 | **Content-key cache survival** — manual SD renames stop resetting progress | ✅ implemented, sim-verified (⚠ device-test) | `Epub.cpp`, `Epub.h` |
| 7 | **Cover prewarm** ("Rebuild Covers") maintenance action | ✅ implemented (⚠ device-test heap/watchdog) | `PrewarmCoversActivity.*`, `SettingsList.h` |
| 9 | **Test/tooling**: natural-sort + gap unit tests, simulator screenshot capture | ✅ implemented | `test/natural_sort/`, `SimulatorSmokeTest.cpp` |
| — | Simulator build fixes (mock API skew) | ✅ | `WifiSelectionActivity.cpp`, `main.cpp` |
| 6,8 | Primary-author display, orphan-cache reaper | 📋 designed — see [ROADMAP.md](ROADMAP.md) | — |

> ⚠ **Needs on-device verification**: #5 (cache adopt/rename) and #7 (heap/watchdog under a full library) pass in the simulator, but their real failure modes only surface on hardware. Flash-test before relying on them.

### 4. Jump-to-letter-group
Holding the nav button in Books mode jumps the selector to the next/previous
first-letter group (numbered entries collapse to one group) instead of a fixed
page — far faster across a 95-author folder. Firmware picker keeps page-jump.

### 5. Content-key cache survival
Each cache dir gets a `content.key` sidecar (`fnvHash64(title|author)` + source
path). On a cache miss, if an *orphaned* cache (matching key, whose source file
no longer exists) is found, CrossInked adopts it (renames the dir) instead of
rebuilding — so renumbering/reorganizing files directly on the SD card keeps
reading progress, layout cache, and stats. Sim-verified: rename A→B → *"Adopting
orphaned cache … (content-key match, source gone)"*, no rebuild.

### 7. Cover prewarm
**Settings → System → Rebuild Covers** walks `/books` and pre-decodes every
cover/thumbnail so the recent grid/carousel isn't stuttery on first browse.
RAM-safe incremental dir-stack walk (never materializes all ~1,850 paths), with
a live count and cancel.

### 1. Series-gap markers
When two consecutive numbered files in a folder skip a number
(e.g. `6 …` then `8 …`), the browser marks the later row with the missing
number(s) in its value column: `[7]` for one, `[13-14]` for a range. Decimals
(`16.5` novellas) and omnibus ranges (`1-3`) are parsed so they never produce
false gaps. Toggle: **Settings → System → Flag Series Gaps** (default on).

Verified render (staged folder with gaps at 3-4 and 6-7):

```
1 Storm Front - Jim Butcher            .epub
2 Fool Moon - Jim Butcher              .epub
5 Death Masks - Jim Butc...   [3-4]    .epub
8 Proven Guilty - Jim Butc...  [6-7]   .epub
```

### 2. `start`-only guide reference fix
The upstream condition at `ContentOpfParser.cpp` only honored a guide
`type="start"` reference *if a `type="text"` reference had already been seen*
— inverted logic. Many EPUB2/Project Gutenberg files ship `start` only, so they
opened on the title/license page instead of chapter 1. CrossInked stores `start`
as a fallback and resolves `text ?? start`. Bumps `BOOK_CACHE_VERSION` to 9.

### 3. Remember last-browsed folder
"Browse Files" from Home reopens the last folder you were in (if it still
exists) instead of the SD root — a real time-saver for 4-5-level-deep trees.
Persisted as `lastBrowsePath` in `state.json`.

### 9. Testing & tooling
- `test/natural_sort/NaturalSortTest.cpp`: 11 gtest cases covering
  `naturalCompare`, `parseSeriesNumber`, and gap detection (omnibus/novella
  edge cases). Build with CMake, run the `NaturalSortTest` binary.
- Simulator screenshot capture: set `CROSSINK_SIM_SHOT_DIR=/screenshots` and the
  smoke test dumps each rendered screen to `fs_/screenshots/*.bmp` for visual
  inspection.

## Building & testing

```sh
# Simulator (host, needs SDL2 + PlatformIO)
pio run -e simulator
./scripts/run_simulator_smoke_test.py --book "path/to/book.epub"

# Unit tests (needs cmake)
cd test && cmake -S . -B build && cmake --build build && ./build/natural_sort/NaturalSortTest
```

Device builds are unchanged from upstream (`pio run -e tiny`, etc.).
