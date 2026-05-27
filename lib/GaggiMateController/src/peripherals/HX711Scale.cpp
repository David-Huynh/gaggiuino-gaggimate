#if defined(ARDUINO_ARCH_STM32) && !defined(GAGGIMATE_DISABLE_HARDWARE_SCALE)
#include <STM32FreeRTOS.h>
#define xTaskDelayUntil vTaskDelayUntil

#include "HX711Scale.h"
#include "logging.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

HX711Scale::HX711Scale(uint8_t dout1_pin, uint8_t dout2_pin, uint8_t sck_pin, const scale_sample_cb_t &sampleCb,
                       float calibration1, float calibration2)
    : _dout1_pin(dout1_pin), _dout2_pin(dout2_pin), _sck_pin(sck_pin), _scale1(calibration1), _scale2(calibration2),
      _sampleCb(sampleCb) {
    _snapMutex = xSemaphoreCreateMutex();
    recomputeNotCalibratedFlag();
}

void HX711Scale::setup() {
    _drv = new HX711Dual();
    _present = false;

    // The original library used push-pull SCK. Try push-pull first, fall back to
    // open-drain for boards that need pull-up sharing.
    const uint32_t sckModes[] = {OUTPUT, OUTPUT_OPEN_DRAIN};

    for (size_t modeIdx = 0; modeIdx < (sizeof(sckModes) / sizeof(sckModes[0])) && !_present; modeIdx++) {
        const uint32_t sckMode = sckModes[modeIdx];
        _drv->begin(_dout1_pin, _dout2_pin, _sck_pin, 128U, sckMode);

        for (int attempt = 1; attempt <= SCALE_INIT_RETRIES; attempt++) {
            _drv->power_up();
            const int dout1Before = digitalRead(_dout1_pin);
            const int dout2Before = digitalRead(_dout2_pin);
            ESP_LOGI(LOG_TAG, "HX711 init check (mode=%s attempt=%d/%d): DOUT1=%d DOUT2=%d (LOW means ready)",
                     sckMode == OUTPUT_OPEN_DRAIN ? "open-drain" : "push-pull", attempt, SCALE_INIT_RETRIES, dout1Before,
                     dout2Before);

            if (_drv->wait_ready_timeout(SCALE_INIT_TIMEOUT_MS, SCALE_READY_DELAY_MS)) {
                _present = true;
                ESP_LOGI(LOG_TAG, "HX711 detected (SCK mode=%s) on attempt %d/%d (DOUT1=%d, DOUT2=%d, SCK=%d)",
                         sckMode == OUTPUT_OPEN_DRAIN ? "open-drain" : "push-pull", attempt, SCALE_INIT_RETRIES, _dout1_pin,
                         _dout2_pin, _sck_pin);
                break;
            }

            _drv->power_down();
            if (attempt < SCALE_INIT_RETRIES) {
                ESP_LOGW(LOG_TAG, "HX711 not ready, retrying in %d ms", SCALE_INIT_RETRY_DELAY_MS);
                delay(SCALE_INIT_RETRY_DELAY_MS);
            }
        }
    }

    if (!_present) {
        _drv->power_down();
        ESP_LOGW(LOG_TAG, "HX711 scale not detected after %d attempts, disabling", SCALE_INIT_RETRIES);
        return;
    }

    _nativeHz = _drv->detect_rate();
    if (_nativeHz == 80) {
        _outputDivisor = 8;
        _samplePeriodMs = 12; // ~12.5 ms; integer cadence is fine
        _tareWarmupSamples = 48;
        _tareTargetSamples = 192;
        _tareMinSamples = 144;
        _calWarmupSamples = 48;
        _calTargetSamples = 256;
    } else {
        _outputDivisor = 1;
        _samplePeriodMs = 100;
        _tareWarmupSamples = 6;
        _tareTargetSamples = 24;
        _tareMinSamples = 18;
        _calWarmupSamples = 6;
        _calTargetSamples = 32;
    }
    ESP_LOGI(LOG_TAG, "HX711 native rate: %u Hz (oversample x%u, sample period %u ms)", _nativeHz, _outputDivisor,
             _samplePeriodMs);

    xTaskCreate(loopTask, "HX711Scale::loop", configMINIMAL_STACK_SIZE * 8, this, 1, &_taskHandle);
}

