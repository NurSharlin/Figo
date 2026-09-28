#ifndef BOOT_SCREENS_H
#define BOOT_SCREENS_H

#include <lvgl.h>
#include "FaceRenderer.h"
#include "SharedState.h"  // OtaUiState

// Two static screens shown while SharedState.h's g_networkMode is BOOTING
// or AP (never STA) - a "hello" splash, then a "how to connect to WiFi"
// screen once the device has actually fallen back to SoftAP. Driven by
// DisplayManager::loop() polling g_networkMode, not a timer: showing
// "join Figo-Setup" is only true once the AP actually exists, and a saved
// network's STA attempt can take up to WIFI_STA_CONNECT_TIMEOUT_MS before
// that's known. Neither screen ever gets a gesture event callback attached
// (only ScreenManager's carousel screens do), so swipe navigation is
// impossible here by construction.
class BootScreens {
 public:
  void init();       // builds both screens once, off-screen; call from DisplayManager::begin()
  void showHello();    // loads the hello screen (no-op if already showing)
  // Loads the how-to-connect screen. No-op if already showing, and also
  // no-op until helloMinElapsed() - DisplayManager::loop() calls this every
  // AP-mode tick, so the switch happens on the first tick after that.
  void showConnect();

  // The runtime-disconnect screen (the "NO WIFI" screen): red Panicked
  // face, "WiFi Disconnected", "click the screen to configure new WiFi".
  // Shown when a working STA link drops (NetMode::NO_WIFI) - unlike the two
  // screens above it appears AFTER the carousel is already running, so it
  // builds lazily rather than in init() and does not gate on
  // helloMinElapsed(). Tapping it raises the setup AP (see
  // portalRequestPending()) and swaps to the connect screen, one-way - see
  // runtimeTapCb().
  void showNoWifi();
  // True while showNoWifi()'s pair of screens owns the display, so
  // DisplayManager knows to leave the carousel alone.
  bool noWifiActive() const { return noWifiActive_; }

  // A tap on the disconnect screen asked for the setup AP. There is no
  // SoftAP up during a plain disconnect - the link is retried silently so a
  // router reboot needs no user action at all - so raising one is an
  // explicit request that has to travel to Task 2. DisplayManager polls
  // this and latches it into SharedState.h's g_softApRequested, clearing it
  // only once that write actually happened (its stateMutex take can fail).
  bool portalRequestPending() const { return portalRequestPending_; }
  void clearPortalRequest() { portalRequestPending_ = false; }

  // The OTA update screen: a Greeting face, "Updating Figo", a percentage and
  // a bar; on failure a red face and the reason. The third instance of the
  // showNoWifi() pattern - built lazily, because it too appears AFTER the
  // carousel is running - and it gets the same treatment: otaActive() tells
  // DisplayManager to leave the carousel alone. Called every tick while the
  // update is in progress; cheap when nothing changed.
  void showOta(OtaUiState state, uint8_t percent, const char *message);
  bool otaActive() const { return otaActive_; }
  // Deletes the OTA screen. Call only AFTER something else has loaded its own
  // screen - deleting the active screen NULLs disp->act_scr and the next
  // lv_scr_load() dereferences it (see teardown()).
  void hideOta();

  // The boot-time "settings problem" screen: red Panicked face, "Settings
  // problem", "Using defaults" and the 6x code of every NVS section that was
  // present but unparseable (mask is g_configLoadErrors). Shown ONCE, before the
  // hello splash, for BOOT_CONFIG_ERROR_MS; DisplayManager owns the timing.
  // Built lazily and only when there is something to say, so a healthy boot
  // allocates none of it. Like the OTA screen it can only be deleted after some
  // other screen has been loaded - see hideConfigError().
  void showConfigError(uint32_t mask);
  bool configErrorActive() const { return configErrorScreen_ != nullptr; }
  // Deletes the config-error screen. Call only AFTER something else has loaded
  // its own screen (deleting the active one NULLs disp->act_scr - see teardown()).
  void hideConfigError();

