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

    if (!initializeDriver()) {
        ESP_LOGW(LOG_TAG, "HX711 scale not detected at boot; retrying in background");
    }

    xTaskCreate(loopTask, "HX711Scale::loop", configMINIMAL_STACK_SIZE * 8, this, 1, &_taskHandle);
}

bool HX711Scale::initializeDriver() {
    if (!_drv) {
        return false;
    }

    _lastInitAttemptMs = millis();
    _present = false;

    // The two HX711 share one SCK pin (and therefore one SCK mode), so detection
    // runs in two phases: (1) find an SCK mode where the bus comes alive at all,
    // then (2) under that mode confirm BOTH channels. The scale sums two load
    // cells, so a single live chip produces a NaN weight (see emitOutputSample)
    // and is unusable. Re-clocking cannot conjure an absent chip, so phase 2 just
    // polls the still-missing channel — leaving the live one untouched — and the
    // 5 s loop() retry re-checks until the second chip is connected.
    const uint32_t sckModes[] = {OUTPUT, OUTPUT_OPEN_DRAIN};
    bool busAlive = false;

    for (size_t modeIdx = 0; modeIdx < (sizeof(sckModes) / sizeof(sckModes[0])) && !busAlive; modeIdx++) {
        const uint32_t sckMode = sckModes[modeIdx];
        _drv->begin(_dout1_pin, _dout2_pin, _sck_pin, 128U, sckMode);

        for (int attempt = 1; attempt <= SCALE_INIT_RETRIES && !busAlive; attempt++) {
            _drv->power_up();
            const int dout1Before = digitalRead(_dout1_pin);
            const int dout2Before = digitalRead(_dout2_pin);
            ESP_LOGI(LOG_TAG, "HX711 init check (mode=%s attempt=%d/%d): DOUT1=%d DOUT2=%d (LOW means ready)",
                     sckMode == OUTPUT_OPEN_DRAIN ? "open-drain" : "push-pull", attempt, SCALE_INIT_RETRIES, dout1Before,
                     dout2Before);

            const uint32_t readyStart = millis();
            while (millis() - readyStart < static_cast<uint32_t>(SCALE_INIT_TIMEOUT_MS) &&
                   !_drv->is_ready_ch1() && !_drv->is_ready_ch2()) {
                delay(SCALE_READY_DELAY_MS);
            }
            if (_drv->is_ready_ch1() || _drv->is_ready_ch2()) {
                busAlive = true;
                ESP_LOGI(LOG_TAG, "HX711 bus alive (SCK mode=%s) on attempt %d/%d (SCK=%d)",
                         sckMode == OUTPUT_OPEN_DRAIN ? "open-drain" : "push-pull", attempt, SCALE_INIT_RETRIES, _sck_pin);
                break;
            }

            _drv->power_down();
            if (attempt < SCALE_INIT_RETRIES) {
                ESP_LOGW(LOG_TAG, "HX711 not ready, retrying in %d ms", SCALE_INIT_RETRY_DELAY_MS);
                delay(SCALE_INIT_RETRY_DELAY_MS);
            }
        }
    }

    if (!busAlive) {
        _drv->power_down();
        ESP_LOGW(LOG_TAG, "HX711 scale not detected after %d attempts", SCALE_INIT_RETRIES);
        return false;
    }

    // Phase 2: require BOTH channels. They convert in lockstep off the shared
    // clock, so a present second chip appears within ~1-2 sample periods. Latch
    // each channel as its DOUT is first seen ready and keep polling only the one
    // still missing — without power-cycling, which would disturb the live channel.
    bool ready1 = false;
    bool ready2 = false;
    const uint32_t bothStart = millis();
    while (millis() - bothStart < static_cast<uint32_t>(SCALE_BOTH_READY_TIMEOUT_MS)) {
        ready1 = ready1 || _drv->is_ready_ch1();
        ready2 = ready2 || _drv->is_ready_ch2();
        if (ready1 && ready2) {
            break;
        }
        delay(SCALE_READY_DELAY_MS);
    }

    if (!ready1 || !ready2) {
        _drv->power_down();
        ESP_LOGW(LOG_TAG, "HX711 only one channel responding (ch1=%d ch2=%d); both required, will retry", ready1, ready2);
        return false;
    }

    _present = true;
    ESP_LOGI(LOG_TAG, "HX711 detected: both channels ready (DOUT1=%d, DOUT2=%d, SCK=%d)", _dout1_pin, _dout2_pin, _sck_pin);

    _nativeHz = _drv->detect_rate();
    if (_nativeHz == 80) {
        _calWarmupSamples = 48;
        _calTargetSamples = 256;
    } else {
        _calWarmupSamples = 6;
        _calTargetSamples = 32;
    }
    _samplePeriodMs = SCALE_READ_INTERVAL_MS;
    ESP_LOGI(LOG_TAG, "HX711 native rate: %u Hz (poll period %u ms)", _nativeHz, _samplePeriodMs);
    return true;
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
    _filter.reset();
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
    _filter.reset();
    for (auto &point : _calPoints) {
        point.valid = false;
    }
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
    _filter.reset();
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
    if (!_drv) {
        return;
    }
    if (!_present) {
        const uint32_t now = millis();
        if (_lastInitAttemptMs == 0 || (now - _lastInitAttemptMs) >= 5000) {
            initializeDriver();
        }
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
        _tareState = TareState::COLLECT;
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

    // WeighMyBru2 filters each fresh reading rather than first collapsing 80 Hz
    // input to 10 Hz. Both channels use the same synchronized SCK burst.
    emitOutputSample(raw[0], raw[1], valid[0] && !sat[0], valid[1] && !sat[1]);
}

void HX711Scale::emitOutputSample(long raw1, long raw2, bool ch1Valid, bool ch2Valid) {
    const bool ch1Usable = _scale1 != 0.0f && ch1Valid;
    const bool ch2Usable = _scale2 != 0.0f && ch2Valid;
    const bool usable = ch1Usable && ch2Usable;
    DualScaleFilteredReading reading{};
    if (usable) {
        const float rawCh1G = static_cast<float>(static_cast<double>(raw1 - _offset1) / static_cast<double>(_scale1));
        const float rawCh2G = static_cast<float>(static_cast<double>(raw2 - _offset2) / static_cast<double>(_scale2));
        reading = _filter.push(rawCh1G, rawCh2G, millis());
    } else {
        reading.totalG = NAN;
        reading.ch1G = ch1Usable ? static_cast<float>(static_cast<double>(raw1 - _offset1) / static_cast<double>(_scale1)) : NAN;
        reading.ch2G = ch2Usable ? static_cast<float>(static_cast<double>(raw2 - _offset2) / static_cast<double>(_scale2)) : NAN;
        reading.ch1StdG = NAN;
        reading.ch2StdG = NAN;
    }

    const float combinedStd = usable ? sqrtf(reading.ch1StdG * reading.ch1StdG + reading.ch2StdG * reading.ch2StdG) : NAN;

    if (xSemaphoreTake(_snapMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        _snapshot.weightG = reading.totalG;
        _snapshot.stddevG = combinedStd;
        _snapshot.ch1G = reading.ch1G;
        _snapshot.ch2G = reading.ch2G;
        _snapshot.ch1StdG = reading.ch1StdG;
        _snapshot.ch2StdG = reading.ch2StdG;
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

    // WeighMyBru2 uses the arithmetic mean of 20 readings. Keep the same
    // behavior independently for both tray halves, while rejecting clipped or
    // incomplete synchronized reads.
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

    if (_tareCollected >= TARE_TARGET_SAMPLES) {
        const float mean1 = meanLong(_tareBuf1, _tareCollected);
        const float mean2 = meanLong(_tareBuf2, _tareCollected);
        _offset1 = static_cast<long>(mean1 + (mean1 >= 0.0f ? 0.5f : -0.5f));
        _offset2 = static_cast<long>(mean2 + (mean2 >= 0.0f ? 0.5f : -0.5f));
        for (auto &point : _calPoints) {
            point.valid = false;
        }
        _filter.reset();
        finishTare(true, currentStd > TARE_STDDEV_THRESHOLD_G ? SCALE_HEALTH_TARE_NOISY : 0);
        return;
    }

    if (timedOut) {
        finishTare(false, SCALE_HEALTH_TARE_FAILED);
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

    if (valid[0] && valid[1] && !sat[0] && !sat[1] && _calCollected < CAL_RING_MAX) {
        _calBuf1[_calCollected] = raw[0];
        _calBuf2[_calCollected] = raw[1];
        ++_calCollected;
    }

    float curStd = 0.0f;
    if (_calCollected >= 2) {
        // Express stddev in grams using whatever calibration is live (defaults to
        // 1.0 if we're calibrating from scratch). At cal time the user is told
        // the stddev so the units don't have to be perfect.
        const float scale =
            (ch == 1 && _scale1 != 0.0f) ? fabsf(_scale1) : (ch == 2 && _scale2 != 0.0f) ? fabsf(_scale2) : 1.0f;
        curStd = stddevLong(ch == 1 ? _calBuf1 : _calBuf2, _calCollected) / scale;
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

    if (_calRefWeight == 0.0f) {
        finishCal(false, ScaleCalError::REF_OUT_OF_RANGE, 0.0f, curStd);
        return;
    }

    const float meanRaw1 = trimmedMeanLong(_calBuf1, _calCollected, 0.125f);
    const float meanRaw2 = trimmedMeanLong(_calBuf2, _calCollected, 0.125f);
    CalPoint point{};
    point.valid = true;
    point.refWeight = _calRefWeight;
    point.netRaw1 = meanRaw1 - static_cast<float>(_offset1);
    point.netRaw2 = meanRaw2 - static_cast<float>(_offset2);
    _calPoints[ch - 1] = point;

    const auto factorValid = [](float factor) {
        const float absF = fabsf(factor);
        return std::isfinite(factor) && absF >= CAL_FACTOR_MIN && absF <= CAL_FACTOR_MAX;
    };

    const CalPoint &p1 = _calPoints[0];
    const CalPoint &p2 = _calPoints[1];
    if (p1.valid && p2.valid) {
        // Solve:
        //   refLeft  = a * leftRaw1  + b * leftRaw2
        //   refRight = a * rightRaw1 + b * rightRaw2
        // Then convert grams-per-count a/b back to the existing counts-per-gram
        // factor representation. This accounts for mechanical cross-coupling in
        // the tray instead of assuming the opposite channel contributes nothing.
        const float det = (p1.netRaw1 * p2.netRaw2) - (p2.netRaw1 * p1.netRaw2);
        if (!std::isfinite(det) || fabsf(det) < 100.0f) {
            finishCal(false, ScaleCalError::FACTOR_OUT_OF_RANGE, 0.0f, curStd);
            return;
        }
        const float gramsPerCount1 = ((p1.refWeight * p2.netRaw2) - (p2.refWeight * p1.netRaw2)) / det;
        const float gramsPerCount2 = ((p1.netRaw1 * p2.refWeight) - (p2.netRaw1 * p1.refWeight)) / det;
        if (!std::isfinite(gramsPerCount1) || !std::isfinite(gramsPerCount2) || gramsPerCount1 == 0.0f ||
            gramsPerCount2 == 0.0f) {
            finishCal(false, ScaleCalError::FACTOR_OUT_OF_RANGE, 0.0f, curStd);
            return;
        }
        const float solvedScale1 = 1.0f / gramsPerCount1;
        const float solvedScale2 = 1.0f / gramsPerCount2;
        if (!factorValid(solvedScale1) || !factorValid(solvedScale2)) {
            finishCal(false, ScaleCalError::FACTOR_OUT_OF_RANGE, ch == 1 ? solvedScale1 : solvedScale2, curStd);
            return;
        }

        _scale1 = solvedScale1;
        _scale2 = solvedScale2;
        recomputeNotCalibratedFlag();
        _filter.reset();
        ESP_LOGI(LOG_TAG, "Solved dual-point scale calibration: ch1=%.4f ch2=%.4f det=%.3f", _scale1, _scale2, det);
        finishCal(true, ScaleCalError::OK, ch == 1 ? _scale1 : _scale2, curStd, true, ch == 1 ? 2 : 1,
                  ch == 1 ? _scale2 : _scale1);
        return;
    }

    // First side captured: keep the scale usable with a provisional per-channel
    // factor until the opposite side is captured and the compensated pair can be
    // solved. The WebUI instructs users to calibrate both sides before trusting
    // the total.
    const float netRaw = (ch == 1) ? point.netRaw1 : point.netRaw2;
    const float factor = netRaw / _calRefWeight;
    if (!factorValid(factor)) {
        finishCal(false, ScaleCalError::FACTOR_OUT_OF_RANGE, factor, curStd);
        return;
    }

    if (ch == 1) {
        _scale1 = factor;
    } else {
        _scale2 = factor;
    }
    recomputeNotCalibratedFlag();
    _filter.reset();
    finishCal(true, ScaleCalError::OK, factor, curStd);
}

void HX711Scale::finishCal(bool success, ScaleCalError err, float factor, float stddevAtCal, bool notifyOther,
                           uint8_t otherChannel, float otherFactor) {
    _calState = CalState::IDLE;
    _healthBits &= static_cast<uint16_t>(~SCALE_HEALTH_CALIBRATING);
    ESP_LOGI(LOG_TAG, "Cal done ch=%u success=%d factor=%.4f std=%.4f err=%u samples=%u", _calChannel, success ? 1 : 0,
             factor, stddevAtCal, static_cast<unsigned>(err), _calCollected);
    if (_calDoneCb) {
        if (success && notifyOther && (otherChannel == 1 || otherChannel == 2)) {
            _calDoneCb(otherChannel, otherFactor, stddevAtCal, true, static_cast<uint8_t>(ScaleCalError::OK));
        }
        _calDoneCb(_calChannel, factor, stddevAtCal, success, static_cast<uint8_t>(err));
    }
    _calCollected = 0;
}

float HX711Scale::meanLong(const long *buf, size_t n) {
    if (n == 0) {
        return 0.0f;
    }
    double sum = 0.0;
    for (size_t i = 0; i < n; ++i) {
        sum += static_cast<double>(buf[i]);
    }
    return static_cast<float>(sum / static_cast<double>(n));
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
    while (true) {
        scale->loop();
        const bool capturing = scale->_tareState != TareState::IDLE || scale->_calState != CalState::IDLE;
        const uint32_t period = capturing && scale->_nativeHz == 80 ? 12 : scale->_samplePeriodMs;
        xTaskDelayUntil(&lastWake, pdMS_TO_TICKS(period));
    }
}
#endif
