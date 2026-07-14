#include "main.h"

#ifndef GAGGIMATE_HEADLESS
#include <lvgl.h>
#endif

#include <esp_task_wdt.h>

Controller controller;
static bool loopTaskWatchdogRegistered = false;

void setup() {
    Serial.begin(115200);
    // Configure the watchdog before Controller creates its watched logic task.
    // loopTask is added after its first pass because that pass performs bounded,
    // synchronous hardware and network startup.
    esp_task_wdt_init(15, true);
    controller.setup();
}

void loop() {
    if (loopTaskWatchdogRegistered) {
        esp_task_wdt_reset();
    }
    controller.loop();
    if (!loopTaskWatchdogRegistered) {
        loopTaskWatchdogRegistered = esp_task_wdt_add(NULL) == ESP_OK;
    }
    delay(50);
}
