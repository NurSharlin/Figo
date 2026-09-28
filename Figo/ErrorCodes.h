#ifndef ERROR_CODES_H
#define ERROR_CODES_H

#include <stddef.h>
#include <stdint.h>

// The one table of on-panel error codes. Deliberately dependency-free
// (<stddef.h> and <stdint.h> ONLY - no Arduino, no snprintf, no LVGL), for the
// same reason as SleepWindow.h and OtaImage.h: it must stay compilable with a
// plain host compiler so it can be unit-tested off the device.
//
// Decades group the source: 4x network, 5x fetch/parse, 6x config load. A
// number is never reused for a different meaning - the user guide prints this
// table and a user may have written a code down.
enum class ErrorCode : uint8_t {
  None = 0,

  // 4x network. Defined and serial-logged only: the NO WIFI screen has no
  // place for a code.
  WifiLost = 40,

  // 5x fetch. Surfaced per screen once it crosses FETCH_STALE_AFTER_FAILURES.
  HttpFailed = 50,     // non-200 status or a transport failure (the raw status stays in the serial log)
  ParseFailed = 51,    // body is not the JSON we asked for, or has the wrong shape
  SymbolUnknown = 52,  // CoinGecko cannot resolve this symbol to a coin id
  RateLimited = 53,    // HTTP 429
  BadValue = 54,       // parsed fine, but the number is unusable (price <= 0, F&G outside 0..100)

  // 6x config load. One per NVS section that was present but unparseable at boot.
  ConfigWifi = 60,
  ConfigSystem = 61,
  ConfigExpression = 62,
  ConfigProviders = 63,
  ConfigScreens = 64,
};

// Bitmask of NVS sections that failed to load at boot (g_configLoadErrors).
// A section that is simply ABSENT (fresh board, factory reset) is normal and
// never sets a bit - only "present but would not parse" does.
#define CONFIG_ERR_WIFI (1u << 0)
#define CONFIG_ERR_SYSTEM (1u << 1)
#define CONFIG_ERR_EXPRESSION (1u << 2)
#define CONFIG_ERR_PROVIDERS (1u << 3)
#define CONFIG_ERR_SCREENS (1u << 4)
#define CONFIG_ERR_COUNT 5

// The code that names bit `bit` (0..CONFIG_ERR_COUNT-1); None when out of range.
inline ErrorCode configErrorCodeForBit(uint8_t bit) {
  if (bit >= CONFIG_ERR_COUNT) return ErrorCode::None;
  return (ErrorCode)((uint8_t)ErrorCode::ConfigWifi + bit);
}

// Writes "Error NN" (codes are always two digits) and returns the length, or 0
// if the buffer cannot hold the text plus its NUL. A code that is not two
// digits is refused rather than printed wrongly: a panel that says "Error 5"
// for 5 or "Error 260" for 260 is worse than one that says nothing.
inline size_t formatErrorLabel(char *buf, size_t bufLen, uint8_t code) {
  static const char kPrefix[] = "Error ";
  const size_t prefixLen = sizeof(kPrefix) - 1;
  if (!buf || bufLen < prefixLen + 3) return 0;
  if (code < 10 || code > 99) {
    buf[0] = '\0';
    return 0;
  }
  for (size_t i = 0; i < prefixLen; i++) buf[i] = kPrefix[i];
  buf[prefixLen] = (char)('0' + code / 10);
  buf[prefixLen + 1] = (char)('0' + code % 10);
  buf[prefixLen + 2] = '\0';
  return prefixLen + 2;
}

// Writes the codes for every set bit in `mask`, space separated ("60 64"),
// for the boot screen. Returns the length; 0 for an empty mask or a buffer
// that cannot hold even the first code. Stops early rather than truncating a code.
inline size_t formatConfigErrorCodes(char *buf, size_t bufLen, uint32_t mask) {
  if (!buf || bufLen == 0) return 0;
  size_t n = 0;
  buf[0] = '\0';
  for (uint8_t bit = 0; bit < CONFIG_ERR_COUNT; bit++) {
    if (!(mask & (1u << bit))) continue;
    uint8_t code = (uint8_t)configErrorCodeForBit(bit);
    size_t need = (n > 0 ? 1 : 0) + 2 + 1;  // separator + two digits + NUL
    if (n + need > bufLen) break;
    if (n > 0) buf[n++] = ' ';
    buf[n++] = (char)('0' + code / 10);
    buf[n++] = (char)('0' + code % 10);
    buf[n] = '\0';
  }
  return n;
}

#endif  // ERROR_CODES_H
