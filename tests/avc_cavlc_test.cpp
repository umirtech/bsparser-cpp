// SPDX-FileCopyrightText: 2026 Mohd Umeer
// SPDX-License-Identifier: BSD-2-Clause

#include "bsparser.hpp"
#include "avc_cavlc.hpp"
#include "utils/MotionBufferPool.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <span>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

struct Field {
    std::uint32_t width4x4 = 0;
    std::uint32_t height4x4 = 0;
    std::vector<bs::avc::MvCell> cells;
};

struct Context {
    bs::utils::MotionBufferPool* pool = nullptr;
    int pictures = 0;
    int ok = 0;
    std::vector<Field> p_fields;
};

bs::avc::MotionVectorBuffer acquire(void* opaque,
                                   const bs::avc::MotionVectorBufferRequest& request) {
    return static_cast<Context*>(opaque)->pool->acquire(request);
}

void ready(void* opaque, const bs::avc::MotionVectorBufferView& view) {
    auto* context = static_cast<Context*>(opaque);
    ++context->pictures;
    if (view.header == nullptr ||
        view.header->status != static_cast<std::uint8_t>(bs::avc::MotionVectorStatus::Ok)) {
        return;
    }
    ++context->ok;

    /*
     * Keep the P-picture list-0 grid for the optional FFmpeg comparison.
     * B pictures have a second active list and are skipped here.
     */
    if ((view.header->flags & bs::avc::MvPictureIntra) == 0 &&
        view.header->num_ref_idx[1] == 0) {
        const auto cells = bs::avc::motionCells(view);
        Field field;
        field.width4x4 = view.header->width4x4;
        field.height4x4 = view.header->height4x4;
        field.cells.assign(cells.begin(), cells.end());
        context->p_fields.push_back(std::move(field));
    }
}

std::vector<Field> read_mv_reference(const char* path) {
    std::ifstream in(path);
    if (!in) {
        std::fprintf(stderr, "cannot open %s\n", path);
        std::exit(1);
    }
    std::vector<Field> fields;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream header(line);
        std::string kind;
        header >> kind;
        if (kind != "picture") continue;
        Field field;
        int index = 0;
        header >> index >> field.width4x4 >> field.height4x4;
        field.cells.resize(static_cast<std::size_t>(field.width4x4) * field.height4x4);
        for (auto& cell : field.cells) {
            if (!std::getline(in, line)) {
                std::fprintf(stderr, "truncated motion-vector reference %s\n", path);
                std::exit(1);
            }
            std::istringstream entry(line);
            int x = 0;
            int y = 0;
            int dx = 0;
            int dy = 0;
            entry >> x >> y >> dx >> dy;
            cell.mv[0][0] = static_cast<std::int16_t>(dx);
            cell.mv[0][1] = static_cast<std::int16_t>(dy);
        }
        fields.push_back(std::move(field));
    }
    return fields;
}

/*
 * -----------------------------------------------------------
 * CavlcReader unit checks
 * -----------------------------------------------------------
 *
 * These run before every stream test so the same 56 ctest entries also
 * exercise every VLC family, long-code fallbacks, non-byte-aligned starts,
 * truncation and the bit-reservoir position semantics.
 */

struct BitWriter {
    std::vector<std::uint8_t> bytes;
    std::size_t bits = 0;

    explicit BitWriter(std::size_t leading_zeroes = 0) {
        for (std::size_t i = 0; i < leading_zeroes; ++i) push_bit(false);
    }

    void push_bit(bool value) {
        if ((bits & 7u) == 0) bytes.push_back(0);
        if (value) {
            bytes.back() = static_cast<std::uint8_t>(
                    bytes.back() | (0x80u >> (bits & 7u)));
        }
        ++bits;
    }

    void push_code(std::uint32_t code, int length) {
        for (int i = length - 1; i >= 0; --i) {
            push_bit(((code >> i) & 1u) != 0);
        }
    }

    void push_ue(std::uint32_t value) {
        const std::uint32_t code_num = value + 1u;
        int width = 0;
        while ((code_num >> width) > 1u) ++width;
        for (int i = 0; i < width; ++i) push_bit(false);
        for (int i = width; i >= 0; --i) push_bit(((code_num >> i) & 1u) != 0);
    }

