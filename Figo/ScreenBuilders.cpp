#include "ScreenBuilders.h"
#include "ErrorCodes.h"
#include "FigoTheme.h"

namespace {

// Screen geometry. The layout gives text BASELINES; LVGL labels are
// positioned by their top edge, so tops here are baseline - ~0.72*fontSize
// (cap ascent), rounded.

// --- Single ticker ("Single Ticker Price") ---
constexpr lv_coord_t kMarginLeft = 19;
constexpr lv_coord_t kNameTop = 24;         // baseline 49.5, 36px
constexpr lv_coord_t kSymbolTop = 17;       // baseline 28.2, 16px
constexpr lv_coord_t kPriceTop = 71;        // baseline 103.1, 44px
constexpr lv_coord_t kChangeTop = 110;      // baseline 124.3, 20px
constexpr lv_coord_t kTodayTop = 132;       // baseline 146.3, 20px
constexpr lv_coord_t kRightColInset = -28;  // change/TODAY right edge ~292
constexpr lv_coord_t kSymbolInset = -20;    // ticker symbol right edge ~300
constexpr lv_coord_t kNameMaxWidth = 230;   // keep long names off the symbol

// --- Dual ticker ("Double Ticker Price") ---
// Both columns are optically centered in their half of the 320px frame.
constexpr lv_coord_t kDualColOffset = 80;    // half centers at 160 +/- 80
constexpr lv_coord_t kDualNameTop = 27;      // baseline 44.8, 24px
constexpr lv_coord_t kDualPriceTop = 76;     // baseline 98.1, 30px
constexpr lv_coord_t kDualChangeTop = 122;   // baseline 131.9, 14px
constexpr lv_coord_t kDualTodayTop = 139;    // baseline 148.9, 14px
constexpr lv_coord_t kDividerX = 159;        // white 2px line, y 27..145
constexpr lv_coord_t kDividerTop = 27;
constexpr lv_coord_t kDividerHeight = 118;

// --- Fear & greed, shared ---
constexpr lv_coord_t kFgTitleLeft = 19;
constexpr lv_coord_t kFgTitleTop = 24;       // baseline 49.5, 36px
// Slider: the title owns the full width between the side margins.
constexpr lv_coord_t kFgTitleMaxW = 320 - 2 * kFgTitleLeft;
// Circular: the title has to stop short of the gauge, which starts at
// kFgArcX. Wraps to the design's two lines ("CRYPTO / F&G").
constexpr lv_coord_t kFgCircTitleW = 140;

// --- Fear & greed slider ---
// Track: 8px stroke from (44,80) to (277,80); marker center runs 43..279.
constexpr lv_coord_t kFgTrackX = 44;
constexpr lv_coord_t kFgTrackW = 233;
constexpr lv_coord_t kFgTrackY = 76;         // top edge (center y 80, 8px)
constexpr lv_coord_t kFgTrackH = 8;
constexpr lv_coord_t kFgMarkerSize = 20;
constexpr lv_coord_t kFgMarkerTop = 70;      // center y 80
constexpr lv_coord_t kFgSliderScoreTop = 96;   // baseline 124.5, 40px
constexpr lv_coord_t kFgSliderBandTop = 135;   // baseline 146.2, 16px
constexpr lv_coord_t kFgSliderScoreW = 120;  // fixed width, text centered,
constexpr lv_coord_t kFgSliderBandW = 170;   // slid to follow the marker

// --- Fear & greed circular ---
// Circle fitted through the exported path's anchors: center (235.5, 99.5),
// r 67.5, sweeping 252 degrees. LVGL arc angles are clockwise from 3
// o'clock, so the visible track runs from 142 to 34 degrees.
//
// Sizing this correctly needs two LVGL facts, both verified in its source:
//   * the arc radius is MIN(w,h)/2 with no padding (lv_arc.c get_center()),
//   * lv_draw_sw_arc strokes INWARD from that radius (area_in = area_out
//     inset by width), so radius IS the outer edge of the ink, not its
//     centreline.
// The previous 152 therefore drew an outer radius of 76 against the
// design's 71.5 - visibly too big - and put the object's box at y 24..176 on
// a 172px panel, hanging 4px off the bottom. 143 gives r = 71 (outer ink
// 71, knob centreline 67 = the design's 67.5), a box of y 29..171 that fits
// the panel exactly, and ~6px of clearance above the dot row at y 158.
constexpr lv_coord_t kFgArcSize = 143;       // 2 * outer radius
constexpr lv_coord_t kFgArcX = 165;          // center (165+71, 29+71) = (236, 100)
constexpr lv_coord_t kFgArcY = 29;
constexpr uint16_t kFgArcStartAngle = 142;
constexpr uint16_t kFgArcEndAngle = 34;
constexpr lv_coord_t kFgArcWidth = 8;
constexpr lv_coord_t kFgKnobPad = 7;         // knob diameter = width + 2*pad ~ 22
constexpr lv_coord_t kFgCircScoreTop = 71;   // baseline 105.4, 48px
constexpr lv_coord_t kFgCircBandTop = 115;   // baseline 129.8, 20px
constexpr lv_coord_t kFgCircCenterX = 236;
constexpr lv_coord_t kFgCircScoreW = 140;
constexpr lv_coord_t kFgCircBandW = 160;

// --- Dot indicator ("Slide Indicator") ---
constexpr lv_coord_t kDotPitch = 11;
constexpr lv_coord_t kDotActiveSize = 8;
constexpr lv_coord_t kDotInactiveSize = 6;
constexpr lv_coord_t kDotRowBottomOffset = -6;  // active dot center at y=162

lv_obj_t *makeLabel(lv_obj_t *parent, const char *text, const lv_font_t *font,
                    lv_color_t color) {
  lv_obj_t *label = lv_label_create(parent);
  lv_label_set_text(label, text);
  lv_obj_set_style_text_font(label, font, 0);
  lv_obj_set_style_text_color(label, color, 0);
  return label;
}

// A plain filled rectangle/circle with none of the theme's container chrome.
lv_obj_t *makeShape(lv_obj_t *parent, lv_coord_t w, lv_coord_t h,
                    lv_color_t color, lv_coord_t radius) {
  lv_obj_t *obj = lv_obj_create(parent);
  lv_obj_remove_style_all(obj);
  lv_obj_set_size(obj, w, h);
  lv_obj_set_style_radius(obj, radius, 0);
  lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(obj, color, 0);
  lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE);
  return obj;
}

