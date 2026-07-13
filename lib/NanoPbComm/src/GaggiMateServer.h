#ifndef GAGGIMATE_SERVER_H
#define GAGGIMATE_SERVER_H

#include "Endpoint.h"
#include "GaggiMateComm.h"
#if defined(GAGGIMATE_UART_COMMS) || defined(ARDUINO_ARCH_STM32)
#include "uart/UartTransport.h"
#else
#include "ble/BleServerTransport.h"
#endif
#include <Arduino.h>
#include <functional>

/**
 * Controller-side protocol facade.
 *
 * Owns a BLE server transport + Endpoint and exposes semantic send methods and
 * typed command callbacks. Pushes SystemInfo to the display on connect.
 */
class GaggiMateServer {
  public:
    using PingCallback = std::function<void()>;
    using BoilerCallback = std::function<void(uint8_t index, BoilerControlMode mode, float setpoint)>;
    using PumpCallback = std::function<void(uint8_t index, PumpControlMode mode, float power, float pressure, float flow)>;
    // Binary output: index 0 = brew valve, index 1 = alt relay.
    using RelayCallback = std::function<void(uint8_t index, bool open)>;
    using PidCallback = std::function<void(float kp, float ki, float kd, float kf)>;
    using PumpSettingsCallback = std::function<void(gm::PumpSettings settings)>;
    using AutotuneCallback = std::function<void(uint32_t testTime, uint32_t samples, uint32_t heaterWattage)>;
    using PressureScaleCallback = std::function<void(float scale)>;
    using TareCallback = std::function<void()>;
    using LedCallback = std::function<void(uint8_t channel, uint8_t brightness)>;
    using ScaleTareCallback = std::function<void()>;
    using ScaleCalibrationCallback = std::function<void(float calibration1, float calibration2, long offset1, long offset2)>;
    using ScaleCalibrationStartCallback = std::function<void(uint8_t channel, float referenceWeight)>;

    GaggiMateServer();

    void init(const String &deviceName, const String &hardware, const String &version,
              const gm::DeviceCapabilities &capabilities);
    void loop();
    bool isConnected() const { return _endpoint.isConnected(); }
    bool isUpdating() const { return _transport.isUpdating(); }

    void setSystemInfo(const String &hardware, const String &version, const gm::DeviceCapabilities &capabilities);
    void setControllerDiagnostics(const ControllerDiagnostics &diagnostics) {
#if defined(GAGGIMATE_UART_DIAGNOSTICS)
        _controllerDiagnostics = diagnostics;
#else
        (void)diagnostics;
#endif
    }
    UartDiagnostics getDiagnostics() const {
#if defined(GAGGIMATE_UART_COMMS) || defined(ARDUINO_ARCH_STM32)
        return _transport.getDiagnostics();
#else
        return {};
#endif
    }

    // Build a payload without sending (compose your own batch, then send()).
    // sendSensorData reports boiler 0; the wire format supports several boilers.
    gm::Payload buildSensorData(float temperature, float pressure, float puckFlow, float pumpFlow, float puckResistance,
                                float pumpPower = 0.0f, float heaterPower = 0.0f);
    gm::Payload buildButtonState(uint8_t index, bool pressed);
    gm::Payload buildAutotuneResult(float kp, float ki, float kd, float kf);
    gm::Payload buildVolumetricMeasurement(float volume);
    gm::Payload buildTofMeasurement(uint32_t distance);
    gm::Payload buildError(int code);
    gm::Payload buildWeightMeasurement(float weight);
    gm::Payload buildScaleSample(const ScaleSample &sample);
    gm::Payload buildScaleOffsets(long offset1, long offset2);
    gm::Payload buildScaleCalibrationResult(uint8_t channel, float calibration);

