#pragma once

/*
 * ===========================================================================
 * CLI report model + exporters
 * ===========================================================================
 *
 * Collects a structured report while a stream is parsed through the unified
 * bs::parse() API, and serialises it to JSON or to a self-contained HTML
 * viewer (with inline filter controls).
 *
 * This is a tooling helper, not part of the core library: it pulls in the
 * syntax parsers so each handler can attach a short human summary to the NAL
 * it receives.
 */

#include <bsparser.hpp>

#include <parser/avc_sei_parser.hpp>
#include <parser/avc_slice_parser.hpp>
#include <parser/hevc_sei_parser.hpp>
#include <parser/hevc_slice_parser.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace bs {
namespace cli {

/*
 * One parsed NAL unit.
 */
struct NalEntry {
    std::size_t index = 0;
    std::size_t offset = 0;
    std::string type;
    unsigned type_id = 0;
    bool vcl = false;
    std::size_t size = 0;
    std::string summary;
    std::vector<std::pair<std::string, std::string>> fields;
};

/*
 * Whole-stream report.
 */
struct Report {
    std::string codec;
    std::string framing;
    std::size_t parsed = 0;
    std::vector<NalEntry> entries;
};

/*
 * ---------------------------------------------------------------------------
 * Internal: shared capture state
 * ---------------------------------------------------------------------------
 *
 * The unified dispatcher callbacks are plain function pointers, so they cannot
 * capture.  A single build_report() call sets these process-local pointers and
 * clears them afterwards.
 */
namespace detail {

inline Report* g_report = nullptr;
inline const std::uint8_t* g_data_start = nullptr;
inline State* g_state = nullptr;

[[nodiscard]]
inline std::span<const std::byte> to_byte_span(std::span<const std::uint8_t> data) noexcept {
    return std::span<const std::byte>(reinterpret_cast<const std::byte*>(data.data()), data.size());
}

[[nodiscard]]
inline std::size_t offset_of(const std::uint8_t* p) noexcept {
    return static_cast<std::size_t>(p - g_data_start);
}

[[nodiscard]]
inline std::string hevc_type_name(NalUnitType t) noexcept {
    switch (t) {
        case NalUnitType::VPS_NUT:
            return "VPS_NUT";
        case NalUnitType::SPS_NUT:
            return "SPS_NUT";
        case NalUnitType::PPS_NUT:
            return "PPS_NUT";
        case NalUnitType::PREFIX_SEI_NUT:
            return "PREFIX_SEI_NUT";
        case NalUnitType::SUFFIX_SEI_NUT:
            return "SUFFIX_SEI_NUT";
        case NalUnitType::AUD_NUT:
            return "AUD_NUT";
        case NalUnitType::EOS_NUT:
            return "EOS_NUT";
        case NalUnitType::EOB_NUT:
            return "EOB_NUT";
        case NalUnitType::FD_NUT:
            return "FD_NUT";
        default:
            if (is_vcl_nal_unit(t)) {
                return "VCL";
            }
            return "NAL_" + std::to_string(static_cast<unsigned>(t));
    }
}

[[nodiscard]]
inline std::string avc_type_name(avc::NalUnitType t) noexcept {
    switch (t) {
        case avc::NalUnitType::SliceNonIdr:
            return "SliceNonIdr";
        case avc::NalUnitType::SliceDataPartitionA:
            return "SliceDataPartitionA";
        case avc::NalUnitType::SliceDataPartitionB:
            return "SliceDataPartitionB";
        case avc::NalUnitType::SliceDataPartitionC:
            return "SliceDataPartitionC";
        case avc::NalUnitType::SliceIdr:
            return "SliceIdr";
        case avc::NalUnitType::Sei:
            return "SEI";
        case avc::NalUnitType::Sps:
            return "SPS";
        case avc::NalUnitType::Pps:
            return "PPS";
        case avc::NalUnitType::AccessUnitDelimiter:
            return "AUD";
        case avc::NalUnitType::EndOfSequence:
            return "EndOfSequence";
        case avc::NalUnitType::EndOfStream:
            return "EndOfStream";
        case avc::NalUnitType::FillerData:
            return "FillerData";
        case avc::NalUnitType::SpsExtension:
            return "SpsExtension";
        default:
            if (avc::is_vcl_nal_unit(t)) {
                return "VCL";
            }
            return "NAL_" + std::to_string(static_cast<unsigned>(t));
    }
}

inline void add_entry(
    std::string type,
    unsigned type_id,
    bool vcl,
    const std::uint8_t* payload,
    std::size_t size,
    std::string summary,
    std::vector<std::pair<std::string, std::string>> fields = {}
) {
    if (g_report == nullptr) {
        return;
    }

    NalEntry entry;
    entry.index = g_report->entries.size();
    entry.offset = offset_of(payload);
    entry.type = std::move(type);
    entry.type_id = type_id;
    entry.vcl = vcl;
    entry.size = size;
    entry.summary = std::move(summary);
    entry.fields = std::move(fields);

    g_report->entries.push_back(std::move(entry));
}

}  // namespace detail

/*
 * ---------------------------------------------------------------------------
 * Report builder
 * ---------------------------------------------------------------------------
 */
[[nodiscard]]
inline Report build_report(
    Codec codec, std::span<const std::uint8_t> data, NalFramingMode mode, unsigned length_size = 4
) {
    Report report;
    report.codec = (codec == Codec::Hevc) ? "HEVC" : "AVC";
    report.framing = (mode == NalFramingMode::AnnexB) ? "Annex-B" : "Length-prefixed";

    auto state = create_state(codec);

    detail::g_report = &report;
    detail::g_data_start = data.data();
    detail::g_state = state.get();

    if (codec == Codec::Hevc) {
        BsNalHandlers handlers{};

        handlers.vps = [](const NalUnit& nal) {
            std::string summary = "VPS";
            std::vector<std::pair<std::string, std::string>> fields;
            try {
                RbspBitstreamReader reader(detail::to_byte_span(nal.payload_bytes()));
                auto vps = parse_video_parameter_set(reader);
                fields.emplace_back(
                    "vps_id", std::to_string(static_cast<unsigned>(vps.vps_video_parameter_set_id))
                );
                fields.emplace_back("max_layers", std::to_string(vps.max_layers()));
                fields.emplace_back("max_sub_layers", std::to_string(vps.max_sub_layers()));
                summary = "VPS id=" +
                          std::to_string(static_cast<unsigned>(vps.vps_video_parameter_set_id));
            } catch (...) {
                summary = "VPS (unparsable)";
            }
            detail::add_entry(
                detail::hevc_type_name(nal.type()),
                static_cast<unsigned>(nal.type()),
                false,
                nal.payload_bytes().data(),
                nal.payload_bytes().size(),
                summary,
                std::move(fields)
            );
        };

        handlers.sps = [](const NalUnit& nal) {
            std::string summary = "SPS";
            std::vector<std::pair<std::string, std::string>> fields;
            try {
                RbspBitstreamReader reader(detail::to_byte_span(nal.payload_bytes()));
                auto sps = parse_sequence_parameter_set(reader);
                fields.emplace_back(
                    "sps_id", std::to_string(static_cast<unsigned>(sps.sps_seq_parameter_set_id))
                );
                fields.emplace_back(
                    "vps_id", std::to_string(static_cast<unsigned>(sps.sps_video_parameter_set_id))
                );
                fields.emplace_back("width", std::to_string(sps.pic_width_in_luma_samples));
                fields.emplace_back("height", std::to_string(sps.pic_height_in_luma_samples));
                fields.emplace_back(
                    "chroma_format", std::to_string(static_cast<unsigned>(sps.chroma_format))
                );
                fields.emplace_back(
                    "luma_bit_depth", std::to_string(static_cast<unsigned>(sps.luma_bit_depth()))
                );
                summary = std::to_string(sps.pic_width_in_luma_samples) + "x" +
                          std::to_string(sps.pic_height_in_luma_samples) +
                          " chroma=" + std::to_string(static_cast<unsigned>(sps.chroma_format));
            } catch (...) {
                summary = "SPS (unparsable)";
            }
            detail::add_entry(
                detail::hevc_type_name(nal.type()),
                static_cast<unsigned>(nal.type()),
                false,
                nal.payload_bytes().data(),
                nal.payload_bytes().size(),
                summary,
                std::move(fields)
            );
        };

        handlers.pps = [](const NalUnit& nal) {
            std::string summary = "PPS";
            std::vector<std::pair<std::string, std::string>> fields;
            try {
                RbspBitstreamReader reader(detail::to_byte_span(nal.payload_bytes()));
                auto pps = parse_picture_parameter_set(reader);
                fields.emplace_back(
                    "pps_id", std::to_string(static_cast<unsigned>(pps.pps_pic_parameter_set_id))
                );
                fields.emplace_back(
                    "sps_id", std::to_string(static_cast<unsigned>(pps.pps_seq_parameter_set_id))
                );
                summary =
                    "PPS id=" + std::to_string(static_cast<unsigned>(pps.pps_pic_parameter_set_id));
            } catch (...) {
                summary = "PPS (unparsable)";
            }
            detail::add_entry(
                detail::hevc_type_name(nal.type()),
                static_cast<unsigned>(nal.type()),
                false,
                nal.payload_bytes().data(),
                nal.payload_bytes().size(),
                summary,
                std::move(fields)
            );
        };

        handlers.prefix_sei = [](const NalUnit& nal) {
            std::string summary = "PREFIX_SEI";
            std::size_t count = 0;
            try {
                auto sei = parse_sei_nal(nal);
                count = sei.size();
                summary = "PREFIX_SEI messages=" + std::to_string(count);
            } catch (...) {
                summary = "PREFIX_SEI (unparsable)";
            }
            detail::add_entry(
                detail::hevc_type_name(nal.type()),
                static_cast<unsigned>(nal.type()),
                false,
                nal.payload_bytes().data(),
                nal.payload_bytes().size(),
                summary,
                {{"messages", std::to_string(count)}}
            );
        };

        handlers.suffix_sei = [](const NalUnit& nal) {
            std::string summary = "SUFFIX_SEI";
            std::size_t count = 0;
            try {
                auto sei = parse_sei_nal(nal);
                count = sei.size();
                summary = "SUFFIX_SEI messages=" + std::to_string(count);
            } catch (...) {
                summary = "SUFFIX_SEI (unparsable)";
            }
            detail::add_entry(
                detail::hevc_type_name(nal.type()),
                static_cast<unsigned>(nal.type()),
                false,
                nal.payload_bytes().data(),
                nal.payload_bytes().size(),
                summary,
                {{"messages", std::to_string(count)}}
            );
        };

        handlers.slice = [](const NalUnit& nal) {
            const std::string name = detail::hevc_type_name(nal.type());
            detail::add_entry(
                name,
                static_cast<unsigned>(nal.type()),
                true,
                nal.payload_bytes().data(),
                nal.payload_bytes().size(),
                "Slice (" + name + ")",
                {}
            );
        };

        handlers.unsupported = [](const NalUnit& nal) {
            const std::string name = detail::hevc_type_name(nal.type());
            detail::add_entry(
                name,
                static_cast<unsigned>(nal.type()),
                false,
                nal.payload_bytes().data(),
                nal.payload_bytes().size(),
                name,
                {}
            );
        };

        report.parsed = parse(*state, data, mode, handlers, length_size);

    } else {
        avc::NalHandlers handlers{};

        handlers.sps = [](const avc::NalUnit& nal) {
            std::string summary = "SPS";
            std::vector<std::pair<std::string, std::string>> fields;
            try {
                RbspBitstreamReader reader(detail::to_byte_span(nal.payload_bytes()));
                auto sps = avc::parse_sequence_parameter_set(reader);
                fields.emplace_back(
                    "sps_id", std::to_string(static_cast<unsigned>(sps.seq_parameter_set_id))
                );
                fields.emplace_back(
                    "profile_idc", std::to_string(static_cast<unsigned>(sps.profile_idc))
                );
                fields.emplace_back(
                    "level_idc", std::to_string(static_cast<unsigned>(sps.level_idc))
                );
                fields.emplace_back("width", std::to_string(sps.pic_width_in_luma_samples()));
                fields.emplace_back("height", std::to_string(sps.pic_height_in_luma_samples()));
                fields.emplace_back(
                    "chroma_format_idc",
                    std::to_string(static_cast<unsigned>(sps.chroma_format_idc))
                );
                summary = std::to_string(sps.pic_width_in_luma_samples()) + "x" +
                          std::to_string(sps.pic_height_in_luma_samples()) +
                          " profile=" + std::to_string(static_cast<unsigned>(sps.profile_idc));
            } catch (...) {
                summary = "SPS (unparsable)";
            }
            detail::add_entry(
                detail::avc_type_name(nal.type()),
                static_cast<unsigned>(nal.type()),
                false,
                nal.payload_bytes().data(),
                nal.payload_bytes().size(),
                summary,
                std::move(fields)
            );
        };

        handlers.pps = [](const avc::NalUnit& nal) {
            std::string summary = "PPS";
            std::vector<std::pair<std::string, std::string>> fields;
            try {
                RbspBitstreamReader reader(detail::to_byte_span(nal.payload_bytes()));
                auto pps = avc::parse_picture_parameter_set(reader);
                fields.emplace_back(
                    "pps_id", std::to_string(static_cast<unsigned>(pps.pic_parameter_set_id))
                );
                fields.emplace_back(
                    "sps_id", std::to_string(static_cast<unsigned>(pps.seq_parameter_set_id))
                );
                summary =
                    "PPS id=" + std::to_string(static_cast<unsigned>(pps.pic_parameter_set_id));
            } catch (...) {
                summary = "PPS (unparsable)";
            }
            detail::add_entry(
                detail::avc_type_name(nal.type()),
                static_cast<unsigned>(nal.type()),
                false,
                nal.payload_bytes().data(),
                nal.payload_bytes().size(),
                summary,
                std::move(fields)
            );
        };

        handlers.sei = [](const avc::NalUnit& nal) {
            std::string summary = "SEI";
            std::size_t count = 0;
            try {
                auto sei = avc::parse_sei_nal(nal);
                count = sei.messages.size();
                summary = "SEI messages=" + std::to_string(count);
            } catch (...) {
                summary = "SEI (unparsable)";
            }
            detail::add_entry(
                detail::avc_type_name(nal.type()),
                static_cast<unsigned>(nal.type()),
                false,
                nal.payload_bytes().data(),
                nal.payload_bytes().size(),
                summary,
                {{"messages", std::to_string(count)}}
            );
        };

        handlers.slice = [](const avc::NalUnit& nal) {
            const std::string name = detail::avc_type_name(nal.type());
            detail::add_entry(
                name,
                static_cast<unsigned>(nal.type()),
                true,
                nal.payload_bytes().data(),
                nal.payload_bytes().size(),
                "Slice (" + name + ")",
                {}
            );
        };

        handlers.unsupported = [](const avc::NalUnit& nal) {
            const std::string name = detail::avc_type_name(nal.type());
            detail::add_entry(
                name,
                static_cast<unsigned>(nal.type()),
                false,
                nal.payload_bytes().data(),
                nal.payload_bytes().size(),
                name,
                {}
            );
        };

        report.parsed = parse(*state, data, mode, handlers, length_size);
    }

    detail::g_report = nullptr;
    detail::g_data_start = nullptr;
    detail::g_state = nullptr;

    return report;
}

/*
 * ---------------------------------------------------------------------------
 * JSON export
 * ---------------------------------------------------------------------------
 */
[[nodiscard]]
inline std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);

    for (char c : s) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
                    out += buf;
                } else {
                    out += c;
                }
        }
    }

    return out;
}

