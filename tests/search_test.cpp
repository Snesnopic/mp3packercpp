// Holds find_best_config against a search that knows nothing: one that walks every
// legal configuration and keeps the smallest. The optimized search prunes heavily —
// it breaks out of region loops on a monotonicity argument, memoizes spans between
// snapshots, and settles each prefix once — and every one of those shortcuts is a
// claim that the configurations it skipped could not have won. This is where that
// claim gets tested.
//
// A candidate only counts if it round-trips: encoded and decoded back, it has to
// reproduce the coefficients exactly. Otherwise the naive minimum could be some
// configuration that merely happens to emit few bits while not representing the
// data, and the comparison would be against a number the real search is right to
// have refused.
#include <cstdio>
#include <cstdlib>
#include <array>
#include <vector>
#include "huffman.hpp"
#include "bitstream.hpp"

using namespace mp3packer;

namespace {

size_t encoded_bits(const Spectrum& coeffs, const HuffmanConfig& cfg, uint32_t rate) {
    BitstreamWriter w;
    HuffmanOptimizer::encode_quantized_coefficients(coeffs, cfg, w, rate);
    return w.tell_bit();
}

// Encodes, decodes and compares. Returns the bit count, or 0 when the config does
// not faithfully represent the coefficients.
size_t cost_if_faithful(const Spectrum& coeffs, const HuffmanConfig& cfg, uint32_t rate) {
    BitstreamWriter w;
    HuffmanOptimizer::encode_quantized_coefficients(coeffs, cfg, w, rate);
    const size_t bits = w.tell_bit();
    if (bits == 0) return 0;
    const std::vector<uint8_t> bytes = w.data();
    Spectrum back{};
    BitstreamReader r(bytes.data(), bytes.size());
    try {
        HuffmanOptimizer::decode_quantized_coefficients(back, cfg, r, rate, static_cast<int>(bits));
    } catch (const std::exception&) {
        return 0;
    }
    return (back == coeffs) ? bits : 0;
}

// Deterministic. Two shapes, and the second one matters as much as the first:
//   sloped — magnitudes falling off with frequency, most of the tail zero, a few
//            pairs escaping to linbits. What real audio looks like.
//   flat   — one magnitude held across a wide span. This is what puts a large
//            region1 in play: when many bands in the middle want the same table,
//            the optimum uses a wide middle region, and a search that cannot reach
//            those splits loses bits. Without a case of this shape the region
//            boundary loops are never really tested — verified by narrowing them
//            and watching a sloped-only suite still pass.
Spectrum make_spectrum(uint32_t seed, int last_nonzero, int peak, bool flat) {
    uint32_t s = seed * 2654435761u + 1u;
    const auto next = [&s] { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; };
    Spectrum c{};
    for (int i = 0; i < last_nonzero; ++i) {
        const int range = flat ? peak : 1 + (peak * (last_nonzero - i)) / last_nonzero;
        int v = static_cast<int>(next() % static_cast<uint32_t>(range + 1));
        if (!flat && next() % 4 == 0) v = 0;
        if (next() % 2) v = -v;
        c[static_cast<size_t>(i)] = static_cast<int16_t>(v);
    }
    if (last_nonzero > 0 && c[static_cast<size_t>(last_nonzero - 1)] == 0) {
        c[static_cast<size_t>(last_nonzero - 1)] = 1;
    }
    return c;
}

int failures = 0;

void check(const Spectrum& coeffs, uint32_t rate, bool short_block, const char* label) {
    HuffmanConfig orig{};
    orig.window_switching_flag = short_block;
    orig.block_type = short_block ? 2 : 0;
    orig.mixed_block_flag = false;

    int last_nonzero = 0;
    for (int i = 575; i >= 0; --i) {
        if (coeffs[static_cast<size_t>(i)] != 0) { last_nonzero = i + 1; break; }
    }
    orig.big_values = static_cast<uint16_t>((last_nonzero + 1) / 2);
    orig.region0_count = 7;
    orig.region1_count = 7;

    const HuffmanConfig found = HuffmanOptimizer::find_best_config(coeffs, orig, rate);
    const size_t bits_found = cost_if_faithful(coeffs, found, rate);
    if (bits_found == 0) {
        std::printf("FAIL %s: the config the search returned does not round-trip\n", label);
        ++failures;
        return;
    }

    // The naive walk. Region counts are only meaningful for long blocks; a short
    // block's boundaries are fixed by the standard, so those loops collapse.
    size_t best_naive = static_cast<size_t>(-1);
    HuffmanConfig best_cfg{};
    const int max_bv = (last_nonzero + 1) / 2;
    const int r0_hi = short_block ? 0 : 15;
    const int r1_hi = short_block ? 0 : 7;
    for (int bv = 0; bv <= max_bv; ++bv) {
        for (int r0 = 0; r0 <= r0_hi; ++r0) {
            for (int r1 = 0; r1 <= r1_hi; ++r1) {
                for (int c1 = 0; c1 <= 1; ++c1) {
                    HuffmanConfig cfg = orig;
                    cfg.big_values = static_cast<uint16_t>(bv);
                    cfg.region0_count = static_cast<uint8_t>(r0);
                    cfg.region1_count = static_cast<uint8_t>(r1);
                    cfg.count1_table_select = c1 != 0;
                    // Start from a table with linbits, which can represent a
                    // coefficient of any magnitude. Starting from table 0 does not
                    // work: it codes nothing, so while any region still holds it the
                    // whole config fails to round-trip, every candidate scores zero
                    // and the descent never takes its first step. That only shows up
                    // once a spectrum is wide enough to fill more than one region.
                    cfg.table0 = cfg.table1 = cfg.table2 = 31;
                    // Coordinate descent over the three region tables, repeated until
                    // it stops moving. The regions are coded one after another and
                    // their costs simply add, so descending one at a time reaches the
                    // same place as the full 32x32x32 sweep, at a thousandth of it.
                    for (int pass = 0; pass < 2; ++pass) {
                        for (int which = 0; which < 3; ++which) {
                            uint8_t* slot = which == 0 ? &cfg.table0 : (which == 1 ? &cfg.table1 : &cfg.table2);
                            const uint8_t was = *slot;
                            size_t local_best = static_cast<size_t>(-1);
                            uint8_t local_arg = was;
                            for (int t = 0; t < 32; ++t) {
                                *slot = static_cast<uint8_t>(t);
                                const size_t bits = cost_if_faithful(coeffs, cfg, rate);
                                if (bits != 0 && bits < local_best) { local_best = bits; local_arg = static_cast<uint8_t>(t); }
                            }
                            *slot = local_arg;
                        }
                    }
                    const size_t bits = cost_if_faithful(coeffs, cfg, rate);
                    if (bits != 0 && bits < best_naive) { best_naive = bits; best_cfg = cfg; }
                }
            }
        }
    }

    if (best_naive == static_cast<size_t>(-1)) {
        std::printf("FAIL %s: the naive walk found no legal configuration at all\n", label);
        ++failures;
        return;
    }
    if (best_naive < bits_found) {
        std::printf("FAIL %s: search says %zu bits, a plain walk finds %zu "
                    "(bv=%u r0=%u r1=%u t=%u/%u/%u c1=%d)\n",
                    label, bits_found, best_naive, best_cfg.big_values, best_cfg.region0_count,
                    best_cfg.region1_count, best_cfg.table0, best_cfg.table1, best_cfg.table2,
                    best_cfg.count1_table_select ? 1 : 0);
        ++failures;
        return;
    }
    std::printf("ok   %s: %zu bits, and no configuration beats it\n", label, bits_found);
}

} // namespace

