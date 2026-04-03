#ifdef ARDUINO_ARCH_STM32
#include <STM32FreeRTOS.h>
// On STM32, xTaskDelayUntil is usually named vTaskDelayUntil
#define xTaskDelayUntil vTaskDelayUntil
#else
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#endif

#include "HX711Scale.h"
#include "logging.h"

HX711Scale::HX711Scale(uint8_t dout1_pin, uint8_t dout2_pin, uint8_t sck_pin, const weight_callback_t &callback,
                       float calibration1, float calibration2)
    : _dout1_pin(dout1_pin), _dout2_pin(dout2_pin), _sck_pin(sck_pin), _calibration1(calibration1), _calibration2(calibration2),
      _callback(callback), _taskHandle(nullptr) {}

void HX711Scale::setup() {
#ifdef ARDUINO_ARCH_STM32
    // STM32: use hardware timer for synchronized dual-channel reading
    _loadCells = new HX711_2(TIM3);
    _loadCells->begin(_dout1_pin, _dout2_pin, _sck_pin, 128U, OUTPUT_OPEN_DRAIN);
#else
    // ESP32: no hardware timer needed
    _loadCells = new HX711_2();
    _loadCells->begin(_dout1_pin, _dout2_pin, _sck_pin, 128U, OUTPUT);
#endif

    _loadCells->set_scale(_calibration1, _calibration2);
    _loadCells->power_up();

    if (_loadCells->wait_ready_timeout(SCALE_INIT_TIMEOUT_MS, SCALE_READY_DELAY_MS)) {
        _loadCells->tare(SCALE_TARE_READINGS);
        _present = true;
        ESP_LOGI(LOG_TAG, "HX711 scale detected on DOUT1=%d, DOUT2=%d, SCK=%d", _dout1_pin, _dout2_pin, _sck_pin);
        xTaskCreate(loopTask, "HX711Scale::loop", configMINIMAL_STACK_SIZE * 4, this, 1, &_taskHandle);
    } else {
        _loadCells->power_down();
        _present = false;
        ESP_LOGW(LOG_TAG, "HX711 scale not detected, disabling");
    }
}

void HX711Scale::loop() {
    if (!_present || !_loadCells)
        return;

    if (_loadCells->wait_ready_timeout(150, SCALE_READY_DELAY_MS)) {
        float values[2];
        _loadCells->get_units(values);
        _weight = values[0] + values[1];
        ESP_LOGV(LOG_TAG, "Scale reading: ch1=%.2f, ch2=%.2f, total=%.2f", values[0], values[1], _weight);
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

    // get_value returns (raw - offset), averaged over several readings
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