// A fixed-width label whose text alignment is set per-update, so it can be
// slid along x to follow a moving marker without re-measuring the text.
//
// The caller passes the width back in to every placement helper below and
// none of them ever call lv_obj_get_width(). That is deliberate: LVGL 8
// applies an explicit lv_obj_set_width() during the NEXT layout pass, so
// reading the width back here returns the auto-size of the "--" placeholder
// (~20px), not the 120/170px box that eventually gets drawn. Positioning
// math built on that stale number is what pushed the index number and band
// label off the right edge of the panel - and on the circular gauge, whose
// labels are placed once at build time and never re-placed, the wrong x
// stuck permanently.
lv_obj_t *makeCenteredLabel(lv_obj_t *parent, const lv_font_t *font,
                            lv_coord_t width, lv_coord_t centerX, lv_coord_t top) {
  lv_obj_t *label = makeLabel(parent, "--", font, figo_theme::dimGrey());
  lv_obj_set_width(label, width);
  lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_pos(label, centerX - width / 2, top);
  return label;
}

// Marker center x for a 0-100 score, matching the exported position frames
// (score 0 -> 43, score 100 -> 279).
lv_coord_t fgMarkerCenterX(float score) {
  if (score < 0.0f) score = 0.0f;
  if (score > 100.0f) score = 100.0f;
  return 43 + (lv_coord_t)lroundf(score * 2.36f);
}

// Same side margin the rest of the design uses (kMarginLeft), so an
// edge-anchored label lines up with the screen titles above it.
constexpr lv_coord_t kFgEdgeMargin = 19;
// Outside this band the label anchors to the screen edge instead of
// centering, so it can never run off the panel.
constexpr float kFgLeftAlignBelow = 10.0f;
constexpr float kFgRightAlignAbove = 90.0f;

