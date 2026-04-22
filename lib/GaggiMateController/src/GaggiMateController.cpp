#ifdef ARDUINO_ARCH_STM32
#include <STM32FreeRTOS.h>
#else
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#endif
#include "GaggiMateController.h"
#ifndef ARDUINO_ARCH_STM32
#include "NimBLEServerController.h"
#endif
#include "logging.h"
#include "utilities.h"
#include <Arduino.h>
#include <peripherals/DimmedPump.h>
#include <peripherals/SimplePump.h>

#include <cstdio>
#include <utility>

GaggiMateController::GaggiMateController(String version, CommunicationHandler *commHandler)
    : _version(std::move(version)), _comm(commHandler), _comm_owned(false) {
// If no communication handler provided, create default NimBLEServerController for backward compatibility
#ifndef ARDUINO_ARCH_STM32
    if (_comm == nullptr) {
        _comm = new NimBLEServerController();
        _comm_owned = true;
    }
#endif
    configs.push_back(GM_STANDARD_REV_1X);
    configs.push_back(GM_STANDARD_REV_2X);
    configs.push_back(GM_STANDARD_REV_3X);
    configs.push_back(GM_PRO_REV_1x);
    configs.push_back(GM_PRO_LEGO);
    configs.push_back(GM_PRO_REV_11);
    configs.push_back(GM_STM32F4_V1);
}
#ifdef __cplusplus
extern "C" {
#endif

void ControllerLoopTask(void *pvParameters);

#ifdef __cplusplus
}
#endif
void ControllerLoopTask(void *pvParameters) {
    auto *controller = static_cast<GaggiMateController *>(pvParameters);

    for (;;) {
        controller->loop();
    }
}

