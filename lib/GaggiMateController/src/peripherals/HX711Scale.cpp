#ifdef ARDUINO_ARCH_STM32
#include <STM32FreeRTOS.h>
#define xTaskDelayUntil vTaskDelayUntil

#include "HX711Scale.h"
#include "logging.h"
#include <cmath>

HX711Scale::HX711Scale(uint8_t dout1_pin, uint8_t dout2_pin, uint8_t sck_pin, const weight_callback_t &callback,
                       float calibration1, float calibration2)
    : _dout1_pin(dout1_pin), _dout2_pin(dout2_pin), _sck_pin(sck_pin), _calibration1(calibration1), _calibration2(calibration2),
      _callback(callback), _taskHandle(nullptr) {}

void HX711Scale::setup() {
    _loadCells = new HX711Dual();
    _present = false;

    // The original HX711_2 timer-ISR library used push-pull timer output.
    // Try push-pull first (works with or without PCB pull-ups).
    // Fall back to open-drain only if push-pull fails to detect the HX711.
    const uint32_t sckModes[] = {OUTPUT, OUTPUT_OPEN_DRAIN};

    for (size_t modeIdx = 0; modeIdx < (sizeof(sckModes) / sizeof(sckModes[0])) && !_present; modeIdx++) {
        const uint32_t sckMode = sckModes[modeIdx];
        _loadCells->begin(_dout1_pin, _dout2_pin, _sck_pin, 128U, sckMode);
        _loadCells->set_scale(_calibration1, _calibration2);

        for (int attempt = 1; attempt <= SCALE_INIT_RETRIES; attempt++) {
            _loadCells->power_up();
            const int dout1Before = digitalRead(_dout1_pin);
            const int dout2Before = digitalRead(_dout2_pin);
            ESP_LOGI(LOG_TAG, "HX711 init check (mode=%s attempt=%d/%d): DOUT1=%d DOUT2=%d (LOW means ready)",
                     sckMode == OUTPUT_OPEN_DRAIN ? "open-drain" : "push-pull", attempt, SCALE_INIT_RETRIES, dout1Before,
                     dout2Before);

            if (_loadCells->wait_ready_timeout(SCALE_INIT_TIMEOUT_MS, SCALE_READY_DELAY_MS)) {
                _loadCells->tare(SCALE_TARE_READINGS);
                _present = true;
                ESP_LOGI(LOG_TAG, "HX711 detected (SCK mode=%s) on attempt %d/%d (DOUT1=%d, DOUT2=%d, SCK=%d)",
                         sckMode == OUTPUT_OPEN_DRAIN ? "open-drain" : "push-pull", attempt, SCALE_INIT_RETRIES, _dout1_pin,
                         _dout2_pin, _sck_pin);
                xTaskCreate(loopTask, "HX711Scale::loop", configMINIMAL_STACK_SIZE * 4, this, 1, &_taskHandle);
                break;
            }

            _loadCells->power_down();
            if (attempt < SCALE_INIT_RETRIES) {
                const int dout1After = digitalRead(_dout1_pin);
                const int dout2After = digitalRead(_dout2_pin);
                ESP_LOGW(LOG_TAG, "HX711 not ready (SCK mode=%s) on attempt %d/%d, retrying in %d ms",
                         sckMode == OUTPUT_OPEN_DRAIN ? "open-drain" : "push-pull", attempt, SCALE_INIT_RETRIES,
                         SCALE_INIT_RETRY_DELAY_MS);
                ESP_LOGW(LOG_TAG, "HX711 not ready pin states after attempt: DOUT1=%d DOUT2=%d", dout1After, dout2After);
                delay(SCALE_INIT_RETRY_DELAY_MS);
            }
        }
    }

    if (!_present) {
        _loadCells->power_down();
        ESP_LOGW(LOG_TAG, "HX711 scale not detected after %d attempts, disabling", SCALE_INIT_RETRIES);
    }
}