// Places one of the slider's two labels under the marker. Score 10..90
// centers the text on the marker; above 90 it right-aligns to the right
// margin and below 10 it left-aligns to the left margin, which keeps the
// text on screen at the extremes while still reading as "under the dot".
void placeSliderLabel(lv_obj_t *label, lv_coord_t width, lv_coord_t markerCenterX,
                      float score) {
  lv_text_align_t align;
  lv_coord_t x;
  if (score > kFgRightAlignAbove) {
    align = LV_TEXT_ALIGN_RIGHT;
    x = 320 - kFgEdgeMargin - width;
  } else if (score < kFgLeftAlignBelow) {
    align = LV_TEXT_ALIGN_LEFT;
    x = kFgEdgeMargin;
  } else {
    align = LV_TEXT_ALIGN_CENTER;
    x = markerCenterX - width / 2;
    // Backstop at the edges of the centered band. A short string has enough
    // slack inside the fixed-width box to survive an overhanging box, but
    // the longest one ("EXTREME FEAR (stale)") fills the box completely and
    // would be clipped with it. Keeping the BOX on the panel costs a few
    // pixels of "exactly under the dot" at scores near 10 and 90 and is
    // never reached in the middle of the range, where the box fits either
    // way. Both bounds are satisfiable: 320 - 2*19 = 282 > the 170px widest
    // box.
    if (x < kFgEdgeMargin) x = kFgEdgeMargin;
    if (x + width > 320 - kFgEdgeMargin) x = 320 - kFgEdgeMargin - width;
  }
  lv_obj_set_style_text_align(label, align, 0);
  lv_obj_set_x(label, x);
}

void buildDualColumn(lv_obj_t *screen, const char *name, lv_coord_t colOffset,
                     lv_obj_t **priceOut, lv_obj_t **changeOut) {
  lv_obj_t *nameLabel = makeLabel(screen, name, FIGO_FONT_NAME_MD, figo_theme::textWhite());
  lv_obj_align(nameLabel, LV_ALIGN_TOP_MID, colOffset, kDualNameTop);

  *priceOut = makeLabel(screen, "--", FIGO_FONT_PRICE_MD, figo_theme::dimGrey());
  lv_obj_align(*priceOut, LV_ALIGN_TOP_MID, colOffset, kDualPriceTop);

  *changeOut = makeLabel(screen, "", FIGO_FONT_TINY, figo_theme::dimGrey());
  lv_obj_align(*changeOut, LV_ALIGN_TOP_MID, colOffset, kDualChangeTop);

  lv_obj_t *today = makeLabel(screen, "TODAY", FIGO_FONT_TINY, figo_theme::textWhite());
  lv_obj_align(today, LV_ALIGN_TOP_MID, colOffset, kDualTodayTop);
}

// Horizontal room a price label actually has, per layout.
//
// SINGLE: the full width between the side margins. The price row is clear
// across the whole panel - its ink runs y 71..108 (top 71, baseline 103),
// while the symbol above ends at ~33 and the change below starts at 110, so
// nothing shares that band and the price may grow right up to the margin.
constexpr lv_coord_t kSinglePriceMaxW = 320 - 2 * kMarginLeft;
// DUAL: each column is CENTRED on 160 -/+ 80 and must clear the 2px divider
// at x 159, so the usable half-width is min(80 - 6, 159 - 4 - 80) = 74.
constexpr lv_coord_t kDualPriceMaxW = 148;

// Largest font from a ladder whose rendering of the text fits maxWidth.
//
// Prices are not a bounded shape. 44px suits "26,500$", where
// exactly how wide seven digits and a "$" are. A currency CODE makes
// the string three characters longer, and at 30px "191.32 EUR" is 156px in a
// 148px dual column - it would silently overhang the divider. Rather than
// ellipsising (a truncated PRICE is worse than a smaller one) or dropping the
// code (which is the information the user asked for), the label steps down to
// the next compiled size that fits.
//
// Measured with lv_txt_get_width against the real compiled font, not
// estimated: assuming a widget's size instead of asking LVGL for it is a
// reliable source of clipping bugs. letter_space 0
// matches the theme, which sets none on these labels.
//
// Returns the SMALLEST font if nothing fits, so an absurd string shrinks as
// far as it can and then overflows visibly rather than being hidden.
const lv_font_t *fitFont(const char *text, lv_coord_t maxWidth,
                         const lv_font_t *const *ladder, size_t count) {
  for (size_t i = 0; i < count; i++) {
    lv_coord_t w = lv_txt_get_width(text, strlen(text), ladder[i], 0, LV_TEXT_FLAG_NONE);
    if (w <= maxWidth) return ladder[i];
  }
  return ladder[count - 1];
}

