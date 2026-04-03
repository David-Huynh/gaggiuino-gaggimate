/**
 * @file stm32_main.cpp
 * @brief STM32F4 Standalone Peripheral Controller for GaggiMate
 *
 * This is the main entry point for the STM32F4-based peripheral controller.
 * It communicates with the web/display ESP32 via UART using the NanoPb protocol.
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

#if defined(UART_TX_PIN) && defined(UART_RX_PIN)
HardwareSerial Serial2(UART_TX_PIN, UART_RX_PIN);
#endif
// Global controller and communication handler
GaggiMateController *controller = nullptr;

/**
 * @brief Initialize the STM32F4 controller
 *
 * Sets up:
 * 1. UART communication with ESP32 (Serial2)
 * 2. Debug serial output (Serial1) optional
 * 3. GaggiMateController with NanoPb UART transport
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

    // Initialize main UART communication with ESP32.
    // This MUST be initialized before GaggiMateServer starts its UART transport.
    UART_COMM.begin(UART_COMM_BAUD);
    delay(100);

    Serial.println("UART communication initialized.");

    // Create GaggiMateController with NanoPb UART transport
    // GaggiMateController is the core controller logic
    controller = new GaggiMateController(FIRMWARE_VERSION);

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
 * 1. Processing incoming UART frames from ESP32
 * 2. Reading sensors and sending data
 * 3. Updating peripherals (heater PID, pump control, etc.)
 *
 * This runs continuously with GaggiMateController handling timing.
 */
void loop() { // Should be blocked by spinning up vTaskStartScheduler();
    // if (!controller) {
    //     // Controller not initialized, spin
    //     delay(100);
    //     return;
    // }

    // // Run main controller loop
    // // Handles sensor reads, PID updates, safety checks, sends periodic sensor data
    // controller->loop();

    // // Minimal delay to allow other tasks (if using FreeRTOS)
    // // GaggiMateController::loop() already includes a delay(250)
}
