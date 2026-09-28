#include "DisplayManager.h"
#include "Config.h"
#include <esp_heap_caps.h>

void DisplayManager::begin() {
  // Read the config ConfigManager loaded in setup() (before either task
  // started) - a bounded wait is defensive-only here, the mutex is
  // uncontended this early. Never call into ConfigManager itself from
  // Task 1 - only read SharedState under stateMutex, per this class's
  // architectural rule.
  RuntimeConfig initialConfig = {};
  NetMode initialMode = NetMode::BOOTING;
  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
    initialConfig = g_runtimeConfig;
    lastSeenConfigGen_ = g_configGen;
    initialMode = g_networkMode;
    xSemaphoreGive(stateMutex);
  }

  if (!displayDriver_.begin(initialConfig.system.rotation)) {
    Serial.println("DisplayDriver begin() failed");
  }

  // Query post-rotation state from gfx rather than Config.h's
  // PANEL_WIDTH/PANEL_HEIGHT directly: Arduino_ST7789 is always constructed
  // with the panel's native (unrotated) 172x320 dimensions, and
  // DisplayDriver::begin() applies the requested rotation via setRotation()
  // internally. For a 90/270 rotation, Arduino_GFX swaps what width()/
  // height() report - LVGL's hor_res/ver_res and the touch driver's
  // rotation-correction math both need those post-rotation values, exactly
  // like the vendor demo does (it queries gfx->getRotation()/width()/
  // height() after setRotation(), not the pre-rotation constructor args).
  Arduino_GFX *gfx = displayDriver_.gfx();
  uint16_t rotation = gfx->getRotation();
  uint16_t screenWidth = gfx->width();
  uint16_t screenHeight = gfx->height();

  touchDriver_.begin(rotation, screenWidth, screenHeight);

  lv_init();

  // Partial buffer only - a full-screen framebuffer is forbidden
  // (~110KB for 172x320). LVGL_BUFFER_ROWS is sized in Config.h to land at
  // ~11KB per row-count, computed here against the actual (post-rotation)
  // horizontal resolution. MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT matches
  // Waveshare's own working demo allocation flags exactly - do not add
  // MALLOC_CAP_DMA speculatively.
  uint32_t bufSizePixels = (uint32_t)screenWidth * LVGL_BUFFER_ROWS;
  bufPixels_ = (lv_color_t *)heap_caps_malloc(
      bufSizePixels * sizeof(lv_color_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!bufPixels_) {
    Serial.println("LVGL draw buffer allocation failed!");
  }
  lv_disp_draw_buf_init(&drawBuf_, bufPixels_, NULL, bufSizePixels);

  lv_disp_drv_init(&dispDrv_);
  dispDrv_.hor_res = screenWidth;
  dispDrv_.ver_res = screenHeight;
  dispDrv_.flush_cb = flushCb;
  dispDrv_.draw_buf = &drawBuf_;
  dispDrv_.user_data = this;
  lv_disp_drv_register(&dispDrv_);

  lv_indev_drv_init(&indevDrv_);
  indevDrv_.type = LV_INDEV_TYPE_POINTER;
  indevDrv_.read_cb = touchReadCb;
  indevDrv_.user_data = this;
  lv_indev_drv_register(&indevDrv_);

  // No manual tick timer here: lv_conf.h has LV_TICK_CUSTOM 1 (reading
  // millis() directly), which is the vendor's own proven setup for this
  // board. Calling lv_tick_inc() manually on top of that would double-count
  // elapsed time and throw off every animation/timer's timing.

  // Faces must never show until the device is actually on STA. bootScreens_ is always built so it's ready the instant
  // it's needed; which screen (if any) loads first depends on how far
  // FigoNetworkManager (Task 2) has already gotten by the time this runs -
  // task start order isn't guaranteed, and it may already be done (no saved
  // credentials -> near-instant AP fallback) or still mid STA-connect
  // attempt (-> BOOTING, up to WIFI_STA_CONNECT_TIMEOUT_MS).
  bootScreens_.init();
  if (g_configLoadErrors != 0) {
    // A stored section would not parse and defaults are being used instead.
    // Say so first, for a fixed time, then let the normal boot flow run - the
    // hold itself lives in loop()'s pre-carousel branch. g_configLoadErrors is
    // read without the mutex on purpose: it was written before this task
    // existed and is never written again (see SharedState.h).
    bootScreens_.showConfigError(g_configLoadErrors);
    configErrorUp_ = true;
    configErrorUntilMs_ = millis() + BOOT_CONFIG_ERROR_MS;
  } else if (initialMode == NetMode::STA) {
    startCarousel(initialConfig);
  } else {
    // BOOTING and AP both start on the hello splash - even when the SoftAP
    // is already up, the hello screen gets its minimum on-screen time and
    // loop()'s AP branch advances to the connect screen once that elapses
    // (see BootScreens::showConnect()).
    bootScreens_.showHello();
  }
}

