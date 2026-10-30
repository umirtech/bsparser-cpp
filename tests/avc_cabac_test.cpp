// SPDX-FileCopyrightText: 2026 Mohd Umeer
// SPDX-License-Identifier: BSD-2-Clause

/*
 * Focused CABAC tests for the reservoir / renormalisation rewrite.
 *
 * The first half drives the production decoder against a test-only
 * bit-at-a-time Annex 9 reference decoder over deterministic pseudo-random
 * RBSPs and op sequences.  That covers non-byte-aligned start offsets,
 * long bypass runs, terminate bins, skip_bits/restart after I_PCM,
 * ue(v) overflow and truncated streams: for every operation the bin value,
 * context state, bit_position() and CabacError outcome must match.
 *
 * The second half parses the 4:2:0, 4:2:2 and 4:4:4 CABAC corpus fixtures
 * (including the I_PCM and 10-bit ones) and checks an FNV-1a hash of the
 * emitted motion cells, so the slice-data traversal is exercised end to end.
 */

#include "bsparser.hpp"
#include "parser/avc_cabac.hpp"
#include "utils/MotionBufferPool.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(bool condition, const std::string& what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
        ++g_failures;
    }
}

/*
 * Test-only reference: the original bit-at-a-time Annex 9 decoder, kept
 * verbatim so the optimised engine can be compared operation by operation.
 */
class ReferenceDecoder {
   public:
    ReferenceDecoder(std::span<const std::uint8_t> rbsp, std::size_t bit_offset)
        : rbsp_(rbsp), bit_pos_(bit_offset) {
        if (bit_offset > rbsp.size() * 8) {
            throw bs::avc::CabacError("CABAC: start offset outside slice data");
        }
        range_ = 510;
        offset_ = read_bits(9);
    }

    void init_contexts(int slice_qp, int cabac_init_idc, bool i_slice) noexcept {
        const int model = i_slice ? 0 : (cabac_init_idc + 1);
        const int qp = slice_qp < 0 ? 0 : (slice_qp > 87 ? 87 : slice_qp);
        for (int i = 0; i < bs::avc::kCabacContextCount; ++i) {
            const int m = bs::avc::kCabacContextInit[i][model][0];
            const int n = bs::avc::kCabacContextInit[i][model][1];
            int pre = ((m * qp) >> 4) + n;
            if (pre < 1) {
                pre = 1;
            } else if (pre > 126) {
                pre = 126;
            }
            if (pre <= 63) {
                contexts_[i].state = static_cast<std::uint8_t>(63 - pre);
                contexts_[i].mps = 0;
            } else {
                contexts_[i].state = static_cast<std::uint8_t>(pre - 64);
                contexts_[i].mps = 1;
            }
        }
    }

    [[nodiscard]] bs::avc::CabacContext& ctx(int index) noexcept {
        return contexts_[index < 0 ? 0 : (index >= bs::avc::kCabacContextCount
                                                  ? bs::avc::kCabacContextCount - 1
                                                  : index)];
    }

    [[nodiscard]] const bs::avc::CabacContext& ctx(int index) const noexcept {
        return contexts_[index < 0 ? 0 : (index >= bs::avc::kCabacContextCount
                                                  ? bs::avc::kCabacContextCount - 1
                                                  : index)];
    }

    [[nodiscard]] bool decode(bs::avc::CabacContext& context) {
        const auto state = static_cast<std::size_t>(context.state & 63u);
        const std::uint32_t q = (range_ >> 6) & 3u;
        const std::uint32_t lps = bs::avc::kCabacRangeLps[state][q];

        range_ -= lps;

        bool bin = false;
        if (offset_ >= range_) {
            offset_ -= range_;
            bin = context.mps == 0;
            if (context.state == 0) {
                context.mps ^= 1;
            }
            context.state = bs::avc::kCabacStateTrans[state][0];
            range_ = lps;
        } else {
            bin = context.mps != 0;
            context.state = bs::avc::kCabacStateTrans[state][1];
        }

        while (range_ < 256u) {
            range_ <<= 1;
            offset_ = (offset_ << 1) | (read_bit() ? 1u : 0u);
        }

        return bin;
    }

    [[nodiscard]] bool decode_bypass() {
        offset_ = (offset_ << 1) | (read_bit() ? 1u : 0u);
        if (offset_ >= range_) {
            offset_ -= range_;
            return true;
        }
        return false;
    }

