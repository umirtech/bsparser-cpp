#include "bsparser/bsparser.hpp"

#include <algorithm>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>

#if defined(__aarch64__) || defined(_M_ARM64)
#include <arm_neon.h>
#define BSPARSER_HAS_NEON 1
#else
#define BSPARSER_HAS_NEON 0
#endif

#if defined(BSPARSER_ARMV7_NEON_DISPATCH)
#include <asm/hwcap.h>
#include <sys/auxv.h>
extern "C" size_t bsparser_find_start_code_neon_armv7(const uint8_t* data, size_t size,
                                                      size_t from);
#endif

#if (defined(__clang__) || defined(__GNUC__)) && \
    (defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86))
#include <immintrin.h>
#define BSPARSER_CAN_BUILD_X86_SIMD 1
#else
#define BSPARSER_CAN_BUILD_X86_SIMD 0
#endif

namespace bsparser {
namespace {
using Bytes = std::vector<uint8_t>;

constexpr size_t kNotFound = std::numeric_limits<size_t>::max();

size_t find_start_code_scalar(const uint8_t* data, size_t size, size_t from) {
  if (size < 3 || from > size - 3) {
    return kNotFound;
  }
  for (size_t i = from; i + 3 <= size; ++i) {
    if (data[i] == 0 && data[i + 1] == 0 &&
        (data[i + 2] == 1 || (i + 3 < size && data[i + 2] == 0 && data[i + 3] == 1))) {
      return i;
    }
  }
  return kNotFound;
}

#if BSPARSER_CAN_BUILD_X86_SIMD
__attribute__((target("sse2"))) size_t find_start_code_sse2(const uint8_t* data, size_t size,
                                                            size_t from) {
  const __m128i zero = _mm_setzero_si128();
  const __m128i one = _mm_set1_epi8(1);
  size_t cursor = from;
  while (cursor + 16 <= size) {
    const __m128i bytes = _mm_loadu_si128(reinterpret_cast<const __m128i*>(data + cursor));
    const uint32_t zeros = static_cast<uint32_t>(_mm_movemask_epi8(_mm_cmpeq_epi8(bytes, zero)));
    const uint32_t ones = static_cast<uint32_t>(_mm_movemask_epi8(_mm_cmpeq_epi8(bytes, one)));
    const uint32_t three_byte = zeros & (zeros >> 1) & (ones >> 2);
    const uint32_t four_byte = zeros & (zeros >> 1) & (zeros >> 2) & (ones >> 3);
    if ((three_byte | four_byte) != 0) {
      return find_start_code_scalar(data, std::min(size, cursor + 16), cursor);
    }
    cursor += 13;
  }
  return find_start_code_scalar(data, size, cursor);
}

__attribute__((target("avx2"))) size_t find_start_code_avx2(const uint8_t* data, size_t size,
                                                            size_t from) {
  const __m256i zero = _mm256_setzero_si256();
  const __m256i one = _mm256_set1_epi8(1);
  size_t cursor = from;

  // Each 32-byte load checks start-code candidates through byte 28. Advance
  // by 29 bytes so that candidates at the end are not skipped.
  while (cursor + 32 <= size) {
    const __m256i bytes = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(data + cursor));
    const uint32_t zeros =
        static_cast<uint32_t>(_mm256_movemask_epi8(_mm256_cmpeq_epi8(bytes, zero)));
    const uint32_t ones =
        static_cast<uint32_t>(_mm256_movemask_epi8(_mm256_cmpeq_epi8(bytes, one)));
    const uint32_t three_byte = zeros & (zeros >> 1) & (ones >> 2);
    const uint32_t four_byte = zeros & (zeros >> 1) & (zeros >> 2) & (ones >> 3);
    if ((three_byte | four_byte) != 0) {
      return find_start_code_scalar(data, std::min(size, cursor + 32), cursor);
    }
    cursor += 29;
  }
  return find_start_code_scalar(data, size, cursor);
}
#endif

#if BSPARSER_HAS_NEON
size_t find_start_code_neon(const uint8_t* data, size_t size, size_t from) {
  const uint8x16_t zero = vdupq_n_u8(0);
  size_t cursor = from;
  while (cursor + 16 <= size) {
    const uint8x16_t bytes = vld1q_u8(data + cursor);
    if (vmaxvq_u8(vceqq_u8(bytes, zero)) != 0) {
      return find_start_code_scalar(data, std::min(size, cursor + 19), cursor);
    }
    cursor += 16;
  }
  return find_start_code_scalar(data, size, cursor);
}
#endif

ScanBackend detect_scan_backend() noexcept {
#if BSPARSER_CAN_BUILD_X86_SIMD
  if (__builtin_cpu_supports("avx2")) {
    return ScanBackend::Avx2;
  }
  if (__builtin_cpu_supports("sse2")) {
    return ScanBackend::Sse2;
  }
  return ScanBackend::Scalar;
#endif
#if BSPARSER_HAS_NEON
  return ScanBackend::Neon;
#elif defined(BSPARSER_ARMV7_NEON_DISPATCH)
  if ((getauxval(AT_HWCAP) & HWCAP_NEON) != 0) {
    return ScanBackend::Neon;
  }
  return ScanBackend::Scalar;
#else
  return ScanBackend::Scalar;
#endif
}

size_t find_start_code_fast(const uint8_t* data, size_t size, size_t from) {
  static const ScanBackend backend = detect_scan_backend();
  switch (backend) {
#if BSPARSER_CAN_BUILD_X86_SIMD
    case ScanBackend::Avx2:
      return find_start_code_avx2(data, size, from);
    case ScanBackend::Sse2:
      return find_start_code_sse2(data, size, from);
#endif
#if BSPARSER_HAS_NEON
    case ScanBackend::Neon:
      return find_start_code_neon(data, size, from);
#endif
#if defined(BSPARSER_ARMV7_NEON_DISPATCH)
    case ScanBackend::Neon:
      return bsparser_find_start_code_neon_armv7(data, size, from);
#endif
    default:
      return find_start_code_scalar(data, size, from);
  }
}

std::string number(uint64_t v) {
  return std::to_string(v);
}
void field(Header& h, const char* key, uint64_t value) {
  h.fields[key] = number(value);
}
void field(Header& h, const char* key, const std::string& value) {
  h.fields[key] = value;
}
uint16_t le16(const Bytes& b, size_t p) {
  return uint16_t(b.at(p)) | uint16_t(b.at(p + 1)) << 8;
}
uint32_t le32(const Bytes& b, size_t p) {
  return uint32_t(b.at(p)) | uint32_t(b.at(p + 1)) << 8 | uint32_t(b.at(p + 2)) << 16 |
         uint32_t(b.at(p + 3)) << 24;
}
uint64_t le64(const Bytes& b, size_t p) {
  uint64_t n = 0;
  for (unsigned i = 0; i < 8; ++i) n |= uint64_t(b.at(p + i)) << (8 * i);
  return n;
}
std::string fourcc(const Bytes& b, size_t p) {
  return std::string(b.begin() + p, b.begin() + p + 4);
}

Bytes rbsp(const Bytes& nal, size_t start) {
  Bytes out;
  out.reserve(nal.size());
  unsigned zeros = 0;
  for (size_t i = start; i < nal.size(); ++i) {
    if (zeros >= 2 && nal[i] == 3) {
      zeros = 0;
      continue;
    }
    out.push_back(nal[i]);
    zeros = nal[i] == 0 ? zeros + 1 : 0;
  }
  return out;
}
void skip_profile_tier_level(BitReader& b, unsigned max_sub_layers_minus1) {
  b.skip(2 + 1 + 5 + 32 + 48 + 8);
  std::vector<uint32_t> profile(max_sub_layers_minus1), level(max_sub_layers_minus1);
  for (unsigned i = 0; i < max_sub_layers_minus1; ++i) {
    profile[i] = b.u(1);
    level[i] = b.u(1);
  }
  if (max_sub_layers_minus1) b.skip((8 - max_sub_layers_minus1) * 2);
  for (unsigned i = 0; i < max_sub_layers_minus1; ++i) {
    if (profile[i]) b.skip(88);
    if (level[i]) b.skip(8);
  }
}
Header parse_vp8(const Bytes& d, uint64_t off) {
  if (d.size() < 3) throw std::out_of_range("truncated VP8 frame tag");
  uint32_t tag = uint32_t(d[0]) | uint32_t(d[1]) << 8 | uint32_t(d[2]) << 16;
  Header h{off, d.size(), (tag & 1) ? "P" : "I", !(tag & 1), {}};
  field(h, "frame_type", tag & 1);
  field(h, "version", (tag >> 1) & 7);
  field(h, "show_frame", (tag >> 4) & 1);
  field(h, "partition_length", tag >> 5);
  if (!(tag & 1)) {
    if (d.size() < 10) throw std::out_of_range("truncated VP8 key frame");
    field(
        h, "sync_code", "0x" + [&] {
          std::ostringstream s;
          s << std::hex << std::setw(6) << std::setfill('0')
            << (uint32_t(d[3]) << 16 | uint32_t(d[4]) << 8 | d[5]);
          return s.str();
        }());
    field(h, "width", (uint16_t(d[6]) | uint16_t(d[7]) << 8) & 0x3fff);
    field(h, "height", (uint16_t(d[8]) | uint16_t(d[9]) << 8) & 0x3fff);
  }
  return h;
}
Header parse_vp9(const Bytes& d, uint64_t off) {
  BitReader b(d);
  Header h{off, d.size(), "VP9 frame", false, {}};
  auto marker = b.u(2), profile = b.u(1) | (b.u(1) << 1);
  field(h, "frame_marker", marker);
  field(h, "profile", profile);
  if (profile == 3) field(h, "reserved_zero", b.u(1));
  auto existing = b.u(1);
  field(h, "show_existing_frame", existing);
  if (existing) {
    field(h, "frame_to_show_map_idx", b.u(3));
    return h;
  }
  auto type = b.u(1);
  auto show = b.u(1);
  auto resilient = b.u(1);
  h.type = type ? "P" : "I";
  h.keyframe = !type;
  field(h, "frame_type", type);
  field(h, "show_frame", show);
  field(h, "error_resilient_mode", resilient);
  if (!type) {
    field(h, "frame_sync_code", number(b.u(24)));
    if (profile >= 2) field(h, "bit_depth", b.u(1) ? 12 : 10);
    field(h, "color_space", b.u(3));
    field(h, "color_range", b.u(1));
    auto w = b.u(16) + 1, ht = b.u(16) + 1;
    field(h, "width", w);
    field(h, "height", ht);
  }
  return h;
}
Header parse_avc(const Bytes& d, uint64_t off) {
  if (d.empty()) throw std::out_of_range("empty AVC NAL");
  uint8_t t = d[0] & 0x1f;
  static const char* names[] = {"Unspecified",  "Slice",         "Slice data A", "Slice data B",
                                "Slice data C", "IDR slice",     "SEI",          "SPS",
                                "PPS",          "AUD",           "End sequence", "End stream",
                                "Filler",       "SPS extension", "Prefix NAL",   "Subset SPS"};
  Header h{off, d.size(), t < 16 ? names[t] : "AVC NAL", t == 5, {}};
  field(h, "nal_unit_type", t);
  field(h, "nal_ref_idc", (d[0] >> 5) & 3);
  Bytes r = rbsp(d, 1);
  BitReader b(r);
  if (t == 7) {
    auto profile = b.u(8);
    b.skip(8);
    auto level = b.u(8);
    auto id = b.ue();
    field(h, "profile_idc", profile);
    field(h, "level_idc", level);
    field(h, "seq_parameter_set_id", id);
    unsigned chroma = 1;
    if (profile == 100 || profile == 110 || profile == 122 || profile == 244 || profile == 44 ||
        profile == 83 || profile == 86 || profile == 118 || profile == 128 || profile == 138 ||
        profile == 139 || profile == 134) {
      chroma = b.ue();
      if (chroma == 3) b.u(1);
      b.ue();
      b.ue();
      b.u(1);
      if (b.u(1)) {
        unsigned n = chroma != 3 ? 8 : 12;
        for (unsigned i = 0; i < n; ++i)
          if (b.u(1)) {
            int last = 8, next = 8;
            for (unsigned j = 0; j < (i < 6 ? 16 : 64); ++j) {
              if (next) next = (last + b.se() + 256) % 256;
              last = next ? next : last;
            }
          }
      }
    }
    b.ue();
    auto poc = b.ue();
    if (poc == 0)
      b.ue();
    else if (poc == 1) {
      b.u(1);
      b.se();
      b.se();
      auto n = b.ue();
      for (uint32_t i = 0; i < n; ++i) b.se();
    }
    b.ue();
    b.u(1);
    auto w = b.ue() + 1, ht = b.ue() + 1;
    auto frame_only = b.u(1);
    if (!frame_only) b.u(1);
    b.u(1);
    auto crop = b.u(1);
    uint32_t l = 0, rgt = 0, tp = 0, bt = 0;
    if (crop) {
      l = b.ue();
      rgt = b.ue();
      tp = b.ue();
      bt = b.ue();
    }
    const uint32_t crop_x = (chroma == 1 || chroma == 2) ? 2 : 1,
                   crop_y = (2 - frame_only) * (chroma == 1 ? 2 : 1);
    field(h, "width", w * 16 - (l + rgt) * crop_x);
    field(h, "height", ht * 16 * (2 - frame_only) - (tp + bt) * crop_y);
  } else if (t == 8) {
    field(h, "pic_parameter_set_id", b.ue());
    field(h, "seq_parameter_set_id", b.ue());
  } else if (t == 1 || t == 5) {
    field(h, "first_mb_in_slice", b.ue());
    auto st = b.ue();
    field(h, "slice_type", st);
    field(h, "pic_parameter_set_id", b.ue());
  }
  return h;
}
Header parse_hevc(const Bytes& d, uint64_t off, bool vvc = false) {
  if (d.size() < 2) throw std::out_of_range("truncated NAL header");
  uint8_t type = (d[0] >> 1) & 0x3f;
  Header h{off, d.size(), vvc ? "VVC NAL" : "HEVC NAL", false, {}};
  field(h, "nal_unit_type", type);
  field(h, "nuh_layer_id", ((d[0] & 1) << 5) | (d[1] >> 3));
  field(h, "nuh_temporal_id_plus1", d[1] & 7);
  if (vvc) {
    static const char* types[] = {
        "TRAIL",      "STSA",       "RADL",     "RASL", "RSV_VCL_4", "RSV_VCL_5",
        "RSV_VCL_6",  "IDR_W_RADL", "IDR_N_LP", "CRA",  "GDR",       "RSV_IRAP_11",
        "OPI",        "DCI",        "VPS",      "SPS",  "PPS",       "PREFIX_APS",
        "SUFFIX_APS", "PH",         "AUD",      "EOS",  "EOB",       "PREFIX_SEI",
        "SUFFIX_SEI", "FD"};
    h.type = type < 26 ? types[type] : "VVC NAL";
    h.keyframe = type >= 7 && type <= 10;
    return h;
  }
  static const char* types[] = {"TRAIL_N",
                                "TRAIL_R",
                                "TSA_N",
                                "TSA_R",
                                "STSA_N",
                                "STSA_R",
                                "RADL_N",
                                "RADL_R",
                                "RASL_N",
                                "RASL_R",
                                "RSV_VCL_N10",
                                "RSV_VCL_R11",
                                "RSV_VCL_N12",
                                "RSV_VCL_R13",
                                "RSV_VCL_N14",
                                "RSV_VCL_R15",
                                "BLA_W_LP",
                                "BLA_W_RADL",
                                "BLA_N_LP",
                                "IDR_W_RADL",
                                "IDR_N_LP",
                                "CRA_NUT",
                                "RSV_IRAP_VCL22",
                                "RSV_IRAP_VCL23",
                                "VPS",
                                "SPS",
                                "PPS",
                                "AUD",
                                "EOS",
                                "EOB",
                                "FD",
                                "PREFIX_SEI",
                                "SUFFIX_SEI"};
  h.type = type < 33 ? types[type] : "HEVC NAL";
  h.keyframe = type >= 16 && type <= 21;
  if (type == 33) {
    Bytes r = rbsp(d, 2);
    BitReader b(r);
    field(h, "sps_video_parameter_set_id", b.u(4));
    auto layers = b.u(3);
    field(h, "sps_max_sub_layers_minus1", layers);
    b.u(1);
    skip_profile_tier_level(b, layers);
    field(h, "sps_seq_parameter_set_id", b.ue());
    auto chroma = b.ue();
    field(h, "chroma_format_idc", chroma);
    if (chroma == 3) b.u(1);
    auto w = b.ue(), ht = b.ue();
    auto conf = b.u(1);
    if (conf) {
      auto l = b.ue(), rgt = b.ue(), tp = b.ue(), bt = b.ue();
      unsigned sx = chroma == 1 || chroma == 2 ? 2 : 1, sy = chroma == 1 ? 2 : 1;
      w -= (l + rgt) * sx;
      ht -= (tp + bt) * sy;
    }
    field(h, "width", w);
    field(h, "height", ht);
  }
  return h;
}
std::vector<Header> parse_av1(const Bytes& d, uint64_t off) {
  std::vector<Header> out;
  size_t p = 0;
  while (p < d.size()) {
    size_t begin = p;
    uint8_t x = d.at(p++);
    if (x & 0x80) throw std::runtime_error("invalid AV1 OBU forbidden bit");
    uint8_t type = (x >> 3) & 15;
    bool ext = x & 4, has = x & 2;
    if (ext) {
      if (p >= d.size()) throw std::out_of_range("truncated AV1 extension");
      ++p;
    }
    uint64_t size = d.size() - p;
    if (has) {
      size = 0;
      unsigned shift = 0;
      for (;;) {
        if (p >= d.size() || shift > 56) throw std::out_of_range("invalid AV1 size field");
        uint8_t q = d[p++];
        size |= uint64_t(q & 127) << shift;
        shift += 7;
        if (!(q & 128)) break;
      }
    }
    if (size > d.size() - p) throw std::out_of_range("truncated AV1 OBU payload");
    static const char* names[] = {"Reserved",
                                  "Sequence header",
                                  "Temporal delimiter",
                                  "Frame header",
                                  "Tile group",
                                  "Metadata",
                                  "Frame",
                                  "Redundant frame header",
                                  "Tile list",
                                  "Reserved",
                                  "Reserved",
                                  "Reserved",
                                  "Reserved",
                                  "Reserved",
                                  "Reserved",
                                  "Padding"};
    Header h{off + begin, p + size - begin, names[type], false, {}};
    field(h, "obu_type", type);
    field(h, "obu_has_size_field", has);
    if (type == 1 && size >= 1) {
      Bytes payload(d.begin() + p, d.begin() + p + size);
      BitReader b(payload);
      auto profile = b.u(3);
      field(h, "seq_profile", profile);
      auto still = b.u(1);
      auto reduced = b.u(1);
      field(h, "still_picture", still);
      if (reduced) {
        b.u(5);
        auto wb = b.u(4) + 1, hb = b.u(4) + 1;
        field(h, "max_frame_width", b.u(wb) + 1);
        field(h, "max_frame_height", b.u(hb) + 1);
      }
    }
    if ((type == 3 || type == 6) && size) {
      Bytes payload(d.begin() + p, d.begin() + p + size);
      BitReader b(payload);
      try {
        auto show_existing = b.u(1);
        field(h, "show_existing_frame", show_existing);
        if (!show_existing) {
          auto ft = b.u(2);
          field(h, "frame_type", ft);
          h.keyframe = ft == 0;
        }
      } catch (const std::out_of_range&) {
      }
    }
    out.push_back(std::move(h));
    p += size;
  }
  return out;
}
size_t start_code(const Bytes& d, size_t from) {
  return find_start_code_fast(d.data(), d.size(), from);
}
std::string esc(const std::string& v) {
  std::ostringstream s;
  for (char c : v) {
    if (c == '"' || c == '\\') s << '\\';
    if (c == '\n')
      s << "\\n";
    else
      s << c;
  }
  return s.str();
}
}  // namespace

