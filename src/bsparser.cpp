#include "bsparser.h"
#include <cstdio>

namespace bsparser
{

    struct ParserState{
        HevcParseState hevcParseState;
    };


    ParserState* create_state(){
        return new ParserState;
    }

    void destroy_state(ParserState* state){
        delete state;
    }
    
    std::vector<Header> parse_unit(Codec c, const Bytes& b, uint64_t off,ParserState* parserState)
    {

        if (off > b.size() || b.size() - off < 2) {
            return {};
        }

        switch (c)
        {
            case Codec::VP8:
                return {parse_vp8(b, off)};
            case Codec::VP9:
                return {parse_vp9(b, off)};
            case Codec::AV1:
                return parse_av1(b, off);
            case Codec::AVC:
                return {parse_avc(b, off)};
            case Codec::HEVC:
                return {parse_hevc(b, off,parserState->hevcParseState)};
            case Codec::VVC:
                return {parse_vvc(b, off)};
            default:
                throw std::invalid_argument("unknown codec");
        }
    }

    std::vector<Header> parse_unit(Codec codec, const Unit& unit,ParserState* parserState)
    {
        return parse_unit(codec, unit.bytes, unit.offset,parserState);
    }

    UnitScanner::UnitScanner(Codec codec,ParserState* parserState) 
        : codec_(codec),parserState_(parserState)
    {
        if (codec == Codec::Unknown)
        {
            throw std::invalid_argument("UnitScanner requires a known codec");
        }
    }

    std::vector<Unit> UnitScanner::feed(const Bytes& data)
    {
        return feed(data.data(), data.size());
    }

    std::vector<Unit> UnitScanner::feed(const uint8_t* data, size_t size)
    {

        if (size == 0)
        {
            return {};
        }

        if (codec_ == Codec::VP8 || codec_ == Codec::VP9)
        {
            Unit unit;
            unit.kind = UnitKind::Frame;
            unit.offset = pending_offset_;
            unit.bytes.assign(data, data + size);
            unit.keyframe = codec_ == Codec::VP8 && !(data[0] & 1);
            unit.frame_start = false;
            pending_offset_ += size;
            return {std::move(unit)};
        }

        pending_.insert(pending_.end(), data, data + size);

        if (codec_ == Codec::AV1)
        {
            return scan_av1_obus(false);
        }

        return scan_annexb(false);
    }

    std::vector<Unit> UnitScanner::finish()
    {
        std::vector<Unit> units;
        if (codec_ == Codec::AV1)
        {
            units = scan_av1_obus(true);
        }
        else if (codec_ == Codec::AVC || codec_ == Codec::HEVC || codec_ == Codec::VVC)
        {
            units = scan_annexb(true);
        }
        reset();
        return units;
    }

    void UnitScanner::reset()
    {
        pending_.clear();
        pending_begin_ = 0;
        pending_offset_ = 0;
        saw_start_code_ = false;
    }

    void UnitScanner::compact_pending()
    {
        if (pending_begin_ == 0)
        {
            return;
        }
        if (pending_begin_ == pending_.size())
        {
            pending_.clear();
            pending_begin_ = 0;
        }
        else if (pending_begin_ >= 64 * 1024 && pending_begin_ * 2 >= pending_.size())
        {
            pending_.erase(pending_.begin(), pending_.begin() + pending_begin_);
            pending_begin_ = 0;
        }
    }

