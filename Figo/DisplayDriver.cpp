#include "DisplayDriver.h"
#include "Config.h"

bool DisplayDriver::begin(uint16_t rotationDegrees) {
  bus_ = new Arduino_HWSPI(PIN_LCD_DC, PIN_LCD_CS, PIN_LCD_SCK, PIN_LCD_MOSI);
  gfx_ = new Arduino_ST7789(
      bus_, PIN_LCD_RST, 0 /* rotation applied via setRotation below */, false /* IPS */,
      PANEL_WIDTH, PANEL_HEIGHT,
      34 /* col_offset1 */, 0 /* row_offset1 */,
      34 /* col_offset2 */, 0 /* row_offset2 */);

  if (!gfx_->begin()) {
    return false;
  }

  lcdRegInit();
  // 0 degrees (user-facing) -> Arduino_GFX rotation code 1; 180 -> code 3 -
  // this panel's two landscape mounting orientations (Config.h's
  // PANEL_ROTATION comment). rotationDegrees is validated to be exactly 0
  // or 180 by ConfigManager::updateSystem() before it ever reaches NVS, so
  // anything else here defaults to the 0-degree code rather than silently
  // going to portrait.
  gfx_->setRotation(rotationDegrees == 180 ? 3 : 1);
  gfx_->fillScreen(RGB565_BLACK);

  // PWM backlight (core 3.x LEDC API) so night mode can dim to 0
  // instead of the vendor demo's plain digitalWrite(HIGH) on/off.
  ledcAttach(PIN_LCD_BL, 5000 /* Hz */, 8 /* resolution bits */);
  setBacklight(255);

  return true;
}

void DisplayDriver::flush(int32_t x1, int32_t y1, int32_t x2, int32_t y2, uint16_t *pixelData) {
  uint32_t w = x2 - x1 + 1;
  uint32_t h = y2 - y1 + 1;
  // Byte-swapped variant - matches lv_conf.h's LV_COLOR_16_SWAP 1, which is
  // required for correct colors on this panel (confirmed from Waveshare's
  // own working lv_conf.h).
  gfx_->draw16bitBeRGBBitmap(x1, y1, pixelData, w, h);
}

void DisplayDriver::setBacklight(uint8_t pwmDuty) {
  ledcWrite(PIN_LCD_BL, pwmDuty);
}

void DisplayDriver::lcdRegInit() {
  // Exact byte sequence extracted from Waveshare's official Arduino demo
  // (Arduino/examples/04_lvgl_arduino_v8/04_lvgl_arduino_v8.ino,
  // lcd_reg_init()) for the JD9853 panel used on this board. JD9853 is
  // close to ST7789-command-compatible but needs these extra registers -
  // do not "clean up" or reorder without re-testing on a real panel.
  static const uint8_t init_operations[] = {
    BEGIN_WRITE,
    WRITE_COMMAND_8, 0x11,
    END_WRITE,
    DELAY, 120,

    BEGIN_WRITE,
    WRITE_C8_D16, 0xDF, 0x98, 0x53,
    WRITE_C8_D8, 0xB2, 0x23,

    WRITE_COMMAND_8, 0xB7,
    WRITE_BYTES, 4,
    0x00, 0x47, 0x00, 0x6F,

    WRITE_COMMAND_8, 0xBB,
    WRITE_BYTES, 6,
    0x1C, 0x1A, 0x55, 0x73, 0x63, 0xF0,

    WRITE_C8_D16, 0xC0, 0x44, 0xA4,
    WRITE_C8_D8, 0xC1, 0x16,

    WRITE_COMMAND_8, 0xC3,
    WRITE_BYTES, 8,
    0x7D, 0x07, 0x14, 0x06, 0xCF, 0x71, 0x72, 0x77,

    WRITE_COMMAND_8, 0xC4,
    WRITE_BYTES, 12,
    0x00, 0x00, 0xA0, 0x79, 0x0B, 0x0A, 0x16, 0x79, 0x0B, 0x0A, 0x16, 0x82,

    WRITE_COMMAND_8, 0xC8,
    WRITE_BYTES, 32,
    0x3F, 0x32, 0x29, 0x29, 0x27, 0x2B, 0x27, 0x28, 0x28, 0x26, 0x25, 0x17, 0x12, 0x0D, 0x04, 0x00,
    0x3F, 0x32, 0x29, 0x29, 0x27, 0x2B, 0x27, 0x28, 0x28, 0x26, 0x25, 0x17, 0x12, 0x0D, 0x04, 0x00,

    WRITE_COMMAND_8, 0xD0,
    WRITE_BYTES, 5,
    0x04, 0x06, 0x6B, 0x0F, 0x00,

    WRITE_C8_D16, 0xD7, 0x00, 0x30,
    WRITE_C8_D8, 0xE6, 0x14,
    WRITE_C8_D8, 0xDE, 0x01,

    WRITE_COMMAND_8, 0xB7,
    WRITE_BYTES, 5,
    0x03, 0x13, 0xEF, 0x35, 0x35,

    WRITE_COMMAND_8, 0xC1,
    WRITE_BYTES, 3,
    0x14, 0x15, 0xC0,

    WRITE_C8_D16, 0xC2, 0x06, 0x3A,
    WRITE_C8_D16, 0xC4, 0x72, 0x12,
    WRITE_C8_D8, 0xBE, 0x00,
    WRITE_C8_D8, 0xDE, 0x02,

    WRITE_COMMAND_8, 0xE5,
    WRITE_BYTES, 3,
    0x00, 0x02, 0x00,

    WRITE_COMMAND_8, 0xE5,
    WRITE_BYTES, 3,
    0x01, 0x02, 0x00,

    WRITE_C8_D8, 0xDE, 0x00,
    WRITE_C8_D8, 0x35, 0x00,
    WRITE_C8_D8, 0x3A, 0x05,

    WRITE_COMMAND_8, 0x2A,
    WRITE_BYTES, 4,
    0x00, 0x22, 0x00, 0xCD,

    WRITE_COMMAND_8, 0x2B,
    WRITE_BYTES, 4,
    0x00, 0x00, 0x01, 0x3F,

    WRITE_C8_D8, 0xDE, 0x02,

    WRITE_COMMAND_8, 0xE5,
    WRITE_BYTES, 3,
    0x00, 0x02, 0x00,

    WRITE_C8_D8, 0xDE, 0x00,
    WRITE_C8_D8, 0x36, 0x00,
    WRITE_COMMAND_8, 0x21,
    END_WRITE,

    DELAY, 10,

    BEGIN_WRITE,
    WRITE_COMMAND_8, 0x29,
    END_WRITE
  };
  bus_->batchOperation(init_operations, sizeof(init_operations));
}
