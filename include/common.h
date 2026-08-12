#pragma once
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>
#include <sstream>


namespace bsparser {

    using Bytes = std::vector<uint8_t>;

    enum class Codec { VP8, VP9, AV1, AVC, HEVC, VVC, Unknown };

    enum class ScanBackend { Scalar, Sse2, Neon, Avx2 };

    enum class UnitKind { Frame, Obu, NalUnit };

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


    constexpr size_t kNotFound = std::numeric_limits<size_t>::max();

    inline size_t legacy_start_code(const uint8_t* data, size_t size, size_t from, size_t& startCodeSize)
    {
        startCodeSize = 0;

        if (size < 3 || from > size - 3)
        {
            return kNotFound;
        }

        for (size_t i = from; i + 3 <= size; ++i)
        {
            if (data[i] != 0 || data[i + 1] != 0) continue;

            // 00 00 00 01
            if (data[i + 2] == 0 && i + 3 < size && data[i + 3] == 1)
            {
                startCodeSize = 4;
                return i;
            }

            // 00 00 01
            if (data[i + 2] == 1)
            {
                startCodeSize = 3;
                return i;
            }
        }

        return kNotFound;
    }

    inline void field(Header& h, std::string_view key, uint64_t value)
    {
        h.fields[std::string(key)] = std::to_string(value);
    }

    inline void field(Header& h, std::string_view key, const std::string& value)
    {
        h.fields[std::string(key)] = value;
    }

    inline uint16_t le16(const Bytes& b, size_t p)
    {
        return uint16_t(b.at(p)) | uint16_t(b.at(p + 1)) << 8;
    }

    inline uint32_t le32(const Bytes& b, size_t p)
    {
        return uint32_t(b.at(p)) | uint32_t(b.at(p + 1)) << 8 | uint32_t(b.at(p + 2)) << 16 | uint32_t(b.at(p + 3)) << 24;
    }

    inline uint64_t le64(const Bytes& b, size_t p)
    {
        uint64_t n = 0;
        for (unsigned i = 0; i < 8; ++i)
            n |= uint64_t(b.at(p + i)) << (8 * i);
        return n;
    }

    inline std::string fourcc(const Bytes& b, size_t p)
    {
        return {b.begin() + p, b.begin() + p + 4};
    }

    inline Bytes rbsp(const Bytes& nal, size_t start)
    {
        Bytes out;
        out.reserve(nal.size());
        unsigned zeros = 0;
        for (size_t i = start; i < nal.size(); ++i)
        {
            if (zeros >= 2 && nal[i] == 3)
            {
                zeros = 0;
                continue;
            }
            out.push_back(nal[i]);
            zeros = nal[i] == 0 ? zeros + 1 : 0;
        }
        return out;
    }


    inline size_t start_code(const Bytes& d, size_t from, size_t& startCodeSize)
    {
        return legacy_start_code(d.data(), d.size(), from, startCodeSize);
    }

    inline std::string esc(const std::string& v)
    {
        std::ostringstream s;
        for (char c : v)
        {
            if (c == '"' || c == '\\') s << '\\';
            if (c == '\n')
                s << "\\n";
            else
                s << c;
        }
        return s.str();
    }

}