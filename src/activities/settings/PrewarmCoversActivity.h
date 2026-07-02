#pragma once

#include <Epub.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "activities/Activity.h"

// Settings maintenance action (CrossInked): walk /books and pre-decode every book's
// cover/thumbnail into the cache so first browse of the recent grid / carousel isn't
// stuttery. RAM-safe: an explicit directory stack plus a bounded per-directory file
// batch are held at a time, never all book paths (important on the ESP32-C3's small
// SRAM). Incremental and cancellable: each book is processed in per-stage steps
// (load -> grid thumb -> carousel thumbs -> sleep cover), one stage per loop() pass,
// so cancel and the power gesture are honoured between stages rather than only
// between whole books.
class PrewarmCoversActivity final : public Activity {
 public:
  explicit PrewarmCoversActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("PrewarmCovers", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  // (CrossInked, F3) Scope both to WORKING: an unconditional true would keep the
  // device spinning at full clock and never sleeping on the DONE screen after an
  // unattended run, draining the battery. During WORKING we must both suppress the
  // inactivity auto-sleep (a 30-90 min walk presses no buttons) and skip the
  // power-saving loop delay so the walk makes progress.
  bool preventAutoSleep() override { return state == WORKING; }
  bool skipLoopDelay() override { return state == WORKING; }
  void render(RenderLock&&) override;

 private:
  enum State { WARNING, WORKING, DONE };

  // (CrossInked, F15) Per-book pipeline. One stage runs per stepWork()/loop() pass so
  // cancel latency drops from "whole book" to "one decode". A single decode is atomic
  // and remains the irreducible floor (see stepWork).
  enum class BookStage : uint8_t {
    NONE,        // no book in flight; pull the next path
    LOAD,        // build/read metadata cache
    GRID_THUMB,  // recent-books grid cover
    HOME_THUMBS, // Lyra carousel centre + side covers
    SLEEP_COVER  // sleep-screen cover/minimal thumb variant
  };

  State state = WARNING;

  // Directory walk. dirStack holds pending directories; each entry carries a skip
  // cursor so a huge flat folder is drained in bounded batches (F8) across passes.
  struct DirEntry {
    std::string path;
    int skip = 0;  // number of already-queued epub entries to skip on re-open
  };
  std::vector<DirEntry> dirStack;
  std::vector<std::string> pendingFiles;

  // In-flight book state (F15).
  BookStage bookStage = BookStage::NONE;
  std::unique_ptr<Epub> currentEpub;
  std::string currentPath;
  bool bookAnyOk = false;  // did any artifact for this book succeed?

  int processed = 0;
  int ok = 0;
  int failed = 0;
  bool cancelled = false;

  // (CrossInked, F9) Progress-paint pacing.
  int lastPaintedCount = -1;       // processed value at the last paint
  int paintsSinceFullRefresh = 0;  // paints since the last ghost-clearing HALF_REFRESH
  unsigned long lastPaintMs = 0;   // time of the last progress paint

  void goBack() { finish(); }
  void stepWork();
  void finishCurrentBook();
  bool checkCancel();  // poll Back; returns true and transitions to DONE if cancelled
  void maybePaintProgress();
};