BitReader::BitReader(const Bytes& data) : data_(data) {}
uint32_t BitReader::u(unsigned bits) {
  if (bits > 32 || bits > bits_left()) throw std::out_of_range("truncated bitstream");
  uint32_t result = 0;
  for (unsigned i = 0; i < bits; ++i)
    result = (result << 1) | ((data_[bitpos_++] >> (7 - ((bitpos_ - 1) & 7))) & 1);
  return result;
}
int32_t BitReader::s(unsigned bits) {
  auto v = u(bits);
  return (v & (1u << (bits - 1))) ? int32_t(v - (uint64_t(1) << bits)) : int32_t(v);
}
uint32_t BitReader::ue() {
  unsigned zeros = 0;
  while (u(1) == 0) {
    if (++zeros > 31) throw std::runtime_error("Exp-Golomb value too large");
  }
  return zeros ? ((1u << zeros) - 1 + u(zeros)) : 0;
}
int32_t BitReader::se() {
  auto c = ue();
  return c & 1 ? int32_t((c + 1) / 2) : -int32_t(c / 2);
}
uint64_t BitReader::leb128() {
  uint64_t result = 0;
  for (unsigned i = 0; i < 8; ++i) {
    auto byte = u(8);
    result |= uint64_t(byte & 127) << (7 * i);
    if (!(byte & 128)) return result;
  }
  throw std::runtime_error("LEB128 exceeds 8 bytes");
}
size_t BitReader::bit_position() const noexcept {
  return bitpos_;
}
size_t BitReader::bits_left() const noexcept {
  return data_.size() * 8 - bitpos_;
}
void BitReader::skip(size_t n) {
  if (n > bits_left()) throw std::out_of_range("truncated bitstream");
  bitpos_ += n;
}

