#include "bsparser/bsparser.hpp"
#include <cassert>
#include <iostream>
using namespace bsparser;
int main() {
  {
    std::vector<uint8_t> d{0x10, 0, 0, 0x9d, 0x01, 0x2a, 0x80, 0x02, 0x68, 0x01};
    auto h = parse_unit(Codec::VP8, d).at(0);
    assert(h.keyframe && h.fields.at("width") == "640" && h.fields.at("height") == "360");
  }
  {
    std::vector<uint8_t> d{0,    0,    1,    0x67, 0x42, 0, 0x1e, 0xf4,
                           0x05, 0x01, 0xed, 0,    0,    1, 0x65, 0x88};
    StreamParser p(Codec::AVC);
    auto h = p.feed(d);
    auto tail = p.finish();
    h.insert(h.end(), tail.begin(), tail.end());
    assert(h.size() == 2 && h[0].fields.at("nal_unit_type") == "7" && h[1].keyframe);
  }
  {
    // NAL start codes can be split across network reads.
    std::vector<uint8_t> d{0,    0,    1,    0x67, 0x42, 0, 0x1e, 0xf4,
                           0x05, 0x01, 0xed, 0,    0,    1, 0x65, 0x88};
    UnitScanner scanner(Codec::AVC);
    auto units = scanner.feed(d.data(), 11);
    auto next = scanner.feed(d.data() + 11, d.size() - 11);
    auto last = scanner.finish();
    units.insert(units.end(), next.begin(), next.end());
    units.insert(units.end(), last.begin(), last.end());
    assert(units.size() == 2 && units[0].type == 7 && units[0].offset == 3);
    assert(units[1].type == 5 && units[1].frame_start && units[1].keyframe);
  }
  {
    std::vector<uint8_t> d{0x12, 0x01, 0x00};
    auto h = parse_unit(Codec::AV1, d);
    assert(h.size() == 1 && h[0].type == "Temporal delimiter");
  }
  {
    UnitScanner scanner(Codec::AV1);
    const std::vector<uint8_t> d{0x12, 0x01, 0x00};
    assert(scanner.feed(d.data(), 1).empty());
    auto units = scanner.feed(d.data() + 1, d.size() - 1);
    assert(units.size() == 1 && units[0].kind == UnitKind::Obu && units[0].type == 2);
  }
  {
    const std::vector<uint8_t> d{0xff, 0, 0, 1, 0x67, 0, 0, 0, 1, 0x68};
    const auto offsets = find_annexb_start_codes(d.data(), d.size());
    assert(offsets.size() == 2 && offsets[0] == 1 && offsets[1] == 5);
  }
  std::cout << "ok\n";
}
