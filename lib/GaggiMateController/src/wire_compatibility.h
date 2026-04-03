#pragma once

#include <Arduino.h>

// Wire bus compatibility for cross-platform I2C
// On ESP32: Wire and Wire1 are separate I2C buses
// On STM32: Only Wire (I2C1) exists. Wire1 is aliased to Wire.

#ifdef ARDUINO_ARCH_STM32
    // STM32 - Wire1 alias to Wire (single I2C bus)
    #define Wire1 Wire
#endif

// Helper to initialize I2C with platform-specific handling
inline bool initI2CBus(TwoWire& bus, uint8_t sda_pin, uint8_t scl_pin, uint32_t frequency = 400000) {
    #ifdef ARDUINO_ARCH_STM32
        // STM32 - pins and frequency ignored (configured by HAL)
        // Assume Wire is already initialized in main
        (void)sda_pin;  // Suppress unused parameter warnings
        (void)scl_pin;
        (void)frequency;
        return true;
    #else
        // ESP32 - can specify pins and frequency
        return bus.begin(sda_pin, scl_pin, frequency);
    #endif
}
