#include "ConfigManager.h"
#include "Config.h"
#include "ErrorCodes.h"
#include <Preferences.h>
#include <ArduinoJson.h>

namespace {

Preferences g_prefs;

// What one NVS section held at boot. The distinction that matters is
// Absent vs Corrupt: a section that was never written (fresh board, factory
// reset - an erased namespace IS factory state) is normal and silent, while one
// that is present but will not parse means the user's settings just vanished
// and deserves a word on the panel. Both apply the same defaults; only Corrupt
// sets a bit in g_configLoadErrors, which the boot screen reads.
enum class Blob { Absent, Corrupt, Parsed };

// wantArray: `screens` is a JSON array, every other section an object. A blob
// that parses as the wrong kind (a bare number, say) is Corrupt too - the
// loaders index into it and would otherwise quietly read defaults out of it.
Blob readBlob(const char *key, JsonDocument &doc, bool wantArray) {
  String json = g_prefs.getString(key, "");
  if (json.length() == 0) return Blob::Absent;
  if (deserializeJson(doc, json) != DeserializationError::Ok) return Blob::Corrupt;
  bool shapeOk = wantArray ? doc.is<JsonArray>() : doc.is<JsonObject>();
  return shapeOk ? Blob::Parsed : Blob::Corrupt;
}

void setError(String *errorOut, const char *msg) {
  if (errorOut) *errorOut = msg;
}

// Validates one providers host field. These are hostnames only - DataFetcher
// builds "https://" + host + a fixed path per source, so a value carrying a
// scheme, a path, or whitespace would produce a malformed URL that fails at
// fetch time with a confusing error instead of here with a clear one.
bool validHost(const char *host, size_t cap, const char *fieldName, String *errorOut) {
  if (!host || host[0] == '\0') {
    if (errorOut) *errorOut = String(fieldName) + " must not be empty";
    return false;
  }
  if (strlen(host) >= cap) {
    if (errorOut) *errorOut = String(fieldName) + " is too long";
    return false;
  }
  if (strchr(host, '/') || strchr(host, ' ') || strchr(host, ':')) {
    if (errorOut) *errorOut = String(fieldName) + " must be a bare hostname (no scheme, port or path)";
    return false;
  }
  return true;
}

}  // namespace

void ConfigManager::begin() {
  g_prefs.begin(NVS_NAMESPACE, /*readOnly=*/false);
  // Written here, before either task exists, and read-only afterwards - which
  // is why g_configLoadErrors needs no mutex. See SharedState.h.
  g_configLoadErrors = 0;
  loadOrDefaultWifi();
  loadOrDefaultSystem();
  loadOrDefaultExpressionSource();
  loadOrDefaultProviders();
  loadOrDefaultScreens();
  // No bumpConfigGen() here - uiTask hasn't started reading g_configGen yet
  // at this point in setup(), and g_configGen's zero-init already means
  // "config as loaded at boot", which is exactly what ScreenManager::init()
  // (called shortly after, still in uiTask's first begin()) reads directly.
}

RuntimeConfig ConfigManager::getSnapshot() {
  RuntimeConfig snapshot = {};
  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
    snapshot = g_runtimeConfig;
    xSemaphoreGive(stateMutex);
  }
  return snapshot;
}

void ConfigManager::bumpConfigGen() {
  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
    g_configGen++;
    xSemaphoreGive(stateMutex);
  }
}

// ---------------------------------------------------------------------------
// wifi

void ConfigManager::loadOrDefaultWifi() {
  WifiConfig cfg = {};
  JsonDocument doc;
  Blob blob = readBlob(NVS_KEY_WIFI, doc, /*wantArray=*/false);
  if (blob == Blob::Corrupt) g_configLoadErrors |= CONFIG_ERR_WIFI;
  if (blob == Blob::Parsed) {
    strlcpy(cfg.ssid, doc["ssid"] | "", sizeof(cfg.ssid));
    strlcpy(cfg.password, doc["password"] | "", sizeof(cfg.password));
    cfg.configured = doc["configured"] | false;
  } else {
    cfg.ssid[0] = '\0';
    cfg.password[0] = '\0';
    cfg.configured = false;
  }
  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
    g_runtimeConfig.wifi = cfg;
    xSemaphoreGive(stateMutex);
  }
}

