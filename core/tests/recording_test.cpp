// The host's recording reader against proto/testvectors, which the
// firmware's real recstore wrote: if these pass, the puck and the host agree
// on the .mnr format and the crypto. Then every way a file can be rejected.
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <fstream>
#include <initializer_list>
#include <iomanip>
#include <iterator>
#include <span>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "crypto/psa.hpp"
#include "recording/mnr.hpp"
#include "recording/ogg_writer.hpp"
#include "recording/reader.hpp"
#include <nlohmann/json.hpp>

namespace {

using micnote::crypto::Key;
using micnote::recording::FormatError;
using micnote::recording::HostKey;
using micnote::recording::kChunkAudio;
using micnote::recording::kChunkTrailer;
using micnote::recording::ParseChunkHeader;
using micnote::recording::ParseHeader;
using micnote::recording::ParsePlaintext;
using micnote::recording::ReadError;
using micnote::recording::ReadRecording;
using micnote::recording::Recording;
using micnote::recording::UnwrapCek;

using Bytes = std::vector<std::byte>;

std::string VectorPath(const std::string &name) {
  return std::string(MICNOTE_TESTVECTORS_DIR) + "/" + name;
}

Bytes ReadFile(const std::string &name) {
  std::ifstream in(VectorPath(name), std::ios::binary);
  EXPECT_TRUE(in.good()) << VectorPath(name);
  const std::vector<char> chars{std::istreambuf_iterator<char>(in), {}};
  Bytes out(chars.size());
  std::ranges::transform(chars, out.begin(), [](char c) { return static_cast<std::byte>(c); });
  return out;
}

Bytes FromHex(const std::string &hex) {
  Bytes out;
  for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
    out.push_back(static_cast<std::byte>(std::stoul(hex.substr(i, 2), nullptr, 16)));
  }
  return out;
}

std::string ToHex(std::span<const std::byte> bytes) {
  std::string out;
  for (const std::byte b : bytes) {
    out += std::format("{:02x}", std::to_integer<unsigned>(b));
  }
  return out;
}

const nlohmann::json &Vectors() {
  static const nlohmann::json kVectors = [] {
    std::ifstream in(VectorPath("vectors.json"));
    return nlohmann::json::parse(in);
  }();
  return kVectors;
}

HostKey KeyNamed(const std::string &name) {
  const Bytes priv = FromHex(Vectors()["keys"][name]["priv"].get<std::string>());
  auto key = HostKey::FromPrivate(std::span<const std::byte, 32>(priv.data(), 32));
  EXPECT_TRUE(key.has_value());
  return std::move(key.value());
}

// What the generator hashed: every packet as `len u16 LE | bytes`, in order.
std::string PacketStreamSha256(const Recording &rec) {
  Bytes stream;
  for (const Bytes &p : rec.packets) {
    stream.push_back(static_cast<std::byte>(p.size() & 0xFFU));
    stream.push_back(static_cast<std::byte>(p.size() >> 8U));
    stream.insert(stream.end(), p.begin(), p.end());
  }
  return ToHex(micnote::crypto::Sha256(stream));
}

}  // namespace

// ---- the vectors -----------------------------------------------------------

TEST(Recording, KeysMatchVectors) {
  for (const auto &[name, k] : Vectors()["keys"].items()) {
    const HostKey key = KeyNamed(name);
    EXPECT_EQ(ToHex(key.pub()), k["pub"].get<std::string>()) << name;
    EXPECT_EQ(ToHex(key.host_id()), k["host_id"].get<std::string>()) << name;
  }
}

