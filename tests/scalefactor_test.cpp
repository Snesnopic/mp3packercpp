// mpeg1_recode_scalefactors and lsf_recode_scalefactors against a search that knows
// nothing of maxima or candidates: every scalefac_compress, scalefac_scale, preflag,
// scfsi pattern, (short blocks) subblock_gain and (long blocks with band 21 silent)
// global_gain moved either way by any multiple of 4, each checked band by band for
// whether a decoder would hear the same step. Also checks that what is written reads
// back as what was chosen, and lsf_partition against minimp3's way of reading the field.
#include "scalefactors.hpp"
#include <algorithm>
#include <cstdio>
#include <random>
#include <vector>

using namespace mp3packer;

namespace {

const int kS1[16] = {0, 0, 0, 0, 3, 1, 1, 1, 2, 2, 2, 3, 3, 3, 4, 4};
const int kS2[16] = {0, 1, 2, 3, 0, 1, 2, 3, 1, 2, 3, 1, 2, 3, 2, 3};
const int kPre[21] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 3, 3, 3, 2};

int cells(bool short_block) { return short_block ? 36 : 21; }
int width(int k, bool short_block, int cell) { return cell < (short_block ? 18 : 11) ? kS1[k] : kS2[k]; }
int group(int b) { return b < 6 ? 0 : b < 11 ? 1 : b < 16 ? 2 : 3; }
// what a decoder adds up for one cell: ffmpeg's gain - ((sf << shift) + sbg * 8), less gain
int step(bool short_block, bool scale, bool pf, const uint8_t* sbg, int cell, int v) {
    if (short_block) return sbg[cell % 3] * 8 + v * (scale ? 4 : 2);
    return (v + (pf ? kPre[cell] : 0)) * (scale ? 4 : 2);
}

// the value a decoder would need under (scale, pf, sbg) to hear exponent e, or -1
int needed(bool short_block, bool scale, bool pf, const uint8_t* sbg, int cell, int e) {
    const int m = scale ? 4 : 2;
    const int rest = e - (short_block ? sbg[cell % 3] * 8 : 0);
    if (rest < 0 || rest % m) return -1;
    return rest / m - (!short_block && pf ? kPre[cell] : 0);
}

int resolved(const Mpeg1ScalefactorChannel& c, int gr, int cell) {
    if (gr == 1 && !c.gr[1].short_block && c.scfsi[group(cell)]) return c.gr[0].values[cell];
    return c.gr[gr].values[cell];
}

// how far global_gain may drop and rise: long blocks whose band 21 is silent, within
// 0..255, and never by more than the 72 quarter steps a heard exponent can span
template <class S>
int drop_limit(const S& s) { return !s.short_block && !s.last_band_active[0] ? s.global_gain : 0; }
template <class S>
int raise_limit(const S& s) {
    return !s.short_block && !s.last_band_active[0] ? std::min(255 - s.global_gain, 72) & ~3 : 0;
}

int gain_moves = 0;

int bits_of(const Mpeg1ScalefactorChannel& c) { return mpeg1_scalefactor_bits(c, 0) + mpeg1_scalefactor_bits(c, 1); }

// cheapest single granule with nothing reused; short blocks try every subblock_gain
int brute_single(const Mpeg1Scalefactors& s, const int* e) {
    int best = 1 << 30;
    for (int scale = 0; scale < 2; ++scale)
        for (int pf = 0; pf < 2; ++pf) {
            const bool p = s.short_block ? s.preflag : pf;
            if (s.short_block && pf) continue;
            for (int gains = 0; gains < (s.short_block ? 512 : 1); ++gains)
            for (int d = -raise_limit(s); d <= drop_limit(s); d += 4) {
                uint8_t sbg[3] = {s.subblock_gain[0], s.subblock_gain[1], s.subblock_gain[2]};
                bool keeps_band12 = true;
                if (s.short_block)
                    for (int w = 0; w < 3; ++w) {
                        sbg[w] = static_cast<uint8_t>((gains >> (3 * w)) & 7);
                        // band 12 carries no scalefactor: its step is subblock_gain's alone
                        if (s.last_band_active[w] && sbg[w] != s.subblock_gain[w]) keeps_band12 = false;
                    }
                if (!keeps_band12) continue;
                // the value each cell needs, or -1 for free
                int need[36];
                bool ok = true;
                for (int b = 0; b < cells(s.short_block) && ok; ++b) {
                    need[b] = s.active[b] ? needed(s.short_block, scale, p, sbg, b, e[b] - d) : -1;
                    if (s.active[b] && need[b] < 0) ok = false;
                }
                if (!ok) continue;
                for (int k = 0; k < 16; ++k) {
                    int cost = 0;
                    bool fit = true;
                    for (int b = 0; b < cells(s.short_block); ++b) {
                        const int w = width(k, s.short_block, b);
                        cost += w;
                        fit = fit && need[b] < (1 << w);
                    }
                    if (fit) best = std::min(best, cost);
                }
            }
        }
    return best;
}

