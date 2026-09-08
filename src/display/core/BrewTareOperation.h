#pragma once

#include <cstdint>
#include <mutex>

// A single brew-start prerequisite. Telemetry is deliberately not an input:
// only the result of this request may authorize a start. No hardware/UI imports.
class BrewTareOperation {
  public:
    enum class Outcome { IDLE, WAITING, SUCCEEDED, FAILED, TIMED_OUT };
    static constexpr uint32_t TIMEOUT_MS = 4000;

    uint32_t begin(uint32_t now) {
        std::lock_guard<std::mutex> lock(mutex);
        if (state != Outcome::IDLE)
            return 0;
        if (++nextId == 0)
            ++nextId;
        requestId = nextId;
        startedAt = now;
        state = Outcome::WAITING;
        return requestId;
    }

    bool complete(uint32_t id, bool success, uint32_t now) {
        std::lock_guard<std::mutex> lock(mutex);
        if (state != Outcome::WAITING || id == 0 || id != requestId)
            return false;
        state = now - startedAt >= TIMEOUT_MS ? Outcome::TIMED_OUT
                                             : success ? Outcome::SUCCEEDED : Outcome::FAILED;
        return true;
    }

    Outcome outcome(uint32_t now) {
        std::lock_guard<std::mutex> lock(mutex);
        if ((state == Outcome::WAITING || state == Outcome::SUCCEEDED) && now - startedAt >= TIMEOUT_MS)
            state = Outcome::TIMED_OUT;
        return state;
    }

    bool pending() const {
        std::lock_guard<std::mutex> lock(mutex);
        return state != Outcome::IDLE;
    }

    bool cancel() {
        std::lock_guard<std::mutex> lock(mutex);
        const bool wasPending = state != Outcome::IDLE;
        state = Outcome::IDLE;
        return wasPending;
    }

    // Randomized by the device once at boot; avoids reusing IDs after a restart.
    void seed(uint32_t value) {
        std::lock_guard<std::mutex> lock(mutex);
        if (state == Outcome::IDLE)
            nextId = value;
    }

  private:
    mutable std::mutex mutex;
    Outcome state = Outcome::IDLE;
    uint32_t nextId = 0;
    uint32_t requestId = 0;
    uint32_t startedAt = 0;
};
