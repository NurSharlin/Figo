#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>

// --- Panel geometry (Waveshare ESP32-C6-Touch-LCD-1.47, JD9853 172x320) ---
// PANEL_WIDTH/PANEL_HEIGHT are the panel's *native* (unrotated) dimensions,
// passed to the Arduino_ST7789 constructor - they do NOT change with
// rotation. DisplayManager queries gfx->width()/height() (which DO reflect
// the applied rotation) for LVGL's hor_res/ver_res and the touch driver's
// coordinate mapping - see DisplayManager::begin().
#define PANEL_WIDTH  172
#define PANEL_HEIGHT 320
// Rotation is runtime-configurable (RuntimeConfig.system.rotation,
// SharedState.h; loaded from NVS, applied via DisplayDriver::begin() at
// boot - save-and-reboot, not live), so there is no compile-time default.
// For reference, this panel's Arduino_GFX setRotation() codes are 0/2 =
// portrait (unsupported - landscape-only), 1/3 = landscape
// (mirror of each other, direction depends on which way the panel is
// mounted) - DisplayDriver::begin() maps the user-facing 0/180 degrees to
// 1/3 internally.

// --- Display GPIO (confirmed against Waveshare's official Arduino demo) ---
#define PIN_LCD_SCK  1
#define PIN_LCD_MOSI 2
#define PIN_LCD_CS   14
#define PIN_LCD_DC   15
#define PIN_LCD_RST  22
#define PIN_LCD_BL   23

// --- Touch GPIO (AXS5106L over I2C) ---
#define PIN_TOUCH_SDA 18
#define PIN_TOUCH_SCL 19
#define PIN_TOUCH_RST 20
#define PIN_TOUCH_INT 21

// LVGL partial buffer: The project caps this at ~11KB RAM. Row count is sized
// against the landscape (rotated) horizontal resolution of 320px - both
// supported rotations (0/180 degrees) are landscape, so this is correct for
// either: 17 rows * 320px * 2 bytes/px = 10880 bytes (~10.6KB). Portrait
// isn't a supported orientation, so there's no 172px-wide
// case to size for. Do not raise this without revisiting the project's ~11KB
// LVGL RAM budget.
#define LVGL_BUFFER_ROWS 17

// --- Carousel auto-advance ---
// Dwell time on each screen before ScreenManager::loop() advances to the
// next one on its own. Manual swipes reset the countdown (see
// ScreenManager::goTo()), so the auto-advance never yanks a screen away
// from someone who just navigated to it deliberately.
//
// Deliberately a compile-time constant rather than a RuntimeConfig.system
// field: making it configurable would need a new NVS key, REST field and UI
// row.
#define SCREEN_AUTO_ADVANCE_MS 8000UL

// --- FreeRTOS task configuration ---
// ESP32-C6 has a single HP RISC-V core: both tasks MUST use tskNO_AFFINITY.
// Passing core ID 1 to xTaskCreatePinnedToCore() aborts boot on this chip.
#define UI_TASK_STACK_BYTES    8192
#define UI_TASK_PRIORITY       2
// WiFiClientSecure's mbedTLS handshake, HTTPClient, and an ArduinoJson
// document all live on this stack at once during a fetch. A stack overflow
// here surfaces as an opaque boot crash rather than a clear error. Measured
// with uxTaskGetStackHighWaterMark() across every screen type: peak usage was
// about 5KB, so 8KB leaves ~3KB of headroom.
#define FETCH_TASK_STACK_BYTES 8192
#define FETCH_TASK_PRIORITY    1

// uiTask must yield every loop iteration via vTaskDelay(); on a single core
// this also feeds the idle task and prevents an ESP-IDF Task Watchdog trip.
#define UI_TASK_LOOP_DELAY_MS 5

// --- NVS (Preferences) namespace + per-section keys (ConfigManager) ---
#define NVS_NAMESPACE            "figo_cfg"
#define NVS_KEY_WIFI              "wifi"
#define NVS_KEY_SYSTEM            "system"
#define NVS_KEY_EXPRESSION_SOURCE "expr_src"
#define NVS_KEY_SCREENS           "screens"
// Per-provider hosts + optional API key. Same one-key-per-section scheme as
// the four above.
#define NVS_KEY_PROVIDERS         "providers"

// --- NetworkManager ---
// SoftAP has no user-facing password entry (nothing to type it into before
// the phone is connected), so it's fixed and documented here rather than
// left open - open APs get flagged/blocked by some phone OSes' captive
// portal detectors.
#define SOFTAP_SSID       "Figo-Setup"
#define SOFTAP_PASSWORD   "figo1234"
#define MDNS_HOSTNAME     "figo"  // reachable at http://figo.local/
// Bounded retry window before falling back to SoftAP - must not be
// indefinite, or a bad saved network leaves the device unreachable forever.
#define WIFI_STA_CONNECT_TIMEOUT_MS 18000
#define WIFI_STA_RETRY_DELAY_MS     500