void DisplayManager::startCarousel(const RuntimeConfig &config) {
  screenManager_.init(config);
  carouselActive_ = true;
}

void DisplayManager::loop() {
  lv_timer_handler();

  // Carousel auto-advance. Only meaningful once the boot screens have handed
  // over; before that ScreenManager owns no screens at all. Runs before the
  // SharedState poll below so an advance and a rebuild in the same tick
  // resolve in the order the user would expect (advance, then rebuild
  // rearms the dwell).
  // Suppressed while the disconnect screen owns the display: goTo() would
  // animate a carousel screen straight over the top of it.
  // ...and suppressed while asleep, or Figo cycles the carousel all night
  // behind a dim backlight.
  // ...and while the OTA screen owns the panel, for the same reason.
  if (carouselActive_ && !bootScreens_.noWifiActive() && !bootScreens_.otaActive() &&
      !sleepActive_) {
    screenManager_.loop();
  }

  // Non-blocking (timeout 0): this is uiTask's hot loop, it must never
  // stall waiting on a mutex Task 2 might be holding - just try again next
  // tick. Holds the mutex only long enough to copy g_runtimeConfig out; the
  // (possibly deferred) rebuild itself runs unguarded afterwards.
  if (xSemaphoreTake(stateMutex, 0) == pdTRUE) {
    uint32_t currentGen = g_configGen;
    RuntimeConfig snapshot = g_runtimeConfig;
    NetMode currentMode = g_networkMode;
    // Copied out under the same critical section as the config rather than a
    // second take: applySnapshot() has to match values to screens by id, so
    // the two must be a consistent pair. Taking them separately could pair a
    // pre-edit screen list with a post-edit snapshot.
    uint32_t currentMarketGen = g_marketGen;
    MarketSnapshot market = g_marketSnapshot;
    // Task 2 -> Task 1: the OTA update's stage, progress and message (a string
    // literal, so the pointer outlives the mutex).
    OtaUiState otaState = g_otaUiState;
    uint8_t otaPercent = g_otaPercent;
    const char *otaMessage = g_otaMessage;
    // The one Task 1 -> Task 2 write in this class: a tap on the "WiFi
    // Disconnected" screen asking for the setup AP. Cleared only here, so a
    // tick where this mutex take failed simply retries on the next one
    // rather than losing the tap.
    if (bootScreens_.portalRequestPending()) {
      g_softApRequested = true;
      bootScreens_.clearPortalRequest();
    }
    xSemaphoreGive(stateMutex);

    if (!carouselActive_ && configErrorUp_) {
      // Hold the boot-time "settings problem" screen for its full duration,
      // whatever the network is doing meanwhile. When it is over, hand over to
      // the hello splash - loaded FIRST, config-error screen deleted SECOND, the
      // same LVGL ordering trap as everywhere else in this function. The
      // ordinary branch below then runs unchanged: hello's own minimum time,
      // then the carousel (STA) or the connect screen (AP). A boot straight
      // into STA therefore shows hello for its 2.5s after a config error, which
      // an error-free boot skips; that is the price of not special-casing it.
      if ((int32_t)(millis() - configErrorUntilMs_) >= 0) {
        bootScreens_.showHello();
        bootScreens_.hideConfigError();
        configErrorUp_ = false;
      }
    } else if (!carouselActive_) {
      // Forward transition only, and only here in the pre-carousel phase:
      // BOOTING/AP -> STA hands over to the carousel exactly once. The
      // reverse case (a working link dropping later) is NOT this branch -
      // it is NetMode::NO_WIFI, handled further down against a carousel
      // that is already running.
      // Both exits from the boot screens wait out the hello splash's
      // minimum on-screen time (BootScreens::helloMinElapsed()) - a fast
      // STA connect would otherwise replace it before it can be read.
      if (currentMode == NetMode::STA && bootScreens_.helloMinElapsed()) {
        // Order matters: build+load the carousel's screen FIRST, teardown
        // SECOND. lv_scr_load() unconditionally dereferences the currently
        // active screen (lv_disp.c: lv_obj_set_pos(lv_scr_act(), ...)), and
        // deleting the active screen leaves disp->act_scr NULL - tearing
        // down first therefore crashes inside startCarousel()'s
        // lv_scr_load() with a NULL-load fault.
        startCarousel(snapshot);
        bootScreens_.teardown();
        lastSeenConfigGen_ = currentGen;
      } else if (currentMode == NetMode::AP) {
        bootScreens_.showConnect();
      }
      // else: still BOOTING, or STA within the hello splash's minimum
      // duration - either way the hello screen (shown by begin()) stays up
      // and this retries next tick.
    } else if (otaState != OtaUiState::IDLE) {
      // An update owns the panel. Ahead of the NO_WIFI branch on purpose: an
      // update pushed over the setup AP runs in exactly the modes that branch
      // reacts to, and it must not swap the progress screen out from under it.
      // Full brightness even inside the night window - a dark panel during a
      // one-minute update reads as a dead device.
      bootScreens_.showOta(otaState, otaPercent, otaMessage);
      if (!otaShown_) {
        otaShown_ = true;
        applySystem(snapshot.system);
      }
    } else if (bootScreens_.otaActive()) {
      // The update ended without a reboot (it failed, or was disarmed). Hand
      // the display back: load a carousel screen FIRST, delete the OTA screen
      // SECOND - the same LVGL ordering trap as the two branches around this
      // one. reloadCurrent() lands on the parked index, so a failure during the
      // night window returns to the Sleeping face rather than a lit ticker. If
      // the link is also down, the NO_WIFI branch below re-shows its screen on
      // the next tick.
      screenManager_.reloadCurrent();
      bootScreens_.hideOta();
      otaShown_ = false;
      applySystem(snapshot.system);
    } else if (currentMode == NetMode::NO_WIFI || currentMode == NetMode::AP) {
      // A working link dropped. Stop showing prices that can no longer be
      // refreshed; the carousel screens stay allocated and off-screen, so
      // recovery below is a single lv_scr_load() with no rebuild. No setup
      // AP exists at this point - NetworkManager is retrying the saved
      // network silently, and only a tap on this screen raises one.
      bootScreens_.showNoWifi();
      // Deliberately does NOT advance lastSeenConfigGen_/lastSeenMarketGen_:
      // a config saved from the portal while offline is applied on recovery
      // instead of being dropped. rebuildFromConfig() must not run here at
      // all - it parks on screens_[0], which would yank the display off the
      // disconnect screen.
    } else if (bootScreens_.noWifiActive()) {
      // Back on STA. Load a carousel screen FIRST, then delete the
      // disconnect screens - the same LVGL ordering trap documented above:
      // lv_obj_del() on the active screen NULLs disp->act_scr and the next
      // lv_scr_load() dereferences it.
      screenManager_.reloadCurrent();
      bootScreens_.teardown();
    } else if (currentGen != lastSeenConfigGen_) {
      if (screenManager_.rebuildFromConfig(snapshot)) {
        lastSeenConfigGen_ = currentGen;
        // A rebuild replaced every data screen's labels with fresh "--"
        // placeholders, so the current values have to be re-applied even if
        // no new fetch has landed since.
        screenManager_.applySnapshot(snapshot, market);
        lastSeenMarketGen_ = currentMarketGen;
      }
      // else: still mid swipe-transition - lastSeenConfigGen_ stays behind
      // so this retries on a later tick instead of dropping the update.
    } else if (currentMarketGen != lastSeenMarketGen_) {
      screenManager_.applySnapshot(snapshot, market);
      lastSeenMarketGen_ = currentMarketGen;
    }

    // Outside the chain above on purpose: the face's mood does not depend
    // on WHICH screen is currently showing, and it has to be re-evaluated
    // after a config change (the user moved the star or the thresholds) just
    // as much as after a fetch. applyMood() no-ops unless the band actually
    // moved, so running it every tick costs a comparison.
    //
    // Guarded on carouselActive_ because FaceRenderer belongs to the
    // carousel's face screen, which does not exist until startCarousel() has
    // run. It deliberately still runs while the disconnect screen is up: those
    // screens stay allocated off-screen, so the face is already correct the
    // moment the link comes back.
    //
    // Also guarded on !sleepActive_: DataFetcher keeps polling all night (by
    // design - crypto trades 24/7), and without this guard the first band
    // change after sleep entry would call setExpression() straight over the
    // Sleeping face, which then never comes back for the rest of the window -
    // a daytime test can't see this because the band doesn't move in three
    // minutes. This is exactly why the wake edge's applyMood() call below
    // takes force=true: lastMood_ goes stale while this guard is skipping
    // every tick asleep, and the forced call is what makes the wake tick
    // push the market-driven face even though the band "hasn't changed" by
    // lastMood_'s stale reckoning. On the wake tick itself, sleepActive_ is
    // still true when this line runs (it flips inside the block below), so
    // this line correctly no-ops and lets the forced call do the pushing.
    if (carouselActive_ && !sleepActive_) applyMood(snapshot, market);

    // Night scheduler. Gated to once a second: the window has minute
    // resolution, so a time() call plus two string parses on every 5ms tick
    // would be pure waste.
    uint32_t nowMs = millis();
    if ((uint32_t)(nowMs - lastSleepCheckMs_) >= SLEEP_CHECK_PERIOD_MS) {
      lastSleepCheckMs_ = nowMs;

      // Guarded on carouselActive_ because the transitions below touch
      // FaceRenderer and the carousel, neither of which exists before
      // startCarousel(). A device booting inside the window therefore stays
      // awake until the carousel is up, then sleeps on the next tick.
      if (carouselActive_) {
        // Consumed before the window is evaluated, so a touch wakes Figo on
        // this tick rather than the next one. Reading it here also means a
        // press latched before the carousel existed is discarded harmlessly:
        // sleepActive_ cannot be true yet, so it sets no override.
        bool touched = touchPressed_;
        touchPressed_ = false;
        if (sleepActive_ && touched) sleepOverride_ = true;

        bool want = sleepingNow(snapshot.system, time(nullptr));
        // Cleared on any false reading, which IS window exit: the other false
        // cases (window disabled, clock never set) mean there was nothing to
        // override in the first place.
        if (!want) sleepOverride_ = false;
        bool nextSleep = want && !sleepOverride_;
        if (nextSleep != sleepActive_) {
          sleepActive_ = nextSleep;
          // Dimming is the user-visible promise of this feature and must not
          // depend on the network being up, so the state flip, setExpression(),
          // and applySystem() below all run unconditionally even with the
          // disconnect screen on display. What must NOT happen is a goTo():
          // that would animate a carousel screen straight over the disconnect
          // screen, the same trap the auto-advance guard above this function
          // exists for. So the ANIMATION alone is skipped while noWifiActive()
          // - parkIndex() still records where the carousel belongs, so when
          // Wi-Fi returns loop()'s recovery branch (above) calls
          // reloadCurrent() and lands on the right slot.
          //
          // Recording the index is not optional, and getting this wrong is a
          // six-hour failure rather than a glitch: skip it and currentIndex_
          // still holds the data screen that was up when the link dropped, so
          // recovery mid-window loads a dimmed live ticker and the Sleeping
          // face stays off-screen for the rest of the night. The sleep
          // transition has already happened by then, so nothing gives it a
          // second chance.
          // The OTA screen is the second thing goTo() must not animate over.
          bool suppressGoTo = bootScreens_.noWifiActive() || bootScreens_.otaActive();
          if (nextSleep) {
            // Captured here regardless of suppressGoTo, and kept separate from
            // the navigation below: a skipped goTo() must not leave
            // preSleepIndex_ holding a stale value from a previous cycle.
            preSleepIndex_ = screenManager_.currentIndex();
            if (suppressGoTo) {
              screenManager_.parkIndex(0);  // 0 is the face screen
            } else {
              screenManager_.goTo(0, /*animateLeft=*/false);
            }
            screenManager_.faceRenderer().setExpression(Expression::Sleeping);
          } else {
            if (suppressGoTo) {
              screenManager_.parkIndex(preSleepIndex_);
            } else {
              screenManager_.goTo(preSleepIndex_, /*animateLeft=*/true);
            }
            // FORCED, and this is the subtle one. applyMood() pushes only when
            // the band CHANGES, because setExpression() restarts animations.
            // A market that sat still overnight would therefore leave the
            // Sleeping face up all morning - and resetting lastMood_ to
            // Unknown does not fix it either, since a wake onto stale data
            // computes Unknown too and change-detection still finds nothing.
            applyMood(snapshot, market, /*force=*/true);
          }
        }
      }

      // Publish the PANEL's answer for /api/screens/state, which must not be
      // a second copy of the rule that disagrees with the glass (SharedState.h
      // says exactly this about sleepingNow()). Only this derived result
      // crosses tasks - the latches behind it (sleepActive_, sleepOverride_,
      // touchPressed_) stay Task-1-local and unmutexed, which is the plan's
      // Global Constraint. Task 1 -> Task 2, under stateMutex, the same
      // direction and pattern as g_softApRequested.
      //
      // Written every tick rather than only on a transition: a failed take
      // then self-corrects a second later, where an edge-only write would
      // leave the API stale until the next edge - which can be hours.
      if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        g_sleepingEffective = sleepActive_;
        xSemaphoreGive(stateMutex);
      }

      applySystem(snapshot.system);
    }
  }
}

