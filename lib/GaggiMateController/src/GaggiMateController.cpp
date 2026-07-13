#include "GaggiMateController.h"
#ifdef ARDUINO_ARCH_STM32
#include <IWatchdog.h>
#include <STM32FreeRTOS.h>
#else
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <ExtensionIOXL9555.hpp>
#endif
#include "logging.h"
#include "utilities.h"
#include <Arduino.h>
#include <peripherals/DimmedPump.h>
#include <peripherals/SimplePump.h>

#include <utility>

#ifdef ARDUINO_ARCH_STM32
extern "C" void ControllerLoopTask(void *pvParameters) {
    auto *controller = static_cast<GaggiMateController *>(pvParameters);
    for (;;) {
        controller->loop();
    }
}
#endif

GaggiMateController::GaggiMateController(String version) : _version(std::move(version)) {
    configs.push_back(GM_STANDARD_REV_1X);
    configs.push_back(GM_STANDARD_REV_2X);
    configs.push_back(GM_STANDARD_REV_3X);
    configs.push_back(GM_PRO_REV_1x);
    configs.push_back(GM_PRO_LEGO);
    configs.push_back(GM_PRO_REV_11);
    configs.push_back(GM_STM32F4_V1);
}

char albaSwTxBuffer[128];
char albaSwRxBuffer[128];

