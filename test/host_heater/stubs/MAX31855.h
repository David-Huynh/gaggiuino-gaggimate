#pragma once
#include "Arduino.h"
constexpr uint8_t STATUS_OK=0,STATUS_NOREAD=1;
extern float testRawTemperature;
extern uint8_t testTemperatureStatus;
class MAX31855 {
 public:
 MAX31855(int,int,int) {}
 void begin() {} void setSPIspeed(unsigned long) {}
 int read() { return testTemperatureStatus; }
 float getTemperature() { return testRawTemperature; }
};
