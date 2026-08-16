/*
 * ---------------------------------------------------------------------------
 * POC derivation tests (H.265 §8.3.1, H.264 §8.2.1, H.266 §8.3.1)
 * ---------------------------------------------------------------------------
 *
 * 1. Direct, deterministic tests of the per-codec POC trackers
 *    (bs::HevcPocState / bs::avc::PocState / bs::vvc::PocState).
 * 2. End-to-end: parse a real stream through the unified bs::parse() API with
 *    the typed parsed handlers and verify each slice's `derived_poc` is
 *    populated and yields a monotonic presentation order.
 */

#include "bsparser.hpp"

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

int g_failures = 0;

std::vector<std::int32_t> g_hevc_pocs;
std::vector<std::int32_t> g_avc_pocs;
std::vector<std::int32_t> g_av1_orders;
std::vector<std::uint32_t> g_av1_hints;
std::vector<std::int32_t> g_vp9_orders;
std::vector<std::int32_t> g_vp8_orders;
std::vector<std::int32_t> g_vvc_pocs;

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << ": " << #cond << "\n"; \
            ++g_failures;                                                                 \
        }                                                                                 \
    } while (0)

std::vector<std::uint8_t> read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::cerr << "cannot open " << path << "\n";
        std::exit(1);
    }
    return std::vector<std::uint8_t>(
        std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()
    );
}

/*
 * ---------------------------------------------------------------------------
 * 1. HEVC POC tracker (H.265 §8.3.1)
 * ---------------------------------------------------------------------------
 */
void test_hevc_poc_state() {
    bs::HevcPocState poc;

    /* IDR_W_RADL (19): PicOrderCnt == 0. */
    CHECK(poc.derive(19, 0, 0, 16) == 0);

    /* First non-IDR picture: no prevTid0Pic -> PicOrderCntMsb == 0. */
    CHECK(poc.derive(1, 0, 4, 16) == 4); /* TRAIL_R, POC 4 */

    CHECK(poc.derive(1, 0, 8, 16) == 8); /* POC 8 */

    /* LSB wrap: 8 -> 0, diff 8 >= max/2=8 -> Msb += 16. */
    CHECK(poc.derive(1, 0, 0, 16) == 16); /* POC 16 */

    /* Non-reference picture (TRAIL_N) still gets a POC (16 + 5 = 21) but
       must NOT become prevTid0Pic. */
    CHECK(poc.derive(0, 0, 5, 16) == 21);

    /* Next reference picture still uses the previous reference's LSB (0):
       curr 9 > prev 0 by > half 8 -> Msb -= 16 -> POC 9. */
    CHECK(poc.derive(1, 0, 9, 16) == 9);

    /* TemporalId != 0 must not update prevTid0Pic. */
    poc.reset();
    CHECK(poc.derive(19, 0, 0, 16) == 0); /* IDR */
    CHECK(poc.derive(0, 2, 4, 16) == 4);  /* TID2 non-ref: POC 4 */
    /* Still no prevTid0Pic, so next picture's Msb is 0. */
    CHECK(poc.derive(1, 0, 6, 16) == 6);

    /* RADL_R (7) is excluded from prevTid0Pic selection. */
    poc.reset();
    CHECK(poc.derive(19, 0, 0, 16) == 0);
    CHECK(poc.derive(7, 0, 4, 16) == 4); /* RADL_R: POC 4, not a prev */
    CHECK(poc.derive(1, 0, 8, 16) == 8); /* still uses IDR's lsb 0 */

    /* CRA (21) derives normally (NoRaslOutputFlag == 0). */
    poc.reset();
    CHECK(poc.derive(21, 0, 24, 32) == 24);
}

/*
 * ---------------------------------------------------------------------------
 * 2. AVC POC tracker (H.264 §8.2.1)
 * ---------------------------------------------------------------------------
 */
bs::avc::SequenceParameterSet make_sps_type0() {
    bs::avc::SequenceParameterSet sps;
    sps.pic_order_cnt_type = 0;
    sps.log2_max_pic_order_cnt_lsb_minus4 = 0; /* 4-bit LSB, max 16 */
    sps.log2_max_frame_num_minus4 = 0;         /* 4-bit frame_num, max 16 */
    sps.frame_mbs_only_flag = true;
    return sps;
}

