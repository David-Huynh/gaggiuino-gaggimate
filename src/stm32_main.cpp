/**
 * @file stm32_main.cpp
 * @brief STM32F4 Standalone Peripheral Controller for GaggiMate
 *
 * This is the main entry point for the STM32F4-based peripheral controller.
 * It communicates with the web/display ESP32 via UART using the line-v1 protocol.
 *
 * Hardware:
 * - STM32F401 or STM32F411 (Blackpill or similar)
 * - Serial2 (PA2=TX, PA3=RX @ 115200 baud) connects to ESP32
 * - FreeRTOS for task scheduling
 * - Peripherals controlled via GPIO, PWM, I2C, SPI
 *
 * Build: pio run -e stm32f4
 * Upload: pio run -e stm32f4 -t upload
 */

#include "stm32f4_pindef.h"
#include <Arduino.h>
#include <GaggiMateController.h>
#include <UARTComm.h>

// STM32duino HardwareSerial constructor is (RX, TX)
// Gaggiuino pindef: PA2=TX, PA3=RX → so constructor is (PA3, PA2)
HardwareSerial Serial2(PA3, PA2);

// Global controller and communication handler
GaggiMateController *controller = nullptr;
UARTComm *uartComm = nullptr;

/**
 * @brief Initialize the STM32F4 controller
 *
 * Sets up:
 * 1. UART communication with ESP32 (Serial2)
 * 2. Debug serial output (Serial1) optional
 * 3. GaggiMateController with UARTComm backend
 * 4. Board detection and peripheral initialization
 */
void setup() {
    delay(100);

    UART_COMM.begin(UART_COMM_BAUD);

    // Create UARTComm handler — pass true to skip double begin() in initServer()
    uartComm = new UARTComm(&UART_COMM, UART_COMM_BAUD, true);
    controller = new GaggiMateController(FIRMWARE_VERSION, uartComm);

    controller->setup(); // this calls vTaskStartScheduler() and never returns
}

void loop() {
    // Dead code — vTaskStartScheduler() in controller->setup() never returns
}
