#include "recording/reader.hpp"

#include <algorithm>
#include <string_view>
#include <utility>

namespace micnote::recording {

namespace {

// PSA takes uint8_t buffers; these views keep the casts in one place.
const std::uint8_t *In(std::span<const std::byte> s) {
  return reinterpret_cast<const std::uint8_t *>(s.data());
}
std::uint8_t *Out(std::span<std::byte> s) { return reinterpret_cast<std::uint8_t *>(s.data()); }

constexpr std::string_view kKekLabel = "micnote/v1/kek";

void StoreLe(std::uint64_t value, std::span<std::byte> out) {
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = static_cast<std::byte>(value >> (8U * i));
  }
}

// AES-256-CCM, nonce rec_id | seq | 0x00, AAD SHA-256(core) | seq.
std::expected<std::vector<std::byte>, ReadError> OpenChunk(const crypto::Key &cek,
                                                           std::uint64_t rec_id, std::uint32_t seq,
                                                           const crypto::Sha256Digest &core_hash,
                                                           std::span<const std::byte> sealed) {
  std::array<std::byte, crypto::kCcmNonceSize> nonce{};
  StoreLe(rec_id, std::span(nonce).first(8));
  StoreLe(seq, std::span(nonce).subspan(8, 4));
  std::array<std::byte, crypto::kSha256Size + 4> aad{};
  std::ranges::copy(core_hash, aad.begin());
  StoreLe(seq, std::span(aad).subspan(crypto::kSha256Size));

  std::vector<std::byte> pt(sealed.size() - crypto::kCcmTagSize);
  std::size_t len = 0;
  const psa_status_t status =
      psa_aead_decrypt(cek.id(), PSA_ALG_CCM, In(nonce), nonce.size(), In(aad), aad.size(),
                       In(sealed), sealed.size(), Out(pt), pt.size(), &len);
  if (status == PSA_ERROR_INVALID_SIGNATURE) {
    return std::unexpected(ReadError::kChunkAuth);
  }
  // GCOVR_EXCL_START: the key and buffer sizes are right by construction.
  if (status != PSA_SUCCESS) {
    return std::unexpected(ReadError::kCrypto);
  }
  // GCOVR_EXCL_STOP
  return pt;
}

}  // namespace

