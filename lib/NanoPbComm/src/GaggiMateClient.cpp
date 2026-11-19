#if !defined(ARDUINO_ARCH_STM32)
#include "GaggiMateClient.h"
#include <esp_log.h>

GaggiMateClient::GaggiMateClient() = default;

void GaggiMateClient::enableUart(uint8_t rxPin, uint8_t txPin, HardwareSerial &port, uint32_t baud) {
    if (_uart)
        return;
    _uartPort = &port;
    _uartRxPin = rxPin;
    _uartTxPin = txPin;
    _uartBaud = baud;
    _uart = std::make_unique<UartTransport>(port);
    _uartOta = std::make_unique<UartOtaSender>(*_uart);
}

void GaggiMateClient::init(const String &deviceName) {
    registerHandlers();
    _endpoint.onConnection([this](bool connected) {
        if (_connCb)
            _connCb(connected);
    });
    _ble.onIncompatible([this](const String &info) {
        if (_incompatibleCb)
            _incompatibleCb(info);
    });
    _link.add(LinkType::Ble, _ble);
    if (_uart) {
        _link.add(LinkType::Uart, *_uart);
        _link.setPreferred(LinkType::Uart);
    }
    _link.onSelect([this](LinkType active) { onLinkSelected(active); });
    _link.onReject([this](LinkType) { _ble.disconnect(); }); // only BLE can lose, UART preempts
    _endpoint.begin();
    _ble.init(deviceName);

    if (_uart) {
        _uartOta->begin();
        _uartPort->setRxBufferSize(gm_uart::RX_BUFFER_SIZE);
        _uartPort->setTxBufferSize(gm_uart::TX_BUFFER_SIZE);
        _uartPort->begin(_uartBaud, SERIAL_8N1, _uartRxPin, _uartTxPin);
        _uart->begin();
        ESP_LOGI("GaggiMateClient", "Controller link on UART rx=%u tx=%u @ %u baud", _uartRxPin, _uartTxPin,
                 static_cast<unsigned>(_uartBaud));
    }

    // One task owns UART RX + the send pump, so reads, writes and retransmits never depend on the main loop.
    if (xTaskCreatePinnedToCore(pumpTask, "GaggiMateClient", PUMP_TASK_STACK, this, PUMP_TASK_PRIORITY, &_pumpTaskHandle, 0) ==
        pdPASS) {
        _endpoint.setPumpTask(_pumpTaskHandle);
        if (_uart) {
            TaskHandle_t task = _pumpTaskHandle;
            _uartPort->onReceive([task]() { xTaskNotifyGive(task); });
        }
    } else {
        _pumpTaskHandle = nullptr;
        ESP_LOGE("GaggiMateClient", "Failed to create pump task; pumping from loop() instead");
    }
}

// The wire keeps listening while BLE carries the session; once it is up, the radio link goes away and stops searching.
void GaggiMateClient::onLinkSelected(LinkType active) {
    _ble.setSearchEnabled(active != LinkType::Uart);
    if (active == LinkType::Uart)
        _ble.disconnect();
}

bool GaggiMateClient::isReadyForConnection() {
    if (!_ble.isReadyForConnection()) {
        _bleReadySince = 0;
        return false;
    }
    if (!_uart)
        return true;
    if (_bleReadySince == 0)
        _bleReadySince = millis() | 1;
    return millis() - _bleReadySince >= UART_HEAD_START_MS;
}

void GaggiMateClient::disconnect() {
    if (_link.activeType() == LinkType::Uart)
        _uart->disconnect();
    else
        _ble.disconnect();
}

int GaggiMateClient::getRssi() const {
    NimBLEClient *client = _ble.getNativeClient();
    return client != nullptr && client->isConnected() ? client->getRssi() : 0;
}

OtaSender *GaggiMateClient::getOtaSender() {
    if (_link.activeType() == LinkType::Uart)
        return _uartOta.get();
    return &_bleOta;
}

void GaggiMateClient::pumpTask(void *arg) {
    auto *self = static_cast<GaggiMateClient *>(arg);
    for (;;) {
        if (self->_uart)
            self->_uart->loop();
        self->_endpoint.loop();
        // Woken at once by a send, an ACK or UART RX; the timeout paces keepalives and the retransmit check while idle.
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(PUMP_IDLE_INTERVAL_MS));
    }
}

