#ifndef CONTROLLERCONFIG_H
#define CONTROLLERCONFIG_H
#include <string>

struct Capabilities {
    bool dimming;
    bool pressure;
    bool ssrPump;
    bool ledControls;
    bool tof;
    bool scale;
};

struct ControllerConfig {
    std::string name;

    // The autodetect value that is measured through a PCB voltage divider.
    // The detected value in milli volts is divided by 100 and rounded.
    uint16_t autodetectValue;

    uint8_t heaterPin;
    uint8_t pumpPin;
    uint8_t pumpSensePin = 0;
    uint8_t pumpOn;
    uint8_t valvePin;
    uint8_t valveOn;
    uint8_t altPin;
    uint8_t altOn;

    uint8_t pressureScl = 0;
    uint8_t pressureSda = 0;

    uint8_t maxSckPin;
    uint8_t maxCsPin;
    uint8_t maxMisoPin;

    uint8_t brewButtonPin;
    uint8_t steamButtonPin;

    uint8_t scaleSclPin;
    uint8_t scaleSdaPin;
    uint8_t scaleSda1Pin;

    uint8_t sunriseSclPin;
    uint8_t sunriseSdaPin;

    uint8_t ext1Pin;
    uint8_t ext2Pin;
    uint8_t ext3Pin;
    uint8_t ext4Pin;
    uint8_t ext5Pin;

    Capabilities capabilites;
};

const ControllerConfig GM_STANDARD_REV_1X = {.name = "GaggiMate Standard Rev 1.x",
                                             .autodetectValue = 0, // Voltage divider was missing in Rev 1.0 so it's 0
                                             .heaterPin = 14,
                                             .pumpPin = 9,
                                             .pumpOn = 1,
                                             .valvePin = 10,
                                             .valveOn = 1,
                                             .altPin = 11,
                                             .altOn = 1,
                                             .maxSckPin = 6,
                                             .maxCsPin = 7,
                                             .maxMisoPin = 4,
                                             .brewButtonPin = 38,
                                             .steamButtonPin = 48,
                                             .scaleSclPin = 17,
                                             .scaleSdaPin = 18,
                                             .scaleSda1Pin = 39,
                                             .ext1Pin = 1,
                                             .ext2Pin = 2,
                                             .ext3Pin = 8,
                                             .ext4Pin = 12,
                                             .ext5Pin = 13,
                                             .capabilites = {
                                                 .dimming = false,
                                                 .pressure = false,
                                                 .ssrPump = false,
                                                 .ledControls = false,
                                                 .tof = false,
                                                 .scale = false,
                                             }};

const ControllerConfig GM_STANDARD_REV_2X = {.name = "GaggiMate Standard Rev 2.x",
                                             .autodetectValue = 1,
                                             .heaterPin = 14,
                                             .pumpPin = 9,
                                             .pumpOn = 1,
                                             .valvePin = 10,
                                             .valveOn = 1,
                                             .altPin = 47,
                                             .altOn = 1,
                                             .maxSckPin = 6,
                                             .maxCsPin = 7,
                                             .maxMisoPin = 4,
                                             .brewButtonPin = 38,
                                             .steamButtonPin = 48,
                                             .scaleSclPin = 17,
                                             .scaleSdaPin = 18,
                                             .scaleSda1Pin = 39,
                                             .sunriseSclPin = 44,
                                             .sunriseSdaPin = 43,
                                             .ext1Pin = 1,
                                             .ext2Pin = 2,
                                             .ext3Pin = 8,
                                             .ext4Pin = 12,
                                             .ext5Pin = 13,
                                             .capabilites = {
                                                 .dimming = false,
                                                 .pressure = false,
                                                 .ssrPump = true,
                                                 .ledControls = false,
                                                 .tof = false,
                                                 .scale = false,
                                             }};

const ControllerConfig GM_PRO_REV_1x = {.name = "GaggiMate Pro Rev 1.0",
                                        .autodetectValue = 2,
                                        .heaterPin = 14,
                                        .pumpPin = 9,
                                        .pumpSensePin = 21,
                                        .pumpOn = 1,
                                        .valvePin = 10,
                                        .valveOn = 1,
                                        .altPin = 47,
                                        .altOn = 1,
                                        .pressureScl = 41,
                                        .pressureSda = 42,
                                        .maxSckPin = 6,
                                        .maxCsPin = 7,
                                        .maxMisoPin = 4,
                                        .brewButtonPin = 38,
                                        .steamButtonPin = 48,
                                        .scaleSclPin = 17,
                                        .scaleSdaPin = 18,
                                        .scaleSda1Pin = 39,
                                        .sunriseSclPin = 44,
                                        .sunriseSdaPin = 43,
                                        .ext1Pin = 1,
                                        .ext2Pin = 2,
                                        .ext3Pin = 8,
                                        .ext4Pin = 12,
                                        .ext5Pin = 13,
                                        .capabilites = {
                                            .dimming = true,
                                            .pressure = true,
                                            .ssrPump = false,
                                            .ledControls = false,
                                            .tof = false,
                                            .scale = false,
                                        }};

