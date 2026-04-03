#ifndef NIMBLESERVERCONTROLLER_H
#define NIMBLESERVERCONTROLLER_H

#include "CommunicationHandler.h"
#include "NimBLEComm.h"
#include "cstring"
#include <ble_ota_dfu.hpp>

class NimBLEServerController : public CommunicationHandler, public NimBLEServerCallbacks, public NimBLECharacteristicCallbacks {
  public:
    NimBLEServerController();
    void initServer(String infoString);
    void loop();

    void sendSensorData(float temperature, float pressure, float puckFlow, float pumpFlow, float puckResistance);
    void sendError(int errorCode);
    void sendBrewBtnState(bool brewButtonStatus);
    void sendSteamBtnState(bool steamButtonStatus);
    void sendAutotuneResult(float Kp, float Ki, float Kd);
    void sendVolumetricMeasurement(float value);
    void sendTofMeasurement(int value);
    void sendWeightMeasurement(float weight);
    void sendScaleOffsets(long offset1, long offset2) override;
    void sendScaleCalResult(uint8_t channel, float calibration) override;
    void registerOutputControlCallback(const simple_output_callback_t &callback);
    void registerAdvancedOutputControlCallback(const advanced_output_callback_t &callback);
    void registerAltControlCallback(const pin_control_callback_t &callback);
    void registerPidControlCallback(const pid_control_callback_t &callback);
    void registerPumpModelCoeffsCallback(const pump_model_coeffs_callback_t &callback);
    void registerPingCallback(const ping_callback_t &callback);
    void registerAutotuneCallback(const autotune_callback_t &callback);
    void registerPressureScaleCallback(const float_callback_t &callback) override;
    void registerTareCallback(const void_callback_t &callback) override;
    void registerScaleTareCallback(const void_callback_t &callback) override;
    void registerScaleCalibrationCallback(const scale_calibration_callback_t &callback) override;
    void registerScaleCalStartCallback(const scale_cal_start_callback_t &callback) override;
    void registerLedControlCallback(const led_control_callback_t &callback) override;
    void setInfo(String infoString);

  private:
    bool deviceConnected = false;
    String infoString = "";
    NimBLEAdvertising *advertising = nullptr;
    NimBLEServer *server = nullptr;

    NimBLECharacteristic *outputControlChar = nullptr;
    NimBLECharacteristic *pressureScaleChar = nullptr;
    NimBLECharacteristic *altControlChar = nullptr;
    NimBLECharacteristic *pingChar = nullptr;
    NimBLECharacteristic *pidControlChar = nullptr;
    NimBLECharacteristic *pumpModelCoeffsChar = nullptr;
    NimBLECharacteristic *errorChar = nullptr;
    NimBLECharacteristic *autotuneChar = nullptr;
    NimBLECharacteristic *autotuneResultChar = nullptr;
    NimBLECharacteristic *brewBtnChar = nullptr;
    NimBLECharacteristic *steamBtnChar = nullptr;
    NimBLECharacteristic *infoChar = nullptr;
    NimBLECharacteristic *sensorChar = nullptr;
    NimBLECharacteristic *volumetricMeasurementChar = nullptr;
    NimBLECharacteristic *volumetricTareChar = nullptr;
    NimBLECharacteristic *scaleTareChar = nullptr;
    NimBLECharacteristic *scaleCalibrationChar = nullptr;
    NimBLECharacteristic *scaleOffsetsChar = nullptr;
    NimBLECharacteristic *scaleCalStartChar = nullptr;
    NimBLECharacteristic *scaleCalResultChar = nullptr;
    NimBLECharacteristic *tofMeasurementChar = nullptr;
    NimBLECharacteristic *weightMeasurementChar = nullptr;
    NimBLECharacteristic *ledControlChar = nullptr;

    simple_output_callback_t outputControlCallback = nullptr;
    advanced_output_callback_t advancedControlCallback = nullptr;
    pin_control_callback_t altControlCallback = nullptr;
    pid_control_callback_t pidControlCallback = nullptr;
    pump_model_coeffs_callback_t pumpModelCoeffsCallback = nullptr;
    ping_callback_t pingCallback = nullptr;
    autotune_callback_t autotuneCallback = nullptr;
    float_callback_t pressureScaleCallback = nullptr;
    void_callback_t tareCallback = nullptr;
    void_callback_t scaleTareCallback = nullptr;
    scale_calibration_callback_t scaleCalibrationCallback = nullptr;
    scale_cal_start_callback_t scaleCalStartCallback = nullptr;
    led_control_callback_t ledControlCallback = nullptr;
    char sensorDataBuffer[80]{};
    char errorBuffer[12]{};
    char brewBtnBuffer[4]{};
    char steamBtnBuffer[4]{};
    char autotuneResultBuffer[64]{};
    char tofBuffer[16]{};
    char volumetricBuffer[16]{};

    // BLEServerCallbacks overrides
    void onConnect(NimBLEServer *pServer) override;
    void onDisconnect(NimBLEServer *pServer) override;

    // BLECharacteristicCallbacks overrides
    void onWrite(NimBLECharacteristic *pCharacteristic) override;

    BLE_OTA_DFU ota_dfu_ble;

    const char *LOG_TAG = "NimBLEClientController";
    xTaskHandle taskHandle;
    static void loopTask(void *arg);
};

#endif // NIMBLESERVERCONTROLLER_H
