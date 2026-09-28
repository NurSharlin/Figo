#include "NetworkManager.h"
#include "Config.h"
#include "OtaManager.h"
#include "SharedState.h"
#include "WebAssets.h"
#include <WiFi.h>
#include <ESPmDNS.h>
#include <AsyncJson.h>
#include <ArduinoJson.h>
#include <esp_task_wdt.h>
#include <esp_wifi.h>

void FigoNetworkManager::setMode(Mode m) {
  mode_ = m;
  NetMode shared = NetMode::AP;
  if (m == Mode::STA) {
    shared = NetMode::STA;
    // Every path to STA goes through here - first boot, a portal
    // provisioning, and leaveNoWifi() after a drop - so this is the one place
    // the clock needs starting, and re-entry is harmless.
    //
    // gmtoff and dst are both 0 DELIBERATELY: the device holds UTC and
    // SleepWindow.h applies the user's offset itself. Passing a real offset
    // here would make localtime() disagree with the offset SleepWindow.h
    // applies, and two answers to "what time is it" is how they drift apart.
    // Note configTime() calls setTimeZone() internally either way, so the C
    // library's zone handling is linked regardless - both zeros just keep it
    // a no-op (localtime() == gmtime() year-round).
    //
    // Non-blocking: this only registers the servers and starts lwIP's SNTP
    // client; the hostname is resolved asynchronously later, on the tcpip
    // thread, not here. Re-entry is safe because the core stops any running
    // client before restarting one. Re-syncs every 3 hours
    // (CONFIG_LWIP_SNTP_UPDATE_DELAY). Nothing waits on it -
    // sleepWindowActive() fails open until the first sync lands (EPOCH_SANE).
    configTime(0, 0, NTP_HOST_PRIMARY, NTP_HOST_FALLBACK);
  } else if (m == Mode::NO_WIFI) {
    shared = NetMode::NO_WIFI;
  }
  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
    g_networkMode = shared;
    xSemaphoreGive(stateMutex);
  }
}

void FigoNetworkManager::begin() {
  RuntimeConfig cfg = configManager_.getSnapshot();
  if (cfg.wifi.configured && strlen(cfg.wifi.ssid) > 0) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(cfg.wifi.ssid, cfg.wifi.password);
    if (waitForStaConnect(WIFI_STA_CONNECT_TIMEOUT_MS)) {
      setMode(Mode::STA);
      MDNS.begin(MDNS_HOSTNAME);
      Serial.printf("NetworkManager: connected to '%s', IP=%s\n", cfg.wifi.ssid,
                    WiFi.localIP().toString().c_str());
    } else {
      // Bounded retry already exhausted in waitForStaConnect() - fall back
      // rather than retry indefinitely, so the device is always reachable
      // via SoftAP even when the saved network is unreachable. Saved
      // credentials are left untouched in NVS; only an explicit
      // /api/wifi/forget clears them.
      Serial.println("NetworkManager: STA connect failed, falling back to SoftAP");
      startSoftApMode();
    }
  } else {
    startSoftApMode();
  }

  registerRoutes();
  server_.begin();
  serverUp_ = true;
}

void FigoNetworkManager::loop() {
  if (softApUp()) {
    dnsServer_.processNextRequest();
  }
  if (apShutdownPending_ && millis() >= apShutdownAtMs_) {
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    dnsServer_.stop();
    portalUp_ = false;
    // Undo raisePortal()'s takeover. This is the path a user actually takes
    // out of the portal - provisioning succeeds and the AP is dropped a
    // moment later - and it bypasses leaveNoWifi() entirely, so without this
    // auto-reconnect would stay OFF for the rest of the session and the next
    // drop could only be recovered by the 60s backstop instead of healing
    // itself. Cheap to set unconditionally; harmless when it was never off.
    WiFi.setAutoReconnect(true);
    apShutdownPending_ = false;
    staDownSinceMs_ = 0;  // the new link is the one to supervise from here
    setMode(Mode::STA);
  }
  serviceStaLink();
  if (rebootPending_ && millis() >= rebootAtMs_) {
    // g_prefs.clear() cannot reach esp_wifi's own saved-AP record (separate
    // nvs.net80211 namespace). Figo never reads it, but a reset board
    // changing hands must not carry the old household's password in flash.
    // Here and not in the handler: taking the interface down there kills the
    // link the 200 is travelling on (in portal mode, the user's link to the
    // page); at the restart instant there is nothing left to flush.
    // esp_wifi_restore() rather than WiFi.disconnect(true, true): from the
    // portal the STA interface was never started and Arduino's disconnect()
    // returns false without erasing anything.
    if (eraseRadioOnReboot_) esp_wifi_restore();
    ESP.restart();
  }
}

