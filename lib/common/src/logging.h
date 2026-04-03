#pragma once

#include <Arduino.h>

// Platform-agnostic logging macros
// On ESP32: use native ESP_LOG macros
// On STM32 and others: use Serial

#ifndef ARDUINO_ARCH_STM32

// ESP32 - use native macros
#include <esp_log.h>

#else

// STM32 and other platforms - use Serial fallback
#define LOG_E(tag, fmt, ...)                                                                                                     \
    do {                                                                                                                         \
        char _log_buf[256];                                                                                                      \
        snprintf(_log_buf, sizeof(_log_buf), "[E][%s] " fmt "\n", tag, ##__VA_ARGS__);                                           \
        Serial.print(_log_buf);                                                                                                  \
    } while (0)

#define LOG_W(tag, fmt, ...)                                                                                                     \
    do {                                                                                                                         \
        char _log_buf[256];                                                                                                      \
        snprintf(_log_buf, sizeof(_log_buf), "[W][%s] " fmt "\n", tag, ##__VA_ARGS__);                                           \
        Serial.print(_log_buf);                                                                                                  \
    } while (0)

#define LOG_I(tag, fmt, ...)                                                                                                     \
    do {                                                                                                                         \
        char _log_buf[256];                                                                                                      \
        snprintf(_log_buf, sizeof(_log_buf), "[I][%s] " fmt "\n", tag, ##__VA_ARGS__);                                           \
        Serial.print(_log_buf);                                                                                                  \
    } while (0)

#define LOG_D(tag, fmt, ...)                                                                                                     \
    do {                                                                                                                         \
        char _log_buf[256];                                                                                                      \
        snprintf(_log_buf, sizeof(_log_buf), "[D][%s] " fmt "\n", tag, ##__VA_ARGS__);                                           \
        Serial.print(_log_buf);                                                                                                  \
    } while (0)

#define LOG_V(tag, fmt, ...)                                                                                                     \
    do {                                                                                                                         \
        char _log_buf[256];                                                                                                      \
        snprintf(_log_buf, sizeof(_log_buf), "[V][%s] " fmt "\n", tag, ##__VA_ARGS__);                                           \
        Serial.print(_log_buf);                                                                                                  \
    } while (0)
// Compatibility aliases for existing ESP_LOG* calls
#define ESP_LOGE(tag, fmt, ...) LOG_E(tag, fmt, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) LOG_W(tag, fmt, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) LOG_I(tag, fmt, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) LOG_D(tag, fmt, ##__VA_ARGS__)
#define ESP_LOGV(tag, fmt, ...) LOG_V(tag, fmt, ##__VA_ARGS__)
#endif