    [[nodiscard]] bool decode_terminate() {
        range_ -= 2;
        if (offset_ >= range_) {
            return true;
        }
        if (range_ < 256u) {
            range_ <<= 1;
            offset_ = (offset_ << 1) | (read_bit() ? 1u : 0u);
        }
        return false;
    }

    [[nodiscard]] std::uint32_t decode_bypass_bits(int count) {
        std::uint32_t value = 0;
        for (int i = 0; i < count; ++i) {
            value = (value << 1) | (decode_bypass() ? 1u : 0u);
        }
        return value;
    }

    [[nodiscard]] std::uint32_t decode_ue(bs::avc::CabacContext* contexts, int context_count) {
        int leading_zeros = 0;
        while (true) {
            bs::avc::CabacContext& context =
                    contexts[leading_zeros < context_count ? leading_zeros : context_count - 1];
            if (decode(context)) {
                break;
            }
            ++leading_zeros;
            if (leading_zeros > 31) {
                throw bs::avc::CabacError("CABAC: ue(v) overflow");
            }
        }
        if (leading_zeros == 0) {
            return 0;
        }
        const std::uint32_t suffix = decode_bypass_bits(leading_zeros);
        return ((std::uint32_t{1} << leading_zeros) - 1u) + suffix;
    }

    [[nodiscard]] std::size_t bit_position() const noexcept { return bit_pos_; }

    void byte_align() noexcept { bit_pos_ = (bit_pos_ + 7u) & ~std::size_t{7u}; }

    void skip_bits(std::uint64_t count) {
        const std::uint64_t total = static_cast<std::uint64_t>(rbsp_.size()) * 8u;
        if (bit_pos_ > total || count > total - static_cast<std::uint64_t>(bit_pos_)) {
            throw bs::avc::CabacError("CABAC: truncated bit skip");
        }
        bit_pos_ += static_cast<std::size_t>(count);
    }

    void restart() {
        range_ = 510;
        offset_ = read_bits(9);
    }

   private:
    [[nodiscard]] bool read_bit() {
        if (bit_pos_ >= rbsp_.size() * 8) {
            throw bs::avc::CabacError("CABAC: truncated bitstream");
        }
        const std::uint8_t byte = rbsp_[bit_pos_ >> 3];
        const bool value = ((byte >> (7u - (bit_pos_ & 7u))) & 1u) != 0;
        ++bit_pos_;
        return value;
    }

    [[nodiscard]] std::uint32_t read_bits(int count) {
        std::uint32_t value = 0;
        for (int i = 0; i < count; ++i) {
            value = (value << 1) | (read_bit() ? 1u : 0u);
        }
        return value;
    }

    std::span<const std::uint8_t> rbsp_;
    std::size_t bit_pos_ = 0;
    std::uint32_t range_ = 510;
    std::uint32_t offset_ = 0;
    bs::avc::CabacContext contexts_[bs::avc::kCabacContextCount]{};
};

struct Rng {
    std::uint64_t state;

    std::uint64_t next() {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state;
    }

    std::uint32_t below(std::uint32_t bound) {
        return bound == 0 ? 0 : static_cast<std::uint32_t>(next() % bound);
    }
};

bool same_context(const bs::avc::CabacContext& a, const bs::avc::CabacContext& b) {
    return a.state == b.state && a.mps == b.mps;
}

bool same_contexts(const bs::avc::CabacDecoder& a,
                   const ReferenceDecoder& b,
                   int count) {
    for (int i = 0; i < count; ++i) {
        if (!same_context(a.ctx(i), b.ctx(i))) {
            return false;
        }
    }
    return true;
}

enum class OpKind : int {
    Decode = 0,
    Bypass,
    Terminate,
    BypassBits,
    Skip,
    Align,
    Restart,
    Ue,
};

/*
 * Run one random op on both decoders and compare every observable effect.
 * Returns false once a truncated stream ended the case.
 */