void GaggiMateController::setup() {
    detectBoard();
    detectAddon();

    this->thermocouple = new Max31855Thermocouple(
        _config.maxCsPin, _config.maxMisoPin, _config.maxSckPin, [this](float temperature) { /* noop */ },
        [this]() { thermalRunawayShutdown(); });
    this->heater = new Heater(
        this->thermocouple, _config.heaterPin, [this]() { thermalRunawayShutdown(); },
        [this](float Kp, float Ki, float Kd, float Kff) { _comms.sendAutotuneResult(Kp, Ki, Kd, Kff); },
        [this]() { _comms.sendError(ERROR_CODE_AUTOTUNE_TIMEOUT); });
    this->valve = new SimpleRelay(_config.valvePin, _config.valveOn);
    if (_config.altPin > 0) {
        this->alt = new SimpleRelay(_config.altPin, _config.altOn);
    }

    if (_config.capabilites.pressure) {
        this->adc = new ADSAdc(_config.pressureSda, _config.pressureScl, 1);
        this->pressureSensor = new PressureSensor(this->adc);
    }
#if defined(ARDUINO_ARCH_STM32) && !defined(GAGGIMATE_DISABLE_HARDWARE_SCALE)
    if (_config.capabilites.scale) {
        scale = new HX711Scale(_config.scaleSdaPin, _config.scaleSda1Pin, _config.scaleSclPin,
                               [this](const ScaleSnapshot &snap) {
                                   ScaleSample sample{};
                                   sample.weightG = snap.weightG;
                                   sample.stddevG = snap.stddevG;
                                   sample.ch1G = snap.ch1G;
                                   sample.ch2G = snap.ch2G;
                                   sample.ch1StdG = snap.ch1StdG;
                                   sample.ch2StdG = snap.ch2StdG;
                                   sample.healthBits = snap.healthBits;
                                   sample.sampleSeq = snap.sampleSeq;
                                   _comms.sendScaleSample(sample);
                               });
    }
#endif
    if (_config.capabilites.dimming) {
        pump = new DimmedPump(_config.pumpPin, _config.pumpSensePin, pressureSensor);
    } else {
        pump = new SimplePump(_config.pumpPin, _config.pumpOn, _config.capabilites.ssrPump ? 1000.0f : 5000.0f);
    }
    this->brewBtn = new DigitalInput(_config.brewButtonPin, [this](const bool state) { _comms.sendButtonState(0, state); });
    this->steamBtn = new DigitalInput(_config.steamButtonPin, [this](const bool state) { _comms.sendButtonState(1, state); });

    // 4-Pin peripheral port
#ifdef ARDUINO_ARCH_STM32
    Wire.setSDA(_config.sunriseSdaPin);
    Wire.setSCL(_config.sunriseSclPin);
    Wire.begin();
    Wire.setClock(400000);
    this->ledController = new LedController(&Wire);
    this->distanceSensor = new DistanceSensor(&Wire, [this](int distance) { _comms.sendTofMeasurement(distance); });
#else
    albaComms = new SoftWire(_config.sunriseSdaPin, _config.sunriseSclPin);
    albaComms->setTxBuffer(albaSwTxBuffer, sizeof(albaSwTxBuffer));
    albaComms->setRxBuffer(albaSwRxBuffer, sizeof(albaSwRxBuffer));
    albaComms->setTimeout_ms(200);
    albaComms->setDelay_us(20);
    albaComms->begin();
    this->ledController = new LedController(albaComms);
    this->distanceSensor = new DistanceSensor(albaComms, [this](int distance) { _comms.sendTofMeasurement(distance); });
#endif
    if (this->ledController->isAvailable()) {
        _config.capabilites.ledControls = true;
        _config.capabilites.tof = true;
        _comms.onLedControl([this](uint8_t channel, uint8_t brightness) {
            handlePing();
            ledController->setChannel(channel, brightness);
        });
    }

    if (_config.capabilites.ledControls) {
        this->ledController->setup();
    }
    if (_config.capabilites.tof) {
        this->distanceSensor->setup();
    }

    this->thermocouple->setup();
    this->heater->setup();
    this->valve->setup();
    if (this->alt) {
        this->alt->setup();
    }
    this->pump->setup();
    if (this->gearpumpAddon != nullptr) {
        this->gearpumpAddon->setup(this->pump->getPumpPowerPtr());
        auto dimmedPump = static_cast<DimmedPump *>(pump);
        dimmedPump->setBinaryMode(true);
    }
    this->brewBtn->setup();
    this->steamBtn->setup();
    if (_config.capabilites.pressure) {
        this->adc->setup();
        pressureSensor->setup();
        _comms.onPressureScale([this](float scale) {
            handlePing();
            this->pressureSensor->setScale(scale);
        });
    }
#if defined(ARDUINO_ARCH_STM32) && !defined(GAGGIMATE_DISABLE_HARDWARE_SCALE)
    if (_config.capabilites.scale && scale != nullptr) {
        scale->setup();
        scale->setTareDoneCallback([this](long o1, long o2, float, float, bool success, uint16_t) {
            if (success) {
                _comms.sendScaleOffsets(o1, o2);
            }
        });
        scale->setCalDoneCallback([this](uint8_t channel, float factor, float, bool success, uint8_t) {
            if (success) {
                _comms.sendScaleCalibrationResult(channel, factor);
            }
        });
        _comms.onScaleTare([this]() {
            handlePing();
            scale->requestTare();
        });
        _comms.onScaleCalibration([this](float c1, float c2, long offset1, long offset2) {
            handlePing();
            scale->setCalibration(c1, c2);
            scale->setOffset(offset1, offset2);
        });
        _comms.onScaleCalibrationStart([this](uint8_t channel, float refWeight) {
            handlePing();
            scale->requestCalibration(channel, refWeight);
        });
    }
#endif
    // Set up thermal feedforward for main heater if pressure/dimming capability exists
    if (heater && _config.capabilites.dimming && _config.capabilites.pressure) {
        auto dimmedPump = static_cast<DimmedPump *>(pump);
        float *pumpFlowPtr = dimmedPump->getPumpFlowPtr();
        int *valveStatusPtr = dimmedPump->getValveStatusPtr();

        heater->setThermalFeedforward(pumpFlowPtr, 23.0f, valveStatusPtr);
        heater->setFeedforwardScale(0.0f);
    }
    // Initialize last ping time
    lastPingTime = millis();

    // Output control is split into per-component, device-numbered messages. Each
    // arrives independently (or batched together in one frame for an atomic
    // update). Control messages feed the connection watchdog via handlePing().
    _comms.onBoilerControl([this](uint8_t index, BoilerControlMode mode, float setpoint) {
#if defined(GAGGIMATE_UART_DIAGNOSTICS)
        boilerCommandCount++;
        lastBoilerSetpoint = setpoint;
#endif
        if (index != 0) { // single boiler today; reject unknown devices
            ESP_LOGW(LOG_TAG, "Ignoring boiler control for unsupported index %u", index);
            return;
        }
        handlePing();
        if (errorState != ERROR_CODE_NONE) {
            return;
        }
        if (mode == BoilerControlMode::Temperature) {
            this->heater->setSetpoint(setpoint);
        } else {
            // Pressure-regulated boiler not supported by this hardware yet.
            ESP_LOGW(LOG_TAG, "Boiler pressure mode requested but unsupported");
        }
    });
    _comms.onPumpControl([this](uint8_t index, PumpControlMode mode, float power, float pressure, float flow) {
#if defined(GAGGIMATE_UART_DIAGNOSTICS)
        pumpCommandCount++;
        lastPumpPower = power;
#endif
        if (index != 0) { // single pump today; reject unknown devices
            ESP_LOGW(LOG_TAG, "Ignoring pump control for unsupported index %u", index);
            return;
        }
        handlePing();
        if (errorState != ERROR_CODE_NONE) {
            return;
        }
        if (mode == PumpControlMode::Power) {
            this->pump->setPower(power);
            if (power == 0.0f) {
                if (gearpumpAddon != nullptr) {
                    gearpumpAddon->stop();
                }
            }
            return;
        }
        if (!_config.capabilites.dimming) {
            return;
        }
        auto dimmedPump = static_cast<DimmedPump *>(pump);
        if (mode == PumpControlMode::Pressure) {
            dimmedPump->setPressureTarget(pressure, flow);
        } else { // PumpControlMode::Flow
            dimmedPump->setFlowTarget(flow, pressure);
        }
    });
    // Binary outputs: index 0 = brew valve, index 1 = alt relay.
    _comms.onRelayControl([this](uint8_t index, bool open) {
#if defined(GAGGIMATE_UART_DIAGNOSTICS)
        relayCommandCount++;
        if (index == 0) {
            lastRelayOpen = open;
        }
#endif
        if (index == 1) {
            handlePing();
            // Alt relay: independent function, no watchdog/error gating (matches
            // the previous dedicated alt-control path).
            if (this->alt) {
                this->alt->set(open);
            }
            return;
        }
        if (index != 0) { // only 0 (brew valve) and 1 (alt) exist
            ESP_LOGW(LOG_TAG, "Ignoring relay control for unsupported index %u", index);
            return;
        }
        handlePing();
        if (errorState != ERROR_CODE_NONE) {
            return;
        }
        this->valve->set(open);
        if (_config.capabilites.dimming) {
            static_cast<DimmedPump *>(pump)->setValveState(open);
        }
    });
    _comms.onPidSettings([this](float Kp, float Ki, float Kd, float Kf) {
        handlePing();
        this->heater->setTunings(Kp, Ki, Kd);

        // Apply thermal feedforward parameters if available
        this->heater->setFeedforwardScale(Kf);
    });
    _comms.onPumpSettings([this](gm::PumpSettings settings) {
        handlePing();
        if (_config.capabilites.dimming) {
            auto dimmedPump = static_cast<DimmedPump *>(pump);
            // Check if this is a flow measurement call (a and b are flow measurements, c and d are nan)
            if (isnan(settings.c) && isnan(settings.d)) {
                dimmedPump->setPumpFlowCoeff(settings.a, settings.b); // a = oneBarFlow, b = nineBarFlow
            } else {
                dimmedPump->setPumpFlowPolyCoeffs(settings.a, settings.b, settings.c,
                                                  settings.d); // a, b, c, d are polynomial coefficients
            }
            if (this->gearpumpAddon != nullptr) {
                dimmedPump->setGains(settings.commutationGain, settings.convergenceGain, settings.integralGain);
                // Slip model is only meaningful for the positive-displacement gear/rotary-vane pump.
                dimmedPump->setPumpSlipPolyCoeffs(settings.slipA, settings.slipB, settings.slipC, settings.slipD);
            }
        }
        if (this->gearpumpAddon != nullptr) {
            gearpumpAddon->setMaxPower(settings.maxBLDCPower);
        }
    });
    _comms.onPing([this]() {
#if defined(GAGGIMATE_UART_DIAGNOSTICS)
        pingCommandCount++;
#endif
        handlePing();
    });
    _comms.onAutotune([this](uint32_t testTimeSec, uint32_t windowSize, uint32_t heaterWattage) {
        handlePing();
        if (errorState != ERROR_CODE_NONE) { // don't re-engage the heater while faulted
            return;
        }
        this->heater->autotune(static_cast<int>(testTimeSec), static_cast<int>(windowSize), static_cast<int>(heaterWattage));
    });
    _comms.onTare([this]() {
#if defined(GAGGIMATE_UART_DIAGNOSTICS)
        tareCommandCount++;
#endif
        handlePing();
        if (!_config.capabilites.dimming) {
            return;
        }
        auto dimmedPump = static_cast<DimmedPump *>(pump);
        dimmedPump->tare();
    });
    // Send INFO last: this tells the ESP32 the STM32 is ready to receive commands.
    // All callbacks and hardware must be initialized before this point.
    const bool scaleCapability =
#if defined(GAGGIMATE_DISABLE_HARDWARE_SCALE)
        false;
#else
        _config.capabilites.scale;
#endif
    gm::DeviceCapabilities capabilities = gaggimate_Capabilities_init_zero;
    capabilities.dimming = _config.capabilites.dimming;
    capabilities.pressure = _config.capabilites.pressure;
    capabilities.tof = _config.capabilites.tof;
    capabilities.led_control = _config.capabilites.ledControls;
    capabilities.scale = scaleCapability;
    if (this->gearpumpAddon != nullptr) {
        capabilities.addons_count = 1;
        capabilities.addons[0] = gaggimate_Addon_init_zero;
        capabilities.addons[0].type = 7;
    }
    _comms.init("GPBLS", _config.name.c_str(), _version, capabilities);

    ESP_LOGI(LOG_TAG, "Initialization done");
#ifdef ARDUINO_ARCH_STM32
    // Independent watchdog: there is otherwise no recovery path on the STM32, so a
    // wedged control loop or a hard fault would hang until a power cycle (the
    // "STM32 went silent mid-brew" failure). loop() feeds it every ~100 ms via
    // IWatchdog.reload(); a 4 s timeout sits far above the loop period yet recovers
    // quickly. Started here, after all blocking setup (board detect, scale init).
    IWatchdog.begin(4000000);
    xTaskCreate(ControllerLoopTask, "MainLoop", configMINIMAL_STACK_SIZE * 8, this, configMAX_PRIORITIES - 1, nullptr);
    vTaskStartScheduler();
#endif
}

