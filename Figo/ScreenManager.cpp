#include "ScreenManager.h"

#include <Arduino.h>

#include "Config.h"
#include "FigoTheme.h"

namespace {
constexpr uint32_t kTransitionMs = 220;
// Added on top of kTransitionMs when arming the teardown guard. LVGL only
// clears its in-flight screen-load bookkeeping (lv_disp_t::prev_scr /
// scr_to_load) from the animation's ready callback inside
// lv_timer_handler(), which can land an iteration or two after the nominal
// duration has elapsed. Without this margin rebuildFromConfig() can decide
// the transition is over a few milliseconds before LVGL agrees, and delete
// a screen LVGL still holds a pointer to.
constexpr uint32_t kTransitionGuardSlackMs = 60;
}  // namespace

void ScreenManager::init(const RuntimeConfig &config) {
  screens_[0] = lv_obj_create(NULL);
  widgets_[0] = {};  // slot 0 is the face screen - no value widgets
  buildFaceScreen(screens_[0]);
  lv_obj_add_event_cb(screens_[0], gestureEventCb, LV_EVENT_GESTURE, this);

  uint8_t count = config.screen_count;
  if (count > MAX_SCREENS) count = MAX_SCREENS;
  for (uint8_t i = 0; i < count; i++) {
    screens_[i + 1] = lv_obj_create(NULL);
    buildDataScreen(screens_[i + 1], config.screens[i], i + 1, count + 1);
    lv_obj_add_event_cb(screens_[i + 1], gestureEventCb, LV_EVENT_GESTURE, this);
  }
  screenCount_ = count;
  faceDots_ = screen_builders::addDotIndicator(screens_[0], 0, count + 1);

  lv_scr_load(screens_[0]);
  currentIndex_ = 0;
  armAutoAdvance();
}

bool ScreenManager::rebuildFromConfig(const RuntimeConfig &config) {
  // transitionEndsAtMs_ == 0 means "never navigated". It has to be special
  // cased rather than compared: the signed-difference test below is only
  // meaningful while the two timestamps are within ~24 days of each other,
  // and against a zero it would start reporting "still animating" forever
  // once millis() crosses 2^31.
  if (transitionEndsAtMs_ != 0 && (int32_t)(millis() - transitionEndsAtMs_) < 0) {
    return false;  // still animating - caller retries next tick
  }

  // Park on the face screen BEFORE tearing anything down. screens_[0] is the
  // only slot destroyDataScreens() never touches, so this guarantees the
  // display's active screen survives the teardown.
  //
  // This is the same LVGL trap DisplayManager::loop() already documents on
  // the boot-screens -> carousel handover, and it bites exactly as hard
  // here: lv_obj_del() on the currently active screen sets
  // lv_disp_t::act_scr to NULL (lv_obj_tree.c, "the active screen was
  // deleted"), and the very next lv_scr_load() unconditionally calls
  // lv_obj_set_pos(lv_scr_act(), 0, 0) (lv_disp.c). With LV_USE_ASSERT_OBJ
  // 0 in lv_conf.h that assert is compiled out, so it is a straight NULL
  // dereference -> LoadProhibited panic -> reboot.
  //
  // It fired whenever a config was saved from the SPA while the device was
  // showing a data screen - and since g_configGen is bumped by every
  // section, saving brightness could reboot Figo just as readily as saving
  // the screen list.
  // Unconditional rather than guarded on currentIndex_ != 0: currentIndex_ is
  // this class's intent, lv_scr_act() is LVGL's truth, and the whole point
  // here is not to depend on those agreeing. lv_scr_load() already returns
  // immediately when the target is the active screen, and no repaint happens
  // between here and the final load below (LVGL only draws inside
  // lv_timer_handler()), so there is no flicker to trade away.
  lv_scr_load(screens_[0]);

  destroyDataScreens();

  uint8_t count = config.screen_count;
  if (count > MAX_SCREENS) count = MAX_SCREENS;
  for (uint8_t i = 0; i < count; i++) {
    screens_[i + 1] = lv_obj_create(NULL);
    buildDataScreen(screens_[i + 1], config.screens[i], i + 1, count + 1);
    lv_obj_add_event_cb(screens_[i + 1], gestureEventCb, LV_EVENT_GESTURE, this);
  }
  screenCount_ = count;

  // The face screen survives every rebuild, but its dot row shows the TOTAL
  // screen count - refresh it to match the new config. Create/delete is fine
  // here (only applySnapshot() is barred from object churn).
  if (faceDots_) {
    lv_obj_del(faceDots_);
    faceDots_ = nullptr;
  }
  faceDots_ = screen_builders::addDotIndicator(screens_[0], 0, count + 1);

  uint8_t totalScreens = screenCount_ + 1;
  if (currentIndex_ >= totalScreens) {
    currentIndex_ = 0;
  }
  // Straight lv_scr_load(), not the animated goTo() - this is a config
  // change reflecting on screen, not a user swipe gesture. Safe now: every
  // slot in screens_[] has just been rebuilt, and the screen we parked on
  // above was never deleted.
  lv_scr_load(screens_[currentIndex_]);
  // Restart the dwell rather than letting a rebuild that happened to land
  // late in the countdown flick straight past the screen it just rebuilt.
  armAutoAdvance();

  return true;
}