int brute_joint(const Mpeg1ScalefactorChannel& c, const int e[2][36], bool allow) {
    // every scale, preflag and global_gain move under which each heard cell has a value
    // some coding could send (15 below band 11, 7 from it): the rest can never fit
    struct Way {
        int need[21];
    };
    std::vector<Way> ways[2];
    for (int gr = 0; gr < 2; ++gr)
        for (int combo = 0; combo < 4; ++combo)
            for (int d = -raise_limit(c.gr[gr]); d <= drop_limit(c.gr[gr]); d += 4) {
                Way w;
                bool ok = true;
                for (int b = 0; b < 21 && ok; ++b) {
                    w.need[b] = c.gr[gr].active[b] ? needed(false, combo & 1, combo & 2, nullptr, b, e[gr][b] - d) : -1;
                    ok = !c.gr[gr].active[b] || (w.need[b] >= 0 && w.need[b] <= (b < 11 ? 15 : 7));
                }
                if (ok) ways[gr].push_back(w);
            }
    int best = 1 << 30;
    for (int pattern = 0; pattern < (allow ? 16 : 1); ++pattern)
        for (const Way& w1 : ways[1]) {
            // granule 1 sends only the groups it does not reuse
            int c1 = 1 << 30;
            for (int k = 0; k < 16; ++k) {
                int cost = 0;
                bool fit = true;
                for (int b = 0; b < 21; ++b) {
                    if ((pattern >> group(b)) & 1) continue;
                    cost += width(k, false, b);
                    fit = fit && w1.need[b] < (1 << width(k, false, b));
                }
                if (fit) c1 = std::min(c1, cost);
            }
            if (c1 == 1 << 30) continue;
            for (const Way& w0 : ways[0]) {
                // granule 0's value per band: its own need, or for a free cell granule 1's
                int x0[21];
                bool ok = true;
                for (int b = 0; b < 21 && ok; ++b) {
                    int v = w0.need[b];
                    if ((pattern >> group(b)) & 1 && w1.need[b] >= 0) {
                        if (v >= 0 && v != w1.need[b]) ok = false;
                        v = w1.need[b];
                    }
                    x0[b] = v < 0 ? 0 : v;
                }
                if (!ok) continue;
                for (int k = 0; k < 16; ++k) {
                    int cost = 0;
                    bool fit = true;
                    for (int b = 0; b < 21; ++b) {
                        cost += width(k, false, b);
                        fit = fit && x0[b] < (1 << width(k, false, b));
                    }
                    if (fit) best = std::min(best, cost + c1);
                }
            }
        }
    return best;
}

// MPEG-2/2.5: the field read the way minimp3 reads it, as mixed-radix digits over
// three ranges in turn, and the partition sizes as ffmpeg and minimp3 list them
const int kMod[2][3][4] = {{{5, 5, 4, 4}, {5, 5, 4, 1}, {4, 3, 1, 1}}, {{5, 6, 6, 1}, {4, 4, 4, 1}, {4, 3, 1, 1}}};
const int kCount[3][6][4] = {
    {{6, 5, 5, 5}, {6, 5, 7, 3}, {11, 10, 0, 0}, {7, 7, 7, 0}, {6, 6, 6, 3}, {8, 8, 5, 0}},
    {{9, 9, 9, 9}, {9, 9, 12, 6}, {18, 18, 0, 0}, {12, 12, 12, 0}, {12, 9, 9, 6}, {15, 12, 9, 0}},
    {{6, 9, 9, 9}, {6, 9, 12, 6}, {15, 18, 0, 0}, {6, 15, 12, 0}, {6, 12, 9, 6}, {6, 18, 9, 0}},
};