std::vector<Header> parse_unit(Codec c, const Bytes& b, uint64_t off) {
  switch (c) {
    case Codec::VP8:
      return {parse_vp8(b, off)};
    case Codec::VP9:
      return {parse_vp9(b, off)};
    case Codec::AV1:
      return parse_av1(b, off);
    case Codec::AVC:
      return {parse_avc(b, off)};
    case Codec::HEVC:
      return {parse_hevc(b, off)};
    case Codec::VVC:
      return {parse_hevc(b, off, true)};
    default:
      throw std::invalid_argument("unknown codec");
  }
}

std::vector<Header> parse_unit(Codec codec, const Unit& unit) {
  return parse_unit(codec, unit.bytes, unit.offset);
}

UnitScanner::UnitScanner(Codec codec) : codec_(codec) {
  if (codec == Codec::Unknown) {
    throw std::invalid_argument("UnitScanner requires a known codec");
  }
}

std::vector<Unit> UnitScanner::feed(const Bytes& data) {
  return feed(data.data(), data.size());
}

std::vector<Unit> UnitScanner::feed(const uint8_t* data, size_t size) {
  if (size == 0) {
    return {};
  }

  if (codec_ == Codec::VP8 || codec_ == Codec::VP9) {
    Unit unit;
    unit.kind = UnitKind::Frame;
    unit.offset = pending_offset_;
    unit.bytes.assign(data, data + size);
    unit.keyframe = codec_ == Codec::VP8 ? !(data[0] & 1) : false;
    unit.frame_start = true;
    pending_offset_ += size;
    return {std::move(unit)};
  }

  pending_.insert(pending_.end(), data, data + size);
  if (codec_ == Codec::AV1) {
    return scan_av1_obus(false);
  }
  return scan_annexb(false);
}