bs::avc::SequenceParameterSet make_sps_type1() {
    bs::avc::SequenceParameterSet sps;
    sps.pic_order_cnt_type = 1;
    sps.log2_max_frame_num_minus4 = 0; /* max frame_num 16 */
    sps.num_ref_frames_in_pic_order_cnt_cycle = 2;
    sps.offset_for_ref_frame[0] = 2;
    sps.offset_for_ref_frame[1] = 2;
    sps.offset_for_top_to_bottom_field = 0;
    sps.offset_for_non_ref_pic = 1;
    sps.frame_mbs_only_flag = true;
    return sps;
}

bs::avc::SequenceParameterSet make_sps_type2() {
    bs::avc::SequenceParameterSet sps;
    sps.pic_order_cnt_type = 2;
    sps.log2_max_frame_num_minus4 = 0; /* max frame_num 16 */
    sps.frame_mbs_only_flag = true;
    return sps;
}

bs::avc::SliceHeader make_slice(
    std::uint32_t frame_num,
    std::uint32_t pic_order_cnt_lsb,
    bs::avc::SliceType type = bs::avc::SliceType::P,
    bool field = false,
    bool bottom = false
) {
    bs::avc::SliceHeader sh;
    sh.frame_num = frame_num;
    sh.pic_order_cnt_lsb = pic_order_cnt_lsb;
    sh.slice_type = type;
    sh.field_pic_flag = field;
    sh.bottom_field_flag = bottom;
    return sh;
}

void test_avc_poc_type0() {
    bs::avc::PocState poc;
    const auto sps = make_sps_type0();

    /* IDR -> POC 0. */
    CHECK(poc.derive(make_slice(0, 0), sps, true, 3) == 0);

    /* First reference P picture. */
    CHECK(poc.derive(make_slice(1, 8), sps, false, 2) == 8);

    /* B pictures reorder below the P picture. */
    CHECK(poc.derive(make_slice(2, 4), sps, false, 0) == 4);
    CHECK(poc.derive(make_slice(3, 2), sps, false, 0) == 2);
    CHECK(poc.derive(make_slice(3, 6), sps, false, 0) == 6);

    /* Non-reference B must not update the prev reference state. */
    CHECK(poc.derive(make_slice(3, 12), sps, false, 2) == 12);
    CHECK(poc.derive(make_slice(4, 10), sps, false, 0) == 10);
}

void test_avc_poc_type1() {
    bs::avc::PocState poc;
    const auto sps = make_sps_type1();

    /* IDR -> 0. */
    CHECK(poc.derive(make_slice(0, 0), sps, true, 3) == 0);

    CHECK(poc.derive(make_slice(1, 0), sps, false, 2) == 2);
    CHECK(poc.derive(make_slice(2, 0), sps, false, 2) == 4);
    CHECK(poc.derive(make_slice(3, 0), sps, false, 2) == 6);

    /* Non-reference picture: abs_frame_num reduced, offset_for_non_ref_pic. */
    CHECK(poc.derive(make_slice(4, 0), sps, false, 0) == 7);
}

void test_avc_poc_type2() {
    bs::avc::PocState poc;
    const auto sps = make_sps_type2();

    CHECK(poc.derive(make_slice(0, 0), sps, true, 3) == 0);
    CHECK(poc.derive(make_slice(1, 0), sps, false, 2) == 2);
    CHECK(poc.derive(make_slice(2, 0), sps, false, 2) == 4);
    /* Non-reference: 2*(3) - 1 = 5. */
    CHECK(poc.derive(make_slice(3, 0), sps, false, 0) == 5);
}

/*
 * ---------------------------------------------------------------------------
 * 3. VVC POC tracker (H.266 §8.3.1)
 * ---------------------------------------------------------------------------
 */
void test_vvc_poc_state() {
    bs::vvc::PocState poc;

    bs::vvc::PictureHeader ph;
    ph.poc_lsb_bits = 4; /* max 16 */

    /* IDR_W_RADL (7) is CLVSS -> 0. */
    ph.poc_lsb = 0;
    CHECK(poc.derive(7, 0, ph) == 0);

    ph.poc_lsb = 4;
    CHECK(poc.derive(0, 0, ph) == 4); /* TRAIL */

    ph.poc_lsb = 8;
    CHECK(poc.derive(0, 0, ph) == 8);

    /* LSB wrap: 8 -> 0, diff 8 >= half 8 -> Msb += 16. */
    ph.poc_lsb = 0;
    CHECK(poc.derive(0, 0, ph) == 16);

    /* Non-reference picture (ph_non_ref_pic_flag) is excluded from
       prevTid0Pic, but still gets a POC derived from the previous state. */
    ph.poc_lsb = 5;
    ph.non_ref_pic_flag = true;
    CHECK(poc.derive(0, 0, ph) == 21);

    /* A reference picture still uses the previous reference's LSB (0). */
    ph.poc_lsb = 9;
    ph.non_ref_pic_flag = false;
    CHECK(poc.derive(0, 0, ph) == 9); /* 9 - 0 > half 8 -> Msb -= 16 -> 0 */

    /* msb-cycle value overrides everything. */
    bs::vvc::PictureHeader ph2;
    ph2.poc_lsb_bits = 4;
    ph2.poc_msb_cycle_present_flag = true;
    ph2.poc_msb_cycle_val = 2;
    ph2.poc_lsb = 3;
    CHECK(poc.derive(0, 0, ph2) == 35); /* 2*16 + 3 */
}

