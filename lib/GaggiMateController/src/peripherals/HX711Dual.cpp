#ifdef ARDUINO_ARCH_STM32
#include "HX711Dual.h"

void HX711Dual::begin(uint8_t dout1, uint8_t dout2, uint8_t sck, uint8_t gain, uint32_t sckMode) {
    _dout1 = dout1;
    _dout2 = dout2;
    _sck = sck;

    switch (gain) {
    case 64:
        _gainPulses = 3;
        break;
    case 32:
        _gainPulses = 2;
        break;
    default:
        _gainPulses = 1;
        break; // 128
    }

    // Keep DOUT lines stable when a channel is disconnected/not populated.
    pinMode(_dout1, INPUT_PULLUP);
    pinMode(_dout2, INPUT_PULLUP);
    pinMode(_sck, sckMode);
    if (_mutex == nullptr) {
        _mutex = xSemaphoreCreateMutex();
    }
    power_up();
}

void HX711Dual::power_up() { digitalWrite(_sck, LOW); }

void HX711Dual::power_down() {
    // SCK held HIGH for >60µs puts HX711 into power-down
    digitalWrite(_sck, HIGH);
    delayMicroseconds(100);
}

bool HX711Dual::is_ready() const {
    // Some setups only populate one HX711 channel/module. Requiring both channels
    // to be ready causes permanent read timeouts and zero output.
    return digitalRead(_dout1) == LOW || digitalRead(_dout2) == LOW;
}

bool HX711Dual::wait_ready_timeout(unsigned long timeout_ms, unsigned long delay_ms, unsigned long /*fromCounter*/) {
    unsigned long d = (delay_ms > 0) ? delay_ms : 1;
    unsigned long start = millis();
    while (millis() - start < timeout_ms) {
        if (is_ready())
            return true;
        delay(d);
    }
    return false;
}

void HX711Dual::read_raw(long values[2]) {
    // Serialise concurrent callers (loop task vs calibrateChannel). We take the mutex
    // BEFORE waiting for the HX711 ready signal so no concurrent caller can steal the
    // sample between our ready-check and the clock sequence.
    if (!_mutex || xSemaphoreTake(_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
        values[0] = _offset1;
        values[1] = _offset2;
        return;
    }

    // Wait for HX711 to signal data ready while holding the mutex.
    // Any previous conversion must complete (~100 ms at 10 SPS) before we clock.
    {
        unsigned long start = millis();
        while (!is_ready()) {
            if (millis() - start > 600) {
                values[0] = _offset1;
                values[1] = _offset2;
                xSemaphoreGive(_mutex);
                return;
            }
            delay(1);
        }
    }

    // Protect the clock sequence: preemption with SCK high >60µs triggers HX711 power-down.
    // The critical section lasts ~50µs (25 clocks × 2µs). delayMicroseconds() uses
    // DWT->CYCCNT which works without interrupts.
    taskENTER_CRITICAL();

    const bool ready1 = (digitalRead(_dout1) == LOW);
    const bool ready2 = (digitalRead(_dout2) == LOW);

    unsigned long buf0 = 0, buf1 = 0;
    for (int i = 23; i >= 0; i--) {
        digitalWrite(_sck, HIGH);
        delayMicroseconds(1);
        if (ready1 && digitalRead(_dout1) == HIGH)
            buf0 |= (1UL << i);
        if (ready2 && digitalRead(_dout2) == HIGH)
            buf1 |= (1UL << i);
        digitalWrite(_sck, LOW);
        delayMicroseconds(1);
    }
    for (uint8_t i = 0; i < _gainPulses; i++) {
        digitalWrite(_sck, HIGH);
        delayMicroseconds(1);
        digitalWrite(_sck, LOW);
        delayMicroseconds(1);
    }

    taskEXIT_CRITICAL();

    // Sign-extend 24-bit two's complement to 32-bit.
    // If a channel wasn't ready at frame start, keep it at offset so get_value()
    // yields ~0 for that channel rather than phantom constants.
    values[0] = ready1 ? ((buf0 & 0x800000UL) ? (long)(buf0 | 0xFF000000UL) : (long)buf0) : _offset1;
    values[1] = ready2 ? ((buf1 & 0x800000UL) ? (long)(buf1 | 0xFF000000UL) : (long)buf1) : _offset2;

    xSemaphoreGive(_mutex);
}

void HX711Dual::read_average(long values[2], uint8_t times) {
    long sum0 = 0, sum1 = 0;
    long raw[2];
    for (uint8_t i = 0; i < times; i++) {
        read_raw(raw); // read_raw blocks until the HX711 is ready (or times out)
        sum0 += raw[0];
        sum1 += raw[1];
    }
    values[0] = (times > 0) ? sum0 / times : 0;
    values[1] = (times > 0) ? sum1 / times : 0;
}

void HX711Dual::tare(uint8_t times) {
    long values[2];
    read_average(values, times);
    _offset1 = values[0];
    _offset2 = values[1];
}

void HX711Dual::get_value(long *values, uint8_t times) {
    long avg[2];
    read_average(avg, times);
    values[0] = avg[0] - _offset1;
    values[1] = avg[1] - _offset2;
}

void HX711Dual::get_units(float *values, uint8_t times) {
    long raw[2];
    get_value(raw, times);
    values[0] = raw[0] / (_scale1 != 0.0f ? _scale1 : 1.0f);
    values[1] = raw[1] / (_scale2 != 0.0f ? _scale2 : 1.0f);
}

void HX711Dual::set_scale(float scale1, float scale2) {
    _scale1 = scale1;
    _scale2 = scale2;
}

void HX711Dual::set_offset(long offset1, long offset2) {
    _offset1 = offset1;
    _offset2 = offset2;
}

void HX711Dual::get_offset(long *offsets) {
    offsets[0] = _offset1;
    offsets[1] = _offset2;
}
#endif