std::vector<Unit> UnitScanner::finish() {
  std::vector<Unit> units;
  if (codec_ == Codec::AV1) {
    units = scan_av1_obus(true);
  } else if (codec_ == Codec::AVC || codec_ == Codec::HEVC || codec_ == Codec::VVC) {
    units = scan_annexb(true);
  }
  reset();
  return units;
}

void UnitScanner::reset() {
  pending_.clear();
  pending_begin_ = 0;
  pending_offset_ = 0;
  saw_start_code_ = false;
}

void UnitScanner::compact_pending() {
  // Avoid O(n) vector erases for every NAL. Compact only after enough consumed
  // input has accumulated, which keeps scanning large timelines linear.
  if (pending_begin_ == 0) {
    return;
  }
  if (pending_begin_ == pending_.size()) {
    pending_.clear();
    pending_begin_ = 0;
  } else if (pending_begin_ >= 64 * 1024 && pending_begin_ * 2 >= pending_.size()) {
    pending_.erase(pending_.begin(), pending_.begin() + pending_begin_);
    pending_begin_ = 0;
  }
}

Unit UnitScanner::make_annexb_unit(const uint8_t* payload, size_t payload_size,
                                   uint64_t payload_offset, size_t start_code_size) const {
  Unit unit;
  unit.kind = UnitKind::NalUnit;
  unit.offset = payload_offset;
  unit.start_code_size = start_code_size;
  unit.bytes.assign(payload, payload + payload_size);
  if (unit.bytes.empty()) {
    return unit;
  }

  if (codec_ == Codec::AVC) {
    unit.type = unit.bytes[0] & 0x1f;
    if (unit.type == 1 || unit.type == 5) {
      try {
        Bytes slice_rbsp = rbsp(unit.bytes, 1);
        BitReader reader(slice_rbsp);
        unit.frame_start = reader.ue() == 0;  // first_mb_in_slice
        unit.keyframe = unit.type == 5;
      } catch (const std::exception&) {
        // The NAL is still useful even if a truncated slice cannot be classified.
      }
    }
  } else {
    if (unit.bytes.size() < 3) {
      return unit;
    }
    unit.type = (unit.bytes[0] >> 1) & 0x3f;
    if (codec_ == Codec::HEVC) {
      const bool is_vcl = unit.type <= 31;
      unit.frame_start = is_vcl && (unit.bytes[2] & 0x80) != 0;
      unit.keyframe = unit.type >= 16 && unit.type <= 21;
    } else {                               // VVC
      unit.frame_start = unit.type == 19;  // PH_NUT
      unit.keyframe = unit.type >= 7 && unit.type <= 10;
    }
  }
  return unit;
}