void ScreenManager::reloadCurrent() {
  uint8_t total = screenCount_ + 1;
  if (currentIndex_ >= total) currentIndex_ = 0;
  if (!screens_[currentIndex_]) currentIndex_ = 0;  // face screen always exists
  lv_scr_load(screens_[currentIndex_]);
  // The dwell timer kept running while the display was elsewhere; restart it
  // so the screen handed back gets its full time rather than flicking past.
  armAutoAdvance();
}

// Auto-advance. Deliberately does nothing while a swipe transition is still
// running: goTo() rearms the countdown anyway, so the only way to get here
// mid-transition is a rebuild, and stacking a second lv_scr_load_anim() on
// top of a live one makes LVGL force-complete the first (lv_disp.c) - a
// visible stutter for no benefit.
void ScreenManager::loop() {
  if (autoAdvanceAtMs_ == 0) return;
  if (screenCount_ == 0) return;  // face screen only - nothing to advance to
  if (transitionEndsAtMs_ != 0 && (int32_t)(millis() - transitionEndsAtMs_) < 0) return;
  if ((int32_t)(millis() - autoAdvanceAtMs_) < 0) return;
  next();  // rearms the countdown via goTo()
}

// Signed millis() arithmetic throughout, so the ~49-day rollover costs one
// mistimed advance rather than a 49-day stall.
void ScreenManager::armAutoAdvance() {
  autoAdvanceAtMs_ = millis() + SCREEN_AUTO_ADVANCE_MS;
  if (autoAdvanceAtMs_ == 0) autoAdvanceAtMs_ = 1;  // 0 is the "disarmed" sentinel
}

void ScreenManager::destroyDataScreens() {
  for (uint8_t i = 1; i <= screenCount_; i++) {
    if (screens_[i]) {
      lv_obj_del(screens_[i]);
      screens_[i] = nullptr;
    }
    // lv_obj_del() frees the screen's children too, so these handles are
    // dangling the moment the screen goes. Clearing them matters: a snapshot
    // could otherwise arrive between the teardown and the rebuild and have
    // applySnapshot() write into freed memory.
    widgets_[i] = {};
  }
}

void ScreenManager::buildFaceScreen(lv_obj_t *screen) {
  lv_obj_set_style_bg_color(screen, figo_theme::bg(), 0);
  // 99px face (face box ~ (110,40), 99x86-98 across the expressions).
  faceRenderer_.init(screen, 99);
  faceRenderer_.setPosition(110, 40);
}

