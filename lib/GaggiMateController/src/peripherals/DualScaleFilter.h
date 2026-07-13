#pragma once
#if defined(ARDUINO_ARCH_STM32) && !defined(GAGGIMATE_DISABLE_HARDWARE_SCALE)

#include <Arduino.h>
#include <stddef.h>

// Adapted from WeighMyBru2 by 031devstudios:
// https://github.com/031devstudios/weighmybru2
// Copyright (c) 2025 031devstudios. Licensed under CC BY-NC-SA 4.0:
// https://creativecommons.org/licenses/by-nc-sa/4.0/
// Changes: coordinated filtering for two independently calibrated HX711
// channels, combined-weight activity detection, and per-channel diagnostics.

struct DualScaleFilteredReading {
    float totalG = 0.0f;
    float ch1G = 0.0f;
    float ch2G = 0.0f;
    float ch1StdG = 0.0f;
    float ch2StdG = 0.0f;
};

// WeighMyBru2-style dynamic filtering adapted to two load-cell channels.
// Activity is detected from the summed tray weight, then the same filter mode
// is applied to both channels so their filtered values still add exactly.
class DualScaleFilter {
  public:
    DualScaleFilteredReading push(float ch1G, float ch2G, uint32_t nowMs);
    void reset();

  private:
    enum class State : uint8_t { STABLE, BREWING, TRANSITIONING };

    static constexpr size_t MAX_SAMPLES = 10;
    static constexpr size_t STDDEV_SAMPLES = 32;
    static constexpr size_t STABLE_AVERAGE_SAMPLES = 2;
    static constexpr size_t BREWING_MEDIAN_SAMPLES = 3;
    static constexpr float BREWING_THRESHOLD_G = 0.15f;
    static constexpr float IMMEDIATE_RESPONSE_G = 5.0f;
    static constexpr uint32_t STABILITY_TIMEOUT_MS = 2000;

    float _readings1[MAX_SAMPLES]{};
    float _readings2[MAX_SAMPLES]{};
    size_t _readingIndex = 0;
    size_t _readingCount = 0;
    bool _initialized = false;

    State _state = State::STABLE;
    uint32_t _lastBrewingActivityMs = 0;
    float _currentCh1 = 0.0f;
    float _currentCh2 = 0.0f;

    float _stdReadings1[STDDEV_SAMPLES]{};
    float _stdReadings2[STDDEV_SAMPLES]{};
    size_t _stdIndex = 0;
    size_t _stdCount = 0;

    void initializeSamples(float ch1G, float ch2G);
    float average(const float *readings, size_t samples) const;
    float median(const float *readings, size_t samples) const;
    void recordOutput(float ch1G, float ch2G);
    float stddev(const float *readings) const;
};

#endif
