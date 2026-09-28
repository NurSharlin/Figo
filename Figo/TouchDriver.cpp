#include "TouchDriver.h"
#include "esp_lcd_touch_axs5106l.h"
#include "Config.h"
#include <Wire.h>

void TouchDriver::begin(uint16_t rotation, uint16_t width, uint16_t height) {
  Wire.begin(PIN_TOUCH_SDA, PIN_TOUCH_SCL);
  bsp_touch_init(&Wire, PIN_TOUCH_RST, PIN_TOUCH_INT, rotation, width, height);
}

bool TouchDriver::read(int16_t &x, int16_t &y) {
  bsp_touch_read();

  touch_data_t touch_data;
  if (!bsp_touch_get_coordinates(&touch_data)) {
    return false;
  }

  x = touch_data.coords[0].x;
  y = touch_data.coords[0].y;
  return true;
}
