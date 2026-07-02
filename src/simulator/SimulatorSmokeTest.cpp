#ifdef SIMULATOR

#include "SimulatorSmokeTest.h"

#include <Epub.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <memory>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "activities/ActivityManager.h"
#include "activities/reader/EpubReaderMenuActivity.h"
#include "activities/reader/ReaderOptionsActivity.h"
#include "components/UITheme.h"
#include "util/ScreenshotUtil.h"

extern ActivityManager activityManager;
extern GfxRenderer renderer;
extern MappedInputManager mappedInputManager;

namespace {

enum class SmokeStep : uint8_t {
  Start,
  Adoption,
  Home,
  FileBrowser,
  RecentBooks,
  Settings,
  ReaderOptions,
  ReaderMenu,
  Sleep,
  Reader,
  ReaderInput,
  Done,
};

class SimulatorSmokeTest {
 public:
  void tick() {
    if (!enabled()) return;

    try {
      tickImpl();
    } catch (const std::exception& e) {
      fail("Unhandled exception: %s", e.what());
    } catch (...) {
      fail("Unhandled non-standard exception");
    }
  }

 private:
  enum class ScriptActionType : uint8_t { Press, Release, Render };

  struct ScriptAction {
    ScriptActionType type;
    MappedInputManager::Button button;
    const char* label;
    int settleFrames;
  };

  SmokeStep step = SmokeStep::Start;
  int settleFrames = 0;
  const char* activeStepName = nullptr;
  std::vector<ScriptAction> inputScript;
  size_t scriptIndex = 0;

  static bool enabled() { return std::getenv("CROSSINK_SIMULATOR_SMOKE_TEST") != nullptr; }

  static int pageTurnCount() {
    const char* raw = std::getenv("CROSSINK_SIMULATOR_SMOKE_PAGE_TURNS");
    if (raw == nullptr || raw[0] == '\0') {
      return 2;
    }
    return std::max(0, std::atoi(raw));
  }

  static void applyRequestedTheme() {
    const char* raw = std::getenv("CROSSINK_SIMULATOR_SMOKE_THEME");
    if (raw == nullptr || raw[0] == '\0') {
      return;
    }

    const int theme = std::atoi(raw);
    if (theme < 0 || theme >= CrossPointSettings::UI_THEME_COUNT) {
      fail("Invalid smoke test theme index: %d", theme);
    }

    SETTINGS.uiTheme = static_cast<uint8_t>(theme);
    UITheme::getInstance().reload();
    LOG_INF("SMOKE", "Using theme index %d", theme);
  }

  [[noreturn]] static void fail(const char* message) {
    LOG_ERR("SMOKE", "%s", message);
    std::_Exit(2);
  }

  template <typename... Args>
  [[noreturn]] static void fail(const char* format, Args... args) {
    logPrintf("ERR", "SMOKE", format, args...);
    logPrintf("ERR", "SMOKE", "\n");
    std::_Exit(2);
  }

  // Dump the current framebuffer to <CROSSINK_SIM_SHOT_DIR>/<step>.bmp when that env var
  // is set, so tooling/CI can capture the actual rendered screens (file browser with
  // series-gap markers, covers, etc.) for visual verification. (CrossInked)
  static void maybeDumpScreenshot(const char* name) {
    const char* dir = std::getenv("CROSSINK_SIM_SHOT_DIR");
    if (dir == nullptr || dir[0] == '\0') return;
    char safe[64];
    size_t j = 0;
    for (size_t i = 0; name[i] != '\0' && j + 1 < sizeof(safe); ++i) {
      const char c = name[i];
      safe[j++] = (c == ' ' || c == '/') ? '_' : c;
    }
    safe[j] = '\0';
    Storage.mkdir(dir);
    char path[192];
    snprintf(path, sizeof(path), "%s/%s.bmp", dir, safe);
    if (ScreenshotUtil::saveFramebufferAsBmp(path, renderer.getFrameBuffer(), renderer.getDisplayWidth(),
                                             renderer.getDisplayHeight())) {
      LOG_INF("SMOKE", "Screenshot saved: %s", path);
    }
  }

