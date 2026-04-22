#ifndef GAGGIMATECONTROLLER_H
#define GAGGIMATECONTROLLER_H
#include "CommunicationHandler.h"
#include "ControllerConfig.h"
#include <memory>
#include <peripherals/DigitalInput.h>
#include <peripherals/DistanceSensor.h>
#include <peripherals/Heater.h>
#ifdef ARDUINO_ARCH_STM32
#include <peripherals/HX711Scale.h>
#include <peripherals/LedController2.h>
#else
#include <peripherals/LedController.h>
#endif
#include <peripherals/Max31855Thermocouple.h>
#include <peripherals/PressureSensor.h>
#include <peripherals/Pump.h>
#include <peripherals/SimpleRelay.h>
#include <vector>

constexpr double PING_TIMEOUT_SECONDS = 20.0;

constexpr int DETECT_EN_PIN = 40;
constexpr int DETECT_VALUE_PIN = 11;

class GaggiMateController {
  public:
    /**
     * @brief Construct GaggiMateController with optional custom communication handler
     * @param version Firmware version string
     * @param commHandler Optional communication handler (BLE, UART, etc). If nullptr, defaults to NimBLEServerController
     */
    GaggiMateController(String version, CommunicationHandler *commHandler = nullptr);
    void setup(void);
    void loop(void);

    void registerBoardConfig(ControllerConfig config);

  private:
    void detectBoard();
    void detectAddon();
    void handlePing();
    void handlePingTimeout(void);
    void thermalRunawayShutdown(void);
    void startPidAutotune(void);
    void stopPidAutotune(void);
    void sendSensorData(void);

    ControllerConfig _config = ControllerConfig{};
    CommunicationHandler *_comm = nullptr;
    bool _comm_owned = false; // Track if we allocated _comm and should delete it

    Max31855Thermocouple *thermocouple = nullptr;
    Heater *heater = nullptr;
    SimpleRelay *valve = nullptr;
    SimpleRelay *alt = nullptr;
    Pump *pump = nullptr;
    DigitalInput *brewBtn = nullptr;
    DigitalInput *steamBtn = nullptr;
    PressureSensor *pressureSensor = nullptr;
#ifdef ARDUINO_ARCH_STM32
    HX711Scale *scale = nullptr;
#endif
    LedController *ledController = nullptr;
    DistanceSensor *distanceSensor = nullptr;

    std::vector<ControllerConfig> configs;

    String _version;
    unsigned long lastPingTime = 0;
    size_t errorState = ERROR_CODE_NONE;

    const char *LOG_TAG = "GaggiMateController";
};

#endif // GAGGIMATECONTROLLER_H
