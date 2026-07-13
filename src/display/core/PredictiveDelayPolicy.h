#ifndef PREDICTIVE_DELAY_POLICY_H
#define PREDICTIVE_DELAY_POLICY_H

#include <display/core/ScaleSourceResolver.h>

namespace PredictiveDelayPolicy {

constexpr double DEFAULT_HARDWARE_BREW_DELAY_MS = 800.0;

inline double brewDelayForSource(VolumetricMeasurementSource source, double brewDelayMs, double hardwareBrewDelayMs) {
    return source == VolumetricMeasurementSource::HARDWARE_SCALE ? hardwareBrewDelayMs : brewDelayMs;
}

// A movable cup scale can observe beverage carryover after the pump and valve
// stop. The drip-tray scale also observes OPV discharge, while pump-flow
// estimation has no independent final-weight observation; neither can safely
// teach the carryover delay.
inline bool supportsPostStopLearning(VolumetricMeasurementSource source) {
    return source == VolumetricMeasurementSource::BLUETOOTH;
}

} // namespace PredictiveDelayPolicy

#endif // PREDICTIVE_DELAY_POLICY_H
