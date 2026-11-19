#include <Stm32BootWire.h>
#include <monocypher-ed25519.h>

#include <algorithm>
#undef NDEBUG
#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

using namespace stm32_ota;
using gm_uart_ota::getU32;
using gm_uart_ota::putU32;

namespace {

constexpr uint32_t FLASH_BASE = 0x08000000;
constexpr uint32_t SECTORS[] = {
    FLASH_BASE, 0x08004000, 0x08008000, ACTIVE_MANIFEST, APPLICATION, 0x08020000, STAGED_MANIFEST, 0x08060000, FLASH_LIMIT,
};

struct PowerCut {};
enum class Cut { Before, During, After };

class SimFlash final : public Flash {
  public:
    SimFlash() : bytes(FLASH_LIMIT - FLASH_BASE, 0xff) {
        std::fill_n(bytes.begin(), ACTIVE_MANIFEST - FLASH_BASE, 0xa5);
    }

    const uint8_t *at(uint32_t address) const override {
        assert(address >= FLASH_BASE && address < FLASH_LIMIT);
        return bytes.data() + address - FLASH_BASE;
    }

    bool eraseSector(unsigned sector) override {
        assert(sector >= 3 && sector <= 7);
        if (sector == failingSector)
            return false;
        uint8_t *start = bytes.data() + SECTORS[sector] - FLASH_BASE;
        const size_t length = SECTORS[sector + 1] - SECTORS[sector];
        const bool interrupted = visit();
        std::memset(start, 0xff, interrupted && cut == Cut::During ? length / 2 : length);
        if (interrupted)
            throw PowerCut{};
        return true;
    }

    bool program(uint32_t address, const uint8_t *data, size_t length) override {
        assert(address >= ACTIVE_MANIFEST && address < FLASH_LIMIT && length <= FLASH_LIMIT - address);
        assert(address % 4 == 0 && length % 4 == 0);
        uint8_t *destination = bytes.data() + address - FLASH_BASE;
        for (size_t offset = 0; offset < length; offset += 4) {
            if (getU32(data + offset) == UINT32_MAX)
                continue;
            const bool interrupted = visit();
            const size_t written = interrupted && cut == Cut::During ? 2 : 4;
            for (size_t i = offset; i < offset + written; ++i) {
                assert((destination[i] & data[i]) == data[i]);
                destination[i] = data[i];
            }
            if (interrupted)
                throw PowerCut{};
        }
        return true;
    }

    bool bootloaderIntact() const {
        return std::all_of(bytes.begin(), bytes.begin() + (ACTIVE_MANIFEST - FLASH_BASE),
                           [](uint8_t byte) { return byte == 0xa5; });
    }

    std::vector<uint8_t> bytes;
    size_t operations = 0;
    size_t failAt = 0;
    Cut cut = Cut::Before;
    unsigned failingSector = 8;

  private:
    bool visit() {
        const bool interrupted = ++operations == failAt;
        if (interrupted && cut == Cut::Before)
            throw PowerCut{};
        return interrupted;
    }
};

struct TestKey {
    uint8_t publicKey[32];
    uint8_t privateKey[64];

    TestKey() {
        // RFC 8032 test vector. Test keys never enter a firmware build by default.
        uint8_t seed[32] = {0x9d, 0x61, 0xb1, 0x9d, 0xef, 0xfd, 0x5a, 0x60, 0xba, 0x84, 0x4a,
                            0xf4, 0x92, 0xec, 0x2c, 0xc4, 0x44, 0x49, 0xc5, 0x69, 0x7b, 0x32,
                            0x69, 0x19, 0x70, 0x3b, 0xac, 0x03, 0x1c, 0xae, 0x7f, 0x60};
        crypto_ed25519_key_pair(privateKey, publicKey, seed);
    }

