#ifndef SCREEN_MANAGER_H
#define SCREEN_MANAGER_H

#include <lvgl.h>
#include "FaceRenderer.h"
#include "ScreenBuilders.h"
#include "SharedState.h"

// Screen slots: index 0 is always the face screen; indices 1..screenCount_
// are data screens driven by RuntimeConfig.screens[], showing live values
// published by DataFetcher. +1 for the face screen alongside up to
// MAX_SCREENS configured data screens.
#define MAX_SCREEN_SLOTS (MAX_SCREENS + 1)

class ScreenManager {
 public:
  // Builds the face screen plus one data screen per config.screens[0..count)
  // and loads screen 0. Call once from DisplayManager::begin().
  void init(const RuntimeConfig &config);

  // Advances the carousel on its own once a screen has been on display for
  // SCREEN_AUTO_ADVANCE_MS. Call every uiTask iteration from
  // DisplayManager::loop(); cheap (one millis() compare) on the vast
  // majority of ticks. Deliberately a plain millis() check rather than an
  // lv_timer: the countdown has to be restartable from goTo() on every
  // manual swipe and survive a rebuildFromConfig() teardown, both of which
  // are awkward to express against an lv_timer's lifecycle.
  void loop();

  // Re-loads whichever carousel screen was last active, without animation.
  // Used when something else took the display over (the runtime
  // "WiFi Disconnected" screen) and is handing it back. Must be called
  // BEFORE the interloper is deleted: lv_obj_del() on the active screen
  // NULLs disp->act_scr and the next lv_scr_load() dereferences it.
  void reloadCurrent();

  // Pushes the latest fetched values onto the already-built data screens.
  // Called from DisplayManager::loop() when g_marketGen advances. Mutates
  // pre-created widgets ONLY - text, colour, and (from the gauge screens on)
  // marker position and arc value - it must never create or delete an LVGL
  // object, since this runs in the animation hot path (a hard rule). Matches slots to values on ScreenConfig.id, never array position:
  // the SPA's drag-and-drop reorder rewrites screens[] wholesale, so a
  // positional match would show one screen's price under another's name.
  void applySnapshot(const RuntimeConfig &config, const MarketSnapshot &snapshot);

  // Tears down and rebuilds the data screens (index 0's face screen
  // is untouched) when RuntimeConfig.screens[] has changed - see
  // DisplayManager::loop()'s g_configGen check. Clamps currentIndex_ if the
  // new screen count made it fall out of range. Returns false (and does
  // nothing) if a swipe transition is still animating - lv_scr_load_anim()
  // owns the outgoing/incoming screen objects during that window, so
  // deleting one out from under it would be unsafe. Callers should retry on
  // a later tick rather than drop the update; DisplayManager::loop() does
  // this by not advancing its last-seen config generation until this
  // returns true.
  //
  // NOTE: this runs on ANY config generation bump, not just a screens edit -
  // g_configGen is a single counter for all five sections (ConfigManager),
  // so saving brightness rebuilds the data screens too. That is wasteful but
  // harmless; what it is NOT allowed to be is unsafe, which is why the
  // implementation parks on the face screen before deleting anything.
  bool rebuildFromConfig(const RuntimeConfig &config);

  void next();
  void prev();
  // Public since the night scheduler parks the carousel on the face screen at
  // sleep entry and restores the user's screen on wake. Index 0 is always the
  // face screen; data screens follow. Arms the auto-advance dwell, same as a
  // manual swipe.
  void goTo(uint8_t index, bool animateLeft);
  // goTo() without the navigation: validates and records the slot the
  // carousel should be on, but loads nothing. For the one caller that must
  // change where the carousel *is* without animating over what is currently
  // displayed - the night scheduler while the disconnect screen owns the
  // screen. reloadCurrent() then lands on the right slot when Wi-Fi returns,
  // instead of on whatever was showing when the link dropped.
  void parkIndex(uint8_t index);
  uint8_t currentIndex() const { return currentIndex_; }
  FaceRenderer &faceRenderer() { return faceRenderer_; }

 private:
  static void gestureEventCb(lv_event_t *e);
  // (Re)starts the auto-advance dwell from now. Called on every navigation
  // and after a rebuild.
  void armAutoAdvance();
  void buildFaceScreen(lv_obj_t *screen);
  // slot is the screens_[] index being built, so the mutable widgets can be
  // recorded into widgets_[slot] as they're created. totalScreens (data
  // screens + face) sizes the dot indicator row.
  void buildDataScreen(lv_obj_t *screen, const ScreenConfig &cfg, uint8_t slot,
                       uint8_t totalScreens);
  // Deletes every currently-allocated data screen (indices
  // 1..screenCount_) and nulls their slots, so rebuildFromConfig() never
  // leaves a stale lv_obj_t* behind or leaks the old LVGL objects.
  void destroyDataScreens();

  lv_obj_t *screens_[MAX_SCREEN_SLOTS] = {};
  // Retained handles to each data screen's mutable widgets, so
  // applySnapshot() can update values without rebuilding anything.
  // Indexed like screens_[]: slot 0 (the face screen) holds only nulls.
  DataScreenWidgets widgets_[MAX_SCREEN_SLOTS] = {};
  // The face screen's dot indicator row. Unlike the data screens' rows (which
  // die with their screen on rebuild), the face screen itself is never
  // rebuilt - but its dot row shows the TOTAL screen count, so
  // rebuildFromConfig() deletes and re-adds this one explicitly.
  lv_obj_t *faceDots_ = nullptr;
  uint8_t screenCount_ = 0;  // data screens only, not counting the face screen
  uint8_t currentIndex_ = 0;
  FaceRenderer faceRenderer_;

  // Set by goTo() to millis()+kTransitionMs (plus slack), checked by
  // rebuildFromConfig() to avoid tearing down a screen mid
  // lv_scr_load_anim().
  uint32_t transitionEndsAtMs_ = 0;

  // millis() at which loop() should advance to the next screen. Rearmed by
  // goTo() (so a manual swipe restarts the dwell) and by init()/
  // rebuildFromConfig(). 0 means auto-advance is disarmed.
  uint32_t autoAdvanceAtMs_ = 0;
};

#endif // SCREEN_MANAGER_H
