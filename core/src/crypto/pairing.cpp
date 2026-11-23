#include "crypto/pairing.hpp"

#include <algorithm>

namespace micnote::crypto {

namespace {

constexpr std::string_view kLinkLabel = "micnote/v1/link";
constexpr std::string_view kSasLabel = "micnote/v1/sas";

const std::uint8_t *In(std::span<const std::byte> s) {
  return reinterpret_cast<const std::uint8_t *>(s.data());
}
std::uint8_t *Out(std::span<std::byte> s) { return reinterpret_cast<std::uint8_t *>(s.data()); }

// HKDF-SHA256 of the secret key into out (PSA's input order: salt, secret,
// info).
bool Hkdf(const Key &secret, std::span<const std::byte> salt, std::span<const std::byte> info,
          std::span<std::byte> out) {
  psa_key_derivation_operation_t op = PSA_KEY_DERIVATION_OPERATION_INIT;
  // GCOVR_EXCL_START: with a DERIVE key and these sizes, HKDF fails only on
  // resource exhaustion, which no test can arrange.
  const bool ok = psa_key_derivation_setup(&op, PSA_ALG_HKDF(PSA_ALG_SHA_256)) == PSA_SUCCESS &&
                  psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_SALT, In(salt),
                                                 salt.size()) == PSA_SUCCESS &&
                  psa_key_derivation_input_key(&op, PSA_KEY_DERIVATION_INPUT_SECRET, secret.id()) ==
                      PSA_SUCCESS &&
                  psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_INFO, In(info),
                                                 info.size()) == PSA_SUCCESS &&
                  psa_key_derivation_output_bytes(&op, Out(out), out.size()) == PSA_SUCCESS;
  // GCOVR_EXCL_STOP
  static_cast<void>(psa_key_derivation_abort(&op));
  return ok;
}

std::array<std::byte, 4 + (2 * kNonceSize)> ProofInput(std::string_view label, const Nonce &n_h,
                                                       const Nonce &n_d) {
  std::array<std::byte, 4 + (2 * kNonceSize)> in{};
  std::ranges::copy(std::as_bytes(std::span(label).first(4)), in.begin());
  std::ranges::copy(n_h, in.begin() + 4);
  std::ranges::copy(n_d, in.begin() + 4 + kNonceSize);
  return in;
}

// The link key as a PSA HMAC key for one MAC operation.
Key HmacKey(const LinkKey &link_key) {
  psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
  psa_set_key_type(&attr, PSA_KEY_TYPE_HMAC);
  psa_set_key_bits(&attr, 256);
  psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_SIGN_MESSAGE | PSA_KEY_USAGE_VERIFY_MESSAGE);
  psa_set_key_algorithm(&attr, PSA_ALG_HMAC(PSA_ALG_SHA_256));
  psa_key_id_t id = PSA_KEY_ID_NULL;
  // An import failure leaves id null, and the MAC call after it fails.
  static_cast<void>(Init());
  static_cast<void>(psa_import_key(&attr, In(link_key), link_key.size(), &id));
  return Key(id);
}

}  // namespace