// Mirrors firmware/app/src/crypto/wrap.c.
std::expected<std::array<std::byte, crypto::kAes256KeySize>, ReadError> UnwrapCek(
    const HostKey &host, std::uint64_t rec_id, const Wrap &w) {
  std::array<std::byte, crypto::kX25519KeySize> ss{};
  std::size_t len = 0;
  const psa_status_t agreed = psa_raw_key_agreement(PSA_ALG_ECDH, host.key().id(), In(w.eph_pub),
                                                    w.eph_pub.size(), Out(ss), ss.size(), &len);
  if (agreed == PSA_ERROR_INVALID_ARGUMENT) {
    // A low-order eph_pub: the shared secret, and so the KEK, would be public.
    return std::unexpected(ReadError::kUnwrap);
  }

  std::array<std::byte, 8 + crypto::kX25519KeySize> salt{};
  StoreLe(rec_id, std::span(salt).first(8));
  std::ranges::copy(w.eph_pub, salt.begin() + 8);
  std::array<std::byte, kKekLabel.size() + 8> info{};
  std::ranges::copy(std::as_bytes(std::span(kKekLabel)), info.begin());
  std::ranges::copy(w.host_id, info.begin() + kKekLabel.size());

  psa_key_attributes_t secret_attr = PSA_KEY_ATTRIBUTES_INIT;
  psa_set_key_type(&secret_attr, PSA_KEY_TYPE_DERIVE);
  psa_set_key_usage_flags(&secret_attr, PSA_KEY_USAGE_DERIVE);
  psa_set_key_algorithm(&secret_attr, PSA_ALG_HKDF(PSA_ALG_SHA_256));
  psa_key_attributes_t kek_attr = PSA_KEY_ATTRIBUTES_INIT;
  psa_set_key_type(&kek_attr, PSA_KEY_TYPE_AES);
  psa_set_key_bits(&kek_attr, 256);
  psa_set_key_usage_flags(&kek_attr, PSA_KEY_USAGE_DECRYPT);
  psa_set_key_algorithm(&kek_attr, PSA_ALG_CCM);

  psa_key_id_t secret_id = PSA_KEY_ID_NULL;
  psa_key_id_t kek_id = PSA_KEY_ID_NULL;
  psa_key_derivation_operation_t op = PSA_KEY_DERIVATION_OPERATION_INIT;
  // PSA derives a key only from a secret that is itself a key, hence the
  // DERIVE import; HKDF's inputs must come in this order.
  // GCOVR_EXCL_START: with a valid host key and a non-low-order eph_pub,
  // these calls fail only on resource exhaustion, which no test can arrange.
  // A provider that returned an all-zero secret without refusing the key is
  // caught here too (Mbed TLS refuses it above).
  const bool derived =
      agreed == PSA_SUCCESS && len == ss.size() &&
      !std::ranges::all_of(ss, [](std::byte b) { return b == std::byte{0}; }) &&
      psa_import_key(&secret_attr, In(ss), ss.size(), &secret_id) == PSA_SUCCESS &&
      psa_key_derivation_setup(&op, PSA_ALG_HKDF(PSA_ALG_SHA_256)) == PSA_SUCCESS &&
      psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_SALT, In(salt), salt.size()) ==
          PSA_SUCCESS &&
      psa_key_derivation_input_key(&op, PSA_KEY_DERIVATION_INPUT_SECRET, secret_id) ==
          PSA_SUCCESS &&
      psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_INFO, In(info), info.size()) ==
          PSA_SUCCESS &&
      psa_key_derivation_output_key(&kek_attr, &op, &kek_id) == PSA_SUCCESS;
  // GCOVR_EXCL_STOP
  static_cast<void>(psa_key_derivation_abort(&op));
  const crypto::Key secret(secret_id);
  const crypto::Key kek(kek_id);
  crypto::Wipe(ss);
  // GCOVR_EXCL_START: see above.
  if (!derived) {
    return std::unexpected(ReadError::kCrypto);
  }
  // GCOVR_EXCL_STOP

  std::array<std::byte, crypto::kAes256KeySize + crypto::kCcmTagSize> sealed{};
  std::ranges::copy(w.wrapped_cek, sealed.begin());
  std::ranges::copy(w.tag, sealed.begin() + crypto::kAes256KeySize);
  std::array<std::byte, crypto::kAes256KeySize> cek{};
  constexpr std::array<std::uint8_t, crypto::kCcmNonceSize> kZeroNonce{};
  const psa_status_t opened =
      psa_aead_decrypt(kek.id(), PSA_ALG_CCM, kZeroNonce.data(), kZeroNonce.size(), In(w.host_id),
                       w.host_id.size(), In(sealed), sealed.size(), Out(cek), cek.size(), &len);
  if (opened == PSA_ERROR_INVALID_SIGNATURE) {
    return std::unexpected(ReadError::kUnwrap);
  }
  // GCOVR_EXCL_START: the KEK and buffer sizes are right by construction.
  if (opened != PSA_SUCCESS) {
    crypto::Wipe(cek);
    return std::unexpected(ReadError::kCrypto);
  }
  // GCOVR_EXCL_STOP
  return cek;
}

HostKey::HostKey(crypto::Key key, const std::array<std::byte, crypto::kX25519KeySize> &pub)
    : key_(std::move(key)), pub_(pub) {
  const crypto::Sha256Digest digest = crypto::Sha256(pub_);
  std::ranges::copy(std::span(digest).first(host_id_.size()), host_id_.begin());
}

std::expected<HostKey, psa_status_t> HostKey::FromPrivate(
    std::span<const std::byte, crypto::kX25519KeySize> priv) {
  auto key = crypto::ImportX25519(priv);
  // GCOVR_EXCL_START: importing 32 bytes as X25519 can't fail (see psa.cpp),
  // and nor can exporting a key pair's public half.
  if (!key) {
    return std::unexpected(key.error());
  }
  auto pub = crypto::PublicKey(*key);
  if (!pub) {
    return std::unexpected(pub.error());
  }
  // GCOVR_EXCL_STOP
  return HostKey(std::move(*key), *pub);
}