// The runtime link supervisor. Before this existed, mode_ was write-once
// per boot: begin() decided STA or AP and nothing ever revisited it, so
// pulling the router left mode_ == STA forever. DataFetcher stopped
// fetching (its wifiReady() does check WiFi.status()), but nothing told
// Task 1, so the carousel kept showing the last prices it had - with no
// SoftAP to reconfigure from.
void FigoNetworkManager::serviceStaLink() {
  // Stand down while another path owns the radio: a REST-driven connect on
  // AsyncTCP's task, or the deferred AP shutdown that follows one.
  if (radioBusy_ || apShutdownPending_ || rebootPending_) return;

  uint32_t now = millis();
  bool linkUp = (WiFi.status() == WL_CONNECTED);

  if (mode_ == Mode::STA) {
    if (linkUp) {
      staDownSinceMs_ = 0;
      return;
    }
    if (staDownSinceMs_ == 0) {
      // First tick with the link down. Just start the grace window before
      // doing anything the user can see - a DHCP renew or an AP roam
      // routinely blips for a few seconds.
      //
      // Deliberately does NOT call WiFi.reconnect() here. arduino-esp32's
      // auto-reconnect is already re-associating at this instant, so the
      // call is refused every time ("E wifi:sta is connecting, return
      // error") - it was pure log noise on an ordinary disconnect and did
      // nothing. The 60s backstop below, which disconnects first, is what
      // covers the case where the driver has actually given up.
      staDownSinceMs_ = (now == 0) ? 1 : now;  // 0 is the "link up" sentinel
      Serial.println("NetworkManager: STA link lost, waiting for auto-reconnect");
      return;
    }
    if ((now - staDownSinceMs_) < WIFI_STA_DROP_GRACE_MS) return;
    enterNoWifi();
    return;
  }

  if (mode_ == Mode::NO_WIFI) {
    if (linkUp) {
      leaveNoWifi();
      return;
    }
    // The user tapped the "WiFi Disconnected" screen asking to set up a
    // different network. Checked before the retry below so the portal comes
    // up immediately rather than on the next retry tick a minute later.
    if (takePortalRequest() && !portalUp_) raisePortal();

    // Once the portal is up the radio belongs to the user. raisePortal()
    // has already turned auto-reconnect off and aborted the association;
    // this just makes sure nothing here re-arms it. Competing for the radio
    // is what made the scan come back empty and made a good password report
    // "could not connect" - see the note on raisePortal().
    //
    // Note this means a portal Figo does NOT heal itself: the linkUp check
    // above can only fire if something else brought the link back, and with
    // auto-reconnect off nothing will. That is intended - a tap is a
    // deliberate "I want to change networks" - but it does mean the way out
    // of portal mode is finishing provisioning or rebooting.
    if (portalUp_) return;

    if ((int32_t)(now - nextStaRetryAtMs_) < 0) return;
    nextStaRetryAtMs_ = now + WIFI_STA_RETRY_INTERVAL_MS;
    RuntimeConfig cfg = configManager_.getSnapshot();
    if (cfg.wifi.configured && strlen(cfg.wifi.ssid) > 0) {
      // Clear the association attempt first. Without this the begin() below
      // is rejected outright ("E wifi:sta is connecting, cannot set config")
      // and the retry is a no-op - so the retry would do nothing. disconnect() also re-arms auto-reconnect when
      // begin() follows it, so this is a clean restart of the attempt rather
      // than a way of giving up.
      Serial.printf("NetworkManager: retrying saved network '%s'\n", cfg.wifi.ssid);
      WiFi.disconnect(/*wifioff=*/false, /*eraseap=*/false);
      // Fire and forget - the result is read from WiFi.status() on a later
      // iteration. Blocking here would freeze the display's recovery.
      WiFi.begin(cfg.wifi.ssid, cfg.wifi.password);
    }
    return;
  }
  // Mode::AP (first-run, never had a link) is left alone: there are no
  // saved credentials to retry, and begin() already chose this path. A
  // portal request is meaningless there - the portal is already up - so
  // drain it rather than let it fire on some later disconnect.
  takePortalRequest();
}

// Task 1 -> Task 2 handoff for the screen tap. Short bounded wait, and a
// miss is harmless: uiTask re-latches the request every tick until it is
// taken (DisplayManager::loop()).
bool FigoNetworkManager::takePortalRequest() {
  bool requested = false;
  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    requested = g_softApRequested;
    g_softApRequested = false;
    xSemaphoreGive(stateMutex);
  }
  return requested;
}

// Declares the disconnect to Task 1 and starts the background retry cadence.
// Deliberately does NOT touch the radio: no SoftAP, no DNS server, no mode
// change. Recovery here is mostly the driver's own auto-reconnect (which
// arduino-esp32 leaves on); serviceStaLink()'s periodic disconnect+begin is
// the backstop for when that has given up. Either way a router that reboots
// recovers on its own with nothing on screen but the "WiFi Disconnected"
// frame, and no stray "Figo-Setup" ever appears on the household's phones.
// The portal is opt-in via raisePortal().
void FigoNetworkManager::enterNoWifi() {
  Serial.println("NetworkManager: STA down past grace window - retrying in background");
  nextStaRetryAtMs_ = millis() + WIFI_STA_RETRY_INTERVAL_MS;
  setMode(Mode::NO_WIFI);
}

// Requested by a tap on the disconnect screen ("click the screen to
// configure new WiFi"). WIFI_AP_STA rather than WIFI_AP because the STA
// interface still has to exist for the user's own connect attempt to use -
// but it is parked, not retrying, for the reasons below.
void FigoNetworkManager::raisePortal() {
  // Park the STA before the AP goes up, and mean it.
  //
  // Round 2 stopped Figo's OWN retry calls while the portal was up, which was
  // necessary but nowhere near sufficient: arduino-esp32 enables STA
  // auto-reconnect by default, so the driver goes on hammering the network
  // that vanished entirely on its own. While it is mid-association esp_wifi
  // refuses every other radio request outright:
  //     E wifi:sta is connecting, return error       (our WiFi.reconnect())
  //     E wifi:sta is connecting, cannot set config  (our retry - a no-op)
  //     E wifi:sta is connecting, cannot set config  (the USER's connect)
  // That one state caused all of it: the scan returned an empty list, a good
  // network reported "Could not connect to that network" because
  // WiFi.begin() was refused rather than failing to associate, and the phone
  // needed several attempts to join Figo-Setup because a continuously
  // associating STA starves the AP's beacons in AP_STA. Nothing recovers until
  // esp_wifi's internal retry gives up on its own.
  //
  // So the portal takes the radio: auto-reconnect off, association aborted.
  // The trade is that a portal Figo is no longer trying to heal itself - a
  // tap is a deliberate "I want to change networks", and leaveNoWifi()
  // restores auto-reconnect on the way out.
  WiFi.setAutoReconnect(false);
  WiFi.disconnect(/*wifioff=*/false, /*eraseap=*/false);
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(SOFTAP_SSID, SOFTAP_PASSWORD);
  MDNS.end();
  MDNS.begin(MDNS_HOSTNAME);
  dnsServer_.start(53, "*", WiFi.softAPIP());
  portalUp_ = true;
  Serial.printf("NetworkManager: setup AP '%s' raised on request, IP=%s\n", SOFTAP_SSID,
                WiFi.softAPIP().toString().c_str());
}

