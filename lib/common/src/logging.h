#pragma once

#include <Arduino.h>

// Platform-agnostic logging macros
// On ESP32: use native ESP_LOG macros
// On STM32 and others: use Serial

#ifndef ARDUINO_ARCH_STM32

// ESP32 - use native macros
#include <esp_log.h>

#else

// STM32 and other platforms - no-op to avoid interfering with UART communication
#define ESP_LOGE(tag, fmt, ...) ((void)0)
#define ESP_LOGW(tag, fmt, ...) ((void)0)
#define ESP_LOGI(tag, fmt, ...) ((void)0)
#define ESP_LOGD(tag, fmt, ...) ((void)0)
#define ESP_LOGV(tag, fmt, ...) ((void)0)
#endif
