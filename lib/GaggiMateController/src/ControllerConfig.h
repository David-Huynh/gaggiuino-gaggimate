#ifndef CONTROLLERCONFIG_H
#define CONTROLLERCONFIG_H
#include "peripherals/NtcTiming.h"
#include "peripherals/PressureTiming.h"
#include <array>
#include <string>

#if defined(GAGGIMATE_DISABLE_HARDWARE_SCALE)
#define GAGGIMATE_HARDWARE_SCALE_ENABLED false
#else
#define GAGGIMATE_HARDWARE_SCALE_ENABLED true
#endif

struct Capabilities {
    bool dimming = false;
    bool pressure = false;
    bool ssrPump = false;
    bool ledControls = false;
    bool tof = false;
    bool dualBoiler = false;
    bool scale = false;
};

struct ControllerConfig {
    std::string name;

    // The autodetect value that is measured through a PCB voltage divider.
    // The detected value in milli volts is divided by 100 and rounded.
    uint16_t autodetectValue;

    uint8_t heaterPin;
    uint8_t altPin;
    uint8_t altOn;

    uint8_t pumpPin;
    uint8_t pumpSensePin = 0;
    uint8_t pumpOn;
    uint8_t valvePin;
    uint8_t valveOn;
    // Refill relay for solenoid / pump
    uint8_t refillPin = 0;
    // Auxiliary relay for solenoid / pump
    uint8_t auxPin = 0;

    uint8_t waterSensePin = 0;
    uint8_t tankLevelPin = 0;
    uint8_t ledPin = 0;
    uint8_t baristaLedPin = 0;

    uint8_t pressureScl = 0;
    uint8_t pressureSda = 0;

    uint8_t maxSckPin = 0;
    uint8_t maxCsPin = 0;
    uint8_t maxMisoPin = 0;

    uint8_t brewButtonPin;
    uint8_t steamButtonPin;
    uint8_t waterButtonPin = 0;

    uint8_t scaleClkPin;
    uint8_t scaleDat0Pin;
    uint8_t scaleDat1Pin;

    uint8_t sunriseSclPin;
    uint8_t sunriseSdaPin;

    uint8_t ext1Pin;
    uint8_t ext2Pin;
    uint8_t ext3Pin;
    uint8_t ext4Pin;
    uint8_t ext5Pin;

    Capabilities capabilites;
    // ADC channels 0..3 in SPS; zero disables the channel. Total <= 570.
    std::array<uint16_t, 4> adcRates = {0, 0, 0, 0};
    PressureControlRate pressureControlRate = PressureControlRate::Hz60;
    // Per ADS channel; only used where an NTC is instantiated.
    std::array<NtcTiming, 4> ntcTiming{};
};

const ControllerConfig GM_STANDARD_REV_1X = {.name = "GaggiMate Standard Rev 1.x",
                                             .autodetectValue = 0, // Voltage divider was missing in Rev 1.0 so it's 0
                                             .heaterPin = 14,
                                             .altPin = 11,
                                             .altOn = 1,
                                             .pumpPin = 9,
                                             .pumpOn = 1,
                                             .valvePin = 10,
                                             .valveOn = 1,
                                             .maxSckPin = 6,
                                             .maxCsPin = 7,
                                             .maxMisoPin = 4,
                                             .brewButtonPin = 38,
                                             .steamButtonPin = 48,
                                             .scaleClkPin = 17,
                                             .scaleDat0Pin = 18,
                                             .scaleDat1Pin = 39,
                                             .ext1Pin = 1,
                                             .ext2Pin = 2,
                                             .ext3Pin = 8,
                                             .ext4Pin = 12,
                                             .ext5Pin = 13,
                                             .capabilites = {},
                                             .adcRates = {0, 0, 0, 0}};

const ControllerConfig GM_STANDARD_REV_2X = {.name = "GaggiMate Standard Rev 2.x",
                                             .autodetectValue = 1,
                                             .heaterPin = 14,
                                             .altPin = 47,
                                             .altOn = 1,
                                             .pumpPin = 9,
                                             .pumpOn = 1,
                                             .valvePin = 10,
                                             .valveOn = 1,
                                             .maxSckPin = 6,
                                             .maxCsPin = 7,
                                             .maxMisoPin = 4,
                                             .brewButtonPin = 38,
                                             .steamButtonPin = 48,
                                             .scaleClkPin = 17,
                                             .scaleDat0Pin = 18,
                                             .scaleDat1Pin = 39,
                                             .sunriseSclPin = 44,
                                             .sunriseSdaPin = 43,
                                             .ext1Pin = 1,
                                             .ext2Pin = 2,
                                             .ext3Pin = 8,
                                             .ext4Pin = 12,
                                             .ext5Pin = 13,
                                             .capabilites =
                                                 {
                                                 .ssrPump = true,
                                                 },
                                             .adcRates = {0, 0, 0, 0}};

