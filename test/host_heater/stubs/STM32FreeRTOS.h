#pragma once
#include "Arduino.h"
#include <deque>
#include <mutex>
#include <vector>
using TickType_t=uint32_t;
using TaskHandle_t=void*;
using xTaskHandle=void*;
using SemaphoreHandle_t=std::recursive_mutex*;
constexpr int pdTRUE=1,pdPASS=1,pdFALSE=0, configMINIMAL_STACK_SIZE=128;
constexpr uint32_t portMAX_DELAY=0xffffffff,portTICK_PERIOD_MS=1;
inline unsigned long pdMS_TO_TICKS(unsigned long ms) { return ms; }
inline SemaphoreHandle_t xSemaphoreCreateMutex() { return new std::recursive_mutex; }
inline SemaphoreHandle_t xSemaphoreCreateRecursiveMutex() { return xSemaphoreCreateMutex(); }
inline int xSemaphoreTake(SemaphoreHandle_t m,uint32_t) { m->lock(); return pdTRUE; }
inline int xSemaphoreGive(SemaphoreHandle_t m) { m->unlock(); return pdTRUE; }
inline int xSemaphoreTakeRecursive(SemaphoreHandle_t m,uint32_t t) { return xSemaphoreTake(m,t); }
inline int xSemaphoreGiveRecursive(SemaphoreHandle_t m) { return xSemaphoreGive(m); }
inline void vSemaphoreDelete(SemaphoreHandle_t m) { delete m; }
inline int xTaskCreate(void(*)(void*),const char*,int,void*,int,void** task) { *task=reinterpret_cast<void*>(1); return pdPASS; }
inline void vTaskDelete(void*) {}
inline void xTaskNotifyGive(TaskHandle_t) {}
inline TickType_t xTaskGetTickCount() { return testMillis; }
inline void vTaskDelay(uint32_t ms) { testMillis+=std::max(1u,ms); if(delayHook) delayHook(); }
inline void vTaskDelayUntil(TickType_t* t,TickType_t dt) { *t+=dt; testMillis=*t; }
struct TestQueue { size_t cap,size; std::deque<std::vector<uint8_t>> entries; };
using QueueHandle_t=TestQueue*;
inline QueueHandle_t xQueueCreate(size_t cap,size_t size) { return new TestQueue{cap,size,{}}; }
inline int xQueueSend(QueueHandle_t q,const void* value,uint32_t) {
 if(q->entries.size()>=q->cap) return pdFALSE;
 const auto *bytes=static_cast<const uint8_t*>(value); q->entries.emplace_back(bytes,bytes+q->size); return pdTRUE;
}
inline int xQueueReceive(QueueHandle_t q,void* value,uint32_t) {
 if(q->entries.empty()) return pdFALSE;
 std::memcpy(value,q->entries.front().data(),q->size); q->entries.pop_front(); return pdTRUE;
}
inline size_t uxQueueSpacesAvailable(QueueHandle_t q) { return q->cap-q->entries.size(); }
inline void xQueueReset(QueueHandle_t q) { q->entries.clear(); }
inline void vQueueDelete(QueueHandle_t q) { delete q; }
