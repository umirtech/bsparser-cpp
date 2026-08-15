/*
 * ---------------------------------------------------------------------------
 * bs_cli - command-line bitstream parser
 * ---------------------------------------------------------------------------
 *
 * Parses an HEVC (.hevc) or AVC (.h264) Annex-B / length-prefixed stream using
 * the unified bs::parse() API and exports a structured report to JSON or to a
 * self-contained HTML viewer (with inline filter controls).
 *
 * Usage:
 *
 *   bs_cli <input> [options]
 *
 *   --codec <hevc|avc|auto>     codec path (default: auto-detect)
 *   --format <annexb|length>    NAL framing (default: annexb)
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
              << "  --codec <hevc|avc|auto>   codec path (default: auto)\n"
              << "  --format <annexb|length>  NAL framing (default: annexb)\n"
              << "  --length-size <1..4>       length-prefix width (default: 4)\n"
              << "  --out <file>               output (.json / .html / .htm)\n"
              << "  --json                     force JSON output\n"
              << "  --html                     force HTML output\n"
              << "  -h, --help                 show this help\n\n"
              << "With no --out the JSON report is written to stdout.\n";
}

/*
 * Very small auto-detector: scan the first few NAL units.
 *
 * The very first NAL of a stream is not necessarily a parameter set
 * (an encoder may start with AUD, SEI or a VCL slice), so look at up to
 * 64 leading Annex-B NALs and count HEVC vs AVC parameter-set types:
 *
 *   HEVC  VPS=32, SPS=33, PPS=34
 *   AVC   SPS=7,  PPS=8
 *
 * The codec with any matching parameter sets wins; with none we fall
 * back to inspecting the first NAL byte (an AVC type 1..21 is AVC,
 * otherwise HEVC).
 */
[[nodiscard]]
bs::Codec detect_codec(std::span<const std::uint8_t> data) {
    unsigned hevc_ps = 0;
    unsigned avc_ps = 0;
    std::size_t i = 0;
    unsigned nal_count = 0;

    while (i < data.size() && nal_count < 64) {
        if (i + 4 <= data.size() && data[i] == 0x00 && data[i + 1] == 0x00 && data[i + 2] == 0x00 &&
            data[i + 3] == 0x01) {
            i += 4;
        } else if (i + 3 <= data.size() && data[i] == 0x00 && data[i + 1] == 0x00 &&
                   data[i + 2] == 0x01) {
            i += 3;
        } else {
            break;
        }

        if (i >= data.size()) {
            break;
        }

        const std::uint8_t b0 = data[i];
        const std::uint8_t b1 = (i + 1 < data.size()) ? data[i + 1] : 0;

        /*
         * HEVC parameter sets: 2-byte NAL header.  For a base-layer
         * (layer_id 0) temporal-0 set, byte1 == 0x01 (temporal_id_plus1).
         * This distinguishes HEVC VPS/SPS/PPS from an AVC slice whose
         * first byte could alias the same HEVC type (e.g. AVC type-1
         * 0x41 -> HEVC type 32).
         */
        const unsigned hevc_type = (b0 >> 1) & 0x3F;

        if ((hevc_type == 32 || hevc_type == 33 || hevc_type == 34) && (b1 >> 3) == 0 &&
            (b1 & 0x07) != 0) {
            ++hevc_ps;
        }

        /*
         * AVC parameter sets: SPS = 0x67 / 0x27, PPS = 0x68 / 0x28.
         */
        if (b0 == 0x67 || b0 == 0x68 || b0 == 0x27 || b0 == 0x28) {
            ++avc_ps;
        }

        ++nal_count;

        /*
         * Advance to the next Annex-B start code.  If the stream is
         * length-prefixed there is no start code and we stop after the
         * first NAL, which the fallback below still resolves.
         */
        std::size_t j = i;

        while (j + 3 < data.size() &&
               !(data[j] == 0x00 && data[j + 1] == 0x00 && data[j + 2] == 0x01)) {
            ++j;
        }

        if (j + 3 >= data.size()) {
            break;
        }

        i = j;
    }

    if (hevc_ps > avc_ps) {
        return bs::Codec::Hevc;
    }

    if (avc_ps > 0) {
        return bs::Codec::Avc;
    }

    if (data.size() >= 3 && data[0] == 0x00 && data[1] == 0x00 && data[2] == 0x01) {
        i = 3;
    } else if (data.size() >= 4 && data[0] == 0x00 && data[1] == 0x00 && data[2] == 0x00 &&
               data[3] == 0x01) {
        i = 4;
    }

    if (i >= data.size()) {
        return bs::Codec::Hevc;
    }

    const std::uint8_t b0 = data[i];
    const unsigned avc_type = b0 & 0x1F;

    if (avc_type >= 1 && avc_type <= 21) {
        return bs::Codec::Avc;
    }

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
     * Container auto-detection: if the input is a muxed file
     * (MP4 / TS / FLV / AVI / IVF), demux it to an elementary
     * stream and let the demuxer choose the codec + framing.
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
                "?", "MP4", "MPEG-TS", "AVI", "FLV", "IVF", "MKV"
            };

            std::cout << "container=" << container_names[static_cast<unsigned>(container)]
                      << " codec=" << es.codec_name << " " << es.width << "x" << es.height
                      << " (demuxed " << es.bytes.size() << " bytes)\n";
        } else {
            std::cerr << "warning: detected " << static_cast<unsigned>(container)
                      << " container but demux failed; parsing as raw stream\n";
        }
    }

    /*
     * Parse + build the report.
     */
    cli::Report report = cli::build_report(codec, data, mode, length_size);

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
