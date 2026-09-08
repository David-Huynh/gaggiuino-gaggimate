#pragma once
using BaseType_t = int;
using TaskHandle_t = void *;
constexpr BaseType_t taskSCHEDULER_NOT_STARTED = 1;
constexpr BaseType_t taskSCHEDULER_RUNNING = 2;
constexpr BaseType_t taskSCHEDULER_SUSPENDED = 0;