    void sign(Manifest &manifest) const {
        crypto_ed25519_sign(manifest.bytes + SIGNATURE_OFFSET, privateKey, manifest.bytes, SIGNATURE_OFFSET);
    }
};

struct Image {
    explicit Image(uint8_t pattern, size_t size = 2104) : bytes(size, pattern) {
        putU32(bytes.data(), SRAM_END);
        putU32(bytes.data() + 4, APPLICATION + 0x81);
    }

    Manifest manifest(const TestKey &key) const {
        assert(bytes.size() % 4 == 0);
        Manifest result;
        std::memcpy(result.bytes, IMAGE_MAGIC, sizeof(IMAGE_MAGIC));
        putU32(result.bytes + sizeof(IMAGE_MAGIC), bytes.size());
        crypto_sha512(result.bytes + HASH_OFFSET, bytes.data(), bytes.size());
        key.sign(result);
        return result;
    }

    bool isActive(const SimFlash &flash) const {
        return std::memcmp(flash.at(APPLICATION), bytes.data(), bytes.size()) == 0;
    }

    std::vector<uint8_t> bytes;
};

class SimLink final : public Transfer {
  public:
    SimLink(Installer &receiver, const Image &image) : receiver(receiver), image(image) {}

    bool read(uint32_t offset, uint8_t *buffer, size_t length) override {
        assert(offset <= image.bytes.size() && length <= image.bytes.size() - offset);
        std::memcpy(buffer, image.bytes.data() + offset, length);
        return true;
    }

    bool exchange(const uint8_t *request, size_t length, uint8_t (&reply)[REPLY_SIZE]) override {
        ++transmissions;
        const size_t messageLength = encodeFrame(request, length, wire);
        if (corruptFrame) {
            corruptFrame = false;
            wire[2] ^= 0x10;
        }
        size_t received = 0;
        for (size_t i = 0; i < messageLength; ++i) {
            received = receiverReader.push(wire[i]);
            assert(!received || i == messageLength - 1);
        }
        if (receivingAppKey) {
            if (received && authorizedBeginRequest(receiverReader.payload(), received, *receivingAppKey)) {
                ++applicationResets;
                receivingAppKey = nullptr;
            }
            return false;
        }
        uint8_t response[REPLY_SIZE];
        if (!received || !handleRequest(receiver, receiverReader.payload(), received, response))
            return false;
        if (dropReply == static_cast<Command>(request[0])) {
            dropReply = {};
            return false;
        }
        const size_t replyLength = encodeFrame(response, sizeof(response), wire);
        for (size_t i = 0; i < replyLength; ++i)
            received = senderReader.push(wire[i]);
        assert(received == REPLY_SIZE);
        std::memcpy(reply, senderReader.payload(), received);
        return true;
    }

    void progress(uint32_t sent, uint32_t total) override {
        assert(sent >= delivered && sent <= total);
        delivered = sent;
    }

