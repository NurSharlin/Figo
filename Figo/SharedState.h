#ifndef SHARED_STATE_H
#define SHARED_STATE_H

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "SleepWindow.h"

// Data owned and mutated by Task 2 (NetworkManager/DataFetcher/ConfigManager),
// read by Task 1 (DisplayManager/ScreenManager/FaceRenderer). Every
// access from either task MUST go through stateMutex - see Figo.ino for the
// take/give pattern. Task 1 only ever holds the mutex for short bounded
// reads (copy out, release immediately) - it must never render while holding it.

#define MAX_SCREENS 8

enum class ScreenType : uint8_t {
  CRYPTO,
  STOCK,
  COMMODITY,
  FEAR_GREED
};

// Mirrors FigoNetworkManager::Mode (NetworkManager.h) for Task 1's benefit -
// DisplayManager must never call into NetworkManager directly, so this is
// the only way it learns the link state. BOOTING is the default until
// FigoNetworkManager::begin() (Task 2) decides AP vs STA - task start order
// isn't guaranteed, and a saved-network STA attempt can take up to
// WIFI_STA_CONNECT_TIMEOUT_MS before that decision is made.
//
// NO_WIFI is the runtime-disconnect state: a previously good STA link
// dropped and did not come back within WIFI_STA_DROP_GRACE_MS. Task 1 shows
// the "WiFi Disconnected" screen instead of prices nobody can refresh, and
// NetworkManager keeps retrying the saved network in the background.
//
// Deliberately NO SoftAP here. Raising one automatically would put a
// "Figo-Setup" network on every phone in the room every time a router
// blipped, and the whole point of the background retry is that a transient
// drop needs no user action at all. The portal only appears when the user
// asks for it by tapping the screen - see g_softApRequested.
//
// This is NOT a one-way enum any more. It used to be documented as only
// ever advancing to STA, which is precisely why a dropped link left the
// carousel happily displaying its last fetched prices forever. STA ->
// NO_WIFI and NO_WIFI -> STA are both expected transitions now, and
// DisplayManager::loop() handles the regression.
enum class NetMode : uint8_t { BOOTING, AP, STA, NO_WIFI };

// Shared between ConfigManager (NVS JSON) and NetworkManager (REST JSON) so
// the wire/storage representation of ScreenType is defined exactly once.
inline const char *screenTypeToString(ScreenType t) {
  switch (t) {
    case ScreenType::CRYPTO: return "CRYPTO";
    case ScreenType::STOCK: return "STOCK";
    case ScreenType::COMMODITY: return "COMMODITY";
    case ScreenType::FEAR_GREED: return "FEAR_GREED";
  }
  return "CRYPTO";
}

inline bool screenTypeFromString(const char *s, ScreenType &out) {
  if (strcmp(s, "CRYPTO") == 0) { out = ScreenType::CRYPTO; return true; }
  if (strcmp(s, "STOCK") == 0) { out = ScreenType::STOCK; return true; }
  if (strcmp(s, "COMMODITY") == 0) { out = ScreenType::COMMODITY; return true; }
  if (strcmp(s, "FEAR_GREED") == 0) { out = ScreenType::FEAR_GREED; return true; }
  return false;
}

// Per-screen visual variant. Which values are legal depends
// on ScreenType: ticker types take SINGLE/DUAL, FEAR_GREED takes
// FG_SLIDER/FG_CIRCULAR - see screenLayoutValidFor(). Stored/wire strings
// deliberately omit the FG_ prefix ("SLIDER"/"CIRCULAR"): the SPA's layout
// dropdown is already scoped by the selected type.
enum class ScreenLayout : uint8_t { SINGLE, DUAL, FG_SLIDER, FG_CIRCULAR };

inline const char *screenLayoutToString(ScreenLayout l) {
  switch (l) {
    case ScreenLayout::SINGLE: return "SINGLE";
    case ScreenLayout::DUAL: return "DUAL";
    case ScreenLayout::FG_SLIDER: return "SLIDER";
    case ScreenLayout::FG_CIRCULAR: return "CIRCULAR";
  }
  return "SINGLE";
}

