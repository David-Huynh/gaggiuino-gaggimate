#ifdef ARDUINO_ARCH_STM32
#include <STM32FreeRTOS.h>
#endif
#include "UARTComm.h"
#include <Arduino.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>

UARTComm::UARTComm(HardwareSerial *uartPort, uint32_t baudRate) : _uart(uartPort), _baudRate(baudRate) {
    if (_uart) {
        _uart->begin(baudRate);
    }
}
void UARTComm::initServer(String infoString) {
    _sendLine("INFO,%s", infoString.c_str());

    // Create queue (holds up to 10 commands)
    if (!_cmdQueue) {
        _cmdQueue = xQueueCreate(10, sizeof(UARTCommand));
    }

    // Create UART receive task if not already running
    if (_uart && !_uartTaskHandle) {
        xTaskCreate(uartTaskFunction, "UART_RX", configMINIMAL_STACK_SIZE * 8, this, configMAX_PRIORITIES - 2, &_uartTaskHandle);
    }
}

void UARTComm::sendSensorData(float temperature, float pressure, float puckFlow, float pumpFlow, float puckResistance) {
    _sendLine("EVT,SENSOR,%.3f,%.3f,%.3f,%.3f,%.3f", temperature, pressure, puckFlow, pumpFlow, puckResistance);
}

void UARTComm::sendError(int errorCode) { _sendLine("EVT,ERR,%d", errorCode); }

void UARTComm::sendBrewBtnState(bool brewButtonStatus) { _sendLine("EVT,BTN_BREW,%d", brewButtonStatus ? 1 : 0); }

void UARTComm::sendSteamBtnState(bool steamButtonStatus) { _sendLine("EVT,BTN_STEAM,%d", steamButtonStatus ? 1 : 0); }

void UARTComm::sendAutotuneResult(float Kp, float Ki, float Kd) {
    _sendLine("EVT,AUTOTUNE,%.6f,%.6f,%.6f,%.6f", Kp, Ki, Kd, 0.0f);
}

void UARTComm::sendVolumetricMeasurement(float value) { _sendLine("EVT,VOLUMETRIC,%.3f", value); }

void UARTComm::sendTofMeasurement(int value) { _sendLine("EVT,TOF,%d", value); }

void UARTComm::sendWeightMeasurement(float weight) { _sendLine("EVT,WEIGHT,%.2f", weight); }

void UARTComm::registerOutputControlCallback(const simple_output_callback_t &callback) { _outputControlCallback = callback; }

void UARTComm::registerAdvancedOutputControlCallback(const advanced_output_callback_t &callback) {
    _advancedControlCallback = callback;
}

void UARTComm::registerAltControlCallback(const pin_control_callback_t &callback) { _altControlCallback = callback; }

void UARTComm::registerPidControlCallback(const pid_control_callback_t &callback) { _pidControlCallback = callback; }

void UARTComm::registerPumpModelCoeffsCallback(const pump_model_coeffs_callback_t &callback) {
    _pumpModelCoeffsCallback = callback;
}

void UARTComm::registerPingCallback(const ping_callback_t &callback) { _pingCallback = callback; }

void UARTComm::registerAutotuneCallback(const autotune_callback_t &callback) { _autotuneCallback = callback; }

void UARTComm::registerPressureScaleCallback(const float_callback_t &callback) { _pressureScaleCallback = callback; }

void UARTComm::registerTareCallback(const void_callback_t &callback) { _tareCallback = callback; }

void UARTComm::registerScaleTareCallback(const void_callback_t &callback) { _scaleTareCallback = callback; }

void UARTComm::registerScaleCalibrationCallback(const scale_calibration_callback_t &callback) { _scaleCalibrationCallback = callback; }

void UARTComm::registerScaleCalStartCallback(const scale_cal_start_callback_t &callback) { _scaleCalStartCallback = callback; }

void UARTComm::sendScaleOffsets(long offset1, long offset2) { _sendLine("EVT,SCALE_OFFSETS,%ld,%ld", offset1, offset2); }

void UARTComm::sendScaleCalResult(uint8_t channel, float calibration) { _sendLine("EVT,SCALE_CAL_RESULT,%d,%.6f", channel, calibration); }

void UARTComm::registerLedControlCallback(const led_control_callback_t &callback) { _ledControlCallback = callback; }