void HX711Scale::recomputeNotCalibratedFlag() {
    if (_scale1 == 0.0f || _scale2 == 0.0f) {
        _healthBits |= SCALE_HEALTH_NOT_CALIBRATED;
    } else {
        _healthBits &= static_cast<uint16_t>(~SCALE_HEALTH_NOT_CALIBRATED);
    }
}

ScaleSnapshot HX711Scale::snapshot() {
    ScaleSnapshot s{};
    if (_snapMutex && xSemaphoreTake(_snapMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        s = _snapshot;
        xSemaphoreGive(_snapMutex);
    }
    return s;
}

void HX711Scale::setCalibration(float c1, float c2) {
    _scale1 = c1;
    _scale2 = c2;
    _filter1.reset();
    _filter2.reset();
    recomputeNotCalibratedFlag();
    ESP_LOGI(LOG_TAG, "Scale calibration set: %.6f / %.6f", _scale1, _scale2);
}

void HX711Scale::getCalibration(float &c1, float &c2) const {
    c1 = _scale1;
    c2 = _scale2;
}

void HX711Scale::setOffset(long o1, long o2) {
    _offset1 = o1;
    _offset2 = o2;
    _filter1.reset();
    _filter2.reset();
    ESP_LOGI(LOG_TAG, "Scale offsets set: %ld / %ld", o1, o2);
}

void HX711Scale::getOffsets(long &o1, long &o2) const {
    o1 = _offset1;
    o2 = _offset2;
}

void HX711Scale::setGain(uint8_t gain) {
    if (_drv) {
        _drv->set_gain(gain);
    }
    _filter1.reset();
    _filter2.reset();
}

void HX711Scale::requestTare() {
    // Just set the flag — the loop picks it up on the next iteration. Multiple
    // requests collapse into one.
    _tareRequested = true;
}

void HX711Scale::requestCalibration(uint8_t channel, float refWeight) {
    _calRequestChannel = channel;
    _calRequestRefWeight = refWeight;
    _calRequested = true;
}

void HX711Scale::loop() {
    if (!_present || !_drv) {
        return;
    }

    long raw[2] = {0, 0};
    bool valid[2] = {false, false};
    bool sat[2] = {false, false};
    if (!_drv->wait_ready_timeout(150, 1)) {
        const uint32_t now = millis();
        if (_lastOkRead != 0 && (now - _lastOkRead) > STALE_TIMEOUT_MS) {
            _healthBits |= SCALE_HEALTH_STALE;
        }
        return;
    }

    HxResult r = _drv->try_read(raw, valid, sat);
    if (r != HxResult::OK) {
        const uint32_t now = millis();
        if (_lastOkRead != 0 && (now - _lastOkRead) > STALE_TIMEOUT_MS) {
            _healthBits |= SCALE_HEALTH_STALE;
        }
        return;
    }

    _lastOkRead = millis();
    _healthBits &= static_cast<uint16_t>(~SCALE_HEALTH_STALE);

    // Pick up async requests synchronously here so they only mutate state on the
    // scale task. Latch the request → run state machine.
    if (_tareRequested && _tareState == TareState::IDLE && _calState == CalState::IDLE) {
        _tareRequested = false;
        _tareState = TareState::WARMUP;
        _tareWarmupRemaining = _tareWarmupSamples;
        _tareCollected = 0;
        _tareStartMs = millis();
        _tareLastProgMs = 0;
        _healthBits |= SCALE_HEALTH_TARING;
        _healthBits &= static_cast<uint16_t>(~(SCALE_HEALTH_TARE_FAILED | SCALE_HEALTH_TARE_NOISY));
        ESP_LOGI(LOG_TAG, "Tare requested");
    }
    if (_calRequested && _calState == CalState::IDLE && _tareState == TareState::IDLE) {
        _calRequested = false;
        const uint8_t ch = _calRequestChannel;
        const float ref = _calRequestRefWeight;
        if ((ch != 1 && ch != 2) || !std::isfinite(ref) || ref < CAL_REF_MIN || ref > CAL_REF_MAX) {
            ESP_LOGW(LOG_TAG, "Cal rejected: channel=%u ref=%.2f", ch, ref);
            if (_calDoneCb) {
                _calDoneCb(ch, 0.0f, 0.0f, false, static_cast<uint8_t>(ScaleCalError::REF_OUT_OF_RANGE));
            }
        } else {
            _calChannel = ch;
            _calRefWeight = ref;
            _calState = CalState::WARMUP;
            _calWarmupRemaining = _calWarmupSamples;
            _calCollected = 0;
            _calStartMs = millis();
            _calLastProgMs = 0;
            _healthBits |= SCALE_HEALTH_CALIBRATING;
            ESP_LOGI(LOG_TAG, "Cal requested ch=%u ref=%.2f g", ch, ref);
        }
    }

    advanceTareSm(raw, valid, sat);
    advanceCalSm(raw, valid, sat);

    processOneRawSample(raw, valid, sat);
}

void HX711Scale::processOneRawSample(long raw[2], bool valid[2], bool sat[2]) {
    // Track per-channel saturation as a sticky health bit; clear after 8
    // consecutive good (unsaturated) samples on that channel.
    if (valid[0] && sat[0]) {
        _healthBits |= SCALE_HEALTH_SAT_CH1;
        _consecGoodCh1 = 0;
        _consecBadCh1 = std::min<uint8_t>(_consecBadCh1 + 1, 16);
    } else if (valid[0]) {
        if (_consecGoodCh1 < 8) {
            ++_consecGoodCh1;
        }
        if (_consecGoodCh1 >= 8) {
            _healthBits &= static_cast<uint16_t>(~SCALE_HEALTH_SAT_CH1);
            _consecBadCh1 = 0;
        }
    }
    if (valid[1] && sat[1]) {
        _healthBits |= SCALE_HEALTH_SAT_CH2;
        _consecGoodCh2 = 0;
        _consecBadCh2 = std::min<uint8_t>(_consecBadCh2 + 1, 16);
    } else if (valid[1]) {
        if (_consecGoodCh2 < 8) {
            ++_consecGoodCh2;
        }
        if (_consecGoodCh2 >= 8) {
            _healthBits &= static_cast<uint16_t>(~SCALE_HEALTH_SAT_CH2);
            _consecBadCh2 = 0;
        }
    }

    // While taring or calibrating, do not advance the public sample pipeline —
    // the caller is in a settling phase and should not get half-applied state.
    if (_tareState != TareState::IDLE || _calState != CalState::IDLE) {
        return;
    }

    if (_outputDivisor <= 1) {
        // 10 Hz native — push through immediately.
        emitOutputSample(raw[0], raw[1], valid[0] && !sat[0], valid[1] && !sat[1]);
        return;
    }

    // 80 Hz native — accumulate up to 8 raw samples, take the median, emit one
    // output. Saturated samples are dropped from the median set; if all samples
    // for a channel were saturated, mark the channel invalid for this output.
    if (_preCount < PRE_BUF_MAX) {
        if (valid[0] && !sat[0] && _preValidCount1 < PRE_BUF_MAX) {
            _preBuf1[_preValidCount1++] = raw[0];
        }
        if (valid[1] && !sat[1] && _preValidCount2 < PRE_BUF_MAX) {
            _preBuf2[_preValidCount2++] = raw[1];
        }
        ++_preCount;
    }
    if (_preCount < _outputDivisor) {
        return;
    }

    const uint8_t minValid = (_outputDivisor >= 4) ? (_outputDivisor / 2) : 1;
    const bool ch1Valid = _preValidCount1 >= minValid;
    const bool ch2Valid = _preValidCount2 >= minValid;
    const long med1 = ch1Valid ? medianLong(_preBuf1, _preValidCount1) : 0;
    const long med2 = ch2Valid ? medianLong(_preBuf2, _preValidCount2) : 0;
    _preCount = 0;
    _preValidCount1 = 0;
    _preValidCount2 = 0;

    emitOutputSample(med1, med2, ch1Valid, ch2Valid);
}

void HX711Scale::emitOutputSample(long medRaw1, long medRaw2, bool ch1Valid, bool ch2Valid) {
    float ch1G = NAN;
    float ch2G = NAN;
    if (_scale1 != 0.0f && ch1Valid) {
        const double grams = static_cast<double>(medRaw1 - _offset1) / static_cast<double>(_scale1);
        ch1G = _filter1.push(static_cast<float>(grams));
    }
    if (_scale2 != 0.0f && ch2Valid) {
        const double grams = static_cast<double>(medRaw2 - _offset2) / static_cast<double>(_scale2);
        ch2G = _filter2.push(static_cast<float>(grams));
    }

    const float std1 = std::isfinite(ch1G) ? _filter1.stddev() : 0.0f;
    const float std2 = std::isfinite(ch2G) ? _filter2.stddev() : 0.0f;
    const bool calibrated = (_scale1 != 0.0f) && (_scale2 != 0.0f);
    const float sumG =
        (std::isfinite(ch1G) && std::isfinite(ch2G)) ? (ch1G + ch2G) : (calibrated ? NAN : NAN);
    const float combinedStd = sqrtf(std1 * std1 + std2 * std2);

    if (xSemaphoreTake(_snapMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        _snapshot.weightG = sumG;
        _snapshot.stddevG = combinedStd;
        _snapshot.ch1G = ch1G;
        _snapshot.ch2G = ch2G;
        _snapshot.ch1StdG = std1;
        _snapshot.ch2StdG = std2;
        _snapshot.healthBits = _healthBits;
        _snapshot.sampleSeq = ++_sampleSeq;
        const ScaleSnapshot copy = _snapshot;
        xSemaphoreGive(_snapMutex);
        if (_sampleCb) {
            _sampleCb(copy);
        }
    }
}

void HX711Scale::advanceTareSm(long raw[2], bool valid[2], bool sat[2]) {
    if (_tareState == TareState::IDLE) {
        return;
    }

    const uint32_t now = millis();
    const bool timedOut = (now - _tareStartMs) > TARE_TIMEOUT_MS;

    if (_tareState == TareState::WARMUP) {
        if (_tareWarmupRemaining > 0) {
            --_tareWarmupRemaining;
        }
        if (_tareWarmupRemaining == 0) {
            _tareState = TareState::COLLECT;
        }
        // Also report "warming up" progress so UI bar isn't frozen.
        if ((now - _tareLastProgMs) >= PROGRESS_PERIOD_MS) {
            _tareLastProgMs = now;
            if (_tareProgCb) {
                _tareProgCb(0, 0.0f);
            }
        }
        return;
    }

    // Drop saturated samples — never tare on a clipped reading.
    if (valid[0] && valid[1] && !sat[0] && !sat[1] && _tareCollected < TARE_RING_MAX) {
        _tareBuf1[_tareCollected] = raw[0];
        _tareBuf2[_tareCollected] = raw[1];
        ++_tareCollected;
    }

    // Compute current stddev across whatever we have, in grams (using the
    // stored calibration when available; otherwise a unit count → grams=count).
    float currentStd = 0.0f;
    if (_tareCollected >= 2) {
        const float scale1 = _scale1 != 0.0f ? fabsf(_scale1) : 1.0f;
        const float scale2 = _scale2 != 0.0f ? fabsf(_scale2) : 1.0f;
        const float s1 = stddevLong(_tareBuf1, _tareCollected) / scale1;
        const float s2 = stddevLong(_tareBuf2, _tareCollected) / scale2;
        currentStd = sqrtf(s1 * s1 + s2 * s2);
    }

    if ((now - _tareLastProgMs) >= PROGRESS_PERIOD_MS) {
        _tareLastProgMs = now;
        if (_tareProgCb) {
            _tareProgCb(_tareCollected, currentStd);
        }
    }

    const bool haveTarget = _tareCollected >= _tareTargetSamples;
    const bool haveMin = _tareCollected >= _tareMinSamples;

    if (haveTarget || timedOut) {
        // Decide outcome.
        if (_tareCollected < _tareMinSamples) {
            finishTare(false, SCALE_HEALTH_TARE_FAILED);
            return;
        }
        const bool noisy = currentStd > TARE_STDDEV_THRESHOLD_G;
        const bool failed = currentStd > (TARE_STDDEV_THRESHOLD_G * TARE_STDDEV_FAIL_MULT);
        if (failed) {
            finishTare(false, SCALE_HEALTH_TARE_FAILED);
            return;
        }
        // Compute trimmed mean per channel and store as new offsets.
        const float t1 = trimmedMeanLong(_tareBuf1, _tareCollected, 0.125f);
        const float t2 = trimmedMeanLong(_tareBuf2, _tareCollected, 0.125f);
        _offset1 = static_cast<long>(t1 + (t1 >= 0.0f ? 0.5f : -0.5f));
        _offset2 = static_cast<long>(t2 + (t2 >= 0.0f ? 0.5f : -0.5f));
        _filter1.reset();
        _filter2.reset();
        finishTare(true, noisy ? SCALE_HEALTH_TARE_NOISY : 0);
        return;
    }

    // Settling-aware shortcut: enough samples AND already quiet → accept.
    if (haveMin && currentStd <= TARE_STDDEV_THRESHOLD_G) {
        const float t1 = trimmedMeanLong(_tareBuf1, _tareCollected, 0.125f);
        const float t2 = trimmedMeanLong(_tareBuf2, _tareCollected, 0.125f);
        _offset1 = static_cast<long>(t1 + (t1 >= 0.0f ? 0.5f : -0.5f));
        _offset2 = static_cast<long>(t2 + (t2 >= 0.0f ? 0.5f : -0.5f));
        _filter1.reset();
        _filter2.reset();
        finishTare(true, 0);
    }
}

void HX711Scale::finishTare(bool success, uint16_t flagsToAdd) {
    _tareState = TareState::IDLE;
    _healthBits &= static_cast<uint16_t>(~SCALE_HEALTH_TARING);
    if (flagsToAdd) {
        _healthBits |= flagsToAdd;
    }
    if (success) {
        _healthBits &= static_cast<uint16_t>(~SCALE_HEALTH_TARE_FAILED);
    }
    const float scale1 = _scale1 != 0.0f ? fabsf(_scale1) : 1.0f;
    const float scale2 = _scale2 != 0.0f ? fabsf(_scale2) : 1.0f;
    const float std1 = _tareCollected >= 2 ? stddevLong(_tareBuf1, _tareCollected) / scale1 : 0.0f;
    const float std2 = _tareCollected >= 2 ? stddevLong(_tareBuf2, _tareCollected) / scale2 : 0.0f;
    ESP_LOGI(LOG_TAG, "Tare done: success=%d offsets=(%ld,%ld) std=(%.3f,%.3f) g samples=%u flags=0x%04x",
             success ? 1 : 0, _offset1, _offset2, std1, std2, _tareCollected, _healthBits);
    if (_tareDoneCb) {
        _tareDoneCb(_offset1, _offset2, std1, std2, success, _healthBits);
    }
    _tareCollected = 0;
}

void HX711Scale::advanceCalSm(long raw[2], bool valid[2], bool sat[2]) {
    if (_calState == CalState::IDLE) {
        return;
    }

    const uint32_t now = millis();
    const bool timedOut = (now - _calStartMs) > CAL_TIMEOUT_MS;
    const uint8_t ch = _calChannel;

    if (_calState == CalState::WARMUP) {
        if (_calWarmupRemaining > 0) {
            --_calWarmupRemaining;
        }
        if (_calWarmupRemaining == 0) {
            _calState = CalState::COLLECT;
        }
        if ((now - _calLastProgMs) >= PROGRESS_PERIOD_MS) {
            _calLastProgMs = now;
            if (_calProgCb) {
                _calProgCb(ch, 0, 0.0f);
            }
        }
        return;
    }

    const bool channelSat = (ch == 1) ? sat[0] : sat[1];
    const bool channelValid = (ch == 1) ? valid[0] : valid[1];
    if (channelValid && !channelSat && _calCollected < CAL_RING_MAX) {
        _calBuf[_calCollected] = (ch == 1) ? raw[0] : raw[1];
        ++_calCollected;
    }

    float curStd = 0.0f;
    if (_calCollected >= 2) {
        // Express stddev in grams using whatever calibration is live (defaults to
        // 1.0 if we're calibrating from scratch). At cal time the user is told
        // the stddev so the units don't have to be perfect.
        const float scale =
            (ch == 1 && _scale1 != 0.0f) ? fabsf(_scale1) : (ch == 2 && _scale2 != 0.0f) ? fabsf(_scale2) : 1.0f;
        curStd = stddevLong(_calBuf, _calCollected) / scale;
    }

    if ((now - _calLastProgMs) >= PROGRESS_PERIOD_MS) {
        _calLastProgMs = now;
        if (_calProgCb) {
            _calProgCb(ch, _calCollected, curStd);
        }
    }

    if (timedOut && _calCollected < (_calTargetSamples / 2)) {
        finishCal(false, ScaleCalError::TIMEOUT, 0.0f, curStd);
        return;
    }

    const bool haveTarget = _calCollected >= _calTargetSamples;
    if (!haveTarget && !timedOut) {
        return;
    }

    if (curStd > CAL_STDDEV_THRESHOLD_G) {
        finishCal(false, ScaleCalError::TOO_NOISY, 0.0f, curStd);
        return;
    }

    const float meanRaw = trimmedMeanLong(_calBuf, _calCollected, 0.125f);
    // Subtract zero offset so the factor is in counts-per-gram off-zero.
    const float zero = (ch == 1) ? static_cast<float>(_offset1) : static_cast<float>(_offset2);
    const float netRaw = meanRaw - zero;
    if (_calRefWeight == 0.0f) {
        finishCal(false, ScaleCalError::REF_OUT_OF_RANGE, 0.0f, curStd);
        return;
    }
    const float factor = netRaw / _calRefWeight;
    const float absF = fabsf(factor);
    if (!std::isfinite(factor) || absF < CAL_FACTOR_MIN || absF > CAL_FACTOR_MAX) {
        finishCal(false, ScaleCalError::FACTOR_OUT_OF_RANGE, factor, curStd);
        return;
    }

    if (ch == 1) {
        _scale1 = factor;
    } else {
        _scale2 = factor;
    }
    recomputeNotCalibratedFlag();
    _filter1.reset();
    _filter2.reset();
    finishCal(true, ScaleCalError::OK, factor, curStd);
}

void HX711Scale::finishCal(bool success, ScaleCalError err, float factor, float stddevAtCal) {
    _calState = CalState::IDLE;
    _healthBits &= static_cast<uint16_t>(~SCALE_HEALTH_CALIBRATING);
    ESP_LOGI(LOG_TAG, "Cal done ch=%u success=%d factor=%.4f std=%.4f err=%u samples=%u", _calChannel, success ? 1 : 0,
             factor, stddevAtCal, static_cast<unsigned>(err), _calCollected);
    if (_calDoneCb) {
        _calDoneCb(_calChannel, factor, stddevAtCal, success, static_cast<uint8_t>(err));
    }
    _calCollected = 0;
}

long HX711Scale::medianLong(long *buf, size_t n) {
    if (n == 0) {
        return 0;
    }
    // Insertion sort — n ≤ 8, fine.
    for (size_t i = 1; i < n; i++) {
        long v = buf[i];
        size_t j = i;
        while (j > 0 && buf[j - 1] > v) {
            buf[j] = buf[j - 1];
            --j;
        }
        buf[j] = v;
    }
    return buf[n / 2];
}

float HX711Scale::trimmedMeanLong(long *buf, size_t n, float trimFrac) {
    if (n == 0) {
        return 0.0f;
    }
    // Sort in place. Callers do not use the buffer again after the trimmed-mean
    // call (state machine returns to IDLE and resets the count), so destroying
    // the order is fine and saves ~1.3 KB of stack vs. a local copy.
    for (size_t i = 1; i < n; i++) {
        long v = buf[i];
        size_t j = i;
        while (j > 0 && buf[j - 1] > v) {
            buf[j] = buf[j - 1];
            --j;
        }
        buf[j] = v;
    }
    size_t trim = static_cast<size_t>(static_cast<float>(n) * trimFrac);
    if (trim * 2 >= n) {
        trim = (n > 0) ? (n - 1) / 2 : 0;
    }
    const size_t lo = trim;
    const size_t hi = n - trim;
    if (hi <= lo) {
        return 0.0f;
    }
    double sum = 0.0;
    for (size_t i = lo; i < hi; i++) {
        sum += static_cast<double>(buf[i]);
    }
    return static_cast<float>(sum / static_cast<double>(hi - lo));
}

float HX711Scale::stddevLong(const long *buf, size_t n) {
    if (n < 2) {
        return 0.0f;
    }
    double sum = 0.0;
    for (size_t i = 0; i < n; i++) {
        sum += static_cast<double>(buf[i]);
    }
    const double mean = sum / static_cast<double>(n);
    double ss = 0.0;
    for (size_t i = 0; i < n; i++) {
        const double d = static_cast<double>(buf[i]) - mean;
        ss += d * d;
    }
    return static_cast<float>(sqrt(ss / static_cast<double>(n - 1)));
}

[[noreturn]] void HX711Scale::loopTask(void *arg) {
    TickType_t lastWake = xTaskGetTickCount();
    auto *scale = static_cast<HX711Scale *>(arg);
    const uint32_t period = scale->_samplePeriodMs > 0 ? scale->_samplePeriodMs : 100;
    while (true) {
        scale->loop();
        xTaskDelayUntil(&lastWake, pdMS_TO_TICKS(period));
    }
}
#endif
