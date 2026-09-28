#ifndef NETWORK_MANAGER_H
#define NETWORK_MANAGER_H

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <DNSServer.h>
#include "ConfigManager.h"

// Owns WiFi lifecycle, the single AsyncWebServer instance, and the SoftAP
// captive-portal DNS redirect. Runs entirely on Task 2 (fetchTask) - never
// touches LVGL or DisplayManager. Talks to ConfigManager for reads/writes;
// never touches g_runtimeConfig directly.
//
// Named FigoNetworkManager rather than NetworkManager (despite this file's
// own name and the planned module name): arduino-esp32 core 3.x's
// Network.h - pulled in transitively via <WiFi.h> - now declares its own
// global-namespace class NetworkManager, and the two collide if this one
// keeps that name. File names are unaffected; only the C++ class is renamed.
class OtaManager;

class FigoNetworkManager {
 public:
  // NO_WIFI: a working STA link dropped and has not come back. The radio
  // stays in STA and serviceStaLink() keeps retrying the saved credentials,
  // so a transient outage recovers with no user action and NO setup AP ever
  // appears. Whether a portal is up in this state is orthogonal (portalUp_,
  // raised only on request) - that is why this is a separate flag rather
  // than a fourth mode. Kept distinct from AP so Task 1 can show "WiFi
  // Disconnected" rather than the first-run "connect to my WiFi" splash.
  enum class Mode : uint8_t { STA, AP, NO_WIFI };

  explicit FigoNetworkManager(ConfigManager &configManager) : configManager_(configManager) {}

  // Boot sequence: try STA with saved credentials (bounded retry), fall back
  // to SoftAP "Figo-Setup" + DNS wildcard redirect on failure or if no
  // credentials are saved. Registers all REST/static routes and starts the
  // server either way. Call once from fetchTask before its loop starts.
  void begin();

  // Call every fetchTask iteration. Pumps the captive-portal DNS server
  // (only does real work while a SoftAP is up - AsyncWebServer/AsyncTCP
  // itself is event-driven and needs no pumping). Also supervises the STA
  // link (see serviceStaLink()) and services a deferred AP shutdown
  // scheduled by a successful mid-session WiFi switch (see
  // connectAndPersist()) so the HTTP response has time to flush first.
  void loop();

  Mode mode() const { return mode_; }
  // True whenever a SoftAP is actually running - the first-run portal and a
  // user-requested one during a disconnect alike. Used for DNS pumping and
  // the fetch task's loop pacing, both of which care about the AP being up
  // rather than about why it came up. NOT derivable from mode_: NO_WIFI runs
  // without a portal until the user taps for one.
  bool softApUp() const { return portalUp_; }

  // --- OtaManager's view of this class ---------------------------------------
  // Set once from fetchTask before begin(); the OTA routes and the write
  // guard both go through it. Null-safe everywhere it is read.
  void attachOta(OtaManager *ota) { ota_ = ota; }
  // True once a reboot is armed (anything: OTA, WiFi forget, factory reset).
  bool rebooting() const { return rebootPending_; }
  // The web server has been started - one input to "is this new image healthy".
  bool serverUp() const { return serverUp_; }
  // The phone can reach us: joined to WiFi, or the setup AP is up.
  bool linkHealthy() const;
  // Send-the-response-then-restart, deferred a moment via loop() so AsyncTCP
  // can flush before the link drops. Public for OtaManager, which must arm its
  // reboot through THIS (not a private flag): the write guard keys on
  // rebootPending_.
  void scheduleReboot(uint32_t delayMs);

 private:
  void startSoftApMode();
  // Watches a live STA link and drives STA <-> NO_WIFI. Entirely
  // non-blocking: a lost link first gets WIFI_STA_DROP_GRACE_MS of silent
  // WiFi.reconnect() with the carousel still up, and only then is the drop
  // shown to the user. From NO_WIFI it re-attempts the saved credentials
  // every WIFI_STA_RETRY_INTERVAL_MS and returns to STA the moment one
  // lands, so a rebooted router recovers with no user action. Also honours
  // a pending portal request (takePortalRequest()).
  void serviceStaLink();
  // Enters Mode::NO_WIFI: publishes the state for Task 1 and starts the
  // background retry cadence. Does not touch the radio - no portal.
  void enterNoWifi();
  // Returns to Mode::STA on a successful reconnect, dropping a
  // user-requested portal first if one is up.
  void leaveNoWifi();
  // Brings the setup AP up on top of the still-retrying STA (WIFI_AP_STA),
  // in response to a tap on the "WiFi Disconnected" screen.
  void raisePortal();
  // Reads and clears g_softApRequested (SharedState.h) under stateMutex.
  bool takePortalRequest();
  // Sets mode_ and mirrors it into SharedState.h's g_networkMode under
  // stateMutex, so Task 1 (DisplayManager) can tell AP/STA state apart
  // without ever calling into this class directly. Every mode_ write site
  // in this file must go through here, not assign mode_ directly.
  void setMode(Mode m);
  // Attempts an STA connection, keeping the current AP alive via
  // WIFI_AP_STA if we're already serving one, so a bad new password never
  // strands the user without a way to reach the device. Returns true and
  // persists via ConfigManager on success; on failure restores whatever
  // connectivity existed before the attempt and leaves NVS untouched.
  bool connectAndPersist(const char *ssid, const char *password, String *errorOut);
  bool waitForStaConnect(uint32_t timeoutMs);

