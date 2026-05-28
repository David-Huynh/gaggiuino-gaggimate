#if defined(ARDUINO_ARCH_STM32) && !defined(GAGGIMATE_DISABLE_HARDWARE_SCALE)
#include "ChannelFilter.h"

#include <math.h>

namespace {
inline void swap_floats(float &a, float &b) {
    float t = a;
    a = b;
    b = t;
}

float median_of(const float *src, size_t n) {
    float buf[ChannelFilter::MEDIAN_WINDOW];
    for (size_t i = 0; i < n; i++) {
        buf[i] = src[i];
    }
    // Insertion sort: n <= 5, so this is faster than anything fancy.
    for (size_t i = 1; i < n; i++) {
        for (size_t j = i; j > 0 && buf[j - 1] > buf[j]; j--) {
            swap_floats(buf[j - 1], buf[j]);
        }
    }
    return buf[n / 2];
}
} // namespace

ChannelFilter::ChannelFilter()
    // R = 0.05 g (typical noise floor for a clean HX711 channel),
    // P = 0.05 g (initial uncertainty),
    // Q = 0.012 g/sample. Median(5) upstream still absorbs single-sample HX711
    // spikes, so the output can respond quickly without obvious display noise.
    : _kalman(0.05f, 0.05f, 0.012f) {}

void ChannelFilter::reset() {
    _medianIdx = 0;
    _medianCount = 0;
    _stdIdx = 0;
    _stdCount = 0;
    _current = 0.0f;
    _primed = false;
}

float ChannelFilter::push(float grams) {
    static constexpr float RESPONSE_BOOST_THRESHOLD_G = 0.25f;
    static constexpr float RESPONSE_BOOST_ERROR_G = 0.5f;

    _medianRing[_medianIdx] = grams;
    _medianIdx = (_medianIdx + 1) % MEDIAN_WINDOW;
    if (_medianCount < MEDIAN_WINDOW) {
        ++_medianCount;
    }

    const float median = median_of(_medianRing, _medianCount);

    if (!_primed) {
        // Seed the Kalman estimator with the first valid median. SimpleKalmanFilter
        // has no public seed API, so feed the same value a few times to converge
        // quickly without a visible step.
        for (int i = 0; i < 4; ++i) {
            _kalman.updateEstimate(median);
        }
        _current = _kalman.getCurrentEstimate();
        _primed = true;
    } else {
        // A median move has already survived the spike filter. Temporarily raise
        // estimator uncertainty so real step changes settle quickly instead of
        // creeping for several seconds.
        if (fabsf(median - _current) > RESPONSE_BOOST_THRESHOLD_G) {
            _kalman.setEstimateError(RESPONSE_BOOST_ERROR_G);
        }
        _current = _kalman.updateEstimate(median);
    }

    _stdRing[_stdIdx] = _current;
    _stdIdx = (_stdIdx + 1) % STDDEV_WINDOW;
    if (_stdCount < STDDEV_WINDOW) {
        ++_stdCount;
    }

    return _current;
}

float ChannelFilter::stddev() const {
    if (_stdCount < 2) {
        return 0.0f;
    }
    float sum = 0.0f;
    for (size_t i = 0; i < _stdCount; i++) {
        sum += _stdRing[i];
    }
    const float mean = sum / static_cast<float>(_stdCount);
    float ss = 0.0f;
    for (size_t i = 0; i < _stdCount; i++) {
        const float d = _stdRing[i] - mean;
        ss += d * d;
    }
    return sqrtf(ss / static_cast<float>(_stdCount - 1));
}
#endif