// --- Runtime STA link supervision (NetworkManager::serviceStaLink) ---
// How long a live STA link may stay down before Figo shows the user the
// "WiFi Disconnected" screen. Long enough to ride out a router's DHCP renew
// or a brief AP roam without the display flapping to that screen and back;
// short enough that a genuinely dead network is obvious on a desk. The
// carousel keeps showing for this whole window - the prices are only
// seconds stale at that point.
//
// Reaching this does NOT raise a SoftAP. A dropped link is retried silently
// (see below) so a router reboot needs no user action at all, and an AP
// would otherwise put a "Figo-Setup" network on every phone in the house
// on every blip. The setup portal is opt-in: tapping the disconnect screen
// raises it (NetworkManager::raisePortal()).
#define WIFI_STA_DROP_GRACE_MS 20000UL

// While disconnected, how often to retry the saved credentials in the
// background. The retry is non-blocking (a bare WiFi.begin() whose result
// is picked up on a later loop() iteration) - it must never use
// waitForStaConnect(), which busy-delays for up to
// WIFI_STA_CONNECT_TIMEOUT_MS and would stall fetchTask, freezing the
// portal (when the user has raised one) for 18s at a time.
#define WIFI_STA_RETRY_INTERVAL_MS 60000UL
// fetchTask's own vTaskDelay while pumping dnsServer.processNextRequest() -
// must be short in SoftAP mode so iOS/Android's automated captive-portal
// probes resolve fast; relaxed once in STA mode (no DNS server running).
#define FETCH_TASK_LOOP_DELAY_AP_MS  10
#define FETCH_TASK_LOOP_DELAY_STA_MS 100

// Minimum time the hello splash stays up before BootScreens::showConnect()
// is allowed to replace it. Without this, a device with no saved WiFi falls
// back to SoftAP near-instantly and the hello screen never visibly appears.
#define BOOT_HELLO_MIN_DURATION_MS 2500

// How long the boot-time "settings problem" screen stays up when an NVS section
// was present but unparseable (g_configLoadErrors). Fixed rather than
// tap-to-dismiss: nothing has been decided yet at that point and the device
// carries on with defaults exactly as it always did, so this is a notice, not a
// prompt. Long enough to read two short lines and a code.
#define BOOT_CONFIG_ERROR_MS 5000

// --- Night scheduler ---
//
// SNTP. Plain UDP, so unlike every other network feature in this firmware it
// costs nothing against the 25-30KB-per-TLS-connection heap cap
// and needs no place in DataFetcher's one-at-a-time round-robin.
//
// Deliberately NOT user-editable the way PROVIDER_HOST_* is: there is nothing
// a user would need to point elsewhere.
#define NTP_HOST_PRIMARY  "pool.ntp.org"
#define NTP_HOST_FALLBACK "time.nist.gov"

// Backlight percentage while asleep. Low, not off: Figo should look asleep,
// not powered down - that is the whole reason Expression::Sleeping exists.
// Whether this is a pleasant level in a dark room is a matter of taste.
#define SLEEP_BRIGHTNESS_PCT 5

// The window has minute resolution. Evaluating it on every 5ms UI tick would
// mean a time() call and two string parses ~200 times a second for nothing.
#define SLEEP_CHECK_PERIOD_MS 1000UL

// --- ConfigManager validation bounds ---
// Never 0: a fully dark panel reads as a dead device, and the setup
// instructions are shown only on the panel. The clamp is applied on both the
// save path and the load path.
#define BRIGHTNESS_MIN 10
#define BRIGHTNESS_MAX 100
// Floor on a screen's refresh interval. This is a TLS-session budget, not a
// politeness setting, and the number is measured rather than guessed.
//
// Polling at ~12 HTTPS sessions a minute degrades within a few minutes:
// fetches start dying mid-body with ArduinoJson reporting IncompleteInput.
// The cause is not a leak and not rate limiting. It is FRAGMENTATION: this core builds mbedTLS with
// CONFIG_MBEDTLS_SSL_MAX_CONTENT_LEN=16384 and without
// ASYMMETRIC_CONTENT_LEN, so every WiFiClientSecure needs TWO ~16KB
// CONTIGUOUS blocks. Churning those allocations breaks the heap into pieces
// none of which is big enough any more, while ESP.getFreeHeap() still
// reports ~200KB free and looks perfectly healthy. That is why
// DataFetcher's periodic log prints largestFree alongside heap - free total
// is the number that lies here.
//
// 60s puts one screen at 1 session/min and a full 8-screen list at 8/min,
// against the ~12/min that degrades. Note that margin is only ~1.5x with a
// full list: this bound is PER SCREEN, so it does not cap the aggregate.
// If screens ever need to poll faster, the fix is an overall requests/min
// budget across all screens, not a lower floor here.
#define SCREEN_INTERVAL_SEC_MIN 60

