#ifndef CLIENT_COMMUNICATION_HANDLER_H
#define CLIENT_COMMUNICATION_HANDLER_H

#include "CommonTypes.h"
#include <Arduino.h>
#include <functional>
#include <string>

/**
 * @brief Abstract client communication handler interface (display side)
 *
 * Mirrors CommunicationHandler on the controller side. Both NimBLEClientController
 * and UARTClientController implement this interface so Controller.cpp can reference
 * a single pointer type regardless of the active backend.
 *
 * Backend-specific methods (e.g. NimBLEClient* getClient()) stay on the concrete
 * classes and are accessed via #ifdef GAGGIMATE_UART_COMMS guards at call sites.
 */
class ClientCommunicationHandler {
  public:
    virtual ~ClientCommunicationHandler() = default;

    // -------------------------------------------------------------------------
    // Lifecycle
    // -------------------------------------------------------------------------

    virtual void initClient() = 0;
    virtual bool connectToServer() = 0;
    virtual bool isConnected() const = 0;

    /** Returns true when the backend has found a peer and is ready to connect. */
    virtual bool isReadyForConnection() const { return false; }

    /** Trigger a scan/discovery pass. UART treats this as a no-op (always connected). */
    virtual void scan() {}

    /**
     * Process incoming data. Called from the display loop.
     * BLE is interrupt/notify-driven so this is a no-op there.
     */
    virtual void poll() {}

    // -------------------------------------------------------------------------
    // Send commands (display → controller)
    // -------------------------------------------------------------------------

    virtual void sendAdvancedOutputControl(bool valve, float boilerSetpoint, bool pressureTarget,
                                           float pressure, float flow) = 0;
    virtual void sendOutputControl(bool valve, float pumpSetpoint, float boilerSetpoint) = 0;
    virtual void sendAltControl(bool pinState) = 0;
    virtual void sendPing() = 0;
    virtual void sendAutotune(int testTime, int samples) = 0;
    virtual void sendPidSettings(const String &pid) = 0;
    virtual void sendPumpModelCoeffs(const String &pumpModelCoeffs) = 0;
    virtual void setPressureScale(float scale) = 0;
    virtual void sendLedControl(uint8_t channel, uint8_t brightness) = 0;

    /** Tare the pump-flow volumetric measurement. */
    virtual void tare() = 0;

    /** Tare the hardware weight scale (HX711). */
    virtual void scaleTare() = 0;

    /** Send saved calibration factors and offsets to the controller at boot or on change. */
    virtual void sendScaleCalibration(float c1, float c2, long offset1, long offset2) = 0;

    /** Trigger per-channel live calibration with a known reference weight. */
    virtual void startScaleCalibration(uint8_t channel, float refWeight) = 0;

    // -------------------------------------------------------------------------
    // Register callbacks (controller → display notifications)
    // -------------------------------------------------------------------------

    virtual void registerRemoteErrorCallback(const remote_err_callback_t &callback) = 0;
    virtual void registerBrewBtnCallback(const brew_callback_t &callback) = 0;
    virtual void registerSteamBtnCallback(const steam_callback_t &callback) = 0;
    virtual void registerSensorCallback(const sensor_read_callback_t &callback) = 0;
    virtual void registerAutotuneResultCallback(const pid_control_callback_t &callback) = 0;
    virtual void registerVolumetricMeasurementCallback(const float_callback_t &callback) = 0;
    virtual void registerTofMeasurementCallback(const int_callback_t &callback) = 0;
    virtual void registerWeightMeasurementCallback(const float_callback_t &callback) = 0;
    virtual void registerScaleOffsetsCallback(const scale_offsets_callback_t &callback) = 0;
    virtual void registerScaleCalResultCallback(const scale_cal_result_callback_t &callback) = 0;

    // -------------------------------------------------------------------------
    // Info
    // -------------------------------------------------------------------------

    virtual std::string readInfo() const = 0;
};

#endif // CLIENT_COMMUNICATION_HANDLER_H
