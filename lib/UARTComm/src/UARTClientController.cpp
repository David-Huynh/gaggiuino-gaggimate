#include "UARTClientController.h"
#ifdef ARDUINO_ARCH_STM32
#include <STM32FreeRTOS.h>
#endif
#include <cstdio>
#include <cstdlib>
#include <cstring>

UARTClientController::UARTClientController() {
    memset(rxBuffer, 0, RX_BUFFER_SIZE);
    _txMutex = xSemaphoreCreateMutex();
}

void UARTClientController::initClient() {
    // UART is initialized by the main setup()
    // This is called to match NimBLEClientController interface
    connected = false;
}

bool UARTClientController::connectToServer() {
    // Wait until we've received the INFO message from the STM32,
    // which means it has finished setup and is ready to receive commands.
    if (systemInfo.empty()) {
        return false;
    }
    connected = true;
    return true;
}

bool UARTClientController::isConnected() const { return connected; }

bool UARTClientController::isReadyForConnection() const { return !connected; }

void UARTClientController::poll() {
    // Read available data from Serial and process events
    while (Serial2.available()) {
        char c = Serial2.read();

        if (c == '\n') {
            // End of message
            if (rxIndex > 0) {
                rxBuffer[rxIndex] = '\0';

                // Route to appropriate handler
                if (strncmp(rxBuffer, "EVT,", 4) == 0) {
                    _processEvent(rxBuffer);
                } else if (strncmp(rxBuffer, "INFO,", 5) == 0) {
                    _processInfo(rxBuffer + 5); // Skip "INFO," prefix
                }

                rxIndex = 0;
            }
        } else if (c == '\r') {
            // Skip carriage return
            continue;
        } else {
            // Add to buffer
            if (rxIndex < RX_BUFFER_SIZE - 1) {
                rxBuffer[rxIndex++] = c;
            }
        }
    }
}

// ===== Send Commands =====

void UARTClientController::sendAdvancedOutputControl(bool valve, float boilerSetpoint, bool pressureTarget, float pressure,
                                                     float flow) {
    _sendCommand("CMD,ADV,%d,%.2f,%d,%.2f,%.2f", valve ? 1 : 0, boilerSetpoint, pressureTarget ? 1 : 0, pressure, flow);
}

void UARTClientController::sendOutputControl(bool valve, float pumpSetpoint, float boilerSetpoint) {
    _sendCommand("CMD,OUT,%d,%.2f,%.2f", valve ? 1 : 0, pumpSetpoint, boilerSetpoint);
}

void UARTClientController::sendAltControl(bool pinState) { _sendCommand("CMD,ALT,%d", pinState ? 1 : 0); }

void UARTClientController::sendPing() { _sendCommand("CMD,PING"); }

void UARTClientController::sendAutotune(int testTime, int samples) { _sendCommand("CMD,AUTOTUNE,%d,%d", testTime, samples); }

void UARTClientController::sendPidSettings(const String &pid) {
    // Parse "Kp,Ki,Kd,Kf" format
    float values[4] = {0, 0, 0, 0};
    char pidCopy[64];
    strncpy(pidCopy, pid.c_str(), sizeof(pidCopy) - 1);
    pidCopy[sizeof(pidCopy) - 1] = '\0';

    char *token = strtok(pidCopy, ",");
    for (int i = 0; i < 4 && token != nullptr; i++) {
        values[i] = atof(token);
        token = strtok(nullptr, ",");
    }

    _sendCommand("CMD,PID,%.6f,%.6f,%.6f,%.6f", values[0], values[1], values[2], values[3]);
}

void UARTClientController::sendPumpModelCoeffs(const String &pumpModelCoeffs) {
    // Parse "a,b,c,d" format
    float values[4] = {0, 0, 0, 0};
    char coeffCopy[64];
    strncpy(coeffCopy, pumpModelCoeffs.c_str(), sizeof(coeffCopy) - 1);
    coeffCopy[sizeof(coeffCopy) - 1] = '\0';

    char *token = strtok(coeffCopy, ",");
    for (int i = 0; i < 4 && token != nullptr; i++) {
        values[i] = atof(token);
        token = strtok(nullptr, ",");
    }

    _sendCommand("CMD,PUMP_COEFFS,%.6f,%.6f,%.6f,%.6f", values[0], values[1], values[2], values[3]);
}

void UARTClientController::setPressureScale(float scale) { _sendCommand("CMD,PRESSURE_SCALE,%.2f", scale); }

void UARTClientController::sendLedControl(uint8_t channel, uint8_t brightness) {
    _sendCommand("CMD,LED,%d,%d", channel, brightness);
}

