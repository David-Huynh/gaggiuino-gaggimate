/**
 * @file stm32_main.cpp
 * @brief STM32F4 Standalone Peripheral Controller for GaggiMate
 *
 * This is the main entry point for the STM32F4-based peripheral controller.
 * It communicates with the web/display ESP32 via UART using the line-v1 protocol.
 *
 * Hardware:
 * - STM32F401 or STM32F411 (Blackpill or similar)
 * - Serial2 (PA2=TX, PA3=RX @ 460800 baud) connects to ESP32
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

#if defined(UART_TX_PIN) && defined(UART_RX_PIN)
HardwareSerial Serial2(UART_TX_PIN, UART_RX_PIN);
#endif
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
    // Small delay for stable power-up
    delay(100);

    // Initialize debug serial (optional, connect PA9/PA10 to USB-UART adapter)
    // This allows remote debugging via USB serial
    UART_DEBUG.begin(UART_DEBUG_BAUD);
    delay(50);

    // Print startup message
    Serial.print("GaggiMate STM32F4 Controller v");
    Serial.println(FIRMWARE_VERSION);
    Serial.println("Initializing UART communication with ESP32...");

    // Initialize main UART communication with ESP32 (Serial2 @ 460800)
    // This MUST be initialized before creating UARTComm
    UART_COMM.begin(UART_COMM_BAUD);
    delay(100);

    // Create UARTComm handler (uses Serial2 for ESP32 communication)
    uartComm = new UARTComm(&UART_COMM, UART_COMM_BAUD);
    Serial.println("UART communication initialized.");

    // Create GaggiMateController with UARTComm backend
    // GaggiMateController is the core controller logic
    // It uses UARTComm to send/receive commands and data
    controller = new GaggiMateController(FIRMWARE_VERSION, uartComm);

    Serial.println("GaggiMateController instantiated.");
    Serial.println("Starting GaggiMateController setup...");

    // Initialize the controller (board detection, peripheral setup, etc.)
    // This is a blocking call that may take several seconds
    controller->setup();

    Serial.println("Setup complete. Ready for commands from ESP32.");
    Serial.println("Waiting for PING...");
}

/**
 * @brief Main loop
 *
 * Handles:
 * 1. Processing incoming UART commands from ESP32
 * 2. Reading sensors and sending data
 * 3. Updating peripherals (heater PID, pump control, etc.)
 *
 * This runs continuously with GaggiMateController handling timing.
 */
void loop() { // Should be blocked by spinning up vTaskStartScheduler();
    // if (!controller || !uartComm) {
    //     // Controller not initialized, spin
    //     delay(100);
    //     return;
    // }

    // // Process any incoming UART data from ESP32
    // // This parses CSV commands and triggers callbacks
    // uartComm->update();

    // // Run main controller loop
    // // Handles sensor reads, PID updates, safety checks, sends periodic sensor data
    // controller->loop();

    // // Minimal delay to allow other tasks (if using FreeRTOS)
    // // GaggiMateController::loop() already includes a delay(250)
}