bool compare_op(bs::avc::CabacDecoder& prod,
                ReferenceDecoder& ref,
                Rng& rng,
                const std::string& label) {
    const auto kind = static_cast<OpKind>(rng.below(8));
    bool prod_threw = false;
    bool ref_threw = false;
    bool prod_bin = false;
    bool ref_bin = false;
    std::uint32_t prod_value = 0;
    std::uint32_t ref_value = 0;

    switch (kind) {
        case OpKind::Decode: {
            const int index = static_cast<int>(rng.below(1040)) - 8;
            try {
                prod_bin = prod.decode(prod.ctx(index));
            } catch (const bs::avc::CabacError&) {
                prod_threw = true;
            }
            try {
                ref_bin = ref.decode(ref.ctx(index));
            } catch (const bs::avc::CabacError&) {
                ref_threw = true;
            }
            if (!prod_threw && !ref_threw) {
                check(prod_bin == ref_bin, label + ": decode bin");
                check(same_context(prod.ctx(index), ref.ctx(index)),
                      label + ": decode context");
            }
            break;
        }
        case OpKind::Bypass:
            try {
                prod_bin = prod.decode_bypass();
            } catch (const bs::avc::CabacError&) {
                prod_threw = true;
            }
            try {
                ref_bin = ref.decode_bypass();
            } catch (const bs::avc::CabacError&) {
                ref_threw = true;
            }
            if (!prod_threw && !ref_threw) {
                check(prod_bin == ref_bin, label + ": bypass bin");
            }
            break;
        case OpKind::Terminate: {
            try {
                prod_bin = prod.decode_terminate();
            } catch (const bs::avc::CabacError&) {
                prod_threw = true;
            }
            try {
                ref_bin = ref.decode_terminate();
            } catch (const bs::avc::CabacError&) {
                ref_threw = true;
            }
            if (!prod_threw && !ref_threw) {
                check(prod_bin == ref_bin, label + ": terminate bin");
            }
            break;
        }
        case OpKind::BypassBits: {
            const int count = static_cast<int>(rng.below(41));
            try {
                prod_value = prod.decode_bypass_bits(count);
            } catch (const bs::avc::CabacError&) {
                prod_threw = true;
            }
            try {
                ref_value = ref.decode_bypass_bits(count);
            } catch (const bs::avc::CabacError&) {
                ref_threw = true;
            }
            if (!prod_threw && !ref_threw) {
                check(prod_value == ref_value, label + ": bypass bits");
            }
            break;
        }
        case OpKind::Skip: {
            const auto count = rng.below(80);
            try {
                prod.skip_bits(count);
            } catch (const bs::avc::CabacError&) {
                prod_threw = true;
            }
            try {
                ref.skip_bits(count);
            } catch (const bs::avc::CabacError&) {
                ref_threw = true;
            }
            break;
        }
        case OpKind::Align:
            prod.byte_align();
            ref.byte_align();
            break;
        case OpKind::Restart:
            try {
                prod.restart();
            } catch (const bs::avc::CabacError&) {
                prod_threw = true;
            }
            try {
                ref.restart();
            } catch (const bs::avc::CabacError&) {
                ref_threw = true;
            }
            break;
        case OpKind::Ue: {
            const int base = static_cast<int>(rng.below(64));
            const int count = 1 + static_cast<int>(rng.below(8));
            auto* prod_ctx = &prod.ctx(base);
            auto* ref_ctx = &ref.ctx(base);
            try {
                prod_value = prod.decode_ue(prod_ctx, count);
            } catch (const bs::avc::CabacError&) {
                prod_threw = true;
            }
            try {
                ref_value = ref.decode_ue(ref_ctx, count);
            } catch (const bs::avc::CabacError&) {
                ref_threw = true;
            }
            if (!prod_threw && !ref_threw) {
                check(prod_value == ref_value, label + ": ue(v) value");
            }
            for (int i = 0; i < count; ++i) {
                check(same_context(prod_ctx[i], ref_ctx[i]), label + ": ue(v) context");
            }
            break;
        }
    }

    check(prod_threw == ref_threw, label + ": exception outcome");
    check(prod.bit_position() == ref.bit_position(), label + ": bit_position");
    return !(prod_threw || ref_threw);
}