inline bool screenLayoutFromString(const char *s, ScreenLayout &out) {
  if (strcmp(s, "SINGLE") == 0) { out = ScreenLayout::SINGLE; return true; }
  if (strcmp(s, "DUAL") == 0) { out = ScreenLayout::DUAL; return true; }
  if (strcmp(s, "SLIDER") == 0) { out = ScreenLayout::FG_SLIDER; return true; }
  if (strcmp(s, "CIRCULAR") == 0) { out = ScreenLayout::FG_CIRCULAR; return true; }
  return false;
}

// The fallback when a stored/posted config carries no layout (older NVS
// blobs, older SPA payloads) - keeps old configs loading unchanged.
inline ScreenLayout defaultLayoutFor(ScreenType t) {
  return t == ScreenType::FEAR_GREED ? ScreenLayout::FG_SLIDER : ScreenLayout::SINGLE;
}

// Longest display name that still fits the title area of each layout.
//
// Derived, not guessed: measured against LVGL's compiled Montserrat advance
// widths at a mixed-case budget (~22.2px per character at 36px, ~14.8px at
// 24px), versus each layout's title box.
//   SINGLE  36px title in a 230px box  -> 12  ("Bitcoin Cash" is exactly 230px)
//   DUAL    24px title in a ~150px half -> 10  (two columns share the 320px)
//   FG_*    36px title in a 282px box  -> 12  (the circular one wraps this
//                                              across two 140px lines, so it
//                                              also needs each WORD under
//                                              ~6 characters to look right)
//
// This is a sanity cap on ordinary names, NOT the layout's safety guarantee -
// an all-caps name is far wider per character (a 36px 'W' is 40.6px, so 12 of
// them is 487px on a 320px panel). The builders still bound and ellipsise
// every title; this just stops normal names from ever needing that.
inline uint8_t screenNameMaxLen(ScreenLayout l) {
  return l == ScreenLayout::DUAL ? 10 : 12;
}

inline bool screenLayoutValidFor(ScreenType t, ScreenLayout l) {
  if (t == ScreenType::FEAR_GREED) {
    return l == ScreenLayout::FG_SLIDER || l == ScreenLayout::FG_CIRCULAR;
  }
  return l == ScreenLayout::SINGLE || l == ScreenLayout::DUAL;
}

struct ScreenConfig {
  uint8_t id;
  ScreenType type;
  char symbol[16];
  char name[24];
  uint16_t interval_sec;
  ScreenLayout layout;
  // Second instrument, used only when layout == DUAL (two tickers on one
  // screen). Empty otherwise. FEAR_GREED never uses these.
  char symbol2[16];
  char name2[24];
};

struct WifiConfig {
  char ssid[33];
  char password[64]; // WPA2 max 63 chars + NUL
  bool configured;
};

struct SystemConfig {
  uint8_t brightness;
  char sleep_start[6]; // "HH:MM"
  char sleep_end[6];
  // Minutes east of UTC: +180 is UTC+3, -300 is UTC-5. A plain offset, NOT a
  // timezone - there is no DST rule and no zone database on this device.
  // Supplied by the phone (the browser knows its own offset) rather than by a
  // control the user has to find, and re-sent whenever it drifts, which is
  // what makes a DST shift correct itself. Accepted error is one hour, twice
  // a year, until the SPA is next opened.
  //
  // Range -720..+840 (UTC-12..UTC+14), enforced in
  // ConfigManager::updateSystem(). Additive: an NVS blob written before this
  // field existed loads it as 0.
  int16_t utc_offset_min;
  // User-facing landscape orientation in degrees - exactly 0 or 180, never
  // portrait. This is NOT the Arduino_GFX setRotation() code
  // (which uses 0-3, where 1/3 are this panel's two landscape orientations,
  // see Config.h's panel geometry comment) - DisplayDriver::begin() maps
  // 0->1 and 180->3 internally, so this struct/the REST API only ever deal
  // in the semantic 0/180 the Web UI exposes. ConfigManager::updateSystem()
  // rejects any other value rather than clamping it. Applied to
  // DisplayDriver::begin() at next boot only: rotation is save-and-reboot, not
  // live.
  uint16_t rotation;
};

