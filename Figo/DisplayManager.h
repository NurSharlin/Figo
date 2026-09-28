#ifndef DISPLAY_MANAGER_H
#define DISPLAY_MANAGER_H

#include <lvgl.h>
#include "DisplayDriver.h"
#include "TouchDriver.h"
#include "ScreenManager.h"
#include "BootScreens.h"

// Owns everything Task 1 (uiTask) touches: LVGL init, the partial draw
// buffer, the display/touch driver glue, and the screen carousel. Never
// call into NetworkManager/DataFetcher/ConfigManager from here - only read
// SharedState under stateMutex (see Figo.ino's uiTask loop).
class DisplayManager {
 public:
  void begin();
  void loop();  // pumps lv_timer_handler() - call every uiTask iteration

 private:
  static void flushCb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p);
  static void touchReadCb(lv_indev_drv_t *drv, lv_indev_data_t *data);
  // The metric-driven face. Maps the shared MoodBand (SharedState.h's
  // moodFor(), the one place the rule is written) onto an Expression and
  // pushes it to FaceRenderer, but only on an actual change - setExpression()
  // restarts animations, so calling it every 5ms tick would freeze the face
  // mid-transition forever.
  // force skips the "band actually moved" short-circuit. Needed on the
  // wake edge only - see the call site.
  void applyMood(const RuntimeConfig &config, const MarketSnapshot &market, bool force = false);
  // Pushes system config onto the hardware: today that is the backlight
  // alone. Mirrors applyMood() in shape - it no-ops unless the value actually
  // changed, so calling it on every sleep tick costs a comparison.
  //
  // Sleep is just one more input here rather than a special case: a control
  // path from config.system to the panel did not exist before this (see the
  // single setBacklight(255) at DisplayDriver.cpp:29), and the night
  // scheduler needs one.
  void applySystem(const SystemConfig &sys);
  // Builds the face/data screen carousel - factored out of begin() since it
  // also needs to run later, from loop(), once g_networkMode reaches STA
  // (see BootScreens.h).
  void startCarousel(const RuntimeConfig &config);

  DisplayDriver displayDriver_;
  TouchDriver touchDriver_;
  ScreenManager screenManager_;
  BootScreens bootScreens_;
  // True once startCarousel() has run. Until then, loop() polls
  // g_networkMode to drive BootScreens instead of the config-gen carousel
  // rebuild path.
  bool carouselActive_ = false;
  // The boot-time "settings problem" screen is up (g_configLoadErrors != 0) and
  // loop() holds everything else back until millis() reaches
  // configErrorUntilMs_. Set once in begin(), cleared once in loop(); uiTask only.
  bool configErrorUp_ = false;
  uint32_t configErrorUntilMs_ = 0;

  lv_disp_draw_buf_t drawBuf_;
  lv_color_t *bufPixels_ = nullptr;
  lv_disp_drv_t dispDrv_;
  lv_indev_drv_t indevDrv_;

  // Last g_configGen (SharedState.h) this task has acted on. loop() only
  // advances this once ScreenManager::rebuildFromConfig() actually applies
  // the change (it can defer mid-swipe-animation), so a deferred update is
  // retried rather than silently dropped.
  uint32_t lastSeenConfigGen_ = 0;
  // Same idea for DataFetcher's publishes (SharedState.h's g_marketGen):
  // only copy the snapshot and rewrite labels when a fetch actually landed,
  // rather than every one of uiTask's 5ms ticks.
  uint32_t lastSeenMarketGen_ = 0;
  // Last band pushed to FaceRenderer. Starts at Unknown, which maps to the
  // same Expression::Neutral the face is built with, so a device whose first
  // computed mood really is Unknown does not push a redundant setExpression().
  MoodBand lastMood_ = MoodBand::Unknown;
  // True while the night window is in effect and not overridden by a touch.
  // Task-1-local by design: nothing outside uiTask reads it (the phone gets
  // the WINDOW from /api/screens/state, computed independently), so it needs
  // no mutex.
  bool sleepActive_ = false;
  // Last percentage written to the backlight. 255 is unreachable as a
  // percentage, so the first real call always writes.
  uint8_t lastBacklightPct_ = 255;
  uint32_t lastSleepCheckMs_ = 0;
  // The carousel index the user was on when sleep began, restored on wake.
  uint8_t preSleepIndex_ = 0;
  // Set by touchReadCb on any press, consumed and cleared by loop()'s sleep
  // tick. Both run on uiTask (loop() calls lv_timer_handler(), which is what
  // polls the indev), so this needs no mutex - unlike the g_softApRequested
  // tap latch, which crosses into Task 2.
  bool touchPressed_ = false;
  // A touch during sleep cancels it for the rest of the window. Cleared the
  // moment the window itself ends.
  bool sleepOverride_ = false;
  // True while the OTA update screen owns the panel. Task-1-local like the
  // sleep latches above - only the resolved g_ota* answer crosses tasks. It
  // forces full brightness (see applySystem) without touching sleepActive_, so
  // the night scheduler's own bookkeeping is undisturbed and the panel dims
  // again by itself if the update fails inside the window.
  bool otaShown_ = false;
};

#endif // DISPLAY_MANAGER_H
