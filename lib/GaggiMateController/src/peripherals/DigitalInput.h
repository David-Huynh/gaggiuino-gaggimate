#ifndef DIGITALINPUT_H
#define DIGITALINPUT_H
#ifdef ARDUINO_ARCH_STM32
#include <STM32FreeRTOS.h>
#endif
#include <Arduino.h>

constexpr int INPUT_CHECK_INTERVAL_MS = 20;
constexpr unsigned long INPUT_DEBOUNCE_MS = 80;

using input_callback_t = std::function<void(const bool state)>;

class DigitalInput {
  public:
    DigitalInput(uint8_t pin, const input_callback_t &callback, int debounce_count = 1);
    void setup();
    void loop();
    bool getState() const { return !_stable_state; };

  private:
    uint8_t _pin;
    uint8_t _stable_state = HIGH;
    uint8_t _candidate_state = HIGH;
    unsigned long _candidate_since_ms = 0;
    xTaskHandle taskHandle;
    input_callback_t _callback;

    const char *LOG_TAG = "Heater";
    static void loopTask(void *arg);
};

#endif // DIGITALINPUT_H
