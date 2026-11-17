#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
using String = std::string;
extern uint32_t testMillis;
extern bool heaterPinHigh;
extern std::function<void()> delayHook;
extern std::function<void()> pinHighHook;
inline unsigned long millis() { return testMillis; }
inline unsigned long micros() { return uint32_t(testMillis * 1000u); }
inline void delay(unsigned long ms) { testMillis += ms; }
inline void digitalWrite(uint8_t, int value) {
    if (value && pinHighHook) {
        auto hook = std::move(pinHighHook);
        pinHighHook = nullptr;
        hook();
    }
    heaterPinHigh = value != 0;
}
inline void pinMode(uint8_t, int) {}
constexpr int HIGH=1, LOW=0, OUTPUT=1;
constexpr float PI=3.14159265358979323846f;
template<class T> inline T constrain(T value,T low,T high) { return std::clamp(value,low,high); }
inline uint32_t HAL_GetUIDw0() { return 0xa1234567u; }
inline uint32_t HAL_GetUIDw1() { return 0xb7654321u; }