void FigoNetworkManager::leaveNoWifi() {
  Serial.printf("NetworkManager: STA link back, IP=%s\n", WiFi.localIP().toString().c_str());
  if (portalUp_) {
    // Only reachable if the user asked for the portal and then the old
    // network returned by itself. Dropping it is right: they are back
    // online, and the SPA is reachable again over the real network.
    WiFi.softAPdisconnect(true);
    dnsServer_.stop();
    portalUp_ = false;
  }
  WiFi.mode(WIFI_STA);
  // Undo raisePortal()'s takeover, whether or not a portal was actually up.
  WiFi.setAutoReconnect(true);
  MDNS.end();
  MDNS.begin(MDNS_HOSTNAME);
  staDownSinceMs_ = 0;
  setMode(Mode::STA);
}

// Blocking, by design - but it is called from a REST handler, which runs on
// AsyncTCP's task, and THAT task subscribes itself to the Task Watchdog
// (AsyncTCP.h: CONFIG_ASYNC_TCP_USE_WDT defaults to 1) against a 5s
// CONFIG_ESP_TASK_WDT_TIMEOUT_S. An 18s wait here therefore panicked the
// device outright unless the watchdog was fed.
//
// That is not theoretical: it is what rebooted Figo mid-provisioning from
// the disconnect portal, confirmed from the flash coredump - the async_tcp
// task's backtrace was handleWifiConnect -> connectAndPersist ->
// waitForStaConnect. It stayed hidden until then because a SUCCESSFUL
// connect returns in 2-4s, comfortably under the 5s timeout; only a slow or
// failing one runs long enough to trip it.
//
// delay() is vTaskDelay underneath, so the rest of the system keeps running
// throughout - only AsyncTCP's own queue stalls, which is acceptable for a
// bounded provisioning wait.
//
// The subscription is checked once rather than just calling
// esp_task_wdt_reset() unconditionally: on an unsubscribed task that call
// fails AND logs at ERROR level, so fetchTask's boot-time connect
// would print "task_wdt: esp_task_wdt_reset(): task not found" twice a second
// for the whole attempt. esp_task_wdt_status() is the documented query and
// answers ESP_ERR_NOT_FOUND quietly.
bool FigoNetworkManager::waitForStaConnect(uint32_t timeoutMs) {
  const bool feedWdt = (esp_task_wdt_status(NULL) == ESP_OK);
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < timeoutMs) {
    if (feedWdt) esp_task_wdt_reset();
    delay(WIFI_STA_RETRY_DELAY_MS);
  }
  return WiFi.status() == WL_CONNECTED;
}

void FigoNetworkManager::startSoftApMode() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(SOFTAP_SSID, SOFTAP_PASSWORD);
  // Also resolvable during setup, not just once on STA - ESPmDNS binds to
  // whichever interfaces are up, and this coexists fine with the DNSServer
  // wildcard below (different port/protocol: multicast UDP/5353 vs unicast
  // UDP/53).
  MDNS.begin(MDNS_HOSTNAME);
  dnsServer_.start(53, "*", WiFi.softAPIP());
  portalUp_ = true;
  setMode(Mode::AP);
  Serial.printf("NetworkManager: SoftAP '%s' started, IP=%s\n", SOFTAP_SSID,
                WiFi.softAPIP().toString().c_str());
}

bool FigoNetworkManager::connectAndPersist(const char *ssid, const char *password, String *errorOut) {
  // Keep the existing AP alive via WIFI_AP_STA while testing the new
  // network, so a wrong password never strands the user without a way to
  // reach the device - only drop the AP after a *successful* switch.
  // softApUp() rather than == Mode::AP: the runtime NO_WIFI fallback is
  // serving the very same portal, and a connect submitted from there has to
  // take the identical keep-the-AP-alive path.
  bool hadAp = softApUp();
  Mode prevMode = mode_;
  // Runs on AsyncTCP's task; parks fetchTask's serviceStaLink() for the
  // duration so nothing else drives the radio mid-test.
  radioBusy_ = true;
  WiFi.mode(hadAp ? WIFI_AP_STA : WIFI_STA);
  // Guarantee the STA is idle before asking for a new network. esp_wifi
  // refuses WiFi.begin() with "cannot set config" while an association is in
  // flight, and this function reports that as "Could not connect to that
  // network" - blaming the user's perfectly good credentials for a radio
  // that was simply busy. Harmless when already idle.
  WiFi.disconnect(/*wifioff=*/false, /*eraseap=*/false);
  WiFi.begin(ssid, password);
  bool connected = waitForStaConnect(WIFI_STA_CONNECT_TIMEOUT_MS);

  if (!connected) {
    WiFi.disconnect(true);
    if (hadAp) {
      // Restore the AP exactly as it was. A NO_WIFI fallback keeps its STA
      // interface (WIFI_AP_STA) so serviceStaLink() can go on retrying the
      // saved network; a first-run AP has nothing to retry.
      // Restore the interface the portal was serving on. No retry deadline
      // is armed here: serviceStaLink() does not drive the radio while
      // portalUp_, precisely so a failed attempt leaves the user free to
      // scan and try again.
      WiFi.mode(prevMode == Mode::NO_WIFI ? WIFI_AP_STA : WIFI_AP);
    } else {
      // Was already pure STA on a working network - reconnect to the
      // still-persisted old credentials rather than leaving the device
      // offline because of a failed switch attempt.
      RuntimeConfig cfg = configManager_.getSnapshot();
      WiFi.mode(WIFI_STA);
      WiFi.begin(cfg.wifi.ssid, cfg.wifi.password);
      waitForStaConnect(WIFI_STA_CONNECT_TIMEOUT_MS);
    }
    if (errorOut) *errorOut = "Could not connect to that network";
    radioBusy_ = false;
    return false;
  }

  configManager_.updateWifi(ssid, password);
  // begin() is the only other place that reports a successful association,
  // so without this a runtime switch or a portal provisioning completes
  // entirely silently - which made the last round of testing far harder to
  // read than it needed to be.
  Serial.printf("NetworkManager: connected to '%s', IP=%s\n", ssid,
                WiFi.localIP().toString().c_str());
  MDNS.end();
  MDNS.begin(MDNS_HOSTNAME);

  if (hadAp) {
    // Give the HTTP response time to flush back over the AP link (the
    // phone may still be on Figo-Setup right now) before it disappears -
    // loop() drops the AP and flips mode_ to STA once this deadline passes.
    apShutdownPending_ = true;
    apShutdownAtMs_ = millis() + 3000;
  } else {
    staDownSinceMs_ = 0;  // fresh link - restart the supervisor cleanly
    setMode(Mode::STA);
  }
  radioBusy_ = false;
  return true;
}

