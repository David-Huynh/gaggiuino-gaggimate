#pragma once

#include <cstdint>
#include <ctime>
#include <type_traits>

namespace EpochTime {

using Seconds = std::int64_t;
static_assert(sizeof(Seconds) == 8, "absolute epoch values require a 64-bit representation");

inline constexpr Seconds MIN_VALID = 1600000000LL;
inline constexpr Seconds MAX_FUTURE_OFFSET = 365LL * 24LL * 60LL * 60LL;

inline Seconds fromTimeT(std::time_t value) {
    if constexpr (sizeof(std::time_t) <= sizeof(std::uint32_t)) {
        // ESP-IDF 4 exposes signed 32-bit time_t. Reinterpret its epoch bits as
        // unsigned so dates after 2038 survive until the toolchain moves to the
        // native 64-bit time_t ABI provided by ESP-IDF 5 and later.
        if (value == static_cast<std::time_t>(-1)) {
            return 0;
        }
        using UnsignedTime = std::make_unsigned_t<std::time_t>;
        return static_cast<Seconds>(static_cast<UnsignedTime>(value));
    }
    return value > 0 ? static_cast<Seconds>(value) : 0;
}

inline Seconds now() { return fromTimeT(std::time(nullptr)); }

inline bool plausible(Seconds value, Seconds reference = now()) {
    if (value < MIN_VALID) {
        return false;
    }
    return reference < MIN_VALID || value <= reference || value - reference <= MAX_FUTURE_OFFSET;
}

} // namespace EpochTime
