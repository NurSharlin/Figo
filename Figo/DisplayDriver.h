#ifndef DISPLAY_DRIVER_H
#define DISPLAY_DRIVER_H

#include <Arduino.h>
#include <Arduino_GFX_Library.h>

class DisplayDriver {
 public:
  // rotationDegrees is the user-facing landscape orientation from
  // RuntimeConfig.system.rotation (SharedState.h) - exactly 0 or 180, never
  // a raw Arduino_GFX rotation code. Mapped internally to this panel's two
  // landscape setRotation() codes (see Config.h's panel geometry comment:
  // 1/3 are landscape for this panel's mounting orientation).
  bool begin(uint16_t rotationDegrees);

  // Pushes a rect of RGB565 pixel data (byte-swapped - see lv_conf.h's
  // LV_COLOR_16_SWAP) to the panel. Called from DisplayManager's LVGL
  // flush_cb once per partial-buffer chunk.
  void flush(int32_t x1, int32_t y1, int32_t x2, int32_t y2, uint16_t *pixelData);

  void setBacklight(uint8_t pwmDuty);

  Arduino_GFX *gfx() { return gfx_; }

 private:
  // JD9853 is close to ST7789-command-compatible but needs this extra
  // vendor init-register sequence (extracted from Waveshare's own Arduino
  // demo) after Arduino_ST7789::begin() - without it colors/geometry are wrong.
  void lcdRegInit();

  Arduino_DataBus *bus_ = nullptr;
  Arduino_GFX *gfx_ = nullptr;
};

#endif // DISPLAY_DRIVER_H
