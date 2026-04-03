/**
 * HX711 library for Arduino (Dual-Channel, Interrupt-Driven)
 * Modified for STM32 & ESP32 Cross-Compatibility
 **/

#ifndef HX711_2_h
#define HX711_2_h

#include "Arduino.h"

// Pull in STM32 specific headers if compiling for STM
#if defined(ARDUINO_ARCH_STM32)
#include "HardwareTimer.h"
#endif

class HX711_2 {
  private:
    uint32_t PD_SCK;
    uint32_t DOUT;
    uint32_t DOUT2;
    uint8_t GAIN;
    long OFFSET = 0;
    long OFFSET2 = 0;
    float SCALE = 1.f;
    float SCALE2 = 1.f;

    volatile uint32_t readStatus;
    volatile uint32_t readBuffer[2];
    volatile uint32_t readData[2];
    unsigned long lastReadCounter;

// --- Hardware Specific Members ---
#if defined(ARDUINO_ARCH_STM32)
    PinName PD_SCK_PN;
    PinName DOUT_PN;
    PinName DOUT2_PN;
    HardwareTimer *_hx711ReadTimer;
#elif defined(ARDUINO_ARCH_ESP32)
    hw_timer_t *_hx711ReadTimer = NULL;
#endif

  public:
// --- Architecture-Specific Constructors ---
#if defined(ARDUINO_ARCH_STM32)
    HX711_2(TIM_TypeDef *timerInstance = TIM1);
#else
    HX711_2();
#endif

    virtual ~HX711_2();

    void begin(uint32_t dout, uint32_t dout2, uint32_t pd_sck, uint8_t gain = 128, uint32_t sck_mode = OUTPUT);

    bool is_ready(unsigned long fromCounter = 0UL);
    void wait_ready(unsigned long delay_ms = 0, unsigned long fromCounter = 0UL);
    bool wait_ready_retry(unsigned int retries = 3, unsigned long delay_ms = 0, unsigned long fromCounter = 0UL);
    bool wait_ready_timeout(unsigned long timeout = 1000, unsigned long delay_ms = 0, unsigned long fromCounter = 0UL);
    void set_gain(uint8_t gain = 128);
    void read(long *readValues, unsigned long timeout = 1000);
    void read_average(long *readValues, const byte times = 10);
    void get_value(long *readValues, const byte times = 1);
    void get_units(float *readValues, const byte times = 1);
    void tare(const byte times = 10);
    void set_scale(float scale = 1.f, float scale2 = 1.f);
    void get_scale(float *scaleValues);
    void set_offset(long offset = 0, long offset2 = 0);
    void get_offset(long *offsetValues);
    long get_readCounter();
    void power_down();
    void power_up();
    void processReadTimerInterrupt(void);
    static void _onHX711ReadTimerInterrupt(void);
};

#endif /* HX711_2_h */