  void registerRoutes();
  void handleStatus(AsyncWebServerRequest *request);
  void handleWifiScan(AsyncWebServerRequest *request);
  void handleWifiConnect(AsyncWebServerRequest *request, JsonVariant &json);
  void handleWifiForget(AsyncWebServerRequest *request);
  void handleGetConfig(AsyncWebServerRequest *request);
  // GET /api/screens/state - the live half of the config: how fresh each
  // screen's data is, what it last fetched, and the resulting face mood.
  // Read-only and pure memory work; see the definition for why that matters
  // on AsyncTCP's watchdog-subscribed task.
  void handleScreensState(AsyncWebServerRequest *request);
  // POST /api/search {q} - queues an instrument search. Returns immediately:
  // the fetching happens on fetchTask, because a handler that waits on a
  // provider would outlast AsyncTCP's watchdog.
  void handleSearchPost(AsyncWebServerRequest *request, JsonVariant &json);
  // GET /api/search/result - whatever the single job slot currently holds.
  void handleSearchResult(AsyncWebServerRequest *request);
  // POST /api/search/check {type, symbol} - queues ONE price fetch for a symbol
  // the user is considering. Queued, not performed here, for the same reason
  // the search is: a handler must never wait on a provider.
  void handleSearchCheck(AsyncWebServerRequest *request, JsonVariant &json);
  void handlePostConfig(AsyncWebServerRequest *request, JsonVariant &json);
  void handleReboot(AsyncWebServerRequest *request);
  void handleFactoryReset(AsyncWebServerRequest *request);
  // True once a reboot is armed. Mutating handlers answer 503 while it holds:
  // a request queued after a factory reset must not write a fresh blob into
  // the namespace just erased, nor start fetchTask work that could delay the
  // loop() iteration that calls ESP.restart().
  bool goingDown() const { return rebootPending_ || otaBusy(); }
  bool otaBusy() const;
  // The write guard, applied at the top of every mutating route. Answers 503
  // and returns true while a reboot is armed OR an OTA update owns the device:
  // a factory reset or WiFi-forget reboot must not cut an upload short. One
  // predicate for both, not a second mechanism beside goingDown().
  bool rejectIfBusy(AsyncWebServerRequest *request);

  ConfigManager &configManager_;
  AsyncWebServer server_{80};
  DNSServer dnsServer_;
  Mode mode_ = Mode::AP;
  // Whether WiFi.softAP() is currently running. Tracked explicitly because
  // it no longer follows from mode_ - see softApUp().
  bool portalUp_ = false;

  // Set by connectAndPersist() when a successful STA switch happened while
  // an AP was still up for a phone client mid-request; fetchTask's loop()
  // drops the AP once this deadline passes so the REST response has time
  // to reach the client first.
  bool apShutdownPending_ = false;
  uint32_t apShutdownAtMs_ = 0;

  OtaManager *ota_ = nullptr;
  bool serverUp_ = false;

  bool rebootPending_ = false;
  uint32_t rebootAtMs_ = 0;
  // Set by handleFactoryReset(); loop() then calls esp_wifi_restore() right
  // before ESP.restart(). Never in the handler - see loop().
  bool eraseRadioOnReboot_ = false;

  // millis() when a Mode::STA link was first seen down; 0 while it is up.
  // serviceStaLink() declares the disconnect once this is older than
  // WIFI_STA_DROP_GRACE_MS.
  uint32_t staDownSinceMs_ = 0;
  // Next background re-attempt of the saved credentials while in NO_WIFI.
  uint32_t nextStaRetryAtMs_ = 0;
  // Set while a REST handler is driving the radio - a scan or a connect,
  // both of which run on AsyncTCP's task rather than fetchTask.
  // serviceStaLink() (fetchTask) stands down while it is set, because two
  // tasks driving the radio at once is what made scans return empty and made
  // a good password look like a failure.
  volatile bool radioBusy_ = false;
};

#endif  // NETWORK_MANAGER_H
