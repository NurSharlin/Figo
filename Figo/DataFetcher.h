#ifndef DATA_FETCHER_H
#define DATA_FETCHER_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include "ConfigManager.h"
#include "ErrorCodes.h"
#include "SharedState.h"

// The data engine. Runs entirely on Task 2 (fetchTask), alongside
// FigoNetworkManager. Never touches LVGL, DisplayManager or ScreenManager -
// its only output is g_marketSnapshot/g_marketGen, written under stateMutex,
// which Task 1 polls (see DisplayManager::loop()).
//
// Memory rules this class exists to enforce:
//   * Exactly ONE TLS connection at a time. Each WiFiClientSecure costs
//     25-30KB of heap, so loop() fetches at most one screen per call and
//     getFiltered() closes the socket on every exit path before returning.
//   * Responses are stream-parsed through DeserializationOption::Filter and
//     never buffered whole - Yahoo's chart payload is tens of KB and CNN's
//     is hundreds, either of which would fail to allocate next to the LVGL
//     buffer and mbedTLS's own buffers.
class DataFetcher {
 public:
  void begin(ConfigManager &configManager);

  // One round-robin step. Fetches AT MOST ONE screen per call, then returns,
  // so fetchTask keeps yielding to uiTask on this single-core chip and
  // FigoNetworkManager::loop() keeps getting serviced between fetches.
  void loop();

 private:
  // Per-screen scheduling state, keyed by ScreenConfig.id (NOT array
  // position - the SPA's reorder rewrites screens[] wholesale). Kept private
  // rather than in MarketSnapshot: Task 1 has no use for backoff bookkeeping
  // and every field here would only make the mutex-held copy larger.
  struct FetchState {
    uint8_t screen_id;          // 0 = unused slot
    uint32_t nextAttemptMs;     // millis() before which this screen is skipped
    uint32_t backoffMs;         // current backoff step; 0 until first failure
    uint8_t consecutiveFailures;
    // CRYPTO only: the CoinGecko coin id this screen's symbol resolved to,
    // and the symbol it was resolved FROM. Cached in RAM, never persisted -
    // a resolution is one cheap search request, whereas writing it back
    // would mean the fetcher reaching into NVS and silently rewriting what
    // the user typed in the SPA. coin_id_from is what makes the cache safe
    // across a config edit: screens carry their state by id, so a user
    // changing screen 1 from "BTC" to "ETH" must not keep bitcoin's
    // resolution. Empty coin_id means "not resolved yet".
    char coin_id[32];
    char coin_id_from[sizeof(ScreenConfig::symbol)];
    // The same cache for a DUAL screen's second symbol.
    char coin_id2[32];
    char coin_id2_from[sizeof(ScreenConfig::symbol2)];

    // Which instrument this slot's state and value belong to. Stamped by
    // loop() just before each attempt, so it is set even for a screen that
    // has never succeeded.
    // Screen ids survive an edit, so id alone cannot tell "the user renamed
    // screen 2" from "the user pointed screen 2 at a different instrument".
    // Without this, retargeting a screen from BTC to ETH kept bitcoin's
    // price on display - and kept last_update_ms with it, so the screen was
    // not due again for a full interval and the wrong price stayed up for
    // up to SCREEN_INTERVAL_SEC_MIN. syncStatesToConfig() compares these
    // against the incoming config and drops the value when they differ.
    // Zeroed (type/layout included) on a fresh slot, which never matches a
    // real screen and so forces the first fetch - the same effect as
    // last_update_ms == 0 already has.
    bool identity_valid;
    ScreenType fetched_type;
    ScreenLayout fetched_layout;
    char fetched_symbol[sizeof(ScreenConfig::symbol)];
    char fetched_symbol2[sizeof(ScreenConfig::symbol2)];
  };

  // True when `screen` still asks for exactly the instrument(s) `state`'s
  // carried-over value and backoff belong to.
  static bool identityMatches(const FetchState &state, const ScreenConfig &screen);
  // Stamps the instrument an attempt is about to be made for.
  static void rememberIdentity(FetchState &state, const ScreenConfig &screen);

  // Result of one HTTP attempt. The distinction matters for backoff: a 429
  // jumps straight to FETCH_BACKOFF_429_MS (and honours Retry-After) rather
  // than starting at FETCH_BACKOFF_MIN_MS and working up, because re-hitting
  // a rate limit risks a longer block than the one we're already in.
  enum class FetchResult : uint8_t { Ok, Failed, RateLimited };

  // The ErrorCode (ErrorCodes.h) of the most recent failure. Reset at the top
  // of fetchScreen() and set at every failure site through failWith(), so
  // loop() can read WHY a fetch failed without every fetcher's signature
  // growing an out-parameter. A plain member is safe: every fetch runs
  // synchronously on fetchTask, one at a time, and nothing else reads it.
  uint8_t lastErrorCode_ = 0;
  FetchResult failWith(ErrorCode code) {
    lastErrorCode_ = (uint8_t)code;
    return FetchResult::Failed;
  }

  bool wifiReady() const;
  // Re-syncs states_[] against the current config: drops states for screens
  // that no longer exist and adds zeroed ones for new ids, preserving
  // backoff for ids that survived the edit.
  void syncStatesToConfig(const RuntimeConfig &config);
  FetchState *stateFor(uint8_t screenId);
  // Copies values_[] into g_marketSnapshot and bumps g_marketGen, both under
  // one short stateMutex critical section.
  void publish();
  ScreenValue *valueFor(uint8_t screenId);

