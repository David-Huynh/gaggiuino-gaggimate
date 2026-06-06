#include "GaggiMateServer.h"
#include <cstdio>
#include <cstring>
#if defined(ARDUINO_ARCH_STM32)
#define ESP_LOGE(tag, fmt, ...) ((void)0)
#else
#include <esp_log.h>
#endif

namespace {
#if defined(ARDUINO_ARCH_STM32)
constexpr uint32_t SERVER_PUMP_STACK = 768; // STM32 FreeRTOS stack depth is in words, not bytes
#else
constexpr uint32_t SERVER_PUMP_STACK = 4096;
#endif
} // namespace

GaggiMateServer::GaggiMateServer()
#if defined(GAGGIMATE_UART_COMMS) || defined(ARDUINO_ARCH_STM32)
    : _transport(Serial2), _endpoint(_transport) {
}
#else
    : _endpoint(_transport) {
}
#endif

void GaggiMateServer::init(const String &deviceName, const String &hardware, const String &version, bool dimming, bool pressure,
                           bool ledControl, bool tof, bool scale) {
    setSystemInfo(hardware, version, dimming, pressure, ledControl, tof, scale);
    registerHandlers();
    _endpoint.onConnection([this](bool connected) {
        if (connected) {
            _systemInfoAcknowledged = false;
            pushSystemInfo();
        } else {
            _systemInfoAcknowledged = false;
        }
    });
    _endpoint.begin();
#if defined(GAGGIMATE_UART_COMMS) || defined(ARDUINO_ARCH_STM32)
    (void)deviceName;
    _transport.begin();
#else
    _transport.init(deviceName);
#endif

#if defined(ARDUINO_ARCH_STM32)
    if (xTaskCreate(pumpTask, "GaggiMateServer", SERVER_PUMP_STACK, this, 1, &_taskHandle) != pdPASS) {
#else
    if (xTaskCreatePinnedToCore(pumpTask, "GaggiMateServer", SERVER_PUMP_STACK, this, 1, &_taskHandle, 0) != pdPASS) {
#endif
        _taskHandle = nullptr;
        ESP_LOGE("GaggiMateServer", "Failed to create pump task; ACK/retransmit will not run");
    }
}

void GaggiMateServer::pumpTask(void *arg) {
    auto *self = static_cast<GaggiMateServer *>(arg);
    TickType_t lastWake = xTaskGetTickCount();
    for (;;) {
        self->loop();
#if defined(ARDUINO_ARCH_STM32)
        vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(15));
#else
        xTaskDelayUntil(&lastWake, pdMS_TO_TICKS(15));
#endif
    }
}

void GaggiMateServer::loop() {
#if defined(GAGGIMATE_UART_COMMS) || defined(ARDUINO_ARCH_STM32)
    _transport.loop();
    if (_endpoint.isConnected() && !_systemInfoAcknowledged && millis() - _lastSystemInfoPushMs >= SYSTEM_INFO_RETRY_MS) {
        pushSystemInfo();
    }
#endif
    _endpoint.loop();
}

void GaggiMateServer::setSystemInfo(const String &hardware, const String &version, bool dimming, bool pressure, bool ledControl,
                                    bool tof, bool scale) {
    memset(&_systemInfo, 0, sizeof(_systemInfo));
    strlcpy(_systemInfo.hardware, hardware.c_str(), sizeof(_systemInfo.hardware));
    strlcpy(_systemInfo.version, version.c_str(), sizeof(_systemInfo.version));
    _systemInfo.protocol_version = gm_proto::PROTOCOL_VERSION;
    _systemInfo.has_capabilities = true;
    _systemInfo.capabilities.dimming = dimming;
    _systemInfo.capabilities.pressure = pressure;
    _systemInfo.capabilities.led_control = ledControl;
    _systemInfo.capabilities.tof = tof;
    _systemInfo.capabilities.scale = scale;

    // Mirror system info onto the legacy read-only characteristic in the old
    // JSON shape (plus "pv"), so pre-framing tools can still read it.
    char json[224];
    snprintf(json, sizeof(json),
             "{\"hw\":\"%s\",\"v\":\"%s\",\"pv\":%u,\"cp\":{\"ps\":%s,\"dm\":%s,\"led\":%s,\"tof\":%s,\"sc\":%s}}",
             hardware.c_str(), version.c_str(), static_cast<unsigned>(gm_proto::PROTOCOL_VERSION), pressure ? "true" : "false",
             dimming ? "true" : "false", ledControl ? "true" : "false", tof ? "true" : "false", scale ? "true" : "false");
#if !defined(GAGGIMATE_UART_COMMS) && !defined(ARDUINO_ARCH_STM32)
    _transport.setInfo(json);
#else
    (void)json;
#endif
}