void lsf_read_field(int sfc, bool intensity, int slen[4], int& row) {
    if (intensity) sfc >>= 1;
    const int (*mod)[4] = kMod[intensity ? 1 : 0];
    row = 0;
    while (row < 2 && sfc >= mod[row][0] * mod[row][1] * mod[row][2] * mod[row][3]) {
        sfc -= mod[row][0] * mod[row][1] * mod[row][2] * mod[row][3];
        ++row;
    }
    for (int i = 3, m = 1; i >= 0; --i) {
        slen[i] = sfc / m % mod[row][i];
        m *= mod[row][i];
    }
    if (intensity) row += 3;
}

int lsf_partition_failures() {
    int fails = 0;
    for (int intensity = 0; intensity < 2; ++intensity)
        for (int sfc = 0; sfc < 512; ++sfc)
            for (int cat = 0; cat < 3; ++cat) {
                int slen[4], row;
                lsf_read_field(sfc, intensity, slen, row);
                const LsfPartition p = lsf_partition(sfc, intensity, cat);
                int cells = 0;
                for (int i = 0; i < 4; ++i) {
                    cells += kCount[cat][row][i];
                    if (p.slen[i] != slen[i] || p.count[i] != kCount[cat][row][i]) {
                        if (fails++ < 5) printf("lsf_partition(%d, %d, %d) partition %d: slen %d count %d, expected %d %d\n",
                                                sfc, intensity, cat, i, p.slen[i], p.count[i], slen[i], kCount[cat][row][i]);
                    }
                }
                if (cells != (cat == 0 ? 21 : cat == 1 ? 36 : 33) && fails++ < 5) printf("row %d cat %d holds %d cells\n", row, cat, cells);
            }
    return fails;
}

// cheapest LSF coding: every scale, subblock_gain and scalefac_compress
int lsf_brute(const LsfScalefactors& s, const int* e) {
    const int cat = s.short_block ? 1 : 0;
    const int n = cells(s.short_block);
    int best = 1 << 30;
    for (int scale = 0; scale < 2; ++scale)
        for (int gains = 0; gains < (s.short_block ? 512 : 1); ++gains) {
            uint8_t sbg[3] = {s.subblock_gain[0], s.subblock_gain[1], s.subblock_gain[2]};
            bool keeps_band12 = true;
            if (s.short_block)
                for (int w = 0; w < 3; ++w) {
                    sbg[w] = static_cast<uint8_t>((gains >> (3 * w)) & 7);
                    if (s.last_band_active[w] && sbg[w] != s.subblock_gain[w]) keeps_band12 = false;
                }
            if (!keeps_band12) continue;
            for (int pf = 0; pf < 2; ++pf)
            for (int d = -raise_limit(s); d <= drop_limit(s); d += 4) {
                // decoders ignore preflag on short blocks: the coding stays in its range
                if (s.short_block && pf != (s.scalefac_compress >= 500)) continue;
                int need[36];
                bool ok = true;
                for (int b = 0; b < n && ok; ++b) {
                    need[b] = s.active[b] ? needed(s.short_block, scale, pf, sbg, b, e[b] - d) : 0;
                    ok = need[b] >= 0;
                }
                if (!ok) continue;
                int top[3][4] = {};
                for (int row = 0; row < 3; ++row)
                    for (int i = 0, b = 0; i < 4; ++i)
                        for (int k = 0; k < kCount[cat][row][i]; ++k, ++b) top[row][i] = std::max(top[row][i], need[b]);
                for (int sfc = pf ? 500 : 0; sfc < (pf ? 512 : 500); ++sfc) {
                    int slen[4], row;
                    lsf_read_field(sfc, false, slen, row);
                    int cost = 0;
                    bool fit = true;
                    for (int i = 0; i < 4; ++i) {
                        cost += slen[i] * kCount[cat][row][i];
                        fit = fit && top[row][i] < (1 << slen[i]);
                    }
                    if (fit) best = std::min(best, cost);
                }
            }
        }
    return best;
}