void UARTComm::update() {
    if (!_uart) {
        return;
    }

    // Read available bytes from UART
    while (_uart->available()) {
        char c = _uart->read();

        if (c == '\n' || c == '\r') {
            // End of line - process command
            if (_rxIndex > 0) {
                _rxBuffer[_rxIndex] = '\0';
                _processCommand(_rxBuffer);
                _rxIndex = 0;
            }
        } else if (c >= 32 && c < 127) {
            // Printable character
            if (_rxIndex < UART_RX_BUFFER_SIZE - 1) {
                _rxBuffer[_rxIndex++] = c;
            } else {
                // Buffer overflow
                _rxIndex = 0;
                if (_uart) {
                    _sendLine("EVT,ERR,%d", ERROR_CODE_RX_OVERFLOW);
                }
            }
        }
    }
}
void UARTComm::uartTaskFunction(void *param) {
    UARTComm *self = static_cast<UARTComm *>(param);
    self->uartTaskLoop();
}

void UARTComm::uartTaskLoop() {
    TickType_t lastWake = xTaskGetTickCount();
    while (true) {
        update(); // reads UART and calls _processCommand (which now queues)
        vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(10));
    }
}

void UARTComm::_processCommand(const char *line) {
    if (!line || strlen(line) == 0 || !_cmdQueue)
        return;

    char cmdCopy[UART_RX_BUFFER_SIZE];
    strncpy(cmdCopy, line, UART_RX_BUFFER_SIZE - 1);
    cmdCopy[UART_RX_BUFFER_SIZE - 1] = '\0';

    char *saveptr;
    char *token = strtok_r(cmdCopy, ",", &saveptr);
    if (!token || strcmp(token, "CMD") != 0)
        return;

    token = strtok_r(nullptr, ",", &saveptr);
    if (!token) {
        _sendLine("EVT,ERR,%d", ERROR_CODE_UNKNOWN_CMD);
        return;
    }

    char *args = strtok_r(nullptr, "", &saveptr);

    UARTCommand cmd;
    memset(&cmd, 0, sizeof(cmd));

    if (strcmp(token, "OUT") == 0) {
        if (!args)
            return;
        char *argsptr;
        char *valve_str = strtok_r(args, ",", &argsptr);
        char *pump_str = strtok_r(nullptr, ",", &argsptr);
        char *heater_str = strtok_r(nullptr, ",", &argsptr);
        if (valve_str && pump_str && heater_str) {
            cmd.type = UARTCommand::CMD_OUT;
            cmd.data.out.valve = atoi(valve_str) != 0;
            cmd.data.out.pump = strtof(pump_str, nullptr);
            cmd.data.out.heater = strtof(heater_str, nullptr);
            xQueueSend(_cmdQueue, &cmd, portMAX_DELAY);
        }
    } else if (strcmp(token, "ADV") == 0) {
        if (!args)
            return;
        char *argsptr;
        char *valve_str = strtok_r(args, ",", &argsptr);
        char *heater_str = strtok_r(nullptr, ",", &argsptr);
        char *pres_target_str = strtok_r(nullptr, ",", &argsptr);
        char *pressure_str = strtok_r(nullptr, ",", &argsptr);
        char *flow_str = strtok_r(nullptr, ",", &argsptr);
        if (valve_str && heater_str && pres_target_str && pressure_str && flow_str) {
            cmd.type = UARTCommand::CMD_ADV;
            cmd.data.adv.valve = atoi(valve_str) != 0;
            cmd.data.adv.heater = strtof(heater_str, nullptr);
            cmd.data.adv.pressureTarget = atoi(pres_target_str) != 0;
            cmd.data.adv.pressure = strtof(pressure_str, nullptr);
            cmd.data.adv.flow = strtof(flow_str, nullptr);
            xQueueSend(_cmdQueue, &cmd, portMAX_DELAY);
        }
    } else if (strcmp(token, "ALT") == 0) {
        if (!args)
            return;
        cmd.type = UARTCommand::CMD_ALT;
        cmd.data.alt.state = atoi(args) != 0;
        xQueueSend(_cmdQueue, &cmd, portMAX_DELAY);
    } else if (strcmp(token, "PID") == 0) {
        if (!args)
            return;
        char *argsptr;
        char *kp_str = strtok_r(args, ",", &argsptr);
        char *ki_str = strtok_r(nullptr, ",", &argsptr);
        char *kd_str = strtok_r(nullptr, ",", &argsptr);
        char *kf_str = strtok_r(nullptr, ",", &argsptr);
        if (kp_str && ki_str && kd_str && kf_str) {
            cmd.type = UARTCommand::CMD_PID;
            cmd.data.pid.kp = strtof(kp_str, nullptr);
            cmd.data.pid.ki = strtof(ki_str, nullptr);
            cmd.data.pid.kd = strtof(kd_str, nullptr);
            cmd.data.pid.kf = strtof(kf_str, nullptr);
            xQueueSend(_cmdQueue, &cmd, portMAX_DELAY);
        }
    } else if (strcmp(token, "PUMP_COEFFS") == 0) {
        if (!args)
            return;
        char *argsptr;
        char *a_str = strtok_r(args, ",", &argsptr);
        char *b_str = strtok_r(nullptr, ",", &argsptr);
        char *c_str = strtok_r(nullptr, ",", &argsptr);
        char *d_str = strtok_r(nullptr, ",", &argsptr);
        if (a_str && b_str && c_str && d_str) {
            cmd.type = UARTCommand::CMD_PUMP_COEFFS;
            cmd.data.pumpCoeffs.a = strtof(a_str, nullptr);
            cmd.data.pumpCoeffs.b = strtof(b_str, nullptr);
            cmd.data.pumpCoeffs.c = strtof(c_str, nullptr);
            cmd.data.pumpCoeffs.d = strtof(d_str, nullptr);
            xQueueSend(_cmdQueue, &cmd, portMAX_DELAY);
        }
    } else if (strcmp(token, "AUTOTUNE") == 0) {
        if (!args)
            return;
        char *argsptr;
        char *goal_str = strtok_r(args, ",", &argsptr);
        char *window_str = strtok_r(nullptr, ",", &argsptr);
        if (goal_str && window_str) {
            cmd.type = UARTCommand::CMD_AUTOTUNE;
            cmd.data.autotune.goal = atoi(goal_str);
            cmd.data.autotune.window = atoi(window_str);
            xQueueSend(_cmdQueue, &cmd, portMAX_DELAY);
        }
    } else if (strcmp(token, "PRESSURE_SCALE") == 0) {
        if (!args)
            return;
        cmd.type = UARTCommand::CMD_PRESSURE_SCALE;
        cmd.data.pressureScale.scale = strtof(args, nullptr);
        xQueueSend(_cmdQueue, &cmd, portMAX_DELAY);
    } else if (strcmp(token, "LED") == 0) {
        if (!args)
            return;
        char *argsptr;
        char *channel_str = strtok_r(args, ",", &argsptr);
        char *brightness_str = strtok_r(nullptr, ",", &argsptr);
        if (channel_str && brightness_str) {
            cmd.type = UARTCommand::CMD_LED;
            cmd.data.led.channel = atoi(channel_str);
            cmd.data.led.brightness = atoi(brightness_str);
            xQueueSend(_cmdQueue, &cmd, portMAX_DELAY);
            // Note: EVT,LED,OK will be sent from processQueue after execution
        }
    } else if (strcmp(token, "PING") == 0) {
        cmd.type = UARTCommand::CMD_PING;
        xQueueSend(_cmdQueue, &cmd, portMAX_DELAY);
    } else if (strcmp(token, "TARE") == 0) {
        cmd.type = UARTCommand::CMD_TARE;
        xQueueSend(_cmdQueue, &cmd, portMAX_DELAY);
    } else if (strcmp(token, "SCALE_TARE") == 0) {
        cmd.type = UARTCommand::CMD_SCALE_TARE;
        xQueueSend(_cmdQueue, &cmd, portMAX_DELAY);
    } else if (strcmp(token, "SCALE_CAL") == 0) {
        if (!args)
            return;
        char *argsptr;
        char *c1_str = strtok_r(args, ",", &argsptr);
        char *c2_str = strtok_r(nullptr, ",", &argsptr);
        if (c1_str && c2_str) {
            cmd.type = UARTCommand::CMD_SCALE_CAL;
            cmd.data.scaleCal.c1 = strtof(c1_str, nullptr);
            cmd.data.scaleCal.c2 = strtof(c2_str, nullptr);
            char *o1_str = strtok_r(nullptr, ",", &argsptr);
            char *o2_str = strtok_r(nullptr, ",", &argsptr);
            cmd.data.scaleCal.offset1 = o1_str ? atol(o1_str) : 0;
            cmd.data.scaleCal.offset2 = o2_str ? atol(o2_str) : 0;
            xQueueSend(_cmdQueue, &cmd, portMAX_DELAY);
        }
    } else if (strcmp(token, "SCALE_CAL_START") == 0) {
        if (!args)
            return;
        char *argsptr;
        char *ch_str = strtok_r(args, ",", &argsptr);
        char *ref_str = strtok_r(nullptr, ",", &argsptr);
        if (ch_str && ref_str) {
            cmd.type = UARTCommand::CMD_SCALE_CAL_START;
            cmd.data.scaleCalStart.channel = static_cast<uint8_t>(atoi(ch_str));
            cmd.data.scaleCalStart.refWeight = strtof(ref_str, nullptr);
            xQueueSend(_cmdQueue, &cmd, portMAX_DELAY);
        }
    } else {
        _sendLine("EVT,ERR,%d", ERROR_CODE_UNKNOWN_CMD);
    }
}