    [[nodiscard]] std::span<const std::uint8_t> span() const {
        return std::span<const std::uint8_t>(bytes.data(), bytes.size());
    }
};

class ReaderChecks {
   public:
    void expect(bool condition, const char* what) {
        if (condition) return;
        std::fprintf(stderr, "cavlc reader check failed: %s\n", what);
        ++failures_;
    }

    [[nodiscard]] int failures() const { return failures_; }

   private:
    int failures_ = 0;
};

bool decode_coeff_family(ReaderChecks& checks,
                         const char* name,
                         const std::uint8_t* lengths,
                         const std::uint8_t* bits,
                         int count,
                         std::uint32_t nC,
                         bool chroma_dc_420,
                         bool chroma_dc_422,
                         bool& saw_long) {
    const std::size_t offsets[] = {0, 1, 3, 5, 7};
    for (std::size_t offset : offsets) {
        for (int symbol = 0; symbol < count; ++symbol) {
            const int length = lengths[symbol];
            if (length <= 0) continue;
            BitWriter writer(offset);
            writer.push_code(bits[symbol], length);

            bs::avc::CavlcReader reader(writer.span(), offset);
            const auto token =
                    reader.decode_coeff_token(nC, chroma_dc_420, chroma_dc_422);
            const bool ok =
                    token.total_coeff == (symbol >> 2) &&
                    token.trailing_ones == (symbol & 3) &&
                    reader.bit_position() == offset + static_cast<std::size_t>(length);
            if (!ok) {
                std::fprintf(stderr,
                             "cavlc reader check failed: %s offset=%zu sym=%d\n",
                             name, offset, symbol);
                checks.expect(false, name);
            }
            if (length > 9) saw_long = true;
        }
    }
    return saw_long;
}

bool decode_total_zeros_family(ReaderChecks& checks,
                               const char* name,
                               const std::uint8_t* lengths,
                               const std::uint8_t* bits,
                               int row,
                               std::uint32_t total_coeff,
                               bool chroma_dc_420,
                               bool chroma_dc_422,
                               int count,
                               bool& saw_long) {
    const std::size_t offsets[] = {0, 2, 7};
    for (std::size_t offset : offsets) {
        for (int symbol = 0; symbol < count; ++symbol) {
            const int length = lengths[symbol];
            if (length <= 0) continue;
            BitWriter writer(offset);
            writer.push_code(bits[symbol], length);

            bs::avc::CavlcReader reader(writer.span(), offset);
            const std::uint32_t value =
                    reader.decode_total_zeros(total_coeff, chroma_dc_420,
                                              chroma_dc_422);
            const bool ok =
                    value == static_cast<std::uint32_t>(symbol) &&
                    reader.bit_position() == offset + static_cast<std::size_t>(length);
            if (!ok) {
                std::fprintf(stderr,
                             "cavlc reader check failed: %s row=%d offset=%zu sym=%d\n",
                             name, row, offset, symbol);
                checks.expect(false, name);
            }
            if (length > 8) saw_long = true;
        }
    }
    return saw_long;
}

