#ifndef LEDCONTROLLER_H
#define LEDCONTROLLER_H

#include <Arduino.h>
#include <PCA9632.h>
#include <Wire.h>

class LedController {
  public:
    LedController(TwoWire *i2c);
    void setup();
    bool isAvailable();
    void setChannel(uint8_t channel, uint8_t brightness);
    void disable();

  private:
    bool initialize();

    PCA9632 *pca9632 = nullptr;
    bool initialized = false;
};

#endif // LEDCONTROLLER_H