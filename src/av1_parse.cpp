
#include "av1_parse.h"
#include <iomanip>

namespace bsparser{
    
    std::vector<Header> parse_av1(const Bytes& d, uint64_t off)
    {
        std::vector<Header> out;
        size_t p = 0;
        while (p < d.size())
        {
            size_t begin = p;
            uint8_t x = d.at(p++);
            if (x & 0x80) throw std::runtime_error("invalid AV1 OBU forbidden bit");
            uint8_t type = (x >> 3) & 15;
            bool ext = x & 4, has = x & 2;
            uint8_t temporal_id = 0, spatial_id = 0;
            if (ext)
            {
                if (p >= d.size()) throw std::out_of_range("truncated AV1 extension");
                uint8_t ext_byte = d.at(p++);
                temporal_id = (ext_byte >> 5) & 7;
                spatial_id = (ext_byte >> 3) & 3;
            }
            uint64_t size = d.size() - p;
            if (has)
            {
                size = 0;
                unsigned shift = 0;
                for (;;)
                {
                    if (p >= d.size() || shift > 56) throw std::out_of_range("invalid AV1 size field");
                    uint8_t q = d[p++];
                    size |= uint64_t(q & 127) << shift;
                    shift += 7;
                    if (!(q & 128)) break;
                }
            }
            if (size > d.size() - p) throw std::out_of_range("truncated AV1 OBU payload");

            static const char* names[] = {
                    "Reserved",
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
                    "Padding"
            };

            Header h{off + begin, p + size - begin, names[type], false, {}};
            field(h, "obu_type", type);
            field(h, "obu_has_size_field", has);
            if (ext)
            {
                field(h, "obu_extension_flag", 1);
                field(h, "obu_temporal_id", temporal_id);
                field(h, "obu_spatial_id", spatial_id);
            }
            if (type == 1 && size >= 1)
            {
                Bytes payload(d.begin() + p, d.begin() + p + size);
                BitReader b(payload);
                try
                {
                    auto profile = b.u(3);
                    field(h, "seq_profile", profile);
                    auto still = b.u(1);
                    auto reduced = b.u(1);
                    field(h, "still_picture", still);
                    field(h, "reduced_still_picture_header", reduced);
                    if (reduced)
                    {
                        b.u(5);
                        auto wb = b.u(4) + 1, hb = b.u(4) + 1;
                        auto mw = b.u(wb) + 1, mh = b.u(hb) + 1;
                        field(h, "max_frame_width", mw);
                        field(h, "max_frame_height", mh);
                        field(h, "width", mw);
                        field(h, "height", mh);
                    }
                    else
                    {
                        auto timing_info = b.u(1);
                        field(h, "timing_info_present_flag", timing_info);
                        if (timing_info)
                        {
                            auto num_units = b.u(32);
                            auto time_scale = b.u(32);
                            field(h, "num_units_in_display_tick", num_units);
                            field(h, "time_scale", time_scale);
                            if (num_units > 0)
                            {
                                double fps = double(time_scale) / double(num_units);
                                std::ostringstream fps_s;
                                fps_s << std::fixed << std::setprecision(2) << fps;
                                field(h, "fps", fps_s.str());
                            }
                            if (b.u(1)) b.leb128();
                        }
                        auto decoder_model = b.u(1);
                        if (decoder_model) field(h, "decoder_model_info_present_flag", 1);
                        auto operating_points = b.u(5) + 1;
                        field(h, "operating_points_cnt", operating_points);
                        for (unsigned i = 0; i < operating_points; ++i)
                        {
                            b.u(12);
                            b.u(5);
                            if (b.u(1)) b.u(1);
                        }
                        auto wb = b.u(4) + 1, hb = b.u(4) + 1;
                        auto mw = b.u(wb) + 1, mh = b.u(hb) + 1;
                        field(h, "max_frame_width", mw);
                        field(h, "max_frame_height", mh);
                        field(h, "width", mw);
                        field(h, "height", mh);

                        field(h, "frame_width_bits_minus_1", 32 - __builtin_clz(mw) - 1);
                        field(h, "frame_height_bits_minus_1", 32 - __builtin_clz(mh) - 1);
                        auto frame_id_numbers = b.u(1);
                        field(h, "frame_id_numbers_present_flag", frame_id_numbers);
                        if (frame_id_numbers)
                        {
                            b.u(4);
                            b.u(3);
                        }
                        b.u(1);
                        b.u(1);
                        b.u(1);
                        b.u(1);
                        b.u(1);
                        b.u(1);
                        b.u(1);

                        auto high_bd = b.u(1);
                        field(h, "high_bitdepth", high_bd);
                        unsigned bit_depth = 8;
                        if (profile == 2 && high_bd)
                        {
                            auto twelve_bit = b.u(1);
                            bit_depth = twelve_bit ? 12 : 10;
                        }
                        else if (profile <= 2)
                        {
                            bit_depth = high_bd ? 10 : 8;
                        }
                        field(h, "bit_depth", bit_depth);
                        if (profile != 1)
                        {
                            field(h, "mono_chrome", b.u(1));
                        }
                        auto color_desc = b.u(1);
                        field(h, "color_description_present_flag", color_desc);
                        if (color_desc)
                        {
                            field(h, "color_primaries", b.u(8));
                            field(h, "transfer_characteristics", b.u(8));
                            field(h, "matrix_coefficients", b.u(8));
                        }
                    }
                }
                catch (const std::exception&)
                {
                }
            }
            if ((type == 3 || type == 6) && size)
            {
                Bytes payload(d.begin() + p, d.begin() + p + size);
                BitReader b(payload);
                try
                {
                    auto show_existing = b.u(1);
                    field(h, "show_existing_frame", show_existing);
                    if (!show_existing)
                    {
                        auto ft = b.u(2);
                        field(h, "frame_type", ft);
                        static const char* ft_names[] = {"KEY_FRAME", "INTER_FRAME", "INTRA_ONLY_FRAME", "SWITCH_FRAME"};
                        field(h, "frame_type_name", ft_names[ft]);
                        h.keyframe = ft == 0;
                        auto show_frame = b.u(1);
                        field(h, "show_frame", show_frame);
                    }
                }
                catch (const std::exception&)
                {
                }
            }
            out.push_back(std::move(h));
            p += size;
        }
        return out;
    }

}