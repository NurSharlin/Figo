#ifndef SCREEN_BUILDERS_H
#define SCREEN_BUILDERS_H

#include <lvgl.h>
#include "SharedState.h"

// Handles to every mutable widget on one data screen, retained so
// ScreenManager::applySnapshot() can update values without creating or
// deleting any LVGL object (the carousel's hot-path contract). Unused slots
// stay null - which widgets exist depends on the screen's type/layout, and
// updateDataScreen() dispatches on which ones are present.
struct DataScreenWidgets {
  lv_obj_t *price = nullptr;    // single ticker price / dual left price
  lv_obj_t *change = nullptr;   // single ticker change / dual left change
  lv_obj_t *price2 = nullptr;   // dual right price
  lv_obj_t *change2 = nullptr;  // dual right change
  lv_obj_t *fgScore = nullptr;  // fear & greed index number
  lv_obj_t *fgBand = nullptr;   // fear & greed band label
  lv_obj_t *fgMarker = nullptr; // fear & greed slider marker (slider layout)
  lv_obj_t *fgArc = nullptr;    // fear & greed arc (circular layout)
};

namespace screen_builders {

// "Single Ticker Price": name top-left 36px, symbol top-right 16px, price
// left 44px sign-colored, change/TODAY right-aligned 20px.
void buildTickerSingle(lv_obj_t *screen, const ScreenConfig &cfg, DataScreenWidgets &w);

// "Double Ticker Price": white 2px divider at x=160, one name/price/change/
// TODAY column centered in each half (24/30/14px).
void buildTickerDual(lv_obj_t *screen, const ScreenConfig &cfg, DataScreenWidgets &w);

// "Fear & Greed Slider": title 36px, light-grey 8px track at y=80, 20px
// band-colored marker whose x tracks the score, 40px index number and 16px
// band label centered under the marker.
void buildFearGreedSlider(lv_obj_t *screen, const ScreenConfig &cfg, DataScreenWidgets &w);

// "Fear & Greed Circular Bar": two-line title 36px, 252-degree dark-grey arc
// (center (235,99), r 67, LVGL bg angles 142->34) with a band-colored knob at
// the score position, 48px index number and 20px band label in the middle.
void buildFearGreedCircular(lv_obj_t *screen, const ScreenConfig &cfg, DataScreenWidgets &w);

// Writes the latest fetched value into a screen's widgets, dispatching on
// which widgets exist. Mutates pre-created widgets only (text, color,
// position, arc value); it must never create or delete an LVGL object.
// value == null or a never-fetched value renders the "--" placeholder.
void updateDataScreen(const ScreenConfig &cfg, const DataScreenWidgets &w, const ScreenValue *value);

// Adds the carousel position indicator: one dot per screen, bottom-center
// (active 8px #C2C2C2, inactive 6px #676767, 11px pitch). Returns the container so callers that need to
// refresh it on a count change (the face screen) can delete and re-add it.
lv_obj_t *addDotIndicator(lv_obj_t *screen, uint8_t index, uint8_t total);

}  // namespace screen_builders

#endif  // SCREEN_BUILDERS_H
