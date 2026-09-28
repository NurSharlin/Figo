#include "Config.h"
#include "SharedState.h"
#include "DisplayManager.h"
#include "ConfigManager.h"
#include "NetworkManager.h"
#include "DataFetcher.h"
#include "OtaManager.h"

// The version marker the OTA image check scans for. Spelled out only here; see
// the comment on the extern in Config.h. The array length check keeps the
// prefix length used to skip it honest.
const char FIGO_FW_ID[] = "FIGO-FW:" FIGO_FW_VERSION;
static_assert(sizeof("FIGO-FW:") - 1 == FIGO_FW_ID_PREFIX_LEN, "FIGO_FW_ID_PREFIX_LEN out of step");

SemaphoreHandle_t stateMutex;
RuntimeConfig g_runtimeConfig = {};
MarketSnapshot g_marketSnapshot = {};
uint32_t g_configGen = 0;
uint32_t g_marketGen = 0;
NetMode g_networkMode = NetMode::BOOTING;
bool g_softApRequested = false;
bool g_sleepingEffective = false;
SearchJob g_searchJob = {};
OtaUiState g_otaUiState = OtaUiState::IDLE;
uint8_t g_otaPercent = 0;
const char *g_otaMessage = "";
volatile uint32_t g_uiHeartbeat = 0;
uint32_t g_configLoadErrors = 0;

static DisplayManager displayManager;
static ConfigManager configManager;
// FigoNetworkManager, not NetworkManager - arduino-esp32 core 3.x's own
// Network.h (pulled in via <WiFi.h>) claims that class name globally. See
// NetworkManager.h's top-of-file comment.
static FigoNetworkManager networkManager(configManager);
static DataFetcher dataFetcher;
static OtaManager otaManager(networkManager);

// Keep the core from marking a freshly OTA-installed image valid the moment it
// boots (its default): returning true defers that to OtaManager, which does it
// only once the image has proved itself. Without this override the bootloader's
// rollback is compiled in but never gets a chance to fire.
//
// extern "C" is load-bearing: the core defines this as a weak C symbol, and a
// C++ definition is name-mangled, so it silently overrides nothing - the image
// is then marked valid at boot exactly as if this function did not exist.
extern "C" bool verifyRollbackLater() { return true; }

// Task 1 - UI. Owns LVGL rendering, touch handling, and face animation
// exclusively. Only ever takes stateMutex for short bounded reads (copy
// out, release immediately) - never renders while holding it.
static void uiTask(void *pvParameters) {
  displayManager.begin();

  for (;;) {
    g_uiHeartbeat = g_uiHeartbeat + 1;  // (not ++: volatile, C++20) "the UI task is alive" - see SharedState.h
    displayManager.loop();
    // Non-negotiable on this single-core chip: without this yield, a busy
    // high-priority loop here starves the idle task and trips the
    // ESP-IDF Task Watchdog Timer, not just fetchTask.
    vTaskDelay(pdMS_TO_TICKS(UI_TASK_LOOP_DELAY_MS));
  }
}

// Task 2 - background fetcher. NetworkManager owns WiFi/SoftAP/REST;
// DataFetcher shares the loop and does the HTTPS polling. Order matters
// on both lines: networkManager.begin() decides STA vs AP, and DataFetcher
// no-ops until WiFi is actually up.
static void fetchTask(void *pvParameters) {
  // OtaManager first: it creates the mutex the OTA routes take, and those
  // routes go live inside networkManager.begin().
  networkManager.attachOta(&otaManager);
  otaManager.begin();
  networkManager.begin();
  dataFetcher.begin(configManager);

  for (;;) {
    networkManager.loop();
    otaManager.loop();
    // At most one HTTPS fetch per iteration, so this task keeps yielding
    // below and only ever holds one TLS socket at a time.
    // While an OTA update owns the device the fetch is skipped. Reaching this
    // line proves no fetch is in flight (loop() is synchronous), which is what
    // holdFetcher() acks - so DataFetcher needs no pause flag of its own.
    if (!otaManager.holdFetcher()) dataFetcher.loop();
    // AP mode needs fast polling so dnsServer_.processNextRequest() resolves
    // iOS/Android's captive-portal probes quickly; STA mode has no DNS
    // server running and can relax. Either way this priority-1 task yields
    // every iteration, so it never contends with uiTask for the single core.
    // softApUp(), not == Mode::AP: the runtime NO_WIFI fallback runs the
    // very same captive portal and needs the same fast DNS pumping.
    uint32_t delayMs = networkManager.softApUp() ? FETCH_TASK_LOOP_DELAY_AP_MS
                                                 : FETCH_TASK_LOOP_DELAY_STA_MS;
    vTaskDelay(pdMS_TO_TICKS(delayMs));
  }
}

void setup() {
  Serial.begin(115200);

  stateMutex = xSemaphoreCreateMutex();

  // Loads NVS config into g_runtimeConfig before either task starts - both
  // uiTask's first displayManager.begin() and fetchTask's first
  // networkManager.begin() need a valid config on their very first
  // iteration.
  configManager.begin();

  // ESP32-C6 has a single HP RISC-V core: both tasks MUST use
  // tskNO_AFFINITY. Passing core ID 1 trips a FreeRTOS assertion and
  // aborts boot on this chip - there is no core 1 to pin to. Task
  // isolation here comes from priority-based preemption, not core separation.
  xTaskCreatePinnedToCore(uiTask, "ui", UI_TASK_STACK_BYTES, NULL,
                           UI_TASK_PRIORITY, NULL, tskNO_AFFINITY);
  xTaskCreatePinnedToCore(fetchTask, "fetch", FETCH_TASK_STACK_BYTES, NULL,
                           FETCH_TASK_PRIORITY, NULL, tskNO_AFFINITY);
}

void loop() {
  // Both tasks own execution; nothing left for Arduino's default
  // loop task to do.
  vTaskDelete(NULL);
}