const ControllerConfig GM_PRO_REV_1x = {.name = "GaggiMate Pro Rev 1.0",
                                        .autodetectValue = 2,
                                        .heaterPin = 14,
                                        .altPin = 47,
                                        .altOn = 1,
                                        .pumpPin = 9,
                                        .pumpSensePin = 21,
                                        .pumpOn = 1,
                                        .valvePin = 10,
                                        .valveOn = 1,
                                        .pressureScl = 41,
                                        .pressureSda = 42,
                                        .maxSckPin = 6,
                                        .maxCsPin = 7,
                                        .maxMisoPin = 4,
                                        .brewButtonPin = 38,
                                        .steamButtonPin = 48,
                                        .scaleClkPin = 17,
                                        .scaleDat0Pin = 18,
                                        .scaleDat1Pin = 39,
                                        .sunriseSclPin = 44,
                                        .sunriseSdaPin = 43,
                                        .ext1Pin = 1,
                                        .ext2Pin = 2,
                                        .ext3Pin = 8,
                                        .ext4Pin = 12,
                                        .ext5Pin = 13,
                                        .capabilites =
                                            {
                                            .dimming = true,
                                            .pressure = true,
                                            },
                                        .adcRates = {500, 0, 0, 0},
                                        .pressureControlRate = PressureControlRate::Hz60};

const ControllerConfig GM_PRO_LEGO = {.name = "GaggiMate Pro Lego Build",
                                      .autodetectValue = 3,
                                      .heaterPin = 14,
                                      .altPin = 47,
                                      .altOn = 1,
                                      .pumpPin = 9,
                                      .pumpSensePin = 21,
                                      .pumpOn = 1,
                                      .valvePin = 10,
                                      .valveOn = 1,
                                      .pressureScl = 41,
                                      .pressureSda = 42,
                                      .maxSckPin = 6,
                                      .maxCsPin = 7,
                                      .maxMisoPin = 4,
                                      .brewButtonPin = 38,
                                      .steamButtonPin = 48,
                                      .scaleClkPin = 17,
                                      .scaleDat0Pin = 18,
                                      .scaleDat1Pin = 39,
                                      .sunriseSclPin = 44,
                                      .sunriseSdaPin = 43,
                                      .ext1Pin = 1,
                                      .ext2Pin = 2,
                                      .ext3Pin = 8,
                                      .ext4Pin = 12,
                                      .ext5Pin = 13,
                                      .capabilites =
                                          {
                                          .dimming = true,
                                          .pressure = true,
                                          },
                                      .adcRates = {500, 0, 0, 0},
                                      .pressureControlRate = PressureControlRate::Hz60};

const ControllerConfig GM_PRO_REV_11 = {.name = "GaggiMate Pro Rev 1.1",
                                        .autodetectValue = 4,
                                        .heaterPin = 14,
                                        .altPin = 47,
                                        .altOn = 1,
                                        .pumpPin = 9,
                                        .pumpSensePin = 21,
                                        .pumpOn = 1,
                                        .valvePin = 10,
                                        .valveOn = 1,
                                        .pressureScl = 41,
                                        .pressureSda = 42,
                                        .maxSckPin = 6,
                                        .maxCsPin = 7,
                                        .maxMisoPin = 4,
                                        .brewButtonPin = 38,
                                        .steamButtonPin = 48,
                                        .scaleClkPin = 17,
                                        .scaleDat0Pin = 18,
                                        .scaleDat1Pin = 39,
                                        .sunriseSclPin = 44,
                                        .sunriseSdaPin = 43,
                                        .ext1Pin = 1,
                                        .ext2Pin = 2,
                                        .ext3Pin = 8,
                                        .ext4Pin = 12,
                                        .ext5Pin = 13,
                                        .capabilites =
                                            {
                                            .dimming = true,
                                            .pressure = true,
                                            },
                                        .adcRates = {500, 0, 0, 0},
                                        .pressureControlRate = PressureControlRate::Hz60};

