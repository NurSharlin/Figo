#include "OtaManager.h"
#include "Config.h"
#include "NetworkManager.h"
#include "SharedState.h"
#include <Update.h>
#include <esp_heap_caps.h>
#include <esp_ota_ops.h>
#include <esp_system.h>

// ---------------------------------------------------------------------------
// Small helpers

const char *OtaManager::stateName(State s) {
  switch (s) {
    case State::IDLE: return "idle";
    case State::ARMING: return "arming";
    case State::READY: return "ready";
    case State::RECEIVING: return "receiving";
    case State::VERIFYING: return "verifying";
    case State::DONE: return "done";
    case State::FAILED: return "failed";
  }
  return "idle";
}

// Short enough for the 172px panel, and readable to a user with no serial
// console. Never spell the OtaImage marker prefix with its colon in this file:
// the marker must appear in the firmware exactly once (see OtaImage.h).
const char *OtaManager::messageFor(OtaImageResult r) {
  switch (r) {
    case OTA_IMG_OK: return "";
    case OTA_IMG_TRUNCATED: return "File is incomplete";
    case OTA_IMG_BAD_MAGIC: return "Not a firmware file";
    case OTA_IMG_WRONG_CHIP: return "Wrong chip type";
    case OTA_IMG_MERGED_BIN: return "Use Figo.ino.bin";
    case OTA_IMG_NO_MARKER: return "Not a Figo firmware";
    case OTA_IMG_BAD_VERSION: return "Unreadable version";
    case OTA_IMG_TOO_BIG: return "Firmware too large";
  }
  return "Rejected";
}

bool OtaManager::busy() const {
  // A racy read of a one-byte enum is harmless here: the only question is
  // "is an update in progress", and both sides of a race give a sane answer
  // (a write is refused for one more request, or admitted one request early
  // into a state where the update has just ended).
  State s = state_;
  return s == State::ARMING || s == State::READY || s == State::RECEIVING ||
         s == State::VERIFYING;
}

// Task 2 -> Task 1. Only the resolved answer crosses: which stage, how far, and
// a literal string. The pointer is to static storage, so it is safe to read
// after the mutex is released.
void OtaManager::publishUi(uint8_t uiState, uint8_t percent, const char *message) {
  uiState_ = uiState;
  uiPercent_ = percent;
  uiMessage_ = message;
  uiDirty_ = true;
  syncUi();
}

void OtaManager::syncUi() {
  if (!uiDirty_) return;
  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(20)) != pdTRUE) return;  // loop() retries
  g_otaUiState = (OtaUiState)uiState_;
  g_otaPercent = uiPercent_;
  g_otaMessage = uiMessage_;
  xSemaphoreGive(stateMutex);
  uiDirty_ = false;
}

void OtaManager::failLocked(const char *message) {
  Serial.printf("OTA: FAILED - %s\n", message);
  state_ = State::FAILED;
  failMessage_ = message;
  finishedAtMs_ = millis();
  publishUi((uint8_t)OtaUiState::FAILED, percent_, message);
}

void OtaManager::disarmLocked() {
  state_ = State::IDLE;
  publishUi((uint8_t)OtaUiState::IDLE, 0, "");
}

// ---------------------------------------------------------------------------
// fetchTask side

void OtaManager::begin() {
  mu_ = xSemaphoreCreateMutex();

  // Is the image we are running still on probation? A USB flash leaves
  // otadata in a state that is NOT pending-verify, so only an image that
  // arrived by OTA ever reads true here.
  const esp_partition_t *running = esp_ota_get_running_partition();
  esp_ota_img_states_t st;
  if (running && esp_ota_get_state_partition(running, &st) == ESP_OK &&
      st == ESP_OTA_IMG_PENDING_VERIFY) {
    pendingVerify_ = true;
  }
  Serial.printf("OTA: running %s at 0x%x, %s\n", running ? running->label : "?",
                running ? (unsigned)running->address : 0,
                pendingVerify_ ? "PENDING_VERIFY (on probation)" : "valid");
  lastHeartbeat_ = g_uiHeartbeat;
  lastHeartbeatChangeMs_ = millis();
}

