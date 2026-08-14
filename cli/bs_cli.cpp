/*
 * ---------------------------------------------------------------------------
 * bs_cli — command-line bitstream parser
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
 * Very small auto-detector: inspect the first NAL unit.
 *
 * HEVC SPS/PPS/VPS types are 33/34/32, which appear in the high byte of the
 * 2-byte HEVC header as (type>>1)&0x3F == 16..17.  AVC SPS/PPS are 7/8, i.e.
 * (b0 & 0x1F) in 1..21.  We pick HEVC when the inferred HEVC type looks like a
 * parameter set, otherwise AVC when the AVC type is in range, else HEVC.
 */
[[nodiscard]]
bs::Codec detect_codec(std::span<const std::uint8_t> data) {
    std::size_t i = 0;

    /*
     * Skip a leading Annex-B start code if present.
     */
    if (data.size() >= 4 && data[0] == 0x00 && data[1] == 0x00 && data[2] == 0x00 &&
        data[3] == 0x01) {
        i = 4;
    } else if (data.size() >= 3 && data[0] == 0x00 && data[1] == 0x00 && data[2] == 0x01) {
        i = 3;
    }

    if (i >= data.size()) {
        return bs::Codec::Hevc;
    }

    const std::uint8_t b0 = data[i];
    const unsigned hevc_type = (b0 >> 1) & 0x3F;
    const unsigned avc_type = b0 & 0x1F;

    if (hevc_type == 32 || hevc_type == 33 || hevc_type == 34) {
        return bs::Codec::Hevc;
    }

    if (avc_type >= 1 && avc_type <= 21) {
        return bs::Codec::Avc;
    }

    return bs::Codec::Hevc;
}

enum class OutputKind { Json, Html };

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
        std::ifstream probe(input_path, std::ios::binary);
        if (!probe) {
            std::cerr << "error: cannot open '" << input_path << "'\n";
            return 1;
        }
        const std::vector<std::uint8_t> head(
            (std::istreambuf_iterator<char>(probe)), std::istreambuf_iterator<char>()
        );
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
    std::ifstream in(input_path, std::ios::binary);

    if (!in) {
        std::cerr << "error: cannot open '" << input_path << "'\n";
        return 1;
    }

    const std::vector<std::uint8_t> bytes(
        (std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()
    );

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
