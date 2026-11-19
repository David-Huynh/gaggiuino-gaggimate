#include <Arduino.h>
#include <Stm32BootWire.h>
#include <stm32_ota_key.h>
#include "stm32_ota_reset.h"

namespace {

using namespace stm32_ota;

constexpr uint32_t BAUD = 115200;
constexpr uint32_t ACK_RETRY_WINDOW_MS = 2000;
constexpr uint32_t UPDATE_IDLE_MS = 20000;
HardwareSerial uart(PA3, PA2);

class F411Flash final : public Flash {
  public:
    const uint8_t *at(uint32_t address) const override {
        return reinterpret_cast<const uint8_t *>(address);
    }

    bool eraseSector(unsigned sector) override {
        if (sector < 3 || sector > 7 || HAL_FLASH_Unlock() != HAL_OK)
            return false;
        FLASH_EraseInitTypeDef erase{};
        erase.TypeErase = FLASH_TYPEERASE_SECTORS;
        erase.Sector = sector;
        erase.NbSectors = 1;
        erase.VoltageRange = FLASH_VOLTAGE_RANGE_3;
        uint32_t error;
        const bool ok = HAL_FLASHEx_Erase(&erase, &error) == HAL_OK;
        HAL_FLASH_Lock();
        return ok;
    }

    bool program(uint32_t address, const uint8_t *data, size_t length) override {
        if (address < ACTIVE_MANIFEST || address >= FLASH_LIMIT || length > FLASH_LIMIT - address ||
            address % 4 != 0 || length % 4 != 0 || HAL_FLASH_Unlock() != HAL_OK)
            return false;
        bool ok = true;
        for (size_t offset = 0; offset < length; offset += 4) {
            const uint32_t word = gm_uart_ota::getU32(data + offset);
            if (word != UINT32_MAX && HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, address + offset, word) != HAL_OK) {
                ok = false;
                break;
            }
        }
        HAL_FLASH_Lock();
        return ok;
    }
};

F411Flash flash;
Installer installer(flash, STM32_OTA_KEY);
FrameReader reader;
uint8_t replyFrame[MAX_ENCODED_FRAME];
uint32_t lastAcceptedAt = 0;
uint32_t readyAt = 0;
bool canBootApp = false;
bool restarting = false;

// No compiler stack accesses after replacing MSP.
__attribute__((naked, noreturn)) void enterApplication(uint32_t, uint32_t) {
    asm volatile("msr msp, r0\n"
                 "cpsie i\n"
                 "bx r1");
}

[[noreturn]] void bootApplication() {
    __disable_irq();
    SysTick->CTRL = 0;
    SysTick->VAL = 0;
    SCB->ICSR = SCB_ICSR_PENDSTCLR_Msk | SCB_ICSR_PENDSVCLR_Msk;
    for (size_t bank = 0; bank <= (SCnSCB->ICTR & SCnSCB_ICTR_INTLINESNUM_Msk); ++bank) {
        NVIC->ICER[bank] = UINT32_MAX;
        NVIC->ICPR[bank] = UINT32_MAX;
    }
    SCB->VTOR = APPLICATION;
    __DSB();
    __ISB();
    const uint8_t *vectors = flash.at(APPLICATION);
    enterApplication(gm_uart_ota::getU32(vectors), gm_uart_ota::getU32(vectors + 4));
}

} // namespace

void initVariant() { holdOutputsOff(); }

void setup() {
    const bool requested = takeLoaderRequest();
    // Read-back checks must see flash contents immediately after programming.
    __HAL_FLASH_DATA_CACHE_DISABLE();
    __HAL_FLASH_DATA_CACHE_RESET();
    canBootApp = installer.prepareApplication();
    if (canBootApp && !requested)
        bootApplication();
    uart.begin(BAUD);
    lastAcceptedAt = millis();
}

void loop() {
    while (uart.available()) {
        const size_t length = reader.push(static_cast<uint8_t>(uart.read()));
        uint8_t reply[REPLY_SIZE];
        if (length == 0 || !handleRequest(installer, reader.payload(), length, reply))
            continue;
        const size_t encoded = encodeFrame(reply, sizeof(reply), replyFrame);
        uart.write(replyFrame, encoded);
        if (reply[1] == static_cast<uint8_t>(Status::Ok))
            lastAcceptedAt = millis();
        if (installer.updateReady() && !restarting) {
            readyAt = millis();
            restarting = true;
        }
    }
    if (restarting && millis() - readyAt >= ACK_RETRY_WINDOW_MS) {
        uart.flush();
        NVIC_SystemReset();
    }
    if (canBootApp && !restarting && millis() - lastAcceptedAt >= UPDATE_IDLE_MS) {
        uart.end();
        bootApplication();
    }
}