// Builds a data screen in the standard look: static furniture plus
// the mutable widgets recorded into widgets_[slot] for applySnapshot(). Value
// widgets start at "--" rather than 0.00: nothing has been fetched yet, and a
// zero would read as a real price.
void ScreenManager::buildDataScreen(lv_obj_t *screen, const ScreenConfig &cfg,
                                     uint8_t slot, uint8_t totalScreens) {
  lv_obj_set_style_bg_color(screen, figo_theme::bg(), 0);

  widgets_[slot] = {};
  switch (cfg.layout) {
    case ScreenLayout::DUAL:
      screen_builders::buildTickerDual(screen, cfg, widgets_[slot]);
      break;
    case ScreenLayout::FG_SLIDER:
      screen_builders::buildFearGreedSlider(screen, cfg, widgets_[slot]);
      break;
    case ScreenLayout::FG_CIRCULAR:
      screen_builders::buildFearGreedCircular(screen, cfg, widgets_[slot]);
      break;
    case ScreenLayout::SINGLE:
    default:
      // A FEAR_GREED config can only reach here with a corrupt layout value
      // (validation forbids SINGLE on it) - render its default gauge rather
      // than a nonsense ticker.
      if (cfg.type == ScreenType::FEAR_GREED) {
        screen_builders::buildFearGreedSlider(screen, cfg, widgets_[slot]);
      } else {
        screen_builders::buildTickerSingle(screen, cfg, widgets_[slot]);
      }
      break;
  }
  screen_builders::addDotIndicator(screen, slot, totalScreens);
}

void ScreenManager::applySnapshot(const RuntimeConfig &config,
                                   const MarketSnapshot &snapshot) {
  for (uint8_t i = 0; i < screenCount_; i++) {
    uint8_t slot = i + 1;
    const ScreenConfig &cfg = config.screens[i];

    // Match on id, not position - see this method's declaration comment.
    const ScreenValue *value = nullptr;
    for (uint8_t v = 0; v < MAX_SCREENS; v++) {
      if (snapshot.values[v].screen_id == cfg.id) {
        value = &snapshot.values[v];
        break;
      }
    }

    screen_builders::updateDataScreen(cfg, widgets_[slot], value);
  }
}

void ScreenManager::parkIndex(uint8_t index) {
  // The night scheduler restores an index captured minutes or hours earlier,
  // and a config save in between can have shrunk the carousel out from under
  // it. Valid slots are 0..screenCount_ (screens_[0] is the face screen,
  // 1..screenCount_ the data screens - see destroyDataScreens()'s loop bound
  // and reloadCurrent()'s equivalent `>= total` check). Falling back to the
  // face screen is always safe: it is the one slot rebuildFromConfig() never
  // destroys.
  if (index > screenCount_ || !screens_[index]) index = 0;
  currentIndex_ = index;
}

void ScreenManager::goTo(uint8_t index, bool animateLeft) {
  parkIndex(index);
  // Re-read rather than reusing the argument: parkIndex() may have redirected
  // an out-of-range slot to the face screen, and the animation below must
  // load the slot that was actually accepted.
  index = currentIndex_;
  transitionEndsAtMs_ = millis() + kTransitionMs + kTransitionGuardSlackMs;
  // 0 is the "never navigated" sentinel the teardown guard tests for, so
  // never let the rollover land the deadline exactly there.
  if (transitionEndsAtMs_ == 0) transitionEndsAtMs_ = 1;
  // Every navigation - swipe or auto-advance - restarts the dwell, so a
  // screen the user just swiped to always gets its full time on display.
  armAutoAdvance();
  // autoDeleteOldScreen=false: screens are long-lived slot objects owned by
  // this class, not disposable - LVGL must not free them on transition.
  lv_scr_load_anim(screens_[index],
                    animateLeft ? LV_SCR_LOAD_ANIM_MOVE_LEFT : LV_SCR_LOAD_ANIM_MOVE_RIGHT,
                    kTransitionMs, 0, false);
}

void ScreenManager::next() {
  uint8_t total = screenCount_ + 1;
  goTo((currentIndex_ + 1) % total, true);
}

void ScreenManager::prev() {
  uint8_t total = screenCount_ + 1;
  goTo((currentIndex_ + total - 1) % total, false);
}

void ScreenManager::gestureEventCb(lv_event_t *e) {
  ScreenManager *self = static_cast<ScreenManager *>(lv_event_get_user_data(e));
  lv_indev_t *indev = lv_indev_get_act();
  if (!indev) {
    return;
  }

  lv_dir_t dir = lv_indev_get_gesture_dir(indev);
  // Avoids a spurious click firing on the same touch that triggered the
  // gesture - a documented LVGL 8 gotcha.
  lv_indev_wait_release(indev);

  if (dir == LV_DIR_LEFT) {
    self->next();
  } else if (dir == LV_DIR_RIGHT) {
    self->prev();
  }
}
