#include "DataFetcher.h"
#include "Config.h"
#include "SearchFilter.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ctype.h>     // tolower() - toLowerAscii()
#include <strings.h>   // strcasecmp() - resolveCoinGeckoId()

namespace {

// Percent-encodes the characters Yahoo symbols actually contain: '^' for
// indices (^GSPC) and '=' for futures (CL=F). Kept deliberately narrow rather
// than a general encoder - these are the only two that appear in ticker
// symbols, and a full RFC 3986 encoder would be dead weight here.
String urlEncodeSymbol(const char *symbol) {
  String out;
  for (const char *p = symbol; *p; ++p) {
    char c = *p;
    if (c == '^') {
      out += "%5E";
    } else if (c == '=') {
      out += "%3D";
    } else if (c == ' ') {
      out += "%20";
    } else {
      out += c;
    }
  }
  return out;
}

// A search QUERY is not a symbol: it can contain spaces, '&' ("s&p 500"), '+'
// and '#'. urlEncodeSymbol() deliberately handles only the three characters
// ticker symbols contain, so a query needs its own encoder - without it,
// "s&p 500" truncates the query string at the ampersand and Yahoo answers for
// "s".
String urlEncodeQuery(const char *query) {
  String out;
  for (const char *p = query; *p; ++p) {
    unsigned char c = (unsigned char)*p;
    bool unreserved = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                      (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
    if (unreserved) {
      out += (char)c;
    } else {
      char esc[4];
      snprintf(esc, sizeof(esc), "%%%02X", c);
      out += esc;
    }
  }
  return out;
}

// Yahoo's own words, shortened to what fits SearchResultRow::detail beside an
// exchange name.
const char *quoteTypeLabel(const char *quoteType) {
  if (strcmp(quoteType, "EQUITY") == 0) return "Stock";
  if (strcmp(quoteType, "ETF") == 0) return "ETF";
  if (strcmp(quoteType, "INDEX") == 0) return "Index";
  if (strcmp(quoteType, "FUTURE") == 0) return "Futures";
  if (strcmp(quoteType, "CRYPTOCURRENCY") == 0) return "Crypto";
  if (strcmp(quoteType, "CURRENCY") == 0) return "Currency";
  if (strcmp(quoteType, "MUTUALFUND") == 0) return "Fund";
  return quoteType;
}

// A plain, unmodified desktop Chrome User-Agent. Do NOT append an
// app-identifying suffix like "Figo/1.0": appending one makes
// CNN/Akamai's bot management answer with HTTP 418, because a UA
// that starts browser-shaped and then carries an unknown token is precisely
// the anomaly UA-pattern detection looks for. Yahoo tolerated the malformed
// version, CNN did not.
const char *kBrowserUserAgent =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
    "Chrome/128.0.0.0 Safari/537.36";

// ASCII-lowercases into a caller-owned buffer. CoinGecko coin ids are always
// lowercase and, critically, simple/price ECHOES THE ID BACK LOWERCASED
// whatever case you asked with: "ids=Bitcoin" answers {"bitcoin":{...}}. The
// response is then keyed differently from the filter we built around the
// user's spelling, the filter strips the whole body, and a perfectly valid
// id looks exactly like a bad one. Normalising up front is what makes
// "Bitcoin" work, not just "bitcoin".
void toLowerAscii(char *dst, size_t dstLen, const char *src) {
  size_t i = 0;
  for (; src[i] != '\0' && i + 1 < dstLen; i++) {
    dst[i] = (char)tolower((unsigned char)src[i]);
  }
  dst[i] = '\0';
}

}  // namespace

void DataFetcher::begin(ConfigManager &configManager) {
  configManager_ = &configManager;
}

// WiFi.status() rather than FigoNetworkManager::mode(): being in STA mode is
// necessary but not sufficient - a dropped link leaves mode_ == STA while
// every connect attempt would fail. This is also the lower-coupling check;
// DataFetcher never needs to know NetworkManager exists.
bool DataFetcher::wifiReady() const {
  return WiFi.status() == WL_CONNECTED;
}

DataFetcher::FetchState *DataFetcher::stateFor(uint8_t screenId) {
  for (uint8_t i = 0; i < MAX_SCREENS; i++) {
    if (states_[i].screen_id == screenId) return &states_[i];
  }
  return nullptr;
}

ScreenValue *DataFetcher::valueFor(uint8_t screenId) {
  for (uint8_t i = 0; i < MAX_SCREENS; i++) {
    if (values_[i].screen_id == screenId) return &values_[i];
  }
  return nullptr;
}

bool DataFetcher::identityMatches(const FetchState &state, const ScreenConfig &screen) {
  if (!state.identity_valid) return false;
  if (state.fetched_type != screen.type) return false;
  // Layout matters because it decides HOW MANY instruments a value holds:
  // a SINGLE->DUAL switch keeps symbol but leaves price2 unfetched.
  if (state.fetched_layout != screen.layout) return false;
  if (strncmp(state.fetched_symbol, screen.symbol, sizeof(state.fetched_symbol)) != 0) {
    return false;
  }
  return strncmp(state.fetched_symbol2, screen.symbol2, sizeof(state.fetched_symbol2)) == 0;
}

void DataFetcher::rememberIdentity(FetchState &state, const ScreenConfig &screen) {
  state.identity_valid = true;
  state.fetched_type = screen.type;
  state.fetched_layout = screen.layout;
  strlcpy(state.fetched_symbol, screen.symbol, sizeof(state.fetched_symbol));
  strlcpy(state.fetched_symbol2, screen.symbol2, sizeof(state.fetched_symbol2));
}

void DataFetcher::syncStatesToConfig(const RuntimeConfig &config) {
  FetchState freshStates[MAX_SCREENS] = {};
  ScreenValue freshValues[MAX_SCREENS] = {};

  uint8_t count = config.screen_count;
  if (count > MAX_SCREENS) count = MAX_SCREENS;

  for (uint8_t i = 0; i < count; i++) {
    uint8_t id = config.screens[i].id;
    if (id == 0) continue;  // see loop()'s matching guard
    freshStates[i].screen_id = id;
    freshValues[i].screen_id = id;

    // Carry over anything we already know about this id, but ONLY while the
    // screen still points at the same instrument. A reorder or a cosmetic
    // edit (renaming a screen, changing an interval) must not reset a
    // screen's backoff or blank its last-known price; retargeting a screen
    // at a different symbol must do exactly that, or the old instrument's
    // price keeps rendering under the new one's name.
    FetchState *prevState = stateFor(id);
    ScreenValue *prevValue = valueFor(id);
    bool sameInstrument = prevState && identityMatches(*prevState, config.screens[i]);

    if (prevState) {
      // Carried across unconditionally; fetchCoinGecko() re-checks
      // coin_id_from against the screen's CURRENT symbol before trusting it,
      // so an edit that changed the symbol simply invalidates it there
      // rather than needing to be detected here.
      memcpy(freshStates[i].coin_id, prevState->coin_id, sizeof(freshStates[i].coin_id));
      memcpy(freshStates[i].coin_id_from, prevState->coin_id_from,
             sizeof(freshStates[i].coin_id_from));
      memcpy(freshStates[i].coin_id2, prevState->coin_id2, sizeof(freshStates[i].coin_id2));
      memcpy(freshStates[i].coin_id2_from, prevState->coin_id2_from,
             sizeof(freshStates[i].coin_id2_from));
    }

    if (sameInstrument) {
      freshStates[i].nextAttemptMs = prevState->nextAttemptMs;
      freshStates[i].backoffMs = prevState->backoffMs;
      freshStates[i].consecutiveFailures = prevState->consecutiveFailures;
      freshStates[i].identity_valid = prevState->identity_valid;
      freshStates[i].fetched_type = prevState->fetched_type;
      freshStates[i].fetched_layout = prevState->fetched_layout;
      strlcpy(freshStates[i].fetched_symbol, prevState->fetched_symbol,
              sizeof(freshStates[i].fetched_symbol));
      strlcpy(freshStates[i].fetched_symbol2, prevState->fetched_symbol2,
              sizeof(freshStates[i].fetched_symbol2));
      if (prevValue) {
        freshValues[i].price = prevValue->price;
        freshValues[i].change_pct = prevValue->change_pct;
        // Carried with the price it belongs to. Dropping it here would leave
        // a renamed EUR screen rendering a bare number until its next poll.
        strlcpy(freshValues[i].currency, prevValue->currency,
                sizeof(freshValues[i].currency));
        freshValues[i].price2 = prevValue->price2;
        freshValues[i].change_pct2 = prevValue->change_pct2;
        strlcpy(freshValues[i].currency2, prevValue->currency2,
                sizeof(freshValues[i].currency2));
        freshValues[i].stale = prevValue->stale;
        // With `stale`: a cosmetic edit must not drop the code while keeping
        // the dimmed price it explains. A retarget takes the zeroed else-path
        // below, so the old instrument's error never follows a new symbol.
        freshValues[i].error_code = prevValue->error_code;
        freshValues[i].last_update_ms = prevValue->last_update_ms;
      }
    }
    // else: freshStates[i]/freshValues[i] stay zeroed. last_update_ms == 0
    // makes the screen due on the very next loop() iteration (an immediate
    // refetch) and simultaneously renders as "--" instead of the previous
    // instrument's price, so the screen never shows a stale wrong number.
  }

  memcpy(states_, freshStates, sizeof(states_));
  memcpy(values_, freshValues, sizeof(values_));
  if (nextIndex_ >= count) nextIndex_ = 0;
  publish();
}

void DataFetcher::publish() {
  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(200)) != pdTRUE) return;
  memcpy(g_marketSnapshot.values, values_, sizeof(values_));
  g_marketGen++;
  xSemaphoreGive(stateMutex);
}

