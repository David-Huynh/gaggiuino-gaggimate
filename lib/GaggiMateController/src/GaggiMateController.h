#ifndef GAGGIMATECONTROLLER_H
#define GAGGIMATECONTROLLER_H
#include "ControllerConfig.h"
#include "GaggiMateServer.h"
#include <peripherals/DigitalInput.h>
#include <peripherals/DistanceSensor.h>
#include <peripherals/FlowSensor.h>
#include <peripherals/Heater.h>
#ifdef ARDUINO_ARCH_STM32
#ifndef GAGGIMATE_DISABLE_HARDWARE_SCALE
#include <peripherals/HX711Scale.h>
#endif
#include <peripherals/LedController2.h>
#else
#include <peripherals/LedController.h>
#endif
#include <peripherals/Max31855Thermocouple.h>
#include <peripherals/PressureSensor.h>
#include <peripherals/Pump.h>
#include <peripherals/SimpleRelay.h>
#include <peripherals/addons/GearpumpAddon.h>
#include <vector>

constexpr double PING_TIMEOUT_SECONDS = 20.0;

constexpr int DETECT_EN_PIN = 40;
constexpr int DETECT_VALUE_PIN = 11;

class GaggiMateController {
  public:
    GaggiMateController(String version);
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
    void handleSerialCommand(char c);
#if defined(GAGGIMATE_UART_DIAGNOSTICS)
    ControllerDiagnostics buildControllerDiagnostics(void);
#endif

    ControllerConfig _config = ControllerConfig{};
    GaggiMateServer _comms;

    Max31855Thermocouple *thermocouple = nullptr;
    Heater *heater = nullptr;
    SimpleRelay *valve = nullptr;
    SimpleRelay *alt = nullptr;
    Pump *pump = nullptr;
    DigitalInput *brewBtn = nullptr;
    DigitalInput *steamBtn = nullptr;
    PressureSensor *pressureSensor = nullptr;
#if defined(ARDUINO_ARCH_STM32) && !defined(GAGGIMATE_DISABLE_HARDWARE_SCALE)
    HX711Scale *scale = nullptr;
#endif
    LedController *ledController = nullptr;
    DistanceSensor *distanceSensor = nullptr;
    ADSAdc *adc = nullptr;
    FlowSensor *flowSensor = nullptr;

    GearpumpAddon *gearpumpAddon = nullptr;

#ifndef ARDUINO_ARCH_STM32
    SoftWire *albaComms = nullptr;
#endif

    std::vector<ControllerConfig> configs;

    String _version;
    unsigned long lastPingTime = 0;
    size_t errorState = ERROR_CODE_NONE;
#if defined(GAGGIMATE_UART_DIAGNOSTICS)
    uint32_t boilerCommandCount = 0;
    uint32_t pumpCommandCount = 0;
    uint32_t relayCommandCount = 0;
    uint32_t pingCommandCount = 0;
    uint32_t tareCommandCount = 0;
    float lastBoilerSetpoint = 0.0f;
    float lastPumpPower = 0.0f;
    bool lastRelayOpen = false;
#endif

    const char *LOG_TAG = "GaggiMateController";
};

#endif // GAGGIMATECONTROLLER_H