bool FigoNetworkManager::otaBusy() const { return ota_ && ota_->busy(); }

bool FigoNetworkManager::linkHealthy() const {
  return portalUp_ || (mode_ == Mode::STA && WiFi.status() == WL_CONNECTED);
}

bool FigoNetworkManager::rejectIfBusy(AsyncWebServerRequest *request) {
  if (rebootPending_) {
    request->send(503, "application/json", "{\"error\":\"Figo is restarting\"}");
    return true;
  }
  if (otaBusy()) {
    request->send(503, "application/json", "{\"error\":\"Figo is updating\"}");
    return true;
  }
  return false;
}

void FigoNetworkManager::scheduleReboot(uint32_t delayMs) {
  // Every deliberate reboot funnels through here. If the running image is
  // still on OTA probation, it must be marked valid BEFORE the restart or the
  // bootloader reads the reboot as a failed boot and reverts a good image.
  if (ota_) ota_->noteDeliberateReboot();
  rebootPending_ = true;
  rebootAtMs_ = millis() + delayMs;
}

// ---------------------------------------------------------------------------
// REST handlers

void FigoNetworkManager::handleStatus(AsyncWebServerRequest *request) {
  JsonDocument doc;
  // Three-valued as of the runtime-disconnect work: "NO_WIFI" is a SoftAP
  // raised after a working link dropped, which the SPA words differently
  // from a first-run "AP" (nothing was ever configured). Any client that
  // only knows the old two values still behaves correctly - it just falls
  // into its not-STA branch, which is what NO_WIFI is.
  doc["mode"] = (mode_ == Mode::STA) ? "STA" : (mode_ == Mode::NO_WIFI ? "NO_WIFI" : "AP");
  doc["ssid"] = (mode_ == Mode::STA) ? WiFi.SSID() : String(SOFTAP_SSID);
  doc["ip"] = (mode_ == Mode::STA) ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
  doc["mdns"] = String(MDNS_HOSTNAME) + ".local";
  // Read from FIGO_FW_ID rather than FIGO_FW_VERSION so the marker string the
  // OTA image check looks for is REFERENCED and the linker cannot drop it.
  doc["firmware"] = FIGO_FW_ID + FIGO_FW_ID_PREFIX_LEN;
  // "valid" or "pending_verify": the phone waits for "valid" before it calls an
  // update a success, because a pending image can still be reverted.
  doc["ota_state"] = ota_ ? ota_->otaStateName() : "valid";

  String out;
  serializeJson(doc, out);
  request->send(200, "application/json", out);
}

void FigoNetworkManager::handleWifiScan(AsyncWebServerRequest *request) {
  // Blocking scan (~2-4s) - runs in AsyncTCP's own task context, not
  // fetchTask, so it doesn't stall the SoftAP DNS-poll loop or Task 1.
  // Flagged as radio work for the same reason connectAndPersist() is: a
  // background WiFi.begin() landing mid-scan makes the scan return a
  // negative count and the SPA show an empty network list.
  radioBusy_ = true;
  int n = WiFi.scanNetworks();
  radioBusy_ = false;
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (int i = 0; i < n; i++) {
    JsonObject o = arr.add<JsonObject>();
    o["ssid"] = WiFi.SSID(i);
    o["rssi"] = WiFi.RSSI(i);
    o["secure"] = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
  }
  WiFi.scanDelete();

  String out;
  serializeJson(doc, out);
  request->send(200, "application/json", out);
}

void FigoNetworkManager::handleWifiConnect(AsyncWebServerRequest *request, JsonVariant &json) {
  if (rejectIfBusy(request)) return;
  JsonObject obj = json.as<JsonObject>();
  const char *ssid = obj["ssid"] | "";
  const char *password = obj["password"] | "";
  if (strlen(ssid) == 0) {
    request->send(400, "application/json", "{\"error\":\"ssid is required\"}");
    return;
  }

  String error;
  JsonDocument doc;
  if (connectAndPersist(ssid, password, &error)) {
    doc["ok"] = true;
    doc["ip"] = WiFi.localIP().toString();
    doc["mdns"] = String(MDNS_HOSTNAME) + ".local";
    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
  } else {
    doc["error"] = error;
    String out;
    serializeJson(doc, out);
    request->send(400, "application/json", out);
  }
}

void FigoNetworkManager::handleWifiForget(AsyncWebServerRequest *request) {
  if (rejectIfBusy(request)) return;
  configManager_.forgetWifi();
  request->send(200, "application/json", "{\"ok\":true}");
  // wifi.configured is now false, so the next boot goes straight to SoftAP.
  scheduleReboot(1000);
}

void FigoNetworkManager::handleGetConfig(AsyncWebServerRequest *request) {
  RuntimeConfig cfg = configManager_.getSnapshot();
  JsonDocument doc;

  JsonObject wifi = doc["wifi"].to<JsonObject>();
  wifi["ssid"] = cfg.wifi.ssid;
  wifi["configured"] = cfg.wifi.configured;
  // Password intentionally never echoed back over the REST API.

  JsonObject system = doc["system"].to<JsonObject>();
  system["brightness"] = cfg.system.brightness;
  system["sleep_start"] = cfg.system.sleep_start;
  system["sleep_end"] = cfg.system.sleep_end;
  system["utc_offset_min"] = cfg.system.utc_offset_min;
  system["rotation"] = cfg.system.rotation;

  JsonObject expr = doc["expression_source"].to<JsonObject>();
  expr["screen_id"] = cfg.expression_source.screen_id;
  expr["bull_threshold"] = cfg.expression_source.bull_threshold;
  expr["happy_threshold"] = cfg.expression_source.happy_threshold;
  expr["bear_threshold"] = cfg.expression_source.bear_threshold;

  JsonObject providers = doc["providers"].to<JsonObject>();
  providers["yahoo_host"] = cfg.providers.yahoo_host;
  providers["coingecko_host"] = cfg.providers.coingecko_host;
  providers["fng_crypto_host"] = cfg.providers.fng_crypto_host;
  providers["fng_stocks_host"] = cfg.providers.fng_stocks_host;
  // The API key is write-only, exactly like wifi.password above: the SPA is
  // told WHETHER one is set so it can label the field, never the value. Any
  // client that can GET /api/config would otherwise be able to read it back.
  providers["coingecko_key_configured"] = cfg.providers.coingecko_key[0] != '\0';

  JsonArray screens = doc["screens"].to<JsonArray>();
  for (uint8_t i = 0; i < cfg.screen_count; i++) {
    JsonObject s = screens.add<JsonObject>();
    s["id"] = cfg.screens[i].id;
    s["type"] = screenTypeToString(cfg.screens[i].type);
    s["symbol"] = cfg.screens[i].symbol;
    s["name"] = cfg.screens[i].name;
    s["interval_sec"] = cfg.screens[i].interval_sec;
    s["layout"] = screenLayoutToString(cfg.screens[i].layout);
    s["symbol2"] = cfg.screens[i].symbol2;
    s["name2"] = cfg.screens[i].name2;
  }

  String out;
  serializeJson(doc, out);
  request->send(200, "application/json", out);
}