void GaggiMateController::loop() {
#ifdef ARDUINO_ARCH_STM32
    // Feed the independent watchdog from the main control loop so a wedged loop
    // or hard fault resets the STM32 instead of leaving heater/pump state stale.
    IWatchdog.reload();
#endif
    unsigned long now = millis();
    if (lastPingTime < now && (now - lastPingTime) / 1000 > PING_TIMEOUT_SECONDS) {
        handlePingTimeout();
    }
    sendSensorData();
    if (errorState != ERROR_CODE_NONE) {
        ESP_LOGW("GaggiMateController", "Error state: %d", errorState);
    }
#ifdef ARDUINO_ARCH_STM32
    vTaskDelay(pdMS_TO_TICKS(250));
#else
    delay(250);
#endif
    if (Serial.available()) {
        while (Serial.available()) {
            char c = Serial.read();
            handleSerialCommand(c);
        }
    }
}

void GaggiMateController::registerBoardConfig(ControllerConfig config) { configs.push_back(config); }

void GaggiMateController::detectBoard() {
#ifdef ARDUINO_ARCH_STM32
    // STM32 has no voltage-divider auto-detect hardware.
    // Use the STM32F4 config directly.
    _config = GM_STM32F4_V1;
    ESP_LOGI(LOG_TAG, "Using Board: %s", _config.name.c_str());
    return;
#else
    constexpr int MAX_DETECT_RETRIES = 3;
    pinMode(DETECT_EN_PIN, OUTPUT);
    pinMode(DETECT_VALUE_PIN, INPUT_PULLDOWN);

    for (int attempt = 0; attempt < MAX_DETECT_RETRIES; attempt++) {
        digitalWrite(DETECT_EN_PIN, HIGH);
        delay(10); // Allow voltage to stabilize before ADC read
        uint16_t millivolts = analogReadMilliVolts(DETECT_VALUE_PIN);
        digitalWrite(DETECT_EN_PIN, LOW);
        int boardId = round(((float)millivolts) / 100.0f - 0.5f);
        ESP_LOGI(LOG_TAG, "Board detect attempt %d/%d: ID=%d (raw: %d mV)", attempt + 1, MAX_DETECT_RETRIES, boardId, millivolts);
        for (ControllerConfig config : configs) {
            if (config.autodetectValue == boardId) {
                _config = config;
                ESP_LOGI(LOG_TAG, "Using Board: %s", _config.name.c_str());
                return;
            }
        }
        ESP_LOGW(LOG_TAG, "No match on attempt %d, retrying...", attempt + 1);
        delay(500);
    }
    ESP_LOGE(LOG_TAG, "No compatible board detected after %d attempts. Restarting...", MAX_DETECT_RETRIES);
    delay(5000);
    ESP.restart();
#endif
}