void ConfigManager::persistWifi(const WifiConfig &cfg) {
  JsonDocument doc;
  doc["ssid"] = cfg.ssid;
  doc["password"] = cfg.password;
  doc["configured"] = cfg.configured;
  String out;
  serializeJson(doc, out);
  g_prefs.putString(NVS_KEY_WIFI, out);
}

bool ConfigManager::updateWifi(const char *ssid, const char *password, String *errorOut) {
  if (!ssid || strlen(ssid) == 0 || strlen(ssid) >= sizeof(WifiConfig::ssid)) {
    setError(errorOut, "ssid must be 1-32 characters");
    return false;
  }
  if (!password || strlen(password) >= sizeof(WifiConfig::password)) {
    setError(errorOut, "password must be at most 63 characters");
    return false;
  }

  WifiConfig cfg = {};
  strlcpy(cfg.ssid, ssid, sizeof(cfg.ssid));
  strlcpy(cfg.password, password, sizeof(cfg.password));
  cfg.configured = true;

  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
    g_runtimeConfig.wifi = cfg;
    xSemaphoreGive(stateMutex);
  }
  bumpConfigGen();
  persistWifi(cfg);
  return true;
}

bool ConfigManager::forgetWifi() {
  WifiConfig cfg = {};
  cfg.ssid[0] = '\0';
  cfg.password[0] = '\0';
  cfg.configured = false;

  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
    g_runtimeConfig.wifi = cfg;
    xSemaphoreGive(stateMutex);
  }
  bumpConfigGen();
  persistWifi(cfg);
  return true;
}

// NVS only. Takes no mutex, so "flash writes never under stateMutex"
// holds trivially.
//
// No re-seeding and no bumpConfigGen(): loadOrDefault*() applies defaults
// without persisting them, so an erased namespace IS factory state - there is
// no second copy of the defaults to keep in sync. Bumping the generation would
// make DisplayManager::loop() rebuild the carousel and re-apply brightness in
// the second before the reboot (the user's last sight of their device would
// be a stranger's default carousel, which reads as a glitch, not a reset) and
// would make DataFetcher re-sync and start TLS work inside the window that has
// to reach ESP.restart().
bool ConfigManager::factoryReset() {
  // Key ORDER is the recovery story, not housekeeping. Preferences::remove()
  // commits per key, so a power loss mid-wipe is a real outcome. Wiping wifi
  // LAST means an interrupted reset leaves a device still on the user's
  // network and still reachable, so a second tap finishes the job. The other
  // order strands a half-wiped device in the portal looking finished, with the
  // user's screens still on it and no UI that admits it.
  // remove() results are ignored: NOT_FOUND for a section this device never
  // saved is normal, not a failure.
  g_prefs.remove(NVS_KEY_SCREENS);
  g_prefs.remove(NVS_KEY_EXPRESSION_SOURCE);
  g_prefs.remove(NVS_KEY_PROVIDERS);
  g_prefs.remove(NVS_KEY_SYSTEM);
  // clear() rather than remove(NVS_KEY_WIFI): it takes the wifi blob AND any
  // key an older firmware wrote under a name this build no longer knows.
  return g_prefs.clear();
}

// ---------------------------------------------------------------------------
// system

void ConfigManager::loadOrDefaultSystem() {
  SystemConfig cfg = {};
  JsonDocument doc;
  Blob blob = readBlob(NVS_KEY_SYSTEM, doc, /*wantArray=*/false);
  if (blob == Blob::Corrupt) g_configLoadErrors |= CONFIG_ERR_SYSTEM;
  if (blob == Blob::Parsed) {
    cfg.brightness = doc["brightness"] | 80;
    strlcpy(cfg.sleep_start, doc["sleep_start"] | "", sizeof(cfg.sleep_start));
    strlcpy(cfg.sleep_end, doc["sleep_end"] | "", sizeof(cfg.sleep_end));
    cfg.utc_offset_min = doc["utc_offset_min"] | 0;
    cfg.rotation = doc["rotation"] | 0;
  } else {
    // Defaults: brightness 80, sleep window disabled (empty = no scheduled
    // sleep), landscape 0 degrees.
    cfg.brightness = 80;
    cfg.sleep_start[0] = '\0';
    cfg.sleep_end[0] = '\0';
    cfg.utc_offset_min = 0;
    cfg.rotation = 0;
  }
  // Clamp on LOAD, not just on save. BRIGHTNESS_MIN guards updateSystem(),
  // but a blob written before that floor existed - or by an older firmware -
  // can hold anything. A stored 0 would black the panel at boot and leave no
  // way back except a phone UI the user cannot see to use.
  if (cfg.brightness < BRIGHTNESS_MIN) cfg.brightness = BRIGHTNESS_MIN;
  // The ceiling is not about overflow - applySystem()'s (pct * 255 + 50) / 100
  // promotes to int and cannot overflow. It is about the (uint8_t) cast on the
  // result: a corrupt 255 would compute 650 and truncate to 138, a DIMMER
  // panel than 100% asks for. Quiet and wrong, so clamp it.
  if (cfg.brightness > BRIGHTNESS_MAX) cfg.brightness = BRIGHTNESS_MAX;
  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
    g_runtimeConfig.system = cfg;
    xSemaphoreGive(stateMutex);
  }
}