void GaggiMateServer::pushSystemInfo() {
    _lastSystemInfoPushMs = millis();
    gm::Payload p = gaggimate_Payload_init_zero;
    p.which_content = gaggimate_Payload_system_info_tag;
    p.content.system_info = _systemInfo;
    _endpoint.send(p);
}

void GaggiMateServer::acknowledgeSystemInfo() { _systemInfoAcknowledged = true; }

gm::Payload GaggiMateServer::buildSensorData(float temperature, float pressure, float puckFlow, float pumpFlow,
                                             float puckResistance) {
    gm::Payload p = gaggimate_Payload_init_zero;
    p.which_content = gaggimate_Payload_sensor_tag;
    p.content.sensor.boilers_count = 1; // boiler 0; schema allows more
    p.content.sensor.boilers[0].index = 0;
    p.content.sensor.boilers[0].temperature = temperature;
    p.content.sensor.boilers[0].pressure = pressure;
    p.content.sensor.puck_flow = puckFlow;
    p.content.sensor.pump_flow = pumpFlow;
    p.content.sensor.puck_resistance = puckResistance;
#if defined(GAGGIMATE_UART_DIAGNOSTICS)
    p.content.sensor.has_diagnostics = true;
    p.content.sensor.diagnostics.error_code = _controllerDiagnostics.errorCode;
    p.content.sensor.diagnostics.thermocouple_error = _controllerDiagnostics.thermocoupleError;
    p.content.sensor.diagnostics.thermocouple_status = _controllerDiagnostics.thermocoupleStatus;
    p.content.sensor.diagnostics.thermocouple_error_count = _controllerDiagnostics.thermocoupleErrorCount;
    p.content.sensor.diagnostics.thermocouple_read_count = _controllerDiagnostics.thermocoupleReadCount;
    p.content.sensor.diagnostics.thermocouple_raw_temperature = _controllerDiagnostics.thermocoupleRawTemperature;
    p.content.sensor.diagnostics.thermocouple_temperature = _controllerDiagnostics.thermocoupleTemperature;
    p.content.sensor.diagnostics.thermocouple_task_running = _controllerDiagnostics.thermocoupleTaskRunning;
    p.content.sensor.diagnostics.heater_setpoint = _controllerDiagnostics.heaterSetpoint;
    p.content.sensor.diagnostics.heater_output = _controllerDiagnostics.heaterOutput;
    p.content.sensor.diagnostics.heater_relay = _controllerDiagnostics.heaterRelay;
    p.content.sensor.diagnostics.boiler_command_count = _controllerDiagnostics.boilerCommandCount;
    p.content.sensor.diagnostics.pump_command_count = _controllerDiagnostics.pumpCommandCount;
    p.content.sensor.diagnostics.relay_command_count = _controllerDiagnostics.relayCommandCount;
    p.content.sensor.diagnostics.ping_command_count = _controllerDiagnostics.pingCommandCount;
    p.content.sensor.diagnostics.tare_command_count = _controllerDiagnostics.tareCommandCount;
    p.content.sensor.diagnostics.last_boiler_setpoint = _controllerDiagnostics.lastBoilerSetpoint;
    p.content.sensor.diagnostics.last_pump_power = _controllerDiagnostics.lastPumpPower;
    p.content.sensor.diagnostics.last_relay_open = _controllerDiagnostics.lastRelayOpen;
    p.content.sensor.diagnostics.uart_rx_bytes = _controllerDiagnostics.uartRxBytes;
    p.content.sensor.diagnostics.uart_tx_bytes = _controllerDiagnostics.uartTxBytes;
    p.content.sensor.diagnostics.uart_valid_frames = _controllerDiagnostics.uartValidFrames;
    p.content.sensor.diagnostics.uart_parsed_payloads = _controllerDiagnostics.uartParsedPayloads;
    p.content.sensor.diagnostics.free_heap = _controllerDiagnostics.freeHeap;
#endif
    return p;
}