int lsf_trials(std::mt19937& rng, int& recoded, int& saved) {
    int fails = 0;
    for (int t = 0; t < 2000 && fails < 10; ++t) {
        LsfScalefactors s;
        s.short_block = rng() % 4 == 0;
        s.scalefac_scale = rng() & 1;
        s.scalefac_compress = static_cast<uint16_t>(rng() % 5 == 0 ? 500 + rng() % 12 : rng() % 500);
        const bool rich = rng() % 3 == 0;
        // near the top, so a rise has at most a few steps of room and the search stays small
        s.global_gain = static_cast<uint8_t>(rich ? 231 + rng() % 25 : rng() % 3 == 0 ? 243 + rng() % 13 : 255);
        const int cat = s.short_block ? 1 : 0;
        int slen[4], row;
        lsf_read_field(s.scalefac_compress, false, slen, row);
        const int density = static_cast<int>(rng() % 4);
        for (int w = 0; w < 3; ++w) {
            s.subblock_gain[w] = s.short_block ? static_cast<uint8_t>(rng() % 8) : 0;
            s.last_band_active[w] = s.short_block && rng() % 3 == 0;
        }
        if (!s.short_block) s.last_band_active[0] = !rich && rng() % 3 == 0;
        for (int i = 0, b = 0; i < 4; ++i)
            for (int k = 0; k < kCount[cat][row][i]; ++k, ++b) {
                const int range = (rng() % 3 == 0) ? (1 << slen[i]) : std::min(1 << slen[i], 4);
                s.values[b] = static_cast<uint8_t>(rng() % static_cast<unsigned>(range));
                if (rich && slen[i] >= 2) s.values[b] = static_cast<uint8_t>(2 + rng() % static_cast<unsigned>((1 << slen[i]) - 2));
                s.active[b] = density == 3 || (density == 2 && rng() % 4 != 0) || (density == 1 && rng() % 4 == 0);
            }
        const bool pf = s.scalefac_compress >= 500;
        int e[36] = {};
        for (int b = 0; b < cells(s.short_block); ++b) e[b] = step(s.short_block, s.scalefac_scale, pf, s.subblock_gain, b, s.values[b]);

        const LsfScalefactors before = s;
        const int bits_before = lsf_scalefactor_bits(s);
        const bool changed = lsf_recode_scalefactors(s);
        const int bits_after = lsf_scalefactor_bits(s);
        const int expected = std::min(bits_before, lsf_brute(before, e));
        if (bits_after != expected) {
            printf("lsf trial %d: %d bits, a plain search finds %d (before %d, short %d, sfc %d)\n", t, bits_after, expected,
                   bits_before, before.short_block, before.scalefac_compress);
            ++fails;
            continue;
        }
        if (!changed) continue;
        ++recoded;
        saved += bits_before - bits_after;
        const bool pf_after = s.scalefac_compress >= 500;
        if (s.short_block && pf_after != pf) { printf("lsf trial %d: short block left the preflag range\n", t); ++fails; }
        for (int w = 0; w < 3; ++w)
            if (s.short_block && before.last_band_active[w] && s.subblock_gain[w] != before.subblock_gain[w]) {
                printf("lsf trial %d: window %d: band 12 heard, subblock_gain changed\n", t, w);
                ++fails;
            }
        const int moved = before.global_gain - s.global_gain;
        if (moved) ++gain_moves;
        if (moved && (moved % 4 || s.short_block || before.last_band_active[0])) {
            printf("lsf trial %d: global_gain moved by %d\n", t, moved);
            ++fails;
        }
        for (int b = 0; b < cells(s.short_block); ++b)
            if (before.active[b] &&
                s.global_gain - step(s.short_block, s.scalefac_scale, pf_after, s.subblock_gain, b, s.values[b]) != before.global_gain - e[b]) {
                printf("lsf trial %d: cell %d step changed\n", t, b);
                ++fails;
            }
        BitstreamWriter w;
        lsf_write_scalefactors(w, s);
        if (static_cast<int>(w.tell_bit()) != bits_after) { printf("lsf trial %d: wrote %zu bits, counted %d\n", t, w.tell_bit(), bits_after); ++fails; }
        const std::vector<uint8_t> bytes = w.data();
        BitstreamReader r(bytes);
        LsfScalefactors back = s;
        std::fill(std::begin(back.values), std::end(back.values), 0xFF);
        lsf_read_scalefactors(r, back);
        for (int b = 0; b < cells(s.short_block); ++b)
            if (back.values[b] != s.values[b]) { printf("lsf trial %d: cell %d reads back %d, wrote %d\n", t, b, back.values[b], s.values[b]); ++fails; break; }
    }
    return fails;
}

}  // namespace