    Unit UnitScanner::make_annexb_unit(
        const uint8_t* payload,
        size_t payload_size,
        uint64_t payload_offset,
        size_t start_code_size
    ) const
    {
        Unit unit;
        unit.kind = UnitKind::NalUnit;
        unit.offset = payload_offset;
        unit.start_code_size = start_code_size;
        unit.bytes.assign(payload, payload + payload_size);

        if (unit.bytes.empty())
            return unit;

        if (codec_ == Codec::AVC)
        {
            unit.type = unit.bytes[0] & 0x1F;

            if (unit.type == 1 || unit.type == 5)
            {
                unit.keyframe = unit.type == 5;
            }
        }
        else if (codec_ == Codec::HEVC)
        {
            if (unit.bytes.size() < 2)
                return unit;

            unit.type = (unit.bytes[0] >> 1) & 0x3F;

            unit.keyframe =
                unit.type == 19 ||
                unit.type == 20; 
        }
        else if (codec_ == Codec::VVC)
        {
            if (unit.bytes.size() < 2)
                return unit;

            unit.keyframe = false;
        }

        return unit;
    }

    std::vector<Unit> UnitScanner::scan_annexb(bool end_of_stream)
    {
        std::vector<Unit> units;

        const size_t no_position = kNotFound;

        size_t first = start_code(pending_, pending_begin_, startCodeSize);

        if (!saw_start_code_)
        {
            if (first == no_position)
            {
                const size_t available = pending_.size() - pending_begin_;

                if (available > startCodeSize)
                {
                    const size_t discard = available - startCodeSize;
                    pending_begin_ += discard;
                    pending_offset_ += discard;
                    compact_pending();
                }

                return units;
            }

            pending_offset_ += first - pending_begin_;
            pending_begin_ = first;
            saw_start_code_ = true;
        }

        while (true)
        {
            const size_t available = pending_.size() - pending_begin_;

            if (available < startCodeSize)
            {
                break;
            }


            const size_t payload_begin = pending_begin_ + startCodeSize;

            const size_t next = start_code(pending_, payload_begin, startCodeSize);

            if (next == no_position)
            {
                if (end_of_stream && available > startCodeSize)
                {
                    units.push_back(make_annexb_unit(
                        pending_.data() + payload_begin,
                        pending_.size() - payload_begin,
                        pending_offset_ + startCodeSize,
                        startCodeSize
                    ));

                    pending_offset_ += available;
                    pending_begin_ = pending_.size();
                    compact_pending();
                }

                break;
            }

            if (next > payload_begin)
            {
                units.push_back(make_annexb_unit(
                    pending_.data() + payload_begin,
                    next - payload_begin,
                    pending_offset_ + startCodeSize,
                    startCodeSize
                ));
            }

            pending_offset_ += next - pending_begin_;

            pending_begin_ = next;

            compact_pending();
        }

        return units;
    }

    std::vector<Unit> UnitScanner::scan_av1_obus(bool end_of_stream)
    {
        std::vector<Unit> units;
        size_t position = pending_begin_;
        while (position < pending_.size())
        {
            const size_t obu_start = position;
            const uint8_t header = pending_[position++];
            if ((header & 0x80) != 0)
            {
                throw std::runtime_error("invalid AV1 OBU forbidden bit");
            }
            const uint8_t obu_type = (header >> 3) & 0x0f;
            if ((header & 0x04) != 0)
            {
                if (position == pending_.size())
                {
                    position = obu_start;
                    break;
                }
                ++position;
            }
            if ((header & 0x02) == 0)
            {
                if (!end_of_stream)
                {
                    position = obu_start;
                    break;
                }
                position = pending_.size();
            }
            else
            {
                uint64_t payload_size = 0;
                unsigned shift = 0;
                bool complete_size = false;
                while (position < pending_.size() && shift <= 56)
                {
                    const uint8_t byte = pending_[position++];
                    payload_size |= uint64_t(byte & 0x7f) << shift;
                    shift += 7;
                    if ((byte & 0x80) == 0)
                    {
                        complete_size = true;
                        break;
                    }
                }
                if (!complete_size || payload_size > pending_.size() - position)
                {
                    position = obu_start;
                    break;
                }
                position += static_cast<size_t>(payload_size);
            }

            Unit unit;
            unit.kind = UnitKind::Obu;
            unit.offset = pending_offset_ + obu_start - pending_begin_;
            unit.type = obu_type;
            unit.bytes.assign(pending_.begin() + obu_start, pending_.begin() + position);
            unit.frame_start = false;
            if (obu_type == 3 || obu_type == 6)
            {
                try
                {
                    auto headers = parse_unit(
                        Codec::AV1,
                        unit.bytes, 
                        unit.offset,
                        parserState_);

                    unit.keyframe = !headers.empty() && headers.front().keyframe;
                }
                catch (const std::exception&)
                {
                    // Boundary extraction succeeds even if optional frame metadata is absent.
                }
            }
            units.push_back(std::move(unit));
        }

        if (position != pending_begin_)
        {
            pending_offset_ += position - pending_begin_;
            pending_begin_ = position;
            compact_pending();
        }
        if (end_of_stream && pending_begin_ != pending_.size())
        {
            throw std::out_of_range("truncated AV1 OBU at end of stream");
        }
        return units;
    }

