#include "main.h"

#ifndef GAGGIMATE_HEADLESS
#include <lvgl.h>
#endif

#include <esp_task_wdt.h>

Controller controller;

void setup() {
    Serial.begin(115200);
    // 15s task watchdog: if loopTask or this Arduino loop wedges, reboot cleanly
    // rather than requiring a power cycle.
    esp_task_wdt_init(15, true);
    esp_task_wdt_add(NULL);
    controller.setup();
}

void loop() {
    esp_task_wdt_reset();
    controller.loop();
    delay(50);
}
