// mpeg1_recode_scalefactors against a search that knows nothing of maxima or
// candidates: every scalefac_compress, scalefac_scale, preflag, scfsi pattern and
// (short blocks) subblock_gain,
// each checked band by band for whether a decoder would hear the same step.
// Also checks that what is written reads back as what was chosen.
#include "scalefactors.hpp"
#include <algorithm>
#include <cstdio>
#include <random>

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

int bits_of(const Mpeg1ScalefactorChannel& c) { return mpeg1_scalefactor_bits(c, 0) + mpeg1_scalefactor_bits(c, 1); }

// cheapest single granule with nothing reused; short blocks try every subblock_gain
int brute_single(const Mpeg1Scalefactors& s, const int* e) {
    int best = 1 << 30;
    for (int scale = 0; scale < 2; ++scale)
        for (int pf = 0; pf < 2; ++pf) {
            const bool p = s.short_block ? s.preflag : pf;
            if (s.short_block && pf) continue;
            for (int gains = 0; gains < (s.short_block ? 512 : 1); ++gains) {
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
                    need[b] = s.active[b] ? needed(s.short_block, scale, p, sbg, b, e[b]) : -1;
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
    int best = 1 << 30;
    for (int pattern = 0; pattern < (allow ? 16 : 1); ++pattern)
        for (int combo = 0; combo < 16; ++combo) {
            const bool sc0 = combo & 1, pf0 = combo & 2, sc1 = combo & 4, pf1 = combo & 8;
            // granule 0's value per band, or -2 if infeasible, -1 if free
            int x0[21];
            bool ok = true;
            for (int b = 0; b < 21 && ok; ++b) {
                const bool a0 = c.gr[0].active[b], a1 = c.gr[1].active[b];
                int v = -1;
                if (a0) v = needed(false, sc0, pf0, nullptr, b, e[0][b]);
                if (a0 && v < 0) ok = false;
                if ((pattern >> group(b)) & 1 && a1) {
                    const int u = needed(false, sc1, pf1, nullptr, b, e[1][b]);
                    if (u < 0 || (a0 && u != v)) ok = false;
                    v = u;
                }
                x0[b] = v < 0 ? 0 : v;
            }
            if (!ok) continue;
            int c0 = 1 << 30, c1 = 1 << 30;
            for (int k = 0; k < 16; ++k) {
                int cost = 0;
                bool fit = true;
                for (int b = 0; b < 21; ++b) {
                    cost += width(k, false, b);
                    fit = fit && x0[b] < (1 << width(k, false, b));
                }
                if (fit) c0 = std::min(c0, cost);
                cost = 0;
                fit = true;
                for (int b = 0; b < 21; ++b) {
                    if ((pattern >> group(b)) & 1) continue;
                    cost += width(k, false, b);
                    if (!c.gr[1].active[b]) continue;
                    const int u = needed(false, sc1, pf1, nullptr, b, e[1][b]);
                    fit = fit && u >= 0 && u < (1 << width(k, false, b));
                }
                if (fit) c1 = std::min(c1, cost);
            }
            if (c0 < (1 << 30) && c1 < (1 << 30)) best = std::min(best, c0 + c1);
        }
    return best;
}

}  // namespace

int main() {
    std::mt19937 rng(12345);
    int fails = 0, recoded = 0, saved = 0;
    const int trials = 3000;
    for (int t = 0; t < trials && fails < 10; ++t) {
        Mpeg1ScalefactorChannel c;
        const int density = static_cast<int>(rng() % 4);   // none, sparse, dense, all active
        for (int gr = 0; gr < 2; ++gr) {
            Mpeg1Scalefactors& s = c.gr[gr];
            s.short_block = rng() % 5 == 0;
            s.scalefac_scale = rng() & 1;
            s.preflag = rng() & 1;
            s.scalefac_compress = static_cast<uint8_t>(rng() % 16);
            for (int w = 0; w < 3; ++w) {
                s.subblock_gain[w] = s.short_block ? static_cast<uint8_t>(rng() % 8) : 0;
                s.last_band_active[w] = s.short_block && rng() % 3 == 0;
            }
            for (int b = 0; b < cells(s.short_block); ++b) {
                const int w = width(s.scalefac_compress, s.short_block, b);
                // small values are what encoders write, and what makes scale changes possible
                const int range = (rng() % 3 == 0) ? (1 << w) : std::min(1 << w, 4);
                s.values[b] = static_cast<uint8_t>(rng() % static_cast<unsigned>(range));
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
            for (int b = 0; b < cells(s.short_block); ++b) {
                if (!before.gr[gr].active[b]) continue;
                if (step(s.short_block, s.scalefac_scale, s.preflag, s.subblock_gain, b, resolved(c, gr, b)) != e[gr][b]) {
                    printf("trial %d: gr %d cell %d step %d -> %d\n", t, gr, b, e[gr][b],
                           step(s.short_block, s.scalefac_scale, s.preflag, s.subblock_gain, b, resolved(c, gr, b)));
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
    printf("%d trials, %d re-coded, %d bits saved\n", trials, recoded, saved);
    printf(fails ? "FAILURES: %d\n" : "ok\n", fails);
    return fails != 0;
}
