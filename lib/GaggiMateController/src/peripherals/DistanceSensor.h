#ifndef DISTANCESENSOR_H
#define DISTANCESENSOR_H
#ifdef ARDUINO_ARCH_STM32
#include <STM32FreeRTOS.h>
#include <VL53L0X.h>
#include <Wire.h>
#else
#include <SoftWire.h>
#include <VL35L0X/VL53L0X.h>
#endif
#include <Arduino.h>

using distance_callback_t = std::function<void(int)>;

class DistanceSensor {
  public:
#ifdef ARDUINO_ARCH_STM32
    DistanceSensor(TwoWire *wire, distance_callback_t callback);
#else
    DistanceSensor(SoftWire *wire, distance_callback_t callback);
#endif
    void setup();

  private:
    void loop();

#ifdef ARDUINO_ARCH_STM32
    TwoWire *i2c;
#else
    SoftWire *i2c;
#endif
    VL53L0X *tof;
    xTaskHandle taskHandle;
    distance_callback_t _callback;
    int measurements = 0;
    int currentMillis = 0;

    const char *LOG_TAG = "DistanceSensor";
    static void loopTask(void *arg);
};

#endif // DISTANCESENSOR_H
