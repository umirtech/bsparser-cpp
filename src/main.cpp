#include "bsparser.h"

#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <sstream>
#include <stdexcept>

namespace
{

using bsparser::Header;

std::string html_escape(const std::string& value)
{
    std::string escaped;
    escaped.reserve(value.size());
    for (char c : value)
    {
        switch (c)
        {
            case '&':
                escaped += "&amp;";
                break;
            case '<':
                escaped += "&lt;";
                break;
            case '>':
                escaped += "&gt;";
                break;
            case '"':
                escaped += "&quot;";
                break;
            case '\'':
                escaped += "&#39;";
                break;
            default:
                escaped += c;
                break;
        }
    }
    return escaped;
}

std::string json_array(const std::vector<Header>& headers)
{
    std::ostringstream output;
    output << '[';
    for (size_t i = 0; i < headers.size(); ++i)
    {
        if (i) output << ',';
        output << bsparser::to_json(headers[i]);
    }
    return output << ']', output.str();
}

std::string base64_encode(const std::vector<uint8_t>& bytes)
{
    static constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string encoded;
    encoded.reserve((bytes.size() + 2) / 3 * 4);
    for (size_t index = 0; index < bytes.size(); index += 3)
    {
        const size_t remaining = bytes.size() - index;
        const uint32_t value = static_cast<uint32_t>(bytes[index]) << 16 |
                               (remaining > 1 ? static_cast<uint32_t>(bytes[index + 1]) << 8 : 0) |
                               (remaining > 2 ? bytes[index + 2] : 0);
        encoded += kAlphabet[(value >> 18) & 0x3f];
        encoded += kAlphabet[(value >> 12) & 0x3f];
        encoded += remaining > 1 ? kAlphabet[(value >> 6) & 0x3f] : '=';
        encoded += remaining > 2 ? kAlphabet[value & 0x3f] : '=';
    }
    return encoded;
}

void write_html_report(
    const std::string& path,
    const std::string& input_path,
    const std::string& format,
    size_t input_size,
    const std::vector<Header>& headers,
    const std::vector<uint8_t>& input
)
{
    std::ofstream output(path, std::ios::binary);
    if (!output) throw std::runtime_error("unable to create HTML report: " + path);

    std::map<std::string, size_t> type_counts;
    size_t keyframes = 0;
    for (const Header& header : headers)
    {
        ++type_counts[header.type];
        keyframes += header.keyframe ? 1 : 0;
    }

    output << R"(<!doctype html><html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>bsparser report</title>
<style>
:root{color-scheme:dark;--bg:#0b1020;--panel:#151d31;--line:#2e3a56;--text:#e7edf9;--muted:#9daccc;--accent:#63a7ff;--key:#38d39f}*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--text);font:14px/1.45 system-ui,sans-serif}header{padding:24px max(24px,calc((100vw - 1500px)/2));background:#0e1628;border-bottom:1px solid var(--line)}h1{margin:0 0 5px;font-size:24px}.muted{color:var(--muted);word-break:break-all}.stats,.chips,.tools{display:flex;flex-wrap:wrap;gap:9px}.stats{margin-top:17px}.card,.chip{background:var(--panel);border:1px solid var(--line);border-radius:8px;padding:9px 12px}.card b{display:block;font-size:19px;color:var(--accent)}.chips{margin-top:12px}.chip{border-radius:999px;padding:3px 9px}main{max-width:1600px;margin:auto;padding:20px 24px}.tools{margin-bottom:13px}input,select,button{font:inherit;color:var(--text);background:var(--panel);border:1px solid var(--line);border-radius:6px;padding:8px 10px}input{min-width:270px}button{cursor:pointer}button:hover{border-color:var(--accent)}.grid{display:grid;grid-template-columns:minmax(0,1fr) 430px;gap:16px}.box{background:var(--panel);border:1px solid var(--line);border-radius:8px;overflow:hidden}table{width:100%;border-collapse:collapse}th{position:sticky;top:0;background:#18223a;color:var(--muted);text-align:left}th,td{padding:8px 10px;border-bottom:1px solid var(--line);white-space:nowrap}tbody tr{cursor:pointer}tbody tr:hover,tbody tr.active{background:#243454}.type{color:#cfbaff}.key{color:var(--key);font-weight:700}.detail{padding:16px;position:sticky;top:12px;max-height:calc(100vh - 120px);overflow:auto}.detail h2{margin-top:0}dt{color:var(--muted);margin-top:10px}dd{margin:1px 0;overflow-wrap:anywhere}.empty{padding:16px;color:var(--muted)}.position{width:72px;height:6px;border-radius:3px;background:#293550;overflow:hidden}.position i{display:block;height:100%;background:var(--accent)}pre{margin:10px 0 0;padding:10px;background:#0a0f1d;border:1px solid var(--line);border-radius:6px;overflow:auto;color:#c7d5f2;font:12px/1.35 ui-monospace,monospace}@media(max-width:1050px){.grid{grid-template-columns:1fr}.detail{position:static;max-height:none}}
</style></head><body><header><h1>Raw bitstream inspection report</h1><div class="muted">)"
           << html_escape(input_path) << " | " << html_escape(format) << " | " << input_size
           << R"( bytes</div><div class="stats"><div class="card"><b>)" << headers.size()
           << R"(</b>units / headers</div><div class="card"><b>)" << keyframes
           << R"(</b>keyframes</div><div class="card"><b>)" << type_counts.size()
           << R"(</b>distinct types</div></div><div class="chips">)";
    for (const auto& [type, count] : type_counts)
    {
        output << "<span class=\"chip\">" << html_escape(type) << ": " << count << "</span>";
    }
    output
        << R"(</div></header><main><div class="tools"><input id="q" placeholder="Filter type, field, value, or offset"><select id="type"><option value="">All types</option></select><button id="keys">Keyframes only</button><button id="download">Download JSON</button></div><div class="grid"><section class="box"><table><thead><tr><th>#</th><th>Offset (Dec / Hex)</th><th>Position</th><th>Length</th><th>Type</th><th>Fields</th><th>Key</th><th>Summary</th></tr></thead><tbody id="rows"></tbody></table><div id="empty" class="empty" hidden>No matching units.</div></section><aside class="box detail"><h2 id="title">Select a unit</h2><div id="detail" class="muted">Click a row to inspect parsed syntax fields and source bytes.</div></aside></div></main>
<script>
const headers=)"
        << json_array(headers) << R"(;const source=Uint8Array.from(atob(')" << base64_encode(input)
        << R"('),c=>c.charCodeAt(0));
const rows=document.querySelector('#rows'),query=document.querySelector('#q'),type=document.querySelector('#type'),keys=document.querySelector('#keys'),empty=document.querySelector('#empty');
const esc=v=>{const e=document.createElement('span');e.textContent=v;return e.innerHTML};let onlyKeys=false,selected=-1;
[...new Set(headers.map(h=>h.type))].sort().forEach(v=>type.add(new Option(v,v)));
function summary(h){const f=h.fields||{};const dims=f.width&&f.height?`${f.width}x${f.height}`:'';const poc=f.poc!==undefined?`POC ${f.poc}`:(f.slice_pic_order_cnt_lsb!==undefined?`POC ${f.slice_pic_order_cnt_lsb}`:'');const st=f.slice_type_name?`Type ${f.slice_type_name}`:'';const prof=f.profile_name?`${f.profile_name}`:(f.seq_profile!==undefined?`Prof ${f.seq_profile}`:'');const ts=f.timestamp!==undefined?`TS ${f.timestamp}`:'';const fps=f.fps?`${f.fps} fps`:'';const bd=f.bit_depth?`${f.bit_depth}-bit`:'';return[dims,poc,st,prof,bd,fps,ts].filter(Boolean).join(' | ');}
function matches(h){const hexAddr='0x'+h.offset.toString(16).padStart(8,'0');const hay=`${h.offset} ${hexAddr} ${h.length} ${h.type} ${h.keyframe} ${Object.entries(h.fields||{}).flat().join(' ')}`.toLowerCase();return(!onlyKeys||h.keyframe)&&(!type.value||h.type===type.value)&&hay.includes(query.value.toLowerCase());}
function preview(h){const begin=Math.min(h.offset,source.length),end=Math.min(begin+h.length,source.length),data=source.slice(begin,Math.min(begin+256,end));const lines=[];for(let i=0;i<data.length;i+=16){const part=data.slice(i,i+16),hex=[...part].map(v=>v.toString(16).padStart(2,'0')).join(' ').padEnd(47,' '),ascii=[...part].map(v=>v>=32&&v<127?String.fromCharCode(v):'.').join('');lines.push((begin+i).toString(16).padStart(8,'0')+'  '+hex+'  '+ascii)}return `${end-begin} source bytes; showing ${data.length}\n${lines.join('\n')}`;}
function details(i){selected=i;const h=headers[i],f=h.fields||{};const hexAddr='0x'+h.offset.toString(16).padStart(8,'0');document.querySelector('#title').textContent=`#${i} | ${h.type}`;let s=`<dl><dt>Frame # / Unit #</dt><dd>${i}</dd><dt>Offset (Dec / Hex)</dt><dd>${h.offset} (${hexAddr})</dd><dt>Length</dt><dd>${h.length} bytes</dd><dt>Keyframe</dt><dd>${h.keyframe?'yes':'no'}</dd><dt>Parsed fields</dt><dd>${Object.keys(f).length}</dd>`;for(const[k,v]of Object.entries(f))s+=`<dt>${esc(k)}</dt><dd>${esc(v)}</dd>`;s+=`</dl><h3>Source bytes</h3><pre>${esc(preview(h))}</pre>`;document.querySelector('#detail').innerHTML=s;render();}
function render(){const out=[];headers.forEach((h,i)=>{if(!matches(h))return;const hexAddr='0x'+h.offset.toString(16).padStart(8,'0'),percent=source.length?Math.min(100,h.offset/source.length*100):0;out.push(`<tr class="${i===selected?'active':''}" data-i="${i}"><td>${i}</td><td>${h.offset} <span class="muted">(${hexAddr})</span></td><td><div class="position" title="${percent.toFixed(2)}%"><i style="width:${percent}%"></i></div></td><td>${h.length}</td><td class="type">${esc(h.type)}</td><td>${Object.keys(h.fields||{}).length}</td><td class="${h.keyframe?'key':''}">${h.keyframe?'KEY':''}</td><td>${esc(summary(h))}</td></tr>`)});rows.innerHTML=out.join('');empty.hidden=out.length>0;rows.querySelectorAll('tr').forEach(r=>r.onclick=()=>details(+r.dataset.i));}
query.oninput=render;type.onchange=render;keys.onclick=()=>{onlyKeys=!onlyKeys;keys.textContent=onlyKeys?'Show all units':'Keyframes only';render();};document.querySelector('#download').onclick=()=>{const b=new Blob([JSON.stringify(headers,null,2)],{type:'application/json'}),a=Object.assign(document.createElement('a'),{href:URL.createObjectURL(b),download:'bsparser-dump.json'});a.click();URL.revokeObjectURL(a.href);};render();
</script></body></html>)";
}