void ConfigManager::persistSystem(const SystemConfig &cfg) {
  JsonDocument doc;
  doc["brightness"] = cfg.brightness;
  doc["sleep_start"] = cfg.sleep_start;
  doc["sleep_end"] = cfg.sleep_end;
  doc["utc_offset_min"] = cfg.utc_offset_min;
  doc["rotation"] = cfg.rotation;
  String out;
  serializeJson(doc, out);
  g_prefs.putString(NVS_KEY_SYSTEM, out);
}

bool ConfigManager::updateSystem(uint8_t brightness, const char *sleepStart, const char *sleepEnd,
                                  int32_t utcOffsetMin, uint16_t rotationDegrees,
                                  String *errorOut) {
  if (brightness > BRIGHTNESS_MAX) {
    setError(errorOut, "brightness must be 0-100 (values below 10 are raised to 10)");
    return false;
  }
  if (rotationDegrees != 0 && rotationDegrees != 180) {
    setError(errorOut, "rotation must be exactly 0 or 180 (landscape only)");
    return false;
  }
  if (!sleepStart || !sleepEnd || strlen(sleepStart) >= sizeof(SystemConfig::sleep_start) ||
      strlen(sleepEnd) >= sizeof(SystemConfig::sleep_end)) {
    setError(errorOut, "sleep_start/sleep_end must be \"HH:MM\" or empty");
    return false;
  }
  // UTC-12 .. UTC+14, the real inhabited range. Rejected rather than clamped,
  // same discipline as rotation: a value this far out is a bug at the caller,
  // and silently accepting a wrong one would show up months later as a sleep
  // window that fires at the wrong hour.
  if (utcOffsetMin < -720 || utcOffsetMin > 840) {
    setError(errorOut, "utc_offset_min must be -720..840");
    return false;
  }

  SystemConfig cfg = {};
  cfg.brightness = (brightness < BRIGHTNESS_MIN) ? BRIGHTNESS_MIN : brightness;
  strlcpy(cfg.sleep_start, sleepStart, sizeof(cfg.sleep_start));
  strlcpy(cfg.sleep_end, sleepEnd, sizeof(cfg.sleep_end));
  // Narrow only now, after the range check above has already confirmed the
  // value fits int16_t - narrowing before that check would let an
  // out-of-range value wrap into range and slip past validation.
  cfg.utc_offset_min = (int16_t)utcOffsetMin;
  cfg.rotation = rotationDegrees;

  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
    g_runtimeConfig.system = cfg;
    xSemaphoreGive(stateMutex);
  }
  bumpConfigGen();
  persistSystem(cfg);
  return true;
}

// ---------------------------------------------------------------------------
// expression_source