void GaggiMateController::detectAddon() {
#ifdef ARDUINO_ARCH_STM32
    return;
#else
    Wire.begin(_config.ext3Pin, _config.ext2Pin, 400000);
    for (uint8_t addr = XL9555_SLAVE_ADDRESS0; addr <= XL9555_SLAVE_ADDRESS7; ++addr) {
        Wire.beginTransmission(addr);
        if (Wire.endTransmission(addr) == 0) {
            ESP_LOGI(LOG_TAG, "Found an extension at address 0x%X", addr);
            if (addr == 0x26) {
                ESP_LOGI(LOG_TAG, "Identified addon as Gearpump Addon");
                gearpumpAddon = new GearpumpAddon(addr, _config.ext3Pin, _config.ext2Pin, _config.ext1Pin);
            }
        }
    }
#endif
}

void GaggiMateController::handlePing() {
    if (errorState == ERROR_CODE_TIMEOUT) {
        errorState = ERROR_CODE_NONE;
    }
    lastPingTime = millis();
    ESP_LOGV(LOG_TAG, "Ping received, system is alive");
}

void GaggiMateController::handlePingTimeout() {
    // Turn off the heater and pump as a safety measure
    this->heater->setSetpoint(0);
    this->pump->setPower(0);
    this->valve->set(false);
    if (this->alt) {
        this->alt->set(false);
    }
    // On the healthy->timeout transition, drop the BLE link. An in-band error
    // notify can be silently swallowed on a wedged GATT (the failure mode this
    // is here to recover from), but LL_TERMINATE_IND propagates reliably at
    // the link layer. The display's existing disconnect path then rebuilds
    // the link and re-sends control state. loop() re-enters every 250 ms while
    // timed out -- guard so we don't repeatedly bounce the connection or spam
    // the log.
    if (errorState != ERROR_CODE_TIMEOUT) {
        ESP_LOGE(LOG_TAG, "Ping timeout detected. Turning off heater and pump for safety.");
        _comms.sendError(ERROR_CODE_TIMEOUT);
        if (!_comms.isUpdating())
            _comms.disconnect();
    }
    errorState = ERROR_CODE_TIMEOUT;
}

