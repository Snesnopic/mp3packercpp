#include "include/huffman.hpp"
#include "include/huffman_tables.hpp"
#include <map>
#include <algorithm>
#include <iostream>
#include <array>

// The cost machinery keeps all 32 code tables side by side, so its two kernels are
// pure 32-lane vector work: eight vectors cover a whole row, and the only scalar
// work is deriving two table addresses from a packed key. Written by hand rather
// than left to the auto-vectoriser, which will not keep the accumulators live
// across the loop; that is worth 8-9% of a recompressing run on its own. Define
// MP3PACKER_NO_SIMD to build the portable form, which is what the kernels are held
// against.
//
// AArch64 with a GNU-style compiler only: the horizontal minimum the span kernel
// ends in has no 32-bit NEON encoding, so __ARM_NEON alone is not enough of a
// test, and MSVC spells its NEON header differently. Everything else builds the
// portable form, which measured about 9% behind on a recompressing run.
#if !defined(MP3PACKER_NO_SIMD) && defined(__aarch64__) && (defined(__ARM_NEON) || defined(__ARM_NEON__))
#define MP3PACKER_NEON 1
#include <arm_neon.h>
#endif


namespace mp3packer {

struct SymbolCode {
    uint32_t code = 0;
    int length = 0;
    bool valid = false;
};

class HuffmanTableManager {
public:
    static const std::array<SymbolCode, 256>& get_encoding_map(const uint8_t table_idx) {
        // a single function-local static gets the C++11 magic-statics guarantee: the
        // first caller (from any thread) builds all 34 maps once under the compiler's
        // hidden guard, every other caller just reads the finished const array — no
        // per-table locking needed, unlike the old per-index lazy cache which raced
        // across worker threads.
        static const std::array<std::array<SymbolCode, 256>, 34> cache = build_all_maps();
        return cache[table_idx];
    }

private:
    static std::array<std::array<SymbolCode, 256>, 34> build_all_maps() {
        std::array<std::array<SymbolCode, 256>, 34> all{};
        for (uint8_t idx = 0; idx < 34; ++idx) all[idx] = build_map(idx);
        return all;
    }