void differential_random() {
    Rng rng{0x123456789abcdef0ull};
    std::uint32_t constructed = 0;
    for (int test = 0; test < 240; ++test) {
        const std::size_t size = 1 + static_cast<std::size_t>(rng.below(256));
        std::vector<std::uint8_t> rbsp(size);
        for (auto& byte : rbsp) {
            byte = static_cast<std::uint8_t>(rng.next());
        }
        // Sprinkle literal runs so the reservoir sees aligned and
        // non-aligned long runs, not just uniform noise.
        if ((test % 5) == 0) {
            for (std::size_t i = 0; i < size; i += 7) {
                rbsp[i] = (test % 10 == 0) ? 0xFFu : 0x00u;
            }
        }
        const std::size_t total_bits = size * 8u;
        const std::size_t offset = static_cast<std::size_t>(rng.below(
                static_cast<std::uint32_t>(total_bits + 1u)));
        const std::string label = "random[" + std::to_string(test) + "]";

        const std::span<const std::uint8_t> view(rbsp.data(), rbsp.size());
        std::optional<bs::avc::CabacDecoder> dec;
        std::optional<ReferenceDecoder> ref;
        bool prod_ctor = true;
        bool ref_ctor = true;
        try {
            dec.emplace(view, offset);
        } catch (const bs::avc::CabacError&) {
            prod_ctor = false;
        }
        try {
            ref.emplace(view, offset);
        } catch (const bs::avc::CabacError&) {
            ref_ctor = false;
        }
        check(prod_ctor == ref_ctor, label + ": constructor outcome");
        if (!prod_ctor || !ref_ctor) {
            continue;
        }
        ++constructed;

        const int qp = static_cast<int>(rng.below(88));
        const int idc = static_cast<int>(rng.below(3));
        const bool i_slice = (rng.next() & 1u) != 0;
        dec->init_contexts(qp, idc, i_slice);
        ref->init_contexts(qp, idc, i_slice);

        for (int op = 0; op < 320; ++op) {
            if (!compare_op(*dec, *ref, rng, label)) {
                break;
            }
        }
        check(same_contexts(*dec, *ref, bs::avc::kCabacContextCount),
              label + ": final context state");
    }
    check(constructed > 100, "random: enough successful constructions");
}

void non_byte_aligned_starts() {
    Rng rng{0x0f1e2d3c4b5a6978ull};
    std::vector<std::uint8_t> rbsp(512);
    for (auto& byte : rbsp) {
        byte = static_cast<std::uint8_t>(rng.next());
    }
    for (std::size_t offset = 0; offset < 8; ++offset) {
        const std::string label = "unaligned[" + std::to_string(offset) + "]";
        bs::avc::CabacDecoder dec(std::span<const std::uint8_t>(rbsp.data(), rbsp.size()),
                                  offset);
        ReferenceDecoder ref(std::span<const std::uint8_t>(rbsp.data(), rbsp.size()), offset);
        dec.init_contexts(26, 1, false);
        ref.init_contexts(26, 1, false);
        for (int op = 0; op < 400; ++op) {
            if (!compare_op(dec, ref, rng, label)) {
                break;
            }
        }
        check(same_contexts(dec, ref, bs::avc::kCabacContextCount),
              label + ": final context state");
    }
}

void long_bypass_runs() {
    Rng rng{0xdeadbeefcafef00dull};
    std::vector<std::uint8_t> rbsp(4096);
    for (auto& byte : rbsp) {
        byte = static_cast<std::uint8_t>(rng.next());
    }
    bs::avc::CabacDecoder dec(std::span<const std::uint8_t>(rbsp.data(), rbsp.size()), 3);
    ReferenceDecoder ref(std::span<const std::uint8_t>(rbsp.data(), rbsp.size()), 3);
    dec.init_contexts(40, 2, false);
    ref.init_contexts(40, 2, false);

    for (int round = 0; round < 64; ++round) {
        const std::size_t before = dec.bit_position();
        const int count = 1 + static_cast<int>(rng.below(40));
        const std::uint32_t a = dec.decode_bypass_bits(count);
        const std::uint32_t b = ref.decode_bypass_bits(count);
        check(a == b, "bypass run: value");
        check(dec.bit_position() == before + static_cast<std::size_t>(count),
              "bypass run: consumes exactly count bits");
        check(dec.bit_position() == ref.bit_position(), "bypass run: position");
    }
    for (int i = 0; i < 700; ++i) {
        check(dec.decode_bypass() == ref.decode_bypass(), "bypass run: single bin");
    }
    check(dec.bit_position() == ref.bit_position(), "bypass run: final position");
}

