#ifndef SLEEP_WINDOW_H
#define SLEEP_WINDOW_H

#include <stdint.h>
#include <time.h>

// Deliberately dependency-free: <stdint.h> and <time.h> ONLY. No Arduino, no
// FreeRTOS, no LVGL. That is what lets this rule be compiled with a plain host
// g++ and checked at every boundary in a second, instead of standing next to
// the device at 23:00. Do not add includes here.
//
// The rule is used from one place: DisplayManager's sleep tick, via
// SharedState.h's sleepingNow() wrapper. It resolves this schedule answer
// against the touch-wake latch and publishes the result as
// g_sleepingEffective, which is what /api/screens/state reports - so the
// phone and the panel cannot disagree. The rule lives here once for the same
// reason moodFor() lives in SharedState.h once: two copies of a rule are how
// two copies disagree.

// Any UTC timestamp below this means the clock has never been set - lwIP's
// SNTP has not answered yet, or never will. 1.7e9 is 2023-11-14.
#define EPOCH_SANE ((time_t)1700000000)

// Parses exactly "HH:MM" (zero-padded, 5 chars) into minutes since local
// midnight. Returns -1 for anything else, the empty string included - which
// is how the config stores "sleep disabled".
inline int16_t parseHhMm(const char *s) {
  if (!s) return -1;
  if (s[0] < '0' || s[0] > '9') return -1;
  if (s[1] < '0' || s[1] > '9') return -1;
  if (s[2] != ':') return -1;
  if (s[3] < '0' || s[3] > '9') return -1;
  if (s[4] < '0' || s[4] > '9') return -1;
  if (s[5] != '\0') return -1;
  int16_t h = (int16_t)((s[0] - '0') * 10 + (s[1] - '0'));
  int16_t m = (int16_t)((s[3] - '0') * 10 + (s[4] - '0'));
  if (h > 23 || m > 59) return -1;
  return (int16_t)(h * 60 + m);
}

// True when nowUtc falls inside [start, end) in local time.
//
// utcOffsetMin is a plain offset, NOT a timezone: no DST rules, no zone
// database. Accepted tolerance is one hour twice a year, and the phone
// re-sends a DST-aware offset whenever the SPA is next opened.
inline bool sleepWindowActive(const char *start, const char *end,
                              int16_t utcOffsetMin, time_t nowUtc) {
  // Fail OPEN. A device that cannot tell the time must not black itself out,
  // and this covers the permanent case (NTP never succeeds) as well as the
  // boot window. Guarded here rather than at the call sites so no caller can
  // forget it, and so the host test covers it.
  if (nowUtc < EPOCH_SANE) return false;

  int16_t s = parseHhMm(start);
  int16_t e = parseHhMm(end);
  if (s < 0 || e < 0) return false;  // disabled: empty or malformed
  // A zero-length window read as "always on" would be an unrecoverable dark
  // panel, so it reads as disabled instead.
  if (s == e) return false;

  int32_t utcMin = (int32_t)((nowUtc / 60) % 1440);
  // Double-mod: the inner % can still return a negative remainder (C++'s %
  // follows the sign of the dividend), so a second %1440 after re-adding
  // 1440 is what makes the wrap TOTAL, for any int16_t utcOffsetMin - not
  // just the -720..+840 range a caller is expected to send. This header
  // can't see or enforce that caller-side clamp, and an out-of-range offset
  // sliding through would otherwise leave `local` negative, which the
  // midnight-crossing branch below reads as "before end" and reports sleep
  // ACTIVE - failing closed into a dark panel instead of open.
  int32_t local = ((utcMin + (int32_t)utcOffsetMin) % 1440 + 1440) % 1440;

  if (s < e) return local >= s && local < e;
  // start > end means the window crosses midnight - 23:00->07:00, which is
  // the NORMAL configuration, not an edge case. Getting this branch backwards
  // yields a window that is never true, which looks exactly like "the feature
  // doesn't work".
  return local >= s || local < e;
}

#endif  // SLEEP_WINDOW_H
