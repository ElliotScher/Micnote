// Pairing and session crypto, the host's side (docs/protocol.md sections 3
// and 8). The puck's is firmware/app/src/crypto/pairing.c; both are checked
// against the same known answers.
//
//   ss       = X25519(host_priv, device_pub)
//   link_key = HKDF-SHA256(ss, salt = host_id | device_id,
//                          info = "micnote/v1/link", L = 32)
//   sas      = HKDF-SHA256(ss, salt = n_h | n_d,
//                          info = "micnote/v1/sas" | host_pub | device_pub, L = 4)
//   symbol i = sas[i] mod 6
//   proof    = HMAC-SHA256(link_key, "puck" or "host" | n_h | n_d)
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

#include "crypto/psa.hpp"

namespace micnote::crypto {

inline constexpr std::size_t kNonceSize = 16;
inline constexpr std::size_t kLinkKeySize = 32;
inline constexpr std::size_t kProofSize = 32;
inline constexpr std::size_t kSasSymbols = 4;
inline constexpr std::uint8_t kSasColors = 6;
inline constexpr std::string_view kProofPuck = "puck";
inline constexpr std::string_view kProofHost = "host";

using Nonce = std::array<std::byte, kNonceSize>;
using LinkKey = std::array<std::byte, kLinkKeySize>;
using Proof = std::array<std::byte, kProofSize>;
using Sas = std::array<std::uint8_t, kSasSymbols>;

struct Pairing {
  LinkKey link_key{};
  Sas sas{};
};

enum class PairError : std::uint8_t {
  // device_pub is a low-order point: the shared secret would be public.
  kLowOrder,
  // A PSA call failed.
  kCrypto,
};

std::expected<Pairing, PairError> DerivePairing(
    const Key &host_priv, std::span<const std::byte, kX25519KeySize> host_pub,
    std::span<const std::byte, kX25519KeySize> device_pub, std::span<const std::byte, 8> host_id,
    std::span<const std::byte, 8> device_id, const Nonce &n_h, const Nonce &n_d);

// label is kProofPuck or kProofHost.
Proof MakeProof(const LinkKey &link_key, std::string_view label, const Nonce &n_h,
                const Nonce &n_d);

// In constant time.
bool VerifyProof(const LinkKey &link_key, std::string_view label, const Nonce &n_h,
                 const Nonce &n_d, std::span<const std::byte> proof);

// "red", "green", "blue", "yellow", "cyan", "magenta" for symbols 0-5.
std::string_view SasColorName(std::uint8_t symbol);

}  // namespace micnote::crypto