TEST(Recording, EveryVectorReadsBackForEveryRecipient) {
  for (const auto &v : Vectors()["recordings"]) {
    const std::string file = v["file"].get<std::string>();
    const Bytes data = ReadFile(file);
    ASSERT_EQ(data.size(), v["size"].get<std::size_t>()) << file;
    EXPECT_EQ(ToHex(micnote::crypto::Sha256(data)), v["file_sha256"].get<std::string>()) << file;

    for (const auto &who : v["recipients"]) {
      const std::string recipient = who.get<std::string>();
      std::string trace = file;
      trace += " as ";
      trace += recipient;
      SCOPED_TRACE(trace);
      const auto rec = ReadRecording(data, KeyNamed(recipient));
      ASSERT_TRUE(rec.has_value()) << static_cast<int>(rec.error());

      std::ostringstream rec_id;
      rec_id << std::hex << std::setw(16) << std::setfill('0') << rec->header.rec_id;
      EXPECT_EQ(rec_id.str(), v["rec_id"].get<std::string>());
      EXPECT_EQ(rec->chunks, v["chunks"].get<std::uint32_t>());
      EXPECT_EQ(rec->packets.size(), v["packets"].get<std::size_t>());
      std::vector<std::uint32_t> offsets;
      for (const auto &m : rec->markers) {
        offsets.push_back(m.offset_ms);
      }
      EXPECT_EQ(offsets, v["marker_offsets_ms"].get<std::vector<std::uint32_t>>());
      EXPECT_EQ(rec->trailer.has_value(), v["trailer"].get<bool>());
      EXPECT_EQ(rec->DurationMs(), v["duration_ms"].get<std::uint32_t>());
      EXPECT_EQ(PacketStreamSha256(*rec), v["packet_stream_sha256"].get<std::string>());
    }
  }
}

// The vectors hold real Opus (the firmware's encoder), so they make real
// .opus files: every packet is a valid 20 ms Opus packet, and the stream
// ends at pre-skip + 960 samples a packet.
TEST(Recording, EveryVectorBecomesOggOpus) {
  for (const auto &v : Vectors()["recordings"]) {
    const std::string file = v["file"].get<std::string>();
    SCOPED_TRACE(file);
    const auto rec = ReadRecording(ReadFile(file), KeyNamed(v["recipients"][0].get<std::string>()));
    ASSERT_TRUE(rec.has_value());
    const auto ogg = micnote::recording::WriteOggOpus(*rec);
    ASSERT_TRUE(ogg.has_value());

    // The granule of the last page: bytes 6..13 of the last "OggS".
    const Bytes magic{std::byte{'O'}, std::byte{'g'}, std::byte{'g'}, std::byte{'S'}};
    const auto last = std::ranges::find_end(*ogg, magic);
    ASSERT_FALSE(last.empty());
    const auto at = static_cast<std::size_t>(last.begin() - ogg->begin());
    std::uint64_t granule = 0;
    for (std::size_t i = 0; i < 8; ++i) {
      granule |= std::to_integer<std::uint64_t>((*ogg)[at + 6 + i]) << (8U * i);
    }
    EXPECT_EQ(granule, micnote::recording::kOpusPreSkip + (rec->packets.size() * 960U));
  }
}

TEST(Recording, HeaderFieldsOfTwoHostVector) {
  const auto h = ParseHeader(ReadFile("rec_two_hosts.mnr"));
  ASSERT_TRUE(h.has_value());
  EXPECT_EQ(h->length, micnote::recording::kHeaderSize);
  EXPECT_EQ(h->codec, 1U);
  EXPECT_EQ(h->rate_hz, 16000U);
  EXPECT_EQ(h->frame_ms, 20U);
  EXPECT_EQ(h->start_boot, 1U);
  EXPECT_EQ(h->start_uptime_ms, 60000U);
  EXPECT_EQ(h->start_unix_ms, 1763800000000ULL);
  EXPECT_EQ(h->wraps.size(), 2U);
}

// ---- rejecting files -------------------------------------------------------

class RecordingRejects : public ::testing::Test {
 protected:
  Bytes &file() { return file_; }
  [[nodiscard]] const HostKey &host_a() const { return host_a_; }

  ReadError Read() {
    const auto rec = ReadRecording(file_, host_a_);
    EXPECT_FALSE(rec.has_value());
    return rec.has_value() ? ReadError::kCrypto : rec.error();
  }