const ControllerConfig GM_PRO_LEGO = {.name = "GaggiMate Pro Lego Build",
                                      .autodetectValue = 3,
                                      .heaterPin = 14,
                                      .pumpPin = 9,
                                      .pumpSensePin = 21,
                                      .pumpOn = 1,
                                      .valvePin = 10,
                                      .valveOn = 1,
                                      .altPin = 47,
                                      .altOn = 1,
                                      .pressureScl = 41,
                                      .pressureSda = 42,
                                      .maxSckPin = 6,
                                      .maxCsPin = 7,
                                      .maxMisoPin = 4,
                                      .brewButtonPin = 38,
                                      .steamButtonPin = 48,
                                      .scaleSclPin = 17,
                                      .scaleSdaPin = 18,
                                      .scaleSda1Pin = 39,
                                      .sunriseSclPin = 44,
                                      .sunriseSdaPin = 43,
                                      .ext1Pin = 1,
                                      .ext2Pin = 2,
                                      .ext3Pin = 8,
                                      .ext4Pin = 12,
                                      .ext5Pin = 13,
                                      .capabilites = {
                                          .dimming = true,
                                          .pressure = true,
                                          .ssrPump = false,
                                          .ledControls = false,
                                          .tof = false,
                                          .scale = false,
                                      }};

const ControllerConfig GM_PRO_REV_11 = {.name = "GaggiMate Pro Rev 1.1",
                                        .autodetectValue = 4,
                                        .heaterPin = 14,
                                        .pumpPin = 9,
                                        .pumpSensePin = 21,
                                        .pumpOn = 1,
                                        .valvePin = 10,
                                        .valveOn = 1,
                                        .altPin = 47,
                                        .altOn = 1,
                                        .pressureScl = 41,
                                        .pressureSda = 42,
                                        .maxSckPin = 6,
                                        .maxCsPin = 7,
                                        .maxMisoPin = 4,
                                        .brewButtonPin = 38,
                                        .steamButtonPin = 48,
                                        .scaleSclPin = 17,
                                        .scaleSdaPin = 18,
                                        .scaleSda1Pin = 39,
                                        .sunriseSclPin = 44,
                                        .sunriseSdaPin = 43,
                                        .ext1Pin = 1,
                                        .ext2Pin = 2,
                                        .ext3Pin = 8,
                                        .ext4Pin = 12,
                                        .ext5Pin = 13,
                                        .capabilites = {
                                            .dimming = true,
                                            .pressure = true,
                                            .ssrPump = false,
                                            .ledControls = false,
                                            .tof = false,
                                            .scale = false,
                                        }};

// STM32F4 Controller - replaces peripheral ESP32, communicates via UART
// Source of truth: gaggiuino/src/pindef.h
// Arduino STM32 mapping: PA0-PA15=0-15, PB0-PB15=16-31, PC13-PC15=45-47
const ControllerConfig GM_STM32F4_V1 = {.name = "GaggiMate STM32F4 Controller v1.0",
                                        .autodetectValue = 99, // Not used in STM32 (no voltage divider), unique ID for reference

                                        // Heater PWM output: PB1
                                        .heaterPin = 17, // Arduino pin for PB1 on STM32F4

                                        // Pump PWM output: PA1
                                        .pumpPin = 1,      // Arduino pin for PA1 on STM32F4
                                        .pumpSensePin = 0, // Not used on this board (kept for backward compatibility)
                                        .pumpOn = 1,       // Active high

                                        // Valve relay: PC13
                                        .valvePin = 45, // Arduino pin for PC13 on STM32F4
                                        .valveOn = 1,   // Active high

                                        // Auxiliary relay: PA15
                                        .altPin = 15, // Arduino pin for PA15 on STM32F4
                                        .altOn = 1,   // Active high

                                        // I2C1 for pressure sensor (PB6=SCL, PB7=SDA)
                                        .pressureScl = 22, // Arduino pin for PB6
                                        .pressureSda = 23, // Arduino pin for PB7

                                        // SPI1 for thermocouple MAX31855 (PA5=SCK, PA7=MOSI, PB4=MISO, PA6=CS)
                                        .maxSckPin = 5,   // Arduino pin for PA5
                                        .maxCsPin = 6,    // Arduino pin for PA6
                                        .maxMisoPin = 20, // Arduino pin for PB4

                                        // Brew button: PC14
                                        .brewButtonPin = 46, // Arduino pin for PC14

                                        // Steam button: PC15
                                        .steamButtonPin = 47, // Arduino pin for PC15

                                        // HX711: SCK=PB0, DOUT1=PB8, DOUT2=PB9
                                        .scaleSclPin = 16,  // Arduino pin for PB0
                                        .scaleSdaPin = 24,  // Arduino pin for PB8
                                        .scaleSda1Pin = 25, // Arduino pin for PB9

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
                                            .scale = true,       // Supports HX711 hardware scale
                                        }};

const ControllerConfig GM_STANDARD_REV_3X = {.name = "GaggiMate Standard Rev 3.x",
                                             .autodetectValue = 6,
                                             .heaterPin = 14,
                                             .pumpPin = 9,
                                             .pumpOn = 1,
                                             .valvePin = 10,
                                             .valveOn = 1,
                                             .altPin = 47,
                                             .altOn = 1,
                                             .maxSckPin = 6,
                                             .maxCsPin = 7,
                                             .maxMisoPin = 4,
                                             .brewButtonPin = 38,
                                             .steamButtonPin = 48,
                                             .scaleSclPin = 17,
                                             .scaleSdaPin = 18,
                                             .scaleSda1Pin = 39,
                                             .sunriseSclPin = 44,
                                             .sunriseSdaPin = 43,
                                             .ext1Pin = 1,
                                             .ext2Pin = 2,
                                             .ext3Pin = 8,
                                             .ext4Pin = 12,
                                             .ext5Pin = 13,
                                             .capabilites = {
                                                 .dimming = false,
                                                 .pressure = false,
                                                 .ssrPump = true,
                                                 .ledControls = false,
                                                 .tof = false,
                                             }};

#endif // CONTROLLERCONFIG_H