    StreamParser::StreamParser(Codec codec,ParserState* parserState) 
        : codec_(codec),parserState_(parserState)
    {
        if (codec == Codec::Unknown) throw std::invalid_argument("unknown codec");
    }

    std::vector<Header> StreamParser::feed(const Bytes& data)
    {
        return feed(data.data(), data.size());
    }

    std::vector<Header> StreamParser::feed(const uint8_t* data, size_t size)
    {
        std::vector<Header> out;
        if (codec_ == Codec::VP8 || codec_ == Codec::VP9 || codec_ == Codec::AV1)
        {
            Bytes unit(data, data + size);
            const uint64_t unit_offset = input_offset_;
            input_offset_ += size;
            return parse_unit(codec_, unit, unit_offset,parserState_);
        }

        if (!size) return out;

        pending_.insert(pending_.end(), data, data + size);
        input_offset_ += size;

        size_t first = start_code(pending_, 0, startCodeSize);

        if (!annexb_started_)
        {
            if (first == std::string::npos)
            {
                if (pending_.size() > startCodeSize)
                {
                    const size_t keep = startCodeSize;

                    pending_offset_ += pending_.size() - keep;

                    pending_.erase(
                        pending_.begin(),
                        pending_.end() - keep);
                }

                return out;
            }

            pending_offset_ += first;

            pending_.erase(
                pending_.begin(),
                pending_.begin() + first);

            annexb_started_ = true;
        }

        while (true)
        {
            if (pending_.size() < startCodeSize)
                break;

            const size_t next =
                start_code(
                    pending_,
                    startCodeSize,
                    startCodeSize);

            if (next == std::string::npos)
                break;

            if (next > startCodeSize)
            {
                Bytes nal(
                    pending_.begin() + startCodeSize,
                    pending_.begin() + next);

                auto hs = parse_unit(
                    codec_,
                    nal,
                    0,
                    parserState_);

                out.insert(
                    out.end(),
                    hs.begin(),
                    hs.end());
            }

            pending_offset_ += next;

            pending_.erase(
                pending_.begin(),
                pending_.begin() + next);
        }

        return out;
    }

    std::vector<Header> StreamParser::finish()
    {
        std::vector<Header> out;

        if (annexb_started_ && pending_.size() >= startCodeSize)
        {
            Bytes nal(pending_.begin() + startCodeSize, pending_.end());

            auto hs = parse_unit(codec_, nal, 0,parserState_);

            out.insert(out.end(), hs.begin(), hs.end());
        }
        
        pending_.clear();
        annexb_started_ = false;
        return out;
    }


    IvfParser::IvfParser(ParserState* parserState) :parserState_(parserState){}

    std::vector<Header> IvfParser::feed(const Bytes& data)
    {
        return feed(data.data(), data.size());
    }