void UARTClientController::tare() { _sendCommand("CMD,TARE"); }

void UARTClientController::scaleTare() { _sendCommand("CMD,SCALE_TARE"); }

void UARTClientController::sendScaleCalibration(float c1, float c2, long offset1, long offset2) {
    _sendCommand("CMD,SCALE_CAL,%.6f,%.6f,%ld,%ld", c1, c2, offset1, offset2);
}
void UARTClientController::startScaleCalibration(uint8_t channel, float refWeight) {
    _sendCommand("CMD,SCALE_CAL_START,%d,%.1f", channel, refWeight);
}

// ===== Register Callbacks =====

void UARTClientController::registerRemoteErrorCallback(const remote_err_callback_t &callback) { remoteErrorCallback = callback; }

void UARTClientController::registerBrewBtnCallback(const brew_callback_t &callback) { brewBtnCallback = callback; }

void UARTClientController::registerSteamBtnCallback(const steam_callback_t &callback) { steamBtnCallback = callback; }

void UARTClientController::registerSensorCallback(const sensor_read_callback_t &callback) { sensorCallback = callback; }

void UARTClientController::registerAutotuneResultCallback(const pid_control_callback_t &callback) {
    autotuneResultCallback = callback;
}

void UARTClientController::registerVolumetricMeasurementCallback(const float_callback_t &callback) {
    volumetricMeasurementCallback = callback;
}

void UARTClientController::registerTofMeasurementCallback(const int_callback_t &callback) { tofMeasurementCallback = callback; }

void UARTClientController::registerWeightMeasurementCallback(const float_callback_t &callback) {
    weightMeasurementCallback = callback;
}

void UARTClientController::registerScaleOffsetsCallback(const scale_offsets_callback_t &callback) {
    scaleOffsetsCallback = callback;
}

void UARTClientController::registerScaleCalResultCallback(const scale_cal_result_callback_t &callback) {
    scaleCalResultCallback = callback;
}

// ===== Get Info =====

std::string UARTClientController::readInfo() const {
    // Return the info received from STM32 at startup
    if (!systemInfo.empty()) {
        return systemInfo;
    }
    // Fallback if INFO not yet received
    return R"({"hw":"GaggiMate STM32F4","v":"1.0.0","cp":{"dm":true,"ps":true,"led":false,"tof":false}})";
}

// ===== Private Helper Methods =====

void UARTClientController::_processInfo(const char *jsonString) {
    if (!jsonString || strlen(jsonString) == 0) {
        return;
    }

    // Store the received JSON capabilities
    systemInfo = jsonString;
}

void UARTClientController::_processEvent(const char *line) {
    // Events start with "EVT,"
    if (strncmp(line, "EVT,", 4) != 0) {
        return;
    }

    // Make a mutable copy for strtok
    char lineCopy[RX_BUFFER_SIZE];
    strncpy(lineCopy, line, sizeof(lineCopy) - 1);
    lineCopy[sizeof(lineCopy) - 1] = '\0';

    // Skip "EVT,"
    char *eventType = lineCopy + 4;
    char *args = strchr(eventType, ',');
    if (args) {
        *args = '\0'; // Null-terminate event type
        args++;       // Skip the comma
    }

    // Route to appropriate handler
    if (strcmp(eventType, "SENSOR") == 0) {
        _processEventSensor(args);
    } else if (strcmp(eventType, "PING") == 0) {
        _processEventPing(args);
    } else if (strcmp(eventType, "BTN_BREW") == 0) {
        _processEventBtnBrew(args);
    } else if (strcmp(eventType, "BTN_STEAM") == 0) {
        _processEventBtnSteam(args);
    } else if (strcmp(eventType, "ERR") == 0) {
        _processEventError(args);
    } else if (strcmp(eventType, "TOF") == 0) {
        _processEventTof(args);
    } else if (strcmp(eventType, "VOLUMETRIC") == 0) {
        _processEventVolumetric(args);
    } else if (strcmp(eventType, "WEIGHT") == 0) {
        _processEventWeight(args);
    } else if (strcmp(eventType, "SCALE_OFFSETS") == 0) {
        _processEventScaleOffsets(args);
    } else if (strcmp(eventType, "SCALE_CAL_RESULT") == 0) {
        _processEventScaleCalResult(args);
    } else if (strcmp(eventType, "AUTOTUNE") == 0) {
        _processEventAutotune(args);
    }
}