    static std::array<SymbolCode, 256> build_map(const uint8_t table_idx) {
        std::array<SymbolCode, 256> arr{};
        for (auto& sym : arr) sym.valid = false;

        const int16_t* tab = huffman_tables[table_idx].table;
        if (*tab == 0) {
            arr[0] = {0, 0, true};
            return arr;
        }

        struct State { const int16_t* pos; uint32_t code; int length; };
        std::vector<State> stack;
        stack.push_back({tab, 0, 0});

        while (!stack.empty()) {
            const State state = stack.back();
            stack.pop_back();

            const int16_t val = *state.pos;
            if (val < 0) {
                // interior node: push left (bit=0) and right (bit=1) children
                stack.push_back({state.pos + 1,        static_cast<uint32_t>(state.code << 1),       state.length + 1});
                stack.push_back({state.pos + 1 - val,  static_cast<uint32_t>((state.code << 1) | 1), state.length + 1});
            } else if (val < 256) {
                // leaf node: record the codeword
                arr[val] = {state.code, state.length, true};
            }
            // val >= 256: escape code — not mapped for encoding
        }
        return arr;
    }
};

// The band tables are static rather than built per call: every one of decode,
// encode and the search wants them, so returning a vector by value cost three
// heap allocations per granule for data that never changes.
constexpr int kNumBands = 23;      ///< long-block band boundaries, including 0 and 576
constexpr int kNumBandsShort = 14; ///< short-block band boundaries

using SfBands = int[kNumBands];
using SfBandsShort = int[kNumBandsShort];

static constexpr SfBands sf_bands_48000 = { 0, 4, 8, 12, 16, 20, 24, 30, 36, 42, 50, 60, 72, 88, 106, 128, 156, 190, 230, 276, 330, 384, 576 };
static constexpr SfBands sf_bands_44100 = { 0, 4, 8, 12, 16, 20, 24, 30, 36, 44, 52, 62, 74, 90, 110, 134, 162, 196, 238, 288, 342, 418, 576 };
static constexpr SfBands sf_bands_32000 = { 0, 4, 8, 12, 16, 20, 24, 30, 36, 44, 54, 66, 82, 102, 126, 156, 194, 240, 296, 364, 448, 550, 576 };
static constexpr SfBands sf_bands_24000 = { 0, 6, 12, 18, 24, 30, 36, 44, 54, 66, 80, 96, 114, 136, 162, 194, 232, 278, 332, 394, 464, 540, 576 };
static constexpr SfBands sf_bands_lsf   = { 0, 6, 12, 18, 24, 30, 36, 44, 54, 66, 80, 96, 116, 140, 168, 200, 238, 284, 336, 396, 464, 522, 576 };
static constexpr SfBands sf_bands_8000  = { 0, 12, 24, 36, 48, 60, 72, 88, 108, 132, 160, 192, 232, 280, 336, 400, 476, 566, 568, 570, 572, 574, 576 };

static constexpr SfBandsShort sf_bands_short_48000 = { 0, 4, 8, 12, 16, 22, 28, 38, 50, 64, 80, 100, 126, 192 };
static constexpr SfBandsShort sf_bands_short_44100 = { 0, 4, 8, 12, 16, 22, 30, 40, 52, 66, 84, 106, 136, 192 };
static constexpr SfBandsShort sf_bands_short_32000 = { 0, 4, 8, 12, 16, 22, 30, 42, 58, 78, 104, 138, 180, 192 };
static constexpr SfBandsShort sf_bands_short_24000 = { 0, 4, 8, 12, 18, 26, 36, 48, 62, 80, 104, 136, 180, 192 };
static constexpr SfBandsShort sf_bands_short_lsf   = { 0, 4, 8, 12, 18, 24, 32, 42, 56, 74, 100, 132, 174, 192 };
static constexpr SfBandsShort sf_bands_short_8000  = { 0, 8, 16, 24, 36, 52, 72, 96, 124, 160, 162, 164, 166, 192 };

static const int* get_sf_bands(const uint32_t samplerate) {
    if (samplerate == 48000) return sf_bands_48000;
    if (samplerate == 44100) return sf_bands_44100;
    if (samplerate == 32000) return sf_bands_32000;
    if (samplerate == 24000) return sf_bands_24000;
    if (samplerate == 22050 || samplerate == 16000 || samplerate == 12000 || samplerate == 11025)
        return sf_bands_lsf;
    if (samplerate == 8000) return sf_bands_8000;
    return sf_bands_44100;
}

static const int* get_sf_bands_short(const uint32_t samplerate) {
    if (samplerate == 48000) return sf_bands_short_48000;
    if (samplerate == 44100) return sf_bands_short_44100;
    if (samplerate == 32000) return sf_bands_short_32000;
    if (samplerate == 24000) return sf_bands_short_24000;
    if (samplerate == 22050 || samplerate == 16000 || samplerate == 12000 || samplerate == 11025)
        return sf_bands_short_lsf;
    if (samplerate == 8000) return sf_bands_short_8000;
    return sf_bands_short_44100;
}

// --- decode tables ---------------------------------------------------------
namespace {

/// One 8-bit prefix of a code table. A codeword of eight bits or fewer resolves
/// in a single lookup; anything longer hands back the tree node the first eight
/// bits reached, and the walk carries on from there.
struct DecodeEntry {
    int16_t symbol = 0; ///< Decoded table value, unless length is kLongCode
    uint8_t length = 0; ///< Codeword length in bits, or kLongCode to keep walking
    uint16_t node = 0;  ///< Tree index the first eight bits reached
};

/// Marks a prefix whose codeword is longer than eight bits. It cannot be a
/// length, and in particular is not zero: table 0 codes its single symbol in no
/// bits at all, so a zero length is a real one.
constexpr uint8_t kLongCode = 0xFF;

using DecodeLut = DecodeEntry[256];

/// One count1 quadruple, signs applied. Indexed by the codeword's non-zero pattern
/// together with the four bits that follow it, so the pattern and its signs come
/// out of a single lookup however many of those bits are really consumed.
struct Count1Quad { int16_t v[4]; };

static const Count1Quad* count1_quad_table() {
    static const std::array<Count1Quad, 16 * 16> table = [] {
        std::array<Count1Quad, 16 * 16> all{};
        for (int sym = 0; sym < 16; ++sym) {
            for (int signs = 0; signs < 16; ++signs) {
                Count1Quad q{};
                int bit = 3; // sign bits are taken from the top of the four
                for (int i = 0; i < 4; ++i) {
                    if (sym & (8 >> i)) {
                        q.v[i] = (signs >> bit & 1) ? int16_t{-1} : int16_t{1};
                        --bit;
                    }
                }
                all[sym << 4 | signs] = q;
            }
        }
        return all;
    }();
    return table.data();
}

/// How many sign bits a count1 quadruple with this non-zero pattern consumes.
static constexpr uint8_t count1_signs[16] = {0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4};

static const DecodeLut* decode_luts() {
    static const std::array<std::array<DecodeEntry, 256>, 34> luts = [] {
        std::array<std::array<DecodeEntry, 256>, 34> all{};
        for (int idx = 0; idx < 34; ++idx) {
            const int16_t* tab = huffman_tables[idx].table;
            for (int prefix = 0; prefix < 256; ++prefix) {
                int node = 0, len = 0;
                for (;;) {
                    const int16_t v = tab[node];
                    if (v >= 0) { // leaf
                        all[idx][prefix] = {v, static_cast<uint8_t>(len), 0};
                        break;
                    }
                    ++node;
                    if (prefix >> (7 - len) & 1) node -= v;
                    ++len;
                    if (len == 8) { // longer than the prefix: resume at this node
                        all[idx][prefix] = {0, kLongCode, static_cast<uint16_t>(node)};
                        break;
                    }
                }
            }
        }
        return all;
    }();
    return reinterpret_cast<const DecodeLut*>(luts.data());
}

} // namespace

HuffmanOptimizer::HuffmanOptimizer() = default;

void HuffmanOptimizer::decode_quantized_coefficients(
        Spectrum& coeffs, const HuffmanConfig& config, BitstreamReader& reader,
        const uint32_t samplerate, const int max_huffman_bits) {
    int out_off = 0;
    const auto sf_bands = get_sf_bands(samplerate);
    const DecodeLut* luts = decode_luts();
    const Count1Quad* quads = count1_quad_table();
    // The bit position is carried in a local rather than in the reader: going
    // through it for every bit of every symbol put a store-to-load turnaround on
    // the loop's critical path. The reader is resynchronised at each exit.
    size_t bp = reader.tell_bit();
    const size_t bit_limit = (max_huffman_bits >= 0)
                             ? (bp + static_cast<size_t>(max_huffman_bits))
                             : SIZE_MAX;

    // A peeked word only holds 57 bits whatever the misalignment, and a read that
    // reaches past the end of the data has its own behaviour to reproduce, so the
    // last few bytes are decoded a bit at a time instead. That is at most the final
    // granule of a file.
    const size_t fast_until = reader.size_bits() >= 57 ? reader.size_bits() - 57 : 0;

    // One pair, read a bit at a time through the reader. Identical in effect to the
    // fast path, and the only thing that runs where the fast path's assumptions
    // stop holding.
    auto decode_pair_slow = [&](const uint8_t table_idx, const int linbits) {
        reader.seek_bit(bp);
        int16_t got = 0;
        if (table_idx != 0) {
            const int16_t* tab = huffman_tables[table_idx].table;
            while ((got = *tab++) < 0) {
                if (reader.read_bits(1)) tab -= got;
            }
        }
        int y = got & 0xF;
        int x = (got >> 4) & 0xF;
        if (x > 0) {
            if (x == 15 && linbits > 0) x += static_cast<int>(reader.read_bits(linbits));
            if (reader.read_bits(1)) x = -x;
        }
        if (y > 0) {
            if (y == 15 && linbits > 0) y += static_cast<int>(reader.read_bits(linbits));
            if (reader.read_bits(1)) y = -y;
        }
        coeffs[out_off++] = static_cast<int16_t>(x);
        coeffs[out_off++] = static_cast<int16_t>(y);
        bp = reader.tell_bit();
    };

    // big region: boundary checked only at the start of each pair; the symbol is
    // then read whole (matches mp3packer's decode_big_quants). A whole pair —
    // codeword, escape magnitudes and signs — is at most 47 bits, so it comes out
    // of one peeked word rather than bit by bit, and the first eight bits usually
    // resolve the codeword in a single table lookup.
    auto decode_region = [&](const int count_pairs, const uint8_t table_idx) -> bool {
        const int linbits = huffman_tables[table_idx].linbits;
        const int16_t* tree = huffman_tables[table_idx].table;
        const DecodeEntry* lut = luts[table_idx];
        // A magnitude of 15 escapes to linbits, but only for tables that have any.
        // Putting the trigger out of range for the others folds "does this table
        // escape" into the comparison the loop was making anyway.
        const int escape = linbits > 0 ? 15 : 16;
        const int target = out_off + count_pairs * 2;
        // A pair needs two coefficients, so the spectrum's own end bounds the region
        // as much as the declared pair count does; the first even position at or
        // past 575 is 576. Folding them together leaves one comparison per pair
        // instead of two.
        const int end = std::min(target, 576);
        // Table 0 codes nothing: every pair is a pair of zeros and no bits are
        // consumed, so the whole region is a fill rather than a walk. Region 2 uses
        // it often on quiet material.
        if (table_idx == 0) {
            std::fill(coeffs.begin() + out_off, coeffs.begin() + end, int16_t{0});
            out_off = end;
            return true;
        }
        while (out_off < end) {
            if (bp >= bit_limit) return false;
            if (bp > fast_until) { decode_pair_slow(table_idx, linbits); continue; }
            uint64_t w = reader.peek_at(bp);
            int used;
            int sym;
            const DecodeEntry& e = lut[w >> 56];
            if (e.length != kLongCode) {
                sym = e.symbol;
                used = e.length;
                w <<= used;
            } else {
                int node = e.node;
                used = 8;
                w <<= 8;
                for (;;) {
                    const int16_t v = tree[node];
                    if (v >= 0) { sym = v; break; }
                    ++node;
                    if (w >> 63) node -= v;
                    w <<= 1;
                    ++used;
                }
            }
            int y = sym & 0xF;
            int x = (sym >> 4) & 0xF;
            if (x > 0) {
                if (x == escape) {
                    x += static_cast<int>(w >> (64 - linbits));
                    w <<= linbits;
                    used += linbits;
                }
                if (w >> 63) x = -x;
                w <<= 1;
                ++used;
            }
            if (y > 0) {
                if (y == escape) {
                    y += static_cast<int>(w >> (64 - linbits));
                    w <<= linbits;
                    used += linbits;
                }
                if (w >> 63) y = -y;
                ++used;
            }
            coeffs[out_off++] = static_cast<int16_t>(x);
            coeffs[out_off++] = static_cast<int16_t>(y);
            bp += static_cast<size_t>(used);
        }
        return true;
    };

    int r0_pairs, r1_pairs, r2_pairs;
    if (config.window_switching_flag) {
        int region0;
        if (config.block_type == 2 && !config.mixed_block_flag) {
            const auto sf_bands_short = get_sf_bands_short(samplerate);
            region0 = (sf_bands_short[3] / 2) * 3;
        } else {
            region0 = sf_bands[8] / 2;
        }
        r0_pairs = std::min(region0, static_cast<int>(config.big_values));
        r1_pairs = std::max(0, static_cast<int>(config.big_values) - region0);
        r2_pairs = 0;
    } else {
        r0_pairs = std::min(static_cast<int>(config.big_values), sf_bands[config.region0_count + 1] / 2);
        r1_pairs = std::min(static_cast<int>(config.big_values), sf_bands[config.region0_count + config.region1_count + 2] / 2) - r0_pairs;
        r2_pairs = static_cast<int>(config.big_values) - r0_pairs - r1_pairs;
    }

    bool cont = decode_region(r0_pairs, config.table0);
    if (cont) cont = decode_region(r1_pairs, config.table1);
    if (cont) cont = decode_region(r2_pairs, config.table2);

    // count1 region: boundary checked only at the start of each quad. No count1
    // codeword exceeds six bits, so the quadruple and its four possible signs come
    // out of the same peeked word.
    if (cont) {
        const uint8_t count1_table = config.count1_table_select ? 33 : 32;
        const DecodeEntry* lut = luts[count1_table];
        while (out_off <= 572 && bp < bit_limit) {
            if (bp > fast_until) { // last bytes of the data: read a bit at a time
                reader.seek_bit(bp);
                const int16_t* tab = huffman_tables[count1_table].table;
                int16_t got = 0;
                while ((got = *tab++) < 0) {
                    if (reader.read_bits(1)) tab -= got;
                }
                for (int mask = 8; mask != 0; mask >>= 1) {
                    coeffs[out_off++] = static_cast<int16_t>((got & mask) ? (reader.read_bits(1) ? -1 : 1) : 0);
                }
                bp = reader.tell_bit();
                continue;
            }
            uint64_t w = reader.peek_at(bp);
            const DecodeEntry& e = lut[w >> 56];
            if (e.length == kLongCode) break; // unreachable: no count1 codeword exceeds six bits
            const int got = e.symbol;
            w <<= e.length;
            // One lookup covers the pattern and its signs together; the sign bits
            // sit at the top of w now that the codeword has been shifted off.
            const Count1Quad& q = quads[got << 4 | static_cast<int>(w >> 60)];
            coeffs[out_off + 0] = q.v[0];
            coeffs[out_off + 1] = q.v[1];
            coeffs[out_off + 2] = q.v[2];
            coeffs[out_off + 3] = q.v[3];
            out_off += 4;
            bp += static_cast<size_t>(e.length) + count1_signs[got];
        }
    }
    reader.seek_bit(bp);
    // Every coefficient below out_off was written before it could be read, so only
    // the tail above it has to be cleared: zeroing the whole spectrum up front
    // rewrote 1.1 kB per granule to no purpose, since a granule that codes half its
    // coefficients leaves the rest untouched anyway.
    std::fill(coeffs.begin() + out_off, coeffs.end(), int16_t{0});
}

// --- search cost machinery -------------------------------------------------
namespace {
//
// The search never materialises a cost per pair per table. Two observations make
// that unnecessary:
//
//   * A pair's cost under a table follows from its clamped magnitudes alone, so
//     it is a lookup in a table built once for the whole program rather than
//     arithmetic (and a Huffman-map lookup) repeated for 288 pairs × 32 tables
//     on every granule.
//   * Every region boundary the side info can express is a scalefactor band
//     boundary, of which there are 23. So only the running per-table totals *at
//     those boundaries* are ever asked about — 23 rows of 32 lanes, not 288 —
//     and the cost of a span is one row subtracted from another.
//
// Region costs are then memoised by the shape of the span: spans below a
// boundary and spans between two boundaries do not move with big_values and are
// computed once per granule, and only the spans ending at big_values itself are
// recomputed per candidate.

constexpr int kNumTables = 32;

// Per-pair cost of a table that cannot represent the pair. Kept at the original
// value, along with the ">= kPenalty means the region is impossible" test, so
// the accept/reject boundary of the search is bit-for-bit the old one. A granule
// is at most 288 pairs, so a sum of penalties cannot overflow int32.
constexpr int32_t kPenalty = 10000;

/// Costs are held pre-scaled by 1<<kCostShift so that the low bits of a span's
/// cost are free to carry the table index that achieved it. Doing it once in the
/// cost tables rather than per row saves a shift in the innermost kernel; the
/// largest total a granule can reach, 288 pairs of penalties, stays far below the
/// sign bit.
constexpr int kCostShift = 5;

struct PairCosts {
    /// Cost of a pair by clamped symbol: codeword, escape magnitudes and signs.
    int32_t pair[256][kNumTables];
    /// Penalty for a table whose linbits cannot reach a magnitude needing n bits
    /// above the escape threshold. Splitting this out keeps the cost of a pair to
    /// two table rows, both indexed by small integers, which is what lets the
    /// accumulation loop vectorise.
    int32_t escape[16][kNumTables];
};

static const PairCosts& pair_cost_tables() {
    static const PairCosts tables = [] {
        PairCosts t{};
        for (int sym = 0; sym < 256; ++sym) {
            const int sx = sym >> 4, sy = sym & 15;
            for (int tab = 0; tab < kNumTables; ++tab) {
                const auto& map = HuffmanTableManager::get_encoding_map(static_cast<uint8_t>(tab));
                const int max_quant = max_quant_per_table[tab];
                if (max_quant < 0 || !map[sym].valid || sx > max_quant || sy > max_quant) {
                    t.pair[sym][tab] = kPenalty << kCostShift;
                    continue;
                }
                const int linbits = huffman_tables[tab].linbits;
                int bits = map[sym].length;
                if (sx == 15) bits += linbits;
                if (sy == 15) bits += linbits;
                if (sx != 0) bits += 1;
                if (sy != 0) bits += 1;
                t.pair[sym][tab] = bits << kCostShift;
            }
        }
        // A magnitude m > 15 needs bit_length(m - 15) linbits; max_quant_per_table
        // is exactly 15 + 2^linbits - 1 for every escaping table, so "needs more
        // bits than the table has" is the original's m > max_quant test.
        for (int need = 0; need < 16; ++need) {
            for (int tab = 0; tab < kNumTables; ++tab) {
                t.escape[need][tab] = (need <= linbits_per_table[tab] && max_quant_per_table[tab] >= 0)
                                      ? 0 : (need > 0 ? kPenalty << kCostShift : 0);
            }
        }
        return t;
    }();
    return tables;
}

/// Packs a pair into the two cost rows it needs: the clamped symbol, and how
/// many bits of magnitude sit above the escape threshold.
static inline uint32_t pair_key(const int x, const int y) {
    const int ax = std::abs(x), ay = std::abs(y);
    const uint32_t sym = static_cast<uint32_t>(std::min(15, ax) << 4 | std::min(15, ay));
    const int m = std::max(ax, ay);
    uint32_t need = 0;
    if (m > 15) {
        // bit length of m - 15, capped at the table's 16 rows
        unsigned v = static_cast<unsigned>(m - 15);
        while (v) { ++need; v >>= 1; }
        need = std::min(need, 15u);
    }
    return sym | need << 8;
}

/// Labels each table's lane, so that a plain minimum over packed
/// cost<<kCostShift|table values yields both the cheapest cost and which table
/// achieved it, breaking ties towards the lower index.
/// Aligned so the span kernel's loads of it are: leaving it to chance cost 10%.
alignas(16) static constexpr uint32_t lane_index[kNumTables] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
    16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31,
};

/// Adds n pairs' costs into the running per-table totals.
///
/// Both tables it reads are indexed by small fields of the key, so the whole loop
/// is 32 lanes of load-add with no gathers — the shape that lets the compiler hold
/// it in eight vectors. The escape row is all zeros unless the pair's magnitude
/// escapes to linbits, which is uncommon, so it is skipped rather than added.
static void accumulate_costs(int32_t* acc, const uint32_t* keys, const int n, const PairCosts& ct) {
#if defined(MP3PACKER_NEON)
    // Eight vectors cover all 32 lanes and stay live for the whole loop.
    int32x4_t a0 = vld1q_s32(acc + 0),  a1 = vld1q_s32(acc + 4);
    int32x4_t a2 = vld1q_s32(acc + 8),  a3 = vld1q_s32(acc + 12);
    int32x4_t a4 = vld1q_s32(acc + 16), a5 = vld1q_s32(acc + 20);
    int32x4_t a6 = vld1q_s32(acc + 24), a7 = vld1q_s32(acc + 28);
    for (int i = 0; i < n; ++i) {
        const uint32_t key = keys[i];
        const int32_t* base = ct.pair[key & 0xFF];
        int32x4_t b0 = vld1q_s32(base + 0),  b1 = vld1q_s32(base + 4);
        int32x4_t b2 = vld1q_s32(base + 8),  b3 = vld1q_s32(base + 12);
        int32x4_t b4 = vld1q_s32(base + 16), b5 = vld1q_s32(base + 20);
        int32x4_t b6 = vld1q_s32(base + 24), b7 = vld1q_s32(base + 28);
        if (key & 0xF00) {
            const int32_t* esc = ct.escape[key >> 8 & 0xF];
            b0 = vaddq_s32(b0, vld1q_s32(esc + 0));
            b1 = vaddq_s32(b1, vld1q_s32(esc + 4));
            b2 = vaddq_s32(b2, vld1q_s32(esc + 8));
            b3 = vaddq_s32(b3, vld1q_s32(esc + 12));
            b4 = vaddq_s32(b4, vld1q_s32(esc + 16));
            b5 = vaddq_s32(b5, vld1q_s32(esc + 20));
            b6 = vaddq_s32(b6, vld1q_s32(esc + 24));
            b7 = vaddq_s32(b7, vld1q_s32(esc + 28));
        }
        a0 = vaddq_s32(a0, b0); a1 = vaddq_s32(a1, b1);
        a2 = vaddq_s32(a2, b2); a3 = vaddq_s32(a3, b3);
        a4 = vaddq_s32(a4, b4); a5 = vaddq_s32(a5, b5);
        a6 = vaddq_s32(a6, b6); a7 = vaddq_s32(a7, b7);
    }
    vst1q_s32(acc + 0, a0);  vst1q_s32(acc + 4, a1);
    vst1q_s32(acc + 8, a2);  vst1q_s32(acc + 12, a3);
    vst1q_s32(acc + 16, a4); vst1q_s32(acc + 20, a5);
    vst1q_s32(acc + 24, a6); vst1q_s32(acc + 28, a7);
#else
    for (int i = 0; i < n; ++i) {
        const uint32_t key = keys[i];
        const int32_t* base = ct.pair[key & 0xFF];
        if (key & 0xF00) {
            const int32_t* esc = ct.escape[key >> 8 & 0xF];
            for (int t = 0; t < kNumTables; ++t) acc[t] += base[t] + esc[t];
        } else {
            for (int t = 0; t < kNumTables; ++t) acc[t] += base[t];
        }
    }
#endif
}

/// Cheapest table for the span between two prefix rows, packed as
/// cost<<kCostShift|table so that the lowest table index wins a tie — the order
/// the original's ascending scan with a strict comparison settled on.
///
/// Both rows arrive pre-scaled, so their difference already has room for the lane
/// label, and the minimum is taken over unsigned values: costs are non-negative,
/// so the order is the same, and a signed shift that might overflow is enough to
/// stop a vectoriser cold.
static inline uint32_t best_span(const int32_t* from, const int32_t* to) {
#if defined(MP3PACKER_NEON)
    const uint32_t* f = reinterpret_cast<const uint32_t*>(from);
    const uint32_t* t = reinterpret_cast<const uint32_t*>(to);
    uint32x4_t d0 = vorrq_u32(vsubq_u32(vld1q_u32(t + 0),  vld1q_u32(f + 0)),  vld1q_u32(lane_index + 0));
    uint32x4_t d1 = vorrq_u32(vsubq_u32(vld1q_u32(t + 4),  vld1q_u32(f + 4)),  vld1q_u32(lane_index + 4));
    uint32x4_t d2 = vorrq_u32(vsubq_u32(vld1q_u32(t + 8),  vld1q_u32(f + 8)),  vld1q_u32(lane_index + 8));
    uint32x4_t d3 = vorrq_u32(vsubq_u32(vld1q_u32(t + 12), vld1q_u32(f + 12)), vld1q_u32(lane_index + 12));
    uint32x4_t d4 = vorrq_u32(vsubq_u32(vld1q_u32(t + 16), vld1q_u32(f + 16)), vld1q_u32(lane_index + 16));
    uint32x4_t d5 = vorrq_u32(vsubq_u32(vld1q_u32(t + 20), vld1q_u32(f + 20)), vld1q_u32(lane_index + 20));
    uint32x4_t d6 = vorrq_u32(vsubq_u32(vld1q_u32(t + 24), vld1q_u32(f + 24)), vld1q_u32(lane_index + 24));
    uint32x4_t d7 = vorrq_u32(vsubq_u32(vld1q_u32(t + 28), vld1q_u32(f + 28)), vld1q_u32(lane_index + 28));
    d0 = vminq_u32(d0, d1); d2 = vminq_u32(d2, d3);
    d4 = vminq_u32(d4, d5); d6 = vminq_u32(d6, d7);
    d0 = vminq_u32(d0, d2); d4 = vminq_u32(d4, d6);
    return vminvq_u32(vminq_u32(d0, d4));
#else
    uint32_t best = UINT32_MAX;
    for (int t = 0; t < kNumTables; ++t) {
        const uint32_t k = static_cast<uint32_t>(to[t] - from[t]) | lane_index[t];
        best = std::min(best, k);
    }
    return best;
#endif
}

/// The cheapest two-region cover of every pair below a boundary: what it costs,
/// where region0 ends and the tables both regions use. None of it moves with
/// big_values, so it is settled once per boundary and only read thereafter.
struct PrefixSplit {
    int32_t bits = 0;
    int16_t k0 = 0;   ///< boundary region0 ends at
    int8_t t0 = 0;
    int8_t t1 = 0;
    bool ok = false;
    bool done = false;
};

/// Working set of one search. Thread-local because the search runs once per
/// granule — hundreds of thousands of times per second — and these arrays are far
/// too large to keep reallocating.
struct SearchScratch {
    uint32_t keys[288];