void ConfigManager::loadOrDefaultExpressionSource() {
  ExpressionSourceConfig cfg = {};
  JsonDocument doc;
  Blob blob = readBlob(NVS_KEY_EXPRESSION_SOURCE, doc, /*wantArray=*/false);
  if (blob == Blob::Corrupt) g_configLoadErrors |= CONFIG_ERR_EXPRESSION;
  if (blob == Blob::Parsed) {
    // Migration: a blob written before the redesign carries a `metric` string
    // and no screen_id, so this default is what every already-deployed device
    // lands on. 1 rather than 0 (nothing starred) because the old default
    // metric was "BTC_PRICE_CHANGE_24H" and screen 1 is the factory BTC
    // screen - it preserves the intent instead of silently blanking the face
    // to Neutral on every device that takes the update.
    cfg.screen_id = (uint8_t)(doc["screen_id"] | 1);

    // happy_threshold doubles as the schema marker for this section, which is
    // why there is no separate version field: persistExpressionSource() writes
    // the key unconditionally, and handlePostConfig() always supplies a value
    // (defaulting to 0.0f) - so ANY blob this firmware has written has it, and
    // a blob without it necessarily predates the four-band ladder.
    //
    // Those two cases want opposite treatment:
    //
    //   present -> the user chose these three numbers on this firmware, in a
    //              UI that showed them all. Honour them exactly.
    //   absent  -> a two-threshold blob whose bull/bear were tuned against the
    //              old three-band split. Carrying 3.0/-3.0 forward would mean
    //              the device silently ignores the four-band ladder, so adopt
    //              it wholesale.
    //
    // The adoption is idempotent and self-terminating: nothing is persisted
    // here, so it simply re-applies on every boot until the first save writes
    // happy_threshold - after which the stored values win forever, including
    // a deliberate bull_threshold of 3.0.
    // is<float>() rather than isNull() reads as a type check, but it is the
    // right presence test here: ArduinoJson's isFloat() is `type_ &
    // NumberBit`, so it is true for an integer too - and serializing 1.0f
    // writes "1", which parses back as one. Verified against 7.4.3.
    if (doc["happy_threshold"].is<float>()) {
      cfg.bull_threshold = doc["bull_threshold"] | EXPRESSION_BULL_DEFAULT;
      cfg.happy_threshold = doc["happy_threshold"] | EXPRESSION_HAPPY_DEFAULT;
      cfg.bear_threshold = doc["bear_threshold"] | EXPRESSION_BEAR_DEFAULT;
      // Belt and braces: updateExpressionSource() validates the invariant
      // before anything is persisted, so a stored set should always satisfy
      // it. This only catches a hand-edited or corrupted blob, and re-centres
      // rather than leaving a config the UI could never re-save.
      if (!(cfg.bull_threshold > cfg.happy_threshold &&
            cfg.happy_threshold > cfg.bear_threshold)) {
        cfg.happy_threshold = (cfg.bull_threshold + cfg.bear_threshold) / 2.0f;
      }
    } else {
      cfg.bull_threshold = EXPRESSION_BULL_DEFAULT;
      cfg.happy_threshold = EXPRESSION_HAPPY_DEFAULT;
      cfg.bear_threshold = EXPRESSION_BEAR_DEFAULT;
    }
  } else {
    // Screen 2 (Bitcoin, the DUAL screen). moodFor() reads the primary
    // instrument only, and crypto is the one 24/7 instrument in the default
    // set - starring the S&P or Gold would freeze the face every evening and
    // weekend, and a 5% equity-index day would never trip the crypto-tuned
    // ladder. Must track the screens[] default in loadOrDefaultScreens().
    cfg.screen_id = 2;
    cfg.bull_threshold = EXPRESSION_BULL_DEFAULT;
    cfg.happy_threshold = EXPRESSION_HAPPY_DEFAULT;
    cfg.bear_threshold = EXPRESSION_BEAR_DEFAULT;
  }
  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
    g_runtimeConfig.expression_source = cfg;
    xSemaphoreGive(stateMutex);
  }
}

void ConfigManager::persistExpressionSource(const ExpressionSourceConfig &cfg) {
  JsonDocument doc;
  doc["screen_id"] = cfg.screen_id;
  doc["bull_threshold"] = cfg.bull_threshold;
  doc["happy_threshold"] = cfg.happy_threshold;
  doc["bear_threshold"] = cfg.bear_threshold;
  String out;
  serializeJson(doc, out);
  g_prefs.putString(NVS_KEY_EXPRESSION_SOURCE, out);
}

