#pragma once
#include <cstdint>
using SemaphoreHandle_t = void *;
using TaskHandle_t = void *;
using xTaskHandle = void *;
using TickType_t = uint32_t;
constexpr int pdTRUE = 1, configMINIMAL_STACK_SIZE = 128;
inline void *xSemaphoreCreateMutex() { return reinterpret_cast<void *>(1); }
inline int xSemaphoreTake(void *, unsigned long) { return pdTRUE; }
inline void xSemaphoreGive(void *) {}
inline unsigned long pdMS_TO_TICKS(unsigned long ms) { return ms; }
inline void xTaskCreate(void (*)(void *), const char *, int, void *, int, void **) {}
inline unsigned long xTaskGetTickCount() { return 0; }
inline void vTaskDelayUntil(TickType_t *, unsigned long) {}