void DisplayManager::flushCb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p) {
  DisplayManager *self = static_cast<DisplayManager *>(drv->user_data);
  self->displayDriver_.flush(area->x1, area->y1, area->x2, area->y2, (uint16_t *)&color_p->full);
  lv_disp_flush_ready(drv);
}

void DisplayManager::touchReadCb(lv_indev_drv_t *drv, lv_indev_data_t *data) {
  DisplayManager *self = static_cast<DisplayManager *>(drv->user_data);
  int16_t x, y;
  if (self->touchDriver_.read(x, y)) {
    data->point.x = x;
    data->point.y = y;
    data->state = LV_INDEV_STATE_PRESSED;
    // Wake-from-sleep latch. Hooked at the driver rather than as a per-screen
    // LV_EVENT_CLICKED (the BootScreens portal-tap pattern) for two reasons:
    // it catches a press on ANY screen with nothing to register per screen,
    // and it cannot interact with the carousel's swipe gestures.
    self->touchPressed_ = true;
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
}

// The rule itself is NOT here - it is moodFor() in SharedState.h, because
// NetworkManager's /api/screens/state has to report the same answer to the
// phone and two copies of a rule are how two copies disagree. This function
// only does the half that needs LVGL: band -> Expression -> FaceRenderer.
//
// Expression::Greeting is unreachable by construction (boot-splash only, see
// BootScreens::buildHelloScreen()). Expression::Sleeping never comes from
// this switch either - moodFor() has no band for "asleep" - it is pushed
// directly by the night scheduler's sleep-entry transition in loop(), which
// also owns waking this function's own short-circuit via force=true.
void DisplayManager::applyMood(const RuntimeConfig &config, const MarketSnapshot &market,
                               bool force) {
  MoodBand band = moodFor(config.expression_source, market);
  if (band == lastMood_ && !force) return;
  lastMood_ = band;

  Expression e;
  switch (band) {
    case MoodBand::Bullish: e = Expression::Bullish; break;
    // Happy and Bullish are a deliberate pair, not near-duplicates: the
    // artwork gives Bullish "$" eyes and a cha-ching beat, Happy a contented
    // bob. Without this band the face looked identical at +5.01% and +30%.
    case MoodBand::Happy: e = Expression::Happy; break;
    case MoodBand::Bearish: e = Expression::Panicked; break;
    // Unknown lands here with Neutral: nothing starred, a deleted starred
    // screen, nothing fetched yet, or a value gone stale. The panel has no
    // room to explain the difference and a guessed mood would be worse than
    // a blank one - the SPA's Figo tab is where the distinction gets words.
    case MoodBand::Neutral:
    case MoodBand::Unknown:
    default: e = Expression::Neutral; break;
  }
  screenManager_.faceRenderer().setExpression(e);
}

void DisplayManager::applySystem(const SystemConfig &sys) {
  // An update forces full brightness. sys.brightness would honour the user's
  // setting, which BRIGHTNESS_MIN keeps visible, but the night dim is a
  // deliberate "make it dark" - and a dark panel mid-update looks like a crash.
  uint8_t pct = otaShown_ ? 100 : (sleepActive_ ? (uint8_t)SLEEP_BRIGHTNESS_PCT : sys.brightness);
  if (pct == lastBacklightPct_) return;
  lastBacklightPct_ = pct;
  // Rounded, not truncated: at 100% this must reach a true 255 rather than
  // 254, and at low percentages the rounding is the difference between a
  // visible glow and nothing.
  displayDriver_.setBacklight((uint8_t)((pct * 255 + 50) / 100));
}