std::expected<Pairing, PairError> DerivePairing(
    const Key &host_priv, std::span<const std::byte, kX25519KeySize> host_pub,
    std::span<const std::byte, kX25519KeySize> device_pub, std::span<const std::byte, 8> host_id,
    std::span<const std::byte, 8> device_id, const Nonce &n_h, const Nonce &n_d) {
  std::array<std::byte, kX25519KeySize> ss{};
  std::size_t len = 0;
  const psa_status_t agreed = psa_raw_key_agreement(PSA_ALG_ECDH, host_priv.id(), In(device_pub),
                                                    device_pub.size(), Out(ss), ss.size(), &len);
  if (agreed == PSA_ERROR_INVALID_ARGUMENT) {
    return std::unexpected(PairError::kLowOrder);
  }
  // GCOVR_EXCL_START: a valid X25519 key and 32 bytes of peer key can't fail
  // otherwise. A provider that returned an all-zero secret without refusing
  // the key is caught here too (Mbed TLS refuses it above).
  if (std::ranges::all_of(ss, [](std::byte b) { return b == std::byte{0}; })) {
    return std::unexpected(PairError::kLowOrder);
  }
  if (agreed != PSA_SUCCESS || len != ss.size()) {
    return std::unexpected(PairError::kCrypto);
  }
  // GCOVR_EXCL_STOP

  psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
  psa_set_key_type(&attr, PSA_KEY_TYPE_DERIVE);
  psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_DERIVE);
  psa_set_key_algorithm(&attr, PSA_ALG_HKDF(PSA_ALG_SHA_256));
  psa_key_id_t secret_id = PSA_KEY_ID_NULL;
  const psa_status_t imported = psa_import_key(&attr, In(ss), ss.size(), &secret_id);
  const Key secret(secret_id);
  Wipe(ss);

  std::array<std::byte, 16> link_salt{};
  std::ranges::copy(host_id, link_salt.begin());
  std::ranges::copy(device_id, link_salt.begin() + 8);
  std::array<std::byte, 2 * kNonceSize> sas_salt{};
  std::ranges::copy(n_h, sas_salt.begin());
  std::ranges::copy(n_d, sas_salt.begin() + kNonceSize);
  std::array<std::byte, kSasLabel.size() + (2 * kX25519KeySize)> sas_info{};
  std::ranges::copy(std::as_bytes(std::span(kSasLabel)), sas_info.begin());
  std::ranges::copy(host_pub, sas_info.begin() + kSasLabel.size());
  std::ranges::copy(device_pub, sas_info.begin() + kSasLabel.size() + kX25519KeySize);

  Pairing out;
  std::array<std::byte, kSasSymbols> sas{};
  // GCOVR_EXCL_START: see Hkdf().
  if (imported != PSA_SUCCESS ||
      !Hkdf(secret, link_salt, std::as_bytes(std::span(kLinkLabel)), out.link_key) ||
      !Hkdf(secret, sas_salt, sas_info, sas)) {
    Wipe(out.link_key);
    return std::unexpected(PairError::kCrypto);
  }
  // GCOVR_EXCL_STOP
  for (std::size_t i = 0; i < kSasSymbols; ++i) {
    out.sas.at(i) = static_cast<std::uint8_t>(std::to_integer<unsigned>(sas.at(i)) % kSasColors);
  }
  return out;
}

Proof MakeProof(const LinkKey &link_key, std::string_view label, const Nonce &n_h,
                const Nonce &n_d) {
  const Key key = HmacKey(link_key);
  const auto in = ProofInput(label, n_h, n_d);
  Proof proof{};
  std::size_t len = 0;
  // A failure (which only resource exhaustion causes) leaves the proof all
  // zeros, which the other side then refuses.
  static_cast<void>(psa_mac_compute(key.id(), PSA_ALG_HMAC(PSA_ALG_SHA_256), In(in), in.size(),
                                    Out(proof), proof.size(), &len));
  return proof;
}

bool VerifyProof(const LinkKey &link_key, std::string_view label, const Nonce &n_h,
                 const Nonce &n_d, std::span<const std::byte> proof) {
  const Key key = HmacKey(link_key);
  const auto in = ProofInput(label, n_h, n_d);
  return psa_mac_verify(key.id(), PSA_ALG_HMAC(PSA_ALG_SHA_256), In(in), in.size(), In(proof),
                        proof.size()) == PSA_SUCCESS;
}

std::string_view SasColorName(std::uint8_t symbol) {
  constexpr std::array<std::string_view, kSasColors> kNames{"red",    "green", "blue",
                                                            "yellow", "cyan",  "magenta"};
  return symbol < kSasColors ? kNames.at(symbol) : "?";
}

}  // namespace micnote::crypto
