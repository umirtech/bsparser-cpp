/*
 * ---------------------------------------------------------------------------
 * bs_cli - command-line bitstream parser
 * ---------------------------------------------------------------------------
 *
 * Parses HEVC / AVC / VVC / AV1 / VP9 / VP8 streams (raw, or muxed files via
 * the demux layer) using the unified bs::parse() API and exports a structured
 * report to JSON or to a self-contained HTML viewer (with inline filter
 * controls).
 *
 * Usage:
 *
 *   bs_cli <input> [options]
 *
 *   --codec <hevc|avc|vvc|av1|vp9|vp8|auto>
 *                                codec path (default: auto-detect)
 *   --format <annexb|length>    NAL framing for raw streams (default: annexb;
 *                               muxed containers select their own framing)
 *   --length-size <1..4>        length-prefix width (default: 4)
 *   --out <file>                output file (.json / .html / .htm)
 *   --json                      force JSON output
 *   --html                      force HTML output
 *   -h, --help                  show this help
 *
 * If --out is omitted the JSON report is printed to stdout.
 */

#include "report.hpp"

#include <demux/demuxer.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <span>
#include <string>
#include <vector>

namespace {

void print_help(const char* prog) {
    std::cout << "Usage: " << prog << " <input> [options]\n\n"
              << "  --codec <hevc|avc|vvc|av1|vp9|vp8|auto>\n"
              << "                            codec path (default: auto)\n"
              << "  --format <annexb|length>  framing for raw streams (default: annexb)\n"
              << "  --length-size <1..4>       length-prefix width (default: 4)\n"
              << "  --out <file>               output (.json / .html / .htm)\n"
              << "  --json                     force JSON output\n"
              << "  --html                     force HTML output\n"
              << "  -h, --help                 show this help\n\n"
              << "With no --out the JSON report is written to stdout.\n";
}

/*
 * Auto-detector for raw streams (demux already handled).
 *
 * 1. IVF: DKIF + VP80/VP90 fourcc
 * 2. AV1 OBU: Annex-B vs low-overhead OBU probing via obu_framer
 * 3. VVC: Annex-B NAL types 12-15, 25-28 contain DCI/OPI/VPS/SPS/PPS
 * 4. HEVC/AVC: scan up to 64 Annex-B NALs for VPS/SPS/PPS
 */
[[nodiscard]]
bs::Codec detect_codec(std::span<const std::uint8_t> data) {
    // IVF
    if (data.size() >= 32 && std::memcmp(data.data(), "DKIF", 4) == 0) {
        const bool vp9 = data.size() >= 12 && data[8] == 'V' && data[9] == 'P' && data[10] == '9';
        return vp9 ? bs::Codec::Vp9 : bs::Codec::Vp8;
    }
    // AV1 OBU — try to parse as OBU, check for SEQUENCE_HEADER (type 1)
    {
        bs::av1::ObuFramer f{data};
        unsigned seq = 0, other = 0;
        while (f.valid() && seq + other < 8) {
            try {
                bs::av1::Obu obu = bs::av1::parse_obu(f.obu());
                if (obu.type() == 1)
                    ++seq;
                else
                    ++other;
            } catch (...) {
                ++other;
            }
            f.next();
        }
        if (seq > 0)
            return bs::Codec::Av1;
    }
    // VVC Annex-B — validate by actually parsing VPS/SPS (avoids AVC SEI alias)
    {
        bs::AnnexBNalIterator it{data};
        unsigned vvc_ps = 0, vvc_nal = 0;
        while (it.valid() && vvc_nal < 32) {
            auto nal = it.nal();
            if (nal.size() >= 2) {
                try {
                    auto vnal = bs::vvc::parse_nal_unit(nal);
                    unsigned vtype = vnal.nal_type();
                    if (vtype == 14) {  // VPS
                        try {
                            bs::RbspReader r(vnal.payload_bytes());
                            (void)bs::vvc::parse_vps(r);
                            ++vvc_ps;
                        } catch (...) {
                        }
                    } else if (vtype == 15) {  // SPS
                        try {
                            bs::RbspReader r(vnal.payload_bytes());
                            (void)bs::vvc::parse_sps(r);
                            ++vvc_ps;
                        } catch (...) {
                        }
                    } else if (vtype == 16) {  // PPS
                        try {
                            bs::RbspReader r(vnal.payload_bytes());
                            (void)bs::vvc::parse_pps(r);
                            ++vvc_ps;
                        } catch (...) {
                        }
                    }
                } catch (...) {
                }
                ++vvc_nal;
            }
            it.next();
        }
        if (vvc_ps > 0)
            return bs::Codec::Vvc;
    }
    // HEVC vs AVC
    unsigned hevc_ps = 0, avc_ps = 0;
    std::size_t i = 0;
    unsigned nal_count = 0;
    while (i < data.size() && nal_count < 64) {
        if (i + 4 <= data.size() && data[i] == 0x00 && data[i + 1] == 0x00 && data[i + 2] == 0x00 &&
            data[i + 3] == 0x01)
            i += 4;
        else if (i + 3 <= data.size() && data[i] == 0x00 && data[i + 1] == 0x00 &&
                 data[i + 2] == 0x01)
            i += 3;
        else
            break;
        if (i >= data.size())
            break;
        const uint8_t b0 = data[i], b1 = (i + 1 < data.size()) ? data[i + 1] : 0;
        const unsigned hevc_type = (b0 >> 1) & 0x3F;
        if ((hevc_type == 32 || hevc_type == 33 || hevc_type == 34) && (b1 >> 3) == 0 &&
            (b1 & 0x07) != 0)
            ++hevc_ps;
        if (b0 == 0x67 || b0 == 0x68 || b0 == 0x27 || b0 == 0x28)
            ++avc_ps;
        ++nal_count;
        size_t j = i;
        while (j + 3 < data.size() &&
               !(data[j] == 0x00 && data[j + 1] == 0x00 && data[j + 2] == 0x01))
            ++j;
        if (j + 3 >= data.size())
            break;
        i = j;
    }
    if (hevc_ps > avc_ps)
        return bs::Codec::Hevc;
    if (avc_ps > 0)
        return bs::Codec::Avc;
    if (data.size() >= 3 && data[0] == 0x00 && data[1] == 0x00 && data[2] == 0x01)
        i = 3;
    else if (data.size() >= 4 && data[0] == 0x00 && data[1] == 0x00 && data[2] == 0x00 &&
             data[3] == 0x01)
        i = 4;
    if (i >= data.size())
        return bs::Codec::Hevc;
    const uint8_t b0 = data[i];
    const unsigned avc_type = b0 & 0x1F;
    if (avc_type >= 1 && avc_type <= 21)
        return bs::Codec::Avc;
    return bs::Codec::Hevc;
}

enum class OutputKind { Json, Html };

/*
 * Read a file in a single seek + read. std::istreambuf_iterator<char> walks
 * the input character-by-character and is unusably slow for multi-hundred-MB
 * streams (a 467MB MKV took ~3s just to load).
 */
[[nodiscard]]
std::vector<std::uint8_t> read_file(const std::string& path, std::streamsize limit = -1) {
    std::ifstream in(path, std::ios::binary);

    if (!in) {
        return {};
    }

    in.seekg(0, std::ios::end);
    const std::streamoff n = in.tellg();
    in.seekg(0, std::ios::beg);

    if (n <= 0) {
        return {};
    }

    std::vector<std::uint8_t> buf(
        static_cast<std::size_t>(limit >= 0 ? std::min<std::streamoff>(n, limit) : n)
    );

    if (buf.empty()) {
        return buf;
    }

    in.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
    buf.resize(static_cast<std::size_t>(in.gcount()));

    return buf;
}

[[nodiscard]]
OutputKind infer_kind(const std::string& path) {
    const auto pos = path.find_last_of('.');
    if (pos == std::string::npos) {
        return OutputKind::Json;
    }

    const std::string ext = path.substr(pos + 1);

    if (ext == "html" || ext == "htm") {
        return OutputKind::Html;
    }

    return OutputKind::Json;
}

}  // namespace