void DataFetcher::loop() {
  if (!configManager_) return;

  RuntimeConfig config = configManager_->getSnapshot();

  // Pick up screen edits even while offline, so the moment WiFi returns the
  // scheduler is already working against the current screen list.
  uint32_t currentGen = 0;
  if (xSemaphoreTake(stateMutex, 0) == pdTRUE) {
    currentGen = g_configGen;
    xSemaphoreGive(stateMutex);
  }
  if (!statesInitialised_ || currentGen != lastConfigGen_) {
    syncStatesToConfig(config);
    lastConfigGen_ = currentGen;
    statesInitialised_ = true;
  }

  if (!wifiReady()) return;

  // A search takes a turn ahead of the price round-robin: the user is waiting
  // on it, and a screen's next poll is seconds away at worst. One TLS socket
  // at a time still holds: when serviceSearchJob() made provider calls, this
  // iteration stops here rather than also running a price fetch. A search job
  // is a whole turn (its own calls run back to back, never overlapping); it
  // does not run alongside a price poll.
  if (serviceSearchJob()) return;

  uint8_t count = config.screen_count;
  if (count > MAX_SCREENS) count = MAX_SCREENS;
  if (count == 0) return;

  uint32_t now = millis();

  // Round-robin from wherever the cursor left off, taking the first screen
  // that is actually due. Starting from the cursor rather than index 0 keeps
  // one perpetually-due screen from starving the rest.
  for (uint8_t probe = 0; probe < count; probe++) {
    uint8_t index = (nextIndex_ + probe) % count;
    const ScreenConfig &screen = config.screens[index];
    // 0 is the "unused slot" sentinel in states_/values_, so a screen
    // carrying it would match every empty slot. The SPA and ConfigManager
    // both number from 1; this guards against a hand-crafted REST payload.
    if (screen.id == 0) continue;
    FetchState *state = stateFor(screen.id);
    ScreenValue *value = valueFor(screen.id);
    if (!state || !value) continue;  // syncStatesToConfig() lost a race; next tick

    // DUAL screens get the doubled floor even if a stale/hand-crafted config
    // slipped past validation - the TLS-session budget is enforced here too.
    uint16_t floorSec = (screen.layout == ScreenLayout::DUAL)
                            ? SCREEN_INTERVAL_SEC_MIN_DUAL
                            : SCREEN_INTERVAL_SEC_MIN;
    uint16_t intervalSec = screen.interval_sec;
    if (intervalSec < floorSec) intervalSec = floorSec;

    // A screen is due when its interval has elapsed since the last SUCCESS
    // (last_update_ms == 0 means never fetched, i.e. due immediately) and any
    // failure backoff has expired. Both clocks use millis(); a rollover at
    // ~49 days makes one poll early or late, which is harmless here.
    bool intervalElapsed =
        (value->last_update_ms == 0) ||
        (now - value->last_update_ms >= (uint32_t)intervalSec * 1000UL);
    bool backoffElapsed =
        (state->nextAttemptMs == 0) || ((int32_t)(now - state->nextAttemptMs) >= 0);
    if (!intervalElapsed || !backoffElapsed) continue;

    ScreenValue fetched = *value;
    uint32_t retryAfterMs = 0;
    // Stamp the instrument BEFORE the attempt, not after a success. Doing it
    // on success only would leave a screen that has never fetched with no
    // identity, so syncStatesToConfig() could not tell a cosmetic edit from
    // a retarget for it and would reset its failure backoff on every
    // unrelated config save (saving brightness bumps g_configGen too).
    rememberIdentity(*state, screen);
    FetchResult result = fetchScreen(config, screen, *state, fetched, &retryAfterMs);

    if (result == FetchResult::Ok) {
      fetched.screen_id = screen.id;
      fetched.stale = false;
      // `fetched` began as a copy of *value, so without this the previous
      // failure's code would ride along into the recovered screen.
      fetched.error_code = 0;
      fetched.last_update_ms = millis();
      *value = fetched;
      state->consecutiveFailures = 0;
      state->backoffMs = 0;
      state->nextAttemptMs = 0;
      publish();
    } else {
      if (state->consecutiveFailures < 255) state->consecutiveFailures++;
      if (result == FetchResult::RateLimited) {
        // Skip the gentle first step entirely - see FetchResult's comment. A
        // server-supplied Retry-After wins over our own guess when present.
        uint32_t floorMs = (retryAfterMs > 0) ? retryAfterMs : FETCH_BACKOFF_429_MS;
        if (state->backoffMs < floorMs) state->backoffMs = floorMs;
      } else if (state->backoffMs == 0) {
        state->backoffMs = FETCH_BACKOFF_MIN_MS;
      } else {
        state->backoffMs *= 2;
      }
      if (state->backoffMs > FETCH_BACKOFF_MAX_MS) state->backoffMs = FETCH_BACKOFF_MAX_MS;
      state->nextAttemptMs = millis() + state->backoffMs;

      // Only mark stale once failures persist - one dropped poll on a flaky
      // link shouldn't visibly degrade a screen that has good recent data.
      // The on-panel error code follows the same threshold, but is refreshed on
      // every further failure too: a screen can move from Error 53 to Error 50
      // while already stale, and the panel should name the CURRENT reason.
      // A failure site that forgot to set a code must still read as an error,
      // never as "Error 0" (which formatErrorLabel refuses), so it falls back to 50.
      uint8_t code = lastErrorCode_ ? lastErrorCode_ : (uint8_t)ErrorCode::HttpFailed;
      if (state->consecutiveFailures >= FETCH_STALE_AFTER_FAILURES) {
        bool changed = !value->stale || value->error_code != code;
        value->stale = true;
        value->error_code = code;
        if (changed) publish();
      }
      Serial.printf("[fetch] screen %u (%s) failed (error %u), retry in %lus\n", screen.id,
                    screen.symbol, (unsigned)code, (unsigned long)(state->backoffMs / 1000UL));
    }

    nextIndex_ = (index + 1) % count;
    return;  // exactly one fetch per loop() - never two TLS sockets in flight
  }
}