void UARTComm::processQueue() {
    if (!_cmdQueue)
        return;

    UARTCommand cmd;
    while (xQueueReceive(_cmdQueue, &cmd, 0) == pdTRUE) {
        switch (cmd.type) {
        case UARTCommand::CMD_OUT:
            if (_outputControlCallback) {
                _outputControlCallback(cmd.data.out.valve, cmd.data.out.pump, cmd.data.out.heater);
            }
            break;
        case UARTCommand::CMD_ADV:
            if (_advancedControlCallback) {
                _advancedControlCallback(cmd.data.adv.valve, cmd.data.adv.heater, cmd.data.adv.pressureTarget,
                                         cmd.data.adv.pressure, cmd.data.adv.flow);
            }
            break;
        case UARTCommand::CMD_ALT:
            if (_altControlCallback) {
                _altControlCallback(cmd.data.alt.state);
            }
            break;
        case UARTCommand::CMD_PID:
            if (_pidControlCallback) {
                _pidControlCallback(cmd.data.pid.kp, cmd.data.pid.ki, cmd.data.pid.kd, cmd.data.pid.kf);
            }
            break;
        case UARTCommand::CMD_PUMP_COEFFS:
            if (_pumpModelCoeffsCallback) {
                _pumpModelCoeffsCallback(cmd.data.pumpCoeffs.a, cmd.data.pumpCoeffs.b, cmd.data.pumpCoeffs.c,
                                         cmd.data.pumpCoeffs.d);
            }
            break;
        case UARTCommand::CMD_AUTOTUNE:
            if (_autotuneCallback) {
                _autotuneCallback(cmd.data.autotune.goal, cmd.data.autotune.window);
            }
            break;
        case UARTCommand::CMD_PRESSURE_SCALE:
            if (_pressureScaleCallback) {
                _pressureScaleCallback(cmd.data.pressureScale.scale);
            }
            break;
        case UARTCommand::CMD_LED:
            if (_ledControlCallback) {
                _ledControlCallback(cmd.data.led.channel, cmd.data.led.brightness);
                _sendLine("EVT,LED,OK"); // send ACK now
            }
            break;
        case UARTCommand::CMD_PING:
            if (_pingCallback) {
                _pingCallback();
            }
            _sendLine("EVT,PING");
            break;
        case UARTCommand::CMD_TARE:
            if (_tareCallback) {
                _tareCallback();
            }
            break;
        case UARTCommand::CMD_SCALE_TARE:
            if (_scaleTareCallback) {
                _scaleTareCallback();
            }
            break;
        case UARTCommand::CMD_SCALE_CAL:
            if (_scaleCalibrationCallback) {
                _scaleCalibrationCallback(cmd.data.scaleCal.c1, cmd.data.scaleCal.c2,
                                          cmd.data.scaleCal.offset1, cmd.data.scaleCal.offset2);
            }
            break;
        case UARTCommand::CMD_SCALE_CAL_START:
            if (_scaleCalStartCallback) {
                _scaleCalStartCallback(cmd.data.scaleCalStart.channel, cmd.data.scaleCalStart.refWeight);
            }
            break;
        }
    }
}

void UARTComm::_sendLine(const char *format, ...) {
    if (!_uart) {
        return;
    }

    char buffer[UART_TX_BUFFER_SIZE];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, UART_TX_BUFFER_SIZE, format, args);
    va_end(args);

    _uart->println(buffer);
}

char *UARTComm::_getToken(char *input, uint8_t index, char separator) {
    if (!input) {
        return nullptr;
    }

    char sepStr[] = {separator, '\0'};
    char *token = strtok(input, sepStr);
    for (uint8_t i = 0; i < index && token; i++) {
        token = strtok(nullptr, sepStr);
    }
    return token;
}

float UARTComm::_parseFloat(const char *str) {
    if (!str) {
        return 0.0f;
    }
    return strtof(str, nullptr);
}

int UARTComm::_parseInt(const char *str) {
    if (!str) {
        return 0;
    }
    return atoi(str);
}