    // Responses (controller -> display)
    void sendSensorData(float temperature, float pressure, float puckFlow, float pumpFlow, float puckResistance,
                        float pumpPower = 0.0f, float heaterPower = 0.0f);
    void sendButtonState(uint8_t index, bool pressed);
    void sendAutotuneResult(float kp, float ki, float kd, float kf);
    void sendVolumetricMeasurement(float volume);
    void sendTofMeasurement(uint32_t distance);
    void sendError(int code);
    void sendWeightMeasurement(float weight);
    void sendScaleSample(const ScaleSample &sample);
    void sendScaleOffsets(long offset1, long offset2);
    void sendScaleCalibrationResult(uint8_t channel, float calibration);

    // Drop the current BLE link. The ping watchdog calls this so the display
    // sees a real disconnect instead of having to interpret an in-band error.
    void disconnect() { _transport.disconnect(); }

    // Send a pre-built payload / batch of payloads (one frame).
    void send(const gm::Payload &payload) { _endpoint.send(payload); }
    void sendBatch(const gm::Payload *payloads, size_t count) { _endpoint.sendBatch(payloads, count); }

    // Fire-and-forget variants (unacknowledged) for high-rate telemetry.
    void sendUnreliable(const gm::Payload &payload) { _endpoint.sendUnreliable(payload); }
    void sendUnreliableBatch(const gm::Payload *payloads, size_t count) { _endpoint.sendUnreliable(payloads, count); }

    // Command registrations (display -> controller)
    void onPing(PingCallback cb) { _pingCb = std::move(cb); }
    void onBoilerControl(BoilerCallback cb) { _boilerCb = std::move(cb); }
    void onPumpControl(PumpCallback cb) { _pumpCb = std::move(cb); }
    void onRelayControl(RelayCallback cb) { _relayCb = std::move(cb); }
    void onPidSettings(PidCallback cb) { _pidCb = std::move(cb); }
    void onPumpSettings(PumpSettingsCallback cb) { _pumpSettingsCb = std::move(cb); }
    void onAutotune(AutotuneCallback cb) { _autotuneCb = std::move(cb); }
    void onPressureScale(PressureScaleCallback cb) { _pressureScaleCb = std::move(cb); }
    void onTare(TareCallback cb) { _tareCb = std::move(cb); }
    void onLedControl(LedCallback cb) { _ledCb = std::move(cb); }
    void onScaleTare(ScaleTareCallback cb) { _scaleTareCb = std::move(cb); }
    void onScaleCalibration(ScaleCalibrationCallback cb) { _scaleCalibrationCb = std::move(cb); }
    void onScaleCalibrationStart(ScaleCalibrationStartCallback cb) { _scaleCalibrationStartCb = std::move(cb); }

  private:
#if defined(GAGGIMATE_UART_COMMS) || defined(ARDUINO_ARCH_STM32)
    UartTransport _transport;
#else
    BleServerTransport _transport;
#endif
    Endpoint _endpoint;
    gm::SystemInfo _systemInfo = gaggimate_SystemInfo_init_zero;
#if defined(GAGGIMATE_UART_DIAGNOSTICS)
    ControllerDiagnostics _controllerDiagnostics{};
#endif
    bool _systemInfoAcknowledged = false;
    unsigned long _lastSystemInfoPushMs = 0;
    static constexpr unsigned long SYSTEM_INFO_RETRY_MS = 1000;

    PingCallback _pingCb;
    BoilerCallback _boilerCb;
    PumpCallback _pumpCb;
    RelayCallback _relayCb;
    PidCallback _pidCb;
    PumpSettingsCallback _pumpSettingsCb;
    AutotuneCallback _autotuneCb;
    PressureScaleCallback _pressureScaleCb;
    TareCallback _tareCb;
    LedCallback _ledCb;
    ScaleTareCallback _scaleTareCb;
    ScaleCalibrationCallback _scaleCalibrationCb;
    ScaleCalibrationStartCallback _scaleCalibrationStartCb;

    void registerHandlers();
    void pushSystemInfo();
    void acknowledgeSystemInfo();

    // Drives the endpoint send pump / retransmit independently of the
    // controller's (slow, 250ms) main loop, on the NimBLE core.
    TaskHandle_t _taskHandle = nullptr;
    static void pumpTask(void *arg);
};

#endif // GAGGIMATE_SERVER_H