DataFetcher::FetchResult DataFetcher::fetchScreen(const RuntimeConfig &config,
                                                   const ScreenConfig &screen,
                                                   FetchState &state,
                                                   ScreenValue &out,
                                                   uint32_t *retryAfterMsOut) {
  lastErrorCode_ = 0;  // see the member's comment: loop() reads it on failure
  FetchResult primary = FetchResult::Failed;
  switch (screen.type) {
    case ScreenType::CRYPTO:
      primary = fetchCoinGecko(config.providers, screen.symbol, state.coin_id,
                               state.coin_id_from, out, retryAfterMsOut);
      break;
    case ScreenType::STOCK:
    case ScreenType::COMMODITY:
      // Both go to Yahoo's chart endpoint - a commodity is just a futures
      // ticker there (CL=F for WTI, GC=F for gold), same response shape.
      primary = fetchYahoo(config.providers, screen.symbol, out, retryAfterMsOut);
      break;
    case ScreenType::FEAR_GREED:
      // symbol selects WHICH index: these are two genuinely different
      // measures, not two sources for one number. "stocks" is CNN's equity
      // market index; anything else (default "crypto") is alternative.me's.
      // FEAR_GREED never has a DUAL layout (validation forbids it), so this
      // returns directly.
      if (strcmp(screen.symbol, "stocks") == 0) {
        return fetchFearGreedStocks(config.providers, out, retryAfterMsOut);
      }
      return fetchFearGreedCrypto(config.providers, out, retryAfterMsOut);
  }
  if (primary != FetchResult::Ok || screen.layout != ScreenLayout::DUAL) {
    return primary;
  }

  // DUAL: fetch the second instrument back-to-back. Sequential by
  // construction (getFiltered() closed the primary's socket before
  // returning), and all-or-nothing: a second-leg failure fails the whole
  // screen so its result feeds the normal backoff path, rather than
  // publishing a half-updated pair.
  ScreenValue second = {};
  FetchResult r2;
  if (screen.type == ScreenType::CRYPTO) {
    r2 = fetchCoinGecko(config.providers, screen.symbol2, state.coin_id2,
                        state.coin_id2_from, second, retryAfterMsOut);
  } else {
    r2 = fetchYahoo(config.providers, screen.symbol2, second, retryAfterMsOut);
  }
  if (r2 != FetchResult::Ok) return r2;

  out.price2 = second.price;
  out.change_pct2 = second.change_pct;
  strlcpy(out.currency2, second.currency, sizeof(out.currency2));
  return FetchResult::Ok;
}

