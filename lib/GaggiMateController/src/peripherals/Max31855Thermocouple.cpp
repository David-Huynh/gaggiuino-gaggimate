#ifdef ARDUINO_ARCH_STM32
#include <STM32FreeRTOS.h>
#else
#include <freertos/FreeRTOS.h>
#endif
#include "Max31855Thermocouple.h"
#include "logging.h"
#include <Arduino.h>
#include <cmath>
#include <SPI.h>
#ifdef ARDUINO_ARCH_STM32
// On STM32, xTaskDelayUntil is usually named vTaskDelayUntil
#define xTaskDelayUntil vTaskDelayUntil
#endif
Max31855Thermocouple::Max31855Thermocouple(const int csPin, const int misoPin, const int sckPin,
                                           const temperature_callback_t &callback,
                                           const temperature_error_callback_t &error_callback)
    : taskHandle(nullptr), csPin(csPin), misoPin(misoPin), sckPin(sckPin) {
    max31855 = new MAX31855(csPin, misoPin, sckPin);
    this->callback = callback;
    this->error_callback = error_callback;
}

float Max31855Thermocouple::read() { return isErrorState() ? 0.0f : temperature.load(); }

bool Max31855Thermocouple::isErrorState() { return temperature <= 0 || errorCount >= MAX31855_MAX_ERRORS; }

bool Max31855Thermocouple::hasUsableReading() {
    return hasValidReading && lastReadStatus == STATUS_OK && std::isfinite(lastRawTemperature) &&
        lastRawTemperature > 0 && lastRawTemperature <= MAX_SAFE_TEMP &&
        static_cast<uint32_t>(millis()) - lastValidReadAt <= 1000 && !isErrorState();
}

void Max31855Thermocouple::setup() {
    SPI.begin();
    pinMode(csPin, OUTPUT);
    digitalWrite(csPin, HIGH);
    max31855->begin();
    max31855->setSPIspeed(1000000);

    if (xTaskCreate(monitorTask, "Max31855Thermocouple::monitor", configMINIMAL_STACK_SIZE * 8, this, 1, &taskHandle) != pdPASS) {
        taskHandle = nullptr;
    }
}

void Max31855Thermocouple::loop() {
    if (errorCount >= MAX31855_MAX_ERRORS || temperature > MAX_SAFE_TEMP) {
        ESP_LOGE(LOG_TAG, "Thermocouple failure! Error Count: %d, Temperature: %.2f\n", errorCount.load(), temperature.load());
        error_callback();
        return;
    }
    // If buffer has been filled up, remove the previous result from the error count
    if (resultCount == MAX31855_ERROR_WINDOW) {
        errorCount -= resultBuffer[bufferIndex];
    } else {
        ++resultCount;
    }

    float temp;
    int status = max31855->read();
    lastReadStatus = static_cast<uint8_t>(status);
    readCount++;
    if (status != STATUS_OK) {
        ESP_LOGE(LOG_TAG, "Failed to read temperature: %d\n", status);
        temp = 0.0f;
    } else {
        temp = max31855->getTemperature();
    }
    lastRawTemperature = temp;

    if (temp <= 0.0f) {
        ESP_LOGE(LOG_TAG, "Temperature reported below 0°C: %.2f\n", temp);
    }

    resultBuffer[bufferIndex] = (!std::isfinite(temp) || temp <= 0.0f) ? 1 : 0;
    errorCount += resultBuffer[bufferIndex];
    bufferIndex = (bufferIndex + 1) % MAX31855_ERROR_WINDOW;

    if (!std::isfinite(temp) || temp <= 0.0f) return;
    if (temp > MAX_SAFE_TEMP) { error_callback(); return; }
    // A warm restart must start from the actual reading, not an artificial 0 C
    // history that asks the heater for power while the boiler is already hot.
    temperature = hasValidReading.load() ? 0.2f * temp + 0.8f * temperature.load() : temp;
    hasValidReading = true;
    lastValidReadAt = millis();
    ESP_LOGV(LOG_TAG, "Updated temperature: %2f\n", temperature.load());
    callback(temperature.load());
}

[[noreturn]] void Max31855Thermocouple::monitorTask(void *arg) {
    TickType_t lastWake = xTaskGetTickCount();
    auto *thermocouple = static_cast<Max31855Thermocouple *>(arg);
    while (true) {
        thermocouple->loop();
        xTaskDelayUntil(&lastWake, pdMS_TO_TICKS(MAX31855_UPDATE_INTERVAL));
    }
}