bool OtaManager::holdFetcher() {
  xSemaphoreTake(mu_, portMAX_DELAY);
  bool hold = busyLocked();
  if (state_ == State::ARMING) {
    // The ack. See the header: reaching here proves nothing is in flight. The
    // memory check is repeated NOW, not just at begin, because begin can land
    // mid-fetch when a live TLS session is still holding its blocks.
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    Serial.printf("OTA: fetcher idle, largest free block %u B\n", (unsigned)largest);
    if (largest < OTA_MIN_LARGEST_BLOCK) {
      failLocked("Low memory - restart Figo");
    } else {
      state_ = State::READY;
      // Panel goes up now, before any byte arrives: the fetcher is already
      // stopped, so a phone that never uploads still leaves a visible reason.
      publishUi((uint8_t)OtaUiState::PREPARING, 0, "");
    }
  }
  xSemaphoreGive(mu_);
  return hold;
}

void OtaManager::loop() {
  bool doVerify = false;

  xSemaphoreTake(mu_, portMAX_DELAY);
  syncUi();
  uint32_t now = millis();
  switch (state_) {
    case State::ARMING:
    case State::READY:
      if (now - armedAtMs_ > OTA_ARM_TIMEOUT_MS) {
        Serial.println("OTA: nobody uploaded within the arm window, disarming");
        disarmLocked();
      }
      break;
    case State::RECEIVING:
      if (now - lastByteMs_ > OTA_STALL_TIMEOUT_MS) {
        Update.abort();
        failLocked("Upload stalled");
      }
      break;
    case State::VERIFYING:
      doVerify = true;
      break;
    case State::FAILED:
      // The panel keeps the reason up for a while, then goes back to the
      // carousel. The state itself stays FAILED so /api/ota/status can still
      // report why until the next begin.
      if (uiState_ == (uint8_t)OtaUiState::FAILED && now - finishedAtMs_ > OTA_FAIL_SHOW_MS) {
        publishUi((uint8_t)OtaUiState::IDLE, 0, "");
      }
      break;
    default:
      break;
  }
  xSemaphoreGive(mu_);

  if (doVerify) {
    // Update.end(true) hashes the whole ~2MB slot and sets the boot
    // partition. Too long for the AsyncTCP task (a handler blocked past ~5s
    // panics), so it runs here - and WITHOUT mu_, so a status poll during the
    // second or two it takes is answered instead of stalling AsyncTCP. Nothing
    // else touches Update in this state: chunks are dropped, and the stall
    // check only looks at RECEIVING.
    Serial.println("OTA: all bytes in, verifying image");
    bool ok = Update.end(true);
    xSemaphoreTake(mu_, portMAX_DELAY);
    if (ok) {
      state_ = State::DONE;
      finishedAtMs_ = millis();
      publishUi((uint8_t)OtaUiState::DONE, 100, "Restarting");
      Serial.printf("OTA: image verified (%s), rebooting into it\n", version_);
      xSemaphoreGive(mu_);
      // Through scheduleReboot() and not a private flag: the write guard keys on
      // rebootPending_, and a config POST landing between here and the restart
      // would otherwise be written by the OLD firmware and read by the new one.
      net_.scheduleReboot(1500);
    } else {
      Serial.printf("OTA: Update.end failed: %s\n", Update.errorString());
      Update.abort();
      failLocked("Image verify failed");
      xSemaphoreGive(mu_);
    }
  }

  healthCheck();
}

// ---------------------------------------------------------------------------
// Rollback
//
// CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE is on in arduino-esp32 3.3.11's
// precompiled libs, so a freshly OTA'd image boots PENDING_VERIFY and the
// bootloader reverts it on the next reset unless it is marked valid. Figo.ino
// overrides verifyRollbackLater() to keep the core from marking it valid at
// boot; this is what decides when it actually is.

// uiTask bumps g_uiHeartbeat every loop (5ms). "Alive" means it moved recently,
// i.e. the UI task is scheduling and not wedged.
bool OtaManager::uiAlive() const {
  return (uint32_t)(millis() - lastHeartbeatChangeMs_) < 2000;
}

