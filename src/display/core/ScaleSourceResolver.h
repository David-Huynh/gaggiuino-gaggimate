#ifndef SCALE_SOURCE_RESOLVER_H
#define SCALE_SOURCE_RESOLVER_H

// Source type for a single weight measurement. Shared by the Controller, the
// scale plugins (BLE / HW), and the role resolver below. Kept free of
// Arduino/ESP headers so the resolution logic stays a small, self-contained
// unit that is easy to reason about (and unit-test) in isolation.
enum class VolumetricMeasurementSource { INACTIVE, FLOW_ESTIMATION, BLUETOOTH, HARDWARE_SCALE };

enum class ScaleRole { BREW, GRIND };

// Persisted brew scale-source setting values (stored in Settings as ints).
enum ScaleSourceSetting {
    SCALE_SOURCE_AUTO = 0,
    SCALE_SOURCE_BLUETOOTH = 1,
    SCALE_SOURCE_HARDWARE = 2,
    SCALE_SOURCE_PREDICTIVE = 3,
    SCALE_SOURCE_OFF = 4,
};

// Snapshot of which sources are currently usable. The Controller builds this
// from live connection/plugin state and hands it to the pure resolver, so the
// resolver never has to reach into Arduino globals.
struct ScaleAvailability {
    bool bluetoothConnected = false;      // Any BLE scale is connected and recently publishing
    bool brewBluetoothConnected = false;  // Brew-role BLE scale is connected and recently publishing
    bool grindBluetoothConnected = false; // Grind-role BLE scale is connected and recently publishing
    bool hardwarePresent = false;    // HX711 hardware scale detected
    bool hardwareCapable = false;    // build + controller actually support a HW scale
    bool predictiveAvailable = true; // pump-flow estimation (always on unless OFF)
};

namespace ScaleSourceResolver {

// True when the hardware scale can currently serve as a source.
inline bool hardwareUsable(const ScaleAvailability &a) { return a.hardwareCapable && a.hardwarePresent; }

// Resolve the active source for the BREW (and general display) role.
//
// Explicit user selection wins when it is available; otherwise we fall back
// predictably. Predictive pump-flow is the universal floor, so a brew never
// blocks on an unavailable scale — only an explicit OFF yields INACTIVE (which
// the caller turns into a timed brew). AUTO prefers hardware, then Bluetooth,
// then predictive.
inline VolumetricMeasurementSource resolveBrewSource(int setting, const ScaleAvailability &a) {
    switch (setting) {
    case SCALE_SOURCE_OFF:
        return VolumetricMeasurementSource::INACTIVE;
    case SCALE_SOURCE_PREDICTIVE:
        return VolumetricMeasurementSource::FLOW_ESTIMATION;
    case SCALE_SOURCE_BLUETOOTH:
        if (a.brewBluetoothConnected)
            return VolumetricMeasurementSource::BLUETOOTH;
        if (hardwareUsable(a))
            return VolumetricMeasurementSource::HARDWARE_SCALE;
        return VolumetricMeasurementSource::FLOW_ESTIMATION;
    case SCALE_SOURCE_HARDWARE:
        if (hardwareUsable(a))
            return VolumetricMeasurementSource::HARDWARE_SCALE;
        return VolumetricMeasurementSource::FLOW_ESTIMATION;
    case SCALE_SOURCE_AUTO:
    default:
        if (hardwareUsable(a))
            return VolumetricMeasurementSource::HARDWARE_SCALE;
        if (a.brewBluetoothConnected)
            return VolumetricMeasurementSource::BLUETOOTH;
        return VolumetricMeasurementSource::FLOW_ESTIMATION;
    }
}

// Resolve the active source for the GRIND-BY-WEIGHT role.
//
// Grinding has no pump flow, so predictive is not a valid fallback. The
// hardware scale is fixed in the brew path, so grind-by-weight only uses a
// movable Bluetooth scale. Otherwise the caller uses timed grind.
inline VolumetricMeasurementSource resolveGrindSource(const ScaleAvailability &a) {
    if (a.grindBluetoothConnected)
        return VolumetricMeasurementSource::BLUETOOTH;
    return VolumetricMeasurementSource::INACTIVE;
}

inline bool brewVolumetricAvailable(int setting, const ScaleAvailability &a) {
    return resolveBrewSource(setting, a) != VolumetricMeasurementSource::INACTIVE;
}

inline bool grindVolumetricAvailable(const ScaleAvailability &a) {
    return resolveGrindSource(a) != VolumetricMeasurementSource::INACTIVE;
}

} // namespace ScaleSourceResolver

#endif // SCALE_SOURCE_RESOLVER_H