// ---------------------------------------------------------------------------
// The shared HTTPS + stream-filter path.
//
// Every non-obvious line here exists for a reason learned on real devices.
// Read before editing:
//
//   * useHTTP10(true) is REQUIRED, not an optimisation. ESP32 HTTPClient only
//     de-chunks a "Transfer-Encoding: chunked" body inside getString()/
//     writeToStream(); getStreamPtr() hands back the RAW transport socket,
//     framing bytes and all. Stream-parsing a chunked response therefore
//     feeds ArduinoJson hex chunk lengths and fails with EmptyInput. Chunked
//     encoding is HTTP/1.1-only, so asking for 1.0 makes the server send a
//     plain Content-Length instead. We must stream (the project forbids
//     buffering these payloads whole), so we must do this.
//
//   * The stream->available() wait stops ArduinoJson racing the TLS layer:
//     reading immediately after GET() can see zero bytes before the first
//     record is decrypted and conclude the body is empty. It polls with
//     vTaskDelay, never a bare spin - a tight loop here would starve uiTask
//     and the idle task on this single-core chip. (The 7s HTTPClient timeout
//     is NOT a starvation risk by contrast: lwIP socket reads block the task
//     at the RTOS level, so the scheduler keeps running Task 1 throughout.)
//
//   * setInsecure() skips certificate validation. Deliberate: pinning roots
//     for four hosts costs heap Figo doesn't have and breaks silently on CA
//     rotation. The threat model is a desk ornament reading public prices.
//
//   * http.end() runs on EVERY exit path. The project allows exactly one TLS
//     connection at a time, so leaking a socket here would starve the next
//     fetch of the 25-30KB it needs.
DataFetcher::FetchResult DataFetcher::getFiltered(
    const String &url, const char *const *headerNames, const char *const *headerValues,
    size_t headerCount, const JsonDocument &filter, JsonDocument &out,
    uint32_t *retryAfterMsOut) {
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setTimeout(FETCH_HTTP_TIMEOUT_MS);
  http.setConnectTimeout(FETCH_HTTP_TIMEOUT_MS);
  http.useHTTP10(true);  // see the note above - mandatory for stream parsing

  if (!http.begin(client, url)) {
    return failWith(ErrorCode::HttpFailed);
  }
  for (size_t i = 0; i < headerCount; i++) {
    http.addHeader(headerNames[i], headerValues[i]);
  }
  // Must be registered before GET() for header() to return anything.
  static const char *kCollected[] = {"Retry-After"};
  http.collectHeaders(kCollected, 1);

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    // Any non-200 - including HTTPClient's negative transport codes (refused,
    // timed out) - is Error 50; the raw number is in the log line below. 429 is
    // told apart because it is the one the user can actually do something about.
    FetchResult result = failWith(ErrorCode::HttpFailed);
    if (code == HTTP_CODE_TOO_MANY_REQUESTS) {
      lastErrorCode_ = (uint8_t)ErrorCode::RateLimited;
      result = FetchResult::RateLimited;
      if (retryAfterMsOut) {
        // Retry-After is delta-seconds here in practice; the HTTP-date form
        // would need a wall clock we don't reliably have here, so a
        // non-numeric value parses to 0 and the caller falls back to its own
        // backoff.
        long seconds = http.header("Retry-After").toInt();
        if (seconds > 0) *retryAfterMsOut = (uint32_t)seconds * 1000UL;
      }
    }
    Serial.printf("[fetch] HTTP %d for %s\n", code, url.c_str());
    http.end();
    return result;
  }

  WiFiClient *stream = http.getStreamPtr();
  uint32_t waitStart = millis();
  while (stream->available() == 0 && http.connected() &&
         millis() - waitStart < FETCH_STREAM_WAIT_MS) {
    vTaskDelay(pdMS_TO_TICKS(FETCH_STREAM_POLL_MS));
  }

  // Captured BEFORE http.end() tears the client down - afterwards both of
  // these read as though nothing had ever been connected, which would make
  // the diagnostic below actively misleading.
  int contentLength = http.getSize();
  DeserializationError err =
      deserializeJson(out, *stream, DeserializationOption::Filter(filter));
  bool stillConnected = http.connected();
  http.end();

  if (err) {
    // "EmptyInput" here almost always means useHTTP10(true) regressed and we
    // are reading chunk framing rather than JSON - check that first.
    //
    // "IncompleteInput" is the interesting one and the reason for the extra
    // numbers below: it means bytes DID arrive and then stopped partway
    // through the document, i.e. the TLS stream died mid-body. The prime
    // suspect is heap FRAGMENTATION rather than heap exhaustion, and those
    // are two different measurements:
    //
    //   freeHeap    - total bytes free. Stays reassuringly high.
    //   largestFree - biggest CONTIGUOUS block. This is the one that
    //                 matters, because this build of mbedTLS is compiled
    //                 with CONFIG_MBEDTLS_SSL_MAX_CONTENT_LEN=16384 and
    //                 without ASYMMETRIC_CONTENT_LEN, so every
    //                 WiFiClientSecure needs TWO ~16KB contiguous buffers.
    //                 Allocating and freeing those on every poll fragments
    //                 the heap; once no 16KB hole is left, a session can
    //                 fail partway even though freeHeap still reads 200KB.
    //
    // So: if largestFree is comfortably above ~17000 at the moment of
    // failure, fragmentation is NOT the cause and the truncation is a
    // transport drop - look at WiFi and at the provider. If largestFree has
    // collapsed toward or below ~17000 while freeHeap stayed high, that is
    // fragmentation, and the answer is fewer TLS sessions (raise the
    // screens' interval_sec) rather than anything in this function.
    //
    // contentLength is what the server promised; -1 means it sent no
    // Content-Length at all, which would point at chunked framing instead.
    Serial.printf("[fetch] JSON parse failed (%s) for %s\n", err.c_str(), url.c_str());
    Serial.printf("[fetch]   contentLength=%d stillConnected=%d freeHeap=%u largestFree=%u\n",
                  (int)contentLength, (int)stillConnected, (unsigned)ESP.getFreeHeap(),
                  (unsigned)ESP.getMaxAllocHeap());
    return failWith(ErrorCode::ParseFailed);
  }
  return FetchResult::Ok;
}

// ---------------------------------------------------------------------------
// Yahoo Finance - STOCK and COMMODITY.
//
// This endpoint needs no API key and no cookie/crumb handshake; a browser-ish
// User-Agent is enough. (The crumb requirement widely documented online
// applies to yfinance-style bulk use of other Yahoo endpoints, not to this
// one.) It is unofficial and undocumented, so it can change without notice -
// which is exactly why providers.yahoo_host is user-editable.
DataFetcher::FetchResult DataFetcher::fetchYahoo(const ProvidersConfig &p,
                                                  const char *symbol,
                                                  ScreenValue &out,
                                                  uint32_t *retryAfterMsOut) {
  String url = String("https://") + p.yahoo_host + "/v8/finance/chart/" +
               urlEncodeSymbol(symbol) + "?range=1d&interval=1d";

  JsonDocument filter;
  filter["chart"]["result"][0]["meta"]["regularMarketPrice"] = true;
  filter["chart"]["result"][0]["meta"]["chartPreviousClose"] = true;
  // Yahoo reports the quote's own currency per symbol - "USD", "EUR", "ILA"
  // for Tel Aviv, "GBp" for London. Without this line the stream filter
  // discards it mid-parse like every other unlisted key.
  filter["chart"]["result"][0]["meta"]["currency"] = true;

  const char *names[] = {"User-Agent", "Accept"};
  const char *values[] = {kBrowserUserAgent, "application/json"};

  JsonDocument doc;
  FetchResult result = getFiltered(url, names, values, 2, filter, doc, retryAfterMsOut);
  if (result != FetchResult::Ok) return result;

  JsonArray results = doc["chart"]["result"].as<JsonArray>();
  if (results.isNull() || results.size() == 0) return failWith(ErrorCode::ParseFailed);
  JsonObject meta = results[0]["meta"];

  float price = meta["regularMarketPrice"] | 0.0f;
  float prevClose = meta["chartPreviousClose"] | 0.0f;
  // Reject rather than publish a zero: a malformed or throttled response that
  // still parses would otherwise render as a confident "$0.00".
  if (price <= 0.0f || prevClose <= 0.0f) return failWith(ErrorCode::BadValue);

  out.price = price;
  out.change_pct = (price - prevClose) / prevClose * 100.0f;
  // Written unconditionally, empty included: out starts as a COPY of the
  // screen's previous value, so a missing key here has to clear the old code
  // rather than leave the last instrument's currency attached to a new price.
  strlcpy(out.currency, meta["currency"] | "", sizeof(out.currency));
  return FetchResult::Ok;
}