void terminate_bins() {
    Rng rng{0xa5a5a5a5f0f0f0f0ull};
    std::vector<std::uint8_t> rbsp(1024);
    for (auto& byte : rbsp) {
        byte = static_cast<std::uint8_t>(rng.next());
    }
    bs::avc::CabacDecoder dec(std::span<const std::uint8_t>(rbsp.data(), rbsp.size()), 0);
    ReferenceDecoder ref(std::span<const std::uint8_t>(rbsp.data(), rbsp.size()), 0);
    dec.init_contexts(30, 0, true);
    ref.init_contexts(30, 0, true);
    bool terminated = false;
    for (int i = 0; i < 4000; ++i) {
        const std::size_t before = dec.bit_position();
        const bool a = dec.decode_terminate();
        const bool b = ref.decode_terminate();
        check(a == b, "terminate: bin");
        check(dec.bit_position() == ref.bit_position(), "terminate: position");
        if (a && dec.bit_position() != before) {
            check(false, "terminate: a true bin must not read bits");
        }
        if (a) {
            terminated = true;
            break;
        }
        // Keep range renormalised for the next terminate bin.
        (void)dec.decode_bypass();
        (void)ref.decode_bypass();
    }
    check(terminated || dec.bit_position() == ref.bit_position(),
          "terminate: stopped in lockstep");
}

void skip_and_restart_after_pcm() {
    Rng rng{0x13579bdf2468ace0ull};
    std::vector<std::uint8_t> rbsp(2048);
    for (auto& byte : rbsp) {
        byte = static_cast<std::uint8_t>(rng.next());
    }
    bs::avc::CabacDecoder dec(std::span<const std::uint8_t>(rbsp.data(), rbsp.size()), 5);
    ReferenceDecoder ref(std::span<const std::uint8_t>(rbsp.data(), rbsp.size()), 5);
    dec.init_contexts(26, 0, false);
    ref.init_contexts(26, 0, false);

    for (int round = 0; round < 32; ++round) {
        for (int i = 0; i < 40; ++i) {
            check(dec.decode(dec.ctx(i)) == ref.decode(ref.ctx(i)), "pcm: pre-skip bin");
        }
        dec.byte_align();
        ref.byte_align();
        check(dec.bit_position() == ref.bit_position(), "pcm: aligned position");
        check((dec.bit_position() & 7u) == 0, "pcm: byte aligned");

        const auto skip = static_cast<std::uint64_t>(rng.below(3000));
        bool prod_threw = false;
        bool ref_threw = false;
        try {
            dec.skip_bits(skip);
        } catch (const bs::avc::CabacError&) {
            prod_threw = true;
        }
        try {
            ref.skip_bits(skip);
        } catch (const bs::avc::CabacError&) {
            ref_threw = true;
        }
        check(prod_threw == ref_threw, "pcm: skip outcome");
        check(dec.bit_position() == ref.bit_position(), "pcm: skip position");
        if (prod_threw) {
            break;
        }

        prod_threw = false;
        ref_threw = false;
        try {
            dec.restart();
        } catch (const bs::avc::CabacError&) {
            prod_threw = true;
        }
        try {
            ref.restart();
        } catch (const bs::avc::CabacError&) {
            ref_threw = true;
        }
        check(prod_threw == ref_threw, "pcm: restart outcome");
        check(dec.bit_position() == ref.bit_position(), "pcm: restart position");
        if (prod_threw) {
            break;
        }
        check(same_contexts(dec, ref, 460), "pcm: contexts survive restart");
    }
}