void print_usage()
{
    std::cerr << "Usage: bsparse <ivf|vp8|vp9|av1|avc|hevc|vvc> <input> [--html <report.html>]\n";
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        print_usage();
        return 2;
    }

    const std::string format = argv[1];
    const std::string input_path = argv[2];
    std::string html_path;
    for (int i = 3; i < argc; ++i)
    {
        if (std::string(argv[i]) == "--html" && i + 1 < argc)
            html_path = argv[++i];
        else
        {
            print_usage();
            return 2;
        }
    }

    std::ifstream file(input_path, std::ios::binary);
    if (!file)
    {
        std::cerr << "Unable to open input file: " << input_path << '\n';
        return 2;
    }

    std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)), {});
    try
    {
        std::vector<Header> headers;
        if (format == "ivf")
        {
            auto state = bsparser::create_state();
            bsparser::IvfParser parser(state);
            headers = parser.feed(data);
            bsparser::destroy_state(state);
        }
        else
        {
            auto state = bsparser::create_state();
            const auto codec = bsparser::codec_from_name(format);
            if (codec == bsparser::Codec::Unknown) throw std::invalid_argument("unknown codec: " + format);
            bsparser::StreamParser parser(codec,state);
            headers = parser.feed(data);
            auto tail = parser.finish();
            headers.insert(headers.end(), tail.begin(), tail.end());
            bsparser::destroy_state(state);
        }

        if (!html_path.empty())
        {
            write_html_report(html_path, input_path, format, data.size(), headers, data);
            std::cerr << "HTML report written to " << html_path << '\n';
        }

        for (const auto& header : headers)
        {
            std::cout << bsparser::to_json(header) << '\n';
        }
    }
    catch (const std::exception& error)
    {
        std::cerr << "Parse error: " << error.what() << '\n';
        return 1;
    }
}