// ---------------------------------------------------------------------------
// CoinGecko - CRYPTO.
//
// simple/price is keyed by CoinGecko COIN ID ("bitcoin"), not by ticker
// ("BTC"), and an unknown id is not an error: it answers HTTP 200 with an
// empty {}. So a screen configured with "BTC" - the obvious thing to type,
// and what a user who ignores the SPA's placeholder hint will type - would
// fail forever with "[fetch] coingecko: no data for id 'BTC'", climbing the
// backoff to five minutes and never recovering, because nothing in the loop
// could ever turn "BTC" into "bitcoin".
//
// Two things fix that, in this order:
//   1. Case-normalise (see toLowerAscii's comment - "Bitcoin" was broken too,
//      and that one IS a valid id).
//   2. If CoinGecko still doesn't know the id, resolve it ONCE through
//      /api/v3/search and cache the answer in FetchState. Ticker -> id is a
//      question only CoinGecko can answer for the long tail of coins; a
//      hardcoded table would cover the top twenty and quietly fail the rest.
//
// Worst case that costs three requests on the first poll of a mistyped
// screen (price, search, price) and one on every poll after. They are
// strictly sequential - getFiltered() closes each socket before returning -
// so the one-TLS-connection-at-a-time rule still holds; what loop()
// promises is one SCREEN per call, not one socket per call.
//
// Keyless still worked as of 2026-08-27, but CoinGecko has been migrating
// endpoints behind a free Demo key, so providers.coingecko_key is appended
// when set. One coin per request: simple/price can batch several ids, but
// batching would break the one-screen-per-fetch round-robin and give the
// whole batch a single shared backoff. Batching is a possible optimisation.
DataFetcher::FetchResult DataFetcher::fetchCoinGecko(
    const ProvidersConfig &p, const char *symbol, char (&cachedId)[32],
    char (&cachedIdFrom)[sizeof(ScreenConfig::symbol)], ScreenValue &out,
    uint32_t *retryAfterMsOut) {
  char coinId[sizeof(cachedId)];
  // A cached resolution is only valid while it still belongs to the symbol
  // this slot currently carries - screens keep their state across a config
  // edit, so "BTC" -> "bitcoin" must not survive the user retyping the
  // symbol as "ETH".
  bool usingCachedId =
      cachedId[0] != '\0' && strcmp(cachedIdFrom, symbol) == 0;
  if (usingCachedId) {
    strlcpy(coinId, cachedId, sizeof(coinId));
  } else {
    toLowerAscii(coinId, sizeof(coinId), symbol);
  }

  bool unknownId = false;
  FetchResult result = coinGeckoPrice(p, coinId, out, retryAfterMsOut, &unknownId);
  if (!unknownId) return result;

  // Not a coin id CoinGecko knows. Ask it what the user meant. A cached id
  // that has gone unknown is re-resolved from the raw symbol too, so a coin
  // renamed upstream heals itself on the next poll.
  char resolved[sizeof(cachedId)];
  FetchResult resolveResult = resolveCoinGeckoId(p, symbol, resolved, sizeof(resolved));
  if (resolveResult != FetchResult::Ok) {
    Serial.printf("[fetch] coingecko: '%s' is not a coin id and could not be resolved\n",
                  symbol);
    return resolveResult;  // RateLimited propagates so the 429 backoff applies
  }

  Serial.printf("[fetch] coingecko: resolved symbol '%s' to coin id '%s'\n", symbol,
                resolved);
  strlcpy(cachedId, resolved, sizeof(cachedId));
  strlcpy(cachedIdFrom, symbol, sizeof(cachedIdFrom));

  return coinGeckoPrice(p, resolved, out, retryAfterMsOut, nullptr);
}

DataFetcher::FetchResult DataFetcher::coinGeckoPrice(const ProvidersConfig &p,
                                                      const char *coinId, ScreenValue &out,
                                                      uint32_t *retryAfterMsOut,
                                                      bool *unknownIdOut) {
  if (unknownIdOut) *unknownIdOut = false;

  String url = String("https://") + p.coingecko_host +
               "/api/v3/simple/price?ids=" + urlEncodeSymbol(coinId) +
               "&vs_currencies=usd&include_24hr_change=true";
  if (p.coingecko_key[0] != '\0') {
    url += "&x_cg_demo_api_key=";
    url += p.coingecko_key;
  }

  // Built around the runtime coin id - this is why getFiltered() takes a
  // prepared document instead of a compile-time filter string.
  JsonDocument filter;
  filter[coinId]["usd"] = true;
  filter[coinId]["usd_24h_change"] = true;

  const char *names[] = {"User-Agent", "Accept"};
  const char *values[] = {kBrowserUserAgent, "application/json"};

  JsonDocument doc;
  FetchResult result = getFiltered(url, names, values, 2, filter, doc, retryAfterMsOut);
  if (result != FetchResult::Ok) return result;

  JsonObject coin = doc[coinId];
  if (coin.isNull()) {
    // HTTP 200 with an empty body object: the request was fine, the id just
    // isn't one CoinGecko knows. The caller decides whether that is worth a
    // resolution attempt.
    if (unknownIdOut) *unknownIdOut = true;
    Serial.printf("[fetch] coingecko: no data for id '%s'\n", coinId);
    return failWith(ErrorCode::SymbolUnknown);
  }
  float price = coin["usd"] | 0.0f;
  if (price <= 0.0f) return failWith(ErrorCode::BadValue);

  out.price = price;
  out.change_pct = coin["usd_24h_change"] | 0.0f;
  // Not read off the response: the URL above ASKS for usd (vs_currencies=usd)
  // and the key parsed is "usd", so the currency is a property of the request
  // this function makes, not of the coin.
  strlcpy(out.currency, "USD", sizeof(out.currency));
  return FetchResult::Ok;
}