void run_cavlc_reader_checks() {
    ReaderChecks checks;

    /* coeff_token: every nC table plus both chroma DC forms. */
    bool long_coeff = false;
    const std::uint32_t nC_values[4] = {0, 2, 4, 9};
    const char* nC_names[4] = {"coeff_token nC<2", "coeff_token 2<=nC<4",
                               "coeff_token 4<=nC<8", "coeff_token nC>=8"};
    for (int table = 0; table < 4; ++table) {
        (void)decode_coeff_family(
                checks, nC_names[table], bs::avc::kCoeffTokenLen[table],
                bs::avc::kCoeffTokenBits[table], 68, nC_values[table], false,
                false, long_coeff);
    }
    (void)decode_coeff_family(checks, "coeff_token chroma DC 4:2:0",
                              bs::avc::kChromaDcCoeffTokenLen,
                              bs::avc::kChromaDcCoeffTokenBits, 20, 0, true,
                              false, long_coeff);
    (void)decode_coeff_family(checks, "coeff_token chroma DC 4:2:2",
                              bs::avc::kChroma422DcCoeffTokenLen,
                              bs::avc::kChroma422DcCoeffTokenBits, 36, 0, false,
                              true, long_coeff);
    checks.expect(long_coeff, "long coeff_token codes exercised");

    /* total_zeros: all 4:2:0 rows and both chroma DC tables. */
    bool long_zeros = false;
    for (int total_coeff = 1; total_coeff <= 15; ++total_coeff) {
        (void)decode_total_zeros_family(
                checks, "total_zeros 4:2:0", bs::avc::kTotalZerosLen[total_coeff - 1],
                bs::avc::kTotalZerosBits[total_coeff - 1], total_coeff - 1,
                static_cast<std::uint32_t>(total_coeff), false, false,
                17 - total_coeff, long_zeros);
    }
    for (int total_coeff = 1; total_coeff <= 3; ++total_coeff) {
        (void)decode_total_zeros_family(
                checks, "total_zeros chroma DC 4:2:0",
                bs::avc::kChromaDcTotalZerosLen[total_coeff - 1],
                bs::avc::kChromaDcTotalZerosBits[total_coeff - 1],
                total_coeff - 1, static_cast<std::uint32_t>(total_coeff), true,
                false, 5 - total_coeff, long_zeros);
    }
    for (int total_coeff = 1; total_coeff <= 7; ++total_coeff) {
        (void)decode_total_zeros_family(
                checks, "total_zeros chroma DC 4:2:2",
                bs::avc::kChroma422DcTotalZerosLen[total_coeff - 1],
                bs::avc::kChroma422DcTotalZerosBits[total_coeff - 1],
                total_coeff - 1, static_cast<std::uint32_t>(total_coeff), false,
                true, 9 - total_coeff, long_zeros);
    }
    checks.expect(long_zeros, "long total_zeros codes exercised");

    /* run_before: one table per zeros_left class, including the >6 table. */
    bool long_run = false;
    for (std::uint32_t zeros_left = 1; zeros_left <= 15; ++zeros_left) {
        const int table = zeros_left > 6 ? 6 : static_cast<int>(zeros_left) - 1;
        const int count = zeros_left > 6 ? 15 : static_cast<int>(zeros_left) + 1;
        const std::size_t offsets[] = {0, 4, 7};
        for (std::size_t offset : offsets) {
            for (int symbol = 0; symbol < count; ++symbol) {
                const int length = bs::avc::kRunLen[table][symbol];
                if (length <= 0) continue;
                BitWriter writer(offset);
                writer.push_code(bs::avc::kRunBits[table][symbol], length);

                bs::avc::CavlcReader reader(writer.span(), offset);
                const std::uint32_t value = reader.decode_run_before(zeros_left);
                const bool ok =
                        value == static_cast<std::uint32_t>(symbol) &&
                        reader.bit_position() ==
                                offset + static_cast<std::size_t>(length);
                if (!ok) {
                    std::fprintf(stderr,
                                 "cavlc reader check failed: run_before z=%u "
                                 "offset=%zu sym=%d\n",
                                 zeros_left, offset, symbol);
                    checks.expect(false, "run_before");
                }
                if (length > 8) long_run = true;
            }
        }
    }
    checks.expect(long_run, "long run_before codes exercised");

    /* run_before(0) consumes nothing. */
    {
        BitWriter writer;
        writer.push_code(1, 1);
        bs::avc::CavlcReader reader(writer.span(), 0);
        checks.expect(reader.decode_run_before(0) == 0, "run_before(0)");
        checks.expect(reader.bit_position() == 0, "run_before(0) consumes no bits");
    }

    /* Every coeff_token 6-bit pattern not in table 3 is invalid. */
    {
        bool present[64] = {};
        for (int symbol = 0; symbol < 68; ++symbol) {
            if (bs::avc::kCoeffTokenLen[3][symbol] == 6) {
                present[bs::avc::kCoeffTokenBits[3][symbol]] = true;
            }
        }
        std::uint32_t invalid = 0;
        bool found = false;
        for (std::uint32_t code = 0; code < 64; ++code) {
            if (!present[code]) {
                invalid = code;
                found = true;
                break;
            }
        }
        checks.expect(found, "invalid coeff_token code exists");
        BitWriter writer;
        writer.push_code(invalid, 6);
        writer.push_code(0, 10);
        bs::avc::CavlcReader reader(writer.span(), 0);
        bool threw = false;
        try {
            (void)reader.decode_coeff_token(9, false, false);
        } catch (const bs::avc::CavlcError&) {
            threw = true;
        }
        checks.expect(threw, "invalid coeff_token rejected");
    }

    /* An invalid 16-bit run_before pattern has no matching code. */
    {
        const int count = 15;
        std::uint32_t invalid = 0;
        bool found = false;
        for (std::uint32_t pattern = 0; pattern < 65536 && !found; ++pattern) {
            bool valid = false;
            for (int symbol = 0; symbol < count; ++symbol) {
                const int length = bs::avc::kRunLen[6][symbol];
                if (length <= 0) continue;
                if ((pattern >> (16 - length)) ==
                    bs::avc::kRunBits[6][symbol]) {
                    valid = true;
                    break;
                }
            }
            if (!valid) {
                invalid = pattern;
                found = true;
            }
        }
        checks.expect(found, "invalid run_before code exists");
        BitWriter writer;
        writer.push_code(invalid, 16);
        bs::avc::CavlcReader reader(writer.span(), 0);
        bool threw = false;
        try {
            (void)reader.decode_run_before(15);
        } catch (const bs::avc::CavlcError&) {
            threw = true;
        }
        checks.expect(threw, "invalid run_before rejected");
    }

    /* A truncated code must throw rather than decode past the RBSP. */
    {
        int long_symbol = -1;
        for (int symbol = 0; symbol < 68; ++symbol) {
            if (bs::avc::kCoeffTokenLen[0][symbol] > 9) {
                long_symbol = symbol;
                break;
            }
        }
        checks.expect(long_symbol >= 0, "coeff_token long code exists");
        if (long_symbol >= 0) {
            const int length = bs::avc::kCoeffTokenLen[0][long_symbol];
            const std::uint32_t code = bs::avc::kCoeffTokenBits[0][long_symbol];
            BitWriter writer;
            writer.push_code(code >> (length - 8), 8);
            bs::avc::CavlcReader reader(writer.span(), 0);
            bool threw = false;
            try {
                (void)reader.decode_coeff_token(0, false, false);
            } catch (const bs::avc::CavlcError&) {
                threw = true;
            }
            checks.expect(threw, "truncated coeff_token rejected");
        }
    }

    /* read_bits/read_bit consume exactly up to the truncation point. */
    {
        const std::uint8_t data[2] = {0xAB, 0xCD};
        bs::avc::CavlcReader reader(
                std::span<const std::uint8_t>(data, sizeof(data)), 0);
        checks.expect(reader.read_bits(8) == 0xABu, "read_bits 8");
        checks.expect(reader.bit_position() == 8, "read_bits position");
        bool threw = false;
        try {
            (void)reader.read_bits(9);
        } catch (const bs::avc::CavlcError&) {
            threw = true;
        }
        checks.expect(threw, "read_bits past end throws");
        checks.expect(reader.bit_position() == 16, "read_bits truncation position");
    }
    {
        const std::uint8_t data[1] = {0x80};
        bs::avc::CavlcReader reader(
                std::span<const std::uint8_t>(data, sizeof(data)), 0);
        for (int i = 0; i < 8; ++i) {
            const bool expected = i == 0;
            checks.expect(reader.read_bit() == expected, "read_bit sequence");
        }
        bool threw = false;
        try {
            (void)reader.read_bit();
        } catch (const bs::avc::CavlcError&) {
            threw = true;
        }
        checks.expect(threw, "read_bit past end throws");
    }

    /* ue(v)/se(v)/te(v) round trips, including the 32-bit code space. */
    {
        const std::uint32_t values[] = {0, 1, 2, 3, 4, 7, 31, 255, 65535,
                                        0x7FFFFFFFu, 0xFFFFFFFEu};
        for (std::uint32_t value : values) {
            BitWriter writer;
            writer.push_ue(value);
            bs::avc::CavlcReader reader(writer.span(), 0);
            const std::uint32_t decoded = reader.read_ue();
            const bool ok = decoded == value &&
                            reader.bit_position() == writer.bits;
            if (!ok) {
                std::fprintf(stderr,
                             "cavlc reader check failed: ue(%u) -> %u\n", value,
                             decoded);
                checks.expect(false, "read_ue round trip");
            }
        }
    }
    {
        for (std::int32_t value = -20; value <= 20; ++value) {
            const std::uint32_t code =
                    value > 0 ? static_cast<std::uint32_t>(2 * value - 1)
                              : static_cast<std::uint32_t>(-2 * value);
            BitWriter writer;
            writer.push_ue(code);
            bs::avc::CavlcReader reader(writer.span(), 0);
            const std::int32_t decoded = reader.read_se();
            if (decoded != value) {
                std::fprintf(stderr,
                             "cavlc reader check failed: se(%d) -> %d\n", value,
                             decoded);
                checks.expect(false, "read_se round trip");
            }
        }
    }
    {
        BitWriter writer;
        writer.push_bit(true);
        writer.push_bit(false);
        bs::avc::CavlcReader reader(writer.span(), 0);
        checks.expect(reader.read_te(0) == 0, "te(0)");
        checks.expect(reader.read_te(1) == 0, "te(1)");
        checks.expect(reader.read_te(2) == 0, "te(2) inverted one");
        checks.expect(reader.read_te(2) == 1, "te(2) inverted zero");

        BitWriter clamped;
        clamped.push_ue(9);
        bs::avc::CavlcReader reader2(clamped.span(), 0);
        checks.expect(reader2.read_te(5) == 4, "te(v) clamp");
    }
    {
        /* ue(v) with 32 consecutive zero bits overflows. */
        std::uint8_t zeros[5] = {};
        bs::avc::CavlcReader reader(
                std::span<const std::uint8_t>(zeros, sizeof(zeros)), 0);
        bool threw = false;
        try {
            (void)reader.read_ue();
        } catch (const bs::avc::CavlcError&) {
            threw = true;
        }
        checks.expect(threw, "ue(v) overflow rejected");
    }

    /* Position, alignment and the cached more_rbsp_data() semantics. */
    {
        const std::uint8_t data[3] = {0xF0, 0x00, 0x01};
        bs::avc::CavlcReader reader(
                std::span<const std::uint8_t>(data, sizeof(data)), 3);
        checks.expect(reader.bit_position() == 3, "initial bit position");
        checks.expect(!reader.byte_aligned(), "initial unaligned");
        checks.expect(reader.read_bits(2) == 2u, "read after unaligned start");
        checks.expect(reader.bit_position() == 5, "bit position advances");
        reader.byte_align();
        checks.expect(reader.bit_position() == 8, "byte_align rounds up");
        checks.expect(reader.byte_aligned(), "byte_aligned after align");
        checks.expect(reader.read_bits(16) == 0x0001u, "read after align");
        checks.expect(reader.bit_position() == 24, "position at end");
        checks.expect(!reader.more_rbsp_data(), "no more data at end");
    }
    {
        /*
         * The stop bit is the rightmost set bit.  Positions before it have
         * more data; the stop bit itself does not; a position after it is
         * treated as "more" because the original backward scan found no set
         * bit at or after the position.
         */
        const std::uint8_t data[2] = {0x00, 0x80};
        for (std::size_t offset = 0; offset <= 16; ++offset) {
            bs::avc::CavlcReader reader(
                    std::span<const std::uint8_t>(data, sizeof(data)), offset);
            const bool expected = offset != 8 && offset < 16;
            if (reader.more_rbsp_data() != expected) {
                std::fprintf(stderr,
                             "cavlc reader check failed: more_rbsp_data off=%zu\n",
                             offset);
                checks.expect(false, "more_rbsp_data");
            }
        }
    }
    {
        const std::uint8_t data[1] = {0x00};
        bool threw = false;
        try {
            bs::avc::CavlcReader reader(
                    std::span<const std::uint8_t>(data, sizeof(data)), 9);
            (void)reader;
        } catch (const bs::avc::CavlcError&) {
            threw = true;
        }
        checks.expect(threw, "start offset outside slice data rejected");
    }

    /* End-to-end residual blocks across coeff_token/total_zeros paths. */
    {
        /*
         * Table 3 coeff_token for TotalCoeff=1, TrailingOnes=0 is symbol 4.
         * One level (+2 with the implicit first-level offset) is the bit
         * "1"; total_zeros for TotalCoeff=1 and value 0 is also "1".
         */
        for (std::size_t offset = 0; offset < 8; ++offset) {
            BitWriter writer(offset);
            writer.push_code(bs::avc::kCoeffTokenBits[3][4],
                             bs::avc::kCoeffTokenLen[3][4]);
            writer.push_bit(true);
            writer.push_bit(true);

            bs::avc::CavlcReader reader(writer.span(), offset);
            const std::uint32_t total =
                    reader.decode_residual_block(9, 16, false, false);
            checks.expect(total == 1, "residual block TotalCoeff=1");
            checks.expect(reader.bit_position() == writer.bits,
                          "residual block consumes its syntax");
        }
    }

    if (checks.failures() != 0) {
        std::fprintf(stderr, "%d CAVLC reader check(s) failed\n",
                     checks.failures());
        std::exit(1);
    }
}

}  // namespace