  static void renderCurrentStep(const char* name) {
    LOG_INF("SMOKE", "Rendering %s", name);
    if (activityManager.requestUpdateAndWait() != RequestUpdateResult::Rendered) {
      fail("Render was rejected for %s", name);
    }
    maybeDumpScreenshot(name);
  }

  void queueStep(const char* name, SmokeStep nextStep, int framesToSettle = 3) {
    activeStepName = name;
    settleFrames = framesToSettle;
    step = nextStep;
  }

  void tickImpl() {
    mappedInputManager.simulatorClearInputFrame();

    if (settleFrames > 0) {
      --settleFrames;
      if (settleFrames == 0 && activeStepName != nullptr) {
        renderCurrentStep(activeStepName);
        activeStepName = nullptr;
      }
      return;
    }

    switch (step) {
      case SmokeStep::Start:
        LOG_INF("SMOKE", "Starting simulator smoke test");
        if (!CrossPointSettings::verifySleepTimeoutMigrationContract()) {
          fail("Sleep timeout migration contract failed");
        }
        if (!CrossPointSettings::verifySleepScreenMigrationContract()) {
          fail("Sleep screen migration contract failed");
        }
        applyRequestedTheme();
        step = SmokeStep::Adoption;
        break;

      case SmokeStep::Adoption:
        runAdoptionScenario();
        activityManager.goHome();
        queueStep("Home", SmokeStep::Home);
        break;

      case SmokeStep::Home:
        activityManager.goToFileBrowser("/books");
        queueStep("File Browser", SmokeStep::FileBrowser);
        break;

      case SmokeStep::FileBrowser:
        activityManager.goToRecentBooks();
        queueStep("Recent Books", SmokeStep::RecentBooks);
        break;

      case SmokeStep::RecentBooks:
        activityManager.goToSettings();
        queueStep("Settings", SmokeStep::Settings);
        break;

      case SmokeStep::Settings:
        activityManager.replaceActivity(std::make_unique<ReaderOptionsActivity>(renderer, mappedInputManager));
        queueStep("Reader Options", SmokeStep::ReaderOptions);
        break;

      case SmokeStep::ReaderOptions:
        activityManager.replaceActivity(
            std::make_unique<EpubReaderMenuActivity>(renderer, mappedInputManager, "Smoke Test", 1, 1, 0,
                                                     SETTINGS.orientation, false, false, false, false, false));
        queueStep("Reader Menu", SmokeStep::ReaderMenu);
        break;

      case SmokeStep::ReaderMenu:
        activityManager.goToSleep();
        queueStep("Sleep", SmokeStep::Sleep);
        break;

      case SmokeStep::Sleep: {
        const char* bookPath = std::getenv("CROSSINK_SIMULATOR_SMOKE_BOOK");
        if (bookPath == nullptr || bookPath[0] == '\0') {
          LOG_INF("SMOKE", "Skipping Reader step; CROSSINK_SIMULATOR_SMOKE_BOOK is not set");
          step = SmokeStep::Reader;
          break;
        }
        if (!Storage.exists(bookPath)) {
          fail("Smoke test book is missing: %s", bookPath);
        }
        activityManager.goToReader(bookPath, true);
        queueStep("Reader", SmokeStep::Reader, 8);
        break;
      }

      case SmokeStep::Reader:
        buildReaderInputScript();
        step = SmokeStep::ReaderInput;
        break;

      case SmokeStep::ReaderInput:
        runReaderInputScript();
        break;

      case SmokeStep::Done:
        LOG_INF("SMOKE", "Simulator smoke test passed");
        std::_Exit(0);
    }
  }

