---
title: Simulator
nav_order: 15
---

# Development Device Simulator

CrossInk can run in the [CrossPoint simulator](https://github.com/uxjulia/crosspoint-simulator), which renders the e-ink display in an SDL2 window. Use it for quick sanity checks without flashing firmware every time.

## Platform Support

The simulator is currently configured for macOS on Apple Silicon.

The `platformio.ini` `[env:simulator]` section contains hardcoded `-arch arm64` and Homebrew paths under `/opt/homebrew`.

- Intel Mac users need to remove `-arch arm64` and change Homebrew paths to `/usr/local`.
- Linux requires similar path changes plus a replacement for `lib/simulator_mock/src/MD5Builder.h`, which uses the macOS-only `CommonCrypto` API.
- Native Windows is not supported. Use WSL and follow the Linux adjustments.

## Prerequisites

```sh
# macOS
brew install sdl2

# Linux (Debian/Ubuntu)
sudo apt install libsdl2-dev
```

## Setup

Place EPUB books in `./fs_/books/` relative to the project root. That maps to the SD-card `/books/` path on device.

## Build And Run

```sh
pio run -e simulator
.pio/build/simulator/program
```

## Keyboard Controls

| Key | Action |
| --- | --- |
| Up / Down | Page back / forward (side buttons) |
| Left / Right | Left / right front buttons |
| Return | Confirm / Select |
| Escape | Back |
| P | Power |

## Cache Note

On first open of an EPUB, an **Indexing...** popup appears while the section cache is built in `.crosspoint/`.

If rendering looks stale after a code change, delete `./fs_/.crosspoint/` to clear simulator caches.

## Screenshot Capture (CrossInked)

CrossInked's smoke test can dump each rendered screen to a BMP for visual verification
(e.g. checking the file browser's series-gap markers, cover rendering, or a new screen).
Set `CROSSINK_SIM_SHOT_DIR` to an SD-relative path and run the smoke build directly:

```sh
CROSSINK_SIMULATOR_SMOKE_TEST=1 \
CROSSINK_SIMULATOR_SMOKE_BOOK="/books/Some Book.epub" \
CROSSINK_SIM_SHOT_DIR=/screenshots \
SDL_VIDEODRIVER=dummy \
.pio/build/simulator/program
```

Each smoke step (`Home`, `File Browser`, `Reader`, …) is written to
`fs_/screenshots/<Step>.bmp`. Implemented in `src/simulator/SimulatorSmokeTest.cpp`
via the existing `ScreenshotUtil::saveFramebufferAsBmp`. Convert to PNG with any tool
(e.g. Pillow) for viewing. `run_simulator_smoke_test.py` uses a throwaway `fs_`, so to
keep the screenshots run the program directly against a staged `./fs_/books/`.

## Build Note (CrossInked)

CrossInked carries two `#ifdef SIMULATOR` guards required to build `env:simulator`
against the pinned simulator mock, whose API had drifted from the fork's `src/`:
`WifiSelectionActivity.cpp` (3-arg `WiFi.disconnect` overload absent from the mock) and
`main.cpp` (`Storage.installDateTimeCallback` absent from the mock `HalStorage`). Both
only affect the simulator build; device behavior is unchanged. If a future simulator-lib
bump reintroduces these symbols, the guards can be removed.
