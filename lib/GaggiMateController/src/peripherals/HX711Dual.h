#pragma once
#ifdef ARDUINO_ARCH_STM32
#include <Arduino.h>
#include <STM32FreeRTOS.h>

// FreeRTOS-compatible dual-channel HX711 driver using software bit-bang.
// Drop-in replacement for the banoz HX711_2 timer-ISR library.
// No hardware timer used — SCK is clocked directly in the reading task,
// protected by a brief critical section to prevent preemption mid-frame.
class HX711Dual {
  public:
    void begin(uint8_t dout1, uint8_t dout2, uint8_t sck,
               uint8_t gain = 128, uint32_t sckMode = OUTPUT);
    void set_scale(float scale1 = 1.0f, float scale2 = 1.0f);
    void power_up();
    void power_down();
    bool wait_ready_timeout(unsigned long timeout_ms, unsigned long delay_ms = 0,
                            unsigned long fromCounter = 0);
    void tare(uint8_t times = 10);
    void get_units(float *values, uint8_t times = 1);
    void get_value(long *values, uint8_t times = 1);
    void set_offset(long offset1 = 0, long offset2 = 0);
    void get_offset(long *offsets);

  private:
    uint8_t _dout1 = 0, _dout2 = 0, _sck = 0;
    uint8_t _gainPulses = 1; // extra clocks after data: 128→1, 32→2, 64→3
    float _scale1 = 1.0f, _scale2 = 1.0f;
    long _offset1 = 0, _offset2 = 0;
    SemaphoreHandle_t _mutex = nullptr;

    bool is_ready() const;
    void read_raw(long values[2]);
    void read_average(long values[2], uint8_t times);
};
#endif