    std::vector<Header> IvfParser::feed(const uint8_t* data, size_t size)
    {
        pending_.insert(pending_.end(), data, data + size);
        std::vector<Header> out;
        if (!got_file_header_)
        {
            if (pending_.size() < 32) return out;
            if (fourcc(pending_, 0) != "DKIF") throw std::runtime_error("not an IVF stream");
            uint16_t header_size = le16(pending_, 6);
            if (header_size < 32 || pending_.size() < header_size) return out;
            auto f = fourcc(pending_, 8);
            codec_ = codec_from_name(f);
            Header h{offset_, header_size, "IVF", false, {}};
            field(h, "version", le16(pending_, 4));
            field(h, "fourcc", f);
            field(h, "width", le16(pending_, 12));
            field(h, "height", le16(pending_, 14));
            field(h, "frame_rate", le32(pending_, 16));
            field(h, "time_scale", le32(pending_, 20));
            field(h, "num_frames", le32(pending_, 24));
            out.push_back(std::move(h));
            pending_.erase(pending_.begin(), pending_.begin() + header_size);
            offset_ += header_size;
            got_file_header_ = true;
        }

        while (pending_.size() >= 12)
        {
            uint32_t length = le32(pending_, 0);
            if (pending_.size() < 12ull + length) break;
            uint64_t ts = le64(pending_, 4);
            Bytes frame(pending_.begin() + 12, pending_.begin() + 12 + length);

            auto hs = parse_unit(
                codec_,
                 frame,
                  offset_ + 12,
                  parserState_);

            for (auto& h : hs)
            {
                field(h, "timestamp", ts);
                out.push_back(std::move(h));
            }
            pending_.erase(pending_.begin(), pending_.begin() + 12 + length);
            offset_ += 12 + length;
        }
        return out;
    }

    Codec IvfParser::codec() const noexcept
    {
        return codec_;
    }

    Codec codec_from_name(const std::string& s)
    {
        if (s == "vp8" || s == "VP80") return Codec::VP8;
        if (s == "vp9" || s == "VP90") return Codec::VP9;
        if (s == "av1" || s == "AV01") return Codec::AV1;
        if (s == "avc" || s == "h264" || s == "264" || s == "H264") return Codec::AVC;
        if (s == "hevc" || s == "h265" || s == "265" || s == "H265" || s == "HEVC") return Codec::HEVC;
        if (s == "vvc" || s == "h266" || s == "266" || s == "H266" || s == "VVC") return Codec::VVC;
        return Codec::Unknown;
    }

    const char* codec_name(Codec c)
    {
        switch (c)
        {
            case Codec::VP8:
                return "VP8";
            case Codec::VP9:
                return "VP9";
            case Codec::AV1:
                return "AV1";
            case Codec::AVC:
                return "AVC";
            case Codec::HEVC:
                return "HEVC";
            case Codec::VVC:
                return "VVC";
            default:
                return "Unknown";
        }
    }

    std::vector<size_t> find_annexb_start_codes(const uint8_t* data, size_t size)
    {
        if (data == nullptr && size >= 3)
        {
            throw std::invalid_argument("data must not be null when size is non-zero");
        }

        std::vector<size_t> offsets;
        size_t position = 0;
        size_t startCodeSize = 0;
        while (position < size)
        {
            const size_t found = legacy_start_code(data, size, position, startCodeSize);
            if (found == kNotFound) break;
            offsets.push_back(found);
            position = found + startCodeSize;
        }
        return offsets;
    }

    std::string to_json(const Header& h)
    {
        std::ostringstream s;
        s << "{\"offset\":" << h.offset << ",\"length\":" << h.length << R"(,"type":")" << esc(h.type) << R"(","keyframe":)"
          << (h.keyframe ? "true" : "false") << ",\"fields\":{";
        bool first = true;
        for (const auto& [k, v] : h.fields)
        {
            if (!first) s << ',';
            first = false;
            s << '\"' << esc(k) << "\":\"" << esc(v) << '\"';
        }
        return s << "}}", s.str();
    }

}  // namespace bsparser