void GaggiMateController::thermalRunawayShutdown() {
    ESP_LOGE(LOG_TAG, "Thermal runaway detected! Turning off heater and pump!\n");
    // Turn off the heater and pump immediately
    this->heater->setSetpoint(0);
    this->pump->setPower(0);
    this->valve->set(false);
    if (this->alt)
        this->alt->set(false);
    errorState = ERROR_CODE_RUNAWAY;
    _comms.sendError(ERROR_CODE_RUNAWAY);
}

#if defined(GAGGIMATE_UART_DIAGNOSTICS)
ControllerDiagnostics GaggiMateController::buildControllerDiagnostics() {
    ControllerDiagnostics diagnostics{};
    diagnostics.errorCode = static_cast<uint32_t>(errorState);
    if (thermocouple != nullptr) {
        diagnostics.thermocoupleError = thermocouple->isErrorState();
        diagnostics.thermocoupleStatus = thermocouple->lastStatus();
        diagnostics.thermocoupleErrorCount = static_cast<uint32_t>(thermocouple->getErrorCount());
        diagnostics.thermocoupleReadCount = thermocouple->getReadCount();
        diagnostics.thermocoupleRawTemperature = thermocouple->rawTemperature();
        diagnostics.thermocoupleTemperature = thermocouple->filteredTemperature();
        diagnostics.thermocoupleTaskRunning = thermocouple->isTaskRunning();
    }
    if (heater != nullptr) {
        diagnostics.heaterSetpoint = heater->getSetpoint();
        diagnostics.heaterOutput = heater->getOutput();
        diagnostics.heaterRelay = heater->isRelayOn();
    }
    diagnostics.boilerCommandCount = boilerCommandCount;
    diagnostics.pumpCommandCount = pumpCommandCount;
    diagnostics.relayCommandCount = relayCommandCount;
    diagnostics.pingCommandCount = pingCommandCount;
    diagnostics.tareCommandCount = tareCommandCount;
    diagnostics.lastBoilerSetpoint = lastBoilerSetpoint;
    diagnostics.lastPumpPower = lastPumpPower;
    diagnostics.lastRelayOpen = lastRelayOpen;

    const UartDiagnostics uart = _comms.getDiagnostics();
    diagnostics.uartRxBytes = uart.displayRxByteCount;
    diagnostics.uartTxBytes = uart.displayTxByteCount;
    diagnostics.uartValidFrames = uart.displayValidFrameCount;
    diagnostics.uartParsedPayloads = uart.displayParsedEventCount;

#if defined(ARDUINO_ARCH_STM32) || defined(ESP32)
    diagnostics.freeHeap = static_cast<uint32_t>(xPortGetFreeHeapSize());
#endif
    return diagnostics;
}
#endif

