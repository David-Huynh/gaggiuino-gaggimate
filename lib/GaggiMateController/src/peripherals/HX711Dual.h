#pragma once
#if defined(ARDUINO_ARCH_STM32) && !defined(GAGGIMATE_DISABLE_HARDWARE_SCALE)
#include <Arduino.h>
#include <STM32FreeRTOS.h>

// Outcome of a single read attempt. Values are filled only on OK.
enum class HxResult : uint8_t {
    OK = 0,
    NOT_READY = 1,
    MUTEX_TIMEOUT = 2,
};

struct HxStatus {
    uint32_t okCount = 0;
    uint32_t notReadyCount = 0;
    uint32_t mutexTimeoutCount = 0;
    uint32_t saturationCount = 0; // any channel saturated on a successful read
    uint16_t nativeHz = 0;        // 10 or 80 once detected, 0 otherwise
};

// FreeRTOS-compatible dual-channel HX711 driver using software bit-bang.
// Returns raw 24-bit signed counts only; calibration math lives in HX711Scale.
class HX711Dual {
  public:
    // Saturation threshold: ~1% from full-scale rails of the 24-bit signed range.
    static constexpr long SATURATION_THRESHOLD = 0x7E0000;

    void begin(uint8_t dout1, uint8_t dout2, uint8_t sck, uint8_t gain = 128, uint32_t sckMode = OUTPUT);
    void power_up();
    void power_down();

    // Block until at least one DOUT line goes low (data ready) or timeout elapses.
    bool wait_ready_timeout(unsigned long timeout_ms, unsigned long delay_ms = 0);

    // Fast probe — returns true if data is currently ready without blocking.
    bool is_ready() const;

    // Single-shot read attempt. On OK fills `values` with raw signed 24-bit counts,
    // `valid` with per-channel readiness, and `saturated` with per-channel saturation
    // flags. NOT_READY if both DOUT lines are high (no fresh data); MUTEX_TIMEOUT if
    // another caller holds the bus longer than 50 ms.
    HxResult try_read(long values[2], bool valid[2], bool saturated[2]);

    // Set gain factor (32, 64, or 128). The setting takes effect on the next read.
    void set_gain(uint8_t gain);

    // True if |raw| exceeds SATURATION_THRESHOLD.
    static bool is_saturated(long raw) { return raw > SATURATION_THRESHOLD || raw < -SATURATION_THRESHOLD; }

    // Time consecutive ready cycles to classify the HX711 as 10 Hz or 80 Hz native.
    // Caller must have a powered-up driver with the data line responding.
    // Returns the cached value if already detected; safe default 10 Hz on failure.
    uint16_t detect_rate();
    uint16_t getNativeHz() const { return _nativeHz; }

    HxStatus read_status() const { return _status; }

  private:
    uint8_t _dout1 = 0, _dout2 = 0, _sck = 0;
    uint8_t _gainPulses = 1; // extra clocks after data: 128→1, 32→2, 64→3
    SemaphoreHandle_t _mutex = nullptr;
    uint16_t _nativeHz = 0;
    HxStatus _status{};
};
#endif
