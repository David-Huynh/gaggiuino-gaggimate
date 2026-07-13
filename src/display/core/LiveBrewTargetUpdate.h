#ifndef LIVEBREWTARGETUPDATE_H
#define LIVEBREWTARGETUPDATE_H

#include <cstdint>

constexpr float LIVE_BREW_MIN_PRESSURE_BAR = 0.0f;
constexpr float LIVE_BREW_MAX_PRESSURE_BAR = 12.0f;
constexpr float LIVE_BREW_MIN_FLOW_ML_S = 0.0f;
constexpr float LIVE_BREW_MAX_FLOW_ML_S = 20.0f;
constexpr float LIVE_BREW_MIN_TEMPERATURE_C = 20.0f;
constexpr float LIVE_BREW_MAX_TEMPERATURE_C = 100.0f;
constexpr float LIVE_BREW_MIN_YIELD_STOP_G = 5.0f;
constexpr float LIVE_BREW_MAX_YIELD_STOP_G = 90.0f;
constexpr float LIVE_BREW_MIN_PUMP_DUTY = 0.0f;
constexpr float LIVE_BREW_MAX_PUMP_DUTY = 1.0f;
constexpr float LIVE_BREW_MIN_VALVE_POSITION = 0.0f;
constexpr float LIVE_BREW_MAX_VALVE_POSITION = 1.0f;

struct LiveBrewTargetUpdate {
    bool hasPressureTarget = false;
    float pressureTargetBar = 0.0f;
    bool hasFlowTarget = false;
    float flowTargetMlS = 0.0f;
    bool hasPumpDuty = false;
    float pumpDuty = 0.0f;
    bool hasValvePosition = false;
    float valvePosition = 0.0f;
    bool hasTemperatureTarget = false;
    float temperatureTargetC = 0.0f;
    bool hasYieldStopTarget = false;
    float yieldStopTargetG = 0.0f;
    bool stopRequested = false;

    bool hasAnyField() const {
        return hasPressureTarget || hasFlowTarget || hasPumpDuty || hasValvePosition || hasTemperatureTarget ||
               hasYieldStopTarget || stopRequested;
    }
};

enum class LiveBrewTargetApplyStatus : uint8_t {
    APPLIED,
    NOT_ACTIVE_BREW,
    YIELD_REQUIRES_VOLUMETRIC,
};

#endif // LIVEBREWTARGETUPDATE_H