[[nodiscard]]
inline std::string to_json(const Report& report) {
    std::ostringstream os;
    os << "{\n";
    os << "  \"codec\": \"" << json_escape(report.codec) << "\",\n";
    os << "  \"framing\": \"" << json_escape(report.framing) << "\",\n";
    os << "  \"parsed\": " << report.parsed << ",\n";
    os << "  \"nals\": [\n";

    for (std::size_t i = 0; i < report.entries.size(); ++i) {
        const auto& e = report.entries[i];

        os << "    {\n";
        os << "      \"index\": " << e.index << ",\n";
        os << "      \"offset\": " << e.offset << ",\n";
        os << "      \"type\": \"" << json_escape(e.type) << "\",\n";
        os << "      \"type_id\": " << e.type_id << ",\n";
        os << "      \"vcl\": " << (e.vcl ? "true" : "false") << ",\n";
        os << "      \"size\": " << e.size << ",\n";
        os << "      \"summary\": \"" << json_escape(e.summary) << "\",\n";
        os << "      \"fields\": {";

        for (std::size_t f = 0; f < e.fields.size(); ++f) {
            if (f) {
                os << ", ";
            }
            os << "\"" << json_escape(e.fields[f].first) << "\": \""
               << json_escape(e.fields[f].second) << "\"";
        }

        os << "}\n";
        os << "    }";
        os << (i + 1 < report.entries.size() ? ",\n" : "\n");
    }

    os << "  ]\n";
    os << "}\n";

    return os.str();
}