gm::Payload GaggiMateServer::buildButtonState(uint8_t index, bool pressed) {
    gm::Payload p = gaggimate_Payload_init_zero;
    p.which_content = gaggimate_Payload_button_tag;
    p.content.button.index = index;
    p.content.button.pressed = pressed;
    return p;
}

gm::Payload GaggiMateServer::buildAutotuneResult(float kp, float ki, float kd, float kf) {
    gm::Payload p = gaggimate_Payload_init_zero;
    p.which_content = gaggimate_Payload_autotune_result_tag;
    p.content.autotune_result.kp = kp;
    p.content.autotune_result.ki = ki;
    p.content.autotune_result.kd = kd;
    p.content.autotune_result.kf = kf;
    return p;
}

gm::Payload GaggiMateServer::buildVolumetricMeasurement(float volume) {
    gm::Payload p = gaggimate_Payload_init_zero;
    p.which_content = gaggimate_Payload_volumetric_tag;
    p.content.volumetric.volume = volume;
    return p;
}

gm::Payload GaggiMateServer::buildTofMeasurement(uint32_t distance) {
    gm::Payload p = gaggimate_Payload_init_zero;
    p.which_content = gaggimate_Payload_tof_tag;
    p.content.tof.distance = distance;
    return p;
}

gm::Payload GaggiMateServer::buildError(int code) {
    gm::Payload p = gaggimate_Payload_init_zero;
    p.which_content = gaggimate_Payload_error_tag;
    p.content.error.code = static_cast<gm::ErrorCode>(code);
    return p;
}

gm::Payload GaggiMateServer::buildWeightMeasurement(float weight) {
    gm::Payload p = gaggimate_Payload_init_zero;
    p.which_content = gaggimate_Payload_weight_tag;
    p.content.weight.weight = weight;
    return p;
}

gm::Payload GaggiMateServer::buildScaleOffsets(long offset1, long offset2) {
    gm::Payload p = gaggimate_Payload_init_zero;
    p.which_content = gaggimate_Payload_scale_offsets_tag;
    p.content.scale_offsets.offset1 = static_cast<int32_t>(offset1);
    p.content.scale_offsets.offset2 = static_cast<int32_t>(offset2);
    return p;
}

gm::Payload GaggiMateServer::buildScaleCalibrationResult(uint8_t channel, float calibration) {
    gm::Payload p = gaggimate_Payload_init_zero;
    p.which_content = gaggimate_Payload_scale_calibration_result_tag;
    p.content.scale_calibration_result.channel = channel;
    p.content.scale_calibration_result.calibration = calibration;
    return p;
}

// Telemetry (sensor / volumetric / ToF) is sent fire-and-forget: it is
// high-rate and self-refreshing, so a dropped sample is replaced by the next
// one. This avoids the constant ACK chatter on the high-rate path. Button /
// autotune-result / error / system-info stay reliable.
void GaggiMateServer::sendSensorData(float temperature, float pressure, float puckFlow, float pumpFlow, float puckResistance) {
    _endpoint.sendUnreliable(buildSensorData(temperature, pressure, puckFlow, pumpFlow, puckResistance));
}

void GaggiMateServer::sendButtonState(uint8_t index, bool pressed) { _endpoint.send(buildButtonState(index, pressed)); }

void GaggiMateServer::sendAutotuneResult(float kp, float ki, float kd, float kf) {
    _endpoint.send(buildAutotuneResult(kp, ki, kd, kf));
}

void GaggiMateServer::sendVolumetricMeasurement(float volume) { _endpoint.sendUnreliable(buildVolumetricMeasurement(volume)); }

void GaggiMateServer::sendTofMeasurement(uint32_t distance) { _endpoint.sendUnreliable(buildTofMeasurement(distance)); }

void GaggiMateServer::sendError(int code) { _endpoint.send(buildError(code)); }

