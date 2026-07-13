#if defined(ARDUINO_ARCH_STM32) && !defined(GAGGIMATE_DISABLE_HARDWARE_SCALE)
#include "HX711Dual.h"

void HX711Dual::begin(uint8_t dout1, uint8_t dout2, uint8_t sck, uint8_t gain, uint32_t sckMode) {
    _dout1 = dout1;
    _dout2 = dout2;
    _sck = sck;

    set_gain(gain);

    pinMode(_dout1, INPUT_PULLUP);
    pinMode(_dout2, INPUT_PULLUP);
    pinMode(_sck, sckMode);
    if (_mutex == nullptr) {
        _mutex = xSemaphoreCreateMutex();
    }
    power_up();
}

void HX711Dual::set_gain(uint8_t gain) {
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
}

void HX711Dual::power_up() { digitalWrite(_sck, LOW); }

void HX711Dual::power_down() {
    // SCK held HIGH for >60µs puts HX711 into power-down
    digitalWrite(_sck, HIGH);
    delayMicroseconds(100);
}

bool HX711Dual::is_ready() const {
    return digitalRead(_dout1) == LOW && digitalRead(_dout2) == LOW;
}

bool HX711Dual::wait_ready_timeout(unsigned long timeout_ms, unsigned long delay_ms) {
    unsigned long d = (delay_ms > 0) ? delay_ms : 1;
    unsigned long start = millis();
    while (millis() - start < timeout_ms) {
        if (is_ready())
            return true;
        delay(d);
    }
    return false;
}

HxResult HX711Dual::try_read(long values[2], bool valid[2], bool saturated[2]) {
    if (!_mutex || xSemaphoreTake(_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        ++_status.mutexTimeoutCount;
        return HxResult::MUTEX_TIMEOUT;
    }

    // No fresh data — bail out without clocking. The caller (scale loop) is expected
    // to call wait_ready_timeout() before try_read(), but we still guard here so
    // ill-timed callers don't trigger a phantom conversion or block on the bus.
    if (!is_ready()) {
        xSemaphoreGive(_mutex);
        ++_status.notReadyCount;
        return HxResult::NOT_READY;
    }

    // HX711 timing constraint: SCK held HIGH for >60 us latches the chip into
    // power-down. The obvious guard is to mask interrupts for the whole 25-clock
    // burst -- but on this single-core MCU that blocks the no-FIFO USART RX ISR
    // for ~50-150 us (>1 byte-time at 115200 baud), so an inbound command byte
    // arriving mid-read is silently dropped -> garbled/lost CMD -> valve/pump lag
    // and spurious protocol errors (the very thing that used to brick brewing).
    // Instead, guard only each individual SCK-HIGH pulse: HIGH is the window that
    // must stay <60 us, while SCK held LOW between bits is harmless. This caps the
    // masked window at a single ~1-2 us pulse and lets the UART ISR run between
    // bits. try_read holds _mutex for the whole call, so no other task can clock
    // the bus while we are briefly preempted between bits.
    const bool ready1 = (digitalRead(_dout1) == LOW);
    const bool ready2 = (digitalRead(_dout2) == LOW);

    unsigned long buf0 = 0, buf1 = 0;
    for (int i = 23; i >= 0; i--) {
        taskENTER_CRITICAL();
        digitalWrite(_sck, HIGH);
        delayMicroseconds(1);
        if (ready1 && digitalRead(_dout1) == HIGH)
            buf0 |= (1UL << i);
        if (ready2 && digitalRead(_dout2) == HIGH)
            buf1 |= (1UL << i);
        digitalWrite(_sck, LOW);
        taskEXIT_CRITICAL();
        delayMicroseconds(1);
    }
    for (uint8_t i = 0; i < _gainPulses; i++) {
        taskENTER_CRITICAL();
        digitalWrite(_sck, HIGH);
        delayMicroseconds(1);
        digitalWrite(_sck, LOW);
        taskEXIT_CRITICAL();
        delayMicroseconds(1);
    }

    // Sign-extend 24-bit two's complement to 32-bit.
    long v0 = ready1 ? ((buf0 & 0x800000UL) ? (long)(buf0 | 0xFF000000UL) : (long)buf0) : 0;
    long v1 = ready2 ? ((buf1 & 0x800000UL) ? (long)(buf1 | 0xFF000000UL) : (long)buf1) : 0;

    valid[0] = ready1;
    valid[1] = ready2;
    saturated[0] = ready1 && is_saturated(v0);
    saturated[1] = ready2 && is_saturated(v1);
    values[0] = v0;
    values[1] = v1;

    xSemaphoreGive(_mutex);

    if (saturated[0] || saturated[1]) {
        ++_status.saturationCount;
    }
    ++_status.okCount;
    return HxResult::OK;
}

uint16_t HX711Dual::detect_rate() {
    if (_nativeHz != 0) {
        return _nativeHz;
    }

    // Discard one sample so we start mid-cycle.
    long values[2] = {0, 0};
    bool valid[2] = {false, false};
    bool sat[2] = {false, false};
    if (!wait_ready_timeout(500, 1)) {
        _nativeHz = 10;
        _status.nativeHz = _nativeHz;
        return _nativeHz;
    }
    try_read(values, valid, sat);

    constexpr int N = 16;
    constexpr unsigned long perSampleTimeoutMs = 200;
    constexpr unsigned long totalBudgetMs = 3000;

    unsigned long startMs = millis();
    int got = 0;
    for (int i = 0; i < N && (millis() - startMs) < totalBudgetMs; i++) {
        if (!wait_ready_timeout(perSampleTimeoutMs, 1)) {
            break;
        }
        if (try_read(values, valid, sat) == HxResult::OK) {
            got++;
        }
    }
    unsigned long elapsed = millis() - startMs;

    // Need at least 4 samples to make a confident call; otherwise default to 10 Hz.
    if (got < 4 || elapsed == 0) {
        _nativeHz = 10;
    } else {
        // 80 Hz → ~12.5 ms/sample; 10 Hz → ~100 ms/sample. Decide at 30 ms.
        unsigned long perSampleMs = elapsed / static_cast<unsigned long>(got);
        _nativeHz = (perSampleMs < 30) ? 80 : 10;
    }
    _status.nativeHz = _nativeHz;
    return _nativeHz;
}
#endif
