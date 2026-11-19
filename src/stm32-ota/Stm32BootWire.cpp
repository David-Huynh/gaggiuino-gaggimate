#include "Stm32BootWire.h"

#include <cstring>

namespace stm32_ota {
namespace {

using gm_uart_ota::getU32;
using gm_uart_ota::putU32;

uint8_t responseTo(uint8_t command) { return command | gm_uart_ota::RESPONSE_FLAG; }

bool readBeginRequest(const uint8_t *request, size_t length, Manifest &manifest) {
    if (length != 1 + MANIFEST_SIZE || request[0] != static_cast<uint8_t>(Command::Begin))
        return false;
    std::memcpy(manifest.bytes, request + 1, MANIFEST_SIZE);
    return true;
}

bool sendRequest(Transfer &transfer, const uint8_t *request, size_t length, uint32_t &offset, uint32_t minimum,
                 uint32_t maximum) {
    for (unsigned attempt = 0; attempt < 4; ++attempt) {
        uint8_t reply[REPLY_SIZE];
        if (!transfer.exchange(request, length, reply) || reply[0] != responseTo(request[0]))
            continue;
        if (reply[1] != static_cast<uint8_t>(Status::Ok))
            return false;
        const uint32_t acknowledged = getU32(reply + 2);
        if (acknowledged >= minimum && acknowledged <= maximum && acknowledged % 4 == 0) {
            offset = acknowledged;
            return true;
        }
    }
    return false;
}

} // namespace

bool authorizedBeginRequest(const uint8_t *request, size_t length, const uint8_t (&publicKey)[32]) {
    Manifest manifest;
    return readBeginRequest(request, length, manifest) && manifest.valid(publicKey);
}

bool handleRequest(Installer &installer, const uint8_t *request, size_t length, uint8_t (&reply)[REPLY_SIZE]) {
    if (length == 0)
        return false;

    Status result = Status::Malformed;
    switch (static_cast<Command>(request[0])) {
        case Command::Begin: {
            Manifest manifest;
            if (readBeginRequest(request, length, manifest))
                result = installer.begin(manifest);
            break;
        }
        case Command::Write:
            if (length > WRITE_HEADER_SIZE)
                result = installer.write(getU32(request + 1), request + WRITE_HEADER_SIZE, length - WRITE_HEADER_SIZE);
            break;
        case Command::Finish:
            if (length == 1)
                result = installer.finish();
            break;
        default:
            return false;
    }

    reply[0] = responseTo(request[0]);
    reply[1] = static_cast<uint8_t>(result);
    putU32(reply + 2, installer.nextOffset());
    return true;
}

size_t encodeFrame(const uint8_t *payload, size_t length, uint8_t (&frame)[MAX_ENCODED_FRAME]) {
    if (length == 0 || length > MAX_REQUEST)
        return 0;
    uint8_t raw[MAX_FRAME_BODY];
    raw[0] = OTA_CHANNEL;
    std::memcpy(raw + 1, payload, length);
    const uint16_t crc = gm_uart::crc16(raw, length + 1);
    raw[length + 1] = static_cast<uint8_t>(crc);
    raw[length + 2] = static_cast<uint8_t>(crc >> 8);
    const size_t encoded = gm_uart::cobsEncode(raw, length + 3, frame);
    frame[encoded] = 0;
    return encoded + 1;
}

size_t FrameReader::push(uint8_t byte) {
    if (byte != 0) {
        if (length_ == sizeof(encoded_))
            overflow_ = true;
        if (!overflow_)
            encoded_[length_++] = byte;
        return 0;
    }
    const size_t decoded = overflow_ ? 0 : gm_uart::cobsDecode(encoded_, length_, decoded_, sizeof(decoded_));
    length_ = 0;
    overflow_ = false;
    if (decoded < 4 || decoded_[0] != OTA_CHANNEL)
        return 0;
    const uint16_t crc = decoded_[decoded - 2] | uint16_t{decoded_[decoded - 1]} << 8;
    return crc == gm_uart::crc16(decoded_, decoded - 2) ? decoded - 3 : 0;
}

bool sendImage(Transfer &transfer, const Manifest &manifest) {
    const uint32_t size = manifest.imageSize();
    if (size < 8 || size > MAX_IMAGE || size % 4 != 0 ||
        std::memcmp(manifest.bytes, IMAGE_MAGIC, sizeof(IMAGE_MAGIC)) != 0)
        return false;

    uint8_t request[MAX_REQUEST];
    request[0] = static_cast<uint8_t>(Command::Begin);
    std::memcpy(request + 1, manifest.bytes, MANIFEST_SIZE);
    uint32_t offset = 0;
    if (!sendRequest(transfer, request, MANIFEST_SIZE + 1, offset, 0, size))
        return false;

    transfer.progress(offset, size);
    while (offset < size) {
        const size_t remaining = size - offset;
        const size_t length = remaining < CHUNK_SIZE ? remaining : CHUNK_SIZE;
        request[0] = static_cast<uint8_t>(Command::Write);
        putU32(request + 1, offset);
        if (!transfer.read(offset, request + WRITE_HEADER_SIZE, length) ||
            !sendRequest(transfer, request, length + WRITE_HEADER_SIZE, offset, offset + length, offset + length))
            return false;
        transfer.progress(offset, size);
    }

    request[0] = static_cast<uint8_t>(Command::Finish);
    return sendRequest(transfer, request, 1, offset, size, size);
}

} // namespace stm32_ota
