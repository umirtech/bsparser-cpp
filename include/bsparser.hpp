#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace bsparser {

enum class Codec { VP8, VP9, AV1, AVC, HEVC, VVC, Unknown };
enum class ScanBackend { Scalar, Sse2, Neon, Avx2 };

// The kind of elementary unit found in a raw stream.
enum class UnitKind { Frame, Obu, NalUnit };

// A complete unit extracted using the bundled JavaScript reference semantics.
// Annex-B scanning recognizes only the 3-byte 00 00 01 marker. Consequently,
// a leading zero from a 4-byte prefix is retained at the end of the preceding
// unit. `frame_start` is always false because the reference has no access-unit
// detection. AV1 units include their complete OBU header and payload.
struct Unit {
  UnitKind kind = UnitKind::Frame;
  uint64_t offset = 0;
  size_t start_code_size = 0;
  uint8_t type = 0;
  bool frame_start = false;
  bool keyframe = false;
  std::vector<uint8_t> bytes;
};

struct Header {
  uint64_t offset = 0;
  uint64_t length = 0;
  std::string type;
  bool keyframe = false;
  std::map<std::string, std::string> fields;
};

// A safe, MSB-first reader for video syntax elements. All methods throw
// std::out_of_range when the input is truncated.
class BitReader {
 public:
  explicit BitReader(const std::vector<uint8_t>& data);
  uint32_t u(unsigned bits);
  int32_t s(unsigned bits);
  uint32_t ue();
  int32_t se();
  uint64_t leb128();
  size_t bit_position() const noexcept;
  size_t bits_left() const noexcept;
  void skip(size_t bits);

 private:
  const std::vector<uint8_t>& data_;
  size_t bitpos_ = 0;
};

// Parses one complete elementary unit: a VP frame, an AV1 OBU sequence, or a
// single Annex-B NAL payload (without its start code).
std::vector<Header> parse_unit(Codec codec, const std::vector<uint8_t>& bytes, uint64_t offset = 0);

// Convenience overload for a unit returned by UnitScanner.
std::vector<Header> parse_unit(Codec codec, const Unit& unit);

// Finds complete elementary-unit boundaries while accepting arbitrary chunks.
// Its boundaries and flags reproduce the bundled JavaScript reference parser:
// Annex-B uses the 3-byte marker only, AV1 depends on its OBU size field, and
// no access-unit/frame-start detection is performed. VP8/VP9 treat each feed
// call as a complete frame because raw concatenated streams have no delimiter.
class UnitScanner {
 public:
  explicit UnitScanner(Codec codec);

  std::vector<Unit> feed(const uint8_t* data, size_t size);
  std::vector<Unit> feed(const std::vector<uint8_t>& data);
  std::vector<Unit> finish();
  void reset();

 private:
  std::vector<Unit> scan_annexb(bool end_of_stream);
  std::vector<Unit> scan_av1_obus(bool end_of_stream);
  void compact_pending();
  Unit make_annexb_unit(const uint8_t* payload, size_t payload_size, uint64_t payload_offset,
                        size_t start_code_size) const;

  Codec codec_;
  std::vector<uint8_t> pending_;
  size_t pending_begin_ = 0;
  uint64_t pending_offset_ = 0;
  bool saw_start_code_ = false;
};

// Incremental parsing wrapper. For more control over boundaries and raw unit
// bytes, use UnitScanner and pass each resulting Unit to parse_unit().
class StreamParser {
 public:
  explicit StreamParser(Codec codec);
  std::vector<Header> feed(const uint8_t* data, size_t size);
  std::vector<Header> feed(const std::vector<uint8_t>& data);
  std::vector<Header> finish();

 private:
  Codec codec_;
  std::vector<uint8_t> pending_;
  uint64_t pending_offset_ = 0;
  uint64_t input_offset_ = 0;
  bool annexb_started_ = false;
  bool first_annexb_header_ = true;
};

// Incremental IVF parser. It detects the codec from FourCC and emits the IVF
// header followed by parsed frame headers, each with a timestamp field.
class IvfParser {
 public:
  std::vector<Header> feed(const uint8_t* data, size_t size);
  std::vector<Header> feed(const std::vector<uint8_t>& data);
  Codec codec() const noexcept;

 private:
  std::vector<uint8_t> pending_;
  uint64_t offset_ = 0;
  bool got_file_header_ = false;
  Codec codec_ = Codec::Unknown;
};

Codec codec_from_name(const std::string& name);
const char* codec_name(Codec codec);
// The implementation selected at runtime for Annex-B start-code scanning.
ScanBackend active_scan_backend() noexcept;
const char* scan_backend_name(ScanBackend backend) noexcept;
// Stateless, zero-copy Annex-B scanner. Returned values are byte positions of
// 3-byte 00 00 01 markers in `data`, matching the JavaScript reference parser.
std::vector<size_t> find_annexb_start_codes(const uint8_t* data, size_t size);
std::string to_json(const Header& header);

}  // namespace bsparser
