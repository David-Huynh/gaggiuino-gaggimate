/*
 * STM32F4 Pin Definitions for GaggiMate Controller
 *
 * Hardware Configuration:
 * - Thermocouple: MAX31855 over SPI
 * - Pressure Sensor: I2C-based (0x76 default)
 * - Scales: HX711 dual-channel load cells
 * - Heater: PWM output
 * - Pump: PWM output with current sensing for dimming
 * - Valve: GPIO relay output
 * - Buttons: GPIO digital inputs
 * - LED Controller: I2C (with TOF distance sensor)
 * - Communication: UART to ESP32
 */

#ifndef STM32F4_PINDEF_H
#define STM32F4_PINDEF_H

#include <Arduino.h>

// ===== UART Communication (to ESP32) =====
// Serial2: TX=PA2, RX=PA3 (USART2) — connected to ESP32 via Nextion connector wires
#define UART_COMM Serial2
#define UART_COMM_BAUD 115200 // 460800 caused signal issues — keep at 115200

// Debug logging is disabled (no-op) to avoid interfering with UART communication

// ===== Thermocouple (MAX31855) - SPI =====
// SPI1: MOSI=PA7, MISO=PB4, SCK=PA5
#define THERMO_CS_PIN PA6
#define THERMO_MISO_PIN PB4
#define THERMO_MOSI_PIN PA7
#define THERMO_CLK_PIN PA5
// SPI interface will be configured via stm32duino SPI1

// ===== Heater Control =====
#define HEATER_PIN PA15       // SSR relay output (relayPin in Gaggiuino pindef)
#define HEATER_PWM_FREQ 40000 // 40 kHz PWM frequency

// ===== Pump Control =====
#define PUMP_PIN PA1        // PWM via TIM2_CH2
#define PUMP_SENSE_PIN PA0  // ADC for current sensing (dimmed pump feedback)
#define PUMP_PWM_FREQ 40000 // 40 kHz PWM frequency

// ===== Valve Control =====
#define VALVE_PIN PC13 // GPIO output (normally low = closed, high = open)
#define VALVE_ON_STATE HIGH

// ===== Water Sensor =====
#define WATER_PIN PB15

// ===== Auxiliary Relay (not available on standard Gaggiuino PCB) =====
// #define ALT_PIN — no dedicated alt relay pin on this board

// ===== Brew & Steam Button Inputs =====
#define BREW_BTN_PIN PC14  // GPIO input with pull-down
#define STEAM_BTN_PIN PC15 // GPIO input with pull-down

// ===== I2C Bus (for pressure sensor, LED controller, TOF, scales) =====
// I2C1: SDA=PB7, SCL=PB6 @ 400 kHz
#define I2C_PRESSURE Wire // Will be configured via stm32duino I2C1
#define I2C_SDA_PIN PB7
#define I2C_SCL_PIN PB6
#define I2C_FREQ 400000

// ===== Pressure Sensor (BME280 or similar) =====
#define PRESSURE_SDA_PIN PB7
#define PRESSURE_SCL_PIN PB6
#define PRESSURE_I2C_ADDR 0x76 // Default BME280/BMP280 address

// ===== Scales (HX711 - dual channel) =====
#define HX711_SCK_PIN PB0
#define HX711_DOUT1_PIN PB8 // Channel 1 (puck/group weight)
#define HX711_DOUT2_PIN PB9 // Channel 2 (pump/water weight)

// ===== LED Controller & Distance Sensor (I2C) =====
// Connected via same I2C1 bus (PB6/PB7)
#define LED_CTRL_I2C_ADDR 0x30   // Custom or Adafruit NeoPixel I2C address
#define TOF_SENSOR_I2C_ADDR 0x29 // VL53L0X default address

// ===== Analog Inputs (ADC) =====
// Used for sensing and diagnostics
#define ADC_PUMP_SENSE_PIN PA0 // Current sensing for pump (via op-amp conditioning)
#ifdef ADC_RESOLUTION
#undef ADC_RESOLUTION
#endif
#define ADC_RESOLUTION 12
// ===== System Configuration =====
#define SYSTEM_BOARD_NAME "GaggiMate STM32F4 Controller"
#define SYSTEM_VERSION "1.0.0"
#define FIRMWARE_VERSION SYSTEM_VERSION

// Board autodetect value (not used in STM32 build, but included for compatibility)
#define AUTODETECT_VALUE 99 // Unique ID for this board variant

// ===== Pin Validation =====
// Verify no conflicts:
// - SPI1: PA5(CLK), PA6(CS), PA7(MOSI), PB4(MISO) - Thermocouple
// - USART2: PA2(TX), PA3(RX) - ESP32 communication (Nextion LCD connector wires)
// - I2C1: PB6(SCL), PB7(SDA) - Pressure/LED/TOF/Scales
// - PWM: PA1(pump) - Pump dimmer output
// - GPIO: PA0(ADC), PA15(heater SSR), PC13(valve), PC14(brew), PC15(steam)
// - HX711: PB0(sck), PB8(dout1), PB9(dout2)

#endif // STM32F4_PINDEF_H
