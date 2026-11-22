#include "crypto/psa.hpp"

#include <mutex>

#include <mbedtls/platform_util.h>

namespace micnote::crypto {

psa_status_t Init() {
  static std::once_flag once;
  static psa_status_t status = PSA_ERROR_BAD_STATE;
  std::call_once(once, [] { status = psa_crypto_init(); });
  return status;
}

Key &Key::operator=(Key &&other) noexcept {
  if (this != &other) {
    static_cast<void>(psa_destroy_key(id_));
    id_ = other.id_;
    other.id_ = PSA_KEY_ID_NULL;
  }
  return *this;
}

namespace {

std::expected<Key, psa_status_t> Import(const psa_key_attributes_t &attr,
                                        std::span<const std::byte> raw) {
  psa_key_id_t id = PSA_KEY_ID_NULL;
  // GCOVR_EXCL_START: callers pass correctly sized keys of supported types
  // (any 32 bytes are a valid X25519 or AES-256 key), so this fails only if
  // Init() did or memory ran out, which no test can arrange.
  psa_status_t status = Init();
  if (status == PSA_SUCCESS) {
    status =
        psa_import_key(&attr, reinterpret_cast<const std::uint8_t *>(raw.data()), raw.size(), &id);
  }
  if (status != PSA_SUCCESS) {
    return std::unexpected(status);
  }
  // GCOVR_EXCL_STOP
  return Key(id);
}

}  // namespace

std::expected<Key, psa_status_t> ImportX25519(std::span<const std::byte, kX25519KeySize> priv) {
  psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
  psa_set_key_type(&attr, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_MONTGOMERY));
  psa_set_key_bits(&attr, 255);
  psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_DERIVE);
  psa_set_key_algorithm(&attr, PSA_ALG_ECDH);
  return Import(attr, priv);
}

std::expected<std::array<std::byte, kX25519KeySize>, psa_status_t> PublicKey(const Key &key) {
  std::array<std::byte, kX25519KeySize> pub{};
  std::size_t len = 0;
  const psa_status_t status = psa_export_public_key(
      key.id(), reinterpret_cast<std::uint8_t *>(pub.data()), pub.size(), &len);
  if (status != PSA_SUCCESS) {
    return std::unexpected(status);
  }
  return pub;
}

std::expected<Key, psa_status_t> ImportAesCcm(std::span<const std::byte, kAes256KeySize> raw) {
  psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
  psa_set_key_type(&attr, PSA_KEY_TYPE_AES);
  psa_set_key_bits(&attr, 256);
  psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT);
  psa_set_key_algorithm(&attr, PSA_ALG_CCM);
  return Import(attr, raw);
}

Sha256Digest Sha256(std::span<const std::byte> data) {
  Sha256Digest digest{};
  std::size_t len = 0;
  // GCOVR_EXCL_START: SHA-256 is always built in, so after Init() succeeds
  // psa_hash_compute can't fail; a zero digest would then fail every MAC.
  if (Init() != PSA_SUCCESS ||
      psa_hash_compute(PSA_ALG_SHA_256, reinterpret_cast<const std::uint8_t *>(data.data()),
                       data.size(), reinterpret_cast<std::uint8_t *>(digest.data()), digest.size(),
                       &len) != PSA_SUCCESS) {
    return {};
  }
  // GCOVR_EXCL_STOP
  return digest;
}

void Wipe(std::span<std::byte> secret) { mbedtls_platform_zeroize(secret.data(), secret.size()); }

}  // namespace micnote::crypto
