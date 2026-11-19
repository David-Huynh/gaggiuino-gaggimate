#pragma once

#include <Arduino.h>
#include <Stm32Boot.h>

namespace stm32_ota {

// Both OTA linker layouts reserve the last 16 bytes of SRAM. This mailbox is
// cleared on every boot and only honored after a software reset.
inline constexpr uint32_t LOADER_REQUEST = 0x474d4f54;
inline volatile uint32_t *const requestMailbox = reinterpret_cast<volatile uint32_t *>(SRAM_END - 16);

inline bool takeLoaderRequest() {
    const bool requested = (RCC->CSR & RCC_CSR_SFTRSTF) && requestMailbox[0] == LOADER_REQUEST &&
                           requestMailbox[1] == ~LOADER_REQUEST;
    requestMailbox[0] = 0;
    requestMailbox[1] = 0;
    return requested;
}

inline void holdOutputsOff() {
    // STM32 controller pin mapping from stm32f4_pindef.h.
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();
    constexpr uint16_t heaterAndPump = GPIO_PIN_15 | GPIO_PIN_1;
    HAL_GPIO_WritePin(GPIOA, heaterAndPump, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13, GPIO_PIN_RESET);
    GPIO_InitTypeDef pin{};
    pin.Pin = heaterAndPump;
    pin.Mode = GPIO_MODE_OUTPUT_PP;
    HAL_GPIO_Init(GPIOA, &pin);
    pin.Pin = GPIO_PIN_13;
    HAL_GPIO_Init(GPIOC, &pin);
}

[[noreturn]] inline void resetToLoader() {
    __disable_irq();
    holdOutputsOff();
    requestMailbox[0] = LOADER_REQUEST;
    requestMailbox[1] = ~LOADER_REQUEST;
    __DSB();
    NVIC_SystemReset();
}

} // namespace stm32_ota