std::vector<Unit> UnitScanner::scan_annexb(bool end_of_stream) {
  std::vector<Unit> units;
  const size_t no_position = kNotFound;

  size_t first = start_code(pending_, pending_begin_);
  if (!saw_start_code_) {
    if (first == no_position) {
      // Preserve enough bytes to recognize a start code split over chunks.
      const size_t available = pending_.size() - pending_begin_;
      if (available > 3) {
        const size_t discard = available - 3;
        pending_begin_ += discard;
        pending_offset_ += discard;
        compact_pending();
      }
      return units;
    }
    pending_offset_ += first - pending_begin_;
    pending_begin_ = first;
    saw_start_code_ = true;
  }

  while (true) {
    const size_t available = pending_.size() - pending_begin_;
    const size_t start_size =
        available >= 4 && pending_[pending_begin_] == 0 && pending_[pending_begin_ + 1] == 0 &&
                pending_[pending_begin_ + 2] == 0 && pending_[pending_begin_ + 3] == 1
            ? 4
            : 3;
    if (available < start_size) {
      break;
    }
    const size_t payload_begin = pending_begin_ + start_size;
    const size_t next = start_code(pending_, payload_begin);
    if (next == no_position) {
      if (end_of_stream && available > start_size) {
        units.push_back(make_annexb_unit(pending_.data() + payload_begin,
                                         pending_.size() - payload_begin,
                                         pending_offset_ + start_size, start_size));
        pending_offset_ += available;
        pending_begin_ = pending_.size();
        compact_pending();
      }
      break;
    }
    if (next > payload_begin) {
      units.push_back(make_annexb_unit(pending_.data() + payload_begin, next - payload_begin,
                                       pending_offset_ + start_size, start_size));
    }
    pending_offset_ += next - pending_begin_;
    pending_begin_ = next;
    compact_pending();
  }
  return units;
}

