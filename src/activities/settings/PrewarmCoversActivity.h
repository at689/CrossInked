#pragma once

#include <string>
#include <vector>

#include "activities/Activity.h"

// Settings maintenance action (CrossInked): walk /books and pre-decode every book's
// cover/thumbnail into the cache so first browse of the recent grid / carousel isn't
// stuttery. RAM-safe: an explicit directory stack + one directory's file list are held
// at a time, never all book paths (important on the ESP32-C3's small SRAM). Incremental
// and cancellable so it never blocks the UI or trips the watchdog.
class PrewarmCoversActivity final : public Activity {
 public:
  explicit PrewarmCoversActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("PrewarmCovers", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  bool skipLoopDelay() override { return true; }  // keep processing without power-saving delay
  void render(RenderLock&&) override;

 private:
  enum State { WARNING, WORKING, DONE };

  State state = WARNING;
  std::vector<std::string> dirStack;
  std::vector<std::string> pendingFiles;
  int processed = 0;
  int ok = 0;
  int failed = 0;
  bool cancelled = false;

  void goBack() { finish(); }
  void stepWork();
};
