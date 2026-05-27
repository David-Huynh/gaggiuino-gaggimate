#pragma once
#if defined(ARDUINO_ARCH_STM32) && !defined(GAGGIMATE_DISABLE_HARDWARE_SCALE)
#include <Arduino.h>

#include "SimpleKalmanFilter/SimpleKalmanFilter.h"

// Per-channel DSP pipeline for one HX711 cell:
//   raw grams (already calibrated upstream) → Median(MEDIAN_WINDOW) → Kalman → output
// A rolling stddev over the last STDDEV_WINDOW outputs is kept for settling
// checks (tare/cal) and noise-health UI.
//
// Pure DSP — no FreeRTOS dependency, no ownership of any peripheral. The class
// is designed to be cheap to copy/reset; reset() primes the filter from the
// next sample so the first post-tare reading does not jump.
class ChannelFilter {
  public:
    static constexpr size_t MEDIAN_WINDOW = 5;
    static constexpr size_t STDDEV_WINDOW = 32;

    ChannelFilter();

    // Push one calibrated sample (grams). Returns the filtered output.
    float push(float grams);

    // Discard internal state. The next push() will seed the pipeline from that
    // value with no transient — used after tare or calibration.
    void reset();

    float current() const { return _current; }
    float stddev() const;

    // Number of samples currently held in the rolling stddev window.
    size_t stddevSamples() const { return _stdCount; }

  private:
    float _medianRing[MEDIAN_WINDOW]{};
    size_t _medianIdx = 0;
    size_t _medianCount = 0;

    float _stdRing[STDDEV_WINDOW]{};
    size_t _stdIdx = 0;
    size_t _stdCount = 0;

    SimpleKalmanFilter _kalman;
    float _current = 0.0f;
    bool _primed = false;
};
#endif