void GaggiMateServer::sendWeightMeasurement(float weight) { _endpoint.sendUnreliable(buildWeightMeasurement(weight)); }

void GaggiMateServer::sendScaleOffsets(long offset1, long offset2) { _endpoint.send(buildScaleOffsets(offset1, offset2)); }

void GaggiMateServer::sendScaleCalibrationResult(uint8_t channel, float calibration) {
    _endpoint.send(buildScaleCalibrationResult(channel, calibration));
}

void GaggiMateServer::registerHandlers() {
    _endpoint.on(gaggimate_Payload_ping_tag, [this](const gm::Payload &) {
        if (_pingCb)
            _pingCb();
    });
    _endpoint.on(gaggimate_Payload_boiler_tag, [this](const gm::Payload &p) {
        if (_boilerCb)
            _boilerCb(static_cast<uint8_t>(p.content.boiler.index), static_cast<BoilerControlMode>(p.content.boiler.mode),
                      p.content.boiler.setpoint);
    });
    _endpoint.on(gaggimate_Payload_pump_tag, [this](const gm::Payload &p) {
        if (_pumpCb)
            _pumpCb(static_cast<uint8_t>(p.content.pump.index), static_cast<PumpControlMode>(p.content.pump.mode),
                    p.content.pump.power, p.content.pump.pressure, p.content.pump.flow);
    });
    _endpoint.on(gaggimate_Payload_relay_tag, [this](const gm::Payload &p) {
        if (_relayCb)
            _relayCb(static_cast<uint8_t>(p.content.relay.index), p.content.relay.open);
    });
    _endpoint.on(gaggimate_Payload_pid_tag, [this](const gm::Payload &p) {
        acknowledgeSystemInfo();
        if (_pidCb)
            _pidCb(p.content.pid.kp, p.content.pid.ki, p.content.pid.kd, p.content.pid.kf);
    });
    _endpoint.on(gaggimate_Payload_pump_model_tag, [this](const gm::Payload &p) {
        acknowledgeSystemInfo();
        if (_pumpModelCb)
            _pumpModelCb(p.content.pump_model.a, p.content.pump_model.b, p.content.pump_model.c, p.content.pump_model.d);
    });
    _endpoint.on(gaggimate_Payload_autotune_tag, [this](const gm::Payload &p) {
        if (_autotuneCb)
            _autotuneCb(p.content.autotune.test_time, p.content.autotune.samples, p.content.autotune.heater_wattage);
    });
    _endpoint.on(gaggimate_Payload_pressure_scale_tag, [this](const gm::Payload &p) {
        acknowledgeSystemInfo();
        if (_pressureScaleCb)
            _pressureScaleCb(p.content.pressure_scale.scale);
    });
    _endpoint.on(gaggimate_Payload_tare_tag, [this](const gm::Payload &) {
        if (_tareCb)
            _tareCb();
    });
    _endpoint.on(gaggimate_Payload_led_tag, [this](const gm::Payload &p) {
        if (!_ledCb)
            return;
        // One message carries every changed channel; apply them in order.
        for (pb_size_t i = 0; i < p.content.led.channels_count; i++)
            _ledCb(static_cast<uint8_t>(p.content.led.channels[i].channel),
                   static_cast<uint8_t>(p.content.led.channels[i].brightness));
    });
    _endpoint.on(gaggimate_Payload_scale_tare_tag, [this](const gm::Payload &) {
        if (_scaleTareCb)
            _scaleTareCb();
    });
    _endpoint.on(gaggimate_Payload_scale_calibration_tag, [this](const gm::Payload &p) {
        if (_scaleCalibrationCb)
            _scaleCalibrationCb(p.content.scale_calibration.calibration1, p.content.scale_calibration.calibration2,
                                p.content.scale_calibration.offset1, p.content.scale_calibration.offset2);
    });
    _endpoint.on(gaggimate_Payload_scale_calibration_start_tag, [this](const gm::Payload &p) {
        if (_scaleCalibrationStartCb)
            _scaleCalibrationStartCb(static_cast<uint8_t>(p.content.scale_calibration_start.channel),
                                     p.content.scale_calibration_start.reference_weight);
    });
}
