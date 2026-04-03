#ifndef COMMON_TYPES_H
#define COMMON_TYPES_H

#include <Arduino.h>
#include <functional>

// ========== Error codes (from CommunicationHandler.h) ==========
constexpr size_t ERROR_CODE_NONE = 0;
constexpr size_t ERROR_CODE_COMM_SEND = 1;
constexpr size_t ERROR_CODE_COMM_RCV = 2;
constexpr size_t ERROR_CODE_PROTO_ERR = 3;
constexpr size_t ERROR_CODE_RUNAWAY = 4;
constexpr size_t ERROR_CODE_TIMEOUT = 5;

// ========== System capabilities & info ==========
struct SystemCapabilities {
    bool dimming = false;
    bool pressure = false;
    bool ledControl = false;
    bool tof = false;
};

struct SystemInfo {
    String hardware;
    String version;
    SystemCapabilities capabilities;
};

// ========== Callback type aliases (matching both UART and BLE) ==========
using brew_callback_t = std::function<void(bool brewButtonStatus)>;
using steam_callback_t = std::function<void(bool steamButtonStatus)>;
using pin_control_callback_t = std::function<void(bool isActive)>;
using pid_control_callback_t = std::function<void(float Kp, float Ki, float Kd, float Kf)>;
using pump_model_coeffs_callback_t = std::function<void(float a, float b, float c, float d)>;
using ping_callback_t = std::function<void()>;
using remote_err_callback_t = std::function<void(int errorCode)>;
using autotune_callback_t = std::function<void(int testTime, int samples)>;
// Sent from display → controller at startup: restore saved cal coefficients + tare offsets
using scale_calibration_callback_t = std::function<void(float c1, float c2, long offset1, long offset2)>;
// Sent from display → controller to start per-channel calibration with a known reference weight
using scale_cal_start_callback_t = std::function<void(uint8_t channel, float refWeight)>;
// Sent from controller → display: result of per-channel calibration (which channel, its new factor)
using scale_cal_result_callback_t = std::function<void(uint8_t channel, float calibration)>;
// Sent from controller → display after a tare: new raw ADC zero-offsets for both channels
using scale_offsets_callback_t = std::function<void(long offset1, long offset2)>;
using void_callback_t = std::function<void()>;
using float_callback_t = std::function<void(float val)>;
using int_callback_t = std::function<void(int val)>;
using simple_output_callback_t = std::function<void(bool valve, float pumpSetpoint, float boilerSetpoint)>;
using advanced_output_callback_t =
    std::function<void(bool valve, float boilerSetpoint, bool pressureTarget, float pumpPressure, float pumpFlow)>;
using sensor_read_callback_t =
    std::function<void(float temperature, float pressure, float puckFlow, float pumpFlow, float puckResistance)>;
using led_control_callback_t = std::function<void(uint8_t channel, uint8_t brightness)>;

#endif // COMMON_TYPES_H