 private:
  Bytes file_ = ReadFile("rec_two_hosts.mnr");
  HostKey host_a_ = KeyNamed("host_a");
};

// The first chunk record starts right after the header.
constexpr std::size_t kFirst = micnote::recording::kHeaderSize;

TEST_F(RecordingRejects, BadHeader) {
  file().resize(10);
  EXPECT_EQ(Read(), ReadError::kFormat);
}

TEST_F(RecordingRejects, HostThatIsNotARecipient) {
  const auto rec = ReadRecording(file(), KeyNamed("device_fallback"));
  ASSERT_FALSE(rec.has_value());
  EXPECT_EQ(rec.error(), ReadError::kNotARecipient);
}

TEST_F(RecordingRejects, TamperedWrap) {
  // host_a's wrap is slot 0: its tag is the last 16 of 88 bytes at 41.
  file()[41 + 80] ^= std::byte{0x01};
  EXPECT_EQ(Read(), ReadError::kUnwrap);
}

TEST_F(RecordingRejects, LowOrderEphemeralKey) {
  std::fill_n(file().begin() + 41 + 8, 32, std::byte{0});
  EXPECT_EQ(Read(), ReadError::kUnwrap);
}

TEST_F(RecordingRejects, TruncatedChunkRecord) {
  file().resize(file().size() - 3);
  EXPECT_EQ(Read(), ReadError::kChunkFraming);
}

TEST_F(RecordingRejects, StrayBytesAfterHeader) {
  file().resize(kFirst + 3);
  EXPECT_EQ(Read(), ReadError::kChunkFraming);
}

TEST_F(RecordingRejects, OutOfSequenceChunk) {
  file()[kFirst] = std::byte{1};  // seq 0 -> 1
  EXPECT_EQ(Read(), ReadError::kChunkFraming);
}

TEST_F(RecordingRejects, ZeroLengthChunk) {
  file()[kFirst + 4] = std::byte{0};
  file()[kFirst + 5] = std::byte{0};
  EXPECT_EQ(Read(), ReadError::kChunkFraming);
}

TEST_F(RecordingRejects, TamperedChunk) {
  file()[kFirst + 6] ^= std::byte{0x01};
  EXPECT_EQ(Read(), ReadError::kChunkAuth);
}

// The core is every chunk's AAD: changing it breaks them all.
TEST_F(RecordingRejects, TamperedCore) {
  file()[20] ^= std::byte{0x01};  // start_boot
  EXPECT_EQ(Read(), ReadError::kChunkAuth);
}

// The wrap table isn't in the AAD: rewriting another host's slot (as the
// first pairing does to the fallback wrap) leaves the chunks readable.
TEST_F(RecordingRejects, OtherWrapSlotsAreNotAuthenticatedByChunks) {
  std::fill_n(file().begin() + 41 + 88, 88, std::byte{0x5A});
  const auto rec = ReadRecording(file(), host_a());
  ASSERT_TRUE(rec.has_value());
  EXPECT_EQ(rec->packets.size(), 250U);
}