  // Cache-adoption regression scenario (CrossInked, F1). The old sim test never
  // pre-created the destination cache dir before reopening a renamed book, so it
  // masked the SdFat-vs-POSIX rename divergence: POSIX rename replaces an empty
  // destination dir, SdFat's does not. This scenario pre-creates that dir and
  // asserts adoption still succeeds and preserves the stranded progress.bin. It
  // is skipped when no smoke book is provided.
  static void runAdoptionScenario() {
    const char* bookPath = std::getenv("CROSSINK_SIMULATOR_SMOKE_BOOK");
    if (bookPath == nullptr || bookPath[0] == '\0') {
      LOG_INF("SMOKE", "Skipping adoption scenario; CROSSINK_SIMULATOR_SMOKE_BOOK is not set");
      return;
    }
    if (!Storage.exists(bookPath)) {
      fail("Adoption scenario book is missing: %s", bookPath);
    }

    constexpr char kCacheRoot[] = "/.crosspoint";
    const std::string original(bookPath);
    const size_t dot = original.rfind('.');
    const std::string renamed =
        (dot == std::string::npos ? original : original.substr(0, dot)) + "_adopt_renamed.epub";

    // Start from a clean slate so reruns are deterministic.
    if (Storage.exists(renamed.c_str())) Storage.remove(renamed.c_str());
    Storage.removeDir(Epub::cachePathForFilePath(renamed, kCacheRoot).c_str());

    // 1) Copy the book to a "renamed" path (simulating a manual SD rename where
    //    the destination is a new file; we delete the original below).
    {
      String bytes = Storage.readFile(original.c_str());
      if (bytes.length() == 0) {
        fail("Adoption scenario could not read book bytes: %s", original.c_str());
      }
      if (!Storage.writeFile(renamed.c_str(), bytes)) {
        fail("Adoption scenario could not write renamed book: %s", renamed.c_str());
      }
    }

    // 2) Build the ORIGINAL book's cache and drop a sentinel progress.bin that
    //    must survive the adoption.
    std::string originalCacheDir;
    {
      Epub epub(original, kCacheRoot);
      if (!epub.load(true, true)) {
        fail("Adoption scenario could not build original cache: %s", original.c_str());
      }
      originalCacheDir = epub.getCachePath();
    }
    if (!Epub::cachePathForFilePath(original, kCacheRoot).empty() &&
        !Storage.exists((originalCacheDir + "/book.bin").c_str())) {
      fail("Adoption scenario: original cache has no book.bin");
    }
    const std::string sentinel = originalCacheDir + "/progress.bin";
    if (!Storage.writeFile(sentinel.c_str(), String("SENTINEL"))) {
      fail("Adoption scenario could not write sentinel progress.bin");
    }

    // 3) Delete the original file so its recorded source path is "gone".
    if (!Storage.remove(original.c_str())) {
      fail("Adoption scenario could not remove original book: %s", original.c_str());
    }

    // 4) THE case the old test missed: pre-create the destination (renamed) cache
    //    dir as an empty directory before load(), exactly like prewarm /
    //    setupCacheDir / Clear-Reading-Cache do on device.
    const std::string renamedCacheDir = Epub::cachePathForFilePath(renamed, kCacheRoot);
    Storage.mkdir(renamedCacheDir.c_str());
    if (!Storage.exists(renamedCacheDir.c_str())) {
      fail("Adoption scenario could not pre-create destination cache dir: %s", renamedCacheDir.c_str());
    }

    // 5) Reset the session orphan index so the next miss rescans /.crosspoint.
    //    On device the orphan pre-exists at boot (created by an out-of-band SD
    //    rename), so the first scan sees it; here the orphan was created after
    //    this session's first scan (which built the original cache), so we must
    //    invalidate the "already scanned, empty" state to mirror reality.
    Epub::invalidateOrphanIndex();

    // Open the renamed book -> adoption must rmdir the pre-created dir, rename
    // the orphan into place, and preserve the sentinel.
    {
      Epub epub(renamed, kCacheRoot);
      if (!epub.load(true, true)) {
        fail("Adoption scenario: renamed book failed to load");
      }
    }
    if (!Storage.exists((renamedCacheDir + "/book.bin").c_str())) {
      fail("Adoption scenario: renamed cache has no book.bin after adoption");
    }
    const std::string adoptedSentinel = renamedCacheDir + "/progress.bin";
    if (!Storage.exists(adoptedSentinel.c_str())) {
      fail("Adoption scenario: progress.bin was NOT adopted (stranded reading progress)");
    }
    if (Storage.readFile(adoptedSentinel.c_str()) != String("SENTINEL")) {
      fail("Adoption scenario: adopted progress.bin content mismatch");
    }
    if (Storage.exists((originalCacheDir + "/book.bin").c_str())) {
      fail("Adoption scenario: orphan cache dir was left behind (rename did not move it)");
    }

    // Cleanup: restore the original book (later smoke steps open the smoke book
    // by its original path) and remove the renamed copy + its cache so the
    // isolated fs_ is tidy.
    {
      String bytes = Storage.readFile(renamed.c_str());
      if (bytes.length() == 0 || !Storage.writeFile(original.c_str(), bytes)) {
        fail("Adoption scenario could not restore original book: %s", original.c_str());
      }
    }
    Storage.remove(renamed.c_str());
    Storage.removeDir(renamedCacheDir.c_str());
    // The renamed cache was adopted (moved) from originalCacheDir, so drop it too
    // -- the original book rebuilds a fresh cache when a later step opens it.
    Storage.removeDir(originalCacheDir.c_str());
    // The next real cache miss should rescan now that we've mutated /.crosspoint.
    Epub::invalidateOrphanIndex();
    LOG_INF("SMOKE", "Adoption scenario passed (pre-created dest dir + sentinel preserved)");
  }

