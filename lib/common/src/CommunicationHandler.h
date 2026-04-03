#ifndef COMMUNICATION_HANDLER_H
#define COMMUNICATION_HANDLER_H

#include "CommonTypes.h"
#include <Arduino.h>
#include <functional>

/**
 * @brief Abstract communication handler interface
 *
 * Provides abstraction for different communication backends (BLE, UART, etc.)
 * allowing GaggiMateController to work with multiple communication protocols.
 */
class CommunicationHandler {
  public:
    virtual ~CommunicationHandler() = default;

    /**
     * @brief Initialize the communication handler with system info
     * @param infoString System information string (platform, version, capabilities)
     */
    virtual void initServer(String infoString) = 0;

    /**
     * @brief Send sensor data to the remote controller
     * @param temperature Temperature in °C
     * @param pressure Pressure in bar
     * @param puckFlow Flow at puck in ml/s
     * @param pumpFlow Pump flow in ml/s
     * @param puckResistance Resistance in bar/flow
     */
    virtual void sendSensorData(float temperature, float pressure, float puckFlow, float pumpFlow, float puckResistance) = 0;

    /**
     * @brief Send error code to remote
     * @param errorCode Error code to send
     */
    virtual void sendError(int errorCode) = 0;

    /**
     * @brief Send brew button state
     * @param brewButtonStatus Button state (true=pressed, false=released)
     */
    virtual void sendBrewBtnState(bool brewButtonStatus) = 0;

    /**
     * @brief Send steam button state
     * @param steamButtonStatus Button state (true=pressed, false=released)
     */
    virtual void sendSteamBtnState(bool steamButtonStatus) = 0;

    /**
     * @brief Send autotune results
     * @param Kp Proportional gain
     * @param Ki Integral gain
     * @param Kd Derivative gain
     */
    virtual void sendAutotuneResult(float Kp, float Ki, float Kd) = 0;

    /**
     * @brief Send volumetric measurement (coffee volume)
     * @param value Volume in ml
     */
    virtual void sendVolumetricMeasurement(float value) = 0;

    /**
     * @brief Send TOF (time-of-flight) distance measurement
     * @param value Distance in mm
     */
    virtual void sendTofMeasurement(int value) = 0;

    /**
     * @brief Send scale weight measurement
     * @param weight Weight in grams
     */
    virtual void sendWeightMeasurement(float weight) = 0;

    /**
     * @brief Register callback for simple output control
     * @param callback Function called when valve/pump/heater control received
     */
    virtual void registerOutputControlCallback(const simple_output_callback_t &callback) = 0;

    /**
     * @brief Register callback for advanced output control (pressure/flow targets)
     * @param callback Function called for advanced control commands
     */
    virtual void registerAdvancedOutputControlCallback(const advanced_output_callback_t &callback) = 0;

    /**
     * @brief Register callback for auxiliary output control
     * @param callback Function called for auxiliary relay control
     */
    virtual void registerAltControlCallback(const pin_control_callback_t &callback) = 0;

    /**
     * @brief Register callback for PID control tuning
     * @param callback Function called when PID parameters received
     */
    virtual void registerPidControlCallback(const pid_control_callback_t &callback) = 0;

    /**
     * @brief Register callback for pump model coefficients
     * @param callback Function called when pump model parameters received
     */
    virtual void registerPumpModelCoeffsCallback(const pump_model_coeffs_callback_t &callback) = 0;

    /**
     * @brief Register callback for ping messages
     * @param callback Function called when ping received
     */
    virtual void registerPingCallback(const ping_callback_t &callback) = 0;

    /**
     * @brief Register callback for autotune command
     * @param callback Function called when autotune requested
     */
    virtual void registerAutotuneCallback(const autotune_callback_t &callback) = 0;

    /**
     * @brief Register callback for pressure scale calibration
     * @param callback Function called with pressure scale value
     */
    virtual void registerPressureScaleCallback(const float_callback_t &callback) = 0;

    /**
     * @brief Register callback for tare (zero) pump flow measurement
     * @param callback Function called when tare requested
     */
    virtual void registerTareCallback(const void_callback_t &callback) = 0;

    /**
     * @brief Register callback for tare (zero) weight scale
     * @param callback Function called when scale tare requested
     */
    virtual void registerScaleTareCallback(const void_callback_t &callback) = 0;

    /**
     * @brief Register callback for scale calibration coefficients + offsets
     * @param callback Function called with (c1, c2, offset1, offset2)
     */
    virtual void registerScaleCalibrationCallback(const scale_calibration_callback_t &callback) = 0;

    /**
     * @brief Register callback for per-channel calibration start command
     * @param callback Function called with (channel, refWeight)
     */
    virtual void registerScaleCalStartCallback(const scale_cal_start_callback_t &callback) {}

    /**
     * @brief Send scale tare offsets to display (controller→display, after tare)
     * @param offset1 Raw ADC offset for channel 1
     * @param offset2 Raw ADC offset for channel 2
     */
    virtual void sendScaleOffsets(long offset1, long offset2) {}

    /**
     * @brief Send per-channel calibration result to display
     * @param channel Which channel was calibrated (1 or 2)
     * @param calibration New scale factor for that channel
     */
    virtual void sendScaleCalResult(uint8_t channel, float calibration) {}

    /**
     * @brief Register callback for LED control
     * @param callback Function called for LED channel/brightness control
     */
    virtual void registerLedControlCallback(const led_control_callback_t &callback) = 0;

    /**
     * @brief Dispatch queued commands to callbacks (no-op for non-queued backends)
     */
    virtual void processQueue() {}
};

#endif // COMMUNICATION_HANDLER_H