void GaggiMateController::setup() {
    detectBoard();
    detectAddon();

    this->thermocouple = new Max31855Thermocouple(
        _config.maxCsPin, _config.maxMisoPin, _config.maxSckPin, [this](float temperature) { /* noop */ },
        [this]() { thermalRunawayShutdown(); });
    this->heater = new Heater(
        this->thermocouple, _config.heaterPin, [this]() { thermalRunawayShutdown(); },
        [this](float Kp, float Ki, float Kd) { _comm->sendAutotuneResult(Kp, Ki, Kd); });
    this->valve = new SimpleRelay(_config.valvePin, _config.valveOn);
    if (_config.altPin > 0) {
        this->alt = new SimpleRelay(_config.altPin, _config.altOn);
    }

    if (_config.capabilites.pressure) {
        pressureSensor = new PressureSensor(_config.pressureSda, _config.pressureScl, [this](float pressure) { /* noop */ });
    }
#ifdef ARDUINO_ARCH_STM32
    if (_config.capabilites.scale) {
        scale = new HX711Scale(_config.scaleSdaPin, _config.scaleSda1Pin, _config.scaleSclPin,
                               [this](float weight) { _comm->sendWeightMeasurement(weight); });
    }
#endif
    if (_config.capabilites.dimming) {
        pump = new DimmedPump(_config.pumpPin, _config.pumpSensePin, pressureSensor);
    } else {
        pump = new SimplePump(_config.pumpPin, _config.pumpOn, _config.capabilites.ssrPump ? 1000.0f : 5000.0f);
    }

    this->brewBtn = new DigitalInput(_config.brewButtonPin, [this](const bool state) { _comm->sendBrewBtnState(state); });
    this->steamBtn = new DigitalInput(_config.steamButtonPin, [this](const bool state) { _comm->sendSteamBtnState(state); });

// 4-Pin peripheral port
#ifdef ARDUINO_ARCH_STM32
    Wire.setSDA(_config.sunriseSdaPin);
    Wire.setSCL(_config.sunriseSclPin);
    Wire.begin();
    Wire.setClock(400000);
#else
    if (!Wire.begin(_config.sunriseSdaPin, _config.sunriseSclPin, 400000)) {
        ESP_LOGE(LOG_TAG, "Failed to initialize I2C bus");
    }
#endif

    this->ledController = new LedController(&Wire);
    this->distanceSensor = new DistanceSensor(&Wire, [this](int distance) { _comm->sendTofMeasurement(distance); });
    if (this->ledController->isAvailable()) {
        _config.capabilites.ledControls = true;
        _config.capabilites.tof = true;
        _comm->registerLedControlCallback(
            [this](uint8_t channel, uint8_t brightness) { ledController->setChannel(channel, brightness); });
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
    this->brewBtn->setup();
    this->steamBtn->setup();
    if (_config.capabilites.pressure) {
        pressureSensor->setup();
        _comm->registerPressureScaleCallback([this](float scale) { this->pressureSensor->setScale(scale); });
    }
#ifdef ARDUINO_ARCH_STM32
    if (_config.capabilites.scale) {
        scale->setup();
        scale->setTareResultCallback([this](long o1, long o2) { _comm->sendScaleOffsets(o1, o2); });
        _comm->registerScaleTareCallback([this]() { scale->tare(); });
        _comm->registerScaleCalibrationCallback([this](float c1, float c2, long offset1, long offset2) {
            scale->setCalibration(c1, c2);
            if (offset1 != 0 || offset2 != 0) {
                scale->setOffset(offset1, offset2);
            }
        });
        _comm->registerScaleCalStartCallback([this](uint8_t channel, float refWeight) {
            float newFactor = scale->calibrateChannel(channel, refWeight);
            if (newFactor != 0.0f) {
                _comm->sendScaleCalResult(channel, newFactor);
            }
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

    _comm->registerOutputControlCallback([this](bool valve, float pumpSetpoint, float heaterSetpoint) {
        handlePing();
        if (errorState != ERROR_CODE_NONE) {
            return;
        }
        this->pump->setPower(pumpSetpoint);
        this->valve->set(valve);
        this->heater->setSetpoint(heaterSetpoint);
        if (!_config.capabilites.dimming) {
            return;
        }
        auto dimmedPump = static_cast<DimmedPump *>(pump);
        dimmedPump->setValveState(valve);
    });
    _comm->registerAdvancedOutputControlCallback(
        [this](bool valve, float heaterSetpoint, bool pressureTarget, float pressure, float flow) {
            handlePing();
            if (errorState != ERROR_CODE_NONE) {
                return;
            }
            this->valve->set(valve);
            this->heater->setSetpoint(heaterSetpoint);
            if (!_config.capabilites.dimming) {
                return;
            }
            auto dimmedPump = static_cast<DimmedPump *>(pump);
            if (pressureTarget) {
                dimmedPump->setPressureTarget(pressure, flow);
            } else {
                dimmedPump->setFlowTarget(flow, pressure);
            }
            dimmedPump->setValveState(valve);
        });
    _comm->registerAltControlCallback([this](bool state) {
        if (this->alt)
            this->alt->set(state);
    });
    _comm->registerPidControlCallback([this](float Kp, float Ki, float Kd, float Kf) {
        this->heater->setTunings(Kp, Ki, Kd);

        // Apply thermal feedforward parameters if available
        this->heater->setFeedforwardScale(Kf);
    });
    _comm->registerPumpModelCoeffsCallback([this](float a, float b, float c, float d) {
        if (_config.capabilites.dimming) {
            auto dimmedPump = static_cast<DimmedPump *>(pump);
            // Check if this is a flow measurement call (a and b are flow measurements, c and d are nan)
            if (isnan(c) && isnan(d)) {
                dimmedPump->setPumpFlowCoeff(a, b); // a = oneBarFlow, b = nineBarFlow
            } else {
                dimmedPump->setPumpFlowPolyCoeffs(a, b, c, d); // a, b, c, d are polynomial coefficients
            }
        }
    });
    _comm->registerPingCallback([this]() { handlePing(); });
    _comm->registerAutotuneCallback([this](int goal, int windowSize) { this->heater->autotune(goal, windowSize); });
    _comm->registerTareCallback([this]() {
        if (!_config.capabilites.dimming) {
            return;
        }
        auto dimmedPump = static_cast<DimmedPump *>(pump);
        dimmedPump->tare();
    });
    // Send INFO last — this tells the ESP32 the STM32 is ready to receive commands.
    // All callbacks and hardware must be initialized before this point.
    String systemInfo = make_system_info(_config, _version);
    _comm->initServer(systemInfo);

    ESP_LOGI(LOG_TAG, "Initialization done");
#ifdef ARDUINO_ARCH_STM32
    xTaskCreate(ControllerLoopTask, "MainLoop", configMINIMAL_STACK_SIZE * 8, this, configMAX_PRIORITIES - 1, NULL);

    vTaskStartScheduler();
    // The code will never reach beyond this point on STM32
#endif
}

void GaggiMateController::loop() {
#ifdef ARDUINO_ARCH_STM32
    static TickType_t lastWake = xTaskGetTickCount();
    if (_comm) {
        _comm->processQueue();
    }
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
    vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(100));
#else
    delay(250);
#endif
}

void GaggiMateController::registerBoardConfig(ControllerConfig config) { configs.push_back(config); }

void GaggiMateController::detectBoard() {
#ifdef ARDUINO_ARCH_STM32
    // STM32 has no voltage-divider auto-detect hardware.
    // Use the STM32F4 config directly.
    _config = GM_STM32F4_V1;
    ESP_LOGI(LOG_TAG, "Using Board: %s", _config.name.c_str());
    return;
#endif
    constexpr int MAX_DETECT_RETRIES = 3;
    pinMode(DETECT_EN_PIN, OUTPUT);
    pinMode(DETECT_VALUE_PIN, INPUT_PULLDOWN);

    for (int attempt = 0; attempt < MAX_DETECT_RETRIES; attempt++) {
        digitalWrite(DETECT_EN_PIN, HIGH);
        delay(10); // Allow voltage to stabilize before ADC read
#ifdef ARDUINO_ARCH_STM32
        // STM32 standard analog read (typically 12-bit, 0-4095 over 3.3V)
        uint16_t raw = analogRead(DETECT_VALUE_PIN);
        uint16_t millivolts = (raw * 3300) / 4095;
#else
        // ESP32 specific calibrated read
        uint16_t millivolts = analogReadMilliVolts(DETECT_VALUE_PIN);
#endif
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
#ifdef ARDUINO_ARCH_STM32
    NVIC_SystemReset(); // Standard ARM Cortex way to reset
#else
    ESP.restart(); // ESP32 way to reset
#endif
}

void GaggiMateController::detectAddon() {
    // TODO: Add I2C scanning for extensions
}

void GaggiMateController::handlePing() {
    if (errorState == ERROR_CODE_TIMEOUT) {
        errorState = ERROR_CODE_NONE;
    }
    lastPingTime = millis();
    ESP_LOGV(LOG_TAG, "Ping received, system is alive");
}

void GaggiMateController::handlePingTimeout() {
    ESP_LOGE(LOG_TAG, "Ping timeout detected. Turning off heater and pump for safety.\n");
    // Turn off the heater and pump as a safety measure
    this->heater->setSetpoint(0);
    this->pump->setPower(0);
    this->valve->set(false);
    if (this->alt)
        this->alt->set(false);
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
    _comm->sendError(ERROR_CODE_RUNAWAY);
}

void GaggiMateController::sendSensorData() {
    if (_config.capabilites.pressure) {
        auto dimmedPump = static_cast<DimmedPump *>(pump);
        _comm->sendSensorData(this->thermocouple->read(), this->pressureSensor->getPressure(), dimmedPump->getPuckFlow(),
                              dimmedPump->getPumpFlow(), dimmedPump->getPuckResistance());
        if (this->valve->getState()) {
            _comm->sendVolumetricMeasurement(dimmedPump->getCoffeeVolume());
        }
    } else {
        _comm->sendSensorData(this->thermocouple->read(), 0.0f, 0.0f, 0.0f, 0.0f);
    }
}
