#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

// The AsyncTCP receive callback and main-loop expiry run on different tasks.
// No references into this collection may escape the lock: a completed request
// is moved to its caller before parsing, filesystem access or event dispatch.
template <typename String = std::string> class WebSocketRequestBuffer {
  public:
    enum class Result { Incomplete, Complete, Rejected };
    static constexpr size_t MAX_MESSAGE_BYTES = 64 * 1024;

    explicit WebSocketRequestBuffer(size_t maxClients) : maxClients(maxClients) {}

    // Frame has the metadata supplied by ESPAsyncWebServer's AwsFrameInfo.
    template <typename Frame>
    Result append(uint32_t client, const Frame &frame, const uint8_t *data, size_t size,
                  uint32_t now, String &completed) {
        std::lock_guard<std::mutex> guard(mutex);
        // Text only; check lengths before reserving or appending any storage.
        if (frame.message_opcode != 1 || frame.len > MAX_MESSAGE_BYTES || frame.index > frame.len ||
            size > frame.len - frame.index || (size != 0 && data == nullptr)) {
            pending.erase(client);
            return Result::Rejected;
        }
        if (frame.num == 0 && frame.index == 0) {
            pending.erase(client);
            if (pending.size() >= maxClients) return Result::Rejected;
            pending.try_emplace(client);
        }
        auto it = pending.find(client);
        if (it == pending.end()) return Result::Rejected;
        auto &entry = it->second;
        if (frame.num != entry.frame || frame.index != entry.offset ||
            size > MAX_MESSAGE_BYTES - entry.payload.size()) {
            pending.erase(it);
            return Result::Rejected;
        }
        entry.lastActivity = now;
        if (size) entry.payload.append(reinterpret_cast<const char *>(data), size);
        entry.offset += size;
        if (entry.offset != frame.len) return Result::Incomplete;
        if (!frame.final) {
            ++entry.frame;
            entry.offset = 0;
            return Result::Incomplete;
        }
        completed = std::move(entry.payload);
        pending.erase(it);
        return Result::Complete;
    }

    void disconnect(uint32_t client) {
        std::lock_guard<std::mutex> guard(mutex);
        pending.erase(client);
    }

    size_t expire(uint32_t now, uint32_t timeout) {
        std::lock_guard<std::mutex> guard(mutex);
        size_t count = 0;
        for (auto it = pending.begin(); it != pending.end();) {
            if (static_cast<uint32_t>(now - it->second.lastActivity) > timeout) {
                it = pending.erase(it);
                ++count;
            } else {
                ++it;
            }
        }
        return count;
    }

  private:
    struct Entry {
        String payload;
        uint32_t frame = 0;
        uint64_t offset = 0;
        uint32_t lastActivity = 0;
    };
    const size_t maxClients;
    std::mutex mutex;
    std::unordered_map<uint32_t, Entry> pending;
};