    Installer &receiver;
    const Image &image;
    uint32_t delivered = 0;
    unsigned transmissions = 0;
    unsigned applicationResets = 0;
    const uint8_t (*receivingAppKey)[32] = nullptr;
    Command dropReply{};
    bool corruptFrame = false;
    uint8_t wire[MAX_ENCODED_FRAME];
    FrameReader receiverReader;
    FrameReader senderReader;
};

void installInitial(SimFlash &flash, const TestKey &key, const Image &image) {
    Installer receiver(flash, key.publicKey);
    assert(!receiver.prepareApplication());
    SimLink transfer(receiver, image);
    assert(sendImage(transfer, image.manifest(key)));
    assert(!image.isActive(flash));
    Installer rebooted(flash, key.publicKey);
    assert(rebooted.prepareApplication());
    assert(image.isActive(flash));
}

void testTransportAndValidation(const TestKey &key, const Image &current, const Image &update) {
    SimFlash flash;
    installInitial(flash, key, current);
    for (const Command command : {Command::Begin, Command::Write, Command::Finish}) {
        SimFlash copy = flash;
        Installer receiver(copy, key.publicKey);
        assert(receiver.prepareApplication());
        SimLink transfer(receiver, update);
        transfer.receivingAppKey = &key.publicKey;
        transfer.corruptFrame = true;
        transfer.dropReply = command;
        assert(sendImage(transfer, update.manifest(key)));
        assert(transfer.transmissions == 8 && transfer.applicationResets == 1);
        assert(transfer.delivered == update.bytes.size());
        assert(current.isActive(copy) && copy.bootloaderIntact());
        Installer rebooted(copy, key.publicKey);
        assert(rebooted.prepareApplication() && update.isActive(copy));
        const auto operations = copy.operations;
        assert(rebooted.prepareApplication() && copy.operations == operations);
    }

    Installer receiver(flash, key.publicKey);
    assert(receiver.prepareApplication());
    const auto originalOperations = flash.operations;
    Manifest signedImage = update.manifest(key);
    signedImage.bytes[SIGNATURE_OFFSET] ^= 1;
    assert(receiver.begin(signedImage) == Status::VerificationFailed);
    SimLink app(receiver, update);
    app.receivingAppKey = &key.publicKey;
    assert(!sendImage(app, signedImage) && app.applicationResets == 0);
    signedImage = update.manifest(key);
    signedImage.bytes[4] ^= 1;
    key.sign(signedImage);
    assert(receiver.begin(signedImage) == Status::VerificationFailed);
    signedImage = update.manifest(key);
    for (const uint32_t size : {0u, 9u, MAX_IMAGE + 4, UINT32_MAX}) {
        putU32(signedImage.bytes + sizeof(IMAGE_MAGIC), size);
        key.sign(signedImage);
        assert(receiver.begin(signedImage) == Status::VerificationFailed);
    }
    assert(flash.operations == originalOperations);

    assert(receiver.finish() == Status::BadState);
    assert(receiver.begin(update.manifest(key)) == Status::Ok);
    assert(receiver.write(UINT32_MAX, update.bytes.data(), 4) == Status::BadOffset);
    assert(receiver.write(4, update.bytes.data(), 4) == Status::BadOffset);
    assert(receiver.write(0, update.bytes.data(), CHUNK_SIZE + 4) == Status::BadOffset);
    assert(receiver.write(0, update.bytes.data(), 3) == Status::BadOffset);
    assert(receiver.write(0, update.bytes.data(), 4) == Status::Ok);
    assert(receiver.write(0, update.bytes.data() + 4, 4) == Status::BadOffset);
    assert(receiver.finish() == Status::BadState);
    Installer resetAfterPartialUpload(flash, key.publicKey);
    assert(resetAfterPartialUpload.prepareApplication() && current.isActive(flash));
    SimLink corrupted(resetAfterPartialUpload, current);
    Manifest forgedPayload = update.manifest(key);
    assert(!sendImage(corrupted, forgedPayload));
    Installer resetAfterCorruption(flash, key.publicKey);
    assert(resetAfterCorruption.prepareApplication() && current.isActive(flash));

    Image badVector = update;
    putU32(badVector.bytes.data(), 0);
    SimLink badStack(resetAfterCorruption, badVector);
    assert(!sendImage(badStack, badVector.manifest(key)));
    putU32(badVector.bytes.data(), SRAM_END);
    putU32(badVector.bytes.data() + 4, FLASH_BASE | 1);
    SimLink badEntry(resetAfterCorruption, badVector);
    assert(!sendImage(badEntry, badVector.manifest(key)));
    assert(current.isActive(flash));
}

void testEveryFlashInterruption(const TestKey &key, const Image &current, const Image &update) {
    SimFlash baseline;
    installInitial(baseline, key, current);
    const Manifest next = update.manifest(key);
    SimFlash uninterrupted = baseline;
    Installer receiver(uninterrupted, key.publicKey);
    SimLink transfer(receiver, update);
    assert(sendImage(transfer, next));
    Installer rebooted(uninterrupted, key.publicKey);
    assert(rebooted.prepareApplication());
    assert(update.isActive(uninterrupted));
    const size_t steps = uninterrupted.operations - baseline.operations;

    for (const Cut cut : {Cut::Before, Cut::During, Cut::After}) {
        for (size_t step = 1; step <= steps; ++step) {
            SimFlash flash = baseline;
            flash.failAt = baseline.operations + step;
            flash.cut = cut;
            try {
                Installer interrupted(flash, key.publicKey);
                SimLink link(interrupted, update);
                assert(sendImage(link, next));
                Installer boot(flash, key.publicKey);
                assert(boot.prepareApplication());
                assert(false && "the selected flash operation never ran");
            } catch (const PowerCut &) {
                flash.failAt = 0;
            }

            const uint8_t *staged = flash.at(STAGED_MANIFEST);
            const bool newStageCommitted = getU32(staged + MANIFEST_SIZE) == COMMITTED &&
                                           std::memcmp(staged, next.bytes, MANIFEST_SIZE) == 0;
            Installer powerRestored(flash, key.publicKey);
            assert(powerRestored.prepareApplication()); // UART and ESP32 are unavailable during recovery.
            assert((newStageCommitted ? update : current).isActive(flash));
            assert(flash.bootloaderIntact());

            if (!newStageCommitted) {
                SimLink retry(powerRestored, update);
                assert(sendImage(retry, next));
                Installer boot(flash, key.publicKey);
                assert(boot.prepareApplication() && update.isActive(flash));
            }
        }
    }
    std::printf("Recovered from %zu simulated power cuts across %zu flash operations.\n", steps * 3, steps);
}

void testFailedActivation(const TestKey &key, const Image &current, const Image &update) {
    SimFlash flash;
    installInitial(flash, key, current);
    Installer receiver(flash, key.publicKey);
    SimLink transfer(receiver, update);
    assert(sendImage(transfer, update.manifest(key)));

    flash.failingSector = 4;
    Installer failedBoot(flash, key.publicKey);
    assert(!failedBoot.prepareApplication());
    const auto operations = flash.operations;
    assert(failedBoot.begin(current.manifest(key)) == Status::BadState);
    assert(flash.operations == operations);

    flash.failingSector = 8;
    Installer recoveredBoot(flash, key.publicKey);
    assert(recoveredBoot.prepareApplication() && update.isActive(flash));
}

} // namespace

