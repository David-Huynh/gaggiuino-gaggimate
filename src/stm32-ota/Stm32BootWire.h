#pragma once

#include "Stm32Boot.h"
#include <uart/UartFraming.h>
#include <uart/UartOta.h>

namespace stm32_ota {

// Reuses the UART OTA channel and COBS/CRC framing. The STM32
// command range is separate from the ESP32 receiver's MD5-based update.
constexpr uint8_t OTA_CHANNEL = 1;
enum class Command : uint8_t { Begin = 0x11, Write = 0x12, Finish = 0x13 };
constexpr size_t WRITE_HEADER_SIZE = 1 + sizeof(uint32_t);
constexpr size_t MAX_REQUEST = WRITE_HEADER_SIZE + CHUNK_SIZE;
constexpr size_t REPLY_SIZE = gm_uart_ota::RESPONSE_LEN;
constexpr size_t MAX_FRAME_BODY = 1 + MAX_REQUEST + 2;
constexpr size_t MAX_ENCODED_FRAME = gm_uart::cobsMaxEncodedLen(MAX_FRAME_BODY) + 1;

bool authorizedBeginRequest(const uint8_t *request, size_t length, const uint8_t (&publicKey)[32]);
bool handleRequest(Installer &installer, const uint8_t *request, size_t length, uint8_t (&reply)[REPLY_SIZE]);

// Used by the bare UART bootloader and the transport simulator. The normal ESP32
// firmware already sends equivalent frames via UartTransport::sendOn().
size_t encodeFrame(const uint8_t *payload, size_t length, uint8_t (&frame)[MAX_ENCODED_FRAME]);

class FrameReader {
  public:
    size_t push(uint8_t byte);
    const uint8_t *payload() const { return decoded_ + 1; }

  private:
    uint8_t encoded_[MAX_ENCODED_FRAME - 1];
    uint8_t decoded_[MAX_FRAME_BODY];
    size_t length_ = 0;
    bool overflow_ = false;
};

class Transfer {
  public:
    virtual ~Transfer() = default;
    virtual bool read(uint32_t offset, uint8_t *buffer, size_t length) = 0;
    virtual bool exchange(const uint8_t *request, size_t length, uint8_t (&reply)[REPLY_SIZE]) = 0;
    virtual void progress(uint32_t sent, uint32_t total) = 0;
};

// Acknowledgement means the image is durable in staging. An updater must
// separately check that the controller reconnects on the expected version.
bool sendImage(Transfer &transfer, const Manifest &manifest);

} // namespace stm32_ota