// Bounds screenId but deliberately does NOT require the screen to EXIST.
//
// The SPA edits the star on the Screens tab, but the star lives in a different
// config section, and handlePostConfig() matches exactly one section per
// request - so saving the carousel is two sequential POSTs (screens, then
// expression_source). Requiring existence here would make this call's success
// depend on the other one having landed first, turning an ordering detail into
// a user-visible 400. A dangling id is already a defined state: moodFor()
// reads it as Unknown and the face holds Neutral.
bool ConfigManager::updateExpressionSource(uint8_t screenId, float bullThreshold,
                                            float happyThreshold, float bearThreshold,
                                            String *errorOut) {
  if (screenId > MAX_SCREENS) {
    setError(errorOut, ("screen_id must be 0 (none) or 1-" + String(MAX_SCREENS)).c_str());
    return false;
  }
  // Checked as two separate comparisons so the message names the pair that is
  // actually wrong - "bull > happy > bear" as one error tells the user which
  // rule broke but not which field to move.
  if (!(bullThreshold > happyThreshold)) {
    setError(errorOut, "bull_threshold must be greater than happy_threshold");
    return false;
  }
  if (!(happyThreshold > bearThreshold)) {
    setError(errorOut, "happy_threshold must be greater than bear_threshold");
    return false;
  }

  ExpressionSourceConfig cfg = {};
  cfg.screen_id = screenId;
  cfg.bull_threshold = bullThreshold;
  cfg.happy_threshold = happyThreshold;
  cfg.bear_threshold = bearThreshold;

  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
    g_runtimeConfig.expression_source = cfg;
    xSemaphoreGive(stateMutex);
  }
  bumpConfigGen();
  persistExpressionSource(cfg);
  return true;
}

// ---------------------------------------------------------------------------
// providers

void ConfigManager::loadOrDefaultProviders() {
  ProvidersConfig cfg = {};
  JsonDocument doc;
  // Every field falls back to its Config.h default individually rather than
  // all-or-nothing, so a blob persisted before a new provider field existed
  // still yields a complete config instead of resetting the fields it does
  // carry.
  Blob blob = readBlob(NVS_KEY_PROVIDERS, doc, /*wantArray=*/false);
  if (blob == Blob::Corrupt) g_configLoadErrors |= CONFIG_ERR_PROVIDERS;
  if (blob == Blob::Parsed) {
    strlcpy(cfg.coingecko_key, doc["coingecko_key"] | "", sizeof(cfg.coingecko_key));
    strlcpy(cfg.yahoo_host, doc["yahoo_host"] | PROVIDER_HOST_YAHOO, sizeof(cfg.yahoo_host));
    strlcpy(cfg.coingecko_host, doc["coingecko_host"] | PROVIDER_HOST_COINGECKO, sizeof(cfg.coingecko_host));
    strlcpy(cfg.fng_crypto_host, doc["fng_crypto_host"] | PROVIDER_HOST_FNG_CRYPTO, sizeof(cfg.fng_crypto_host));
    strlcpy(cfg.fng_stocks_host, doc["fng_stocks_host"] | PROVIDER_HOST_FNG_STOCKS, sizeof(cfg.fng_stocks_host));
  } else {
    cfg.coingecko_key[0] = '\0';
    strlcpy(cfg.yahoo_host, PROVIDER_HOST_YAHOO, sizeof(cfg.yahoo_host));
    strlcpy(cfg.coingecko_host, PROVIDER_HOST_COINGECKO, sizeof(cfg.coingecko_host));
    strlcpy(cfg.fng_crypto_host, PROVIDER_HOST_FNG_CRYPTO, sizeof(cfg.fng_crypto_host));
    strlcpy(cfg.fng_stocks_host, PROVIDER_HOST_FNG_STOCKS, sizeof(cfg.fng_stocks_host));
  }
  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
    g_runtimeConfig.providers = cfg;
    xSemaphoreGive(stateMutex);
  }
}

void ConfigManager::persistProviders(const ProvidersConfig &cfg) {
  JsonDocument doc;
  doc["coingecko_key"] = cfg.coingecko_key;
  doc["yahoo_host"] = cfg.yahoo_host;
  doc["coingecko_host"] = cfg.coingecko_host;
  doc["fng_crypto_host"] = cfg.fng_crypto_host;
  doc["fng_stocks_host"] = cfg.fng_stocks_host;
  String out;
  serializeJson(doc, out);
  g_prefs.putString(NVS_KEY_PROVIDERS, out);
}