// GET /api/screens/state
//
// The live half of the API: how fresh each screen's data is, and the face mood
// that follows from it. Adds NO fetching and no new state - everything here is
// already published to SharedState by DataFetcher (g_marketSnapshot), so this
// endpoint only exposes what the panel is rendering from anyway. That is why
// it needed no DataFetcher change at all.
//
// Runs on AsyncTCP's task, which subscribes ITSELF to the Task Watchdog
// against a 5s timeout - see the long note on waitForStaConnect() for what
// happens when a handler on this task runs long. So this one must stay exactly
// what it is: one bounded mutex take, two struct copies, serialise, send.
// Nothing here may ever touch the radio or wait on the network.
void FigoNetworkManager::handleScreensState(AsyncWebServerRequest *request) {
  RuntimeConfig cfg;
  MarketSnapshot market;
  // Both copied out under ONE critical section rather than two takes. Values
  // are matched to screens by id and the mood is computed from both halves, so
  // a pre-edit config paired with a post-edit snapshot would report a screen's
  // freshness against an instrument it no longer points at. Same pairing
  // discipline as DisplayManager::loop().
  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(200)) != pdTRUE) {
    request->send(503, "application/json", "{\"error\":\"state busy, retry\"}");
    return;
  }
  cfg = g_runtimeConfig;
  market = g_marketSnapshot;
  // The panel's own answer, published by DisplayManager's sleep tick - NOT
  // sleepingNow() recomputed here. The two differ after a touch-wake, which
  // keeps Figo up for the rest of the window: the schedule still says
  // "sleeping" while the glass is lit. Reporting the window there would have
  // the phone calling an awake device asleep, and two copies of a rule are
  // how two copies disagree (SharedState.h). Read inside the same take as the
  // config and the snapshot, so all three describe one instant.
  bool sleeping = g_sleepingEffective;
  xSemaphoreGive(stateMutex);

  uint32_t now = millis();
  JsonDocument doc;

  doc["sleeping"] = sleeping;

  JsonObject mood = doc["mood"].to<JsonObject>();
  mood["band"] = moodBandToString(moodFor(cfg.expression_source, market));
  mood["screen_id"] = cfg.expression_source.screen_id;
  mood["bull_threshold"] = cfg.expression_source.bull_threshold;
  mood["happy_threshold"] = cfg.expression_source.happy_threshold;
  mood["bear_threshold"] = cfg.expression_source.bear_threshold;
  const ScreenValue *driver = screenValueFor(market, cfg.expression_source.screen_id);
  // Emitted only when there is a real number behind the band. ABSENT means
  // "no value to quote", which the SPA must not confuse with a change of 0.0.
  if (driver && driver->last_update_ms != 0) {
    mood["change_pct"] = driver->change_pct;
  }

  JsonArray screens = doc["screens"].to<JsonArray>();
  for (uint8_t i = 0; i < cfg.screen_count; i++) {
    const ScreenConfig &sc = cfg.screens[i];
    JsonObject o = screens.add<JsonObject>();
    o["id"] = sc.id;
    const ScreenValue *v = screenValueFor(market, sc.id);
    bool fetched = (v && v->last_update_ms != 0);
    o["fetched"] = fetched;
    // Reported even when nothing has ever been fetched: a screen failing from
    // a cold start passes FETCH_STALE_AFTER_FAILURES with no last-known value
    // at all, and that is precisely the case worth showing an amber dot for.
    o["stale"] = v ? v->stale : false;
    if (!fetched) continue;
    // Age, not a timestamp. last_update_ms is millis() on a device with no RTC
    // and no NTP, so it means nothing to a phone; the subtraction happens here,
    // where both numbers come off the same clock. Unsigned arithmetic, so the
    // ~49-day millis() rollover works out on its own.
    o["age_s"] = (now - v->last_update_ms) / 1000;
    o["price"] = v->price;
    o["change_pct"] = v->change_pct;
    // The SPA's frames are a mirror of the panel, so they need the same
    // currency the panel formats against - it is per-instrument fetched data
    // the phone has no other way to know.
    o["currency"] = v->currency;
    if (sc.layout == ScreenLayout::DUAL) {
      o["price2"] = v->price2;
      o["change_pct2"] = v->change_pct2;
      o["currency2"] = v->currency2;
    }
  }

  String out;
  serializeJson(doc, out);
  request->send(200, "application/json", out);
}

