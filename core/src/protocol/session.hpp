// The authenticated session (docs/protocol.md section 3), after Hello: check
// the puck's device_proof with the link key from pairing, then prove this
// host with Auth. Shared by micnote-cli and the sync job, so the check lives
// in one place.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>

#include "crypto/pairing.hpp"
#include "protocol/client.hpp"

namespace micnote::protocol {

struct AuthError {
  enum class Kind : std::uint8_t {
    // device_proof doesn't verify: not the puck this host paired with (or
    // the puck was reset).
    kWrongPuck,
    // The puck refused Auth, or the link failed (see client).
    kRefused,
  };
  Kind kind = Kind::kRefused;
  ClientError client;
};

std::array<std::byte, 8> DeviceId(const micnote_v1_HelloAck &hello);
crypto::Nonce DeviceNonce(const micnote_v1_HelloAck &hello);
// device_id as 16 lowercase hex digits: the key of the link-key secret.
std::string DeviceIdHex(std::span<const std::byte, 8> id);

std::expected<void, AuthError> Authenticate(Client &client, const crypto::Nonce &host_nonce,
                                            const micnote_v1_HelloAck &hello,
                                            const crypto::LinkKey &link_key);

}  // namespace micnote::protocol
