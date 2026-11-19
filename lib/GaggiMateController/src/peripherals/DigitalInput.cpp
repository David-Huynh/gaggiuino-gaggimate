#if defined(ARDUINO_ARCH_STM32)
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

#else
#include "DigitalInput.h"

DigitalInput::DigitalInput(uint8_t pin, const input_callback_t &callback, const int debounce_count,
                           const int release_debounce_count)
    : _pin(pin), _debounce_count(debounce_count),
      _release_debounce_count(release_debounce_count > 0 ? release_debounce_count : debounce_count), _callback(callback) {}

void DigitalInput::setup() {
    pinMode(_pin, INPUT_PULLUP);
    xTaskCreate(loopTask, "DigitalInput::loop", configMINIMAL_STACK_SIZE * 4, this, 1, &taskHandle);
}

void DigitalInput::loop() {
    if (const int state = digitalRead(_pin); state != _last_state) {
        _stable_counter = 1;
        _last_state = state;
        ESP_LOGI("DigitalInput", "Volatile state changed: %d, counter: %d", !_last_state, _stable_counter);
    } else {
        _stable_counter++;
    }
    const auto threshold = _last_state == LOW ? _debounce_count : _release_debounce_count;
    if (_stable_counter >= threshold && _current_state != _last_state) {
        _current_state = _last_state;
        _callback(!_current_state);
        ESP_LOGI("DigitalInput", "Stable State changed: %d, counter: %d", !_current_state, _stable_counter);
    }
    _stable_counter = min(_stable_counter, threshold);
}

void DigitalInput::loopTask(void *arg) {
    auto *input = static_cast<DigitalInput *>(arg);
    while (true) {
        input->loop();
        vTaskDelay(INPUT_CHECK_INTERVAL_MS / portTICK_PERIOD_MS);
    }
}

#endif