/*
 * ---------------------------------------------------------------------------
 * HTML export (self-contained viewer with filter controls)
 * ---------------------------------------------------------------------------
 */
[[nodiscard]]
inline std::string to_html(const Report& report) {
    /*
     * Embed the report as JSON.  Escape "</" so the data cannot
     * prematurely close the <script> element.
     */
    std::string json = to_json(report);
    std::string safe;
    safe.reserve(json.size());

    for (std::size_t i = 0; i < json.size(); ++i) {
        if (json.compare(i, 2, "</") == 0) {
            safe += "<\\/";
            ++i;
        } else {
            safe += json[i];
        }
    }

    std::ostringstream os;
    os << "<!DOCTYPE html>\n";
    os << "<html lang=\"en\">\n";
    os << "<head>\n";
    os << "<meta charset=\"utf-8\">\n";
    os << "<meta name=\"viewport\" "
          "content=\"width=device-width, initial-scale=1\">\n";
    os << "<title>bsparser report — " << json_escape(report.codec) << "</title>\n";
    os << "<style>\n";
    os << "body{font-family:system-ui,Segoe UI,Roboto,sans-serif;"
          "margin:0;background:#0f1115;color:#e6e6e6}\n";
    os << "header{padding:16px 20px;background:#171a21;"
          "border-bottom:1px solid #2a2f3a;position:sticky;top:0;z-index:1}\n";
    os << "h1{font-size:18px;margin:0 0 4px}\n";
    os << ".meta{font-size:13px;color:#9aa4b2}\n";
    os << ".controls{display:flex;flex-wrap:wrap;gap:10px;"
          "align-items:center;margin-top:12px}\n";
    os << "input,select{background:#0f1115;color:#e6e6e6;"
          "border:1px solid #2a2f3a;border-radius:6px;"
          "padding:6px 8px;font-size:13px}\n";
    os << "label{font-size:13px;color:#9aa4b2;display:flex;"
          "gap:6px;align-items:center}\n";
    os << "table{border-collapse:collapse;width:100%;"
          "font-size:13px;margin-top:14px}\n";
    os << "th,td{text-align:left;padding:8px 10px;border-bottom:"
          "1px solid #20242d;vertical-align:top}\n";
    os << "th{position:sticky;top:150px;background:#171a21;"
          "color:#9aa4b2;font-weight:600}\n";
    os << "tr:hover td{background:#1b1f27}\n";
    os << ".vcl{color:#7ee787}.nonvcl{color:#79c0ff}\n";
    os << ".summary{color:#d6c77e}\n";
    os << ".count{font-size:13px;color:#9aa4b2;margin-left:auto}\n";
    os << "details{font-size:12px;color:#9aa4b2}\n";
    os << "</style>\n";
    os << "</head>\n";
    os << "<body>\n";
    os << "<header>\n";
    os << "<h1>bsparser — " << json_escape(report.codec) << " bitstream report</h1>\n";
    os << "<div class=\"meta\">framing: " << json_escape(report.framing)
       << " &middot; NAL units: " << report.entries.size() << " &middot; parsed: " << report.parsed
       << "</div>\n";
    os << "<div class=\"controls\">\n";
    os << "<input id=\"search\" type=\"text\" "
          "placeholder=\"filter by type / summary…\" "
          "style=\"min-width:240px\">\n";
    os << "<select id=\"typeFilter\"><option value=\"\">"
          "All types</option></select>\n";
    os << "<label><input id=\"vclOnly\" type=\"checkbox\"> "
          "VCL only</label>\n";
    os << "<label><input id=\"nonVclOnly\" type=\"checkbox\"> "
          "Non-VCL only</label>\n";
    os << "<span class=\"count\" id=\"count\"></span>\n";
    os << "</div>\n";
    os << "</header>\n";
    os << "<table>\n";
    os << "<thead><tr><th>#</th><th>Offset</th><th>Type</th>"
          "<th>Size</th><th>Summary</th><th>Details</th>"
          "</tr></thead>\n";
    os << "<tbody id=\"rows\"></tbody>\n";
    os << "</table>\n";
    os << "<script>\n";
    os << "const REPORT = " << safe << ";\n";
    os << R"JS(
const tbody = document.getElementById('rows');
const search = document.getElementById('search');
const typeFilter = document.getElementById('typeFilter');
const vclOnly = document.getElementById('vclOnly');
const nonVclOnly = document.getElementById('nonVclOnly');
const count = document.getElementById('count');

function buildTypeOptions() {
  const seen = new Set();
  REPORT.nals.forEach(n => { seen.add(n.type); });
  [...seen].sort().forEach(t => {
    const o = document.createElement('option');
    o.value = t; o.textContent = t;
    typeFilter.appendChild(o);
  });
}

function esc(s) {
  return String(s).replace(/[&<>"']/g, c => ({
    '&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'
  }[c]));
}