int main() {
    std::mt19937 rng(12345);
    int fails = 0, recoded = 0, saved = 0;
    const int trials = 3000;
    for (int t = 0; t < trials && fails < 10; ++t) {
        Mpeg1ScalefactorChannel c;
        const int density = static_cast<int>(rng() % 4);   // none, sparse, dense, all active
        // rich: every value at least 2 where its field allows, so global_gain can usually drop
        const bool rich = rng() % 3 == 0;
        for (int gr = 0; gr < 2; ++gr) {
            Mpeg1Scalefactors& s = c.gr[gr];
            s.short_block = rng() % 5 == 0;
            s.scalefac_scale = rng() & 1;
            s.preflag = rng() & 1;
            s.scalefac_compress = static_cast<uint8_t>(rng() % 16);
            s.global_gain = static_cast<uint8_t>(rich ? 243 + rng() % 13 : rng() % 3 == 0 ? 247 + rng() % 9 : 255);
            for (int w = 0; w < 3; ++w) {
                s.subblock_gain[w] = s.short_block ? static_cast<uint8_t>(rng() % 8) : 0;
                s.last_band_active[w] = s.short_block && rng() % 3 == 0;
            }
            if (!s.short_block) s.last_band_active[0] = !rich && rng() % 3 == 0;
            for (int b = 0; b < cells(s.short_block); ++b) {
                const int w = width(s.scalefac_compress, s.short_block, b);
                // small values are what encoders write, and what makes scale changes possible
                const int range = (rng() % 3 == 0) ? (1 << w) : std::min(1 << w, 4);
                s.values[b] = static_cast<uint8_t>(rng() % static_cast<unsigned>(range));
                if (rich && w >= 2) s.values[b] = static_cast<uint8_t>(2 + rng() % static_cast<unsigned>((1 << w) - 2));
                s.active[b] = density == 3 || (density == 2 && rng() % 4 != 0) || (density == 1 && rng() % 4 == 0);
            }
            s.any_nonzero = rng() % 8 != 0;
        }
        const bool joint = !c.gr[0].short_block && !c.gr[1].short_block;
        if (joint && rng() % 2) {
            for (int g = 0; g < 4; ++g) c.scfsi[g] = rng() & 1;
            for (int b = 0; b < 21; ++b)
                if (c.scfsi[group(b)]) c.gr[1].values[b] = c.gr[0].values[b];
        }
        const bool allow = c.gr[0].any_nonzero;

        int e[2][36] = {};
        for (int gr = 0; gr < 2; ++gr)
            for (int b = 0; b < cells(c.gr[gr].short_block); ++b)
                e[gr][b] = step(c.gr[gr].short_block, c.gr[gr].scalefac_scale, c.gr[gr].preflag, c.gr[gr].subblock_gain, b, resolved(c, gr, b));

        const Mpeg1ScalefactorChannel before = c;
        const int bits_before = bits_of(c);
        const bool changed = mpeg1_recode_scalefactors(c, allow);
        const int bits_after = bits_of(c);

        int expected = bits_before;
        if (joint) expected = std::min(expected, brute_joint(before, e, allow));
        else if (!(before.scfsi[0] || before.scfsi[1] || before.scfsi[2] || before.scfsi[3]))
            expected = std::min(expected, brute_single(before.gr[0], e[0]) + brute_single(before.gr[1], e[1]));

        if (bits_after != expected) {
            printf("trial %d: %d bits, a plain search finds %d (before %d, joint %d)\n", t, bits_after, expected, bits_before, joint);
            ++fails;
            continue;
        }
        if (!changed) continue;
        ++recoded;
        saved += bits_before - bits_after;

        // decoders hear the same step in every active cell
        for (int gr = 0; gr < 2; ++gr) {
            const Mpeg1Scalefactors& s = c.gr[gr];
            if (s.short_block && s.preflag != before.gr[gr].preflag) { printf("trial %d: short preflag changed\n", t); ++fails; }
            for (int w = 0; w < 3; ++w)
                if (s.short_block && before.gr[gr].last_band_active[w] && s.subblock_gain[w] != before.gr[gr].subblock_gain[w]) {
                    printf("trial %d: gr %d window %d: band 12 heard, subblock_gain %d -> %d\n", t, gr, w, before.gr[gr].subblock_gain[w], s.subblock_gain[w]);
                    ++fails;
                }
            const int moved = before.gr[gr].global_gain - s.global_gain;
            if (moved) ++gain_moves;
            if (moved && (moved % 4 || s.short_block || before.gr[gr].last_band_active[0])) {
                printf("trial %d: gr %d global_gain moved by %d (short %d, band 21 heard %d)\n", t, gr, moved,
                       s.short_block, before.gr[gr].last_band_active[0]);
                ++fails;
            }
            for (int b = 0; b < cells(s.short_block); ++b) {
                if (!before.gr[gr].active[b]) continue;
                const int now = step(s.short_block, s.scalefac_scale, s.preflag, s.subblock_gain, b, resolved(c, gr, b));
                if (s.global_gain - now != before.gr[gr].global_gain - e[gr][b]) {
                    printf("trial %d: gr %d cell %d hears %d -> %d\n", t, gr, b, before.gr[gr].global_gain - e[gr][b],
                           s.global_gain - now);
                    ++fails;
                }
            }
        }
        if (!allow && (c.scfsi[0] || c.scfsi[1] || c.scfsi[2] || c.scfsi[3]) &&
            !(before.scfsi[0] == c.scfsi[0] && before.scfsi[1] == c.scfsi[1] && before.scfsi[2] == c.scfsi[2] && before.scfsi[3] == c.scfsi[3])) {
            printf("trial %d: scfsi chosen without allow\n", t);
            ++fails;
        }

        // what is written reads back as what was chosen, in the bits counted
        BitstreamWriter w;
        mpeg1_write_scalefactors(w, c, 0);
        mpeg1_write_scalefactors(w, c, 1);
        if (static_cast<int>(w.tell_bit()) != bits_after) { printf("trial %d: wrote %zu bits, counted %d\n", t, w.tell_bit(), bits_after); ++fails; }
        const std::vector<uint8_t> bytes = w.data();
        BitstreamReader r(bytes);
        Mpeg1ScalefactorChannel back = c;
        for (int gr = 0; gr < 2; ++gr) std::fill(std::begin(back.gr[gr].values), std::end(back.gr[gr].values), 0xFF);
        mpeg1_read_scalefactors(r, back, 0);
        mpeg1_read_scalefactors(r, back, 1);
        for (int gr = 0; gr < 2; ++gr)
            for (int b = 0; b < cells(c.gr[gr].short_block); ++b)
                if (resolved(back, gr, b) != resolved(c, gr, b)) { printf("trial %d: gr %d cell %d reads back %d, wrote %d\n", t, gr, b, resolved(back, gr, b), resolved(c, gr, b)); ++fails; break; }
    }
    printf("%d trials, %d re-coded, %d bits saved; global_gain moved in %d granules\n", trials, recoded, saved, gain_moves);
    gain_moves = 0;
    fails += lsf_partition_failures();
    int lsf_recoded = 0, lsf_saved = 0;
    fails += lsf_trials(rng, lsf_recoded, lsf_saved);
    printf("lsf: 2000 trials, %d re-coded, %d bits saved; global_gain moved in %d granules\n", lsf_recoded, lsf_saved, gain_moves);
    printf(fails ? "FAILURES: %d\n" : "ok\n", fails);
    return fails != 0;
}
