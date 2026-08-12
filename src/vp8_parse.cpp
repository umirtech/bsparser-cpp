#include "vp8_parse.h"
#include <iomanip>


namespace bsparser{
    
    Header parse_vp8(const Bytes& d, uint64_t off)
    {
        if (d.size() < 3) throw std::out_of_range("truncated VP8 frame tag");
        uint32_t tag = uint32_t(d[0]) | uint32_t(d[1]) << 8 | uint32_t(d[2]) << 16;
        Header h{off, d.size(), (tag & 1) ? "P" : "I", !(tag & 1), {}};
        field(h, "frame_type", tag & 1);
        field(h, "frame_type_name", (tag & 1) ? "INTER_FRAME" : "KEY_FRAME");
        field(h, "version", (tag >> 1) & 7);
        field(h, "show_frame", (tag >> 4) & 1);
        field(h, "partition_length", tag >> 5);
        if (!(tag & 1))
        {
            if (d.size() < 10) throw std::out_of_range("truncated VP8 key frame");
            field(
                    h,
                    "sync_code",
                    "0x" +
                    [&]
                    {
                        std::ostringstream s;
                        s << std::hex << std::setw(6) << std::setfill('0')
                            << (uint32_t(d[3]) << 16 | uint32_t(d[4]) << 8 | d[5]);
                        return s.str();
                    }()
            );

            auto w_raw = uint16_t(d[6]) | uint16_t(d[7]) << 8;
            auto h_raw = uint16_t(d[8]) | uint16_t(d[9]) << 8;
            field(h, "width", w_raw & 0x3fff);
            field(h, "horizontal_scale", w_raw >> 14);
            field(h, "height", h_raw & 0x3fff);
            field(h, "vertical_scale", h_raw >> 14);
        }
        return h;
    }
}