std::vector<Unit> UnitScanner::scan_av1_obus(bool end_of_stream) {
  std::vector<Unit> units;
  size_t position = pending_begin_;
  while (position < pending_.size()) {
    const size_t obu_start = position;
    const uint8_t header = pending_[position++];
    if ((header & 0x80) != 0) {
      throw std::runtime_error("invalid AV1 OBU forbidden bit");
    }
    const uint8_t obu_type = (header >> 3) & 0x0f;
    if ((header & 0x04) != 0) {
      if (position == pending_.size()) {
        position = obu_start;
        break;
      }
      ++position;
    }
    if ((header & 0x02) == 0) {
      if (!end_of_stream) {
        position = obu_start;
        break;
      }
      position = pending_.size();
    } else {
      uint64_t payload_size = 0;
      unsigned shift = 0;
      bool complete_size = false;
      while (position < pending_.size() && shift <= 56) {
        const uint8_t byte = pending_[position++];
        payload_size |= uint64_t(byte & 0x7f) << shift;
        shift += 7;
        if ((byte & 0x80) == 0) {
          complete_size = true;
          break;
        }
      }
      if (!complete_size || payload_size > pending_.size() - position) {
        position = obu_start;
        break;
      }
      position += static_cast<size_t>(payload_size);
    }

    Unit unit;
    unit.kind = UnitKind::Obu;
    unit.offset = pending_offset_ + obu_start - pending_begin_;
    unit.type = obu_type;
    unit.bytes.assign(pending_.begin() + obu_start, pending_.begin() + position);
    unit.frame_start = obu_type == 3 || obu_type == 6;
    if (obu_type == 3 || obu_type == 6) {
      try {
        auto headers = parse_unit(Codec::AV1, unit.bytes, unit.offset);
        unit.keyframe = !headers.empty() && headers.front().keyframe;
      } catch (const std::exception&) {
        // Boundary extraction succeeds even if optional frame metadata is absent.
      }
    }
    units.push_back(std::move(unit));
  }

  if (position != pending_begin_) {
    pending_offset_ += position - pending_begin_;
    pending_begin_ = position;
    compact_pending();
  }
  if (end_of_stream && pending_begin_ != pending_.size()) {
    throw std::out_of_range("truncated AV1 OBU at end of stream");
  }
  return units;
}