    int32_t acc[kNumTables];                      ///< totals over [0, big_values)
    int32_t rows[kNumBands + 1][kNumTables];      ///< totals at each boundary; slot kNumBands is the short-block region0 boundary
    bool row_valid[kNumBands + 1];

    PrefixSplit prefix[kNumBands];                ///< best two-region cover below a boundary, fixed across candidates
    uint32_t head[kNumBands + 1];                 ///< span [0, boundary), fixed across candidates
    bool head_done[kNumBands + 1];
    uint32_t mid[kNumBands + 1][kNumBands + 1];   ///< span between two boundaries, likewise fixed
    bool mid_done[kNumBands + 1][kNumBands + 1];
    uint32_t tail[kNumBands + 1];                 ///< span [boundary, big_values), recomputed per candidate
    int tail_bv[kNumBands + 1];

    int32_t c1_bits32[kNumCoefficients + 8];
    int32_t c1_bits33[kNumCoefficients + 8];
    bool c1_ok[kNumCoefficients + 8];
};

} // namespace

HuffmanConfig HuffmanOptimizer::find_best_config(
        const Spectrum& coeffs,
        const HuffmanConfig& orig_config, uint32_t samplerate) {
    HuffmanConfig best = orig_config;
    uint32_t min_total_bits = 0xFFFFFFFF;
    const int* sf_bands = get_sf_bands(samplerate);
    const PairCosts& ct = pair_cost_tables();
    static thread_local SearchScratch sc;

    int last_bv_pair = 0;
    for (int pair = 287; pair >= 0; --pair) {
        if (std::abs(coeffs[static_cast<size_t>(2 * pair)]) > 1 || std::abs(coeffs[static_cast<size_t>(2 * pair + 1)]) > 1) {
            last_bv_pair = pair + 1;
            break;
        }
    }

    int last_nonzero_coeff = 0;
    for (int i = 575; i >= 0; --i) {
        if (coeffs[i] != 0) {
            last_nonzero_coeff = i + 1;
            break;
        }
    }

    int max_possible_bv = (last_nonzero_coeff + 1) / 2;
    if (max_possible_bv < last_bv_pair) max_possible_bv = last_bv_pair;

    for (int pair = 0; pair < max_possible_bv; ++pair) {
        sc.keys[pair] = pair_key(coeffs[static_cast<size_t>(2 * pair)], coeffs[static_cast<size_t>(2 * pair + 1)]);
    }

    // count1 cost as a backward recurrence: coding the tail from an even position
    // is one quadruple plus the cost four coefficients later, so one walk serves
    // every candidate big_values instead of a rescan per candidate. The stopping
    // rules are the original's — a position at or above the last non-zero
    // coefficient, or past the last quadruple, costs nothing and is usable.
    const auto& c1_arr_32 = HuffmanTableManager::get_encoding_map(32);
    const auto& c1_arr_33 = HuffmanTableManager::get_encoding_map(33);
    for (int pos = kNumCoefficients; pos >= 2 * last_bv_pair; pos -= 2) {
        if (pos >= last_nonzero_coeff || pos > kNumCoefficients - 4) {
            sc.c1_bits32[pos] = 0;
            sc.c1_bits33[pos] = 0;
            sc.c1_ok[pos] = true;
            continue;
        }
        const int v = coeffs[pos], w = coeffs[pos+1], x = coeffs[pos+2], y = coeffs[pos+3];
        const int symbol = ((v != 0) << 3) | ((w != 0) << 2) | ((x != 0) << 1) | (y != 0);
        if (!c1_arr_32[symbol].valid || c1_arr_32[symbol].length == 0 || !sc.c1_ok[pos + 4]) {
            sc.c1_ok[pos] = false;
            continue;
        }
        const int32_t signs = (v != 0) + (w != 0) + (x != 0) + (y != 0);
        sc.c1_ok[pos] = true;
        sc.c1_bits32[pos] = sc.c1_bits32[pos + 4] + c1_arr_32[symbol].length + signs;
        sc.c1_bits33[pos] = sc.c1_bits33[pos + 4] + c1_arr_33[symbol].length + signs;
    }

    // Boundary positions in pairs, and the order the accumulator reaches them.
    // Short blocks put region0's boundary somewhere that is not a long-band
    // boundary at all, so it gets a slot of its own.
    int boundary[kNumBands + 1];
    for (int k = 0; k < kNumBands; ++k) boundary[k] = sf_bands[k] / 2;
    boundary[kNumBands] = get_sf_bands_short(samplerate)[3] / 2 * 3;
    int order[kNumBands + 1];
    for (int k = 0; k <= kNumBands; ++k) order[k] = k;
    for (int i = 1; i <= kNumBands; ++i) { // insertion sort: only the short slot is out of place
        const int s = order[i];
        int j = i - 1;
        while (j >= 0 && boundary[order[j]] > boundary[s]) { order[j + 1] = order[j]; --j; }
        order[j + 1] = s;
    }

    std::fill(std::begin(sc.acc), std::end(sc.acc), 0);
    std::fill(std::begin(sc.row_valid), std::end(sc.row_valid), false);
    std::fill(std::begin(sc.head_done), std::end(sc.head_done), false);
    std::fill(std::begin(sc.tail_bv), std::end(sc.tail_bv), -1);
    for (auto& row : sc.mid_done) std::fill(std::begin(row), std::end(row), false);
    for (auto& p : sc.prefix) p = PrefixSplit{};

    // The accumulator walks the pairs once, in step with big_values, snapshotting
    // each boundary as it passes: every prefix the search can ask about is either
    // behind it or exactly at it.
    int pos = 0, next_snap = 0;
    auto advance = [&](const int to) {
        while (next_snap <= kNumBands) {
            const int slot = order[next_snap];
            const int at = boundary[slot];
            if (at > to) break;
            if (at > pos) { accumulate_costs(sc.acc, sc.keys + pos, at - pos, ct); pos = at; }
            std::copy(std::begin(sc.acc), std::end(sc.acc), std::begin(sc.rows[slot]));
            sc.row_valid[slot] = true;
            ++next_snap;
        }
        if (to > pos) { accumulate_costs(sc.acc, sc.keys + pos, to - pos, ct); pos = to; }
    };

    int bv = 0;
    auto span_to_bv = [&](const int slot) {
        if (sc.tail_bv[slot] != bv) {
            sc.tail[slot] = best_span(sc.rows[slot], sc.acc);
            sc.tail_bv[slot] = bv;
        }
        return sc.tail[slot];
    };
    auto span_head = [&](const int slot) {
        if (!sc.head_done[slot]) {
            sc.head[slot] = best_span(sc.rows[0], sc.rows[slot]);
            sc.head_done[slot] = true;
        }
        return sc.head[slot];
    };
    auto span_mid = [&](const int a, const int b) {
        if (!sc.mid_done[a][b]) {
            sc.mid[a][b] = best_span(sc.rows[a], sc.rows[b]);
            sc.mid_done[a][b] = true;
        }
        return sc.mid[a][b];
    };

    // The best two-region cover below a boundary. Both spans it considers are
    // between snapshots, so neither moves with big_values and the answer is
    // computed the first time a candidate can reach the boundary at all. On a tie
    // the lower region0 boundary wins, which is the order the enumeration it
    // replaces would have found them in.
    auto span_prefix = [&](const int k1) -> const PrefixSplit& {
        PrefixSplit& p = sc.prefix[k1];
        if (p.done) return p;
        p.done = true;
        for (int k0 = std::max(1, k1 - 8); k0 <= std::min(k1 - 1, 16); ++k0) {
            const uint32_t packed0 = span_head(k0);
            const uint32_t bits0 = packed0 >> kCostShift;
            if (bits0 >= static_cast<uint32_t>(kPenalty)) continue;
            const uint32_t packed1 = span_mid(k0, k1);
            const uint32_t bits1 = packed1 >> kCostShift;
            if (bits1 >= static_cast<uint32_t>(kPenalty)) continue;
            const int32_t total = static_cast<int32_t>(bits0 + bits1);
            if (!p.ok || total < p.bits) {
                p.ok = true;
                p.bits = total;
                p.k0 = static_cast<int16_t>(k0);
                p.t0 = static_cast<int8_t>(packed0 & 31);
                p.t1 = static_cast<int8_t>(packed1 & 31);
            }
        }
        return p;
    };


    if (orig_config.window_switching_flag) {
        // region boundaries are fixed by the standard for short/mixed blocks:
        //   pure short (block_type=2, mixed=0): region0 = (sf_bands_short[3]/2)*3 pairs
        //   mixed or start/end blocks:           region0 = sf_bands[8]/2 pairs
        // only two tables (table0, table1); no region2; region0_count/region1_count unused.
        const int slot = (orig_config.block_type == 2 && !orig_config.mixed_block_flag) ? kNumBands : 8;
        const int region0_boundary = boundary[slot];

        for (bv = last_bv_pair; bv <= max_possible_bv; ++bv) {
            if (!sc.c1_ok[2 * bv]) continue;
            const bool use_33 = sc.c1_bits33[2 * bv] < sc.c1_bits32[2 * bv];
            const uint32_t c1_bits = static_cast<uint32_t>(use_33 ? sc.c1_bits33[2 * bv] : sc.c1_bits32[2 * bv]);
            advance(bv);

            // region0 saturating at big_values leaves region1 empty, which the
            // original costed as zero bits with table 0.
            const bool clip0 = region0_boundary >= bv;
            const uint32_t packed0 = clip0 ? span_to_bv(0) : span_head(slot);
            const uint32_t r0_bits = packed0 >> kCostShift;
            if (r0_bits >= static_cast<uint32_t>(kPenalty)) continue;

            const uint32_t packed1 = clip0 ? 0u : span_to_bv(slot);
            const uint32_t r1_bits = packed1 >> kCostShift;
            if (r1_bits >= static_cast<uint32_t>(kPenalty)) continue;

            const uint32_t total = r0_bits + r1_bits + c1_bits;
            if (total < min_total_bits) {
                min_total_bits = total;
                best = {
                    orig_config.region0_count,
                    orig_config.region1_count,
                    static_cast<uint16_t>(bv),
                    static_cast<uint8_t>(packed0 & 31),
                    static_cast<uint8_t>(packed1 & 31),
                    uint8_t{0},
                    use_33,
                    true,
                    orig_config.block_type,
                    orig_config.mixed_block_flag
                };
            }
        }
    } else {
        // Candidates are enumerated by shape rather than by (region0_count,
        // region1_count). Every candidate splits the pairs below big_values into
        // one, two or three regions at band boundaries, and which shape a
        // (region0_count, region1_count) pair produces depends only on how many
        // boundaries lie below big_values:
        //
        //   nTail is that count, and because the boundaries are sorted,
        //   "boundary[k] >= big_values" — the test that used to saturate the two
        //   loops — is exactly "k >= nTail".
        //
        // What that buys is the three-region case. Regions 0 and 1 cover
        // everything below their upper boundary and neither moves with
        // big_values, so the cheapest pair of them is settled once per granule per
        // boundary and only the tail is recomputed per candidate. The old nested
        // loops re-derived it for every candidate: 113 combinations per
        // big_values, where this is one per boundary.
        //
        // Ties are resolved exactly as the nested loops did — lowest total, then
        // lowest region0_count, then lowest region1_count — which is why the
        // prefix search below prefers the lowest region0 boundary on a tie and why
        // the shapes are offered in that order.
        int n_tail = 0;
        for (bv = last_bv_pair; bv <= max_possible_bv; ++bv) {
            if (!sc.c1_ok[2 * bv]) continue;
            const bool use_33 = sc.c1_bits33[2 * bv] < sc.c1_bits32[2 * bv];
            const uint32_t c1_bits = static_cast<uint32_t>(use_33 ? sc.c1_bits33[2 * bv] : sc.c1_bits32[2 * bv]);
            advance(bv);
            // big_values only grows, so this is carried across candidates rather
            // than recounted.
            while (n_tail < kNumBands && boundary[n_tail] < bv) ++n_tail;

            uint32_t cand_total = UINT32_MAX;
            int cand_r0 = 0, cand_r1 = 0, cand_t0 = 0, cand_t1 = 0, cand_t2 = 0;
            const auto offer = [&](const uint32_t total, const int r0_idx, const int r1_idx,
                                   const int t0, const int t1, const int t2) {
                if (total > cand_total) return;
                if (total == cand_total && (r0_idx > cand_r0 || (r0_idx == cand_r0 && r1_idx >= cand_r1))) return;
                cand_total = total;
                cand_r0 = r0_idx; cand_r1 = r1_idx;
                cand_t0 = t0; cand_t1 = t1; cand_t2 = t2;
            };

            // One region: the first region0 boundary at or beyond big_values
            // swallows every pair, and no larger region0_count can do anything
            // different. Its region1_count is 0, since the inner loop broke at once.
            const int only = std::max(0, n_tail - 1);
            if (only < 16) {
                const uint32_t packed = span_to_bv(0);
                if ((packed >> kCostShift) < static_cast<uint32_t>(kPenalty)) {
                    offer((packed >> kCostShift) + c1_bits, only, 0, static_cast<int>(packed & 31), 0, 0);
                }
            }

            // Two regions: region1 runs from a boundary up to big_values, which
            // needs the smallest region1_count whose own boundary reaches it.
            for (int k0 = 1; k0 <= std::min(n_tail - 1, 16); ++k0) {
                const int r1_idx = std::max(0, n_tail - k0 - 1);
                if (r1_idx > 7) continue; // region1_count cannot stretch that far
                const uint32_t packed0 = span_head(k0);
                if ((packed0 >> kCostShift) >= static_cast<uint32_t>(kPenalty)) continue;
                const uint32_t packed1 = span_to_bv(k0);
                if ((packed1 >> kCostShift) >= static_cast<uint32_t>(kPenalty)) continue;
                offer((packed0 >> kCostShift) + (packed1 >> kCostShift) + c1_bits,
                      k0 - 1, r1_idx, static_cast<int>(packed0 & 31), static_cast<int>(packed1 & 31), 0);
            }

            // Three regions: the pairs below a boundary are covered by regions 0
            // and 1 at their settled best, and region2 runs from there to
            // big_values.
            for (int k1 = 2; k1 < n_tail; ++k1) {
                const PrefixSplit& p = span_prefix(k1);
                if (!p.ok) continue;
                const uint32_t packed2 = span_to_bv(k1);
                if ((packed2 >> kCostShift) >= static_cast<uint32_t>(kPenalty)) continue;
                offer(static_cast<uint32_t>(p.bits) + (packed2 >> kCostShift) + c1_bits,
                      p.k0 - 1, k1 - p.k0 - 1, p.t0, p.t1, static_cast<int>(packed2 & 31));
            }

            if (cand_total < min_total_bits) {
                min_total_bits = cand_total;
                best = {
                    static_cast<uint8_t>(cand_r0),
                    static_cast<uint8_t>(cand_r1),
                    static_cast<uint16_t>(bv),
                    static_cast<uint8_t>(cand_t0),
                    static_cast<uint8_t>(cand_t1),
                    static_cast<uint8_t>(cand_t2),
                    use_33,
                    false,
                    uint8_t{0},
                    false
                };
            }
        }
    }

    return best;
}