void GaggiMateClient::loop() {
    _ble.maintain();
    if (_pumpTaskHandle == nullptr) {
        if (_uart)
            _uart->loop();
    _endpoint.loop();
    }
}

gm::Payload GaggiMateClient::buildPing() {
    gm::Payload p = gaggimate_Payload_init_zero;
    p.which_content = gaggimate_Payload_ping_tag;
    return p;
}

gm::Payload GaggiMateClient::buildBoilerControl(uint8_t index, BoilerControlMode mode, float setpoint) {
    gm::Payload p = gaggimate_Payload_init_zero;
    p.which_content = gaggimate_Payload_boiler_tag;
    p.content.boiler.index = index;
    p.content.boiler.mode = static_cast<gm::BoilerMode>(mode);
    p.content.boiler.setpoint = setpoint;
    return p;
}

gm::Payload GaggiMateClient::buildPumpControl(uint8_t index, PumpControlMode mode, float power, float pressure, float flow) {
    gm::Payload p = gaggimate_Payload_init_zero;
    p.which_content = gaggimate_Payload_pump_tag;
    p.content.pump.index = index;
    p.content.pump.mode = static_cast<gm::PumpMode>(mode);
    p.content.pump.power = power;
    p.content.pump.pressure = pressure;
    p.content.pump.flow = flow;
    return p;
}

gm::Payload GaggiMateClient::buildRelayControl(uint8_t index, bool open) {
    gm::Payload p = gaggimate_Payload_init_zero;
    p.which_content = gaggimate_Payload_relay_tag;
    p.content.relay.index = index;
    p.content.relay.open = open;
    return p;
}

gm::Payload GaggiMateClient::buildHeaterSettings(float kp, float ki, float kd, float kf, bool heaterCoordinationEnabled,
                                                 uint32_t handoverMs, float steamKp, float steamKi, float steamKd) {
    gm::Payload p = gaggimate_Payload_init_zero;
    p.which_content = gaggimate_Payload_heater_tag;
    p.content.heater.kp = kp;
    p.content.heater.ki = ki;
    p.content.heater.kd = kd;
    p.content.heater.kf = kf;
    p.content.heater.heater_coordination_enabled = heaterCoordinationEnabled;
    p.content.heater.handover_ms = handoverMs;
    p.content.heater.steam_kp = steamKp;
    p.content.heater.steam_ki = steamKi;
    p.content.heater.steam_kd = steamKd;
    return p;
}

gm::Payload GaggiMateClient::buildPumpSettings(float a, float b, float c, float d, float commutationGain, float convergenceGain,
                                               float integralGain, float maxPower, float slipA, float slipB, float slipC,
                                               float slipD) {
    gm::Payload p = gaggimate_Payload_init_zero;
    p.which_content = gaggimate_Payload_pump_model_tag;
    p.content.pump_model.a = a;
    p.content.pump_model.b = b;
    p.content.pump_model.c = c;
    p.content.pump_model.d = d;
    p.content.pump_model.commutationGain = commutationGain;
    p.content.pump_model.convergenceGain = convergenceGain;
    p.content.pump_model.integralGain = integralGain;
    p.content.pump_model.maxBLDCPower = maxPower;
    p.content.pump_model.slipA = slipA;
    p.content.pump_model.slipB = slipB;
    p.content.pump_model.slipC = slipC;
    p.content.pump_model.slipD = slipD;
    return p;
}

gm::Payload GaggiMateClient::buildAutotune(uint32_t testTime, uint32_t samples, uint32_t heaterWattage) {
    gm::Payload p = gaggimate_Payload_init_zero;
    p.which_content = gaggimate_Payload_autotune_tag;
    p.content.autotune.test_time = testTime;
    p.content.autotune.samples = samples;
    p.content.autotune.heater_wattage = heaterWattage;
    return p;
}

void GaggiMateClient::sendMachineHardware(const gm::MachineHardwareSettings &settings) {
    gm::Payload p = gaggimate_Payload_init_zero;
    p.which_content = gaggimate_Payload_machine_hardware_tag;
    p.content.machine_hardware = settings;
    _endpoint.send(p);
}

gm::Payload GaggiMateClient::buildPressureScale(float scale) {
    gm::Payload p = gaggimate_Payload_init_zero;
    p.which_content = gaggimate_Payload_pressure_scale_tag;
    p.content.pressure_scale.scale = scale;
    return p;
}

gm::Payload GaggiMateClient::buildTare() {
    gm::Payload p = gaggimate_Payload_init_zero;
    p.which_content = gaggimate_Payload_tare_tag;
    return p;
}