StreamParser::StreamParser(Codec codec) : codec_(codec) {
  if (codec == Codec::Unknown) throw std::invalid_argument("unknown codec");
}
std::vector<Header> StreamParser::feed(const Bytes& data) {
  return feed(data.data(), data.size());
}
std::vector<Header> StreamParser::feed(const uint8_t* data, size_t size) {
  std::vector<Header> out;
  if (codec_ == Codec::VP8 || codec_ == Codec::VP9 || codec_ == Codec::AV1) {
    Bytes unit(data, data + size);
    const uint64_t unit_offset = input_offset_;
    input_offset_ += size;
    return parse_unit(codec_, unit, unit_offset);
  }
  if (!size) return out;
  pending_.insert(pending_.end(), data, data + size);
  input_offset_ += size;
  size_t first = start_code(pending_, 0);
  if (!annexb_started_) {
    if (first == std::string::npos) {
      if (pending_.size() > 3) {
        pending_offset_ += pending_.size() - 3;
        pending_.erase(pending_.begin(), pending_.end() - 3);
      }
      return out;
    }
    pending_offset_ += first;
    pending_.erase(pending_.begin(), pending_.begin() + first);
    annexb_started_ = true;
  }
  while (true) {
    size_t code_len = pending_.size() >= 4 && pending_[0] == 0 && pending_[1] == 0 &&
                              pending_[2] == 0 && pending_[3] == 1
                          ? 4
                          : 3;
    if (pending_.size() < code_len) break;
    size_t next = start_code(pending_, code_len);
    if (next == std::string::npos) break;
    if (next > code_len) {
      Bytes nal(pending_.begin() + code_len, pending_.begin() + next);
      auto hs = parse_unit(codec_, nal, pending_offset_ + code_len);
      out.insert(out.end(), hs.begin(), hs.end());
    }
    pending_offset_ += next;
    pending_.erase(pending_.begin(), pending_.begin() + next);
  }
  return out;
}
std::vector<Header> StreamParser::finish() {
  std::vector<Header> out;
  if (annexb_started_ && pending_.size() >= 4) {
    size_t code_len = pending_[2] == 1 ? 3 : 4;
    if (pending_.size() > code_len) {
      Bytes nal(pending_.begin() + code_len, pending_.end());
      auto hs = parse_unit(codec_, nal, pending_offset_ + code_len);
      out.insert(out.end(), hs.begin(), hs.end());
    }
  }
  pending_.clear();
  annexb_started_ = false;
  return out;
}