void OtaManager::healthCheck() {
  uint32_t hb = g_uiHeartbeat;
  uint32_t now = millis();
  if (hb != lastHeartbeat_) {
    lastHeartbeat_ = hb;
    lastHeartbeatChangeMs_ = now;
  }
  if (!pendingVerify_) return;

  // Healthy = up long enough to have crashed if it was going to, the UI task
  // running, the web server started, and a link the phone can reach (joined
  // WiFi OR the setup AP). Internet and fetch success are deliberately NOT
  // required: a home-WiFi outage right after an update must not roll back a
  // good image.
  bool healthy = now >= OTA_HEALTHY_AFTER_MS && uiAlive() && net_.serverUp() && net_.linkHealthy();
  if (healthy) {
    esp_err_t e = esp_ota_mark_app_valid_cancel_rollback();
    Serial.printf("OTA: healthy after %lu ms, marking image valid (%s)\n", (unsigned long)now,
                  esp_err_to_name(e));
    if (e == ESP_OK) pendingVerify_ = false;
  } else if (now >= OTA_VERIFY_DEADLINE_MS) {
    Serial.printf("OTA: NOT healthy after %lu ms (ui=%d server=%d link=%d), rolling back\n",
                  (unsigned long)now, (int)uiAlive(), (int)net_.serverUp(), (int)net_.linkHealthy());
    esp_ota_mark_app_invalid_rollback_and_reboot();  // does not return on success
  }
}

void OtaManager::noteDeliberateReboot() {
  if (!pendingVerify_) return;
  // Only if it has shown it is alive. A deliberate reboot must not become a
  // way to launder a broken image into "valid".
  if (uiAlive() && net_.serverUp()) {
    esp_err_t e = esp_ota_mark_app_valid_cancel_rollback();
    Serial.printf("OTA: deliberate reboot while on probation, marking valid (%s)\n",
                  esp_err_to_name(e));
    if (e == ESP_OK) pendingVerify_ = false;
  }
}

// ---------------------------------------------------------------------------
// HTTP surface (AsyncTCP task)

// Does this request carry the live session's stamp? Call with mu_ held.
// (A request that was rejected at chunk 0 never got one: _tempObject is null.)
bool OtaManager::isMine(AsyncWebServerRequest *request) const {
  return request->_tempObject && *(const uint32_t *)request->_tempObject == session_;
}

static void sendJsonError(AsyncWebServerRequest *request, int code, const char *msg) {
  char buf[160];
  snprintf(buf, sizeof(buf), "{\"error\":\"%s\"}", msg);
  request->send(code, "application/json", buf);
}

void OtaManager::handleBegin(AsyncWebServerRequest *request) {
  xSemaphoreTake(mu_, portMAX_DELAY);
  if (net_.rebooting()) {
    xSemaphoreGive(mu_);
    sendJsonError(request, 503, "Figo is restarting");
    return;
  }
  if (pendingVerify_) {
    // Updating again before the last image has been marked valid would leave
    // the bootloader nothing valid to fall back to.
    xSemaphoreGive(mu_);
    sendJsonError(request, 503, "Figo is still checking its new firmware - try again in a minute");
    return;
  }
  if (busyLocked()) {
    xSemaphoreGive(mu_);
    sendJsonError(request, 409, "An update is already in progress");
    return;
  }
  size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  if (largest < OTA_MIN_LARGEST_BLOCK) {
    Serial.printf("OTA: refusing begin, largest free block %u B\n", (unsigned)largest);
    xSemaphoreGive(mu_);
    sendJsonError(request, 507, "Not enough free memory - restart Figo, then update");
    return;
  }
  if (!esp_ota_get_next_update_partition(nullptr)) {
    xSemaphoreGive(mu_);
    sendJsonError(request, 500, "No update slot on this board");
    return;
  }

  snprintf(token_, sizeof(token_), "%08x", (unsigned)esp_random());
  session_++;
  state_ = State::ARMING;
  armedAtMs_ = millis();
  percent_ = 0;
  received_ = 0;
  total_ = 0;
  failMessage_ = "";
  version_[0] = 0;
  Serial.printf("OTA: armed (session %u), waiting for the fetcher to go idle\n", (unsigned)session_);

  char buf[48];
  snprintf(buf, sizeof(buf), "{\"token\":\"%s\"}", token_);
  xSemaphoreGive(mu_);
  request->send(200, "application/json", buf);
}

void OtaManager::handleStatus(AsyncWebServerRequest *request) {
  xSemaphoreTake(mu_, portMAX_DELAY);
  char buf[192];
  snprintf(buf, sizeof(buf), "{\"state\":\"%s\",\"percent\":%u,\"error\":\"%s\"}", stateName(state_),
           (unsigned)percent_, state_ == State::FAILED ? failMessage_ : "");
  xSemaphoreGive(mu_);
  request->send(200, "application/json", buf);
}