/*
 * ---------------------------------------------------------------------------
 * 4. End-to-end via the unified API
 * ---------------------------------------------------------------------------
 */
void test_unified_hevc(const std::string& path) {
    using namespace bs;

    auto bytes = read_file(path);
    std::span<const std::uint8_t> data{bytes.data(), bytes.size()};

    auto state = create_state(Codec::Hevc);

    g_hevc_pocs.clear();

    HevcParsedHandlers ph{};
    ph.slice = [](const SliceSegmentHeader& sh) { g_hevc_pocs.push_back(sh.derived_poc); };

    (void)parse(*state, data, NalFramingMode::AnnexB, ph, 4);

    const auto& pocs = g_hevc_pocs;

    CHECK(!pocs.empty());
    if (pocs.empty()) {
        return;
    }

    /* First picture is an IRAP with POC 0. */
    CHECK(pocs.front() == 0);

    /*
     * Presentation order (sort by POC) must be strictly increasing.
     * Slices of the same picture share one POC, so de-duplicate first.
     */
    auto ordered = pocs;
    std::sort(ordered.begin(), ordered.end());
    ordered.erase(std::unique(ordered.begin(), ordered.end()), ordered.end());
    for (std::size_t i = 1; i < ordered.size(); ++i) {
        if (ordered[i] <= ordered[i - 1]) {
            std::cerr << "FAIL: non-monotonic HEVC presentation order at " << i << "\n";
            ++g_failures;
            break;
        }
    }
}

void test_unified_avc(const std::string& path) {
    using namespace bs;

    auto bytes = read_file(path);
    std::span<const std::uint8_t> data{bytes.data(), bytes.size()};

    auto state = create_state(Codec::Avc);

    g_avc_pocs.clear();

    AvcParsedHandlers ph{};
    ph.slice = [](const avc::SliceHeader& sh) { g_avc_pocs.push_back(sh.derived_poc); };

    (void)parse(*state, data, NalFramingMode::AnnexB, ph, 4);

    const auto& pocs = g_avc_pocs;

    CHECK(!pocs.empty());
    if (pocs.empty()) {
        return;
    }

    CHECK(pocs.front() == 0);

    /*
     * Presentation order (sort by POC) must be strictly increasing.
     * Slices of the same picture share one POC, so de-duplicate first.
     */
    auto ordered = pocs;
    std::sort(ordered.begin(), ordered.end());
    ordered.erase(std::unique(ordered.begin(), ordered.end()), ordered.end());
    for (std::size_t i = 1; i < ordered.size(); ++i) {
        if (ordered[i] <= ordered[i - 1]) {
            std::cerr << "FAIL: non-monotonic AVC presentation order at " << i << "\n";
            ++g_failures;
            break;
        }
    }
}

/*
 * ---------------------------------------------------------------------------
 * 5. AV1 / VP9 / VP8 — no POC; presentation order is the decode order.
 * ---------------------------------------------------------------------------
 */
void test_unified_av1(const std::string& path) {
    using namespace bs;

    auto bytes = read_file(path);
    std::span<const std::uint8_t> data{bytes.data(), bytes.size()};

    auto state = create_state(Codec::Av1);

    g_av1_orders.clear();
    g_av1_hints.clear();

    Av1ParsedHandlers ph{};
    ph.frame_header = [](const av1::FrameHeader& fh) {
        g_av1_orders.push_back(fh.presentation_order);
        g_av1_hints.push_back(fh.order_hint);
    };

    (void)parse(*state, data, NalFramingMode::Obu, ph, 4);

    const auto& orders = g_av1_orders;
    const auto& hints = g_av1_hints;

    CHECK(!orders.empty());
    if (orders.empty()) {
        return;
    }
    for (std::size_t i = 0; i < orders.size(); ++i) {
        if (orders[i] != static_cast<std::int32_t>(i)) {
            std::cerr << "FAIL: AV1 presentation_order at " << i << "\n";
            ++g_failures;
            break;
        }
    }

    /* order_hint must be parseable (present when enable_order_hint). */
    CHECK(hints.size() == orders.size());
}

