#ifndef HEATER_H
#define HEATER_H
#ifdef ARDUINO_ARCH_STM32
#include <STM32FreeRTOS.h>
#else
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#endif
#include <atomic>
#include "Autotune/Autotune.h"
#include "Max31855Thermocouple.h"
#include "TemperatureSensor.h"
#include <SimplePID/SimplePID.h>

constexpr float MAX_AUTOTUNE_TEMP = 125.0f;
constexpr float TUNER_OUTPUT_SPAN = 1000.0f;

using heater_error_callback_t = std::function<void()>;
using heater_autotune_fail_callback_t = std::function<void()>;
// Kff = combinedKff = output units per watt (disturbance feedforward), derived
// in Heater::loopAutotune as 1000 / heaterWattage when wattage > 0; 0 when the
// caller didn't supply a wattage.
using pid_result_callback_t = std::function<void(float Kp, float Ki, float Kd, float Kff)>;

class Heater {
  public:
    Heater(TemperatureSensor *sensor, uint8_t heaterPin, const heater_error_callback_t &error_callback,
           const pid_result_callback_t &pid_callback, const heater_autotune_fail_callback_t &autotune_fail_callback = nullptr);
    void setup();
    void loop();

    void setSetpoint(float setpoint);
    float getSetpoint() const { return hasConfiguration() ? requestedSetpoint.load() : 0.0f; }
    float getDutyCycle() const { return hasConfiguration() ? reportedOutput.load() / TUNER_OUTPUT_SPAN * 100.0f : 0.0f; }
    float getOutput() const { return reportedOutput.load(); }
    bool isRelayOn() const { return relayStatus; }
    bool configure(float Kp, float Ki, float Kd, float Kff);
    void invalidateConfiguration();
    bool hasConfiguration() const { return configured.load() && installedEpoch.load() == controlEpoch.load(); }
    static constexpr uint32_t TARGET_LEASE_MS = 3000;
    void autotune(int testTimeSec, int windowSize, int heaterWattage);

    // Thermal feedforward control
    void setThermalFeedforward(float *pumpFlowPtr = nullptr, float incomingWaterTemp = 23.0f, int *valveStatusPtr = nullptr);


  private:
    void setupPid();
    void applyConfiguration();
    void stopOutput();
    void driveRelayOff();
    bool canEnergize(uint32_t now) const;
    struct PidConfiguration { float kp = 0, ki = 0, kd = 0, kff = 0; uint32_t epoch = 0; };
    PidConfiguration pendingConfiguration;
    SemaphoreHandle_t configurationMutex = nullptr;
    bool configurationPending = false;
    std::atomic<bool> configured{false};
    std::atomic<uint32_t> controlEpoch{1}, installedEpoch{0};
    std::atomic<float> requestedSetpoint{0};
    std::atomic<uint32_t> lastTargetAt{0};
    void setupAutotune(int testTimeSec, int windowSize, int heaterWattage);
    void loopPid();
    void loopAutotune();
    float softPwm(uint32_t windowSize);
    void plot(float optimumOutput, float outputScale, uint8_t everyNth);
    float calculateDisturbanceFeedforwardGain();
    float calculateSafetyScaling(float tempError);
    TemperatureSensor *sensor;
    uint8_t heaterPin;
    xTaskHandle taskHandle;
    SimplePID *simplePid = nullptr;
    Autotune *autotuner = nullptr;

    heater_error_callback_t error_callback;
    pid_result_callback_t pid_callback;
    heater_autotune_fail_callback_t autotune_fail_callback;

    float temperature = 0.0f;
    float output = 0.0f; // PID storage: accessed only by the heater task
    std::atomic<float> reportedOutput{0};
    float setpoint = 0.0f;
    int plotCount = 0;

    std::atomic<bool> relayStatus{false};
    unsigned long windowStartTime = 0;


    // Autotune variables
    bool startup = true;
    std::atomic<bool> autotuning{false};
    // Stashed at autotune-start (BLE field 3) so loopAutotune can derive
    // combinedKff = 1000 / wattage on completion. 0 ⇒ caller didn't supply
    // wattage (older display firmware) ⇒ skip combinedKff derivation.
    int autotuneHeaterWattage = 0;

    // Thermal feedforward variables
    float *pumpFlowRate = nullptr;
    int *valveStatus = nullptr;
    float lastSafetyFactor = 1.0f; // For smooth safety scaling transitions
    float incomingWaterTemp = 23.0f;
    float heaterEfficiency = 0.95f; // 95% efficiency (immersion heater)
    float heatLossWatts = 5.0f;     // 5W heat loss (well-insulated boiler)
    float combinedKff = 0.0f;       // Combined feedforward gain (output units per watt) - disabled by default

    // Thermal model constants
    static constexpr float WATER_DENSITY = 1.0f;        // g/ml
    static constexpr float WATER_SPECIFIC_HEAT = 4.18f; // J/(g·°C)

    const char *LOG_TAG = "Heater";
    static void loopTask(void *arg);
};

#endif // HEATER_H