// Resolves a ticker, a name, or a miscased id to a real CoinGecko coin id.
// The search response is a few KB unfiltered; the filter below keeps only
// id+symbol per hit, so what reaches RAM is a short list of short strings.
//
// Results come back ranked by market cap, which is what makes the exact
// ticker match trustworthy: a query for "BTC" also matches
// "bitget-wrapped-btc" and a dozen other wrapped variants, and taking the
// first EXACT symbol match picks plain bitcoin out of that. The top-ranked
// hit is only the fallback, for queries that are names or typo'd ids rather
// than tickers.
DataFetcher::FetchResult DataFetcher::resolveCoinGeckoId(const ProvidersConfig &p,
                                                          const char *query, char *coinIdOut,
                                                          size_t coinIdOutLen) {
  if (coinIdOutLen == 0) return failWith(ErrorCode::SymbolUnknown);
  coinIdOut[0] = '\0';
  if (!query || query[0] == '\0') return failWith(ErrorCode::SymbolUnknown);

  String url = String("https://") + p.coingecko_host +
               "/api/v3/search?query=" + urlEncodeSymbol(query);
  if (p.coingecko_key[0] != '\0') {
    url += "&x_cg_demo_api_key=";
    url += p.coingecko_key;
  }

  // Element 0 of a filter array is applied to EVERY element of the matching
  // input array, so this keeps id+symbol for all hits and discards the
  // thumbnail URLs, which are the bulk of the payload.
  JsonDocument filter;
  filter["coins"][0]["id"] = true;
  filter["coins"][0]["symbol"] = true;

  const char *names[] = {"User-Agent", "Accept"};
  const char *values[] = {kBrowserUserAgent, "application/json"};

  JsonDocument doc;
  FetchResult result = getFiltered(url, names, values, 2, filter, doc, nullptr);
  if (result != FetchResult::Ok) return result;

  JsonArray coins = doc["coins"].as<JsonArray>();
  // No "coins" key at all is the wrong shape (51); an empty list is the
  // provider answering "never heard of it" (52).
  if (coins.isNull()) return failWith(ErrorCode::ParseFailed);
  if (coins.size() == 0) return failWith(ErrorCode::SymbolUnknown);

  for (JsonObject c : coins) {
    const char *symbol = c["symbol"] | "";
    if (strcasecmp(symbol, query) == 0) {
      strlcpy(coinIdOut, c["id"] | "", coinIdOutLen);
      if (coinIdOut[0] != '\0') return FetchResult::Ok;
      break;
    }
  }

  strlcpy(coinIdOut, coins[0]["id"] | "", coinIdOutLen);
  return coinIdOut[0] != '\0' ? FetchResult::Ok : failWith(ErrorCode::SymbolUnknown);
}

// ---------------------------------------------------------------------------
// Instrument search - Yahoo.
//
// Same undocumented API family as the chart endpoint, same browser-ish
// User-Agent, same user-editable host. newsCount=0 and listsCount=0 matter:
// without them the response carries article bodies and is an order of magnitude
// larger, all of which the filter would then have to stream past.
DataFetcher::FetchResult DataFetcher::searchYahoo(const ProvidersConfig &p, const char *query,
                                                   SearchResultRow *rows, uint8_t &count) {
  String url = String("https://") + p.yahoo_host + "/v1/finance/search?q=" +
               urlEncodeQuery(query) + "&quotesCount=" + String(SEARCH_YAHOO_QUOTES) +
               "&newsCount=0&listsCount=0&enableFuzzyQuery=false";

  // Element 0 of a filter array applies to EVERY element of the matching input
  // array - the same trick resolveCoinGeckoId() uses to keep thumbnails out.
  JsonDocument filter;
  JsonObject q = filter["quotes"][0].to<JsonObject>();
  q["symbol"] = true;
  q["shortname"] = true;
  q["longname"] = true;
  q["quoteType"] = true;
  q["exchDisp"] = true;

  const char *names[] = {"User-Agent", "Accept"};
  const char *values[] = {kBrowserUserAgent, "application/json"};

  JsonDocument doc;
  FetchResult result = getFiltered(url, names, values, 2, filter, doc, nullptr);
  if (result != FetchResult::Ok) return result;

  JsonArray quotes = doc["quotes"].as<JsonArray>();
  if (quotes.isNull()) return FetchResult::Ok;  // no matches is not a failure

  for (JsonObject hit : quotes) {
    if (count >= SEARCH_MAX_RESULTS) break;
    const char *symbol = hit["symbol"] | "";
    const char *type = hit["quoteType"] | "";
    if (!searchSymbolAcceptable(symbol)) continue;
    if (!searchQuoteTypeAcceptable(type)) continue;

    // longname is the human one ("Teva Pharmaceutical Industries Limited");
    // shortname is the exchange's abbreviation ("TEVA PHARMA IND"). Prefer the
    // first, fall back in order, and never show an empty row.
    const char *name = hit["longname"] | "";
    if (name[0] == '\0') name = hit["shortname"] | "";
    if (name[0] == '\0') name = symbol;

    SearchResultRow &row = rows[count++];
    strlcpy(row.symbol, symbol, sizeof(row.symbol));
    strlcpy(row.name, name, sizeof(row.name));
    snprintf(row.detail, sizeof(row.detail), "%s · %s", quoteTypeLabel(type),
             (const char *)(hit["exchDisp"] | ""));
    row.source = (searchSourceForQuoteType(type) == SEARCH_SOURCE_COMMODITY)
                     ? ScreenType::COMMODITY
                     : ScreenType::STOCK;
  }
  return FetchResult::Ok;
}