std::uint32_t Recording::DurationMs() const {
  if (trailer) {
    return trailer->duration_ms;
  }
  return static_cast<std::uint32_t>(packets.size()) * header.frame_ms;
}

std::expected<void, ReadError> ParsePlaintext(std::span<const std::byte> pt, Recording &into) {
  if (pt.empty() || into.trailer) {
    // Nothing may follow the trailer.
    return std::unexpected(ReadError::kPlaintext);
  }
  const auto type = std::to_integer<std::uint8_t>(pt[0]);
  if (type == kChunkTrailer) {
    if (pt.size() != kTrailerSize) {
      return std::unexpected(ReadError::kPlaintext);
    }
    const Trailer t{.duration_ms = LoadLe32(pt.subspan(1)),
                    .marker_count = LoadLe32(pt.subspan(5))};
    if (t.marker_count != into.markers.size()) {
      return std::unexpected(ReadError::kPlaintext);
    }
    into.trailer = t;
    return {};
  }
  if (type != kChunkAudio) {
    return std::unexpected(ReadError::kPlaintext);
  }
  for (std::size_t off = 1; off < pt.size();) {
    if (pt.size() - off < 2) {
      return std::unexpected(ReadError::kPlaintext);
    }
    const std::uint16_t len = LoadLe16(pt.subspan(off));
    if (len == kMarkerSentinel) {
      if (pt.size() - off < kMarkerSize) {
        return std::unexpected(ReadError::kPlaintext);
      }
      into.markers.push_back({.type = std::to_integer<std::uint8_t>(pt[off + 2]),
                              .offset_ms = LoadLe32(pt.subspan(off + 3))});
      off += kMarkerSize;
      continue;
    }
    if (len == 0 || pt.size() - off - 2 < len) {
      return std::unexpected(ReadError::kPlaintext);
    }
    const auto packet = pt.subspan(off + 2, len);
    into.packets.emplace_back(packet.begin(), packet.end());
    off += 2 + std::size_t{len};
  }
  return {};
}

std::expected<Recording, ReadError> ReadRecording(std::span<const std::byte> file,
                                                  const HostKey &key) {
  auto header = ParseHeader(file);
  if (!header) {
    return std::unexpected(ReadError::kFormat);
  }
  Recording rec;
  rec.header = std::move(*header);

  const auto wrap = std::ranges::find(rec.header.wraps, key.host_id(), &Wrap::host_id);
  if (wrap == rec.header.wraps.end()) {
    return std::unexpected(ReadError::kNotARecipient);
  }
  auto raw_cek = UnwrapCek(key, rec.header.rec_id, *wrap);
  if (!raw_cek) {
    return std::unexpected(raw_cek.error());
  }
  auto cek = crypto::ImportAesCcm(*raw_cek);
  crypto::Wipe(*raw_cek);
  // GCOVR_EXCL_START: importing 32 bytes as AES-256 can't fail (psa.cpp).
  if (!cek) {
    return std::unexpected(ReadError::kCrypto);
  }
  // GCOVR_EXCL_STOP
  const crypto::Sha256Digest core_hash = crypto::Sha256(file.first(kCoreSize));

  for (std::size_t off = rec.header.length; off < file.size(); ++rec.chunks) {
    if (file.size() - off < kChunkHeaderSize) {
      return std::unexpected(ReadError::kChunkFraming);
    }
    const auto ch = ParseChunkHeader(file.subspan(off).first<kChunkHeaderSize>());
    if (!ch || ch->seq != rec.chunks) {
      return std::unexpected(ReadError::kChunkFraming);
    }
    const std::size_t sealed_len = std::size_t{ch->ct_len} + kTagSize;
    if (file.size() - off - kChunkHeaderSize < sealed_len) {
      return std::unexpected(ReadError::kChunkFraming);
    }
    auto pt = OpenChunk(*cek, rec.header.rec_id, ch->seq, core_hash,
                        file.subspan(off + kChunkHeaderSize, sealed_len));
    if (!pt) {
      return std::unexpected(pt.error());
    }
    const auto parsed = ParsePlaintext(*pt, rec);
    crypto::Wipe(*pt);
    if (!parsed) {
      return std::unexpected(parsed.error());
    }
    off += kChunkHeaderSize + sealed_len;
  }
  return rec;
}

}  // namespace micnote::recording