// STM32F4 Controller - replaces peripheral ESP32, communicates via UART
// Source of truth: gaggiuino/src/pindef.h
// Arduino STM32 mapping (BlackPill F411CE variant):
// PA0-PA15=0-15, PB0-PB10=16-26, PB12-PB15=27-30, PC13-PC15=31-33
const ControllerConfig GM_STM32F4_V1 = {.name = "GaggiMate STM32F4 Controller v1.0",
                                        .autodetectValue = 99, // Not used in STM32 (no voltage divider), unique ID for reference

                                        // Heater SSR relay: PA15 (relayPin in Gaggiuino pindef)
                                        .heaterPin = 15, // Arduino pin for PA15 on STM32F4

                                        // Auxiliary relay: not available on standard Gaggiuino PCB
                                        .altPin = 0,
                                        .altOn = 1,

                                        // Pump PWM output: PA1
                                        .pumpPin = 1,      // Arduino pin for PA1 on STM32F4
                                        .pumpSensePin = 0, // Not used on this board (kept for backward compatibility)
                                        .pumpOn = 1,       // Active high

                                        // Valve relay: PC13 (also onboard LED on BlackPill)
                                        .valvePin = 31, // Arduino pin for PC13 on BlackPill F411CE
                                        .valveOn = 1,   // Active high

                                        // I2C1 for pressure sensor (PB6=SCL, PB7=SDA)
                                        .pressureScl = 22, // Arduino pin for PB6
                                        .pressureSda = 23, // Arduino pin for PB7

                                        // SPI1 for thermocouple MAX31855 (PA5=SCK, PA7=MOSI, PB4=MISO, PA6=CS)
                                        .maxSckPin = 5,   // Arduino pin for PA5
                                        .maxCsPin = 6,    // Arduino pin for PA6
                                        .maxMisoPin = 20, // Arduino pin for PB4

                                        // Brew button: PC14
                                        .brewButtonPin = 32, // Arduino pin for PC14 on BlackPill F411CE

                                        // Steam button: PC15
                                        .steamButtonPin = 33, // Arduino pin for PC15 on BlackPill F411CE

                                        // HX711: SCK=PB0, DOUT1=PB8, DOUT2=PB9
                                        .scaleClkPin = 16,  // Arduino pin for PB0
                                        .scaleDat0Pin = 24,  // Arduino pin for PB8
                                        .scaleDat1Pin = 25, // Arduino pin for PB9

                                        // LED/TOF I2C (same I2C1: PB6=SCL, PB7=SDA)
                                        .sunriseSclPin = 22, // Arduino pin for PB6 (I2C1 SCL)
                                        .sunriseSdaPin = 23, // Arduino pin for PB7 (I2C1 SDA)

                                        // Unused extension pins (placeholder for future use)
                                        .ext1Pin = 0,
                                        .ext2Pin = 0,
                                        .ext3Pin = 0,
                                        .ext4Pin = 0,
                                        .ext5Pin = 0,

                                        // Full capabilities with UART communication
                                        .capabilites = {
                                            .dimming = true,     // Supports dimmed pump control
                                            .pressure = true,    // Supports pressure sensor
                                            .ssrPump = false,    // Uses dimmed pump, not SSR
                                            .ledControls = true, // Supports LED control
                                            .tof = true,         // Supports time-of-flight sensor
                                            .scale = GAGGIMATE_HARDWARE_SCALE_ENABLED, // Supports HX711 hardware scale
                                        }};


const ControllerConfig GM_STANDARD_REV_3X = {.name = "GaggiMate Standard Rev 3.x",
                                             .autodetectValue = 6,
                                             .heaterPin = 14,
                                             .altPin = 47,
                                             .altOn = 1,
                                             .pumpPin = 9,
                                             .pumpOn = 1,
                                             .valvePin = 10,
                                             .valveOn = 1,
                                             .maxSckPin = 6,
                                             .maxCsPin = 7,
                                             .maxMisoPin = 4,
                                             .brewButtonPin = 38,
                                             .steamButtonPin = 48,
                                             .scaleClkPin = 17,
                                             .scaleDat0Pin = 18,
                                             .scaleDat1Pin = 39,
                                             .sunriseSclPin = 44,
                                             .sunriseSdaPin = 43,
                                             .ext1Pin = 1,
                                             .ext2Pin = 2,
                                             .ext3Pin = 8,
                                             .ext4Pin = 12,
                                             .ext5Pin = 13,
                                             .capabilites =
                                                 {
                                                 .ssrPump = true,
                                                 },
                                             .adcRates = {0, 0, 0, 0}};

const ControllerConfig GM_MAX_REV10 = {.name = "GaggiMate Max Rev 1.x",
                                       .autodetectValue = 5,
                                       .heaterPin = 12,
                                       .altPin = 10,
                                       .altOn = 1,
                                       .pumpPin = 41,
                                       .pumpSensePin = 42,
                                       .pumpOn = 1,
                                       .valvePin = 13,
                                       .valveOn = 1,
                                       .refillPin = 21,
                                       .auxPin = 14,
                                       .waterSensePin = 38,
                                       .tankLevelPin = 17,
                                       .ledPin = 9,
                                       .baristaLedPin = 1,
                                       .pressureScl = 47,
                                       .pressureSda = 48,
                                       .brewButtonPin = 6,
                                       .steamButtonPin = 4,
                                       .waterButtonPin = 5,
                                       .scaleClkPin = 7,
                                       .scaleDat0Pin = 15,
                                       .scaleDat1Pin = 16,
                                       .sunriseSclPin = 8,
                                       .sunriseSdaPin = 18,
                                       .ext1Pin = 2,
                                       .ext2Pin = 47,
                                       .ext3Pin = 48,
                                       .ext4Pin = 44,
                                       .ext5Pin = 43,
                                       .capabilites =
                                           {
                                               .dimming = true,
                                               .pressure = true,
                                               .dualBoiler = true,
                                           },
                                       .adcRates = {500, 0, 35, 35},
                                       .pressureControlRate = PressureControlRate::Hz60,
                                       .ntcTiming = {{{}, {}, {100, 1120.355f}, {100, 1120.355f}}}};

#endif // CONTROLLERCONFIG_H
