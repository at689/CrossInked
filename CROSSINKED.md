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
| 9 | **Test/tooling**: natural-sort + gap unit tests, simulator screenshot capture | ✅ implemented | `test/natural_sort/`, `SimulatorSmokeTest.cpp` |
| — | Simulator build fixes (mock API skew) | ✅ | `WifiSelectionActivity.cpp`, `main.cpp` |
| 4,5,6,7,8 | Jump-to-letter, content-key cache survival, primary-author, cover prewarm, orphan-cache reaper | 📋 designed — see [ROADMAP.md](ROADMAP.md) | — |

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