// ---------------------------------------------------------------------------
// Instrument search - CoinGecko.
//
// The same /api/v3/search endpoint resolveCoinGeckoId() already uses, with a
// wider filter: that one only needs id+symbol to resolve a ticker, this one
// also needs the display name and the market-cap rank it filters on.
DataFetcher::FetchResult DataFetcher::searchCoinGeckoRows(const ProvidersConfig &p,
                                                           const char *query,
                                                           SearchResultRow *rows, uint8_t &count,
                                                           uint8_t &hiddenOut) {
  String url = String("https://") + p.coingecko_host +
               "/api/v3/search?query=" + urlEncodeQuery(query);
  if (p.coingecko_key[0] != '\0') {
    url += "&x_cg_demo_api_key=";
    url += p.coingecko_key;
  }

  JsonDocument filter;
  JsonObject c = filter["coins"][0].to<JsonObject>();
  c["id"] = true;
  c["symbol"] = true;
  c["name"] = true;
  c["market_cap_rank"] = true;

  const char *names[] = {"User-Agent", "Accept"};
  const char *values[] = {kBrowserUserAgent, "application/json"};

  JsonDocument doc;
  FetchResult result = getFiltered(url, names, values, 2, filter, doc, nullptr);
  if (result != FetchResult::Ok) return result;

  JsonArray coins = doc["coins"].as<JsonArray>();
  if (coins.isNull()) return FetchResult::Ok;

  for (JsonObject coin : coins) {
    const char *id = coin["id"] | "";
    int32_t rank = coin["market_cap_rank"] | 0;
    if (!searchSymbolAcceptable(id)) continue;
    if (!searchCoinRankAcceptable(rank)) {
      if (hiddenOut < 255) hiddenOut++;
      continue;
    }
    // Keep scanning even once rows[] is full: every remaining coin still has
    // to run the rank check above, or hiddenOut stops counting the moment the
    // array fills (rows[]/count are shared with searchYahoo, which usually
    // fills them first). Just skip the append - no room left to put it.
    if (count < SEARCH_MAX_RESULTS) {
      const char *name = coin["name"] | "";
      if (name[0] == '\0') name = id;

      SearchResultRow &row = rows[count++];
      strlcpy(row.symbol, id, sizeof(row.symbol));
      strlcpy(row.name, name, sizeof(row.name));
      // Ticker plus rank: the ticker is what the user recognises, and the rank is
      // what justifies this coin being in the list at all.
      char ticker[12];
      strlcpy(ticker, coin["symbol"] | "", sizeof(ticker));
      for (char *t = ticker; *t; ++t) *t = (char)toupper((unsigned char)*t);
      snprintf(row.detail, sizeof(row.detail), "%s · #%d by market cap", ticker, (int)rank);
      row.source = ScreenType::CRYPTO;
    }
  }
  return FetchResult::Ok;
}

bool DataFetcher::publishSearchState(uint32_t seq, SearchState state) {
  bool current = false;
  if (xSemaphoreTake(stateMutex, portMAX_DELAY) != pdTRUE) return false;
  if (g_searchJob.seq == seq) {
    g_searchJob.state = state;
    current = true;
  }
  xSemaphoreGive(stateMutex);
  return current;
}

void DataFetcher::publishSearchRows(uint32_t seq, const SearchResultRow *rows, uint8_t count,
                                     uint8_t hiddenCoins, bool ok) {
  if (xSemaphoreTake(stateMutex, portMAX_DELAY) != pdTRUE) return;
  // Superseded while we were out on the network - the answer belongs to a
  // question nobody is asking any more, so it is dropped rather than published.
  if (g_searchJob.seq == seq) {
    for (uint8_t i = 0; i < count; i++) g_searchJob.rows[i] = rows[i];
    g_searchJob.count = count;
    g_searchJob.hiddenCoins = hiddenCoins;
    g_searchJob.state = ok ? SearchState::Done : SearchState::Failed;
  }
  xSemaphoreGive(stateMutex);
}

void DataFetcher::publishCheckResult(uint32_t seq, bool ok, const ScreenValue &v) {
  if (xSemaphoreTake(stateMutex, portMAX_DELAY) != pdTRUE) return;
  // Superseded while we were out on the network - same newest-wins rule as
  // publishSearchRows(), and the reason a failed check can never leak a
  // previous check's price: v is this call's own freshly-fetched value, not
  // whatever g_searchJob happened to hold before.
  if (g_searchJob.seq == seq) {
    g_searchJob.checkOk = ok;
    g_searchJob.price = v.price;
    g_searchJob.change_pct = v.change_pct;
    strlcpy(g_searchJob.currency, v.currency, sizeof(g_searchJob.currency));
    g_searchJob.state = ok ? SearchState::Done : SearchState::Failed;
  }
  xSemaphoreGive(stateMutex);
}

// One turn of the search machine. Called from loop() before the price
// round-robin. A query job makes up to two provider calls (Yahoo, then
// CoinGecko), one after the other, each socket closed before the next opens;
// a check job makes one. Returns true when this call actually made a provider
// (TLS) call, so loop() can skip its own price fetch on that iteration - the
// one-TLS-connection-at-a-time rule against the price round-robin is enforced
// by the CALLER honouring this return value, not by anything inside this
// function alone.
bool DataFetcher::serviceSearchJob() {
  uint32_t seq = 0;
  SearchKind kind = SearchKind::Query;
  char query[SEARCH_QUERY_MAX] = {};
  char checkSymbol[sizeof(ScreenConfig::symbol)] = {};
  ScreenType checkType = ScreenType::STOCK;

  // Only the request fields are copied out, not the ~1KB of result rows.
  if (xSemaphoreTake(stateMutex, 0) != pdTRUE) return false;
  if (g_searchJob.state != SearchState::Queued) {
    xSemaphoreGive(stateMutex);
    return false;
  }
  seq = g_searchJob.seq;
  kind = g_searchJob.kind;
  strlcpy(query, g_searchJob.query, sizeof(query));
  strlcpy(checkSymbol, g_searchJob.checkSymbol, sizeof(checkSymbol));
  checkType = g_searchJob.checkType;
  // Decided from kind INSIDE the critical section - the state transition and
  // the value that validates it must not straddle the mutex release.
  g_searchJob.state =
      (kind == SearchKind::Check) ? SearchState::Checking : SearchState::Yahoo;
  xSemaphoreGive(stateMutex);

  RuntimeConfig config = configManager_->getSnapshot();

  if (kind == SearchKind::Check) {
    // Exactly the request this screen will make on its own schedule - which is
    // the entire point: a symbol that cannot be priced says so HERE, in the
    // editor, instead of becoming a blank frame on the desk an interval later.
    ScreenValue v = {};
    FetchResult r;
    if (checkType == ScreenType::CRYPTO) {
      // Local cache slots: a check is a one-off, so nothing is remembered.
      // fetchCoinGecko() still resolves a ticker like "BTC" through /search
      // on the way, so the check answers for whatever the user picked.
      char coinId[32] = {};
      char coinIdFrom[sizeof(ScreenConfig::symbol)] = {};
      r = fetchCoinGecko(config.providers, checkSymbol, coinId, coinIdFrom, v, nullptr);
    } else {
      r = fetchYahoo(config.providers, checkSymbol, v, nullptr);
    }
    publishCheckResult(seq, r == FetchResult::Ok, v);
    Serial.printf("[search] check %s -> %s\n", checkSymbol, r == FetchResult::Ok ? "ok" : "failed");
    return true;
  }

  SearchResultRow rows[SEARCH_MAX_RESULTS] = {};
  uint8_t count = 0;
  uint8_t hidden = 0;

  FetchResult rY = searchYahoo(config.providers, query, rows, count);
  // Sequential, never concurrent: getFiltered() has closed Yahoo's socket
  // before this line, which is what keeps the one-TLS-connection rule true for
  // a search exactly as it is for a DUAL screen's two legs.
  // Superseded while Yahoo's call was in flight - Yahoo's socket has already
  // been touched, so this call did make a provider call even though it stops
  // here without reaching CoinGecko.
  if (!publishSearchState(seq, SearchState::CoinGecko)) return true;  // superseded
  FetchResult rC = searchCoinGeckoRows(config.providers, query, rows, count, hidden);

  // Either provider alone is a usable answer; only both failing is a failure.
  bool ok = (rY == FetchResult::Ok) || (rC == FetchResult::Ok);
  publishSearchRows(seq, rows, count, hidden, ok);
  Serial.printf("[search] '%s' -> %u rows, %u coins hidden (yahoo %s, coingecko %s)\n", query,
                (unsigned)count, (unsigned)hidden, rY == FetchResult::Ok ? "ok" : "failed",
                rC == FetchResult::Ok ? "ok" : "failed");
  return true;
}

