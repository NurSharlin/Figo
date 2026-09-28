#ifndef TOUCH_DRIVER_H
#define TOUCH_DRIVER_H

#include <Arduino.h>

class TouchDriver {
 public:
  void begin(uint16_t rotation, uint16_t width, uint16_t height);

  // Polls the AXS5106L and reports the primary touch point, already
  // rotation-corrected. Returns false when no finger is down.
  bool read(int16_t &x, int16_t &y);
};

#endif // TOUCH_DRIVER_H
