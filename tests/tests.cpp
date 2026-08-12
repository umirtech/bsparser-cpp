#include "bsparser.h"

#include <algorithm>
#include <cassert>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

using namespace bsparser;

namespace
{

    std::vector<uint8_t> read_fixture(const char* name)
    {
        const std::string path = std::string(BSPARSER_TEST_FILES_DIR) + "/" + name;
        std::ifstream file(path, std::ios::binary);
        assert(file && "test fixture is missing");
        return {std::istreambuf_iterator<char>(file), {}};
    }

    std::vector<Unit> scan_annexb_fixture(Codec codec, const char* name)
    {
        const auto bytes = read_fixture(name);

        auto state = bsparser::create_state();
        UnitScanner scanner(codec,state);
        std::vector<Unit> units;
        constexpr size_t kChunkSize = 4093;
        for (size_t offset = 0; offset < bytes.size(); offset += kChunkSize)
        {
            const size_t size = std::min(kChunkSize, bytes.size() - offset);
            auto next = scanner.feed(bytes.data() + offset, size);
            units.insert(units.end(), next.begin(), next.end());
        }
        auto tail = scanner.finish();
        units.insert(units.end(), tail.begin(), tail.end());
        bsparser::destroy_state(state);
        return units;
    }

    void test_annexb_fixture(Codec codec, const char* name)
    {
        const auto units = scan_annexb_fixture(codec, name);

        assert(units.size() > 1);

        auto state = bsparser::create_state();
        bool has_keyframe = false;
        uint64_t previous_offset = 0;
        for (size_t index = 0; index < units.size(); ++index)
        {
            const auto& unit = units[index];
            assert(unit.kind == UnitKind::NalUnit);
            assert(!unit.bytes.empty());
            assert(unit.start_code_size != 3 || unit.start_code_size != 4);
            assert(!unit.frame_start);
            if (index != 0) assert(previous_offset < unit.offset);
            previous_offset = unit.offset;
            has_keyframe = has_keyframe || unit.keyframe;
            assert(!parse_unit(codec, unit,state).empty());
        }

        assert(has_keyframe);

        bsparser::destroy_state(state);
    }

    void test_ivf_fixture(const char* name, Codec expected_codec, const char* fourcc)
    {
        const auto bytes = read_fixture(name);
        auto state = bsparser::create_state();
        IvfParser parser(state);
        std::vector<Header> headers;
        constexpr size_t kChunkSize = 4093;
        for (size_t offset = 0; offset < bytes.size(); offset += kChunkSize)
        {
            const size_t size = std::min(kChunkSize, bytes.size() - offset);
            auto next = parser.feed(bytes.data() + offset, size);
            headers.insert(headers.end(), next.begin(), next.end());
        }

        assert(parser.codec() == expected_codec);
        assert(headers.size() > 1);
        assert(headers.front().type == "IVF");
        assert(headers.front().fields.at("fourcc") == fourcc);
        for (size_t index = 1; index < headers.size(); ++index)
        {
            assert(headers[index].fields.count("timestamp") == 1);
        }

        bsparser::destroy_state(state);
    }

}  // namespace

int main()
{

    test_annexb_fixture(Codec::AVC, "h264.h264");

    test_annexb_fixture(Codec::HEVC, "h265.h265");

    test_ivf_fixture("vp8.ivf", Codec::VP8, "VP80");

    test_ivf_fixture("vp9.ivf", Codec::VP9, "VP90");

    test_ivf_fixture("av1.ivf", Codec::AV1, "AV01");

    std::cout << "ok\n";
}
