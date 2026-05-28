#ifndef HX711SCALE_H
#define HX711SCALE_H
#if defined(ARDUINO_ARCH_STM32) && !defined(GAGGIMATE_DISABLE_HARDWARE_SCALE)
#include <Arduino.h>
#include <STM32FreeRTOS.h>
#include <functional>

#include "ChannelFilter.h"
#include "GaggiMateComm.h"
#include "HX711Dual.h"

// Output cadence is fixed at 10 Hz regardless of native HX711 rate. At 80 Hz
// native we oversample × 8 (median of 8 raw → one output); at 10 Hz native
// each raw sample is one output.
constexpr uint32_t SCALE_OUTPUT_PERIOD_MS = 100;

constexpr int SCALE_INIT_TIMEOUT_MS = 5000;
constexpr int SCALE_READY_DELAY_MS = 10;
constexpr int SCALE_INIT_RETRIES = 3;
constexpr int SCALE_INIT_RETRY_DELAY_MS = 250;
// Once the bus is alive, both HX711 convert in lockstep off the shared clock, so a
// present second channel appears within ~1-2 sample periods. Bounds how long we
// wait for the second channel before declaring it missing and retrying.
constexpr int SCALE_BOTH_READY_TIMEOUT_MS = 1000;

// Single immutable snapshot published every output tick. Trivially copyable so
// callers can grab it under a brief mutex and read fields without further care.
// Mirrors the wire-side ScaleSample but keeps a separate name on the controller
// to make the producer/consumer split explicit.
struct ScaleSnapshot {
    float weightG;     // sum of both filtered channels
    float stddevG;     // sqrt(stddev1² + stddev2²) — combined noise floor
    float ch1G;        // per-channel filtered output
    float ch2G;
    float ch1StdG;     // per-channel rolling stddev
    float ch2StdG;
    uint16_t healthBits;
    uint32_t sampleSeq;
};

enum class ScaleCalError : uint8_t {
    OK = 0,
    REF_OUT_OF_RANGE = 1,
    TOO_NOISY = 2,
    FACTOR_OUT_OF_RANGE = 3,
    NOT_PRESENT = 4,
    TIMEOUT = 5,
    BUSY = 6,
};

using scale_sample_cb_t = std::function<void(const ScaleSnapshot &)>;
using scale_tare_progress_cb_t = std::function<void(uint16_t samples, float stddevG)>;
using scale_tare_done_cb_t =
    std::function<void(long offset1, long offset2, float stddev1, float stddev2, bool success, uint16_t healthBits)>;
using scale_cal_progress_cb_t = std::function<void(uint8_t channel, uint16_t samples, float stddevG)>;
using scale_cal_done_cb_t =
    std::function<void(uint8_t channel, float factor, float stddevG, bool success, uint8_t errorCode)>;

class HX711Scale {
  public:
    // calibration1/2: counts-per-gram. Use 0.0f as the NOT_CALIBRATED sentinel.
    HX711Scale(uint8_t dout1_pin, uint8_t dout2_pin, uint8_t sck_pin, const scale_sample_cb_t &sampleCb,
               float calibration1 = 0.0f, float calibration2 = 0.0f);
    ~HX711Scale() = default;

    void setup();
    bool isPresent() const { return _present; }
    uint16_t getNativeHz() const { return _nativeHz; }

    // Snapshot accessor. Safe to call from any task.
    ScaleSnapshot snapshot();

    // Calibration coefficients (counts per gram). 0.0f marks a channel as
    // uncalibrated; downstream output will be NaN for that channel.
    void setCalibration(float c1, float c2);
    void getCalibration(float &c1, float &c2) const;

    // Zero-point offsets in raw ADC counts.
    void setOffset(long o1, long o2);
    void getOffsets(long &o1, long &o2) const;

    // Runtime gain change (32, 64, 128). Triggers a soft-reset of the filter.
    void setGain(uint8_t gain);

    // Async — settling-aware. requestTare() returns immediately; tare progress
    // and completion arrive via callbacks.
    void requestTare();

    // refWeight in grams; range checked. Channel = 1 or 2.
    void requestCalibration(uint8_t channel, float refWeight);

    // Callback registration.
    void setSampleCallback(const scale_sample_cb_t &cb) { _sampleCb = cb; }
    void setTareProgressCallback(const scale_tare_progress_cb_t &cb) { _tareProgCb = cb; }
    void setTareDoneCallback(const scale_tare_done_cb_t &cb) { _tareDoneCb = cb; }
    void setCalProgressCallback(const scale_cal_progress_cb_t &cb) { _calProgCb = cb; }
    void setCalDoneCallback(const scale_cal_done_cb_t &cb) { _calDoneCb = cb; }

  private:
    // ----- pins / driver -----
    uint8_t _dout1_pin;
    uint8_t _dout2_pin;
    uint8_t _sck_pin;
    HX711Dual *_drv = nullptr;
    bool _present = false;

    // ----- rate state -----
    uint16_t _nativeHz = 10;     // detected at setup; 10 or 80
    uint16_t _outputDivisor = 1; // 1 at 10 Hz native, 8 at 80 Hz native
    uint16_t _samplePeriodMs = SCALE_OUTPUT_PERIOD_MS; // 100 ms / 12 ms
    uint32_t _lastInitAttemptMs = 0;

