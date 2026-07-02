#include "PrewarmCoversActivity.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include "MappedInputManager.h"
#include "activities/boot_sleep/SleepCoverAssets.h"
#include "components/UITheme.h"
#include "components/themes/lyra/LyraCarouselTheme.h"
#include "activities/home/RecentBooksGridActivity.h"
#include "fontIds.h"

namespace {
constexpr char BOOKS_ROOT[] = "/books";

// (CrossInked, F16) FAT long filenames hold up to 255 UTF-16 units (~765 UTF-8
// bytes). Match the browser's generous buffer (FileIndex::MAX_NAME is 511) so long
// CJK/Cyrillic paths are not truncated -- a truncated name loses its ".epub" and the
// book is silently skipped, or a truncated dir path skips a whole subtree.
constexpr int NAME_BUFFER_SIZE = 512;

// (CrossInked, F8) Bound the per-directory batch pulled into RAM at once. pendingFiles
// stores full path strings; under -fno-exceptions a failed std::vector growth on a huge
// flat folder aborts (reboots) the device. Worst case held in RAM: DIR_BATCH_MAX paths
// of up to NAME_BUFFER_SIZE-1 bytes each, plus the parent dir prefix. Bounding at 256
// entries caps this at ~256 * (24 B std::string + ~256 B heap) ~= 72 KB even for very
// long paths, well under the ESP32-C3's ~150-250 KB free heap, with headroom for the
// vector-doubling transient (old+new arrays live simultaneously). Remaining entries are
// picked up by re-opening the dir with a skip cursor (F8).
constexpr int DIR_BATCH_MAX = 256;

// (CrossInked, F9) e-ink hygiene. Clear accumulated ghosting with a HALF_REFRESH every
// N progress paints (the reader uses the same idiom for its periodic full refresh), and
// throttle paints by time so a cached re-run -- where each book is a ~50-200 ms cache
// hit -- does not FAST_REFRESH the panel continuously.
constexpr int PAINTS_PER_FULL_REFRESH = 24;
constexpr unsigned long MIN_PAINT_INTERVAL_MS = 2500;
}  // namespace

void PrewarmCoversActivity::onEnter() {
  Activity::onEnter();
  state = WARNING;
  requestUpdate();
}

void PrewarmCoversActivity::onExit() { Activity::onExit(); }

// Poll the cancel (Back) button between pipeline stages. Because each stage runs in its
// own loop() pass, the edge state has been refreshed by the main loop's gpio.update()
// since the previous stage, so cancel is honoured with one-stage latency (F15).
bool PrewarmCoversActivity::checkCancel() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    cancelled = true;
    {
      RenderLock lock(*this);
      state = DONE;
    }
    requestUpdate();
    return true;
  }
  return false;
}

void PrewarmCoversActivity::finishCurrentBook() {
  if (bookAnyOk) {
    ok++;
  } else {
    failed++;
  }
  processed++;
  currentEpub.reset();
  currentPath.clear();
  bookStage = BookStage::NONE;
  bookAnyOk = false;
}

void PrewarmCoversActivity::stepWork() {
  // (CrossInked, F15) Per-book pipeline: advance exactly ONE stage per call. A single
  // JPEG decode is atomic and is the irreducible cancel-latency floor -- everything
  // coarser than that is now interruptible between passes.
  if (bookStage != BookStage::NONE) {
    switch (bookStage) {
      case BookStage::LOAD:
        currentEpub = std::make_unique<Epub>(currentPath, "/.crosspoint");
        // Do NOT pre-create the cache dir: load() creates it on the build path, and
        // pre-creating it defeats orphan-cache adoption on device (F1).
        if (currentEpub->load(true, true)) {  // build metadata if missing; skip CSS
          bookStage = BookStage::GRID_THUMB;
        } else {
          currentEpub.reset();
          finishCurrentBook();  // load failed -> counts as a failure, no artifacts
        }
        return;

      case BookStage::GRID_THUMB:
        // (CrossInked, F4) Recent-books grid cover (RecentBooksGridActivity 123x180).
        bookAnyOk = currentEpub->generateThumbBmp(RecentBooksGridActivity::COVER_WIDTH,
                                                  RecentBooksGridActivity::COVER_HEIGHT) ||
                    bookAnyOk;
        bookStage = BookStage::HOME_THUMBS;
        return;

      case BookStage::HOME_THUMBS:
        // (CrossInked, F4) Lyra carousel centre + side covers (296x468, 200x390) -- the
        // sizes HomeActivity::loadRecentCovers actually requests.
        bookAnyOk = currentEpub->generateThumbBmp(LyraCarouselTheme::kCenterThumbW,
                                                  LyraCarouselTheme::kCenterThumbH) ||
                    bookAnyOk;
        bookAnyOk = currentEpub->generateThumbBmp(LyraCarouselTheme::kSideCoverW,
                                                  LyraCarouselTheme::kSideCoverH) ||
                    bookAnyOk;
        bookStage = BookStage::SLEEP_COVER;
        return;

      case BookStage::SLEEP_COVER:
        // (CrossInked, F4) Sleep-screen variant: reuse the exact preparation path the
        // sleep screen consumes so the crop/fit cover (or minimal thumb) matches
        // SETTINGS.sleepScreen(CoverMode). prepareEpub no-ops cheaply when the current
        // sleep mode needs neither.
        bookAnyOk = SleepCoverAssets::prepareEpub(*currentEpub) || bookAnyOk;
        finishCurrentBook();
        return;

      case BookStage::NONE:
        break;  // unreachable
    }
  }

  // Pull the next pending book path and start its pipeline.
  if (!pendingFiles.empty()) {
    currentPath = pendingFiles.back();
    pendingFiles.pop_back();
    bookAnyOk = false;
    bookStage = BookStage::LOAD;
    return;
  }

  if (!dirStack.empty()) {
    const DirEntry entry = dirStack.back();
    dirStack.pop_back();
    auto d = Storage.open(entry.path.c_str());
    if (d && d.isDirectory()) {
      char name[NAME_BUFFER_SIZE];
      int seenEpubs = 0;   // epub files encountered in this dir (for the skip cursor)
      int batched = 0;     // epub files queued this pass
      for (auto f = d.openNextFile(); f; f = d.openNextFile()) {
        name[0] = '\0';
        // (CrossInked, F16) Guard against getName failure / truncation of long names.
        if (!f.getName(name, sizeof(name)) || name[0] == '\0') {
          f.close();
          continue;
        }
        // (CrossInked, F7) Skip macOS/hidden junk: AppleDouble "._Foo.epub" resource
        // forks (which end in .epub and would otherwise be "loaded", failing and
        // leaking a permanent junk cache dir + a scary failure count) and dot-dirs
        // (.crosspoint/.Trashes/...). Matches the browser's hidden-file filtering.
        if (name[0] == '.') {
          f.close();
          continue;
        }
        std::string child = entry.path;
        if (child.empty() || child.back() != '/') child += '/';
        child += name;
        if (f.isDirectory()) {
          dirStack.push_back(DirEntry{child, 0});
        } else if (FsHelpers::hasEpubExtension(std::string_view{child})) {
          // (CrossInked, F8) Skip epub entries already queued by a previous pass over
          // this same directory, then cap the batch and requeue the remainder.
          if (seenEpubs++ < entry.skip) {
            f.close();
            continue;
          }
          if (batched >= DIR_BATCH_MAX) {
            // Requeue this directory to resume after the ones we just queued; skip
            // everything queued so far (previous skip + this batch).
            dirStack.push_back(DirEntry{entry.path, entry.skip + batched});
            LOG_INF("PRW", "prewarm: dir batch capped at %d, requeueing %s (skip=%d)",
                    DIR_BATCH_MAX, entry.path.c_str(), entry.skip + batched);
            f.close();
            break;
          }
          pendingFiles.push_back(child);
          batched++;
        }
        f.close();
      }
    }
    if (d) d.close();
    return;
  }

  state = DONE;
  requestUpdate();
}