// A DUAL-layout screen makes TWO TLS sessions per refresh, so it gets double
// the floor: 8 dual screens at 120s is 8 sessions/min aggregate - the same
// worst case as 8 single screens at 60s, and safely under the measured
// ~12/min degradation point documented above.
#define SCREEN_INTERVAL_SEC_MIN_DUAL 120

// --- DataFetcher ---
// Default provider hosts. Overridable at runtime via the providers NVS
// section (RuntimeConfig.providers, SharedState.h) so an endpoint move
// doesn't need a reflash - the project has no backend of its own.
#define PROVIDER_HOST_YAHOO      "query1.finance.yahoo.com"
#define PROVIDER_HOST_COINGECKO  "api.coingecko.com"
#define PROVIDER_HOST_FNG_CRYPTO "api.alternative.me"
#define PROVIDER_HOST_FNG_STOCKS "production.dataviz.cnn.io"

// Whole-request budget for one fetch (connect + TLS handshake + response).
#define FETCH_HTTP_TIMEOUT_MS 7000
// Bounded wait for the first decrypted TLS record to land before handing the
// stream to ArduinoJson - reading immediately after GET() can otherwise race
// the TLS layer and look like an empty stream. Polled with vTaskDelay, never
// a bare spin: this is the single HP core (see the FreeRTOS note above).
#define FETCH_STREAM_WAIT_MS      3000
#define FETCH_STREAM_POLL_MS      10

// Per-screen failure backoff, doubling from MIN to MAX. A screen that has
// never succeeded also uses this rather than retrying every loop iteration
// against an endpoint that is actively rejecting us.
#define FETCH_BACKOFF_MIN_MS  30000UL
#define FETCH_BACKOFF_MAX_MS  300000UL
// HTTP 429 skips straight to this floor instead of starting at
// FETCH_BACKOFF_MIN_MS - keyless CoinGecko rate-limits around 10-30 req/min,
// and re-hitting a 429 risks a longer block. A Retry-After header, when the
// response carries one, overrides this.
#define FETCH_BACKOFF_429_MS  60000UL
// Consecutive failures before a screen's last-known value is marked stale.
// Below this the previous value keeps rendering normally - one dropped poll
// on a flaky link shouldn't visibly degrade the display.
#define FETCH_STALE_AFTER_FAILURES 3

// --- Instrument search ---
// How many quotes to ask Yahoo for. More than SEARCH_MAX_RESULTS on purpose:
// options and over-long symbols are dropped after the response arrives, so the
// request has to over-fetch slightly to still fill a list of ten.
#define SEARCH_YAHOO_QUOTES 12

// --- Expression source defaults ---
// The face's three cut points, as percent 24h change on the starred screen.
// Named here rather than left as literals in ConfigManager because they are
// product decisions, and loadOrDefaultExpressionSource() needs each of them
// in two places (the parsed-blob fallback and the no-blob branch).
//
// There is deliberately no NEUTRAL default: Neutral is the band left over
// between HAPPY and BEAR, not a configured range.
#define EXPRESSION_BULL_DEFAULT   5.0f    // at or above -> Bullish
#define EXPRESSION_HAPPY_DEFAULT  1.0f    // 1.0 .. 5.0  -> Happy
#define EXPRESSION_BEAR_DEFAULT  (-2.5f)  // at or below -> Panicked

// --- OTA update (OtaManager.cpp) -------------------------------------------
// begin refuses below this. The Updater itself needs only a 4KB buffer; this is
// a health check - the board has already lost time to a fragmented heap that
// ESP.getFreeHeap() reported as fine, and an update on top of one is a bad idea.
#define OTA_MIN_LARGEST_BLOCK   (24 * 1024)
// begin -> first byte: after this the device disarms and resumes fetching.
#define OTA_ARM_TIMEOUT_MS      60000
// No bytes for this long mid-upload: abort and free the fetcher.
#define OTA_STALL_TIMEOUT_MS    15000
// Rollback probation: a freshly OTA'd image is marked valid once it has been up
// this long AND shown a live UI task, a started web server and a reachable link.
#define OTA_HEALTHY_AFTER_MS    30000
// ...and is handed back to the bootloader for rollback if it still is not by then.
#define OTA_VERIFY_DEADLINE_MS  120000
// How long the panel keeps a failure reason up before returning to the carousel.
#define OTA_FAIL_SHOW_MS        8000

// The first OTA-capable versions (0.5.0 onwards) go on over USB; every later
// version can go over the air.
#define FIGO_FW_VERSION "1.0.0"

// The firmware carries its own version as a NUL-terminated string that the OTA
// image check (OtaImage.h) finds by scanning the .bin - the phone reads the
// candidate's version out of the file before offering to install it. It is
// DEFINED once, in Figo.ino, and must stay a single copy: two TUs each emitting
// their own would give the scanner two markers to choose between. It is also
// READ by /api/status (past the prefix) so the linker cannot drop it.
extern const char FIGO_FW_ID[];
#define FIGO_FW_ID_PREFIX_LEN 8  // strlen of the marker prefix in Figo.ino

#endif // CONFIG_H
