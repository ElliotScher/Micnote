// A thin C++ layer over the PSA Crypto API (TF-PSA-Crypto), the same API
// the firmware uses: RAII key handles, and the handful of primitives the
// recording format needs (docs/stages/01-firmware-and-sync.md section 4.5).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

#include <psa/crypto.h>

namespace micnote::crypto {

inline constexpr std::size_t kX25519KeySize = 32;
inline constexpr std::size_t kAes256KeySize = 32;
inline constexpr std::size_t kSha256Size = 32;
inline constexpr std::size_t kCcmTagSize = 16;
inline constexpr std::size_t kCcmNonceSize = 13;

using Sha256Digest = std::array<std::byte, kSha256Size>;

// Initializes PSA once per process; safe from any thread. Every function
// below calls it, so callers never need to.
psa_status_t Init();

// Owns a PSA key ID and destroys it (wiping the key) when it goes away.
class Key {
 public:
  Key() = default;
  explicit Key(psa_key_id_t id) : id_(id) {}
  Key(const Key &) = delete;
  Key &operator=(const Key &) = delete;
  Key(Key &&other) noexcept : id_(other.id_) { other.id_ = PSA_KEY_ID_NULL; }
  Key &operator=(Key &&other) noexcept;
  ~Key() { static_cast<void>(psa_destroy_key(id_)); }

  [[nodiscard]] psa_key_id_t id() const { return id_; }

 private:
  psa_key_id_t id_ = PSA_KEY_ID_NULL;
};

// An X25519 private key usable for ECDH (raw key agreement).
std::expected<Key, psa_status_t> ImportX25519(std::span<const std::byte, kX25519KeySize> priv);

std::expected<std::array<std::byte, kX25519KeySize>, psa_status_t> PublicKey(const Key &key);

// An AES-256 key for CCM with a 16-byte tag.
std::expected<Key, psa_status_t> ImportAesCcm(std::span<const std::byte, kAes256KeySize> raw);

Sha256Digest Sha256(std::span<const std::byte> data);

// SHA-256 over data that arrives in pieces (a model file, a block at a
// time). Finish() once; the object is done after that.
class Sha256Stream {
 public:
  Sha256Stream();
  Sha256Stream(const Sha256Stream &) = delete;
  Sha256Stream &operator=(const Sha256Stream &) = delete;
  Sha256Stream(Sha256Stream &&) = delete;
  Sha256Stream &operator=(Sha256Stream &&) = delete;
  ~Sha256Stream();

  void Update(std::span<const std::byte> data);
  Sha256Digest Finish();

 private:
  psa_hash_operation_t op_ = psa_hash_operation_init();
};

// Wipes memory that held secrets, in a way the compiler can't optimize out.
void Wipe(std::span<std::byte> secret);

}  // namespace micnote::crypto