void HuffmanOptimizer::encode_quantized_coefficients(
        const Spectrum& coeffs, const HuffmanConfig& config,
        BitstreamWriter& writer, const uint32_t samplerate) {
    int cur = 0;
    const auto sf_bands = get_sf_bands(samplerate);

    // The accumulator is carried in these two locals for the whole granule and
    // handed back at the end, so appending a field is a shift and an or in
    // registers rather than a call into the writer and a round trip through its
    // state. A pair's codeword, escape magnitudes and signs come to at most 47
    // bits, so they are assembled in one word and committed in one step.
    uint64_t acc;
    int nacc;
    writer.pending(acc, nacc);
    auto emit = [&](const uint64_t word, const int n) {
        if (nacc + n > 64) writer.store(acc, nacc);
        acc |= (word & ((uint64_t{1} << n) - 1)) << (64 - nacc - n);
        nacc += n;
    };

    auto encode_pair = [&](const int x, const int y, const std::array<SymbolCode, 256>& map, const int linbits) {
        const int mag_x = std::abs(x), mag_y = std::abs(y);
        const int abs_x = std::min(15, mag_x), abs_y = std::min(15, mag_y);
        const SymbolCode& code = map[static_cast<size_t>(abs_x << 4 | abs_y)];
        if (!code.valid) return;
        uint64_t word = code.code;
        int n = code.length;
        if (abs_x == 15 && linbits > 0) { word = word << linbits | static_cast<uint32_t>(mag_x - 15); n += linbits; }
        if (x != 0) { word = word << 1 | (x < 0 ? 1u : 0u); n += 1; }
        if (abs_y == 15 && linbits > 0) { word = word << linbits | static_cast<uint32_t>(mag_y - 15); n += linbits; }
        if (y != 0) { word = word << 1 | (y < 0 ? 1u : 0u); n += 1; }
        emit(word, n);
    };

    // Everything that depends only on the table is settled per region: the
    // per-pair loop then touches one code entry and nothing else global.
    auto encode_region = [&](const int pairs, const uint8_t table_idx) {
        if (table_idx == 0) { cur += 2 * pairs; return; } // table 0 codes nothing
        const auto& map = HuffmanTableManager::get_encoding_map(table_idx);
        const int linbits = huffman_tables[table_idx].linbits;
        for (int i = 0; i < pairs; ++i) {
            encode_pair(coeffs[cur], coeffs[cur + 1], map, linbits);
            cur += 2;
        }
    };

    int r0_pairs, r1_pairs, r2_pairs;
    if (config.window_switching_flag) {
        int region0;
        if (config.block_type == 2 && !config.mixed_block_flag) {
            const auto sf_bands_short = get_sf_bands_short(samplerate);
            region0 = (sf_bands_short[3] / 2) * 3;
        } else {
            region0 = sf_bands[8] / 2;
        }
        r0_pairs = std::min(region0, static_cast<int>(config.big_values));
        r1_pairs = std::max(0, static_cast<int>(config.big_values) - region0);
        r2_pairs = 0;
    } else {
        r0_pairs = std::min(static_cast<int>(config.big_values), sf_bands[config.region0_count + 1] / 2);
        r1_pairs = std::min(static_cast<int>(config.big_values), sf_bands[config.region0_count + config.region1_count + 2] / 2) - r0_pairs;
        r2_pairs = static_cast<int>(config.big_values) - r0_pairs - r1_pairs;
    }

    encode_region(r0_pairs, config.table0);
    encode_region(r1_pairs, config.table1);
    encode_region(r2_pairs, config.table2);

    int last_nonzero = 0;
    for (int i = 575; i >= 0; --i) {
        if (coeffs[i] != 0) { last_nonzero = i + 1; break; }
    }

    const uint8_t count1_table = config.count1_table_select ? 33 : 32;
    const auto& c1_map = HuffmanTableManager::get_encoding_map(count1_table);
    while (cur <= 572) {
        if (cur >= last_nonzero) break;
        const int v = coeffs[cur];
        const int w = coeffs[cur+1];
        const int x = coeffs[cur+2];
        const int y = coeffs[cur+3];
        const int symbol = ((v != 0) << 3) | ((w != 0) << 2) | ((x != 0) << 1) | (y != 0);
        const SymbolCode& code = c1_map[static_cast<size_t>(symbol)];
        if (!code.valid || code.length == 0) break;
        // The quadruple's codeword and its signs, at most ten bits, go over in one
        // step: a zero coefficient contributes no sign bit and shifts nothing.
        uint64_t word = code.code;
        int n = code.length;
        if (v != 0) { word = word << 1 | (v < 0 ? 1u : 0u); ++n; }
        if (w != 0) { word = word << 1 | (w < 0 ? 1u : 0u); ++n; }
        if (x != 0) { word = word << 1 | (x < 0 ? 1u : 0u); ++n; }
        if (y != 0) { word = word << 1 | (y < 0 ? 1u : 0u); ++n; }
        emit(word, n);
        cur += 4;
    }

    writer.resume(acc, nacc);
}

} // namespace mp3packer
