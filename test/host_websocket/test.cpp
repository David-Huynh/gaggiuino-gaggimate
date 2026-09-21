#include <display/util/WebSocketRequestBuffer.h>
#include <atomic>
#include <cassert>
#include <iostream>
#include <thread>
#include <vector>

struct Frame {
    unsigned message_opcode = 1;
    uint32_t num = 0;
    bool final = true;
    uint64_t len = 4;
    uint64_t index = 0;
};
using Buffer = WebSocketRequestBuffer<>;
using Result = Buffer::Result;
const auto *bytes = reinterpret_cast<const uint8_t *>("abcd");

int main() {
    Buffer buffer(3);
    std::string output;
    Frame frame;
    assert(buffer.append(1, frame, bytes, 2, 10, output) == Result::Incomplete);
    frame.index = 2;
    assert(buffer.append(1, frame, bytes + 2, 2, 11, output) == Result::Complete);
    assert(output == "abcd");
    buffer.disconnect(1);
    buffer.expire(9999, 1);
    assert(output == "abcd"); // Dispatch owns its data, even after cleanup.

    frame = Frame{};
    frame.final = false;
    assert(buffer.append(1, frame, bytes, 4, 10, output) == Result::Incomplete);
    frame.num = 1;
    frame.final = true;
    assert(buffer.append(1, frame, bytes, 4, 11, output) == Result::Complete);
    assert(output == "abcdabcd");

    frame = Frame{};
    frame.len = Buffer::MAX_MESSAGE_BYTES + 1;
    assert(buffer.append(1, frame, bytes, 4, 0, output) == Result::Rejected);
    frame = Frame{};
    frame.message_opcode = 2;
    assert(buffer.append(1, frame, bytes, 4, 0, output) == Result::Rejected);
    frame = Frame{};
    frame.index = 1; // No matching initial fragment.
    assert(buffer.append(1, frame, bytes, 2, 0, output) == Result::Rejected);
    frame.index = UINT64_MAX;
    assert(buffer.append(1, frame, bytes, 2, 0, output) == Result::Rejected);

    frame = Frame{};
    for (unsigned id = 1; id <= 3; ++id)
        assert(buffer.append(id, frame, bytes, 2, UINT32_MAX - 5, output) == Result::Incomplete);
    assert(buffer.append(4, frame, bytes, 2, 0, output) == Result::Rejected);
    assert(buffer.expire(4, 9) == 3); // millis() rollover.
    assert(buffer.expire(4, 9) == 0);

    std::string large(Buffer::MAX_MESSAGE_BYTES, 'x');
    frame.len = large.size();
    assert(buffer.append(1, frame, reinterpret_cast<const uint8_t *>(large.data()), large.size(), 0, output) == Result::Complete);
    assert(output == large);
    frame.final = false;
    assert(buffer.append(1, frame, reinterpret_cast<const uint8_t *>(large.data()), large.size(), 0, output) == Result::Incomplete);
    frame = Frame{};
    frame.num = 1;
    assert(buffer.append(1, frame, bytes, 4, 0, output) == Result::Rejected); // Total across frames.

    // Concurrent arrivals/reconnects and loop-task cleanup must never share an
    // unprotected map iterator or invalidate a completed payload.
    std::atomic<bool> running{true};
    std::thread cleaner([&] {
        while (running) {
            buffer.expire(1000, 1);
            buffer.disconnect(2);
        }
    });
    std::vector<std::thread> readers;
    for (uint32_t id = 1; id <= 3; ++id) readers.emplace_back([&, id] {
        for (unsigned i = 0; i < 10000; ++i) {
            std::string message;
            Frame chunk;
            buffer.append(id, chunk, bytes, 2, 1, message);
            chunk.index = 2;
            const auto result = buffer.append(id, chunk, bytes + 2, 2, 1, message);
            if (result == Result::Complete) assert(message == "abcd");
            buffer.disconnect(id);
        }
    });
    for (auto &reader : readers) reader.join();
    running = false;
    cleaner.join();
    std::cout << "PASS WebSocket fragments, bounds, disconnect, expiry and 30000 concurrent requests\n";
}