void HX711Scale::loop() {
    if (!_present || !_loadCells)
        return;

    if (_loadCells->wait_ready_timeout(150, SCALE_READY_DELAY_MS)) {
        float values[2];
        _loadCells->get_units(values, 2);

        if (!_emaInitialized) {
            _emaCh1 = values[0];
            _emaCh2 = values[1];
            _emaInitialized = true;
        } else {
            _emaCh1 += SCALE_EMA_ALPHA * (values[0] - _emaCh1);
            _emaCh2 += SCALE_EMA_ALPHA * (values[1] - _emaCh2);
        }

        const float signedSum = _emaCh1 + _emaCh2;

        // Reject implausible values and sudden spikes caused by vibration/EMI.
        if (fabsf(signedSum) > SCALE_MAX_ABS_WEIGHT_G) {
            ESP_LOGW(LOG_TAG, "Scale outlier rejected: %.2f g (ema1=%.2f ema2=%.2f)", signedSum, _emaCh1, _emaCh2);
            return;
        }

        float filteredSum = signedSum;
        const float delta = signedSum - _weight;
        if (fabsf(delta) > SCALE_MAX_DELTA_PER_SAMPLE_G) {
            filteredSum = _weight + (delta > 0.0f ? SCALE_MAX_DELTA_PER_SAMPLE_G : -SCALE_MAX_DELTA_PER_SAMPLE_G);
            ESP_LOGV(LOG_TAG, "Scale slew-limited: prev=%.2f raw=%.2f out=%.2f", _weight, signedSum, filteredSum);
        }

        _weight = filteredSum;
        ESP_LOGV(LOG_TAG, "SCALE: raw1=%.2f raw2=%.2f ema1=%.2f ema2=%.2f sum=%.2f", values[0], values[1], _emaCh1, _emaCh2,
                 _weight);
        _callback(_weight);
    }
}

void HX711Scale::tare() {
    if (!_present || !_loadCells)
        return;

    if (_loadCells->wait_ready_timeout(150, SCALE_READY_DELAY_MS)) {
        _loadCells->tare(SCALE_TARE_READINGS);
        _weight = 0.0f;
        _emaInitialized = false;
        _emaCh1 = 0.0f;
        _emaCh2 = 0.0f;
        long offsets[2] = {0, 0};
        _loadCells->get_offset(offsets);
        _offset1 = offsets[0];
        _offset2 = offsets[1];
        ESP_LOGI(LOG_TAG, "Scale tared, offsets: %ld, %ld", _offset1, _offset2);
        if (_tareResultCallback) {
            _tareResultCallback(_offset1, _offset2);
        }
    }
}

void HX711Scale::setCalibration(float calibration1, float calibration2) {
    _calibration1 = calibration1;
    _calibration2 = calibration2;
    if (_loadCells) {
        _loadCells->set_scale(_calibration1, _calibration2);
    }
}

void HX711Scale::getCalibration(float &calibration1, float &calibration2) const {
    calibration1 = _calibration1;
    calibration2 = _calibration2;
}

void HX711Scale::setOffset(long offset1, long offset2) {
    _offset1 = offset1;
    _offset2 = offset2;
    if (_loadCells) {
        _loadCells->set_offset(offset1, offset2);
        ESP_LOGI(LOG_TAG, "Scale offsets set: %ld, %ld", offset1, offset2);
    }
}

void HX711Scale::getOffsets(long &offset1, long &offset2) const {
    if (_loadCells) {
        long offsets[2] = {0, 0};
        _loadCells->get_offset(offsets);
        offset1 = offsets[0];
        offset2 = offsets[1];
    } else {
        offset1 = _offset1;
        offset2 = _offset2;
    }
}

float HX711Scale::calibrateChannel(uint8_t channel, float refWeight) {
    if (!_present || !_loadCells || refWeight == 0.0f)
        return 0.0f;
    if (!_loadCells->wait_ready_timeout(500, SCALE_READY_DELAY_MS))
        return 0.0f;

    long values[2] = {0, 0};
    _loadCells->get_value(values, SCALE_CALIBRATION_READINGS);

    float newFactor = 0.0f;
    if (channel == 1 && values[0] != 0) {
        newFactor = static_cast<float>(values[0]) / refWeight;
        _calibration1 = newFactor;
        _loadCells->set_scale(_calibration1, _calibration2);
        ESP_LOGI(LOG_TAG, "Ch1 calibrated: factor=%.4f (raw=%ld, ref=%.1f)", newFactor, values[0], refWeight);
    } else if (channel == 2 && values[1] != 0) {
        newFactor = static_cast<float>(values[1]) / refWeight;
        _calibration2 = newFactor;
        _loadCells->set_scale(_calibration1, _calibration2);
        ESP_LOGI(LOG_TAG, "Ch2 calibrated: factor=%.4f (raw=%ld, ref=%.1f)", newFactor, values[1], refWeight);
    }
    return newFactor;
}

[[noreturn]] void HX711Scale::loopTask(void *arg) {
    TickType_t lastWake = xTaskGetTickCount();
    auto *scale = static_cast<HX711Scale *>(arg);
    while (true) {
        scale->loop();
        xTaskDelayUntil(&lastWake, pdMS_TO_TICKS(SCALE_READ_INTERVAL_MS));
    }
}
#endif