int main(int argc, char** argv) {
    run_cavlc_reader_checks();
    if (argc < 2) {
        std::fprintf(stderr,
                     "usage: avc_cavlc_test <stream.h264> [motion.mv.txt] [pictures]\n");
        return 1;
    }

    std::ifstream in(argv[1], std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "cannot open %s\n", argv[1]);
        return 1;
    }
    const std::vector<std::uint8_t> bytes{
            std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};

    bs::utils::MotionBufferPool pool({2, 16, 8u << 20, 64});
    Context context{&pool, 0, 0, {}};

    auto state = bs::create_state(bs::Codec::Avc);
    bs::AvcParsedHandlers handlers{};
    handlers.motion_vectors.ctx = &context;
    handlers.motion_vectors.acquire = &acquire;
    handlers.motion_vectors.ready = &ready;
    handlers.motion_vectors.options.emit_intra = true;
    (void)bs::parse(*state,
                    std::span<const std::uint8_t>(bytes.data(), bytes.size()),
                    bs::NalFramingMode::AnnexB,
                    handlers,
                    4);

    if (context.pictures == 0 || context.ok != context.pictures) {
        std::fprintf(stderr, "CAVLC stream produced %d/%d decoded pictures\n",
                     context.ok, context.pictures);
        return 1;
    }

    if (argc >= 4) {
        const int expected = std::atoi(argv[3]);
        if (expected > 0 && context.pictures != expected) {
            std::fprintf(stderr, "CAVLC stream delivered %d/%d pictures\n",
                         context.pictures, expected);
            return 1;
        }
    }

    if (argc >= 3) {
        const auto reference = read_mv_reference(argv[2]);
        if (context.p_fields.size() < reference.size()) {
            std::fprintf(stderr, "missing P pictures: %zu/%zu\n",
                         context.p_fields.size(), reference.size());
            return 1;
        }
        for (std::size_t i = 0; i < reference.size(); ++i) {
            const Field& actual = context.p_fields[i];
            const Field& expected = reference[i];
            if (actual.width4x4 != expected.width4x4 ||
                actual.height4x4 != expected.height4x4 ||
                actual.cells.size() != expected.cells.size()) {
                std::fprintf(stderr, "P-picture %zu geometry mismatch\n", i);
                return 1;
            }
            for (std::size_t c = 0; c < expected.cells.size(); ++c) {
                if (actual.cells[c].mv[0][0] != expected.cells[c].mv[0][0] ||
                    actual.cells[c].mv[0][1] != expected.cells[c].mv[0][1]) {
                    std::fprintf(stderr, "P-picture %zu cell %zu motion mismatch\n",
                                 i, c);
                    return 1;
                }
            }
        }
    }

    std::puts("avc cavlc: ok");
    return 0;
}