  // True once the hello splash has had its minimum on-screen time, or if it
  // was never shown at all (boot straight into STA - begin() goes directly
  // to the carousel and there is no splash to hold). Both exits from the
  // boot screens gate on this: without it a fast STA connect or the
  // near-instant no-credentials SoftAP fallback makes hello flash by
  // unreadably.
  bool helloMinElapsed() const;
  // Deletes whatever screens currently exist. Idempotent and repeatable:
  // NetworkManager can move STA -> NO_WIFI -> STA, so this runs once
  // after the carousel first starts and again after every recovery from a
  // runtime disconnect. Call it only AFTER something else (the carousel)
  // has loaded its own screen. Must
  // never run while one of these screens is still active: deleting the
  // active screen NULLs disp->act_scr, and the next lv_scr_load()
  // dereferences lv_scr_act() unconditionally - see DisplayManager::loop()'s
  // ordering comment. Stops greetingFace_'s blink timer first -
  // lv_obj_del() frees the LVGL objects but does not know to stop a
  // still-running lv_timer that references them.
  void teardown();

 private:
  void buildHelloScreen(lv_obj_t *screen);
  void buildConnectScreen(lv_obj_t *screen);
  void buildNoWifiScreen(lv_obj_t *screen);
  // Builds connectScreen_/noWifiScreen_ on demand. init() pre-builds the
  // boot pair; the disconnect path arrives later and builds what it needs.
  void ensureConnectScreen();
  void ensureNoWifiScreen();
  void buildOtaScreen(lv_obj_t *screen);
  void buildConfigErrorScreen(lv_obj_t *screen, uint32_t mask);
  // LV_EVENT_CLICKED on either runtime screen - toggles between them. A
  // no-op unless noWifiActive_, so taps during the first-run AP flow (where
  // there is nothing to toggle to) do nothing.
  static void runtimeTapCb(lv_event_t *e);

  lv_obj_t *helloScreen_ = nullptr;
  lv_obj_t *connectScreen_ = nullptr;
  lv_obj_t *noWifiScreen_ = nullptr;
  bool noWifiActive_ = false;
  lv_obj_t *otaScreen_ = nullptr;
  lv_obj_t *otaTitle_ = nullptr;
  lv_obj_t *otaPercent_ = nullptr;
  lv_obj_t *otaBar_ = nullptr;
  lv_obj_t *otaNote_ = nullptr;
  bool otaActive_ = false;
  // What the widgets currently show, so showOta() touches LVGL only on change.
  // Set to impossible values when the screen is built.
  int otaShownState_ = -1;
  int otaShownPct_ = -1;
  const char *otaShownMsg_ = nullptr;
  // Own face instance for the same reason noWifiFace_ has one: a FaceRenderer
  // owns exactly one set of LVGL objects and this screen can coexist with the
  // others.
  FaceRenderer otaFace_;
  bool portalRequestPending_ = false;
  lv_obj_t *configErrorScreen_ = nullptr;
  // Its own face instance, for the same reason as noWifiFace_ / otaFace_.
  FaceRenderer configErrorFace_;
  // millis() when showHello() first loaded the hello screen; showConnect()
  // holds off until BOOT_HELLO_MIN_DURATION_MS past this. 0 = not shown yet.
  uint32_t helloShownAtMs_ = 0;
  // Boot-splash-only face shown on helloScreen_ with Expression::Greeting,
  // next to the greeting text. Separate instance
  // from ScreenManager's carousel FaceRenderer - this one is smaller (96px
  // vs 99px) and is torn down with the rest of the boot screens.
  FaceRenderer greetingFace_;
  // Red Panicked face on the disconnect screen (104px).
  // Its own instance, not greetingFace_: the two screens can exist at the
  // same time and a FaceRenderer owns exactly one set of LVGL objects.
  FaceRenderer noWifiFace_;
};

#endif  // BOOT_SCREENS_H