void truncated_streams() {
    // Truncate a CABAC payload at every length: both decoders must fail
    // (or survive) at the same operation and with the same bit position.
    const std::vector<std::uint8_t> rbsp(64, 0xFFu);
    for (std::size_t bytes = 0; bytes <= 40; ++bytes) {
        const std::span<const std::uint8_t> view(rbsp.data(), bytes);
        const std::string label = "truncated[" + std::to_string(bytes) + "]";
        std::optional<bs::avc::CabacDecoder> dec;
        std::optional<ReferenceDecoder> ref;
        bool prod_ctor = true;
        bool ref_ctor = true;
        try {
            dec.emplace(view, 0);
        } catch (const bs::avc::CabacError&) {
            prod_ctor = false;
        }
        try {
            ref.emplace(view, 0);
        } catch (const bs::avc::CabacError&) {
            ref_ctor = false;
        }
        check(prod_ctor == ref_ctor, label + ": constructor outcome");
        if (!prod_ctor || !ref_ctor) {
            continue;
        }
        dec->init_contexts(26, 0, false);
        ref->init_contexts(26, 0, false);

        for (int i = 0; i < 200; ++i) {
            bool prod_threw = false;
            bool ref_threw = false;
            bool prod_bin = false;
            bool ref_bin = false;
            try {
                prod_bin = dec->decode(dec->ctx(i % 460));
            } catch (const bs::avc::CabacError&) {
                prod_threw = true;
            }
            try {
                ref_bin = ref->decode(ref->ctx(i % 460));
            } catch (const bs::avc::CabacError&) {
                ref_threw = true;
            }
            check(prod_threw == ref_threw, label + ": throw outcome");
            check(dec->bit_position() == ref->bit_position(), label + ": position");
            if (prod_threw || ref_threw) {
                break;
            }
            check(prod_bin == ref_bin, label + ": bin");
        }
    }

    // All-zero payload with a huge ue(v) leading-zero run: both decoders
    // must raise the overflow error at the same bit position.
    const std::vector<std::uint8_t> zeros(32, 0x00u);
    bs::avc::CabacDecoder dec(std::span<const std::uint8_t>(zeros.data(), zeros.size()), 0);
    ReferenceDecoder ref(std::span<const std::uint8_t>(zeros.data(), zeros.size()), 0);
    dec.init_contexts(26, 0, false);
    ref.init_contexts(26, 0, false);
    bool prod_threw = false;
    bool ref_threw = false;
    auto* prod_context = &dec.ctx(0);
    auto* ref_context = &ref.ctx(0);
    try {
        (void)dec.decode_ue(prod_context, 1);
    } catch (const bs::avc::CabacError&) {
        prod_threw = true;
    }
    try {
        (void)ref.decode_ue(ref_context, 1);
    } catch (const bs::avc::CabacError&) {
        ref_threw = true;
    }
    check(prod_threw && ref_threw, "ue(v): all-zero overflow throws");
    check(dec.bit_position() == ref.bit_position(), "ue(v): overflow position");
}

void ctx_clamping() {
    std::vector<std::uint8_t> rbsp(64, 0x5Au);
    bs::avc::CabacDecoder dec(std::span<const std::uint8_t>(rbsp.data(), rbsp.size()), 0);
    dec.init_contexts(26, 0, false);
    check(&dec.ctx(-1) == &dec.ctx(0), "ctx: negative index clamps to 0");
    check(&dec.ctx(bs::avc::kCabacContextCount) ==
                  &dec.ctx(bs::avc::kCabacContextCount - 1),
          "ctx: high index clamps to last");
    check(&dec.ctx(-1000000) == &dec.ctx(0), "ctx: far negative index clamps");
    check(&dec.ctx(1000000) == &dec.ctx(bs::avc::kCabacContextCount - 1),
          "ctx: far high index clamps");
}

// ---------------------------------------------------------------------------
// Fixture hashes
// ---------------------------------------------------------------------------

constexpr std::size_t kFixtureCount = 8;

struct Fixture {
    const char* name;
    std::uint64_t hash;
};

constexpr Fixture kFixtures[kFixtureCount] = {
        {"avc_static_cabac.h264", 0x8494abe467181258ull},
        {"avc_static_422_cabac.h264", 0x05972d5b98033ad3ull},
        {"avc_static_444_cabac.h264", 0x05972d5b98033ad3ull},
        {"avc_residual_422_cabac.h264", 0x06343a872c611965ull},
        {"avc_residual_444_cabac.h264", 0xf2585a2f70446f2dull},
        {"avc_residual_444_10bit_cabac.h264", 0xcdd09b6ff09432dbull},
        {"avc_pcm_cabac_420.h264", 0xa642d7bb923393c8ull},
        {"avc_pcm_cabac_444.h264", 0xbe526cdae0448640ull},
};

struct FixtureContext {
    bs::utils::MotionBufferPool* pool = nullptr;
    std::uint64_t hash = 1469598103934665603ull;
    std::size_t pictures = 0;
    std::size_t ok_pictures = 0;
    std::size_t cells = 0;
    std::size_t other_pictures = 0;
};