// A chunk that authenticates but holds malformed contents: an audio chunk
// sealed with the real CEK and appended after the trailer.
TEST_F(RecordingRejects, ValidChunkAfterTrailer) {
  const auto h = ParseHeader(file());
  ASSERT_TRUE(h.has_value());
  auto raw = UnwrapCek(host_a(), h->rec_id, h->wraps[0]);
  ASSERT_TRUE(raw.has_value());
  auto cek = micnote::crypto::ImportAesCcm(*raw);
  ASSERT_TRUE(cek.has_value());

  const auto vectors = Vectors()["recordings"][0];
  const auto seq = vectors["chunks"].get<std::uint32_t>();
  // The spec's nonce and AAD, built independently of reader.cpp.
  std::array<std::uint8_t, 13> nonce{};
  std::array<std::uint8_t, 36> aad{};
  for (std::size_t i = 0; i < 8; ++i) {
    nonce.at(i) = static_cast<std::uint8_t>(h->rec_id >> (8 * i));
  }
  const auto core = micnote::crypto::Sha256(std::span(file()).first(40));
  for (std::size_t i = 0; i < 32; ++i) {
    aad.at(i) = std::to_integer<std::uint8_t>(core.at(i));
  }
  for (std::size_t i = 0; i < 4; ++i) {
    nonce.at(8 + i) = aad.at(32 + i) = static_cast<std::uint8_t>(seq >> (8 * i));
  }
  const std::array<std::uint8_t, 4> pt{kChunkAudio, 1, 0, 'x'};
  std::array<std::uint8_t, pt.size() + 16> sealed{};
  std::size_t len = 0;
  ASSERT_EQ(psa_aead_encrypt(cek->id(), PSA_ALG_CCM, nonce.data(), nonce.size(), aad.data(),
                             aad.size(), pt.data(), pt.size(), sealed.data(), sealed.size(), &len),
            PSA_SUCCESS);

  const std::array<std::uint8_t, 6> record{static_cast<std::uint8_t>(seq & 0xFFU),
                                           static_cast<std::uint8_t>(seq >> 8U),
                                           0,
                                           0,
                                           static_cast<std::uint8_t>(pt.size()),
                                           0};
  for (const std::uint8_t b : record) {
    file().push_back(std::byte{b});
  }
  for (const std::uint8_t b : sealed) {
    file().push_back(std::byte{b});
  }
  EXPECT_EQ(Read(), ReadError::kPlaintext);
}

// ---- header parsing --------------------------------------------------------

TEST(RecordingHeader, Rejects) {
  const Bytes good = ReadFile("rec_two_hosts.mnr");
  const auto parse = [&](auto mutate) {
    Bytes f = good;
    mutate(f);
    const auto h = ParseHeader(f);
    EXPECT_FALSE(h.has_value());
    return h.has_value() ? FormatError::kTruncated : h.error();
  };
  EXPECT_EQ(parse([](Bytes &f) { f.resize(6); }), FormatError::kTruncated);
  EXPECT_EQ(parse([](Bytes &f) { f[3] = std::byte{'2'}; }), FormatError::kBadMagic);
  EXPECT_EQ(parse([](Bytes &f) { f[4] = std::byte{2}; }), FormatError::kUnsupportedVersion);
  EXPECT_EQ(parse([](Bytes &f) { f[5] = std::byte{0x88}; }), FormatError::kBadHeaderLength);
  EXPECT_EQ(parse([](Bytes &f) { f[6] = std::byte{0x03}; }), FormatError::kBadHeaderLength);
  EXPECT_EQ(parse([](Bytes &f) { f.resize(392); }), FormatError::kTruncated);
  EXPECT_EQ(parse([](Bytes &f) { f[40] = std::byte{5}; }), FormatError::kTooManyWraps);
}

TEST(RecordingHeader, ChunkHeaderLengths) {
  std::array<std::byte, 6> bytes{std::byte{7}, {}, {}, {}, std::byte{0x00}, std::byte{0x10}};
  const auto ok = ParseChunkHeader(bytes);
  ASSERT_TRUE(ok.has_value());
  const auto ch = ok.value_or(micnote::recording::ChunkHeader{});
  EXPECT_EQ(ch.seq, 7U);
  EXPECT_EQ(ch.ct_len, 4096U);
  bytes[4] = std::byte{0x01};  // 4097
  EXPECT_FALSE(ParseChunkHeader(bytes).has_value());
  bytes[4] = std::byte{0};
  bytes[5] = std::byte{0};
  EXPECT_FALSE(ParseChunkHeader(bytes).has_value());
}

// ---- plaintext parsing -----------------------------------------------------

namespace {

Bytes Pt(std::initializer_list<int> values) {
  Bytes out;
  for (const int v : values) {
    out.push_back(static_cast<std::byte>(v));
  }
  return out;
}

ReadError ParseFails(const Bytes &pt, Recording rec = {}) {
  const auto r = ParsePlaintext(pt, rec);
  EXPECT_FALSE(r.has_value());
  return r.has_value() ? ReadError::kCrypto : r.error();
}

}  // namespace

