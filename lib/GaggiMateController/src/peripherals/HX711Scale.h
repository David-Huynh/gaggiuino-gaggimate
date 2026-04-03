#ifndef HX711SCALE_H
#define HX711SCALE_H

#include <Arduino.h>
#include <functional>

#ifdef ARDUINO_ARCH_STM32
#include <STM32FreeRTOS.h>
#else
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#endif

#include <HX711_2.h>

constexpr int SCALE_READ_INTERVAL_MS = 100;
constexpr int SCALE_TARE_READINGS = 4;
constexpr int SCALE_INIT_TIMEOUT_MS = 1000;
constexpr int SCALE_READY_DELAY_MS = 10;

using weight_callback_t = std::function<void(float)>;
using tare_result_callback_t = std::function<void(long offset1, long offset2)>;

class HX711Scale {
  public:
    HX711Scale(uint8_t dout1_pin, uint8_t dout2_pin, uint8_t sck_pin, const weight_callback_t &callback,
               float calibration1 = 1.0f, float calibration2 = 1.0f);
    ~HX711Scale() = default;

    void setup();
    void loop();
    void tare();

    inline float getWeight() const { return _weight; }
    inline bool isPresent() const { return _present; }

    // Calibration coefficients (grams per raw ADC unit)
    void setCalibration(float calibration1, float calibration2);
    void getCalibration(float &calibration1, float &calibration2) const;

    // Zero-point offsets (raw ADC value when scale is empty)
    void setOffset(long offset1, long offset2);
    void getOffsets(long &offset1, long &offset2) const;

    // Per-channel calibration: place refWeight on channel, call this to compute & apply new factor.
    // Returns the new scale factor for that channel (0 on failure).
    float calibrateChannel(uint8_t channel, float refWeight);

    // Optional callback fired after every tare with the new raw offsets
    void setTareResultCallback(const tare_result_callback_t &cb) { _tareResultCallback = cb; }

  private:
    uint8_t _dout1_pin;
    uint8_t _dout2_pin;
    uint8_t _sck_pin;
    float _calibration1;
    float _calibration2;
    long _offset1 = 0;
    long _offset2 = 0;
    float _weight = 0.0f;
    bool _present = false;

    HX711_2 *_loadCells = nullptr;

    weight_callback_t _callback;
    tare_result_callback_t _tareResultCallback = nullptr;
    xTaskHandle _taskHandle = nullptr;

    const char *LOG_TAG = "HX711Scale";
    [[noreturn]] static void loopTask(void *arg);
};

#endif // HX711SCALE_H