// (CrossInked, F9) Paint the progress counter, throttled by time and periodically
// promoted to a HALF_REFRESH to clear ghosting.
void PrewarmCoversActivity::maybePaintProgress() {
  const unsigned long now = millis();
  // Only repaint when the displayed number actually changed AND enough time elapsed --
  // this stops cached-skip runs from repainting on every fast iteration.
  if (processed == lastPaintedCount) return;
  if (lastPaintMs != 0 && now - lastPaintMs < MIN_PAINT_INTERVAL_MS) return;
  lastPaintedCount = processed;
  lastPaintMs = now;
  paintsSinceFullRefresh++;
  requestUpdate();
}

void PrewarmCoversActivity::loop() {
  if (state == WARNING) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      dirStack.clear();
      pendingFiles.clear();
      dirStack.push_back(DirEntry{BOOKS_ROOT, 0});
      currentEpub.reset();
      currentPath.clear();
      bookStage = BookStage::NONE;
      bookAnyOk = false;
      processed = ok = failed = 0;
      cancelled = false;
      lastPaintedCount = -1;
      paintsSinceFullRefresh = 0;
      lastPaintMs = 0;
      {
        RenderLock lock(*this);
        state = WORKING;
      }
      requestUpdateAndWait();
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      goBack();
    }
    return;
  }

  if (state == WORKING) {
    // (CrossInked, F15) Poll cancel between stages -- honoured with one-stage latency.
    if (checkCancel()) return;
    stepWork();
    if (state == WORKING) {
      maybePaintProgress();
    }
    return;
  }

  if (state == DONE) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
        mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      goBack();
    }
    return;
  }
}

void PrewarmCoversActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_PREWARM_COVERS));

  if (state == WARNING) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 30, tr(STR_PREWARM_WARNING_1), true);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_PREWARM_WARNING_2), true);
    const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), tr(STR_CONFIRM), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == WORKING) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 20, tr(STR_PREWARMING), true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 10, std::to_string(processed).c_str());
    const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    // (CrossInked, F9) Promote to a ghost-clearing HALF_REFRESH every N paints;
    // otherwise a cheap FAST_REFRESH partial update.
    if (paintsSinceFullRefresh >= PAINTS_PER_FULL_REFRESH) {
      paintsSinceFullRefresh = 0;
      renderer.displayBuffer(HalDisplay::RefreshMode::HALF_REFRESH);
    } else {
      renderer.displayBuffer(HalDisplay::RefreshMode::FAST_REFRESH);
    }
    return;
  }

  // DONE
  renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 20, tr(STR_PREWARM_DONE), true, EpdFontFamily::BOLD);
  std::string resultText = std::to_string(ok);
  if (failed > 0) {
    resultText += " / " + std::to_string(failed) + " " + std::string(tr(STR_FAILED_LOWER));
  }
  renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 10, resultText.c_str());
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  // (CrossInked, F9) Clear accumulated ghosting on the final screen with a HALF_REFRESH.
  renderer.displayBuffer(HalDisplay::RefreshMode::HALF_REFRESH);
}
