#include "recording/ogg_writer.hpp"

#include <array>
#include <chrono>
#include <string>
#include <string_view>
#include <utility>

namespace micnote::recording {

namespace {

constexpr std::string_view kVendor = "micnote";
constexpr std::size_t kMaxSegments = 255;
constexpr std::size_t kMaxPacket = (255 * 255) - 1;  // its lacing fills one page at most
constexpr std::size_t kOffCrc = 22;
constexpr std::byte kFlagBos{0x02};
constexpr std::byte kFlagEos{0x04};

constexpr std::array<std::uint32_t, 256> MakeCrcTable() {
  std::array<std::uint32_t, 256> table{};
  for (std::uint32_t i = 0; i < 256; ++i) {
    std::uint32_t r = i << 24U;
    for (int bit = 0; bit < 8; ++bit) {
      r = (r & 0x80000000U) != 0 ? (r << 1U) ^ 0x04C11DB7U : r << 1U;
    }
    table.at(i) = r;
  }
  return table;
}
constexpr std::array<std::uint32_t, 256> kCrcTable = MakeCrcTable();

void PutLe(std::vector<std::byte> &out, std::uint64_t value, std::size_t bytes) {
  for (std::size_t i = 0; i < bytes; ++i) {
    out.push_back(static_cast<std::byte>(value >> (8U * i)));
  }
}

void PutStr(std::vector<std::byte> &out, std::string_view s) {
  for (const char c : s) {
    out.push_back(static_cast<std::byte>(c));
  }
}

// 48 kHz samples in one Opus packet, from its TOC byte (RFC 6716 section
// 3.1), or 0 if the packet is malformed.
std::uint32_t PacketSamples(std::span<const std::byte> p) {
  if (p.empty()) {
    return 0;
  }
  const auto toc = std::to_integer<std::uint32_t>(p[0]);
  const std::uint32_t config = toc >> 3U;
  std::uint32_t frame = 0;
  if (config < 12) {  // SILK: 10, 20, 40, 60 ms
    constexpr std::array<std::uint32_t, 4> kSilk{480, 960, 1920, 2880};
    frame = kSilk.at(config % 4);
  } else if (config < 16) {  // hybrid: 10, 20 ms
    frame = config % 2 == 0 ? 480 : 960;
  } else {  // CELT: 2.5, 5, 10, 20 ms
    frame = 120U << (config % 4);
  }
  std::uint32_t frames = 1;
  switch (toc & 3U) {
    case 0:
      break;
    case 1:
    case 2:
      frames = 2;
      break;
    default:  // code 3: the count is in the next byte
      if (p.size() < 2) {
        return 0;
      }
      frames = std::to_integer<std::uint32_t>(p[1]) & 0x3FU;
      break;
  }
  const std::uint32_t total = frame * frames;
  // At most 120 ms per packet.
  return total == 0 || total > 5760 ? 0 : total;
}

// One Ogg stream being written: pages appended to out, numbered from 0.
struct Stream {
  std::vector<std::byte> out;
  std::uint32_t serial = 0;
  std::uint32_t seq = 0;
};

// Appends one page holding whole packets; the caller keeps the lacing in
// bounds.
void WritePage(Stream &s, std::span<const std::span<const std::byte>> packets,
               std::uint64_t granule, std::byte flags) {
  std::vector<std::byte> &out = s.out;
  const std::size_t start = out.size();
  PutStr(out, "OggS");
  out.push_back(std::byte{0});  // version
  out.push_back(flags);
  PutLe(out, granule, 8);
  PutLe(out, s.serial, 4);
  PutLe(out, s.seq++, 4);
  PutLe(out, 0, 4);  // CRC, filled in below
  const std::size_t n_segments_at = out.size();
  out.push_back(std::byte{0});
  std::size_t segments = 0;
  for (const auto &p : packets) {
    // A packet is 255-byte segments and one shorter one, which may be 0.
    for (std::size_t left = p.size();; left -= 255) {
      out.push_back(static_cast<std::byte>(left < 255 ? left : 255));
      ++segments;
      if (left < 255) {
        break;
      }
    }
  }
  out[n_segments_at] = static_cast<std::byte>(segments);
  for (const auto &p : packets) {
    out.insert(out.end(), p.begin(), p.end());
  }
  const std::uint32_t crc = OggCrc(std::span(out).subspan(start));
  for (std::size_t i = 0; i < 4; ++i) {
    out[start + kOffCrc + i] = static_cast<std::byte>(crc >> (8U * i));
  }
}

std::size_t Segments(std::size_t len) { return (len / 255) + 1; }

// Library code avoids format(): Apple's libc++ instantiates its floating
// point formatter with every format() call, and that needs to_chars,
// which the app's iOS 16 / macOS 13 deployment targets don't have.

// v in decimal, zero-padded to `width` digits.
std::string Dec(std::uint64_t v, std::size_t width) {
  std::string s = std::to_string(v);
  if (s.size() < width) {
    s.insert(0, width - s.size(), '0');
  }
  return s;
}  // GCOVR_EXCL_LINE: gcc's exception cleanup of `s`

std::string Hex16(std::uint64_t v) {
  constexpr std::string_view kDigits = "0123456789abcdef";
  std::string s(16, '0');
  for (std::size_t i = 0; i < 16; ++i) {
    s.at(15 - i) = kDigits.at((v >> (4U * i)) & 0xFU);
  }
  return s;
}  // GCOVR_EXCL_LINE: gcc's exception cleanup of `s`

// hh:mm:ss.mmm
std::string Timestamp(std::uint32_t ms) {
  return Dec(ms / 3600000, 2) + ":" + Dec(ms / 60000 % 60, 2) + ":" + Dec(ms / 1000 % 60, 2) + "." +
         Dec(ms % 1000, 3);
}

// ISO 8601 UTC, to the second.
std::string IsoDate(std::uint64_t unix_ms) {
  using namespace std::chrono;
  const sys_seconds t{duration_cast<seconds>(milliseconds{unix_ms})};
  const sys_days day = floor<days>(t);
  const year_month_day ymd{day};
  const hh_mm_ss<seconds> hms{t - day};
  return Dec(static_cast<std::uint64_t>(static_cast<int>(ymd.year())), 4) + "-" +
         Dec(static_cast<unsigned>(ymd.month()), 2) + "-" +
         Dec(static_cast<unsigned>(ymd.day()), 2) + "T" +
         Dec(static_cast<std::uint64_t>(hms.hours().count()), 2) + ":" +
         Dec(static_cast<std::uint64_t>(hms.minutes().count()), 2) + ":" +
         Dec(static_cast<std::uint64_t>(hms.seconds().count()), 2) + "Z";
}

}  // namespace

std::uint32_t OggCrc(std::span<const std::byte> data) {
  std::uint32_t crc = 0;
  for (const std::byte b : data) {
    crc = (crc << 8U) ^ kCrcTable.at(((crc >> 24U) ^ std::to_integer<std::uint32_t>(b)) & 0xFFU);
  }
  return crc;
}

std::vector<std::string> OpusComments(const Recording &rec) {
  std::vector<std::string> c;
  c.push_back("MICNOTE_REC_ID=" + Hex16(rec.header.rec_id));
  if (rec.header.start_unix_ms != 0) {
    c.push_back("DATE=" + IsoDate(rec.header.start_unix_ms));
  }
  if (!rec.trailer) {
    c.emplace_back("MICNOTE_ENDED=power cut");
  }
  for (std::size_t i = 0; i < rec.markers.size(); ++i) {
    const std::string n = Dec(i + 1, 3);
    c.push_back("CHAPTER" + n + "=" + Timestamp(rec.markers[i].offset_ms));
    c.push_back("CHAPTER" + n + "NAME=Marker " + std::to_string(i + 1));
  }
  return c;
}  // GCOVR_EXCL_LINE: gcc's exception cleanup of `c`

std::expected<std::vector<std::byte>, OggError> WriteOggOpus(const Recording &rec) {
  if (rec.header.codec != kCodecOpus) {
    return std::unexpected(OggError::kUnsupportedCodec);
  }
  for (const auto &p : rec.packets) {
    if (p.size() > kMaxPacket || PacketSamples(p) == 0) {
      return std::unexpected(OggError::kBadPacket);
    }
  }

  Stream stream;
  stream.serial = static_cast<std::uint32_t>(rec.header.rec_id ^ (rec.header.rec_id >> 32U));

  // OpusHead (RFC 7845 section 5.1).
  std::vector<std::byte> head;
  PutStr(head, "OpusHead");
  head.push_back(std::byte{1});  // version
  head.push_back(std::byte{1});  // channels
  PutLe(head, kOpusPreSkip, 2);
  PutLe(head, rec.header.rate_hz, 4);
  PutLe(head, 0, 2);             // output gain
  head.push_back(std::byte{0});  // mapping family: mono/stereo
  const std::array<std::span<const std::byte>, 1> head_page{head};
  WritePage(stream, head_page, 0, kFlagBos);

  // OpusTags (section 5.2).
  std::vector<std::byte> tags;
  PutStr(tags, "OpusTags");
  PutLe(tags, kVendor.size(), 4);
  PutStr(tags, kVendor);
  const std::vector<std::string> comments = OpusComments(rec);
  PutLe(tags, comments.size(), 4);
  for (const auto &c : comments) {
    PutLe(tags, c.size(), 4);
    PutStr(tags, c);
  }
  // Audio: whole packets only, up to 1 s or a full lacing table per page.
  // Grouped first, so the last page can be written with EOS set.
  struct Page {
    std::vector<std::span<const std::byte>> packets;
    std::uint64_t granule = 0;
  };
  std::vector<Page> audio;
  std::uint64_t granule = kOpusPreSkip;
  std::size_t segments = 0;
  for (const auto &p : rec.packets) {
    if (audio.empty() || audio.back().packets.size() == kOggMaxPacketsPerPage ||
        segments + Segments(p.size()) > kMaxSegments) {
      audio.emplace_back();
      segments = 0;
    }
    granule += PacketSamples(p);
    audio.back().packets.emplace_back(p);
    audio.back().granule = granule;
    segments += Segments(p.size());
  }

  const std::array<std::span<const std::byte>, 1> tags_page{tags};
  WritePage(stream, tags_page, 0, audio.empty() ? kFlagEos : std::byte{0});
  for (std::size_t i = 0; i < audio.size(); ++i) {
    WritePage(stream, audio[i].packets, audio[i].granule,
              i + 1 == audio.size() ? kFlagEos : std::byte{0});
  }
  return std::move(stream.out);
}

}  // namespace micnote::recording
