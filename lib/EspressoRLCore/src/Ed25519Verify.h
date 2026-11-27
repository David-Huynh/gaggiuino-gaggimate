#ifndef ESPRESSORL_CORE_ED25519_VERIFY_H
#define ESPRESSORL_CORE_ED25519_VERIFY_H

#include <cstddef>
#include <cstdint>

namespace EspressoRL {

constexpr std::size_t ED25519_PUBLIC_KEY_BYTES = 32;
constexpr std::size_t ED25519_SIGNATURE_BYTES = 64;

/// Pure-C++17 RFC 8032 Ed25519 signature verification (zero external/framework deps, zero heap).
/// Enforces strict canonical encoding (y < 2^255 - 19, canonical sign bit, and S < L).
bool ed25519Verify(const std::uint8_t publicKey[ED25519_PUBLIC_KEY_BYTES], const std::uint8_t *message,
                   std::size_t messageLen, const std::uint8_t signature[ED25519_SIGNATURE_BYTES]);

} // namespace EspressoRL

#endif // ESPRESSORL_CORE_ED25519_VERIFY_H