// Ladders start at the design's size and step down through sizes lv_conf.h
// already compiles - no new font is linked in for this.
const lv_font_t *const kSinglePriceFonts[] = {
    &lv_font_montserrat_44,  // the design's size, and what USD always uses
    &lv_font_montserrat_40, &lv_font_montserrat_36, &lv_font_montserrat_30};
const lv_font_t *const kDualPriceFonts[] = {
    &lv_font_montserrat_30,  // the design's size
    &lv_font_montserrat_24, &lv_font_montserrat_20};

// One ticker instrument's price+change update (shared by single, dual's two
// columns). Colors price and change by the change's sign per the design.
// Takes the currency the provider reported for THIS instrument, not the
// screen's: a DUAL screen can pair a Tel Aviv listing with a dollar-quoted
// one, so each column formats against its own value's code.
//
// errorCode (0 = none) only applies to a stale value: the price stays, dimmed,
// and the change label - which would otherwise read "-1.2% (stale)" - names the
// reason instead. Only existing labels are touched, so an error path allocates
// no LVGL objects.
void updateTickerPair(lv_obj_t *price, lv_obj_t *change, const char *currency,
                      float priceVal, float changeVal, bool stale, uint8_t errorCode,
                      lv_coord_t maxWidth, const lv_font_t *const *fonts,
                      size_t fontCount) {
  char priceText[24];
  char changeText[32];
  figo_theme::formatPrice(priceText, sizeof(priceText), priceVal, currency);
  figo_theme::formatChangePct(changeText, sizeof(changeText), changeVal);
  // Font before text: set_text triggers the layout that measures the label,
  // so changing the font afterwards would leave the first frame at the old
  // size. Re-applied on every update because the currency can change under a
  // screen (a retarget) and a shrunk label must be able to grow back.
  lv_obj_set_style_text_font(price, fitFont(priceText, maxWidth, fonts, fontCount), 0);
  lv_label_set_text(price, priceText);

  if (stale) {
    char staleText[48];
    // formatErrorLabel refuses anything but a two-digit code, so a 0 (or a
    // corrupt value) falls through to the plain "(stale)" text rather than
    // printing a wrong number.
    if (formatErrorLabel(staleText, sizeof(staleText), errorCode) == 0) {
      snprintf(staleText, sizeof(staleText), "%s (stale)", changeText);
    }
    lv_label_set_text(change, staleText);
    lv_obj_set_style_text_color(price, figo_theme::dimGrey(), 0);
    lv_obj_set_style_text_color(change, figo_theme::dimGrey(), 0);
    return;
  }
  lv_label_set_text(change, changeText);
  lv_color_t signColor =
      changeVal >= 0.0f ? figo_theme::positive() : figo_theme::negative();
  lv_obj_set_style_text_color(price, signColor, 0);
  lv_obj_set_style_text_color(change, signColor, 0);
}

// A screen with nothing to show yet. When it has failed enough times to carry an
// error code, the code takes the price label's place ("--" would hide the reason
// the screen is empty); the change label still says "unavailable". The price
// font is re-fitted every time, like updateTickerPair(): "Error 53" is wider
// than "--" and steps down the ladder, and the next real price must be able to
// grow the label back rather than inherit the shrunk size.
void setPlaceholder(lv_obj_t *price, lv_obj_t *change, bool unavailable, uint8_t errorCode,
                    lv_coord_t maxWidth, const lv_font_t *const *fonts, size_t fontCount) {
  char text[24];
  if (!unavailable || formatErrorLabel(text, sizeof(text), errorCode) == 0) {
    strcpy(text, "--");
  }
  lv_obj_set_style_text_font(price, fitFont(text, maxWidth, fonts, fontCount), 0);
  lv_label_set_text(price, text);
  lv_obj_set_style_text_color(price, figo_theme::dimGrey(), 0);
  lv_label_set_text(change, unavailable ? "unavailable" : "");
  lv_obj_set_style_text_color(change, figo_theme::dimGrey(), 0);
}

}  // namespace

