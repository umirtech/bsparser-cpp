/*
 * ---------------------------------------------------------------------------
 * Broad fuzz target: exercises every codec parser and the container
 * demux layer on arbitrary bytes.
 * ---------------------------------------------------------------------------
 */
#include <bsparser.hpp>
#include <demux/demuxer.hpp>

#include <cstdint>
#include <cstring>
#include <span>

namespace {

void parse_with_codec(bs::Codec codec, bs::NalFramingMode framing,
                      std::span<const std::uint8_t> data) {
    if (data.empty()) {
        return;
    }

    try {
        auto state = bs::create_state(codec);

        switch (codec) {

        case bs::Codec::Hevc: {
            bs::HevcParsedHandlers h{};
            h.vps = [](const bs::VideoParameterSet&) {};
            h.sps = [](const bs::SequenceParameterSet&) {};
            h.pps = [](const bs::PictureParameterSet&) {};
            h.sei = [](const bs::ParsedSei&) {};
            h.slice = [](const bs::SliceSegmentHeader&) {};
            (void)bs::parse(*state, data, framing, h);
            break;
        }

        case bs::Codec::Avc: {
            bs::AvcParsedHandlers h{};
            h.sps = [](const bs::avc::SequenceParameterSet&) {};
            h.pps = [](const bs::avc::PictureParameterSet&) {};
            h.sei = [](const bs::avc::ParsedSei&) {};
            h.slice = [](const bs::avc::SliceHeader&) {};
            (void)bs::parse(*state, data, framing, h);
            break;
        }

        case bs::Codec::Vvc: {
            bs::VvcParsedHandlers h{};
            h.dci = [](const bs::vvc::Dci&) {};
            h.opi = [](const bs::vvc::Opi&) {};
            h.vps = [](const bs::vvc::VideoParameterSet&) {};
            h.sps = [](const bs::vvc::SequenceParameterSet&) {};
            h.pps = [](const bs::vvc::PictureParameterSet&) {};
            h.ph = [](const bs::vvc::PictureHeader&) {};
            h.slice = [](const bs::vvc::SliceHeader&) {};
            (void)bs::parse(*state, data, framing, h);
            break;
        }

        case bs::Codec::Av1: {
            bs::Av1ParsedHandlers h{};
            h.sequence_header = [](const bs::av1::SequenceHeader&) {};
            h.frame_header = [](const bs::av1::FrameHeader&) {};
            (void)bs::parse(*state, data, framing, h);
            break;
        }

        case bs::Codec::Vp9: {
            bs::Vp9ParsedHandlers h{};
            h.frame_header = [](const bs::vp9::FrameHeader&) {};
            (void)bs::parse(*state, data, framing, h);
            break;
        }

        case bs::Codec::Vp8: {
            bs::Vp8ParsedHandlers h{};
            h.frame_header = [](const bs::vp8::FrameHeader&) {};
            (void)bs::parse(*state, data, framing, h);
            break;
        }

        default:
            break;
        }
    } catch (...) {
        /* fuzz targets must never leak exceptions */
    }
}

} // namespace


extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    std::span<const std::uint8_t> sp(data, size);

    /*
     * Container demux path: auto-detect and parse the extracted
     * elementary stream.
     */
    try {
        const bs::demux::ElementaryStream es = bs::demux::demux(sp);

        if (es.ok && !es.bytes.empty()) {
            parse_with_codec(es.codec, es.framing, es.bytes);
        }
    } catch (...) {
    }

    /*
     * Raw-stream paths: hand the bytes to each codec with its
     * natural framing.
     */
    parse_with_codec(bs::Codec::Hevc, bs::NalFramingMode::AnnexB, sp);
    parse_with_codec(bs::Codec::Avc, bs::NalFramingMode::AnnexB, sp);
    parse_with_codec(bs::Codec::Vvc, bs::NalFramingMode::AnnexB, sp);
    parse_with_codec(bs::Codec::Av1, bs::NalFramingMode::Obu, sp);
    parse_with_codec(bs::Codec::Vp9, bs::NalFramingMode::Ivf, sp);
    parse_with_codec(bs::Codec::Vp8, bs::NalFramingMode::Ivf, sp);

    return 0;
}