int main() {
    static const uint32_t rates[] = {44100, 48000, 32000};
    char label[96];
    for (uint32_t seed = 1; seed <= 4; ++seed) {
        const uint32_t rate = rates[seed % 3];
        const int last_nonzero = 24 + static_cast<int>(seed) * 12;
        const int peak = (seed % 3 == 0) ? 20 : (seed % 3 == 1 ? 3 : 1);
        const Spectrum c = make_spectrum(seed, last_nonzero, peak, false);
        std::snprintf(label, sizeof label, "sloped seed=%u nz=%d peak=%d @%uHz", seed, last_nonzero, peak, rate);
        check(c, rate, false, label);
    }
    for (uint32_t seed = 5; seed <= 8; ++seed) {
        const uint32_t rate = rates[seed % 3];
        const int last_nonzero = 72 + static_cast<int>(seed) * 8;
        const int peak = (seed % 2 == 0) ? 1 : 5;
        const Spectrum c = make_spectrum(seed, last_nonzero, peak, true);
        std::snprintf(label, sizeof label, "flat   seed=%u nz=%d peak=%d @%uHz", seed, last_nonzero, peak, rate);
        check(c, rate, false, label);
    }
    for (uint32_t seed = 9; seed <= 11; ++seed) {
        const uint32_t rate = rates[seed % 3];
        const Spectrum c = make_spectrum(seed, 36, 4, seed % 2 == 0);
        std::snprintf(label, sizeof label, "short  seed=%u @%uHz", seed, rate);
        check(c, rate, true, label);
    }
    std::printf("%s\n", failures ? "FAILURES" : "all searches confirmed optimal");
    return failures ? 1 : 0;
}