bool ConfigManager::updateProviders(const ProvidersConfig &cfg, bool keepExistingKey,
                                     String *errorOut) {
  if (!validHost(cfg.yahoo_host, sizeof(cfg.yahoo_host), "yahoo_host", errorOut)) return false;
  if (!validHost(cfg.coingecko_host, sizeof(cfg.coingecko_host), "coingecko_host", errorOut)) return false;
  if (!validHost(cfg.fng_crypto_host, sizeof(cfg.fng_crypto_host), "fng_crypto_host", errorOut)) return false;
  if (!validHost(cfg.fng_stocks_host, sizeof(cfg.fng_stocks_host), "fng_stocks_host", errorOut)) return false;
  if (strlen(cfg.coingecko_key) >= sizeof(cfg.coingecko_key)) {
    setError(errorOut, "coingecko_key is too long");
    return false;
  }

  ProvidersConfig merged = cfg;
  if (keepExistingKey) {
    // Read the stored key back out rather than trusting the caller's copy -
    // the REST handler builds its ProvidersConfig from the request body,
    // which by design never contains the key.
    if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
      strlcpy(merged.coingecko_key, g_runtimeConfig.providers.coingecko_key,
              sizeof(merged.coingecko_key));
      xSemaphoreGive(stateMutex);
    }
  }

  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
    g_runtimeConfig.providers = merged;
    xSemaphoreGive(stateMutex);
  }
  bumpConfigGen();
  persistProviders(merged);
  return true;
}

// ---------------------------------------------------------------------------
// screens

void ConfigManager::loadOrDefaultScreens() {
  ScreenConfig screens[MAX_SCREENS] = {};
  uint8_t count = 0;

  JsonDocument doc;
  Blob blob = readBlob(NVS_KEY_SCREENS, doc, /*wantArray=*/true);
  if (blob == Blob::Corrupt) g_configLoadErrors |= CONFIG_ERR_SCREENS;
  if (blob == Blob::Parsed) {
    JsonArray arr = doc.as<JsonArray>();
    for (JsonObject obj : arr) {
      if (count >= MAX_SCREENS) break;
      ScreenConfig &s = screens[count];
      s.id = obj["id"] | (count + 1);
      ScreenType type;
      if (!screenTypeFromString(obj["type"] | "CRYPTO", type)) type = ScreenType::CRYPTO;
      s.type = type;
      strlcpy(s.symbol, obj["symbol"] | "", sizeof(s.symbol));
      strlcpy(s.name, obj["name"] | "", sizeof(s.name));
      s.interval_sec = obj["interval_sec"] | SCREEN_INTERVAL_SEC_MIN;
      // Additive keys: older NVS blobs simply lack them, and the | "" fallback plus defaultLayoutFor() keep those blobs
      // loading unchanged - deliberately no migration step.
      ScreenLayout layout;
      if (!screenLayoutFromString(obj["layout"] | "", layout)) layout = defaultLayoutFor(type);
      s.layout = layout;
      strlcpy(s.symbol2, obj["symbol2"] | "", sizeof(s.symbol2));
      strlcpy(s.name2, obj["name2"] | "", sizeof(s.name2));
      // Truncate rather than reject. The cap arrived after these blobs were
      // written, and a stored config that is merely too wordy must still
      // boot - it renders ellipsised at worst. Validation only rejects on
      // the way IN, where there is a user to tell.
      s.name[screenNameMaxLen(s.layout)] = '\0';
      s.name2[screenNameMaxLen(ScreenLayout::DUAL)] = '\0';
      count++;
    }
  } else {
    // Factory set: 4 of 4 screen types, 3 of 4 layouts, so a stranger sees a
    // commodity and a dual screen without building one. An erased namespace
    // IS factory state (nothing here is ever persisted), so
    // factoryReset() inherits these literals. ~1.2 TLS sessions/min total.
    screens[0] = {1, ScreenType::STOCK, "^GSPC", "S&P 500", 900,
                  ScreenLayout::SINGLE, "", ""};
    // 120s is SCREEN_INTERVAL_SEC_MIN_DUAL, the floor: two TLS sessions per
    // refresh. This is the starred screen - see loadOrDefaultExpressionSource.
    screens[1] = {2, ScreenType::CRYPTO, "bitcoin", "BTC", SCREEN_INTERVAL_SEC_MIN_DUAL,
                  ScreenLayout::DUAL, "ethereum", "ETH"};
    screens[2] = {3, ScreenType::COMMODITY, "GC=F", "Gold", 900,
                  ScreenLayout::SINGLE, "", ""};
    // FG_CIRCULAR wraps its title over two 140px lines; each WORD must stay
    // under ~6 chars. "Fear & Greed" is exactly the 12-char cap, but no word
    // is over 5. If a longer name ever looks wrong on the panel, shorten it -
    // do not widen the cap.
    screens[3] = {4, ScreenType::FEAR_GREED, "stocks", "Fear & Greed", 900,
                  ScreenLayout::FG_CIRCULAR, "", ""};
    count = 4;
  }

  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
    for (uint8_t i = 0; i < MAX_SCREENS; i++) g_runtimeConfig.screens[i] = screens[i];
    g_runtimeConfig.screen_count = count;
    xSemaphoreGive(stateMutex);
  }
}