void GaggiMateController::sendSensorData() {
    const float pumpPower = *pump->getPumpPowerPtr();
    const float heaterPower = heater ? heater->getDutyCycle() : 0.0f;
#if defined(GAGGIMATE_UART_DIAGNOSTICS)
    _comms.setControllerDiagnostics(buildControllerDiagnostics());
#endif
    if (_config.capabilites.pressure) {
        // Flow/volumetric come from the DimmedPump; only cast when this board
        // actually has one (pressure and dimming are configured independently).
        float puckFlow = 0.0f;
        float pumpFlow = 0.0f;
        float puckResistance = 0.0f;
        // Sensor + (optional) volumetric ride in one frame.
        gm::Payload batch[2];
        size_t n = 0;
        if (_config.capabilites.dimming) {
            auto dimmedPump = static_cast<DimmedPump *>(pump);
            puckFlow = dimmedPump->getPuckFlow();
            pumpFlow = dimmedPump->getPumpFlow();
            puckResistance = dimmedPump->getPuckResistance();
            if (this->valve->getState()) {
                batch[n++] = _comms.buildVolumetricMeasurement(dimmedPump->getCoffeeVolume());
            }
        }
        batch[n++] = _comms.buildSensorData(this->thermocouple->read(), this->pressureSensor->getPressure(), puckFlow, pumpFlow,
                                            puckResistance, pumpPower, heaterPower);
        _comms.sendUnreliableBatch(batch, n); // telemetry: fire-and-forget
    } else {
        _comms.sendSensorData(this->thermocouple->read(), 0.0f, 0.0f, 0.0f, 0.0f, pumpPower, heaterPower);
    }
}

