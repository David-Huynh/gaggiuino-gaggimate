#if defined(ARDUINO_ARCH_STM32)
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

#else
#ifndef DISTANCESENSOR_H
#define DISTANCESENSOR_H

#include "SoftWireBus.h"
#include <Arduino.h>
#include <VL35L0X/VL53L0X.h>
#include <memory>

using distance_callback_t = std::function<void(int)>;

class DistanceSensor {
  public:
    DistanceSensor(SoftWireBus *bus, distance_callback_t callback);
    void setup();
    bool isHealthy() const { return healthy; }

  private:
    void loop();
    bool initSensor();
    bool readRange(uint16_t &range);
    bool checkIdentity();
    void recover();

    SoftWireBus *bus;
    std::unique_ptr<VL53L0X> tof;
    xTaskHandle taskHandle;
    distance_callback_t _callback;
    int measurements = 0;
    int currentMillis = 0;

    bool healthy = false;
    uint8_t failures = 0;
    uint8_t staleReads = 0;
    uint8_t pollsSinceIdentityCheck = 0;
    unsigned long lastRecovery = 0;
    unsigned long recoveryBackoff = 0;

    const char *LOG_TAG = "DistanceSensor";
    static void loopTask(void *arg);
};

#endif // DISTANCESENSOR_H

#endif
