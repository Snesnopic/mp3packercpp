#include "include/huffman.hpp"
#include "include/huffman_tables.hpp"
#include <map>
#include <algorithm>
#include <iostream>
#include <array>

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

HuffmanOptimizer::HuffmanOptimizer() = default;

std::vector<int16_t> HuffmanOptimizer::decode_quantized_coefficients(
        const HuffmanConfig& config, BitstreamReader& reader,
        const uint32_t samplerate, const int max_huffman_bits) {
    std::vector<int16_t> coeffs(576, 0);
    int out_off = 0;
    const auto sf_bands = get_sf_bands(samplerate);
    const size_t bit_limit = (max_huffman_bits >= 0)
                             ? (reader.tell_bit() + static_cast<size_t>(max_huffman_bits))
                             : SIZE_MAX;

    // decode one full huffman symbol; never aborts mid-traversal (matches mp3packer's decode_big_quants)
    auto decode_symbol = [&](const uint8_t table_idx) -> int16_t {
        if (table_idx == 0) return 0;
        const int16_t* tab = huffman_tables[table_idx].table;
        int16_t got = 0;
        while ((got = *tab++) < 0) {
            if (reader.read_bits(1)) tab -= got;
        }
        return got;
    };

    // big region: boundary checked only at the start of each pair; the symbol is then read whole
    auto decode_region = [&](const int count_pairs, const uint8_t table_idx) -> bool {
        const int linbits = huffman_tables[table_idx].linbits;
        const int target = out_off + count_pairs * 2;
        while (out_off < target && out_off < 575) {
            if (table_idx != 0 && reader.tell_bit() >= bit_limit) return false;
            const int got = decode_symbol(table_idx);
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
    if (cont) decode_region(r2_pairs, config.table2);

    // count1 region: boundary checked only at the start of each quad
    const uint8_t count1_table = config.count1_table_select ? 33 : 32;
    while (out_off <= 572 && reader.tell_bit() < bit_limit) {
        const int got = decode_symbol(count1_table);
        coeffs[out_off++] = static_cast<int16_t>((got & 8) ? (reader.read_bits(1) ? -1 : 1) : 0);
        coeffs[out_off++] = static_cast<int16_t>((got & 4) ? (reader.read_bits(1) ? -1 : 1) : 0);
        coeffs[out_off++] = static_cast<int16_t>((got & 2) ? (reader.read_bits(1) ? -1 : 1) : 0);
        coeffs[out_off++] = static_cast<int16_t>((got & 1) ? (reader.read_bits(1) ? -1 : 1) : 0);
    }
    return coeffs;
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
constexpr int kNumCoefficients = 576; ///< quantized spectral values per granule

// Per-pair cost of a table that cannot represent the pair. Kept at the original
// value, along with the ">= kPenalty means the region is impossible" test, so
// the accept/reject boundary of the search is bit-for-bit the old one. A granule
// is at most 288 pairs, so a sum of penalties cannot overflow int32.
constexpr int32_t kPenalty = 10000;

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
                    t.pair[sym][tab] = kPenalty;
                    continue;
                }
                const int linbits = huffman_tables[tab].linbits;
                int bits = map[sym].length;
                if (sx == 15) bits += linbits;
                if (sy == 15) bits += linbits;
                if (sx != 0) bits += 1;
                if (sy != 0) bits += 1;
                t.pair[sym][tab] = bits;
            }
        }
        // A magnitude m > 15 needs bit_length(m - 15) linbits; max_quant_per_table
        // is exactly 15 + 2^linbits - 1 for every escaping table, so "needs more
        // bits than the table has" is the original's m > max_quant test.
        for (int need = 0; need < 16; ++need) {
            for (int tab = 0; tab < kNumTables; ++tab) {
                t.escape[need][tab] = (need <= linbits_per_table[tab] && max_quant_per_table[tab] >= 0)
                                      ? 0 : (need > 0 ? kPenalty : 0);
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

/// Adds n pairs' costs into the running per-table totals.
static void accumulate_costs(int32_t* acc, const uint32_t* keys, const int n, const PairCosts& ct) {
    for (int i = 0; i < n; ++i) {
        const int32_t* base = ct.pair[keys[i] & 0xFF];
        const int32_t* esc = ct.escape[keys[i] >> 8 & 0xF];
        for (int t = 0; t < kNumTables; ++t) acc[t] += base[t] + esc[t];
    }
}

/// Cheapest table for the span between two prefix rows, packed as cost<<5|table
/// so that the lowest table index wins a tie — the order the original's ascending
/// scan with a strict comparison settled on.
static inline uint32_t best_span(const int32_t* from, const int32_t* to) {
    int32_t best = INT32_MAX;
    for (int t = 0; t < kNumTables; ++t) {
        const int32_t k = (to[t] - from[t]) << 5 | t;
        if (k < best) best = k;
    }
    return static_cast<uint32_t>(best);
}

/// Working set of one search. Thread-local because the search runs once per
/// granule — hundreds of thousands of times per second — and these arrays are far
/// too large to keep reallocating.
struct SearchScratch {
    uint32_t keys[288];

    int32_t acc[kNumTables];                      ///< totals over [0, big_values)
    int32_t rows[kNumBands + 1][kNumTables];      ///< totals at each boundary; slot kNumBands is the short-block region0 boundary
    bool row_valid[kNumBands + 1];

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
        const std::vector<int16_t>& coeffs,
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
            const uint32_t r0_bits = packed0 >> 5;
            if (r0_bits >= static_cast<uint32_t>(kPenalty)) continue;

            const uint32_t packed1 = clip0 ? 0u : span_to_bv(slot);
            const uint32_t r1_bits = packed1 >> 5;
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
        for (bv = last_bv_pair; bv <= max_possible_bv; ++bv) {
            if (!sc.c1_ok[2 * bv]) continue;
            const bool use_33 = sc.c1_bits33[2 * bv] < sc.c1_bits32[2 * bv];
            const uint32_t c1_bits = static_cast<uint32_t>(use_33 ? sc.c1_bits33[2 * bv] : sc.c1_bits32[2 * bv]);
            advance(bv);

            for (int r0_idx = 0; r0_idx < 16; ++r0_idx) {
                const int k0 = r0_idx + 1;
                const bool clip0 = boundary[k0] >= bv; // region0 covers every pair there is
                const uint32_t packed0 = clip0 ? span_to_bv(0) : span_head(k0);
                const uint32_t r0_bits = packed0 >> 5;
                if (r0_bits >= static_cast<uint32_t>(kPenalty)) continue;

                for (int r1_idx = 0; r1_idx < 8; ++r1_idx) {
                    const int k1 = std::min(r0_idx + r1_idx + 2, kNumBands - 1);
                    const bool clip1 = boundary[k1] >= bv;
                    // An empty region costs nothing and takes table 0, whichever
                    // end of the span saturated.
                    const uint32_t packed1 = clip0 ? 0u : (clip1 ? span_to_bv(k0) : span_mid(k0, k1));
                    const uint32_t r1_bits = packed1 >> 5;
                    if (r1_bits >= static_cast<uint32_t>(kPenalty)) continue;

                    const uint32_t packed2 = clip1 ? 0u : span_to_bv(k1);
                    const uint32_t r2_bits = packed2 >> 5;
                    if (r2_bits >= static_cast<uint32_t>(kPenalty)) continue;

                    const uint32_t total = r0_bits + r1_bits + r2_bits + c1_bits;
                    if (total < min_total_bits) {
                        min_total_bits = total;
                        best = {
                            static_cast<uint8_t>(r0_idx),
                            static_cast<uint8_t>(r1_idx),
                            static_cast<uint16_t>(bv),
                            static_cast<uint8_t>(packed0 & 31),
                            static_cast<uint8_t>(packed1 & 31),
                            static_cast<uint8_t>(packed2 & 31),
                            use_33,
                            false,
                            uint8_t{0},
                            false
                        };
                    }

                    // sf_bands is monotone, so r1_end saturates at bv and stays there for
                    // every larger r1_idx: region1/region2 would be identical, so stop here.
                    if (clip1) break;
                }

                // same saturation argument for r0_end: once it hits bv, every larger r0_idx
                // reproduces the exact same r0_bits/t0 and an immediately-saturated inner loop.
                if (clip0) break;
            }
        }
    }

    return best;
}

void HuffmanOptimizer::encode_quantized_coefficients(
        const std::vector<int16_t>& coeffs, const HuffmanConfig& config,
        BitstreamWriter& writer, const uint32_t samplerate) {
    int cur = 0;
    const auto sf_bands = get_sf_bands(samplerate);

    auto encode_pair = [&](const int x, const int y, const uint8_t table_idx) {
        if (table_idx == 0) return;
        const int linbits = huffman_tables[table_idx].linbits;
        const int abs_x = std::min(15, std::abs(x));
        const int abs_y = std::min(15, std::abs(y));
        const auto& map = HuffmanTableManager::get_encoding_map(table_idx);
        const int idx = (abs_x << 4) | abs_y;
        if (!map[idx].valid) return;
        writer.write_bits(map[idx].code, map[idx].length);
        if (abs_x == 15 && linbits > 0) writer.write_bits(static_cast<uint32_t>(std::abs(x) - 15), linbits);
        if (x != 0) writer.write_bits(x < 0 ? 1u : 0u, 1);
        if (abs_y == 15 && linbits > 0) writer.write_bits(static_cast<uint32_t>(std::abs(y) - 15), linbits);
        if (y != 0) writer.write_bits(y < 0 ? 1u : 0u, 1);
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

    for (int i = 0; i < r0_pairs; ++i) { encode_pair(coeffs[cur], coeffs[cur+1], config.table0); cur += 2; }
    for (int i = 0; i < r1_pairs; ++i) { encode_pair(coeffs[cur], coeffs[cur+1], config.table1); cur += 2; }
    for (int i = 0; i < r2_pairs; ++i) { encode_pair(coeffs[cur], coeffs[cur+1], config.table2); cur += 2; }

    int last_nonzero = 0;
    for (int i = 575; i >= 0; --i) {
        if (coeffs[i] != 0) { last_nonzero = i + 1; break; }
    }

    const uint8_t count1_table = config.count1_table_select ? 33 : 32;
    const auto& c1_map = HuffmanTableManager::get_encoding_map(count1_table);
    while (cur <= 572) {
        if (cur >= last_nonzero) break;
        const int v = coeffs[cur];
        const int x = coeffs[cur+2];
        const int y = coeffs[cur+3];
        const int w = coeffs[cur+1];
        const int symbol = ((v != 0) << 3) | ((w != 0) << 2) | ((x != 0) << 1) | (y != 0);
        if (!c1_map[symbol].valid || c1_map[symbol].length == 0) break;
        writer.write_bits(c1_map[symbol].code, c1_map[symbol].length);
        if (v != 0) writer.write_bits(v < 0 ? 1u : 0u, 1);
        if (w != 0) writer.write_bits(w < 0 ? 1u : 0u, 1);
        if (x != 0) writer.write_bits(x < 0 ? 1u : 0u, 1);
        if (y != 0) writer.write_bits(y < 0 ? 1u : 0u, 1);
        cur += 4;
    }
}

} // namespace mp3packer
