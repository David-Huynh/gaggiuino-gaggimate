#ifdef ARDUINO_ARCH_STM32
  #include <STM32FreeRTOS.h>
#endif
#include "DigitalInput.h"

DigitalInput::DigitalInput(uint8_t pin, const input_callback_t &callback, const int debounce_count)
    : _pin(pin), _callback(callback) {
    (void)debounce_count;
}

void DigitalInput::setup() {
    pinMode(_pin, INPUT_PULLUP);
    _stable_state = digitalRead(_pin);
    _candidate_state = _stable_state;
    _candidate_since_ms = millis();
    xTaskCreate(loopTask, "DigitalInput::loop", configMINIMAL_STACK_SIZE * 8, this, 1, &taskHandle);
}

void DigitalInput::loop() {
    const uint8_t raw_state = digitalRead(_pin);
    const unsigned long now = millis();

    if (raw_state != _candidate_state) {
        _candidate_state = raw_state;
        _candidate_since_ms = now;
        return;
    }

    if (raw_state != _stable_state && now - _candidate_since_ms >= INPUT_DEBOUNCE_MS) {
        _stable_state = raw_state;
        _callback(!_stable_state);
    }
}

void DigitalInput::loopTask(void *arg) {
    auto *input = static_cast<DigitalInput *>(arg);
    while (true) {
        input->loop();
        vTaskDelay(INPUT_CHECK_INTERVAL_MS / portTICK_PERIOD_MS);
    }
}