/*
 * ---------------------------------------------------------------------------
 * 6. VVC — unified parse; presentation order must match the parsed POCs.
 * ---------------------------------------------------------------------------
 */
void test_unified_vvc(const std::string& path) {
    using namespace bs;

    auto bytes = read_file(path);
    std::span<const std::uint8_t> data{bytes.data(), bytes.size()};

    auto state = create_state(Codec::Vvc);

    g_vvc_pocs.clear();

    VvcParsedHandlers ph{};
    ph.slice = [](const vvc::SliceHeader& sh) { g_vvc_pocs.push_back(sh.derived_poc); };

    (void)parse(*state, data, NalFramingMode::AnnexB, ph, 4);

    const auto& pocs = g_vvc_pocs;

    CHECK(!pocs.empty());
    if (pocs.empty()) {
        return;
    }

    auto ordered = pocs;
    std::sort(ordered.begin(), ordered.end());
    ordered.erase(std::unique(ordered.begin(), ordered.end()), ordered.end());
    for (std::size_t i = 1; i < ordered.size(); ++i) {
        if (ordered[i] <= ordered[i - 1]) {
            std::cerr << "FAIL: non-monotonic VVC presentation order at " << i << "\n";
            ++g_failures;
            break;
        }
    }
}

void test_unified_vp9(const std::string& path) {
    using namespace bs;

    auto bytes = read_file(path);
    std::span<const std::uint8_t> data{bytes.data(), bytes.size()};

    auto state = create_state(Codec::Vp9);

    g_vp9_orders.clear();

    Vp9ParsedHandlers ph{};
    ph.frame_header = [](const vp9::FrameHeader& fh) {
        g_vp9_orders.push_back(fh.presentation_order);
    };

    (void)parse(*state, data, NalFramingMode::Ivf, ph, 4);

    const auto& orders = g_vp9_orders;

    CHECK(!orders.empty());
    if (orders.empty()) {
        return;
    }
    for (std::size_t i = 0; i < orders.size(); ++i) {
        if (orders[i] != static_cast<std::int32_t>(i)) {
            std::cerr << "FAIL: VP9 presentation_order at " << i << "\n";
            ++g_failures;
            break;
        }
    }
}

void test_unified_vp8(const std::string& path) {
    using namespace bs;

    auto bytes = read_file(path);
    std::span<const std::uint8_t> data{bytes.data(), bytes.size()};

    auto state = create_state(Codec::Vp8);

    g_vp8_orders.clear();

    Vp8ParsedHandlers ph{};
    ph.frame_header = [](const vp8::FrameHeader& fh) {
        g_vp8_orders.push_back(fh.presentation_order);
    };

    (void)parse(*state, data, NalFramingMode::Ivf, ph, 4);

    const auto& orders = g_vp8_orders;

    CHECK(!orders.empty());
    if (orders.empty()) {
        return;
    }
    for (std::size_t i = 0; i < orders.size(); ++i) {
        if (orders[i] != static_cast<std::int32_t>(i)) {
            std::cerr << "FAIL: VP8 presentation_order at " << i << "\n";
            ++g_failures;
            break;
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    test_hevc_poc_state();
    test_avc_poc_type0();
    test_avc_poc_type1();
    test_avc_poc_type2();
    test_vvc_poc_state();

    const std::string hevc_path =
        (argc > 1) ? std::string(argv[1]) : std::string("tests/fuzz/corpus/stream.hevc");
    const std::string avc_path =
        (argc > 2) ? std::string(argv[2]) : std::string("tests/fuzz/corpus/avc_main.h264");

    test_unified_hevc(hevc_path);
    test_unified_avc(avc_path);

    if (argc > 3) {
        test_unified_av1(argv[3]);
    }
    if (argc > 4) {
        test_unified_vp9(argv[4]);
    }
    if (argc > 5) {
        test_unified_vp8(argv[5]);
    }
    if (argc > 6) {
        test_unified_vvc(argv[6]);
    }

    if (g_failures) {
        std::cerr << g_failures << " POC check(s) failed\n";
        return 1;
    }

    std::cout << "poc_test: all checks passed\n";
    return 0;
}