// Which screen's 24h change drives the face, and where the bands fall.
//
// The starred screen is identified by screen_id, not a free-form metric name:
// an opaque name could not be resolved to an actual fetched value. It is
// matched on ScreenConfig.id, the same key MarketSnapshot uses, which is what
// makes the choice survive the SPA's drag reorder.
struct ExpressionSourceConfig {
  // 0 = nothing starred; the face holds Neutral. A DANGLING id (a starred
  // screen the user later deleted) is treated the same way rather than
  // rejected at validation time - see moodFor() and the note on
  // ConfigManager::updateExpressionSource().
  uint8_t screen_id;
  // Three cut points, four bands. Invariant: bull > happy > bear, enforced by
  // ConfigManager::updateExpressionSource().
  //
  // Neutral has NO threshold of its own on purpose - it is whatever is left
  // between happy_threshold and bear_threshold. A fourth number would be a
  // fourth thing to keep consistent with the other three, and there is only
  // one sensible value it could ever hold.
  float bull_threshold;   // at or above -> Bullish
  float happy_threshold;  // at or above (but under bull) -> Happy
  float bear_threshold;   // at or below -> Bearish
};

// Where DataFetcher gets its data. Hosts only, not full URLs - the path
// and query shape are baked into each fetcher because the response schema
// its JSON filter expects is baked in too. A user can therefore point Figo
// at a mirror or a moved endpoint without a reflash, but
// can't point it at something whose response the parser wouldn't understand.
struct ProvidersConfig {
  // Optional CoinGecko Demo key. Empty means keyless, which still worked as
  // of 2026-08-27; CoinGecko has since begun requiring a Demo key on some
  // endpoints, so this is the escape hatch when simple/price follows.
  // Appended as &x_cg_demo_api_key= only when non-empty.
  char coingecko_key[40];
  char yahoo_host[48];
  char coingecko_host[48];
  char fng_crypto_host[48];  // alternative.me - the CRYPTO Fear & Greed index
  char fng_stocks_host[48];  // CNN - the STOCK MARKET Fear & Greed index
};

// Mirrors the NVS/Web API JSON config schema (see product plan section 7).
struct RuntimeConfig {
  WifiConfig wifi;
  SystemConfig system;
  ExpressionSourceConfig expression_source;
  ProvidersConfig providers;
  ScreenConfig screens[MAX_SCREENS];
  uint8_t screen_count;
};

