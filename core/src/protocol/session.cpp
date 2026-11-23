#include "protocol/session.hpp"

#include <algorithm>
#include <string_view>

namespace micnote::protocol {

std::array<std::byte, 8> DeviceId(const micnote_v1_HelloAck &hello) {
  std::array<std::byte, 8> id{};
  std::ranges::transform(hello.device_info.device_id, id.begin(),
                         [](pb_byte_t b) { return std::byte{b}; });
  return id;
}

crypto::Nonce DeviceNonce(const micnote_v1_HelloAck &hello) {
  crypto::Nonce n{};
  std::ranges::transform(hello.device_nonce, n.begin(), [](pb_byte_t b) { return std::byte{b}; });
  return n;
}

std::string DeviceIdHex(std::span<const std::byte, 8> id) {
  constexpr std::string_view kDigits = "0123456789abcdef";
  std::string s;
  for (const std::byte b : id) {
    s += kDigits.at(std::to_integer<unsigned>(b) >> 4U);
    s += kDigits.at(std::to_integer<unsigned>(b) & 0xFU);
  }
  return s;
}  // GCOVR_EXCL_LINE: gcc's exception cleanup of `s`

std::expected<void, AuthError> Authenticate(Client &client, const crypto::Nonce &host_nonce,
                                            const micnote_v1_HelloAck &hello,
                                            const crypto::LinkKey &link_key) {
  const crypto::Nonce n_d = DeviceNonce(hello);
  const auto proof =
      std::as_bytes(std::span(hello.device_proof.bytes).first(hello.device_proof.size));
  if (!crypto::VerifyProof(link_key, crypto::kProofPuck, host_nonce, n_d, proof)) {
    return std::unexpected(AuthError{.kind = AuthError::Kind::kWrongPuck, .client = {}});
  }
  if (auto ok = client.Auth(crypto::MakeProof(link_key, crypto::kProofHost, host_nonce, n_d));
      !ok) {
    return std::unexpected(AuthError{.kind = AuthError::Kind::kRefused, .client = ok.error()});
  }
  return {};
}

}  // namespace micnote::protocol
