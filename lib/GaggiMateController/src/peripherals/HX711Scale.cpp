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

    // Some boards expect open-drain SCK, others require push-pull.
    // Try both modes to match legacy behavior across hardware variants.
    const uint32_t sckModes[] = {OUTPUT_OPEN_DRAIN, OUTPUT};

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

        float signedSum = values[0] + values[1];
        const float absSum = fabsf(values[0]) + fabsf(values[1]);

        // If channels are opposite polarity, signed sum can collapse to ~0.
        // In that case, use magnitude sum so readings remain usable.
        if (fabsf(signedSum) < 0.05f && absSum > 1.0f) {
            signedSum = absSum;
            ESP_LOGW(LOG_TAG, "Channel cancellation detected, using abs sum: ch1=%.2f ch2=%.2f abs=%.2f", values[0], values[1],
                     absSum);
        }

        _weight = signedSum;
        ESP_LOGV(LOG_TAG, "RAW: ch1=%.2f ch2=%.2f sum=%.2f abs=%.2f", values[0], values[1], signedSum, absSum);
        _callback(_weight);
    }
}

void HX711Scale::tare() {
    if (!_present || !_loadCells)
        return;

    if (_loadCells->wait_ready_timeout(150, SCALE_READY_DELAY_MS)) {
        _loadCells->tare(SCALE_TARE_READINGS);
        _weight = 0.0f;
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
    _loadCells->get_value(values, 5);

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