// One fetched data point. Deliberately minimal: price + change_pct is
// everything the data screens render and everything the face's mood rule
// needs, which keeps the mutex-held copy of MarketSnapshot cheap.
// FEAR_GREED screens reuse price as the 0-100 score and change_pct as the
// move against the previous close; the rating word ("Extreme Fear" etc.) is
// derived from the score at render time rather than stored.
struct ScreenValue {
  // Which ScreenConfig this belongs to, matched on ScreenConfig.id rather
  // than array position. Position is NOT stable: the SPA's drag-and-drop
  // reorder rewrites the whole screens[] array (ConfigManager::updateScreens),
  // so a positional slot would silently show Bitcoin's price under Apple's
  // label the moment a user reorders. 0 means the slot is unused.
  uint8_t screen_id;
  float price;
  float change_pct;
  // ISO code the PROVIDER reported for this instrument, verbatim - "USD",
  // "EUR", "JPY", "ILA" (agorot, what Yahoo sends for .TA), "GBp" (pence,
  // for .L). Yahoo's chart meta carries it per symbol, so it is fetched data
  // rather than anything the user configures, and it lives here beside the
  // price it describes. Empty means "not known yet"; figo_theme::formatPrice
  // then appends nothing rather than guessing a currency.
  //
  // Deliberately NOT normalised to a major unit: Yahoo quotes TASE in agorot
  // and London in pence, and so do those exchanges themselves, so dividing by
  // 100 here would make the panel disagree with every other place the price
  // is checked.
  char currency[8];
  // Second instrument's values for layout == DUAL, fetched back-to-back with
  // the first (all-or-nothing: stale/last_update_ms below cover both).
  // Untouched (0) for every other layout. A DUAL screen can legitimately pair
  // two different currencies, which is why this is not one field per screen.
  float price2;
  float change_pct2;
  char currency2[8];
  // Set once a screen has failed FETCH_STALE_AFTER_FAILURES times in a row.
  // The last-known price keeps rendering (dimmed, with a marker) rather than
  // blanking - old data beats no data on a desk display.
  bool stale;
  // Why the most recent fetch failed, as an ErrorCode (ErrorCodes.h); 0 = no
  // error. Written by DataFetcher only once the screen has failed
  // FETCH_STALE_AFTER_FAILURES times - the same threshold as `stale`, so one
  // dropped poll never flashes a code - and cleared by the next success. It is
  // a rendering hint, not state Task 1 acts on: the price keeps showing.
  uint8_t error_code;
  // millis() of the last SUCCESSFUL fetch. 0 means never fetched, which
  // renders as "--" rather than a misleading 0.00 (or, with error_code set, as
  // the code itself).
  uint32_t last_update_ms;
};

struct MarketSnapshot {
  ScreenValue values[MAX_SCREENS];
};

// --- Instrument search ------------------------------------------------------
//
// Task 2 -> Task 2, across an internal boundary rather than a task one: the
// REST handlers run on AsyncTCP's task and the fetching runs on fetchTask, so
// every access goes through stateMutex exactly as a cross-task global does.
//
// ONE job slot, newest wins. That is not a simplification - it is the TLS
// budget stated as a data structure: only one search can be in flight because
// only one socket may be open, and a user who has typed another letter does not
// want the previous query's answer.
#define SEARCH_MAX_RESULTS 10
#define SEARCH_QUERY_MAX 32

enum class SearchKind : uint8_t { Query, Check };

enum class SearchState : uint8_t {
  Idle,       // nothing has ever been asked for
  Queued,     // the handler stored a request; fetchTask has not picked it up
  Yahoo,      // asking Yahoo now
  CoinGecko,  // Yahoo done, asking CoinGecko now
  Checking,   // pricing one symbol (kind == Check)
  Done,
  Failed,
};

inline const char *searchStateToString(SearchState s) {
  switch (s) {
    case SearchState::Idle: return "idle";
    case SearchState::Queued: return "queued";
    case SearchState::Yahoo: return "yahoo";
    case SearchState::CoinGecko: return "coingecko";
    case SearchState::Checking: return "checking";
    case SearchState::Done: return "done";
    case SearchState::Failed: return "failed";
  }
  return "idle";
}

struct SearchResultRow {
  char symbol[16];   // same width as ScreenConfig::symbol
  char name[48];     // provider's own name, ellipsised by the SPA if it must be
  char detail[32];   // "Stock · Tel Aviv", "Futures · COMEX", "BTC · #1"
  ScreenType source; // what the screen's type becomes if this row is picked
};

struct SearchJob {
  uint32_t seq;      // bumped on every new request; the phone matches on it
  SearchKind kind;
  SearchState state;
  char query[SEARCH_QUERY_MAX];              // kind == Query
  ScreenType checkType;                      // kind == Check
  char checkSymbol[sizeof(ScreenConfig::symbol)];
  uint8_t count;
  SearchResultRow rows[SEARCH_MAX_RESULTS];
  // How many CoinGecko hits were dropped for rank, so the SPA can say so
  // rather than silently showing a shorter list.
  uint8_t hiddenCoins;
  // kind == Check results.
  bool checkOk;
  float price;
  float change_pct;
  char currency[8];
};