void FigoNetworkManager::handlePostConfig(AsyncWebServerRequest *request, JsonVariant &json) {
  if (rejectIfBusy(request)) return;
  JsonObject obj = json.as<JsonObject>();
  String error;
  bool ok = false;
  bool matchedSection = false;

  if (obj["system"].is<JsonObject>()) {
    matchedSection = true;
    JsonObject s = obj["system"];
    // Absent utc_offset_min defaults to 0 - the SPA doesn't send this field
    // until Task 7, and those saves must keep working. Present-but-wrong-type
    // is different: ArduinoJson's operator| silently returns 0 for a string,
    // a float, or anything else that isn't an integer, which would accept a
    // malformed value as UTC+0 instead of rejecting it, so that case is
    // checked explicitly here rather than left to the default.
    JsonVariant offsetV = s["utc_offset_min"];
    if (!offsetV.isNull() && !offsetV.is<long>()) {
      error = "utc_offset_min must be an integer";
    } else {
      ok = configManager_.updateSystem(s["brightness"] | 80, s["sleep_start"] | "",
                                        s["sleep_end"] | "", s["utc_offset_min"] | 0,
                                        s["rotation"] | 0, &error);
    }
  } else if (obj["expression_source"].is<JsonObject>()) {
    matchedSection = true;
    JsonObject e = obj["expression_source"];
    ok = configManager_.updateExpressionSource((uint8_t)(e["screen_id"] | 0),
                                                e["bull_threshold"] | 0.0f,
                                                e["happy_threshold"] | 0.0f,
                                                e["bear_threshold"] | 0.0f, &error);
  } else if (obj["providers"].is<JsonObject>()) {
    matchedSection = true;
    JsonObject p = obj["providers"];
    ProvidersConfig pc = {};
    strlcpy(pc.yahoo_host, p["yahoo_host"] | PROVIDER_HOST_YAHOO, sizeof(pc.yahoo_host));
    strlcpy(pc.coingecko_host, p["coingecko_host"] | PROVIDER_HOST_COINGECKO,
            sizeof(pc.coingecko_host));
    strlcpy(pc.fng_crypto_host, p["fng_crypto_host"] | PROVIDER_HOST_FNG_CRYPTO,
            sizeof(pc.fng_crypto_host));
    strlcpy(pc.fng_stocks_host, p["fng_stocks_host"] | PROVIDER_HOST_FNG_STOCKS,
            sizeof(pc.fng_stocks_host));
    // Absent key field = "leave whatever is stored alone", which is what a
    // plain round-trip save sends (handleGetConfig never gave the SPA the
    // key to send back). Present-but-empty = "go keyless", a deliberate
    // clear. Only these two cases exist, so the SPA must omit the field
    // entirely unless the user actually edited it.
    bool keyProvided = p["coingecko_key"].is<const char *>();
    if (keyProvided) {
      strlcpy(pc.coingecko_key, p["coingecko_key"] | "", sizeof(pc.coingecko_key));
    }
    ok = configManager_.updateProviders(pc, /*keepExistingKey=*/!keyProvided, &error);
  } else if (obj["screens"].is<JsonArray>()) {
    matchedSection = true;
    JsonArray arr = obj["screens"];
    ScreenConfig screens[MAX_SCREENS] = {};
    uint8_t count = 0;
    for (JsonObject s : arr) {
      if (count >= MAX_SCREENS) break;
      ScreenConfig &sc = screens[count];
      sc.id = s["id"] | (count + 1);
      ScreenType type;
      if (!screenTypeFromString(s["type"] | "CRYPTO", type)) type = ScreenType::CRYPTO;
      sc.type = type;
      strlcpy(sc.symbol, s["symbol"] | "", sizeof(sc.symbol));
      strlcpy(sc.name, s["name"] | "", sizeof(sc.name));
      sc.interval_sec = s["interval_sec"] | SCREEN_INTERVAL_SEC_MIN;
      // Same additive defaulting as ConfigManager::loadOrDefaultScreens(), so
      // an older SPA payload without these keys still validates.
      ScreenLayout layout;
      if (!screenLayoutFromString(s["layout"] | "", layout)) layout = defaultLayoutFor(type);
      sc.layout = layout;
      strlcpy(sc.symbol2, s["symbol2"] | "", sizeof(sc.symbol2));
      strlcpy(sc.name2, s["name2"] | "", sizeof(sc.name2));
      count++;
    }
    ok = configManager_.updateScreens(screens, count, &error);
  }

  if (!matchedSection) {
    error = "body must contain exactly one of: system, expression_source, providers, screens";
  }

  if (ok) {
    request->send(200, "application/json", "{\"ok\":true}");
  } else {
    JsonDocument doc;
    doc["error"] = error;
    String out;
    serializeJson(doc, out);
    request->send(400, "application/json", out);
  }
}

void FigoNetworkManager::handleSearchPost(AsyncWebServerRequest *request, JsonVariant &json) {
  if (rejectIfBusy(request)) return;
  JsonObject obj = json.as<JsonObject>();
  const char *q = obj["q"] | "";
  if (strlen(q) < 2) {
    request->send(400, "application/json", "{\"error\":\"q must be at least 2 characters\"}");
    return;
  }
  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(200)) != pdTRUE) {
    request->send(503, "application/json", "{\"error\":\"state busy, retry\"}");
    return;
  }
  // Newest wins: whatever was pending is abandoned where it stands. fetchTask
  // compares seq before publishing, so an in-flight answer to the old query
  // cannot land on top of this one.
  g_searchJob.seq++;
  g_searchJob.kind = SearchKind::Query;
  g_searchJob.state = SearchState::Queued;
  strlcpy(g_searchJob.query, q, sizeof(g_searchJob.query));
  g_searchJob.count = 0;
  g_searchJob.hiddenCoins = 0;
  g_searchJob.checkOk = false;
  uint32_t seq = g_searchJob.seq;
  xSemaphoreGive(stateMutex);

  JsonDocument doc;
  doc["ok"] = true;
  doc["seq"] = seq;
  String out;
  serializeJson(doc, out);
  request->send(200, "application/json", out);
}

