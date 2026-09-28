#ifndef OTA_MANAGER_H
#define OTA_MANAGER_H

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include "OtaImage.h"

class FigoNetworkManager;

// Over-the-air update: the phone pushes Figo.ino.bin, the device writes it to
// the INACTIVE app slot and reboots into it. The reasons behind every
// choice are given in the comments below.
//
// THREADS. Four actors touch this object:
//   - AsyncTCP task: handleBegin / handleStatus / handleBody / handleDone, and
//     the onDisconnect callback. Handlers must stay short (a handler that
//     blocks past ~5s panics), so nothing here waits on anything slow.
//   - fetchTask: begin(), loop(), holdFetcher(). The slow work lives here:
//     the image verify (Update.end hashes ~2MB) and the rollback health check.
//   - uiTask: never calls in. It reads the g_ota* fields in SharedState.h.
//   - Any task: busy() and noteDeliberateReboot().
// All state is guarded by mu_. Lock order is mu_ THEN stateMutex, never the
// reverse, and uiTask only ever takes stateMutex, so it cannot deadlock.
//
// WHY THE UPLOAD IS SAFE. It writes only the inactive slot, and the Updater
// writes the image's 0xE9 magic byte last, so an interrupted upload leaves a
// slot that will not boot and the running firmware untouched. A complete image
// that then crashes, trips a watchdog or never becomes healthy is reverted by
// the bootloader (see the rollback section in OtaManager.cpp).
class OtaManager {
 public:
  explicit OtaManager(FigoNetworkManager &net) : net_(net) {}

  // fetchTask, BEFORE networkManager.begin(): the routes it registers can
  // call into this object as soon as the server starts, and they need mu_.
  void begin();
  // fetchTask, once per iteration: timeouts, the deferred verify, and the
  // rollback health check.
  void loop();
  // fetchTask, at the exact point dataFetcher.loop() would run. True while an
  // update owns the device, meaning "skip the fetch this iteration".
  //
  // This is also the ACK: dataFetcher.loop() is synchronous, so reaching this
  // line proves no fetch or search job is in flight. That is the whole
  // reason DataFetcher needs no change and no pause flag of its own.
  bool holdFetcher();

  // True from an accepted begin until the update ends one way or the other.
  // NetworkManager's write guard reads this so a config save, a WiFi forget or
  // a factory reset cannot reboot the device out from under an upload.
  bool busy() const;

  // True while the running image is still on probation (PENDING_VERIFY).
  bool pendingVerify() const { return pendingVerify_; }
  // "valid" or "pending_verify", for /api/status.
  const char *otaStateName() const { return pendingVerify_ ? "pending_verify" : "valid"; }

  // A deliberate reboot (WiFi save/forget, factory reset) while on probation:
  // mark the image valid first, provided it has shown it is alive. Without
  // this a user action would roll back a perfectly good image.
  void noteDeliberateReboot();

  // --- HTTP surface (AsyncTCP task) -----------------------------------------
  void handleBegin(AsyncWebServerRequest *request);
  void handleStatus(AsyncWebServerRequest *request);
  // The raw octet-stream body, chunk by chunk. index/total are byte offsets
  // into the body, exactly as ESPAsyncWebServer hands them over.
  void handleBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index,
                  size_t total);
  // Runs once the whole body has arrived (or immediately, if there was none).
  void handleDone(AsyncWebServerRequest *request);

 private:
  // VERIFYING is internal: the last byte is in, and fetchTask is running
  // Update.end() (an SHA-256 over the whole slot, too long for a handler).
  enum class State : uint8_t { IDLE, ARMING, READY, RECEIVING, VERIFYING, DONE, FAILED };

  static const char *stateName(State s);
  static const char *messageFor(OtaImageResult r);

  bool busyLocked() const {
    return state_ == State::ARMING || state_ == State::READY || state_ == State::RECEIVING ||
           state_ == State::VERIFYING;
  }
  // Call with mu_ held.
  void failLocked(const char *message);
  void disarmLocked();
  void publishUi(uint8_t uiState, uint8_t percent, const char *message);
  void syncUi();
  void healthCheck();
  bool uiAlive() const;
  bool isMine(AsyncWebServerRequest *request) const;

  FigoNetworkManager &net_;
  SemaphoreHandle_t mu_ = nullptr;

  State state_ = State::IDLE;
  char token_[9] = {0};
  // Incremented at every accepted begin. A request stamps itself with it
  // (AsyncWebServerRequest::_tempObject) at chunk 0, and every later chunk
  // must match. Without it, the tail of a FAILED upload - the phone keeps
  // sending after the device gives up - would be fed into the NEXT session.
  uint32_t session_ = 0;
  OtaImageCheck check_;
  size_t total_ = 0;
  size_t received_ = 0;
  uint32_t armedAtMs_ = 0;
  uint32_t lastByteMs_ = 0;
  uint32_t finishedAtMs_ = 0;
  const char *failMessage_ = "";
  uint8_t percent_ = 0;
  char version_[OTA_IMAGE_VERSION_MAX + 1] = {0};

  // What the panel should show, resent by syncUi() until stateMutex could be
  // taken - a dropped take must not lose a FAILED, or the panel would sit on
  // "Updating" forever.
  bool uiDirty_ = false;
  uint8_t uiState_ = 0;
  uint8_t uiPercent_ = 0;
  const char *uiMessage_ = "";

  // Rollback probation. Written on fetchTask (begin/healthCheck) and read
  // from any task, so volatile.
  volatile bool pendingVerify_ = false;
  uint32_t lastHeartbeat_ = 0;
  uint32_t lastHeartbeatChangeMs_ = 0;
};

#endif  // OTA_MANAGER_H