TEST(RecordingPlaintext, AudioPacketsAndMarkers) {
  Recording rec;
  // packet "ab", marker type 1 at 0x01020304 ms, packet "c".
  const Bytes pt = Pt({kChunkAudio, 2, 0, 'a', 'b', 0xFF, 0xFF, 1, 4, 3, 2, 1, 1, 0, 'c'});
  ASSERT_TRUE(ParsePlaintext(pt, rec).has_value());
  ASSERT_EQ(rec.packets.size(), 2U);
  EXPECT_EQ(rec.packets[0], Pt({'a', 'b'}));
  EXPECT_EQ(rec.packets[1], Pt({'c'}));
  ASSERT_EQ(rec.markers.size(), 1U);
  EXPECT_EQ(rec.markers[0].type, 1U);
  EXPECT_EQ(rec.markers[0].offset_ms, 0x01020304U);
}

TEST(RecordingPlaintext, Trailer) {
  Recording rec;
  rec.markers.push_back({});
  ASSERT_TRUE(ParsePlaintext(Pt({kChunkTrailer, 0x88, 0x13, 0, 0, 1, 0, 0, 0}), rec).has_value());
  ASSERT_TRUE(rec.trailer.has_value());
  EXPECT_EQ(rec.DurationMs(), 5000U);
}

TEST(RecordingPlaintext, Rejects) {
  EXPECT_EQ(ParseFails({}), ReadError::kPlaintext) << "empty";
  EXPECT_EQ(ParseFails(Pt({0x03})), ReadError::kPlaintext) << "unknown type";
  EXPECT_EQ(ParseFails(Pt({kChunkTrailer, 0, 0, 0, 0, 0, 0, 0})), ReadError::kPlaintext)
      << "short trailer";
  EXPECT_EQ(ParseFails(Pt({kChunkTrailer, 0, 0, 0, 0, 1, 0, 0, 0})), ReadError::kPlaintext)
      << "trailer counts a marker that isn't there";
  EXPECT_EQ(ParseFails(Pt({kChunkAudio, 2})), ReadError::kPlaintext) << "half a length";
  EXPECT_EQ(ParseFails(Pt({kChunkAudio, 0xFF, 0xFF, 1, 0})), ReadError::kPlaintext)
      << "truncated marker";
  EXPECT_EQ(ParseFails(Pt({kChunkAudio, 0, 0})), ReadError::kPlaintext) << "empty packet";
  EXPECT_EQ(ParseFails(Pt({kChunkAudio, 3, 0, 'a', 'b'})), ReadError::kPlaintext)
      << "packet overruns the chunk";

  Recording ended;
  ended.trailer = micnote::recording::Trailer{};
  EXPECT_EQ(ParseFails(Pt({kChunkAudio}), ended), ReadError::kPlaintext)
      << "audio after the trailer";
}

// ---- the PSA layer ---------------------------------------------------------

TEST(Psa, KeyMoves) {
  auto imported = micnote::crypto::ImportAesCcm(std::array<std::byte, 32>{});
  ASSERT_TRUE(imported.has_value());
  Key a = std::move(*imported);
  const psa_key_id_t id = a.id();
  Key b;
  b = std::move(a);
  EXPECT_EQ(b.id(), id);
  EXPECT_EQ(a.id(), PSA_KEY_ID_NULL);  // NOLINT(bugprone-use-after-move): moved-from state
  Key &same = b;
  b = std::move(same);  // self-move leaves the key alone
  EXPECT_EQ(b.id(), id);
}

TEST(Psa, PublicKeyOfNoKeyFails) { EXPECT_FALSE(micnote::crypto::PublicKey(Key{}).has_value()); }

TEST(Psa, InitIsIdempotent) {
  EXPECT_EQ(micnote::crypto::Init(), PSA_SUCCESS);
  EXPECT_EQ(micnote::crypto::Init(), PSA_SUCCESS);
}
