#include "main.h"
#include "ControllerConfig.h"
#include "GaggiMateController.h"
#include "UARTComm.h"

// Serial1 = USART_LCD: PA9(TX) → ESP32 RX, PA10(RX) ← ESP32 TX
UARTComm comm(&Serial1, 460800);
GaggiMateController controller(BUILD_GIT_VERSION, &comm);

void setup() { controller.setup(); }

void loop() { controller.loop(); }