int main(int argc, char **argv) {
    const TestKey key;
    const Image current(0x59), update(0xb2);
    testTransportAndValidation(key, current, update);
    testEveryFlashInterruption(key, current, update);
    testFailedActivation(key, current, update);
    SimFlash fullFlash;
    installInitial(fullFlash, key, Image(0x17, MAX_IMAGE));
    if (argc >= 2) {
        std::ifstream file(argv[1], std::ios::binary);
        Image app(0, 8);
        app.bytes.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
        assert(!app.bytes.empty() && app.bytes.size() <= MAX_IMAGE);
        app.bytes.resize((app.bytes.size() + 3) & ~size_t{3}, 0xff);
        SimFlash flash;
        installInitial(flash, key, app);
        std::printf("Transferred and verified the relocated STM32 application binary (%zu bytes).\n", app.bytes.size());
    }
    if (argc == 3) {
        std::ifstream file(argv[2], std::ios::binary);
        Manifest manifest;
        file.read(reinterpret_cast<char *>(manifest.bytes), MANIFEST_SIZE);
        assert(file && manifest.valid(key.publicKey));
        Image app(0, 8);
        app.bytes.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
        assert(app.bytes.size() == manifest.imageSize());
        SimFlash flash;
        Installer receiver(flash, key.publicKey);
        SimLink transfer(receiver, app);
        assert(sendImage(transfer, manifest));
        Installer rebooted(flash, key.publicKey);
        assert(rebooted.prepareApplication() && app.isActive(flash));
        std::puts("Verified and installed the independently signed STM32 package.");
    }
    std::puts("STM32 OTA transport, authentication and recovery simulation passed.");
}