// --- The face's mood rule ---------------------------------------------
//
// Deliberately a free function here rather than a method on DisplayManager:
// BOTH tasks need this answer and they must never disagree. Task 1 drives the
// actual face on the panel; Task 2's REST handler reports the same mood to the
// phone SPA, which prints the reasoning next to it. Two implementations of one
// rule is exactly how those two drift apart.
//
// The Expression enum can't be the shared vocabulary: it lives in
// FaceRenderer.h, which includes <lvgl.h>, and NetworkManager pulling that in
// would drag LVGL into AsyncTCP's translation unit. MoodBand is that
// vocabulary instead; mapping a band to an Expression stays on Task 1's side.
// Ordered coldest to warmest, so a comparison reads the way the bands do.
enum class MoodBand : uint8_t { Unknown, Bearish, Neutral, Happy, Bullish };

inline const char *moodBandToString(MoodBand b) {
  switch (b) {
    case MoodBand::Bearish: return "BEARISH";
    case MoodBand::Neutral: return "NEUTRAL";
    case MoodBand::Happy: return "HAPPY";
    case MoodBand::Bullish: return "BULLISH";
    case MoodBand::Unknown: return "UNKNOWN";
  }
  return "UNKNOWN";
}

// nullptr when no slot carries this id. screen_id 0 also marks an UNUSED slot,
// so a 0 query can never accidentally match one - which is exactly the right
// answer for "nothing starred".
inline const ScreenValue *screenValueFor(const MarketSnapshot &snap, uint8_t screenId) {
  if (screenId == 0) return nullptr;
  for (uint8_t i = 0; i < MAX_SCREENS; i++) {
    if (snap.values[i].screen_id == screenId) return &snap.values[i];
  }
  return nullptr;
}

// Unknown is not a fourth band - it is "do not claim to know": nothing is
// starred, the starred screen no longer exists, it has never fetched, or its
// value has gone stale. Callers render it as Neutral on the panel; only the
// SPA has room to say why.
//
// Stale is folded in on purpose. A face grinning at a price that stopped
// updating sixteen minutes ago is worse than a face that admits nothing is
// known, and stale is precisely the point at which DataFetcher stops
// believing the number itself (FETCH_STALE_AFTER_FAILURES).
inline MoodBand moodFor(const ExpressionSourceConfig &src, const MarketSnapshot &snap) {
  const ScreenValue *v = screenValueFor(snap, src.screen_id);
  if (!v || v->last_update_ms == 0 || v->stale) return MoodBand::Unknown;
  // Always the PRIMARY instrument's change, even on a DUAL screen: the user
  // starred one screen, and a screen's identity is its first instrument.
  // Evaluated warmest-first, which is also what makes a config that somehow
  // violates bull > happy > bear degrade gracefully rather than misreport:
  // a happy_threshold at or above bull_threshold simply never wins, and the
  // result is the old three-band behaviour.
  if (v->change_pct >= src.bull_threshold) return MoodBand::Bullish;
  if (v->change_pct >= src.happy_threshold) return MoodBand::Happy;
  if (v->change_pct <= src.bear_threshold) return MoodBand::Bearish;
  return MoodBand::Neutral;
}

// The night-scheduler rule: does the SCHEDULE say Figo should be asleep right
// now? That is a narrower question than "is Figo asleep", because a touch-wake
// keeps it up for the rest of the window - so this has exactly one caller,
// DisplayManager's sleep tick, which resolves the two into g_sleepingEffective
// (below) for everyone else. /api/screens/state reads that, not this: the
// phone and the panel must never disagree, and two copies of a rule are how
// two copies disagree.
//
// The arithmetic itself is in SleepWindow.h, which has no dependencies so it
// can be unit-tested off the device. This is a pass-through, not a second
// copy.
//
// nowUtc is passed in rather than read here, so a caller that also needs the
// timestamp for something else sees the same instant, and so the rule stays
// testable off-device. Returns false when the window is disabled OR when the
// clock has never been set.
inline bool sleepingNow(const SystemConfig &sys, time_t nowUtc) {
  return sleepWindowActive(sys.sleep_start, sys.sleep_end, sys.utc_offset_min, nowUtc);
}