    // ----- calibration / offsets (set externally, read in scale loop) -----
    float _scale1 = 0.0f;
    float _scale2 = 0.0f;
    long _offset1 = 0;
    long _offset2 = 0;

    // ----- DSP per channel -----
    ChannelFilter _filter1;
    ChannelFilter _filter2;
    // Pre-output median buffer for 80 Hz oversampling. Holds raw counts.
    static constexpr uint8_t PRE_BUF_MAX = 8;
    long _preBuf1[PRE_BUF_MAX]{};
    long _preBuf2[PRE_BUF_MAX]{};
    uint8_t _preCount = 0;
    uint8_t _preValidCount1 = 0;
    uint8_t _preValidCount2 = 0;

    // ----- consecutive-good counters used to clear sticky health bits -----
    uint8_t _consecGoodCh1 = 0;
    uint8_t _consecGoodCh2 = 0;
    uint8_t _consecBadCh1 = 0;
    uint8_t _consecBadCh2 = 0;

    // ----- snapshot / publication -----
    SemaphoreHandle_t _snapMutex = nullptr;
    ScaleSnapshot _snapshot{};
    uint16_t _healthBits = SCALE_HEALTH_NOT_CALIBRATED | SCALE_HEALTH_STALE;
    uint32_t _sampleSeq = 0;
    uint32_t _lastOkRead = 0;

    // ----- async tare worker state -----
    enum class TareState : uint8_t { IDLE, WARMUP, COLLECT };
    volatile bool _tareRequested = false;
    TareState _tareState = TareState::IDLE;
    uint16_t _tareWarmupRemaining = 0;
    uint16_t _tareCollected = 0;
    uint32_t _tareStartMs = 0;
    uint32_t _tareLastProgMs = 0;
    static constexpr size_t TARE_RING_MAX = 256; // sized for 80 Hz × 2.4 s + headroom
    long _tareBuf1[TARE_RING_MAX]{};
    long _tareBuf2[TARE_RING_MAX]{};

    // ----- async calibration worker state -----
    enum class CalState : uint8_t { IDLE, WARMUP, COLLECT };
    volatile bool _calRequested = false;
    uint8_t _calRequestChannel = 0;
    float _calRequestRefWeight = 0.0f;
    CalState _calState = CalState::IDLE;
    uint8_t _calChannel = 0;
    float _calRefWeight = 0.0f;
    uint16_t _calWarmupRemaining = 0;
    uint16_t _calCollected = 0;
    uint32_t _calStartMs = 0;
    uint32_t _calLastProgMs = 0;
    static constexpr size_t CAL_RING_MAX = 320;
    long _calBuf[CAL_RING_MAX]{};

    // ----- per-rate sample-window constants (filled at setup) -----
    uint16_t _tareWarmupSamples = 6;
    uint16_t _tareTargetSamples = 24;
    uint16_t _tareMinSamples = 18;
    uint16_t _calWarmupSamples = 6;
    uint16_t _calTargetSamples = 32;
    static constexpr float TARE_STDDEV_THRESHOLD_G = 0.05f;
    static constexpr float TARE_STDDEV_FAIL_MULT = 5.0f;
    static constexpr uint32_t TARE_TIMEOUT_MS = 3000;
    static constexpr uint32_t CAL_TIMEOUT_MS = 5000;
    static constexpr float CAL_STDDEV_THRESHOLD_G = 0.5f;
    static constexpr float CAL_FACTOR_MIN = 100.0f;
    static constexpr float CAL_FACTOR_MAX = 1.0e6f;
    static constexpr float CAL_REF_MIN = 0.5f;
    static constexpr float CAL_REF_MAX = 5000.0f;
    static constexpr uint32_t PROGRESS_PERIOD_MS = 100;
    static constexpr uint32_t STALE_TIMEOUT_MS = 500;

    // ----- callbacks -----
    scale_sample_cb_t _sampleCb;
    scale_tare_progress_cb_t _tareProgCb;
    scale_tare_done_cb_t _tareDoneCb;
    scale_cal_progress_cb_t _calProgCb;
    scale_cal_done_cb_t _calDoneCb;

    xTaskHandle _taskHandle = nullptr;

    // ----- internals -----
    [[noreturn]] static void loopTask(void *arg);
    bool initializeDriver();
    void loop();
    void processOneRawSample(long raw[2], bool valid[2], bool sat[2]);
    void emitOutputSample(long medRaw1, long medRaw2, bool ch1Valid, bool ch2Valid);
    void updateHealthForOutput(bool ch1Valid, bool ch2Valid);
    void publishSnapshot();
    void advanceTareSm(long raw[2], bool valid[2], bool sat[2]);
    void advanceCalSm(long raw[2], bool valid[2], bool sat[2]);
    void finishTare(bool success, uint16_t flagsToAdd);
    void finishCal(bool success, ScaleCalError err, float factor, float stddevAtCal);
    void recomputeNotCalibratedFlag();

    static long medianLong(long *buf, size_t n);
    static float trimmedMeanLong(long *buf, size_t n, float trimFrac);
    static float stddevLong(const long *buf, size_t n);

    const char *LOG_TAG = "HX711Scale";
};
#endif
#endif // HX711SCALE_H