void ConfigManager::persistScreens(const ScreenConfig *screens, uint8_t count) {
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (uint8_t i = 0; i < count; i++) {
    JsonObject obj = arr.add<JsonObject>();
    obj["id"] = screens[i].id;
    obj["type"] = screenTypeToString(screens[i].type);
    obj["symbol"] = screens[i].symbol;
    obj["name"] = screens[i].name;
    obj["interval_sec"] = screens[i].interval_sec;
    obj["layout"] = screenLayoutToString(screens[i].layout);
    obj["symbol2"] = screens[i].symbol2;
    obj["name2"] = screens[i].name2;
  }
  String out;
  serializeJson(doc, out);
  g_prefs.putString(NVS_KEY_SCREENS, out);
}

bool ConfigManager::updateScreens(const ScreenConfig *screens, uint8_t count, String *errorOut) {
  if (!screens || count == 0 || count > MAX_SCREENS) {
    setError(errorOut, "screens must contain 1-8 entries");
    return false;
  }
  for (uint8_t i = 0; i < count; i++) {
    if (strlen(screens[i].symbol) == 0 || strlen(screens[i].symbol) >= sizeof(ScreenConfig::symbol)) {
      setError(errorOut, "screen symbol must be 1-15 characters");
      return false;
    }
    // Per-layout cap, not the buffer size: a name that fits in the struct can
    // still be far too wide for its title area. Names the actual number, like
    // the interval check below - a limit you are not told is one you cannot
    // correct.
    uint8_t nameMax = screenNameMaxLen(screens[i].layout);
    if (strlen(screens[i].name) == 0 || strlen(screens[i].name) > nameMax) {
      if (errorOut) {
        *errorOut = String("screen name must be 1-") + nameMax +
                    " characters for this layout";
      }
      return false;
    }
    if (screens[i].interval_sec < SCREEN_INTERVAL_SEC_MIN) {
      // Names the actual number rather than saying "below the minimum" - the
      // SPA surfaces this string verbatim, and a limit you are not told is
      // one you cannot correct.
      if (errorOut) {
        *errorOut = String("screen interval_sec must be at least ") +
                    SCREEN_INTERVAL_SEC_MIN + " seconds";
      }
      return false;
    }
    if (!screenLayoutValidFor(screens[i].type, screens[i].layout)) {
      setError(errorOut, "screen layout is not valid for its type");
      return false;
    }
    if (screens[i].layout == ScreenLayout::DUAL) {
      if (strlen(screens[i].symbol2) == 0 || strlen(screens[i].symbol2) >= sizeof(ScreenConfig::symbol2)) {
        setError(errorOut, "dual screen second symbol must be 1-15 characters");
        return false;
      }
      uint8_t name2Max = screenNameMaxLen(ScreenLayout::DUAL);
      if (strlen(screens[i].name2) == 0 || strlen(screens[i].name2) > name2Max) {
        if (errorOut) {
          *errorOut = String("dual screen second name must be 1-") + name2Max +
                      " characters";
        }
        return false;
      }
      // Two TLS sessions per refresh - see SCREEN_INTERVAL_SEC_MIN_DUAL's
      // budget note in Config.h.
      if (screens[i].interval_sec < SCREEN_INTERVAL_SEC_MIN_DUAL) {
        if (errorOut) {
          *errorOut = String("dual screen interval_sec must be at least ") +
                      SCREEN_INTERVAL_SEC_MIN_DUAL + " seconds";
        }
        return false;
      }
    }
  }

  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
    for (uint8_t i = 0; i < count; i++) g_runtimeConfig.screens[i] = screens[i];
    g_runtimeConfig.screen_count = count;
    xSemaphoreGive(stateMutex);
  }
  bumpConfigGen();
  persistScreens(screens, count);
  return true;
}
