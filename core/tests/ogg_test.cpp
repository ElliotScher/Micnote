// OggOpusWriter: a small Ogg demuxer here checks every page's framing and
// CRC, then that the packets, headers and granule positions are what RFC 7845
// asks for. recording_test.cpp runs the test vectors (real Opus from the
// firmware's encoder) through it as well.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "recording/mnr.hpp"
#include "recording/ogg_writer.hpp"
#include "recording/reader.hpp"

namespace {

using micnote::recording::kCodecOpus;
using micnote::recording::kOggMaxPacketsPerPage;
using micnote::recording::kOpusPreSkip;
using micnote::recording::Marker;
using micnote::recording::OggCrc;
using micnote::recording::OggError;
using micnote::recording::OpusComments;
using micnote::recording::Recording;
using micnote::recording::Trailer;
using micnote::recording::WriteOggOpus;

using Bytes = std::vector<std::byte>;

Bytes FromString(std::string_view s) {
  Bytes out;
  for (const char c : s) {
    out.push_back(static_cast<std::byte>(c));
  }
  return out;
}

std::uint64_t Le(std::span<const std::byte> b) {
  std::uint64_t v = 0;
  for (std::size_t i = b.size(); i-- > 0;) {
    v = (v << 8U) | std::to_integer<std::uint64_t>(b[i]);
  }
  return v;
}

struct Page {
  std::uint8_t flags = 0;
  std::uint64_t granule = 0;
  std::uint32_t serial = 0;
  std::uint32_t seq = 0;
  std::vector<Bytes> packets;  // whole packets ending on this page
};

// Splits a file into pages, failing the test on any framing or CRC error.
// Packets never span pages in what the writer produces; that's checked too.
std::vector<Page> Demux(std::span<const std::byte> file) {
  std::vector<Page> pages;
  std::size_t off = 0;
  while (off < file.size()) {
    EXPECT_GE(file.size() - off, 27U);
    if (file.size() - off < 27) {
      return pages;
    }
    const auto hdr = file.subspan(off);
    EXPECT_EQ(Bytes(hdr.begin(), hdr.begin() + 4), FromString("OggS"));
    EXPECT_EQ(hdr[4], std::byte{0});
    Page p;
    p.flags = std::to_integer<std::uint8_t>(hdr[5]);
    p.granule = Le(hdr.subspan(6, 8));
    p.serial = static_cast<std::uint32_t>(Le(hdr.subspan(14, 4)));
    p.seq = static_cast<std::uint32_t>(Le(hdr.subspan(18, 4)));
    const auto crc = static_cast<std::uint32_t>(Le(hdr.subspan(22, 4)));
    const auto n_seg = std::to_integer<std::size_t>(hdr[26]);
    std::size_t body = 0;
    for (std::size_t i = 0; i < n_seg; ++i) {
      body += std::to_integer<std::size_t>(hdr[27 + i]);
    }
    const std::size_t len = 27 + n_seg + body;
    EXPECT_LE(len, file.size() - off);
    if (len > file.size() - off) {
      return pages;
    }
    Bytes copy(hdr.begin(), hdr.begin() + static_cast<std::ptrdiff_t>(len));
    std::fill_n(copy.begin() + 22, 4, std::byte{0});
    EXPECT_EQ(OggCrc(copy), crc) << "page " << p.seq;

    std::size_t at = 27 + n_seg;
    Bytes packet;
    for (std::size_t i = 0; i < n_seg; ++i) {
      const auto lace = std::to_integer<std::size_t>(hdr[27 + i]);
      packet.insert(packet.end(), hdr.begin() + static_cast<std::ptrdiff_t>(at),
                    hdr.begin() + static_cast<std::ptrdiff_t>(at + lace));
      at += lace;
      if (lace < 255) {
        p.packets.push_back(std::move(packet));
        packet.clear();
      }
    }
    EXPECT_TRUE(packet.empty()) << "a packet continues past page " << p.seq;
    pages.push_back(std::move(p));
    off += len;
  }
  return pages;
}

// An Opus packet of n bytes: TOC config 9 (SILK wideband 20 ms), code 0.
Bytes Packet(std::size_t n, std::uint8_t fill = 0xAB) {
  Bytes p(n, std::byte{fill});
  p[0] = std::byte{9U << 3U};
  return p;
}

Recording MakeRecording(std::size_t n_packets, std::size_t packet_len = 30) {
  Recording r;
  r.header.codec = kCodecOpus;
  r.header.rate_hz = 16000;
  r.header.frame_ms = 20;
  r.header.rec_id = 0x0000000300000007ULL;
  r.header.start_unix_ms = 1763800000123ULL;
  for (std::size_t i = 0; i < n_packets; ++i) {
    r.packets.push_back(Packet(packet_len, static_cast<std::uint8_t>(i)));
  }
  r.trailer = Trailer{.duration_ms = static_cast<std::uint32_t>(n_packets * 20)};
  return r;
}

std::vector<std::string> Comments(const Bytes &tags) {
  std::vector<std::string> out;
  std::size_t at = 8;
  const std::size_t vendor = Le(std::span(tags).subspan(at, 4));
  at += 4 + vendor;
  const std::size_t n = Le(std::span(tags).subspan(at, 4));
  at += 4;
  for (std::size_t i = 0; i < n; ++i) {
    const std::size_t len = Le(std::span(tags).subspan(at, 4));
    at += 4;
    out.emplace_back(reinterpret_cast<const char *>(tags.data() + at), len);
    at += len;
  }
  EXPECT_EQ(at, tags.size());
  return out;
}

TEST(OggCrc, KnownAnswer) {
  // CRC-32 with Ogg's parameters over "123456789" (the CRC catalogue's check
  // input): CRC-32/CKSUM's check value without its final XOR.
  EXPECT_EQ(OggCrc(FromString("123456789")), 0x89A1897FU);
  EXPECT_EQ(OggCrc({}), 0U);
}

TEST(OggOpus, HeadersFollowRfc7845) {
  const auto file = WriteOggOpus(MakeRecording(3));
  ASSERT_TRUE(file);
  const auto pages = Demux(*file);
  ASSERT_EQ(pages.size(), 3U);

  // OpusHead alone on a BOS page with granule 0.
  EXPECT_EQ(pages[0].flags, 0x02);
  EXPECT_EQ(pages[0].granule, 0U);
  ASSERT_EQ(pages[0].packets.size(), 1U);
  const Bytes &head = pages[0].packets[0];
  ASSERT_EQ(head.size(), 19U);
  EXPECT_EQ(Bytes(head.begin(), head.begin() + 8), FromString("OpusHead"));
  EXPECT_EQ(head[8], std::byte{1});  // version
  EXPECT_EQ(head[9], std::byte{1});  // channels
  EXPECT_EQ(Le(std::span(head).subspan(10, 2)), kOpusPreSkip);
  EXPECT_EQ(Le(std::span(head).subspan(12, 4)), 16000U);
  EXPECT_EQ(Le(std::span(head).subspan(16, 2)), 0U);  // gain
  EXPECT_EQ(head[18], std::byte{0});                  // mapping family

  // OpusTags alone on the next page, also granule 0.
  EXPECT_EQ(pages[1].flags, 0);
  EXPECT_EQ(pages[1].granule, 0U);
  ASSERT_EQ(pages[1].packets.size(), 1U);
  const Bytes &tags = pages[1].packets[0];
  EXPECT_EQ(Bytes(tags.begin(), tags.begin() + 8), FromString("OpusTags"));
  EXPECT_EQ(Comments(tags), OpusComments(MakeRecording(3)));

  // One stream, pages numbered from 0, EOS on the last one only.
  for (std::size_t i = 0; i < pages.size(); ++i) {
    EXPECT_EQ(pages[i].serial, 0x00000004U);  // 3 ^ 7
    EXPECT_EQ(pages[i].seq, i);
  }
  EXPECT_EQ(pages[2].flags, 0x04);
}

TEST(OggOpus, CommentsCarryIdDateAndChapters) {
  Recording r = MakeRecording(1);
  r.markers = {Marker{.type = 1, .offset_ms = 1500}, Marker{.type = 1, .offset_ms = 3723004}};
  EXPECT_EQ(OpusComments(r), (std::vector<std::string>{
                                 "MICNOTE_REC_ID=0000000300000007",
                                 "DATE=2025-11-22T08:26:40Z",
                                 "CHAPTER001=00:00:01.500",
                                 "CHAPTER001NAME=Marker 1",
                                 "CHAPTER002=01:02:03.004",
                                 "CHAPTER002NAME=Marker 2",
                             }));

  // No clock: no DATE. No trailer: the recording was cut short.
  r.header.start_unix_ms = 0;
  r.trailer.reset();
  r.markers.clear();
  EXPECT_EQ(OpusComments(r), (std::vector<std::string>{
                                 "MICNOTE_REC_ID=0000000300000007",
                                 "MICNOTE_ENDED=power cut",
                             }));
}

TEST(OggOpus, AudioPagesHoldOneSecondAndCountGranules) {
  const Recording r = MakeRecording(120);  // 2.4 s
  const auto file = WriteOggOpus(r);
  ASSERT_TRUE(file);
  const auto pages = Demux(*file);
  ASSERT_EQ(pages.size(), 2U + 3U);  // 50 + 50 + 20

  std::vector<Bytes> packets;
  std::uint64_t samples = kOpusPreSkip;
  for (std::size_t i = 2; i < pages.size(); ++i) {
    EXPECT_LE(pages[i].packets.size(), kOggMaxPacketsPerPage);
    samples += pages[i].packets.size() * 960;
    // A page's granule is the end of its last packet, pre-skip included.
    EXPECT_EQ(pages[i].granule, samples);
    EXPECT_EQ(pages[i].flags, i + 1 == pages.size() ? 0x04 : 0);
    packets.insert(packets.end(), pages[i].packets.begin(), pages[i].packets.end());
  }
  EXPECT_EQ(packets, r.packets);
  EXPECT_EQ(pages.back().granule, kOpusPreSkip + (120U * 960U));
}

TEST(OggOpus, LongPacketsSpanSegmentsAndFillPages) {
  // 600 bytes is 3 segments (255, 255, 90); 510 is 3 too (255, 255, 0).
  Recording r = MakeRecording(0);
  r.packets = {Packet(600), Packet(510), Packet(255), Packet(1)};
  const auto file = WriteOggOpus(r);
  ASSERT_TRUE(file);
  const auto pages = Demux(*file);
  ASSERT_EQ(pages.size(), 3U);
  EXPECT_EQ(pages[2].packets, r.packets);

  // 5000-byte packets are 20 segments each: the 255-entry lacing table
  // fills after 12, before the 50-packet limit.
  r.packets.assign(30, Packet(5000));
  const auto big = WriteOggOpus(r);
  ASSERT_TRUE(big);
  const auto big_pages = Demux(*big);
  ASSERT_EQ(big_pages.size(), 2U + 3U);  // 12 + 12 + 6
  EXPECT_EQ(big_pages[2].packets.size(), 12U);
  EXPECT_EQ(big_pages[4].packets.size(), 6U);
}

TEST(OggOpus, GranulesFollowEachPacketsToc) {
  Recording r = MakeRecording(0);
  Bytes celt_2_5ms(10, std::byte{0});
  celt_2_5ms[0] = std::byte{16U << 3U};  // CELT NB 2.5 ms, code 0: 120 samples
  Bytes silk_60ms_x2(10, std::byte{0});
  silk_60ms_x2[0] = std::byte{(3U << 3U) | 1U};  // SILK NB 60 ms, two frames: 5760
  Bytes code3(10, std::byte{0});
  code3[0] = std::byte{(13U << 3U) | 3U};  // hybrid 20 ms, code 3...
  code3[1] = std::byte{3};                 // ...3 frames: 2880
  Bytes hybrid_10ms(10, std::byte{0});
  hybrid_10ms[0] = std::byte{12U << 3U};  // hybrid SWB 10 ms: 480
  r.packets = {celt_2_5ms, silk_60ms_x2, code3, hybrid_10ms};
  const auto file = WriteOggOpus(r);
  ASSERT_TRUE(file);
  EXPECT_EQ(Demux(*file).back().granule, kOpusPreSkip + 120U + 5760U + 2880U + 480U);
}

TEST(OggOpus, EmptyRecordingEndsOnTheTagsPage) {
  const auto file = WriteOggOpus(MakeRecording(0));
  ASSERT_TRUE(file);
  const auto pages = Demux(*file);
  ASSERT_EQ(pages.size(), 2U);
  EXPECT_EQ(pages[1].flags, 0x04);
}

TEST(OggOpus, Rejects) {
  Recording r = MakeRecording(2);
  r.header.codec = 2;
  EXPECT_EQ(WriteOggOpus(r).error(), OggError::kUnsupportedCodec);

  const auto bad = [](Bytes packet) {
    Recording rec = MakeRecording(1);
    rec.packets.push_back(std::move(packet));
    return WriteOggOpus(rec).error();
  };
  EXPECT_EQ(bad({}), OggError::kBadPacket);
  EXPECT_EQ(bad({std::byte{3}}), OggError::kBadPacket);                // code 3 without a count
  EXPECT_EQ(bad({std::byte{3}, std::byte{0}}), OggError::kBadPacket);  // 0 frames
  // SILK 60 ms, code 3, 3 frames: 180 ms, over the 120 ms limit.
  EXPECT_EQ(bad({std::byte{(3U << 3U) | 3U}, std::byte{3}}), OggError::kBadPacket);
  EXPECT_EQ(bad(Packet(65025)), OggError::kBadPacket);
  EXPECT_TRUE(WriteOggOpus([] {
    Recording rec = MakeRecording(0);
    rec.packets.push_back(Packet(65024));
    return rec;
  }()));
}

TEST(OggOpus, SameRecordingSameBytes) {
  EXPECT_EQ(WriteOggOpus(MakeRecording(77)), WriteOggOpus(MakeRecording(77)));
}

}  // namespace
