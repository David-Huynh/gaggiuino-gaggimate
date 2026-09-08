#pragma once
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include "WString.h"
#include "Print.h"
#include "Stream.h"
#include "esp_heap_caps.h"
using std::min;
using std::max;
using byte = uint8_t;
using boolean = bool;
extern "C" unsigned long millis();
extern "C" void delay(uint32_t);
extern "C" void yield();
