#ifndef UARTCLIENTCONTROLLER_H
#define UARTCLIENTCONTROLLER_H
#ifdef ARDUINO_ARCH_STM32
#include <STM32FreeRTOS.h>
#endif
#include <Arduino.h>
#include <cstring>
#include <functional>
#include <string>
#include "CommonTypes.h"
#include "ClientCommunicationHandler.h"

/**
 * @brief UART Client Controller for ESP32 Display
 *
 * Communicates with a remote STM32F4 peripheral controller via UART.
 * Provides the same interface as NimBLEClientController for compatibility.
 *
 * Protocol (line-v1 CSV format at 460800 baud):
 *
 * Commands sent to controller:
 *   CMD,OUT,valve,pumpSetpoint,heaterSetpoint
 *   CMD,ADV,valve,heaterSetpoint,pressureTarget,pressure,flow
 *   CMD,PING
 *   CMD,TARE
 *   CMD,SCALE_TARE
 *   CMD,SCALE_CAL,c1,c2,offset1,offset2
 *   CMD,SCALE_CAL_START,channel,refWeight
 *   CMD,LED,channel,brightness
 *   CMD,PID,Kp,Ki,Kd,Kf
 *   CMD,PUMP_COEFFS,a,b,c,d
 *   CMD,AUTOTUNE,goal,windowSize
 *   CMD,PRESSURE_SCALE,scale
 *
 * Events received from controller:
 *   EVT,SENSOR,temp,pressure,puckFlow,pumpFlow,resistance
 *   EVT,PING
 *   EVT,BTN_BREW,state
 *   EVT,BTN_STEAM,state
 *   EVT,ERR,code
 *   EVT,TOF,distance
 *   EVT,VOLUMETRIC,volume
 *   EVT,WEIGHT,grams
 *   EVT,AUTOTUNE,Kp,Ki,Kd,Kf
 *   EVT,SCALE_OFFSETS,offset1,offset2
 *   EVT,SCALE_CAL_RESULT,channel,calibration
 */
class UARTClientController : public ClientCommunicationHandler {
  public:
    // Matching NimBLEClientController interface
    static constexpr size_t BLE_SCAN_DURATION_SECONDS = 2; // Quick init for UART

    UARTClientController();

    void initClient();
    bool connectToServer();
    bool isConnected() const;
    bool isReadyForConnection() const;
    void scan() { connected = true; } // UART is always "ready"
    void poll();

    // Send commands
    void sendAdvancedOutputControl(bool valve, float boilerSetpoint, bool pressureTarget, float pressure, float flow);
    void sendOutputControl(bool valve, float pumpSetpoint, float boilerSetpoint);
    void sendAltControl(bool pinState);
    void sendPing();
    void sendAutotune(int testTime, int samples);
    void sendPidSettings(const String &pid);
    void sendPumpModelCoeffs(const String &pumpModelCoeffs);
    void setPressureScale(float scale);
    void sendLedControl(uint8_t channel, uint8_t brightness);
    void tare();
    void scaleTare();
    void sendScaleCalibration(float c1, float c2, long offset1, long offset2);
    void startScaleCalibration(uint8_t channel, float refWeight);

    // Register callbacks
    void registerRemoteErrorCallback(const remote_err_callback_t &callback);
    void registerBrewBtnCallback(const brew_callback_t &callback);
    void registerSteamBtnCallback(const steam_callback_t &callback);
    void registerSensorCallback(const sensor_read_callback_t &callback);
    void registerAutotuneResultCallback(const pid_control_callback_t &callback);
    void registerVolumetricMeasurementCallback(const float_callback_t &callback);
    void registerTofMeasurementCallback(const int_callback_t &callback);
    void registerWeightMeasurementCallback(const float_callback_t &callback);
    void registerScaleOffsetsCallback(const scale_offsets_callback_t &callback);
    void registerScaleCalResultCallback(const scale_cal_result_callback_t &callback);

    // Get info
    std::string readInfo() const;
    void *getClient() const { return nullptr; } // Not applicable for UART

  private:
    bool connected = false;
    std::string systemInfo; // Stores INFO received from STM32 at startup

    SemaphoreHandle_t _txMutex = nullptr;

    // ===== Callbacks =====
    remote_err_callback_t remoteErrorCallback = nullptr;
    brew_callback_t brewBtnCallback = nullptr;
    steam_callback_t steamBtnCallback = nullptr;
    sensor_read_callback_t sensorCallback = nullptr;
    pid_control_callback_t autotuneResultCallback = nullptr;
    float_callback_t volumetricMeasurementCallback = nullptr;
    int_callback_t tofMeasurementCallback = nullptr;
    float_callback_t weightMeasurementCallback = nullptr;
    scale_offsets_callback_t scaleOffsetsCallback = nullptr;
    scale_cal_result_callback_t scaleCalResultCallback = nullptr;

    // ===== Receive buffer and parsing =====
    static constexpr size_t RX_BUFFER_SIZE = 256;
    char rxBuffer[RX_BUFFER_SIZE];
    size_t rxIndex = 0;

    // ===== Helper methods =====
    void _processEvent(const char *line);
    void _processInfo(const char *jsonString);
    void _processEventSensor(char *args);
    void _processEventPing(char *args);
    void _processEventBtnBrew(char *args);
    void _processEventBtnSteam(char *args);
    void _processEventError(char *args);
    void _processEventTof(char *args);
    void _processEventVolumetric(char *args);
    void _processEventWeight(char *args);
    void _processEventScaleOffsets(char *args);
    void _processEventScaleCalResult(char *args);
    void _processEventAutotune(char *args);

    void _sendCommand(const char *format, ...);
    bool _getToken(char *input, uint8_t index, char separator, char *out, size_t outSize);
    float _parseFloat(const char *str);
    int _parseInt(const char *str);

    const char *LOG_TAG = "UARTClientController";
};

#endif
