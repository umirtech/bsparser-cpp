#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include <bitreader.h>

namespace bsparser {



// Parses one complete elementary unit: a VP frame, an AV1 OBU sequence, or a
// single Annex-B NAL payload (without its start code).
std::vector<Header> parse_unit(Codec codec, const std::vector<uint8_t>& bytes, uint64_t offset = 0);

// Convenience overload for a unit returned by UnitScanner.
std::vector<Header> parse_unit(Codec codec, const Unit& unit);

// Finds complete elementary-unit boundaries while accepting arbitrary chunks.
// Its boundaries and flags reproduce the bundled JavaScript reference parser:
// Annex-B uses the 3-byte [00 00 01] or 4-byte [00 00 00 01] markers, AV1 depends on its OBU size field, and
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
  size_t startCodeSize = 0;
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
  size_t startCodeSize = 0;
  bool annexb_started_ = false;
  bool first_annexb_header_ = true;
};

// Incremental IVF parser. It detects the codec from FourCC and emits the IVF
// header followed by parsed frame headers, each with a timestamp field.
class IvfParser {
 public:
  std::vector<Header> feed(const uint8_t* data, size_t size);
  std::vector<Header> feed(const std::vector<uint8_t>& data);
  [[nodiscard]] Codec codec() const noexcept;

 private:
  std::vector<uint8_t> pending_;
  uint64_t offset_ = 0;
  bool got_file_header_ = false;
  Codec codec_ = Codec::Unknown;
};

Codec codec_from_name(const std::string& name);

const char* codec_name(Codec codec);

// Stateless, zero-copy Annex-B scanner. Returned values are byte positions of
// 3-byte [00 00 01] or 4-byte [00 00 00 01] markers in `data`, matching the JavaScript reference parser.
std::vector<size_t> find_annexb_start_codes(const uint8_t* data, size_t size);

std::string to_json(const Header& header);

}  // namespace bsparser