  static ScriptAction press(MappedInputManager::Button button) { return {ScriptActionType::Press, button, nullptr, 0}; }

  static ScriptAction release(MappedInputManager::Button button) {
    return {ScriptActionType::Release, button, nullptr, 0};
  }

  static ScriptAction render(const char* label, int framesToSettle = 3) {
    return {ScriptActionType::Render, MappedInputManager::Button::Back, label, framesToSettle};
  }

  void addTap(MappedInputManager::Button button) {
    inputScript.push_back(press(button));
    inputScript.push_back(release(button));
  }

  void buildReaderInputScript() {
    inputScript.clear();
    scriptIndex = 0;

    const int turns = pageTurnCount();
    for (int i = 0; i < turns; i++) {
      addTap(MappedInputManager::Button::PageForward);
      inputScript.push_back(render("Reader after page forward", 4));
    }

    addTap(MappedInputManager::Button::Confirm);
    inputScript.push_back(render("Reader Menu opened from EPUB", 4));

    addTap(MappedInputManager::Button::Down);
    inputScript.push_back(render("Reader Menu Reader Options selection", 3));

    addTap(MappedInputManager::Button::Confirm);
    inputScript.push_back(render("Reader Options opened from Reader Menu", 4));

    addTap(MappedInputManager::Button::Down);
    inputScript.push_back(render("Reader Options after navigation", 3));

    addTap(MappedInputManager::Button::Confirm);
    inputScript.push_back(render("Reader Options after toggle", 3));

    addTap(MappedInputManager::Button::Back);
    inputScript.push_back(render("Reader Menu after closing Reader Options", 4));

    addTap(MappedInputManager::Button::Back);
    inputScript.push_back(render("Reader after closing Reader Menu", 4));

    LOG_INF("SMOKE", "Running reader input script with %d page turn(s)", turns);
  }

  void runReaderInputScript() {
    if (scriptIndex >= inputScript.size()) {
      step = SmokeStep::Done;
      return;
    }

    const auto& action = inputScript[scriptIndex++];
    switch (action.type) {
      case ScriptActionType::Press:
        mappedInputManager.simulatorInjectPress(action.button);
        break;
      case ScriptActionType::Release:
        mappedInputManager.simulatorInjectRelease(action.button);
        break;
      case ScriptActionType::Render:
        queueStep(action.label, SmokeStep::ReaderInput, action.settleFrames);
        break;
    }
  }
};

SimulatorSmokeTest smokeTest;

}  // namespace

void runSimulatorSmokeTestTick() { smokeTest.tick(); }

#endif