void GaggiMateController::handleSerialCommand(char c) {
    if (c == 'S') {
        ESP_LOGI("Controller", "");
        ESP_LOGI("Controller", "╔════════════════╗");
        ESP_LOGI("Controller", "║ Status Summary ║");
        ESP_LOGI("Controller", "╠════════════════╝");
        ESP_LOGI("Controller", "║");
        ESP_LOGI("Controller", "╠═ Error codes");
        ESP_LOGI("Controller", "║  ├─ Controller Error: %d", errorState);
        ESP_LOGI("Controller", "║  └─ Thermocouple Error: %d", thermocouple->isErrorState());
        ESP_LOGI("Controller", "║");
        ESP_LOGI("Controller", "╠═ Readings");
        if (_config.capabilites.pressure) {
            auto dimmedPump = static_cast<DimmedPump *>(pump);
            ESP_LOGI("Controller", "║  ├─ Pressure: %.2f", pressureSensor->getPressure());
            ESP_LOGI("Controller", "║  ├─ Flow: %.2f", dimmedPump->getPumpFlow());
            ESP_LOGI("Controller", "║  ├─ Pump Power: %.2f", dimmedPump->getPowerTarget());
        }
        ESP_LOGI("Controller", "║  └─ Temperature: %.2f", thermocouple->read());
        ESP_LOGI("Controller", "║");
        ESP_LOGI("Controller", "╠═ Control");
        if (_config.capabilites.pressure) {
            auto dimmedPump = static_cast<DimmedPump *>(pump);
            ESP_LOGI("Controller", "║  ├─ Pressure: %.2f", dimmedPump->getPressureTarget());
            ESP_LOGI("Controller", "║  ├─ Flow: %.2f", dimmedPump->getFlowTarget());
            ESP_LOGI("Controller", "║  ├─ Pump Power: %.2f", dimmedPump->getPowerTarget());
        }
        ESP_LOGI("Controller", "║  └─ Temperature: %.2f", heater->getSetpoint());
        ESP_LOGI("Controller", "║");

#ifndef ARDUINO_ARCH_STM32
        size_t free = heap_caps_get_free_size(MALLOC_CAP_DEFAULT | MALLOC_CAP_INTERNAL);
        size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT | MALLOC_CAP_INTERNAL);
        size_t total = heap_caps_get_total_size(MALLOC_CAP_DEFAULT | MALLOC_CAP_INTERNAL);
        float fragmentation = 100 - (largest * 100) / free;
        ESP_LOGI("Controller", "╠═ Memory");
        ESP_LOGI("Controller", "║  ├─ Heap: %d / %d (%.2f%%)", (total - free), total, (100.0f * (total - free)) / total);
        ESP_LOGI("Controller", "║  └─ Fragmentation: %.2f%%", fragmentation);
#endif
        ESP_LOGI("Controller", "");
    } else {
        ESP_LOGI("Controller", "Unrecognized Input! Available commands: S (Status)");
    }
}
