#ifndef FIGO_THEME_H
#define FIGO_THEME_H

#include <lvgl.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <math.h>

// Shared visual language for every Figo screen. Header-only: colors are lv_color_t
// built at call sites via inline functions rather than global constructors,
// because lv_color_hex() is not constexpr under LV_COLOR_16_SWAP.
namespace figo_theme {

// ---- Palette (hex values) ----
inline lv_color_t bg()            { return lv_color_black(); }
inline lv_color_t textWhite()     { return lv_color_white(); }
inline lv_color_t positive()      { return lv_color_hex(0x00BC16); }
inline lv_color_t negative()      { return lv_color_hex(0xD60307); }
// Muted greys for stale/never-fetched values - dimming rather than blanking.
inline lv_color_t dimGrey()       { return lv_color_make(120, 120, 120); }
inline lv_color_t subtitleGrey()  { return lv_color_make(160, 160, 160); }
// Screen-dot indicator.
inline lv_color_t dotActive()     { return lv_color_hex(0xC2C2C2); }
inline lv_color_t dotInactive()   { return lv_color_hex(0x676767); }
// Fear & Greed gauge tracks: the slider uses the light grey, the circular
// bar the dark grey - both 8px strokes in the design.
inline lv_color_t trackLight()    { return lv_color_hex(0xC2C2C2); }
inline lv_color_t trackDark()     { return lv_color_hex(0x676767); }

// ---- Fonts (nearest compiled Montserrat per the design's Open Sans sizes) ----
// Design px -> Montserrat: 36->36 names/titles, 44->44 single price,
// 50->48 circular index, 40->40 slider index, 30->30 dual price,
// 24->24 dual name, 20/16/14 exact.
#define FIGO_FONT_NAME       (&lv_font_montserrat_36)
#define FIGO_FONT_PRICE_LG   (&lv_font_montserrat_44)
#define FIGO_FONT_INDEX_LG   (&lv_font_montserrat_48)
#define FIGO_FONT_INDEX_MD   (&lv_font_montserrat_40)
#define FIGO_FONT_PRICE_MD   (&lv_font_montserrat_30)
#define FIGO_FONT_NAME_MD    (&lv_font_montserrat_24)
#define FIGO_FONT_LABEL      (&lv_font_montserrat_20)
#define FIGO_FONT_SMALL      (&lv_font_montserrat_16)
#define FIGO_FONT_TINY       (&lv_font_montserrat_14)

// ---- Fear & Greed bands ----
// Thresholds are alternative.me's published bands (CNN's index follows them
// closely enough for a desk display). Strings uppercase per the design.
inline const char *fearGreedRating(float score) {
  if (score < 25.0f) return "EXTREME FEAR";
  if (score < 45.0f) return "FEAR";
  if (score <= 55.0f) return "NEUTRAL";
  if (score <= 75.0f) return "GREED";
  return "EXTREME GREED";
}

// Band colors from the "Fear & Greed Slider (all position)" frame.
inline lv_color_t fearGreedBandColor(float score) {
  if (score < 25.0f) return lv_color_hex(0xD60307);   // extreme fear - red
  if (score < 45.0f) return lv_color_hex(0xD68503);   // fear - orange
  if (score <= 55.0f) return lv_color_hex(0xC9AE00);  // neutral - yellow
  if (score <= 75.0f) return lv_color_hex(0x5CCD4D);  // greed - light green
  return lv_color_hex(0x00BC16);                      // extreme greed - green
}

// ---- Value formatting ----
// Appends the currency the PROVIDER reported for this instrument (see
// ScreenValue::currency). Three cases:
//
//   USD        -> "319.70$"    tight against the number
//   ILS / ILA  -> "11,030"     nothing at all
//   any other  -> "191.32 EUR" the ISO code, spaced, as plain text
//
// The code is TEXT because there is no symbol to draw. LVGL's compiled
// Montserrat faces carry ASCII 0x20-0x7E plus a degree sign, a bullet and the
// FontAwesome subset - verified in lv_font_montserrat_44.c, whose cmaps are
// {range_start 32, range_length 95} and a sparse list of 0xF0xx icons. There
// is no euro, pound, yen or shekel glyph at ANY of the eight sizes Figo
// compiles, so the alternative to "EUR" is a missing-glyph box, not a nicer
// symbol. Real symbols need custom faces built with lv_font_conv.
//
// "ILA" is agorot and "GBp" is pence - Yahoo quotes those exchanges in their
// minor unit and the codes say so, which is why the code is echoed verbatim
// rather than upper-cased.
//
// An EMPTY code appends nothing rather than defaulting to "$": a screen that
// has never fetched renders "--" anyway, and inventing a currency for one
// that did is worse than omitting it.
inline void appendCurrency(char *buf, size_t len, const char *currency) {
  if (!buf || !currency || currency[0] == '\0') return;
  size_t used = strlen(buf);
  if (used + 1 >= len) return;  // no room for even one more character
  if (strcasecmp(currency, "USD") == 0) {
    buf[used] = '$';
    buf[used + 1] = '\0';
    return;
  }
  if (strcasecmp(currency, "ILS") == 0 || strcasecmp(currency, "ILA") == 0) return;
  snprintf(buf + used, len - used, " %s", currency);
}

// The design renders prices as "26,500$": thousands separator, then the
// currency. Magnitude-scaled decimals as before: five-figure indices don't
// need cents, a sub-dollar altcoin is unreadable without them. The DUAL
// layout's second column must be passed ITS value's currency2, not the
// first's - a dual screen can legitimately pair EUR with USD.
inline void formatPrice(char *buf, size_t len, float price, const char *currency) {
  if (!buf || len == 0) return;
  if (price >= 1000.0f) {
    char raw[16];
    snprintf(raw, sizeof(raw), "%ld", lroundf(price));
    size_t n = strlen(raw);
    size_t out = 0;
    for (size_t i = 0; i < n && out + 2 < len; i++) {
      if (i > 0 && (n - i) % 3 == 0) buf[out++] = ',';
      buf[out++] = raw[i];
    }
    buf[out] = '\0';
  } else if (price >= 1.0f) {
    snprintf(buf, len, "%.2f", price);
  } else {
    snprintf(buf, len, "%.4f", price);
  }
  appendCurrency(buf, len, currency);
}

inline void formatChangePct(char *buf, size_t len, float changePct) {
  snprintf(buf, len, "%+.2f%%", changePct);
}

}  // namespace figo_theme

#endif  // FIGO_THEME_H
