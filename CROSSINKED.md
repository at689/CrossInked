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
| 5 | **Content-key cache survival** — manual SD renames stop resetting progress | ✅ implemented + hardened by the July 2026 audit (⚠ device-test) | `Epub.cpp`, `Epub.h`, `ContentKeySidecar.h` |
| 7 | **Cover prewarm** ("Rebuild Covers") maintenance action | ✅ implemented, reworked by the July 2026 audit (⚠ device-test heap/watchdog) | `PrewarmCoversActivity.*`, `SettingsList.h` |
| 9 | **Test/tooling**: natural-sort + gap unit tests, simulator screenshot capture | ✅ implemented | `test/natural_sort/`, `SimulatorSmokeTest.cpp` |
| — | Simulator build fixes (mock API skew) | ✅ | `WifiSelectionActivity.cpp`, `main.cpp` |
| 6,8 | Primary-author display, orphan-cache reaper | 📋 designed — see [ROADMAP.md](ROADMAP.md) | — |

> ⚠ **Needs on-device verification**: #5 (cache adopt/rename) and #7 (heap/watchdog under a full library) pass in the simulator, but their real failure modes only surface on hardware. Flash-test before relying on them.

## July 2026 deep audit & fix wave

A second-pass audit ([docs/audit-2026-07.md](docs/audit-2026-07.md)) confirmed 20
defects the first sweep missed, plus two repo-level landmines. All were fixed in
commits `6040a08e..fe2f6893` (2026-07-02); the report carries a per-finding
resolution table. Highlights:

- **#5 adoption worked only in the simulator** — POSIX `rename()` accepts an
  existing empty destination dir, SdFat's FAT rename does not, and several flows
  pre-created that dir. Adoption is now hardened (pre-created dirs cleared with
  stray stats preserved, rename result checked), validated by source file size
  (rejects other editions), case-insensitive for case-only renames, and backed by
  a session-scoped in-RAM orphan index instead of an O(N²) per-miss SD walk. The
  sim smoke test now includes an adoption scenario with a pre-created destination.
- **Prewarm (#7) was rewritten**: it keeps the device awake for the run, generates
  the cover sizes the UI actually reads (recents grid, Lyra carousel, sleep
  screen), skips macOS `._*` junk, bounds per-folder memory, clears e-ink ghosting
  with periodic half-refreshes, and polls cancel between per-book pipeline stages.
- **`BOOK_CACHE_VERSION` moved to `0x89`** — a fork-reserved range (`0x80|N`) so
  upstream's own future v9 can never be confused with fork caches. One-time cache
  rebuild per book on first open after flashing.
- **OTA now points at this fork** (`at689/CrossInked`), not upstream — upstream's
  next release would otherwise have silently replaced CrossInked via
  Settings → Check for Updates.
- **All JSON stores write atomically** (tmp + `.bak` + rename), `state.json` is
  no longer rewritten on every folder navigation, and `book.bin` builds
  crash-consistently with truncation detection on load.
- The **simulator now applies the JPEGDEC progressive-JPEG patches** the device
  build already had, restoring sim/device decode parity.

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
as a fallback and resolves `text ?? start` (with `#fragment` hrefs stripped since
the July 2026 audit). Bumps `BOOK_CACHE_VERSION` (now `0x89`, fork-reserved range).

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