int main(int argc, char** argv) {
    using namespace bs;

    if (argc < 2) {
        print_help(argv[0]);
        return 1;
    }

    std::string input_path;
    std::string out_path;
    std::string codec_arg = "auto";
    std::string format_arg = "annexb";
    unsigned length_size = 4;
    bool have_out = false;
    bool force_json = false;
    bool force_html = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];

        if (arg == "-h" || arg == "--help") {
            print_help(argv[0]);
            return 0;
        }

        auto need_value = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::cerr << "error: " << name << " requires a value\n";
                std::exit(1);
            }
            return argv[++i];
        };

        if (arg == "--codec") {
            codec_arg = need_value("--codec");
        } else if (arg == "--format") {
            format_arg = need_value("--format");
        } else if (arg == "--length-size") {
            length_size =
                static_cast<unsigned>(std::strtoul(need_value("--length-size"), nullptr, 10));
        } else if (arg == "--out") {
            out_path = need_value("--out");
            have_out = true;
        } else if (arg == "--json") {
            force_json = true;
        } else if (arg == "--html") {
            force_html = true;
        } else if (arg[0] == '-' && arg != "-") {
            std::cerr << "error: unknown option '" << arg << "'\n";
            print_help(argv[0]);
            return 1;
        } else if (input_path.empty()) {
            input_path = arg;
        } else {
            std::cerr << "error: unexpected argument '" << arg << "'\n";
            return 1;
        }
    }

    if (input_path.empty()) {
        std::cerr << "error: no input file\n";
        print_help(argv[0]);
        return 1;
    }

    /*
     * Resolve codec.
     */
    Codec codec;

    if (codec_arg == "hevc") {
        codec = Codec::Hevc;
    } else if (codec_arg == "avc") {
        codec = Codec::Avc;
    } else if (codec_arg == "vvc") {
        codec = Codec::Vvc;
    } else if (codec_arg == "av1") {
        codec = Codec::Av1;
    } else if (codec_arg == "vp9") {
        codec = Codec::Vp9;
    } else if (codec_arg == "vp8") {
        codec = Codec::Vp8;
    } else if (codec_arg == "auto") {
        /*
         * Probe a generous prefix: parameter sets can appear hundreds of
         * KB in when a stream starts with large SEI messages.
         */
        const std::vector<std::uint8_t> head = read_file(input_path, 512 * 1024);

        if (head.empty()) {
            std::cerr << "error: cannot open '" << input_path << "'\n";
            return 1;
        }

        codec = detect_codec(std::span<const std::uint8_t>(head.data(), head.size()));
    } else {
        std::cerr << "error: unknown --codec '" << codec_arg << "'\n";
        return 1;
    }

    /*
     * Resolve framing.
     */
    NalFramingMode mode;

    if (format_arg == "annexb") {
        mode = NalFramingMode::AnnexB;
    } else if (format_arg == "length" || format_arg == "length-prefixed") {
        mode = NalFramingMode::LengthPrefixed;
    } else {
        std::cerr << "error: unknown --format '" << format_arg << "'\n";
        return 1;
    }

    if (length_size < 1 || length_size > 4) {
        std::cerr << "error: --length-size must be 1..4\n";
        return 1;
    }

    /*
     * Load the stream.
     */
    std::vector<std::uint8_t> bytes = read_file(input_path);

    if (bytes.empty()) {
        std::cerr << "error: cannot open '" << input_path << "'\n";
        return 1;
    }

    if (bytes.size() < 3) {
        std::cerr << "error: input too small\n";
        return 1;
    }

    std::span<const std::uint8_t> data{bytes.data(), bytes.size()};

    /*
     * Container auto-detection: try demux for all known containers
     * (MP4/MOV/fMP4, MKV/WebM, FLV, AVI, TS, IVF, OGG, PS) via
     * bs::demux::sniff/demux. The demuxer's es.codec/framing always
     * win for muxed files (container is authoritative).
     */
    std::vector<std::uint8_t> demuxed;
    const demux::Container container = demux::sniff(data);
    if (container != demux::Container::Unknown) {
        const demux::ElementaryStream es = demux::demux(container, data);
        if (es.ok && !es.bytes.empty()) {
            demuxed = es.bytes;
            data = std::span<const std::uint8_t>(demuxed.data(), demuxed.size());
            codec = es.codec;
            mode = es.framing;
            static const char* container_names[] = {
                "?", "MP4", "MPEG-TS", "AVI", "FLV", "IVF", "MKV", "Ogg", "PS"
            };
            const unsigned ci = static_cast<unsigned>(container);
            const char* cname = ci < 9 ? container_names[ci] : "?";
            std::cout << "container=" << cname << " codec=" << es.codec_name << " " << es.width
                      << "x" << es.height << " (demuxed " << es.bytes.size() << " bytes)\n";
        } else {
            std::cerr << "warning: detected container " << static_cast<unsigned>(container)
                      << " but demux failed; parsing as raw stream\n";
        }
    }
    // Raw streams: pick framing from codec when auto (AV1→OBU, VP9/8→IVF)
    if (container == demux::Container::Unknown && codec_arg == "auto" && format_arg == "annexb") {
        if (codec == Codec::Av1)
            mode = NalFramingMode::Obu;
        else if (codec == Codec::Vp9 || codec == Codec::Vp8)
            mode = NalFramingMode::Ivf;
    }

    /*
     * Parse + build the report — hardened for unsupported input.
     * Any exception from build_report (malformed/truncated/unsupported
     * codec) is turned into a user error, not a crash (fuzz-safe).
     */
    cli::Report report;
    try {
        report = cli::build_report(codec, data, mode, length_size);
    } catch (const std::exception& e) {
        std::cerr << "error: unsupported or malformed input (" << e.what() << ")\n";
        std::cerr << "hint: try --codec <hevc|avc|vvc|av1|vp9|vp8> and --format "
                     "<annexb|length|obu|ivf>\n";
        return 2;
    } catch (...) {
        std::cerr << "error: unsupported or malformed input (unknown)\n";
        return 2;
    }

    // No NALs/OBUs parsed at all — treat as unsupported (e.g. text, PDF, empty video track)
    if (report.entries.empty() && report.parsed == 0) {
        // Distinguish "supported container but no valid video" vs pure garbage
        if (container != demux::Container::Unknown) {
            std::cerr
                << "error: container detected (" << static_cast<unsigned>(container)
                << ") but no decodable video stream found — unsupported codec or empty track\n";
        } else {
            // Quick magic check for obviously non-video files
            bool looks_like_text = true;
            for (size_t k = 0; k < std::min<size_t>(data.size(), 256); ++k) {
                unsigned char c = data[k];
                if (c < 0x09 || (c > 0x0D && c < 0x20) || c > 0x7E) {
                    looks_like_text = false;
                    break;
                }
            }
            if (looks_like_text && data.size() > 0) {
                std::cerr << "error: input does not look like a video bitstream (no start codes, "
                             "no OBU, no IVF)\n";
            } else {
                std::cerr
                    << "error: no NALs/OBUs parsed — unsupported, truncated, or empty stream\n";
            }
            std::cerr << "hint: file type not recognized; supported: Annex-B HEVC/AVC/VVC, AV1 "
                         "OBU, VP8/9 IVF, "
                         "and containers MP4/MOV/fMP4, MKV/WebM, FLV, AVI, TS, OGG, PS\n";
        }
        // Still emit the empty JSON report for tooling, but signal failure
        const std::string empty_payload = (infer_kind(out_path) == OutputKind::Html)
                                              ? cli::to_html(report)
                                              : cli::to_json(report);
        if (have_out) {
            std::ofstream out(out_path, std::ios::binary);
            if (out)
                out.write(empty_payload.data(), (std::streamsize)empty_payload.size());
        } else {
            std::cout << empty_payload;
        }
        return 2;
    }

    // Non-fatal warning: explicit --codec disagrees with parse result
    if (codec_arg != "auto" && report.entries.empty() && report.parsed == 0) {
        std::cerr << "warning: --codec " << codec_arg << " produced no output — try --codec auto\n";
    }

    /*
     * Determine output kind.
     */
    OutputKind kind = OutputKind::Json;

    if (force_json) {
        kind = OutputKind::Json;
    } else if (force_html) {
        kind = OutputKind::Html;
    } else if (have_out) {
        kind = infer_kind(out_path);
    }

    const std::string payload =
        (kind == OutputKind::Html) ? cli::to_html(report) : cli::to_json(report);

    if (have_out) {
        std::ofstream out(out_path, std::ios::binary);

        if (!out) {
            std::cerr << "error: cannot write '" << out_path << "'\n";
            return 1;
        }

        out.write(payload.data(), static_cast<std::streamsize>(payload.size()));

        std::cout << "wrote " << payload.size() << " bytes to " << out_path << "\n";
    } else {
        std::cout << payload;
    }

    /*
     * Human summary.
     */
    std::size_t vcl = 0;
    for (const auto& e : report.entries) {
        if (e.vcl) {
            ++vcl;
        }
    }

    std::cout << "codec=" << report.codec << " framing=" << report.framing
              << " nals=" << report.entries.size() << " parsed=" << report.parsed << " vcl=" << vcl
              << "\n";

    return 0;
}
