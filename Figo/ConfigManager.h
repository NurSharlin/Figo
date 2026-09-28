#ifndef CONFIG_MANAGER_H
#define CONFIG_MANAGER_H

#include <Arduino.h>
#include "SharedState.h"

// NVS-backed config store - one Preferences/NVS key per RuntimeConfig
// section (Config.h's NVS_KEY_*), each a small JSON blob via ArduinoJson.
// Owns the only writes to g_runtimeConfig; treats it as the mutex-guarded
// RAM cache SharedState.h already declares, rather than keeping a private
// copy. Runs entirely on Task 2 (NetworkManager's REST handlers call these
// update*() methods); Task 1 only ever calls getSnapshot().
//
// Every update*() validates and clamps *before* touching RAM or NVS - a
// malformed REST payload must never corrupt persisted state. On validation
// failure the method returns false, writes nothing, and (if errorOut is
// non-null) explains which field was rejected so the REST handler can
// return a useful 400.
class ConfigManager {
 public:
  // Loads all five sections from NVS; any section missing or failing to
  // parse gets a hardcoded default (not persisted until the next explicit
  // update, to avoid flash writes on every fresh boot). Must be called from
  // setup(), before either FreeRTOS task starts.
  void begin();

  // Mutex-guarded copy-out - the read pattern SharedState.h documents for
  // Task 1. Safe to call from either task.
  RuntimeConfig getSnapshot();

  bool updateWifi(const char *ssid, const char *password, String *errorOut = nullptr);
  bool forgetWifi();  // clears saved credentials, sets configured=false, persists
  // Erases every NVS section (NVS only). An erased namespace
  // IS factory state, so nothing is re-seeded. Caller reboots.
  bool factoryReset();

  // utcOffsetMin is int32_t, wider than SystemConfig::utc_offset_min (int16_t),
  // so the range check below sees the value a client actually sent instead of
  // one already wrapped by a narrowing cast at the call site.
  bool updateSystem(uint8_t brightness, const char *sleepStart, const char *sleepEnd,
                     int32_t utcOffsetMin, uint16_t rotationDegrees,
                     String *errorOut = nullptr);
  // screenId is the ScreenConfig.id whose 24h change drives the face; 0 means
  // nothing is starred. Deliberately does NOT check that the screen exists -
  // see the note on the definition.
  bool updateExpressionSource(uint8_t screenId, float bullThreshold, float happyThreshold,
                               float bearThreshold, String *errorOut = nullptr);
  // Takes the whole ProvidersConfig rather than a parameter per field - it's
  // five strings and growing, and the REST handler already has it in struct
  // form. keepExistingKey preserves the stored coingecko_key and ignores
  // cfg.coingecko_key entirely: handleGetConfig() never echoes the key back
  // (write-only, so it can't leak to anything that can GET /api/config), so
  // a round-tripped SPA save has no key to send and must not blank it. The
  // SPA sets it false only when the user actually typed a new key, or
  // deliberately cleared the field to go keyless.
  bool updateProviders(const ProvidersConfig &cfg, bool keepExistingKey,
                        String *errorOut = nullptr);
  // screens/count describe the full desired list (REST payload replaces the
  // whole array, matching how the SPA's drag-and-drop reorder round-trips
  // the full list on every save - there's no partial single-screen PATCH).
  bool updateScreens(const ScreenConfig *screens, uint8_t count, String *errorOut = nullptr);

 private:
  void loadOrDefaultWifi();
  void loadOrDefaultSystem();
  void loadOrDefaultExpressionSource();
  void loadOrDefaultProviders();
  void loadOrDefaultScreens();

  void persistWifi(const WifiConfig &cfg);
  void persistSystem(const SystemConfig &cfg);
  void persistExpressionSource(const ExpressionSourceConfig &cfg);
  void persistProviders(const ProvidersConfig &cfg);
  void persistScreens(const ScreenConfig *screens, uint8_t count);

  // Takes stateMutex just long enough to bump g_configGen - called after
  // each update*() has already written its section into g_runtimeConfig
  // under its own short mutex critical section. Kept separate from the NVS
  // persist step below: flash writes must never happen while stateMutex is
  // held, since that would stall Task 1's snapshot reads.
  void bumpConfigGen();
};

#endif  // CONFIG_MANAGER_H
