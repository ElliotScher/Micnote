// Pairing and session crypto against the known answers computed
// independently with pyca/cryptography; firmware/tests/pairing checks the
// puck's implementation against the same values.
//
// Inputs: the host key is RFC 7748 section 6.1's Bob, the device's pairing
// key its Alice; device_id = 01..08, n_h = 00..0f, n_d = 10..1f.
#include "crypto/pairing.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "crypto/psa.hpp"

namespace {

using micnote::crypto::DerivePairing;
using micnote::crypto::kProofHost;
using micnote::crypto::kProofPuck;
using micnote::crypto::LinkKey;
using micnote::crypto::MakeProof;
using micnote::crypto::Nonce;
using micnote::crypto::PairError;
using micnote::crypto::Sas;
using micnote::crypto::SasColorName;
using micnote::crypto::VerifyProof;

constexpr unsigned Nibble(char c) {
  return c <= '9' ? static_cast<unsigned>(c - '0') : static_cast<unsigned>(c - 'a' + 10);
}

template <std::size_t N>
constexpr std::array<std::byte, N> Hex(std::string_view hex) {
  std::array<std::byte, N> out{};
  for (std::size_t i = 0; i < N; ++i) {
    out.at(i) = static_cast<std::byte>((Nibble(hex.at(2 * i)) << 4U) | Nibble(hex.at((2 * i) + 1)));
  }
  return out;
}

constexpr auto kHostPriv =
    Hex<32>("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb");
constexpr auto kHostPub =
    Hex<32>("de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f");
constexpr auto kDevicePub =
    Hex<32>("8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a");
constexpr auto kHostId = Hex<8>("f35e5616160a30bf");
constexpr auto kDeviceId = Hex<8>("0102030405060708");
constexpr auto kLinkKey =
    Hex<32>("e239f0fd7ec27d64cd8272a6f00b7b1615d58b09aa390b2b571f28b8d329417c");
constexpr auto kDeviceProof =
    Hex<32>("90eb93121c4b723915524bd0fcc00fe8acfa479519c5c3377089c6f133ab0604");
constexpr auto kHostProof =
    Hex<32>("70b9b6068042f5a488c1b0c47f2ab30c2a3c0214c1712c4a804d603da2c46d47");

constexpr Nonce Counting(std::uint8_t from) {
  Nonce n{};
  for (std::size_t i = 0; i < n.size(); ++i) {
    n.at(i) = static_cast<std::byte>(from + i);
  }
  return n;
}

constexpr Nonce kNh = Counting(0);
constexpr Nonce kNd = Counting(16);

TEST(Pairing, MatchesKnownAnswers) {
  auto host = micnote::crypto::ImportX25519(kHostPriv);
  ASSERT_TRUE(host);
  const auto p = DerivePairing(*host, kHostPub, kDevicePub, kHostId, kDeviceId, kNh, kNd);
  ASSERT_TRUE(p);
  EXPECT_EQ(p->link_key, kLinkKey);
  // sas bytes 25 7d b4 37, mod 6.
  EXPECT_EQ(p->sas, (Sas{1, 5, 0, 1}));
  EXPECT_EQ(MakeProof(p->link_key, kProofPuck, kNh, kNd), kDeviceProof);
  EXPECT_EQ(MakeProof(p->link_key, kProofHost, kNh, kNd), kHostProof);
}

TEST(Pairing, VerifyProof) {
  const LinkKey key = kLinkKey;
  EXPECT_TRUE(VerifyProof(key, kProofPuck, kNh, kNd, kDeviceProof));
  EXPECT_FALSE(VerifyProof(key, kProofPuck, kNh, kNd, kHostProof));
  EXPECT_FALSE(VerifyProof(key, kProofPuck, kNd, kNh, kDeviceProof));
  auto bad = kDeviceProof;
  bad[0] ^= std::byte{1};
  EXPECT_FALSE(VerifyProof(key, kProofPuck, kNh, kNd, bad));
  // A truncated proof is wrong, not an error.
  EXPECT_FALSE(VerifyProof(key, kProofPuck, kNh, kNd, std::span(kDeviceProof).first(16)));
}

TEST(Pairing, RefusesLowOrderDeviceKey) {
  auto host = micnote::crypto::ImportX25519(kHostPriv);
  ASSERT_TRUE(host);
  const std::array<std::byte, 32> zero{};
  EXPECT_EQ(DerivePairing(*host, kHostPub, zero, kHostId, kDeviceId, kNh, kNd).error(),
            PairError::kLowOrder);
}

TEST(Pairing, ColorNames) {
  EXPECT_EQ(SasColorName(0), "red");
  EXPECT_EQ(SasColorName(5), "magenta");
  EXPECT_EQ(SasColorName(6), "?");
}

}  // namespace