void FigoNetworkManager::handleSearchCheck(AsyncWebServerRequest *request, JsonVariant &json) {
  if (rejectIfBusy(request)) return;
  JsonObject obj = json.as<JsonObject>();
  const char *symbol = obj["symbol"] | "";
  ScreenType type;
  if (!screenTypeFromString(obj["type"] | "", type)) {
    request->send(400, "application/json", "{\"error\":\"type must be CRYPTO, STOCK or COMMODITY\"}");
    return;
  }
  if (type == ScreenType::FEAR_GREED) {
    // FEAR_GREED has no symbol the user picks (the SPA offers no field for
    // one) and fetchScreen() routes it by a fixed "stocks"/"crypto" sentinel,
    // never through fetchYahoo/fetchCoinGecko - so it can never be routed to
    // the right fetcher here. Reject before the mutex is even taken; there is
    // no job to queue and nothing for serviceSearchJob() to do with it.
    request->send(400, "application/json", "{\"error\":\"type must be CRYPTO, STOCK or COMMODITY\"}");
    return;
  }
  if (symbol[0] == '\0') {
    request->send(400, "application/json", "{\"error\":\"symbol is required\"}");
    return;
  }
  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(200)) != pdTRUE) {
    request->send(503, "application/json", "{\"error\":\"state busy, retry\"}");
    return;
  }
  g_searchJob.seq++;
  g_searchJob.kind = SearchKind::Check;
  g_searchJob.state = SearchState::Queued;
  g_searchJob.checkType = type;
  strlcpy(g_searchJob.checkSymbol, symbol, sizeof(g_searchJob.checkSymbol));
  g_searchJob.checkOk = false;
  g_searchJob.count = 0;
  // Mirror of handleSearchPost()'s reset: these are the Query-kind fields,
  // and handleSearchResult() writes both unconditionally (any kind), so a
  // check queued after a search would otherwise report the previous query's
  // hidden-coin count and query string alongside an unrelated check result.
  g_searchJob.hiddenCoins = 0;
  g_searchJob.query[0] = '\0';
  uint32_t seq = g_searchJob.seq;
  xSemaphoreGive(stateMutex);

  JsonDocument doc;
  doc["ok"] = true;
  doc["seq"] = seq;
  String out;
  serializeJson(doc, out);
  request->send(200, "application/json", out);
}

void FigoNetworkManager::handleSearchResult(AsyncWebServerRequest *request) {
  SearchJob job;
  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(200)) != pdTRUE) {
    request->send(503, "application/json", "{\"error\":\"state busy, retry\"}");
    return;
  }
  job = g_searchJob;
  xSemaphoreGive(stateMutex);

  JsonDocument doc;
  doc["seq"] = job.seq;
  doc["kind"] = (job.kind == SearchKind::Check) ? "check" : "query";
  doc["state"] = searchStateToString(job.state);
  doc["q"] = job.query;
  doc["hidden_coins"] = job.hiddenCoins;
  JsonArray rows = doc["results"].to<JsonArray>();
  for (uint8_t i = 0; i < job.count; i++) {
    JsonObject o = rows.add<JsonObject>();
    o["symbol"] = job.rows[i].symbol;
    o["name"] = job.rows[i].name;
    o["detail"] = job.rows[i].detail;
    o["source"] = screenTypeToString(job.rows[i].source);
  }
  if (job.kind == SearchKind::Check) {
    doc["symbol"] = job.checkSymbol;
    doc["check_ok"] = job.checkOk;
    if (job.checkOk) {
      doc["price"] = job.price;
      doc["change_pct"] = job.change_pct;
      doc["currency"] = job.currency;
    }
  }
  String out;
  serializeJson(doc, out);
  request->send(200, "application/json", out);
}

void FigoNetworkManager::handleReboot(AsyncWebServerRequest *request) {
  if (otaBusy()) {
    request->send(503, "application/json", "{\"error\":\"Figo is updating\"}");
    return;
  }
  request->send(200, "application/json", "{\"ok\":true}");
  scheduleReboot(1000);
}

// NVS only - the SPA is compiled into the firmware (WebAssets.h), so a factory reset never touches it.
void FigoNetworkManager::handleFactoryReset(AsyncWebServerRequest *request) {
  // Idempotent. Once the reboot is armed the wipe has happened; re-arming
  // would push rebootAtMs_ another second out on every repeat, so a client
  // retrying in a loop could keep the device from ever restarting.
  if (otaBusy()) {
    request->send(503, "application/json", "{\"error\":\"Figo is updating\"}");
    return;
  }
  // eraseRadioOnReboot_ is set only by this handler, so it is what says "the
  // wipe already happened". A bare rebootPending_ is not enough: a reboot armed
  // by an OTA update or /api/reboot has erased nothing, and answering ok to a
  // reset that never ran is the wrong kind of idempotent.
  if (rebootPending_) {
    if (eraseRadioOnReboot_) {
      request->send(200, "application/json", "{\"ok\":true}");
    } else {
      request->send(503, "application/json", "{\"error\":\"Figo is restarting\"}");
    }
    return;
  }
  configManager_.factoryReset();
  eraseRadioOnReboot_ = true;
  request->send(200, "application/json", "{\"ok\":true}");
  scheduleReboot(1000);  // same reason handleWifiForget() uses it: AsyncTCP
                         // must flush the response before the link dies
}

// ---------------------------------------------------------------------------

// Serves one embedded gzip asset. The bytes live in flash (WebAssets.h) and are
// streamed from there - no RAM copy. no-cache + ETag rather than max-age: the
// phone must notice a new page right after an OTA update, and a 304 costs one
// round trip and no body.
static void sendGzAsset(AsyncWebServerRequest *request, const char *mime, const uint8_t *data,
                        size_t len) {
  const AsyncWebHeader *inm = request->getHeader("If-None-Match");
  AsyncWebServerResponse *resp;
  if (inm && inm->value() == WEB_ASSETS_ETAG) {
    resp = request->beginResponse(304);
  } else {
    resp = request->beginResponse(200, mime, data, len);
    resp->addHeader("Content-Encoding", "gzip");
  }
  resp->addHeader("ETag", WEB_ASSETS_ETAG);
  resp->addHeader("Cache-Control", "no-cache");
  request->send(resp);
}