gm::Payload GaggiMateClient::buildHardwareScaleTare() {
    gm::Payload p = gaggimate_Payload_init_zero;
    p.which_content = gaggimate_Payload_hardware_scale_tare_tag;
    return p;
}

gm::Payload GaggiMateClient::buildScaleTare(uint32_t requestId) {
    gm::Payload p = gaggimate_Payload_init_zero;
    p.which_content = gaggimate_Payload_scale_tare_tag;
    p.content.scale_tare.request_id = requestId;
    return p;
}

gm::Payload GaggiMateClient::buildScaleCalibration(float calibration1, float calibration2, long offset1, long offset2) {
    gm::Payload p = gaggimate_Payload_init_zero;
    p.which_content = gaggimate_Payload_scale_calibration_tag;
    p.content.scale_calibration.calibration1 = calibration1;
    p.content.scale_calibration.calibration2 = calibration2;
    p.content.scale_calibration.offset1 = static_cast<int32_t>(offset1);
    p.content.scale_calibration.offset2 = static_cast<int32_t>(offset2);
    return p;
}

gm::Payload GaggiMateClient::buildScaleCalibrationStart(uint8_t channel, float referenceWeight) {
    gm::Payload p = gaggimate_Payload_init_zero;
    p.which_content = gaggimate_Payload_scale_calibration_start_tag;
    p.content.scale_calibration_start.channel = channel;
    p.content.scale_calibration_start.reference_weight = referenceWeight;
    return p;
}

gm::Payload GaggiMateClient::buildLedControl(const LedChannelCommand *channels, size_t count) {
    gm::Payload p = gaggimate_Payload_init_zero;
    p.which_content = gaggimate_Payload_led_tag;
    const size_t maxCount = sizeof(p.content.led.channels) / sizeof(p.content.led.channels[0]);
    if (channels == nullptr)
        count = 0;
    if (count > maxCount)
        count = maxCount;
    p.content.led.channels_count = static_cast<pb_size_t>(count);
    for (size_t i = 0; i < count; i++) {
        p.content.led.channels[i].channel = channels[i].channel;
        p.content.led.channels[i].brightness = channels[i].brightness;
        p.content.led.channels[i].effect = static_cast<uint32_t>(channels[i].effect);
        p.content.led.channels[i].min_brightness = channels[i].minBrightness;
        p.content.led.channels[i].cycle_time_ms = channels[i].cycleTimeMs;
    }
    return p;
}

void GaggiMateClient::sendPing() { _endpoint.send(buildPing()); }

void GaggiMateClient::sendBoilerControl(uint8_t index, BoilerControlMode mode, float setpoint) {
    _endpoint.send(buildBoilerControl(index, mode, setpoint));
}

void GaggiMateClient::sendPumpControl(uint8_t index, PumpControlMode mode, float power, float pressure, float flow) {
    _endpoint.send(buildPumpControl(index, mode, power, pressure, flow));
}

void GaggiMateClient::sendRelayControl(uint8_t index, bool open) { _endpoint.send(buildRelayControl(index, open)); }

void GaggiMateClient::sendHeaterSettings(float kp, float ki, float kd, float kf, bool heaterCoordinationEnabled,
                                         uint32_t handoverMs, float steamKp, float steamKi, float steamKd) {
    _endpoint.send(buildHeaterSettings(kp, ki, kd, kf, heaterCoordinationEnabled, handoverMs, steamKp, steamKi, steamKd));
}

void GaggiMateClient::sendPumpSettings(float a, float b, float c, float d, float commutationGain, float convergenceGain,
                                       float integralGain, float maxPower, float slipA, float slipB, float slipC, float slipD) {
    _endpoint.send(
        buildPumpSettings(a, b, c, d, commutationGain, convergenceGain, integralGain, maxPower, slipA, slipB, slipC, slipD));
}

void GaggiMateClient::sendAutotune(uint32_t testTime, uint32_t samples, uint32_t heaterWattage) {
    _endpoint.send(buildAutotune(testTime, samples, heaterWattage));
}

void GaggiMateClient::sendPressureScale(float scale) { _endpoint.send(buildPressureScale(scale)); }

void GaggiMateClient::tare() { _endpoint.send(buildTare()); }
void GaggiMateClient::tareHardwareScale() { _endpoint.send(buildHardwareScaleTare()); }