void mix_bytes(std::uint64_t& hash, const void* data, std::size_t bytes) {
    const auto* p = static_cast<const std::uint8_t*>(data);
    for (std::size_t i = 0; i < bytes; ++i) {
        hash ^= p[i];
        hash *= 1099511628211ull;
    }
}

template <typename T>
void mix_value(std::uint64_t& hash, const T& value) {
    mix_bytes(hash, &value, sizeof(value));
}

bs::avc::MotionVectorBuffer acquire_fixture(
        void* opaque,
        const bs::avc::MotionVectorBufferRequest& request) {
    return static_cast<FixtureContext*>(opaque)->pool->acquire(request);
}

void ready_fixture(void* opaque, const bs::avc::MotionVectorBufferView& view) {
    auto* ctx = static_cast<FixtureContext*>(opaque);
    ++ctx->pictures;
    if (view.header == nullptr) {
        ++ctx->other_pictures;
        return;
    }
    const auto& header = *view.header;
    if (header.status != static_cast<std::uint32_t>(bs::avc::MotionVectorStatus::Ok)) {
        ++ctx->other_pictures;
        return;
    }
    ++ctx->ok_pictures;
    mix_value(ctx->hash, header.poc);
    mix_value(ctx->hash, header.frame_num);
    mix_value(ctx->hash, header.flags);
    mix_value(ctx->hash, header.width4x4);
    mix_value(ctx->hash, header.height4x4);
    mix_value(ctx->hash, header.status);
    mix_value(ctx->hash, header.colour_plane_id);
    mix_value(ctx->hash, header.plane_count);
    mix_value(ctx->hash, header.ref_poc);
    mix_value(ctx->hash, header.num_ref_idx);
    mix_value(ctx->hash, header.ref_non_existing_mask);
    const auto cells = bs::avc::motionCells(view);
    ctx->cells += cells.size();
    mix_bytes(ctx->hash, cells.data(), cells.size() * sizeof(bs::avc::MvCell));
}

std::vector<std::uint8_t> read_file(const char* path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "cannot open %s\n", path);
        std::exit(1);
    }
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in),
                                     std::istreambuf_iterator<char>());
}

void run_fixture(const char* path) {
    const auto bytes = read_file(path);
    bs::utils::MotionBufferPool pool({8, 64, 512u << 20, 64});
    FixtureContext ctx{&pool};
    auto state = bs::create_state(bs::Codec::Avc);
    bs::AvcParsedHandlers handlers{};
    handlers.motion_vectors.ctx = &ctx;
    handlers.motion_vectors.acquire = &acquire_fixture;
    handlers.motion_vectors.ready = &ready_fixture;
    handlers.motion_vectors.options.emit_intra = true;

    bool parsed = false;
    try {
        (void)bs::parse(*state,
                        std::span<const std::uint8_t>(bytes.data(), bytes.size()),
                        bs::NalFramingMode::AnnexB,
                        handlers,
                        4);
        parsed = true;
    } catch (const std::exception&) {
        parsed = false;
    }
    const std::string label = std::string("fixture: ") + path;
    check(parsed, label + ": parse completes");
    check(ctx.pictures > 0, label + ": pictures emitted");
    check(ctx.other_pictures == 0, label + ": every picture is Ok");
    check(ctx.cells > 0, label + ": cells emitted");

    std::uint64_t expected = 0;
    for (const auto& fixture : kFixtures) {
        if (label.find(fixture.name) != std::string::npos) {
            expected = fixture.hash;
            break;
        }
    }
    check(expected != 0, label + ": known fixture");
    check(ctx.hash == expected, label + ": motion-cell hash");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != static_cast<int>(kFixtureCount) + 1) {
        std::fprintf(stderr, "usage: avc_cabac_test <cabac-fixture> x%zu\n", kFixtureCount);
        return 1;
    }

    differential_random();
    non_byte_aligned_starts();
    long_bypass_runs();
    terminate_bins();
    skip_and_restart_after_pcm();
    truncated_streams();
    ctx_clamping();

    for (int i = 1; i < argc; ++i) {
        run_fixture(argv[i]);
    }

    if (g_failures != 0) {
        std::fprintf(stderr, "%d CABAC failure(s)\n", g_failures);
        return 1;
    }
    std::puts("avc CABAC reservoir/reference/fixtures: ok");
    return 0;
}