// ---------------------------------------------------------------------------
// alternative.me - the CRYPTO Fear & Greed index (0-100). Documented,
// keyless, and a ~200 byte response; the default of the two F&G sources.
//
// limit=2 fetches today plus the previous reading so change_pct can be a real
// move rather than always zero. Note the API returns "value" as a STRING, so
// it is read via as<float>() rather than the | default operator.
DataFetcher::FetchResult DataFetcher::fetchFearGreedCrypto(const ProvidersConfig &p,
                                                            ScreenValue &out,
                                                            uint32_t *retryAfterMsOut) {
  String url = String("https://") + p.fng_crypto_host + "/fng/?limit=2&format=json";

  JsonDocument filter;
  filter["data"][0]["value"] = true;

  const char *names[] = {"User-Agent", "Accept"};
  const char *values[] = {kBrowserUserAgent, "application/json"};

  JsonDocument doc;
  FetchResult result = getFiltered(url, names, values, 2, filter, doc, retryAfterMsOut);
  if (result != FetchResult::Ok) return result;

  JsonArray data = doc["data"].as<JsonArray>();
  if (data.isNull() || data.size() == 0) return failWith(ErrorCode::ParseFailed);

  float score = data[0]["value"].as<float>();
  if (score < 0.0f || score > 100.0f) return failWith(ErrorCode::BadValue);

  out.price = score;
  // Index points moved since the previous reading, not a percentage - the
  // renderer knows FEAR_GREED screens read change_pct that way.
  out.change_pct = 0.0f;
  if (data.size() > 1) {
    float previous = data[1]["value"].as<float>();
    if (previous > 0.0f) out.change_pct = score - previous;
  }
  return FetchResult::Ok;
}

// ---------------------------------------------------------------------------
// CNN - the STOCK MARKET Fear & Greed index. A different measure from
// alternative.me's crypto index, which is why both are offered.
//
// Undocumented and behind Akamai bot management: it answers HTTP 418 unless
// the request looks like CNN's own page fetching its own JSON, hence the
// Referer/Origin/Accept-Language headers below alongside the unmodified
// Chrome UA. Treat a sustained 418 as a normal stale-data outcome rather than
// something to escalate against - loop()'s backoff handles it, and
// alternative.me is the supported default for users who don't want this.
//
// ENDPOINT: /current, NOT /graphdata. /graphdata fails with a steady stream of
//   [fetch] JSON parse failed (IncompleteInput)
// because, measured against the live endpoint, it is ~177KB: the score we
// want sits in the first ~260 bytes, and the remaining ~99.9% is a year of
// daily history for the index plus its nine sub-indicators. A JSON filter
// stops that history reaching RAM but does NOT stop it crossing the wire -
// ArduinoJson still has to stream and discard every byte to reach the root
// object's closing brace. Pulling 177KB one byte at a time through mbedTLS
// on a C6 takes long enough that any stall past the 7s Stream read timeout
// (FETCH_HTTP_TIMEOUT_MS), or the peer simply hanging up, truncates the body
// mid-document - which is exactly what IncompleteInput means.
//
// /current returns the same score and previous_close in a 239 byte response
// with the two fields at the top level, so the fix is to ask for less data
// rather than to try to read a large response faster. Keep the filter anyway:
// it costs nothing and keeps the parse bounded if CNN ever grows the payload.
DataFetcher::FetchResult DataFetcher::fetchFearGreedStocks(const ProvidersConfig &p,
                                                            ScreenValue &out,
                                                            uint32_t *retryAfterMsOut) {
  String url = String("https://") + p.fng_stocks_host + "/index/fearandgreed/current";

  JsonDocument filter;
  filter["score"] = true;
  filter["previous_close"] = true;

  const char *names[] = {"User-Agent", "Accept", "Accept-Language", "Referer", "Origin"};
  const char *values[] = {kBrowserUserAgent,
                          "application/json, text/plain, */*",
                          "en-US,en;q=0.9",
                          "https://www.cnn.com/markets/fear-and-greed",
                          "https://www.cnn.com"};

  JsonDocument doc;
  FetchResult result = getFiltered(url, names, values, 5, filter, doc, retryAfterMsOut);
  if (result != FetchResult::Ok) return result;

  if (doc["score"].isNull()) return failWith(ErrorCode::ParseFailed);  // wrong shape, not a bad number
  float score = doc["score"] | -1.0f;
  if (score < 0.0f || score > 100.0f) return failWith(ErrorCode::BadValue);

  out.price = score;
  float previous = doc["previous_close"] | score;
  out.change_pct = score - previous;  // index points, as above
  return FetchResult::Ok;
}