void GaggiMateClient::scaleTare(uint32_t requestId) { _endpoint.send(buildScaleTare(requestId)); }

void GaggiMateClient::sendScaleCalibration(float calibration1, float calibration2, long offset1, long offset2) {
    _endpoint.send(buildScaleCalibration(calibration1, calibration2, offset1, offset2));
}

void GaggiMateClient::startScaleCalibration(uint8_t channel, float referenceWeight) {
    _endpoint.send(buildScaleCalibrationStart(channel, referenceWeight));
}

void GaggiMateClient::sendLedControl(const LedChannelCommand *channels, size_t count) {
    _endpoint.send(buildLedControl(channels, count));
}

void GaggiMateClient::registerHandlers() {
    _endpoint.on(gaggimate_Payload_system_info_tag, [this](const gm::Payload &p) {
        if (_systemInfoCb) {
            std::vector<uint32_t> addonList = {};
            if (p.content.system_info.capabilities.addons_count > 0) {
                for (int i = 0; i < p.content.system_info.capabilities.addons_count; i++) {
                    addonList.push_back(p.content.system_info.capabilities.addons[i].type);
                }
            }
            _systemInfoCb(p.content.system_info.hardware, p.content.system_info.version, p.content.system_info.protocol_version,
                          p.content.system_info.capabilities.dimming, p.content.system_info.capabilities.pressure,
                          p.content.system_info.capabilities.led_control, p.content.system_info.capabilities.tof,
                          p.content.system_info.capabilities.dual_boiler, p.content.system_info.capabilities.scale, addonList);
        }
    });
    _endpoint.on(gaggimate_Payload_machine_hardware_status_tag, [this](const gm::Payload &p) {
        if (_machineHardwareCb)
            _machineHardwareCb(p.content.machine_hardware_status.hash, p.content.machine_hardware_status.ready,
                               p.content.machine_hardware_status.error);
    });
    _endpoint.on(gaggimate_Payload_sensor_tag, [this](const gm::Payload &p) {
        if (_tankLevelCb)
            _tankLevelCb(p.content.sensor.has_tank_level, p.content.sensor.tank_level_ok);
        if (p.content.sensor.has_steam_boiler_low && _steamLevelCb)
            _steamLevelCb(p.content.sensor.steam_boiler_low);
        if (!_sensorCb)
            return;
        // The display tracks a single boiler today; read boiler 0 if present.
        float temperature = 0.0f;
        float temp2 = 0.0f;
        float pressure = 0.0f;
        float heater_power = 0.0f;
        if (p.content.sensor.boilers_count > 0) {
            temperature = p.content.sensor.boilers[0].temperature;
            pressure = p.content.sensor.boilers[0].pressure;
            heater_power = p.content.sensor.boilers[0].power;
        }
        if (p.content.sensor.boilers_count > 1) {
            temp2 = p.content.sensor.boilers[1].temperature;
        }
#if defined(GAGGIMATE_UART_DIAGNOSTICS)
        if (p.content.sensor.has_diagnostics) {
            const gm::ControllerDiagnostics &d = p.content.sensor.diagnostics;
            _controllerDiagnostics.errorCode = d.error_code;
            _controllerDiagnostics.thermocoupleError = d.thermocouple_error;
            _controllerDiagnostics.thermocoupleStatus = d.thermocouple_status;
            _controllerDiagnostics.thermocoupleErrorCount = d.thermocouple_error_count;
            _controllerDiagnostics.thermocoupleReadCount = d.thermocouple_read_count;
            _controllerDiagnostics.thermocoupleRawTemperature = d.thermocouple_raw_temperature;
            _controllerDiagnostics.thermocoupleTemperature = d.thermocouple_temperature;
            _controllerDiagnostics.thermocoupleTaskRunning = d.thermocouple_task_running;
            _controllerDiagnostics.heaterSetpoint = d.heater_setpoint;
            _controllerDiagnostics.heaterOutput = d.heater_output;
            _controllerDiagnostics.heaterRelay = d.heater_relay;
            _controllerDiagnostics.boilerCommandCount = d.boiler_command_count;
            _controllerDiagnostics.pumpCommandCount = d.pump_command_count;
            _controllerDiagnostics.relayCommandCount = d.relay_command_count;
            _controllerDiagnostics.pingCommandCount = d.ping_command_count;
            _controllerDiagnostics.tareCommandCount = d.tare_command_count;
            _controllerDiagnostics.lastBoilerSetpoint = d.last_boiler_setpoint;
            _controllerDiagnostics.lastPumpPower = d.last_pump_power;
            _controllerDiagnostics.lastRelayOpen = d.last_relay_open;
            _controllerDiagnostics.uartRxBytes = d.uart_rx_bytes;
            _controllerDiagnostics.uartTxBytes = d.uart_tx_bytes;
            _controllerDiagnostics.uartValidFrames = d.uart_valid_frames;
            _controllerDiagnostics.uartParsedPayloads = d.uart_parsed_payloads;
            _controllerDiagnostics.freeHeap = d.free_heap;
        }
#endif
        _sensorCb(temperature, temp2, pressure, p.content.sensor.puck_flow, p.content.sensor.pump_flow,
                  p.content.sensor.puck_resistance, p.content.sensor.pump_power, heater_power, p.content.sensor.water_pumped,
                  p.content.sensor.boilers_count > 0 ? p.content.sensor.boilers[0].target_temperature : 0,
                  p.content.sensor.boilers_count > 0 && p.content.sensor.boilers[0].heating_configured);
    });
    _endpoint.on(gaggimate_Payload_button_tag, [this](const gm::Payload &p) {
        if (_buttonCb)
            _buttonCb(static_cast<uint8_t>(p.content.button.index), p.content.button.pressed);
    });
    _endpoint.on(gaggimate_Payload_autotune_result_tag, [this](const gm::Payload &p) {
        if (_autotuneResultCb)
            _autotuneResultCb(p.content.autotune_result.kp, p.content.autotune_result.ki, p.content.autotune_result.kd,
                              p.content.autotune_result.kf);
    });
    _endpoint.on(gaggimate_Payload_volumetric_tag, [this](const gm::Payload &p) {
        if (_volumetricCb)
            _volumetricCb(p.content.volumetric.volume);
    });
    _endpoint.on(gaggimate_Payload_scale_tag, [this](const gm::Payload &p) {
        if (_scaleCb)
            _scaleCb(p.content.scale.weight, p.content.scale.cell1_weight, p.content.scale.cell2_weight,
                     p.content.scale.cell1_valid, p.content.scale.cell2_valid);
    });
    _endpoint.on(gaggimate_Payload_tof_tag, [this](const gm::Payload &p) {
        if (_tofCb)
            _tofCb(p.content.tof.distance);
    });
    _endpoint.on(gaggimate_Payload_error_tag, [this](const gm::Payload &p) {
        if (_errorCb)
            _errorCb(static_cast<int>(p.content.error.code));
    });
    _endpoint.on(gaggimate_Payload_weight_tag, [this](const gm::Payload &p) {
        if (_weightCb)
            _weightCb(p.content.weight.weight);
    });
    _endpoint.on(gaggimate_Payload_scale_sample_tag, [this](const gm::Payload &p) {
        if (!_scaleSampleCb)
            return;
        ScaleSample sample{};
        sample.weightG = p.content.scale_sample.weight_g;
        sample.stddevG = p.content.scale_sample.stddev_g;
        sample.ch1G = p.content.scale_sample.ch1_g;
        sample.ch2G = p.content.scale_sample.ch2_g;
        sample.ch1StdG = p.content.scale_sample.ch1_std_g;
        sample.ch2StdG = p.content.scale_sample.ch2_std_g;
        sample.healthBits = static_cast<uint16_t>(p.content.scale_sample.health_bits);
        sample.sampleSeq = p.content.scale_sample.sample_seq;
        _scaleSampleCb(sample);
    });
    _endpoint.on(gaggimate_Payload_scale_offsets_tag, [this](const gm::Payload &p) {
        if (_scaleOffsetsCb) {
            const auto &wire = p.content.scale_offsets;
            ScaleTareResult result{wire.request_id, wire.success, wire.offset1, wire.offset2,
                                   static_cast<uint16_t>(wire.health_bits), wire.stddev1, wire.stddev2};
            if (wire.health_bits > 0xFFu)
                result.success = false;
            _scaleOffsetsCb(result);
        }
    });
    _endpoint.on(gaggimate_Payload_scale_calibration_result_tag, [this](const gm::Payload &p) {
        if (_scaleCalibrationResultCb)
            _scaleCalibrationResultCb(static_cast<uint8_t>(p.content.scale_calibration_result.channel),
                                      p.content.scale_calibration_result.calibration);
    });
}
#endif