void OtaManager::handleBody(AsyncWebServerRequest *request, uint8_t *data, size_t len,
                            size_t index, size_t total) {
  xSemaphoreTake(mu_, portMAX_DELAY);

  if (index == 0) {
    const AsyncWebParameter *tok = request->getParam("token");
    if (state_ != State::READY || !tok || strcmp(tok->value().c_str(), token_) != 0) {
      // Not armed, or not this session's token. Leave _tempObject unset so
      // every later chunk of this request is dropped, and handleDone answers
      // 409 - without touching the state of whichever session IS live.
      xSemaphoreGive(mu_);
      return;
    }
    const esp_partition_t *slot = esp_ota_get_next_update_partition(nullptr);
    check_.begin(slot ? (uint32_t)slot->size : 0);
    total_ = total;
    received_ = 0;
    percent_ = 0;
    // Stamp this request as the live session before anything can fail, so
    // handleDone reports the failure to the request that caused it.
    //
    // _tempObject MUST be a heap allocation: ~AsyncWebServerRequest() calls
    // free() on it. Storing the session number in the pointer itself would crash the
    // device with free(1) the moment the request ended.
    uint32_t *stamp = (uint32_t *)malloc(sizeof(uint32_t));
    if (!stamp) {
      failLocked("Out of memory");
      xSemaphoreGive(mu_);
      return;
    }
    *stamp = session_;
    request->_tempObject = stamp;
    state_ = State::RECEIVING;
    lastByteMs_ = millis();
    uint32_t mySession = session_;
    // The phone walking away (WiFi hop, app backgrounded) is the realistic
    // failure. Abort at once rather than waiting for the stall timer, so the
    // fetcher and the panel come back quickly.
    request->onDisconnect([this, mySession]() {
      xSemaphoreTake(mu_, portMAX_DELAY);
      if (state_ == State::RECEIVING && session_ == mySession) {
        Update.abort();
        failLocked("Connection lost");
      }
      xSemaphoreGive(mu_);
    });
    // Begun here, not in begin(): the size is only known once the body starts.
    if (!Update.begin(total, U_FLASH)) {
      Serial.printf("OTA: Update.begin(%u) failed: %s\n", (unsigned)total, Update.errorString());
      failLocked(slot && total > slot->size ? messageFor(OTA_IMG_TOO_BIG)
                                             : "Could not start the update");
      xSemaphoreGive(mu_);
      return;
    }
    Serial.printf("OTA: receiving %u bytes\n", (unsigned)total);
  }

  // A chunk of some other request, or of a session that already failed (the
  // phone keeps sending after the device gives up): drop it.
  if (!isMine(request) || state_ != State::RECEIVING) {
    xSemaphoreGive(mu_);
    return;
  }

  lastByteMs_ = millis();

  OtaImageResult r = check_.feed(data, len);
  if (r != OTA_IMG_OK) {
    Update.abort();
    failLocked(messageFor(r));
    xSemaphoreGive(mu_);
    return;
  }
  if (Update.write(data, len) != len) {
    Serial.printf("OTA: write failed at %u: %s\n", (unsigned)received_, Update.errorString());
    Update.abort();
    failLocked("Flash write failed");
    xSemaphoreGive(mu_);
    return;
  }
  received_ += len;

  uint8_t pct = total_ ? (uint8_t)((uint64_t)received_ * 100 / total_) : 0;
  if (pct != percent_) {
    percent_ = pct;
    publishUi((uint8_t)OtaUiState::RECEIVING, pct, "");
  }

  if (index + len == total) {
    r = check_.finish();
    if (r != OTA_IMG_OK) {
      Update.abort();
      failLocked(messageFor(r));
    } else {
      strncpy(version_, check_.version(), sizeof(version_) - 1);
      version_[sizeof(version_) - 1] = 0;
      state_ = State::VERIFYING;
      percent_ = 100;
      publishUi((uint8_t)OtaUiState::VERIFYING, 100, "");
    }
  }
  xSemaphoreGive(mu_);
}

void OtaManager::handleDone(AsyncWebServerRequest *request) {
  xSemaphoreTake(mu_, portMAX_DELAY);
  bool mine = isMine(request);
  State s = state_;
  char msg[96];
  snprintf(msg, sizeof(msg), "%s", failMessage_);
  xSemaphoreGive(mu_);

  if (!mine) {
    sendJsonError(request, 409, "Not armed - start the update again");
  } else if (s == State::FAILED) {
    sendJsonError(request, 422, msg);
  } else {
    // Everything arrived. The verify runs on fetchTask, so this is not yet a
    // success: the phone polls /api/ota/status until "done" or "failed".
    request->send(200, "application/json", "{\"ok\":true}");
  }
}
