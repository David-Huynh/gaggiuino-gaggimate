#if defined(ARDUINO_ARCH_STM32) && !defined(GAGGIMATE_DISABLE_HARDWARE_SCALE)

#include "DualScaleFilter.h"

#include <algorithm>
#include <cmath>

void DualScaleFilter::reset() {
    _readingIndex = 0;
    _readingCount = 0;
    _initialized = false;
    _state = State::STABLE;
    _lastBrewingActivityMs = 0;
    _currentCh1 = 0.0f;
    _currentCh2 = 0.0f;
    _stdIndex = 0;
    _stdCount = 0;
}

void DualScaleFilter::initializeSamples(float ch1G, float ch2G) {
    for (size_t i = 0; i < MAX_SAMPLES; ++i) {
        _readings1[i] = ch1G;
        _readings2[i] = ch2G;
    }
    _readingIndex = 0;
    _readingCount = MAX_SAMPLES;
    _initialized = true;
}

float DualScaleFilter::average(const float *readings, size_t samples) const {
    samples = std::min(samples, _readingCount);
    if (samples == 0) {
        return 0.0f;
    }

    float sum = 0.0f;
    for (size_t i = 0; i < samples; ++i) {
        const size_t index = (_readingIndex + MAX_SAMPLES - 1 - i) % MAX_SAMPLES;
        sum += readings[index];
    }
    return sum / static_cast<float>(samples);
}

float DualScaleFilter::median(const float *readings, size_t samples) const {
    samples = std::min(samples, _readingCount);
    if (samples == 0) {
        return 0.0f;
    }

    float values[BREWING_MEDIAN_SAMPLES]{};
    for (size_t i = 0; i < samples; ++i) {
        const size_t index = (_readingIndex + MAX_SAMPLES - 1 - i) % MAX_SAMPLES;
        values[i] = readings[index];
    }
    std::sort(values, values + samples);
    return values[samples / 2];
}

void DualScaleFilter::recordOutput(float ch1G, float ch2G) {
    _stdReadings1[_stdIndex] = ch1G;
    _stdReadings2[_stdIndex] = ch2G;
    _stdIndex = (_stdIndex + 1) % STDDEV_SAMPLES;
    if (_stdCount < STDDEV_SAMPLES) {
        ++_stdCount;
    }
}

float DualScaleFilter::stddev(const float *readings) const {
    if (_stdCount < 2) {
        return 0.0f;
    }

    float sum = 0.0f;
    for (size_t i = 0; i < _stdCount; ++i) {
        sum += readings[i];
    }
    const float mean = sum / static_cast<float>(_stdCount);
    float squaredError = 0.0f;
    for (size_t i = 0; i < _stdCount; ++i) {
        const float delta = readings[i] - mean;
        squaredError += delta * delta;
    }
    return sqrtf(squaredError / static_cast<float>(_stdCount - 1));
}

DualScaleFilteredReading DualScaleFilter::push(float ch1G, float ch2G, uint32_t nowMs) {
    const float rawTotal = ch1G + ch2G;

    if (!_initialized) {
        initializeSamples(ch1G, ch2G);
        _currentCh1 = ch1G;
        _currentCh2 = ch2G;
        recordOutput(_currentCh1, _currentCh2);
        return {_currentCh1 + _currentCh2, _currentCh1, _currentCh2, 0.0f, 0.0f};
    }

    _readings1[_readingIndex] = ch1G;
    _readings2[_readingIndex] = ch2G;
    _readingIndex = (_readingIndex + 1) % MAX_SAMPLES;

    const float currentTotal = _currentCh1 + _currentCh2;
    const float weightChange = fabsf(rawTotal - currentTotal);

    if (_state == State::STABLE) {
        if (weightChange > BREWING_THRESHOLD_G) {
            _state = State::BREWING;
            _lastBrewingActivityMs = nowMs;
        }
    } else if (_state == State::BREWING) {
        if (weightChange > BREWING_THRESHOLD_G) {
            _lastBrewingActivityMs = nowMs;
        } else if (nowMs - _lastBrewingActivityMs > STABILITY_TIMEOUT_MS) {
            _state = State::TRANSITIONING;
        }
    } else {
        if (weightChange > BREWING_THRESHOLD_G) {
            _state = State::BREWING;
            _lastBrewingActivityMs = nowMs;
        } else if (nowMs - _lastBrewingActivityMs > STABILITY_TIMEOUT_MS * 2) {
            _state = State::STABLE;
        }
    }

    if (_state == State::BREWING) {
        _currentCh1 = median(_readings1, BREWING_MEDIAN_SAMPLES);
        _currentCh2 = median(_readings2, BREWING_MEDIAN_SAMPLES);
    } else {
        _currentCh1 = average(_readings1, STABLE_AVERAGE_SAMPLES);
        _currentCh2 = average(_readings2, STABLE_AVERAGE_SAMPLES);
    }

    // Match WeighMyBrew2's immediate response for placing or removing a cup.
    if (weightChange > IMMEDIATE_RESPONSE_G) {
        _currentCh1 = ch1G;
        _currentCh2 = ch2G;
        initializeSamples(ch1G, ch2G);
        if (_state == State::STABLE) {
            _state = State::BREWING;
            _lastBrewingActivityMs = nowMs;
        }
    }

    recordOutput(_currentCh1, _currentCh2);
    return {_currentCh1 + _currentCh2, _currentCh1, _currentCh2, stddev(_stdReadings1), stddev(_stdReadings2)};
}

#endif