std::vector<Header> IvfParser::feed(const Bytes& data) {
  return feed(data.data(), data.size());
}
std::vector<Header> IvfParser::feed(const uint8_t* data, size_t size) {
  pending_.insert(pending_.end(), data, data + size);
  std::vector<Header> out;
  if (!got_file_header_) {
    if (pending_.size() < 32) return out;
    if (fourcc(pending_, 0) != "DKIF") throw std::runtime_error("not an IVF stream");
    uint16_t header_size = le16(pending_, 6);
    if (header_size < 32 || pending_.size() < header_size) return out;
    auto f = fourcc(pending_, 8);
    codec_ = codec_from_name(f);
    Header h{offset_, header_size, "IVF", false, {}};
    field(h, "version", le16(pending_, 4));
    field(h, "fourcc", f);
    field(h, "width", le16(pending_, 12));
    field(h, "height", le16(pending_, 14));
    field(h, "frame_rate", le32(pending_, 16));
    field(h, "time_scale", le32(pending_, 20));
    field(h, "num_frames", le32(pending_, 24));
    out.push_back(std::move(h));
    pending_.erase(pending_.begin(), pending_.begin() + header_size);
    offset_ += header_size;
    got_file_header_ = true;
  }
  while (pending_.size() >= 12) {
    uint32_t length = le32(pending_, 0);
    if (pending_.size() < 12ull + length) break;
    uint64_t ts = le64(pending_, 4);
    Bytes frame(pending_.begin() + 12, pending_.begin() + 12 + length);
    auto hs = parse_unit(codec_, frame, offset_ + 12);
    for (auto& h : hs) {
      field(h, "timestamp", ts);
      out.push_back(std::move(h));
    }
    pending_.erase(pending_.begin(), pending_.begin() + 12 + length);
    offset_ += 12 + length;
  }
  return out;
}
Codec IvfParser::codec() const noexcept {
  return codec_;
}

Codec codec_from_name(const std::string& s) {
  if (s == "vp8" || s == "VP80") return Codec::VP8;
  if (s == "vp9" || s == "VP90") return Codec::VP9;
  if (s == "av1" || s == "AV01") return Codec::AV1;
  if (s == "avc" || s == "h264" || s == "264" || s == "H264") return Codec::AVC;
  if (s == "hevc" || s == "h265" || s == "265" || s == "H265" || s == "HEVC") return Codec::HEVC;
  if (s == "vvc" || s == "h266" || s == "266" || s == "H266" || s == "VVC") return Codec::VVC;
  return Codec::Unknown;
}
const char* codec_name(Codec c) {
  switch (c) {
    case Codec::VP8:
      return "VP8";
    case Codec::VP9:
      return "VP9";
    case Codec::AV1:
      return "AV1";
    case Codec::AVC:
      return "AVC";
    case Codec::HEVC:
      return "HEVC";
    case Codec::VVC:
      return "VVC";
    default:
      return "Unknown";
  }
}

ScanBackend active_scan_backend() noexcept {
  static const ScanBackend backend = detect_scan_backend();
  return backend;
}

const char* scan_backend_name(ScanBackend backend) noexcept {
  switch (backend) {
    case ScanBackend::Avx2:
      return "AVX2";
    case ScanBackend::Neon:
      return "NEON";
    default:
      return "scalar";
  }
}

std::vector<size_t> find_annexb_start_codes(const uint8_t* data, size_t size) {
  if (data == nullptr && size != 0) {
    throw std::invalid_argument("data must not be null when size is non-zero");
  }

  std::vector<size_t> offsets;
  size_t position = 0;
  while (position < size) {
    const size_t found = find_start_code_fast(data, size, position);
    if (found == kNotFound) {
      break;
    }
    offsets.push_back(found);
    // A new start code cannot begin before the final 0x01 of this one.
    position = found + (found + 3 < size && data[found + 2] == 0 && data[found + 3] == 1 ? 4 : 3);
  }
  return offsets;
}
std::string to_json(const Header& h) {
  std::ostringstream s;
  s << "{\"offset\":" << h.offset << ",\"length\":" << h.length << ",\"type\":\"" << esc(h.type)
    << "\",\"keyframe\":" << (h.keyframe ? "true" : "false") << ",\"fields\":{";
  bool first = true;
  for (const auto& [k, v] : h.fields) {
    if (!first) s << ',';
    first = false;
    s << '\"' << esc(k) << "\":\"" << esc(v) << '\"';
  }
  return s << "}}", s.str();
}

}  // namespace bsparser
