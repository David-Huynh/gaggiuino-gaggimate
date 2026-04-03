#ifdef ARDUINO_ARCH_STM32
#include <STM32FreeRTOS.h>
#endif
#include "LedController2.h"
#include "logging.h"

LedController::LedController(TwoWire *i2c) { this->pca9632 = new PCA9632(0x00, i2c); }

void LedController::setup() {
    this->initialize();
    this->disable();
}

bool LedController::isAvailable() { return this->initialize(); }

void LedController::setChannel(uint8_t channel, uint8_t brightness) {
    ESP_LOGI("LedController", "Setting channel %u to %u", channel, brightness);
    if (channel > 3) {
        ESP_LOGE("LedController", "Invalid channel %u, PCA9632 supports 0-3", channel);
        return;
    }
    uint8_t error = this->pca9632->write(channel, brightness);
    if (error != 0) {
        ESP_LOGE("LedController", "Error setting channel %u to %u: %d", channel, brightness, this->pca9632->lastError());
    }
}

void LedController::disable() { this->pca9632->allOff(); }

bool LedController::initialize() {
    if (this->initialized) {
        return true;
    }

    bool retval = this->pca9632->begin();
    if (!retval) {
        ESP_LOGE("LedController", "Failed to initialize PCA9632");
        return false;
    }

    ESP_LOGI("LedController", "Initialized PCA9632");
    this->initialized = retval;

    // Configure Mode1 and Mode2 registers
    this->pca9632->setMode1(PCA9632_MODE1_NONE);
    this->pca9632->setMode2(PCA9632_MODE2_TOTEMPOLE);

    // Turn all LEDs off
    this->pca9632->allOff();

    // Set all channels to PWM mode
    this->pca9632->setLedDriverModeAll(PCA9632_LEDPWM);

    ESP_LOGI("LedController", "Mode1: %d", this->pca9632->getMode1());
    ESP_LOGI("LedController", "Mode2: %d", this->pca9632->getMode2());

    return retval;
}