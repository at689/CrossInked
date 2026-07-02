#include "PrewarmCoversActivity.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int RENDER_EVERY = 8;  // refresh the progress count every N books
constexpr char BOOKS_ROOT[] = "/books";
}  // namespace

void PrewarmCoversActivity::onEnter() {
  Activity::onEnter();
  state = WARNING;
  requestUpdate();
}

void PrewarmCoversActivity::onExit() { Activity::onExit(); }

void PrewarmCoversActivity::stepWork() {
  // Process one pending file, else expand one directory. Holds at most the dir stack
  // plus one directory's file list -> RAM-safe on the ESP32-C3.
  if (!pendingFiles.empty()) {
    const std::string path = pendingFiles.back();
    pendingFiles.pop_back();
    Epub epub(path, "/.crosspoint");
    // (CrossInked, F1) Do NOT pre-create the cache dir here: load() calls
    // setupCacheDir() itself on the build path, and pre-creating it defeats
    // orphan-cache adoption on device (SdFat's FAT rename fails when the
    // destination path already exists), stranding reading progress after a
    // manual SD rename -- the exact workflow this maintenance action serves.
    if (epub.load(true, true)) {  // build metadata cache if missing; skip CSS (not needed for covers)
      const bool coverOk = epub.generateCoverBmp();
      const bool thumbOk = epub.generateThumbBmp(0, 0);
      if (coverOk || thumbOk) {
        ok++;
      } else {
        failed++;
      }
    } else {
      failed++;
    }
    processed++;
    return;
  }

  if (!dirStack.empty()) {
    const std::string dir = dirStack.back();
    dirStack.pop_back();
    auto d = Storage.open(dir.c_str());
    if (d && d.isDirectory()) {
      char name[256];
      for (auto f = d.openNextFile(); f; f = d.openNextFile()) {
        f.getName(name, sizeof(name));
        std::string child = dir;
        if (child.empty() || child.back() != '/') child += '/';
        child += name;
        if (f.isDirectory()) {
          dirStack.push_back(child);
        } else if (FsHelpers::hasEpubExtension(std::string_view{child})) {
          pendingFiles.push_back(child);
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

void PrewarmCoversActivity::loop() {
  if (state == WARNING) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      dirStack.clear();
      pendingFiles.clear();
      dirStack.emplace_back(BOOKS_ROOT);
      processed = ok = failed = 0;
      cancelled = false;
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
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      cancelled = true;
      {
        RenderLock lock(*this);
        state = DONE;
      }
      requestUpdate();
      return;
    }
    const int before = processed;
    stepWork();
    if (state == WORKING && (processed / RENDER_EVERY) != (before / RENDER_EVERY)) {
      requestUpdate();
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
    renderer.displayBuffer(HalDisplay::RefreshMode::FAST_REFRESH);
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
  renderer.displayBuffer();
}