extern SemaphoreHandle_t stateMutex;
extern RuntimeConfig g_runtimeConfig;
extern MarketSnapshot g_marketSnapshot;

// Bumped by ConfigManager (Task 2) on every successful update, under
// stateMutex. uiTask (Task 1) compares this against its own last-seen value
// each loop iteration to decide whether ScreenManager::rebuildFromConfig()
// needs to run, instead of diffing the full RuntimeConfig every frame.
extern uint32_t g_configGen;

// The MarketSnapshot equivalent of g_configGen: bumped by DataFetcher
// (Task 2) under stateMutex on every successful publish, compared by
// DisplayManager::loop() (Task 1) against its own last-seen value so it only
// copies the snapshot and re-renders labels when something actually changed.
extern uint32_t g_marketGen;

// Set by FigoNetworkManager::setMode() (NetworkManager.cpp) under
// stateMutex; polled by DisplayManager::loop() to decide whether boot
// screens or the face carousel should be showing (see BootScreens.h).
extern NetMode g_networkMode;

// Task 1 -> Task 2, the only signal that travels in that direction. Set by
// DisplayManager (under stateMutex) when the user taps the "WiFi
// Disconnected" screen, meaning "raise the setup AP so I can configure a
// new network"; consumed and cleared by FigoNetworkManager::loop().
// A plain latch, not a queue: repeated taps while the portal is already up
// are idempotent.
extern bool g_softApRequested;

// Task 1 -> Task 2. The panel's ACTUAL sleep state, which is not the same
// question as sleepingNow(): a touch-wake keeps Figo up for the rest of the
// window, so the schedule says "sleeping" while the glass is lit and awake.
// /api/screens/state reports this, not the window, so the phone and the panel
// never disagree about whether Figo is asleep.
//
// Published by DisplayManager's sleep tick (once a second) under stateMutex.
// The latches it is derived from stay private to DisplayManager and unmutexed
// because they are read and written only on uiTask - only this one resolved
// bool is shared, which is what keeps that true.
extern bool g_sleepingEffective;

// The single instrument-search slot. Written by NetworkManager's handlers
// (AsyncTCP task) and by DataFetcher (fetchTask), always under stateMutex.
extern SearchJob g_searchJob;

// Task 2 -> Task 1: what the OTA update screen should show (BootScreens.h). The
// stage, how far it got, and a pointer to a string LITERAL - static storage, so
// it stays valid after stateMutex is released. Written by OtaManager under
// stateMutex; uiTask copies them out under the same mutex.
enum class OtaUiState : uint8_t { IDLE, PREPARING, RECEIVING, VERIFYING, DONE, FAILED };
extern OtaUiState g_otaUiState;
extern uint8_t g_otaPercent;
extern const char *g_otaMessage;

// Bumped by uiTask every loop iteration. OtaManager watches it to tell a live
// UI task from a wedged one when deciding whether a new image is healthy.
// The one shared word that is NOT under stateMutex: uiTask is the only writer,
// a 32-bit store is atomic on this core, and taking a mutex 200 times a second
// just to publish "still running" would be the wrong cost. Readers only ever
// ask "did it change".
extern volatile uint32_t g_uiHeartbeat;

// Bit i set (CONFIG_ERR_*, ErrorCodes.h) = the NVS section for that bit was
// present at boot but would not parse, so defaults were applied. Written once by
// ConfigManager::begin() before either task is created and never again, which is
// why - unlike everything above - it has no mutex and no publish step: a
// read-only value needs neither. DisplayManager::begin() reads it to decide
// whether to show the "settings problem" screen. A section that is merely absent
// (fresh board, factory reset) does not set a bit.
extern uint32_t g_configLoadErrors;

#endif // SHARED_STATE_H