void UARTClientController::_processEventSensor(char *args) {
    if (!sensorCallback || !args)
        return;

    char token[32];
    if (!_getToken(args, 0, ',', token, sizeof(token)))
        return;
    float temp = _parseFloat(token);
    if (!_getToken(args, 1, ',', token, sizeof(token)))
        return;
    float pressure = _parseFloat(token);
    if (!_getToken(args, 2, ',', token, sizeof(token)))
        return;
    float puckFlow = _parseFloat(token);
    if (!_getToken(args, 3, ',', token, sizeof(token)))
        return;
    float pumpFlow = _parseFloat(token);
    if (!_getToken(args, 4, ',', token, sizeof(token)))
        return;
    float resistance = _parseFloat(token);

    sensorCallback(temp, pressure, puckFlow, pumpFlow, resistance);
}

void UARTClientController::_processEventPing(char *args) {
    (void)args; // Unused
    if (connected) {
        // Keep-alive
        return;
    }
    connected = true;
}

void UARTClientController::_processEventBtnBrew(char *args) {
    if (!brewBtnCallback || !args)
        return;
    int state = _parseInt(args);
    brewBtnCallback(state);
}

void UARTClientController::_processEventBtnSteam(char *args) {
    if (!steamBtnCallback || !args)
        return;
    int state = _parseInt(args);
    steamBtnCallback(state);
}

void UARTClientController::_processEventError(char *args) {
    if (!remoteErrorCallback || !args)
        return;
    int errorCode = _parseInt(args);
    remoteErrorCallback(errorCode);
}

void UARTClientController::_processEventTof(char *args) {
    if (!tofMeasurementCallback || !args)
        return;
    int distance = _parseInt(args);
    tofMeasurementCallback(distance);
}

void UARTClientController::_processEventVolumetric(char *args) {
    if (!volumetricMeasurementCallback || !args)
        return;
    float volume = _parseFloat(args);
    volumetricMeasurementCallback(volume);
}

void UARTClientController::_processEventWeight(char *args) {
    if (!weightMeasurementCallback || !args)
        return;
    float weight = _parseFloat(args);
    weightMeasurementCallback(weight);
}

void UARTClientController::_processEventScaleOffsets(char *args) {
    if (!scaleOffsetsCallback || !args)
        return;
    char token[32];
    if (!_getToken(args, 0, ',', token, sizeof(token)))
        return;
    long offset1 = atol(token);
    if (!_getToken(args, 1, ',', token, sizeof(token)))
        return;
    long offset2 = atol(token);
    scaleOffsetsCallback(offset1, offset2);
}

void UARTClientController::_processEventScaleCalResult(char *args) {
    if (!scaleCalResultCallback || !args)
        return;
    char token[32];
    if (!_getToken(args, 0, ',', token, sizeof(token)))
        return;
    uint8_t channel = static_cast<uint8_t>(atoi(token));
    if (!_getToken(args, 1, ',', token, sizeof(token)))
        return;
    float calibration = _parseFloat(token);
    scaleCalResultCallback(channel, calibration);
}

void UARTClientController::_processEventAutotune(char *args) {
    if (!autotuneResultCallback || !args)
        return;

    char token[32];
    if (!_getToken(args, 0, ',', token, sizeof(token)))
        return;
    float Kp = _parseFloat(token);
    if (!_getToken(args, 1, ',', token, sizeof(token)))
        return;
    float Ki = _parseFloat(token);
    if (!_getToken(args, 2, ',', token, sizeof(token)))
        return;
    float Kd = _parseFloat(token);
    if (!_getToken(args, 3, ',', token, sizeof(token)))
        return;
    float Kf = _parseFloat(token);

    autotuneResultCallback(Kp, Ki, Kd, Kf);
}

void UARTClientController::_sendCommand(const char *format, ...) {
    char buffer[256];

    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    if (xSemaphoreTake(_txMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        Serial2.println(buffer);
        Serial2.flush();
        xSemaphoreGive(_txMutex);
    }
}

bool UARTClientController::_getToken(char *input, uint8_t index, char separator, char *out, size_t outSize) {
    if (!input || !out || outSize == 0)
        return false;
    out[0] = '\0';

    char *start = input;
    for (uint8_t i = 0; i < index; i++) {
        start = strchr(start, separator);
        if (!start)
            return false;
        start++; // Skip separator
    }

    char *end = strchr(start, separator);
    size_t len;
    if (end) {
        len = end - start;
        if (len >= outSize)
            len = outSize - 1;
        strncpy(out, start, len);
        out[len] = '\0';
    } else {
        strncpy(out, start, outSize - 1);
        out[outSize - 1] = '\0';
    }
    return true;
}

float UARTClientController::_parseFloat(const char *str) {
    if (!str || strlen(str) == 0)
        return 0.0f;
    return atof(str);
}

int UARTClientController::_parseInt(const char *str) {
    if (!str || strlen(str) == 0)
        return 0;
    return atoi(str);
}