function matches(n, q, type, vc, nv) {
  if (type && n.type !== type) return false;
  if (vc && !n.vcl) return false;
  if (nv && n.vcl) return false;
  if (q) {
    const hay = (n.type + ' ' + n.summary + ' ' + JSON.stringify(n.fields)).toLowerCase();
    if (!hay.includes(q)) return false;
  }
  return true;
}

function render() {
  const q = search.value.trim().toLowerCase();
  const type = typeFilter.value;
  const vc = vclOnly.checked;
  const nv = nonVclOnly.checked;
  tbody.innerHTML = '';
  let shown = 0;
  REPORT.nals.forEach(n => {
    if (!matches(n, q, type, vc, nv)) return;
    shown++;
    const tr = document.createElement('tr');
    const cls = n.vcl ? 'vcl' : 'nonvcl';
    let details = '';
    for (const [k, v] of Object.entries(n.fields)) {
      details += esc(k) + ': ' + esc(v) + '<br>';
    }
    tr.innerHTML =
      '<td>' + n.index + '</td>' +
      '<td>0x' + n.offset.toString(16) + '</td>' +
      '<td class="' + cls + '">' + esc(n.type) + '</td>' +
      '<td>' + n.size + '</td>' +
      '<td class="summary">' + esc(n.summary) + '</td>' +
      '<td><details><summary>fields</summary>' + details + '</details></td>';
    tbody.appendChild(tr);
  });
  count.textContent = shown + ' / ' + REPORT.nals.length + ' shown';
}

[search, typeFilter, vclOnly, nonVclOnly].forEach(el =>
  el.addEventListener('input', render));
buildTypeOptions();
render();
)JS";
    os << "\n</script>\n";
    os << "</body>\n";
    os << "</html>\n";

    return os.str();
}

}  // namespace cli
}  // namespace bs
