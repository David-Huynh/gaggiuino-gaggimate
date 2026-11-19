#pragma once
#if defined(ARDUINO_ARCH_STM32)
#include <STM32FreeRTOS.h>
#include <semphr.h>
#else
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#endif

// TwoWire protects transfers, but not consumption of its shared RX buffer.
// Only devices sharing one bus across tasks supply a mutex here.
class SharedI2CBusGuard {
  public:
    explicit SharedI2CBusGuard(SemaphoreHandle_t mutex) : mutex(mutex) {
        if (mutex)
            xSemaphoreTake(mutex, portMAX_DELAY);
    }
    ~SharedI2CBusGuard() {
        if (mutex)
            xSemaphoreGive(mutex);
    }
    SharedI2CBusGuard(const SharedI2CBusGuard &) = delete;
    SharedI2CBusGuard &operator=(const SharedI2CBusGuard &) = delete;

  private:
    SemaphoreHandle_t mutex;
};