  // retryAfterMsOut is filled only on a 429 that carried a parseable
  // Retry-After header; loop() prefers it over its own backoff guess. Passed
  // down through every fetcher so any provider that starts rate-limiting is
  // handled the same way, not just the one we expect it from.
  // For a DUAL-layout ticker screen this makes TWO fetches back-to-back
  // (primary then symbol2), strictly sequential - getFiltered() closes each
  // socket before returning, so the one-TLS-connection rule holds; the pair
  // is all-or-nothing (out.price2 is only written when both succeeded).
  FetchResult fetchScreen(const RuntimeConfig &config, const ScreenConfig &screen,
                           FetchState &state, ScreenValue &out, uint32_t *retryAfterMsOut);
  // Symbol passed explicitly (not the whole ScreenConfig) so a DUAL screen
  // can run the same path for its second instrument.
  FetchResult fetchYahoo(const ProvidersConfig &p, const char *symbol, ScreenValue &out,
                          uint32_t *retryAfterMsOut);
  // Takes the coin-id cache slots by reference because a CRYPTO symbol may
  // have to resolve to a real coin id first, and that resolution is cached
  // (in FetchState's coin_id/coin_id2 pair) so it happens once, not per poll.
  FetchResult fetchCoinGecko(const ProvidersConfig &p, const char *symbol,
                              char (&cachedId)[32],
                              char (&cachedIdFrom)[sizeof(ScreenConfig::symbol)],
                              ScreenValue &out, uint32_t *retryAfterMsOut);
  // One simple/price call against an id we already believe in.
  // unknownIdOut distinguishes "the request worked but CoinGecko has never
  // heard of this id" (an empty {} with HTTP 200 - what a ticker like "BTC"
  // produces) from a transport/parse failure. Only the former is worth
  // spending a resolution request on.
  FetchResult coinGeckoPrice(const ProvidersConfig &p, const char *coinId, ScreenValue &out,
                              uint32_t *retryAfterMsOut, bool *unknownIdOut);
  // Maps whatever the user typed to a CoinGecko coin id via /api/v3/search.
  // On FetchResult::Ok, coinIdOut holds the id. RateLimited is propagated so
  // the caller's 429 backoff still applies to a failure during resolution.
  FetchResult resolveCoinGeckoId(const ProvidersConfig &p, const char *query, char *coinIdOut,
                                  size_t coinIdOutLen);
  FetchResult fetchFearGreedCrypto(const ProvidersConfig &p, ScreenValue &out,
                                    uint32_t *retryAfterMsOut);
  FetchResult fetchFearGreedStocks(const ProvidersConfig &p, ScreenValue &out,
                                    uint32_t *retryAfterMsOut);

  // --- Instrument search ---
  // Serviced from loop() BEFORE the price round-robin. Returns true when it
  // made provider (TLS) calls, in which case loop() returns immediately
  // instead of also running a price fetch that iteration. A query job makes
  // two calls (Yahoo, then CoinGecko), strictly one after the other with the
  // first socket closed - what matters is that never two are open at once,
  // and that the caller honours this return value so a search takes the whole
  // turn instead of running alongside a poll.
  bool serviceSearchJob();
  // Appends accepted rows to rows[]/count. Bounded by SEARCH_MAX_RESULTS.
  FetchResult searchYahoo(const ProvidersConfig &p, const char *query,
                           SearchResultRow *rows, uint8_t &count);
  // Appends coins ranked inside SEARCH_CG_RANK_MAX; hiddenOut counts the ones
  // dropped for rank, so the SPA can say the list was trimmed rather than
  // quietly showing fewer rows.
  FetchResult searchCoinGeckoRows(const ProvidersConfig &p, const char *query,
                                   SearchResultRow *rows, uint8_t &count, uint8_t &hiddenOut);
  // Publishes a state change for the job identified by seq. Returns false when
  // the job has been superseded, which is the caller's signal to stop work.
  bool publishSearchState(uint32_t seq, SearchState state);
  // Publishes the finished row set, if seq is still current.
  void publishSearchRows(uint32_t seq, const SearchResultRow *rows, uint8_t count,
                          uint8_t hiddenCoins, bool ok);
  // Publishes the outcome of a kind==Check job, if seq is still current.
  void publishCheckResult(uint32_t seq, bool ok, const ScreenValue &v);

  // The single place every hard-won HTTPS fix lives - see the block comment
  // on its definition in DataFetcher.cpp. All four fetchers go through it.
  // Takes a prepared filter document rather than a JSON string because
  // CoinGecko's filter has to be built around a runtime coin id.
  FetchResult getFiltered(const String &url, const char *const *headerNames,
                           const char *const *headerValues, size_t headerCount,
                           const JsonDocument &filter, JsonDocument &out,
                           uint32_t *retryAfterMsOut);

  ConfigManager *configManager_ = nullptr;
  FetchState states_[MAX_SCREENS] = {};
  // Authoritative copy of what has been fetched. g_marketSnapshot is a
  // publish target, not working storage: keeping the master here means a
  // config change that rebuilds the snapshot can't lose values for screens
  // that survived the edit.
  ScreenValue values_[MAX_SCREENS] = {};
  uint8_t nextIndex_ = 0;      // round-robin cursor into config.screens[]
  uint32_t lastConfigGen_ = 0;
  bool statesInitialised_ = false;
};

#endif  // DATA_FETCHER_H
