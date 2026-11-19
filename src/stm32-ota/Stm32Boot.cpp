#include "Stm32Boot.h"

#include <cstring>
#include <monocypher-ed25519.h>
#include <uart/UartOta.h>

namespace stm32_ota {

using gm_uart_ota::getU32;
using gm_uart_ota::putU32;

uint32_t Manifest::imageSize() const { return getU32(bytes + sizeof(IMAGE_MAGIC)); }

bool Manifest::valid(const uint8_t *publicKey) const {
    const uint32_t length = imageSize();
    return length >= 8 && length <= MAX_IMAGE && length % 4 == 0 &&
           std::memcmp(bytes, IMAGE_MAGIC, sizeof(IMAGE_MAGIC)) == 0 &&
           crypto_ed25519_check(bytes + SIGNATURE_OFFSET, publicKey, bytes, SIGNATURE_OFFSET) == 0;
}

bool Manifest::matchesImage(const uint8_t *image) const {
    const uint32_t stack = getU32(image);
    const uint32_t entry = getU32(image + sizeof(uint32_t));
    const uint32_t entryAddress = entry & ~uint32_t{1};
    if (stack <= SRAM_BEGIN || stack > SRAM_END || stack % 8 != 0 || !(entry & 1) ||
        entryAddress < APPLICATION + 8 || entryAddress >= APPLICATION + imageSize())
        return false;

    uint8_t hash[64];
    crypto_sha512(hash, image, imageSize());
    return std::memcmp(hash, bytes + HASH_OFFSET, sizeof(hash)) == 0;
}

bool Manifest::operator==(const Manifest &other) const {
    return std::memcmp(bytes, other.bytes, sizeof(bytes)) == 0;
}

bool Installer::readManifest(uint32_t address, Manifest &manifest) const {
    const uint8_t *record = flash_.at(address);
    if (getU32(record + MANIFEST_SIZE) != COMMITTED)
        return false;
    std::memcpy(manifest.bytes, record, MANIFEST_SIZE);
    return manifest.valid(publicKey_);
}

bool Installer::commit(uint32_t address, const Manifest &manifest) {
    uint8_t marker[sizeof(uint32_t)];
    putU32(marker, COMMITTED);
    return flash_.program(address, manifest.bytes, MANIFEST_SIZE) &&
           std::memcmp(flash_.at(address), manifest.bytes, MANIFEST_SIZE) == 0 &&
           flash_.program(address + MANIFEST_SIZE, marker, sizeof(marker)) &&
           getU32(flash_.at(address + MANIFEST_SIZE)) == COMMITTED;
}

bool Installer::install(const Manifest &staged) {
    // The staging sectors remain untouched as long as the active copy is incomplete.
    if (!flash_.eraseSector(3) || !flash_.eraseSector(4) || !flash_.eraseSector(5))
        return false;

    for (uint32_t offset = 0; offset < staged.imageSize();) {
        const size_t remaining = staged.imageSize() - offset;
        const size_t length = remaining < CHUNK_SIZE ? remaining : CHUNK_SIZE;
        if (!flash_.program(APPLICATION + offset, flash_.at(STAGED_IMAGE + offset), length))
            return false;
        offset += length;
    }
    return staged.matchesImage(flash_.at(APPLICATION)) && commit(ACTIVE_MANIFEST, staged);
}

bool Installer::prepareApplication() {
    Manifest active, staged;
    const bool activeValid = readManifest(ACTIVE_MANIFEST, active) && active.matchesImage(flash_.at(APPLICATION));
    if (!readManifest(STAGED_MANIFEST, staged) || (activeValid && active == staged) ||
        !staged.matchesImage(flash_.at(STAGED_IMAGE)))
        return activeValid;

    const bool installed = install(staged);
    state_ = installed ? State::Waiting : State::ActivationFailed;
    return installed;
}

Status Installer::begin(const Manifest &manifest) {
    if (!manifest.valid(publicKey_))
        return Status::VerificationFailed;
    if (state_ == State::ActivationFailed || (state_ == State::Staged && !(receiving_ == manifest)))
        return Status::BadState;
    if (state_ != State::Waiting && receiving_ == manifest)
        return Status::Ok;

    state_ = State::Waiting;
    nextOffset_ = 0;
    Manifest active;
    if (readManifest(ACTIVE_MANIFEST, active) && active == manifest &&
        active.matchesImage(flash_.at(APPLICATION))) {
        receiving_ = manifest;
        nextOffset_ = manifest.imageSize();
        state_ = State::Staged;
        return Status::Ok;
    }
    if (!flash_.eraseSector(6) || !flash_.eraseSector(7))
        return Status::FlashError;

    receiving_ = manifest;
    state_ = State::Receiving;
    return Status::Ok;
}

Status Installer::write(uint32_t offset, const uint8_t *data, size_t length) {
    if (state_ != State::Receiving)
        return Status::BadState;
    if (offset > nextOffset_ || offset % 4 != 0 || length == 0 || length > CHUNK_SIZE || length % 4 != 0 ||
        length > receiving_.imageSize() - offset)
        return Status::BadOffset;

    const uint32_t address = STAGED_IMAGE + offset;
    if (offset < nextOffset_) {
        return length <= nextOffset_ - offset && std::memcmp(flash_.at(address), data, length) == 0
                   ? Status::Ok : Status::BadOffset;
    }
    if (!flash_.program(address, data, length)) {
        state_ = State::Waiting;
        nextOffset_ = 0;
        return Status::FlashError;
    }
    nextOffset_ += length;
    return Status::Ok;
}

Status Installer::finish() {
    if (state_ == State::Staged)
        return Status::Ok;
    if (state_ != State::Receiving || nextOffset_ != receiving_.imageSize())
        return Status::BadState;
    if (!receiving_.matchesImage(flash_.at(STAGED_IMAGE))) {
        state_ = State::Waiting;
        nextOffset_ = 0;
        return Status::VerificationFailed;
    }
    if (!commit(STAGED_MANIFEST, receiving_)) {
        state_ = State::Waiting;
        nextOffset_ = 0;
        return Status::FlashError;
    }
    state_ = State::Staged;
    return Status::Ok;
}

} // namespace stm32_ota
