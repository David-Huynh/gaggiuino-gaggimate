#ifndef UARTCOMM_H
#define UARTCOMM_H

#include "CommunicationHandler.h"
#include <Arduino.h>
#include <cstring>
#include <queue>

constexpr size_t UART_RX_BUFFER_SIZE = 256;
constexpr size_t UART_TX_BUFFER_SIZE = 256;
constexpr uint32_t UART_DEFAULT_BAUD = 460800;

// Error codes for UART protocol
constexpr int ERROR_CODE_RX_OVERFLOW = 3;
constexpr int ERROR_CODE_UNKNOWN_CMD = 4;

/**
 * @brief UART Communication handler for GaggiMate
 *
 * Implements the line-v1 CSV-based protocol for communicating with a remote
 * ESP32 or other controller. Protocol format:
 *
 * Commands (remote → controller):
 *   CMD,OUT,valve,pumpSetpoint,heaterSetpoint
 *   CMD,ADV,valve,heaterSetpoint,pressureTarget,pressure,flow
 *   CMD,PING
 *   CMD,TARE
 *   CMD,SCALE_TARE
 *   CMD,SCALE_CAL,c1,c2
 *   CMD,LED,channel,brightness
 *   CMD,PID,Kp,Ki,Kd,Kf
 *   CMD,PUMP_COEFFS,a,b,c,d
 *   CMD,AUTOTUNE,goal,windowSize
 *   CMD,PRESSURE_SCALE,scale
 *
 * Events (controller → remote):
 *   EVT,SENSOR,temp,pressure,puckFlow,pumpFlow,resistance
 *   EVT,PING
 *   EVT,BTN_BREW,state
 *   EVT,BTN_STEAM,state
 *   EVT,ERR,code
 *   EVT,TOF,distance
 *   EVT,VOLUMETRIC,volume
 *   EVT,WEIGHT,grams
 *   EVT,AUTOTUNE,Kp,Ki,Kd,Kf
 *   EVT,LED,OK
 */
class UARTComm : public CommunicationHandler {
  public:
    /**
     * @brief Construct UART communication handler
     * @param uartPort HardwareSerial port to use (e.g., Serial2)
     * @param baudRate Baud rate (default 460800)
     */
    UARTComm(HardwareSerial *uartPort, uint32_t baudRate = UART_DEFAULT_BAUD);

    /**
     * @brief Initialize communication (required by CommunicationHandler)
     * @param infoString System information string
     */
    void initServer(String infoString) override;

    // ===== Outbound methods (send data to remote) =====
    void sendSensorData(float temperature, float pressure, float puckFlow, float pumpFlow, float puckResistance) override;
    void sendError(int errorCode) override;
    void sendBrewBtnState(bool brewButtonStatus) override;
    void sendSteamBtnState(bool steamButtonStatus) override;
    void sendAutotuneResult(float Kp, float Ki, float Kd) override;
    void sendVolumetricMeasurement(float value) override;
    void sendTofMeasurement(int value) override;
    void sendWeightMeasurement(float weight) override;

    // ===== Callback registration (for receiving commands) =====
    void registerOutputControlCallback(const simple_output_callback_t &callback) override;
    void registerAdvancedOutputControlCallback(const advanced_output_callback_t &callback) override;
    void registerAltControlCallback(const pin_control_callback_t &callback) override;
    void registerPidControlCallback(const pid_control_callback_t &callback) override;
    void registerPumpModelCoeffsCallback(const pump_model_coeffs_callback_t &callback) override;
    void registerPingCallback(const ping_callback_t &callback) override;
    void registerAutotuneCallback(const autotune_callback_t &callback) override;
    void registerPressureScaleCallback(const float_callback_t &callback) override;
    void registerTareCallback(const void_callback_t &callback) override;
    void registerScaleTareCallback(const void_callback_t &callback) override;
    void registerScaleCalibrationCallback(const scale_calibration_callback_t &callback) override;
    void registerScaleCalStartCallback(const scale_cal_start_callback_t &callback) override;
    void sendScaleOffsets(long offset1, long offset2) override;
    void sendScaleCalResult(uint8_t channel, float calibration) override;
    void registerLedControlCallback(const led_control_callback_t &callback) override;
    void processQueue() override;
    /**
     * @brief Process incoming UART data (should be called frequently from main loop)
     */
    void update();

  private:
    QueueHandle_t _cmdQueue = nullptr;
    struct UARTCommand {
        enum Type {
            CMD_OUT,
            CMD_ADV,
            CMD_ALT,
            CMD_PID,
            CMD_PUMP_COEFFS,
            CMD_AUTOTUNE,
            CMD_PRESSURE_SCALE,
            CMD_LED,
            CMD_PING,
            CMD_TARE,
            CMD_SCALE_TARE,
            CMD_SCALE_CAL,
            CMD_SCALE_CAL_START
        };
        Type type;
        union {
            struct {
                bool valve;
                float pump;
                float heater;
            } out;
            struct {
                bool valve;
                float heater;
                bool pressureTarget;
                float pressure;
                float flow;
            } adv;
            struct {
                bool state;
            } alt;
            struct {
                float kp, ki, kd, kf;
            } pid;
            struct {
                float a, b, c, d;
            } pumpCoeffs;
            struct {
                int goal, window;
            } autotune;
            struct {
                float scale;
            } pressureScale;
            struct {
                uint8_t channel, brightness;
            } led;
            struct {
                float c1, c2;
                long offset1, offset2;
            } scaleCal;
            struct {
                uint8_t channel;
                float refWeight;
            } scaleCalStart;
        } data;
    };
    HardwareSerial *_uart = nullptr;
    uint32_t _baudRate = UART_DEFAULT_BAUD;
    TaskHandle_t _uartTaskHandle = nullptr;
    static void uartTaskFunction(void *param);
    void uartTaskLoop();

    // ===== Callbacks for handling commands =====
    simple_output_callback_t _outputControlCallback = nullptr;
    advanced_output_callback_t _advancedControlCallback = nullptr;
    pin_control_callback_t _altControlCallback = nullptr;
    pid_control_callback_t _pidControlCallback = nullptr;
    pump_model_coeffs_callback_t _pumpModelCoeffsCallback = nullptr;
    ping_callback_t _pingCallback = nullptr;
    autotune_callback_t _autotuneCallback = nullptr;
    float_callback_t _pressureScaleCallback = nullptr;
    void_callback_t _tareCallback = nullptr;
    void_callback_t _scaleTareCallback = nullptr;
    scale_calibration_callback_t _scaleCalibrationCallback = nullptr;
    scale_cal_start_callback_t _scaleCalStartCallback = nullptr;
    led_control_callback_t _ledControlCallback = nullptr;

    // ===== Receive buffer and parsing =====
    char _rxBuffer[UART_RX_BUFFER_SIZE];
    size_t _rxIndex = 0;

    // ===== Helper methods =====
    void _processCommand(const char *line);
    void _sendLine(const char *format, ...);
    char *_getToken(char *input, uint8_t index, char separator);
    float _parseFloat(const char *str);
    int _parseInt(const char *str);

    const char *LOG_TAG = "UARTComm";
};

#endif // UARTCOMM_H