void FigoNetworkManager::registerRoutes() {
  server_.on("/api/status", HTTP_GET, [this](AsyncWebServerRequest *r) { handleStatus(r); });
  server_.on("/api/wifi/scan", HTTP_GET, [this](AsyncWebServerRequest *r) { handleWifiScan(r); });
  server_.on("/api/wifi/forget", HTTP_POST, [this](AsyncWebServerRequest *r) { handleWifiForget(r); });
  server_.on("/api/config", HTTP_GET, [this](AsyncWebServerRequest *r) { handleGetConfig(r); });
  server_.on("/api/screens/state", HTTP_GET,
             [this](AsyncWebServerRequest *r) { handleScreensState(r); });
  server_.on("/api/search/result", HTTP_GET,
             [this](AsyncWebServerRequest *r) { handleSearchResult(r); });
  server_.on("/api/reboot", HTTP_POST, [this](AsyncWebServerRequest *r) { handleReboot(r); });
  server_.on("/api/factory-reset", HTTP_POST,
             [this](AsyncWebServerRequest *r) { handleFactoryReset(r); });

  // OTA. exact() matchers, like the search routes: the default string matcher
  // also claims sub-paths. The body callback receives the raw octet-stream in
  // chunks; the request callback runs once it has all arrived.
  server_.on(AsyncURIMatcher::exact("/api/ota/begin"), HTTP_POST, [this](AsyncWebServerRequest *r) {
    if (ota_) ota_->handleBegin(r); else r->send(404);
  });
  server_.on(AsyncURIMatcher::exact("/api/ota/status"), HTTP_GET, [this](AsyncWebServerRequest *r) {
    if (ota_) ota_->handleStatus(r); else r->send(404);
  });
  server_.on(
      AsyncURIMatcher::exact("/api/ota/upload"), HTTP_POST,
      [this](AsyncWebServerRequest *r) {
        if (ota_) ota_->handleDone(r); else r->send(404);
      },
      nullptr,
      [this](AsyncWebServerRequest *r, uint8_t *data, size_t len, size_t index, size_t total) {
        if (ota_) ota_->handleBody(r, data, len, index, total);
      });

  auto *wifiConnectHandler = new AsyncCallbackJsonWebHandler(
      "/api/wifi/connect",
      [this](AsyncWebServerRequest *r, JsonVariant &json) { handleWifiConnect(r, json); });
  server_.addHandler(wifiConnectHandler);

  auto *configHandler = new AsyncCallbackJsonWebHandler(
      "/api/config", [this](AsyncWebServerRequest *r, JsonVariant &json) { handlePostConfig(r, json); });
  server_.addHandler(configHandler);

  // History: a plain string ctor gives AsyncCallbackJsonWebHandler an
  // AsyncURIMatcher of the default BackwardCompatible type, matching
  // "^{uri}(/.*)?$" - so a "/api/search" handler also caught POSTs to
  // "/api/search/check" as a sub-path, and _attachHandler() (WebServer.cpp)
  // stops at the first canHandle() == true in registration order. Task 4
  // first fixed this by registering the check route before the query route,
  // which worked but left correctness resting on registration order - a
  // later regrouping of these addHandler() calls could silently reintroduce
  // the shadowing with no error to notice it by. AsyncURIMatcher::exact()
  // (matching "^{uri}$" only) removes the coupling instead: both handlers
  // below are exact-matched, so they can no longer shadow one another or
  // "/api/search/result", and registration order is no longer load-bearing
  // for this pair.
  //
  // A second instance of the same shape: the JSON
  // handler's default method composite includes HTTP_GET (AsyncJson.h), so
  // canHandle() also returns true for GET requests on a matching URI. With
  // the BackwardCompatible matcher, "/api/search" would therefore also
  // match "GET /api/search/result" - saved only by that route's own
  // server_.on() registration appearing earlier in this function. exact()
  // closes that too: "^/api/search$" cannot match "/api/search/result".
  auto *searchCheckHandler = new AsyncCallbackJsonWebHandler(
      AsyncURIMatcher::exact("/api/search/check"),
      [this](AsyncWebServerRequest *r, JsonVariant &json) { handleSearchCheck(r, json); });
  server_.addHandler(searchCheckHandler);

  auto *searchHandler = new AsyncCallbackJsonWebHandler(
      AsyncURIMatcher::exact("/api/search"),
      [this](AsyncWebServerRequest *r, JsonVariant &json) { handleSearchPost(r, json); });
  server_.addHandler(searchHandler);

  // The SPA is compiled into the firmware (WebAssets.h, generated by
  // tools/embed-spa/embed.mjs), so an OTA update replaces the page together
  // with the code that talks to it. Exact matchers: the default string
  // matcher also claims sub-paths (see the search routes above), and "/"
  // would then swallow everything.
  server_.on(AsyncURIMatcher::exact("/"), HTTP_GET, [](AsyncWebServerRequest *r) {
    sendGzAsset(r, "text/html", WEB_INDEX_HTML_GZ, WEB_INDEX_HTML_GZ_LEN);
  });
  server_.on(AsyncURIMatcher::exact("/index.html"), HTTP_GET, [](AsyncWebServerRequest *r) {
    sendGzAsset(r, "text/html", WEB_INDEX_HTML_GZ, WEB_INDEX_HTML_GZ_LEN);
  });
  // The fallback updater: standalone on purpose, so it still works when a new
  // firmware is healthy but its Update card (in app.js) is not.
  server_.on(AsyncURIMatcher::exact("/update"), HTTP_GET, [](AsyncWebServerRequest *r) {
    sendGzAsset(r, "text/html", WEB_UPDATE_HTML_GZ, WEB_UPDATE_HTML_GZ_LEN);
  });
  server_.on(AsyncURIMatcher::exact("/style.css"), HTTP_GET, [](AsyncWebServerRequest *r) {
    sendGzAsset(r, "text/css", WEB_STYLE_CSS_GZ, WEB_STYLE_CSS_GZ_LEN);
  });
  server_.on(AsyncURIMatcher::exact("/app.js"), HTTP_GET, [](AsyncWebServerRequest *r) {
    sendGzAsset(r, "application/javascript", WEB_APP_JS_GZ, WEB_APP_JS_GZ_LEN);
  });

  // Captive-portal fallback. onNotFound() is a separate fallback slot only
  // invoked when nothing above matched - it isn't part of the ordered
  // handler-matching chain, so it can never shadow a real asset/API route
  // regardless of registration order, and there's no redirect-loop risk to
  // reason about. Generic wildcard redirect, not per-OS-tuned probe
  // responses (Apple/Android/Windows all resolve fine against this).
  server_.onNotFound([](AsyncWebServerRequest *request) { request->redirect("/"); });
}
