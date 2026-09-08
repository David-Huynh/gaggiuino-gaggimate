#pragma once
#include <cstdint>
#include <cstddef>
#include <cmath>
#include <algorithm>
extern unsigned long clockMs;
inline unsigned long millis() { return clockMs; }
inline void delay(unsigned long ms) { clockMs += ms; }
inline void delayMicroseconds(unsigned long) {}
constexpr int LOW = 0, HIGH = 1, OUTPUT = 1, OUTPUT_OPEN_DRAIN = 2;
inline int digitalRead(int) { return LOW; }