namespace screen_builders {

void buildTickerSingle(lv_obj_t *screen, const ScreenConfig &cfg,
                       DataScreenWidgets &w) {
  lv_obj_t *name = makeLabel(screen, cfg.name, FIGO_FONT_NAME, figo_theme::textWhite());
  lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
  lv_obj_set_width(name, kNameMaxWidth);
  lv_obj_set_pos(name, kMarginLeft, kNameTop);

  lv_obj_t *symbol = makeLabel(screen, cfg.symbol, FIGO_FONT_SMALL, figo_theme::textWhite());
  lv_obj_align(symbol, LV_ALIGN_TOP_RIGHT, kSymbolInset, kSymbolTop);

  w.price = makeLabel(screen, "--", FIGO_FONT_PRICE_LG, figo_theme::dimGrey());
  lv_obj_set_pos(w.price, kMarginLeft, kPriceTop);

  w.change = makeLabel(screen, "", FIGO_FONT_LABEL, figo_theme::dimGrey());
  lv_obj_align(w.change, LV_ALIGN_TOP_RIGHT, kRightColInset, kChangeTop);

  lv_obj_t *today = makeLabel(screen, "TODAY", FIGO_FONT_LABEL, figo_theme::textWhite());
  lv_obj_align(today, LV_ALIGN_TOP_RIGHT, kRightColInset, kTodayTop);
}

void buildTickerDual(lv_obj_t *screen, const ScreenConfig &cfg,
                     DataScreenWidgets &w) {
  lv_obj_t *divider = makeShape(screen, 2, kDividerHeight, figo_theme::textWhite(), 0);
  lv_obj_set_pos(divider, kDividerX, kDividerTop);

  buildDualColumn(screen, cfg.name, -kDualColOffset, &w.price, &w.change);
  buildDualColumn(screen, cfg.name2, kDualColOffset, &w.price2, &w.change2);
}

void buildFearGreedSlider(lv_obj_t *screen, const ScreenConfig &cfg,
                          DataScreenWidgets &w) {
  lv_obj_t *title = makeLabel(screen, cfg.name, FIGO_FONT_NAME, figo_theme::textWhite());
  // Bounded + ellipsised. This label had no width at all, so it auto-sized:
  // a name like "Crypto Fear & Greed" at 36px is ~380px wide and simply ran
  // off the right of a 320px panel. The screen name is user-editable, so the
  // layout has to survive any length rather than trust a short one.
  lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
  lv_obj_set_width(title, kFgTitleMaxW);
  lv_obj_set_pos(title, kFgTitleLeft, kFgTitleTop);

  lv_obj_t *track = makeShape(screen, kFgTrackW, kFgTrackH, figo_theme::trackLight(),
                              kFgTrackH / 2);
  lv_obj_set_pos(track, kFgTrackX, kFgTrackY);

  w.fgMarker = makeShape(screen, kFgMarkerSize, kFgMarkerSize,
                         figo_theme::dimGrey(), LV_RADIUS_CIRCLE);
  lv_obj_set_pos(w.fgMarker, fgMarkerCenterX(50.0f) - kFgMarkerSize / 2, kFgMarkerTop);

  w.fgScore = makeCenteredLabel(screen, FIGO_FONT_INDEX_MD, kFgSliderScoreW,
                                fgMarkerCenterX(50.0f), kFgSliderScoreTop);
  w.fgBand = makeCenteredLabel(screen, FIGO_FONT_SMALL, kFgSliderBandW,
                               fgMarkerCenterX(50.0f), kFgSliderBandTop);
  lv_label_set_text(w.fgBand, "");
}

void buildFearGreedCircular(lv_obj_t *screen, const ScreenConfig &cfg,
                            DataScreenWidgets &w) {
  lv_obj_t *title = makeLabel(screen, cfg.name, FIGO_FONT_NAME, figo_theme::textWhite());
  // Two-line title per the design ("CRYPTO / F&G"): wrap at a width that
  // clears the gauge, with the design's ~10px extra leading.
  lv_obj_set_width(title, kFgCircTitleW);
  lv_obj_set_style_text_line_space(title, 10, 0);
  lv_obj_set_pos(title, kFgTitleLeft, kFgTitleTop);

  w.fgArc = lv_arc_create(screen);
  lv_obj_set_size(w.fgArc, kFgArcSize, kFgArcSize);
  lv_obj_set_pos(w.fgArc, kFgArcX, kFgArcY);
  lv_arc_set_bg_angles(w.fgArc, kFgArcStartAngle, kFgArcEndAngle);
  lv_arc_set_range(w.fgArc, 0, 100);
  lv_arc_set_value(w.fgArc, 50);
  // Track on MAIN; the value indicator arc is invisible - the design shows
  // only the grey track plus a colored KNOB at the score position, and the
  // knob conveniently rides the indicator's end angle.
  lv_obj_set_style_arc_width(w.fgArc, kFgArcWidth, LV_PART_MAIN);
  lv_obj_set_style_arc_color(w.fgArc, figo_theme::trackDark(), LV_PART_MAIN);
  lv_obj_set_style_arc_rounded(w.fgArc, true, LV_PART_MAIN);
  lv_obj_set_style_arc_opa(w.fgArc, LV_OPA_TRANSP, LV_PART_INDICATOR);
  lv_obj_set_style_arc_width(w.fgArc, kFgArcWidth, LV_PART_INDICATOR);
  lv_obj_set_style_pad_all(w.fgArc, kFgKnobPad, LV_PART_KNOB);
  lv_obj_set_style_bg_color(w.fgArc, figo_theme::dimGrey(), LV_PART_KNOB);
  // Display-only gauge: never a touch control.
  lv_obj_clear_flag(w.fgArc, LV_OBJ_FLAG_CLICKABLE);

  // Fixed in the middle of the gauge for every score - unlike the slider's,
  // these labels never track a marker, so they are placed once here and
  // updateDataScreen() only ever rewrites their text and color.
  w.fgScore = makeCenteredLabel(screen, FIGO_FONT_INDEX_LG, kFgCircScoreW,
                                kFgCircCenterX, kFgCircScoreTop);
  w.fgBand = makeCenteredLabel(screen, FIGO_FONT_LABEL, kFgCircBandW,
                               kFgCircCenterX, kFgCircBandTop);
  lv_label_set_text(w.fgBand, "");
}

void updateDataScreen(const ScreenConfig &cfg, const DataScreenWidgets &w,
                      const ScreenValue *value) {
  bool noData = (!value || value->last_update_ms == 0);
  bool unavailable = (value && value->stale);

  // --- Fear & greed gauges ---
  if (w.fgScore && w.fgBand) {
    if (noData) {
      lv_label_set_text(w.fgScore, "--");
      lv_obj_set_style_text_color(w.fgScore, figo_theme::dimGrey(), 0);
      // Both F&G layouts have a band label but no price label, so the code goes
      // here in place of "unavailable"; the score stays "--".
      char noDataText[24];
      if (!unavailable) {
        noDataText[0] = '\0';
      } else if (formatErrorLabel(noDataText, sizeof(noDataText), value->error_code) == 0) {
        strcpy(noDataText, "unavailable");
      }
      lv_label_set_text(w.fgBand, noDataText);
      lv_obj_set_style_text_color(w.fgBand, figo_theme::dimGrey(), 0);
      if (w.fgMarker) lv_obj_set_style_bg_color(w.fgMarker, figo_theme::dimGrey(), 0);
      if (w.fgArc) lv_obj_set_style_bg_color(w.fgArc, figo_theme::dimGrey(), LV_PART_KNOB);
      return;
    }

    float score = value->price;
    lv_color_t bandColor =
        value->stale ? figo_theme::dimGrey() : figo_theme::fearGreedBandColor(score);
    char scoreText[8];
    snprintf(scoreText, sizeof(scoreText), "%.0f", score);
    lv_label_set_text(w.fgScore, scoreText);
    lv_obj_set_style_text_color(w.fgScore, bandColor, 0);

    // Stale with a reason: the score stays (dimmed) and the band label names the
    // error instead of "EXTREME FEAR (stale)". "Error NN" is shorter than the
    // longest rating string the boxes were sized for, so it cannot overflow.
    char bandText[32];
    if (!value->stale || formatErrorLabel(bandText, sizeof(bandText), value->error_code) == 0) {
      snprintf(bandText, sizeof(bandText), value->stale ? "%s (stale)" : "%s",
               figo_theme::fearGreedRating(score));
    }
    lv_label_set_text(w.fgBand, bandText);
    lv_obj_set_style_text_color(w.fgBand, bandColor, 0);

    // Slider only: both labels ride the marker. The circular gauge's labels
    // are fixed at the arc's center by buildFearGreedCircular() and are
    // deliberately not touched here.
    if (w.fgMarker) {
      lv_coord_t centerX = fgMarkerCenterX(score);
      lv_obj_set_x(w.fgMarker, centerX - kFgMarkerSize / 2);
      lv_obj_set_style_bg_color(w.fgMarker, bandColor, 0);
      placeSliderLabel(w.fgScore, kFgSliderScoreW, centerX, score);
      placeSliderLabel(w.fgBand, kFgSliderBandW, centerX, score);
    }
    if (w.fgArc) {
      lv_arc_set_value(w.fgArc, (int16_t)lroundf(score));
      lv_obj_set_style_bg_color(w.fgArc, bandColor, LV_PART_KNOB);
    }
    return;
  }

  // --- Tickers (single + dual) ---
  if (!w.price || !w.change) return;  // slot mid-rebuild
  // w.price2 is what distinguishes the two layouts here - the dual builder is
  // the only one that creates a second column.
  bool dual = (w.price2 && w.change2);
  lv_coord_t maxW = dual ? kDualPriceMaxW : kSinglePriceMaxW;
  const lv_font_t *const *fonts = dual ? kDualPriceFonts : kSinglePriceFonts;
  size_t fontCount = dual ? (sizeof(kDualPriceFonts) / sizeof(kDualPriceFonts[0]))
                          : (sizeof(kSinglePriceFonts) / sizeof(kSinglePriceFonts[0]));

  if (noData) {
    // value can be null here (noData covers !value), and only a real value has a code.
    uint8_t code = value ? value->error_code : 0;
    setPlaceholder(w.price, w.change, unavailable, code, maxW, fonts, fontCount);
    if (dual) setPlaceholder(w.price2, w.change2, unavailable, code, maxW, fonts, fontCount);
    return;
  }

  // A DUAL screen is all-or-nothing, so its one error_code applies to both columns.
  updateTickerPair(w.price, w.change, value->currency, value->price,
                   value->change_pct, value->stale, value->error_code, maxW, fonts, fontCount);
  if (dual) {
    updateTickerPair(w.price2, w.change2, value->currency2, value->price2,
                     value->change_pct2, value->stale, value->error_code, maxW, fonts, fontCount);
  }
}

lv_obj_t *addDotIndicator(lv_obj_t *screen, uint8_t index, uint8_t total) {
  if (total < 2) return nullptr;  // a lone screen needs no pager

  lv_obj_t *row = lv_obj_create(screen);
  lv_obj_remove_style_all(row);  // plain container: no theme bg/border/pad
  lv_obj_set_size(row, (total - 1) * kDotPitch + kDotActiveSize, kDotActiveSize);
  lv_obj_align(row, LV_ALIGN_BOTTOM_MID, 0, kDotRowBottomOffset);
  lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

  for (uint8_t i = 0; i < total; i++) {
    bool active = (i == index);
    lv_coord_t size = active ? kDotActiveSize : kDotInactiveSize;
    lv_obj_t *dot = lv_obj_create(row);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, size, size);
    // Position by center: centers sit at i*pitch + activeRadius.
    lv_obj_set_pos(dot, i * kDotPitch + (kDotActiveSize - size) / 2,
                   (kDotActiveSize - size) / 2);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(
        dot, active ? figo_theme::dotActive() : figo_theme::dotInactive(), 0);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);
  }
  return row;
}

}  // namespace screen_builders
