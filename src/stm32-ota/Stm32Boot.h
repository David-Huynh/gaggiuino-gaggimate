#pragma once

#include <cstddef>
#include <cstdint>

namespace stm32_ota {

// STM32F411CE sectors 0–2 contain the bootloader. Sector 3 holds the active
// manifest, sectors 4–5 the application, and sectors 6–7 the complete staged image.
constexpr uint32_t ACTIVE_MANIFEST = 0x0800c000;
constexpr uint32_t APPLICATION = 0x08010000;
constexpr uint32_t STAGED_MANIFEST = 0x08040000;
constexpr uint32_t STAGED_IMAGE = STAGED_MANIFEST + 0x200;
constexpr uint32_t MAX_IMAGE = STAGED_MANIFEST - APPLICATION;
constexpr uint32_t FLASH_LIMIT = 0x08080000;
constexpr uint32_t SRAM_BEGIN = 0x20000000;
constexpr uint32_t SRAM_END = 0x20020000;
constexpr uint8_t IMAGE_MAGIC[] = {'G', 'M', '4', '1', '1', 'O', 'T', 1};
constexpr size_t HASH_OFFSET = sizeof(IMAGE_MAGIC) + sizeof(uint32_t);
constexpr size_t SIGNATURE_OFFSET = HASH_OFFSET + 64;
constexpr size_t MANIFEST_SIZE = SIGNATURE_OFFSET + 64;
constexpr size_t CHUNK_SIZE = 1024;
constexpr uint32_t COMMITTED = 0x474d4f54;

static_assert(MANIFEST_SIZE % 4 == 0);
static_assert(STAGED_IMAGE + MAX_IMAGE <= FLASH_LIMIT);

struct Manifest {
    uint8_t bytes[MANIFEST_SIZE];

    uint32_t imageSize() const;
    bool valid(const uint8_t *publicKey) const;
    bool matchesImage(const uint8_t *image) const;
    bool operator==(const Manifest &other) const;
};

class Flash {
  public:
    virtual ~Flash() = default;
    // The installer uses fixed regions whose contents are directly readable.
    virtual const uint8_t *at(uint32_t address) const = 0;
    virtual bool eraseSector(unsigned sector) = 0;
    virtual bool program(uint32_t address, const uint8_t *data, size_t length) = 0;
};

enum class Status : uint8_t {
    Ok,
    BadState,
    FlashError,
    BadOffset,
    VerificationFailed,
    Malformed,
};

class Installer {
  public:
    Installer(Flash &flash, const uint8_t (&publicKey)[32]) : flash_(flash), publicKey_(publicKey) {}

    // Run on reset before handing control to the application. A verified staged copy
    // survives any interruption of the active copy and can be installed again.
    bool prepareApplication();
    Status begin(const Manifest &manifest);
    Status write(uint32_t offset, const uint8_t *data, size_t length);
    Status finish();
    uint32_t nextOffset() const { return nextOffset_; }
    bool updateReady() const { return state_ == State::Staged; }

  private:
    enum class State { Waiting, Receiving, Staged, ActivationFailed };

    bool readManifest(uint32_t address, Manifest &manifest) const;
    bool commit(uint32_t address, const Manifest &manifest);
    bool install(const Manifest &staged);

    Flash &flash_;
    const uint8_t (&publicKey_)[32];
    Manifest receiving_{};
    uint32_t nextOffset_ = 0;
    State state_ = State::Waiting;
};

} // namespace stm32_ota
