
#include "vp9_parse.h"
#include <iomanip>

namespace bsparser{

    Header parse_vp9(const Bytes& d, uint64_t off)
    {
        BitReader b(d);
        Header h{off, d.size(), "VP9 frame", false, {}};
        try
        {
            auto marker = b.u(2);
            auto profile = b.u(1) | (b.u(1) << 1);
            field(h, "frame_marker", marker);
            field(h, "profile", profile);
            if (profile == 3) field(h, "reserved_zero", b.u(1));
            auto existing = b.u(1);
            field(h, "show_existing_frame", existing);
            if (existing)
            {
                field(h, "frame_to_show_map_idx", b.u(3));
                return h;
            }
            auto type = b.u(1);
            auto show = b.u(1);
            auto resilient = b.u(1);
            h.type = type ? "P" : "I";
            h.keyframe = !type;
            field(h, "frame_type", type);
            field(h, "frame_type_name", type ? "INTER_FRAME" : "KEY_FRAME");
            field(h, "show_frame", show);
            field(h, "error_resilient_mode", resilient);
            if (!type)
            {
                field(
                        h,
                        "frame_sync_code",
                        "0x" +
                        [&]
                        {
                            std::ostringstream s;
                            s << std::hex << std::setw(6) << std::setfill('0') << b.u(24);
                            return s.str();
                        }()
                );
                unsigned bit_depth = 8;
                if (profile >= 2)
                {
                    bit_depth = b.u(1) ? 12 : 10;
                }
                field(h, "bit_depth", bit_depth);
                auto cs = b.u(3);
                field(h, "color_space", cs);
                if (cs != 7)
                {
                    field(h, "color_range", b.u(1));
                }
                auto w = b.u(16) + 1, ht = b.u(16) + 1;
                field(h, "width", w);
                field(h, "height", ht);
                if (b.u(1))
                {
                    field(h, "render_width", b.u(16) + 1);
                    field(h, "render_height", b.u(16) + 1);
                }
                else
                {
                    field(h, "render_width", w);
                    field(h, "render_height", ht);
                }
            }
            else
            {
                auto intra_only = show ? 0 : b.u(1);
                field(h, "intra_only", intra_only);
                if (!resilient)
                {
                    field(h, "reset_frame_context", b.u(2));
                }
            }
        }
        catch (const std::exception&)
        {
        }
        return h;
    }

}