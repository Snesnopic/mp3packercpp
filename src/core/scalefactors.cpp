#include "scalefactors.hpp"

#include <algorithm>

namespace mp3packer {
namespace {

constexpr int kSlen1[16] = {0, 0, 0, 0, 3, 1, 1, 1, 2, 2, 2, 3, 3, 3, 4, 4};
constexpr int kSlen2[16] = {0, 1, 2, 3, 0, 1, 2, 3, 1, 2, 3, 1, 2, 3, 2, 3};
constexpr int kPretab[21] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 3, 3, 3, 2};
constexpr int kGroupFirst[5] = {0, 6, 11, 16, 21};
constexpr int kLongCells = 21;
constexpr int kShortCells = 36;

constexpr int kLongEdges[3][23] = {
    {0, 4, 8, 12, 16, 20, 24, 30, 36, 44, 52, 62, 74, 90, 110, 134, 162, 196, 238, 288, 342, 418, 576},
    {0, 4, 8, 12, 16, 20, 24, 30, 36, 42, 50, 60, 72, 88, 106, 128, 156, 190, 230, 276, 330, 384, 576},
    {0, 4, 8, 12, 16, 20, 24, 30, 36, 44, 54, 66, 82, 102, 126, 156, 194, 240, 296, 364, 448, 550, 576},
};
constexpr int kShortEdges[3][14] = {
    {0, 4, 8, 12, 16, 22, 30, 40, 52, 66, 84, 106, 136, 192},
    {0, 4, 8, 12, 16, 22, 28, 38, 50, 64, 80, 100, 126, 192},
    {0, 4, 8, 12, 16, 22, 30, 42, 58, 78, 104, 138, 180, 192},
};

int group_of(const int band) { return band < 6 ? 0 : band < 11 ? 1 : band < 16 ? 2 : 3; }

int first_slen2_cell(const bool short_block) { return short_block ? 18 : 11; }

int slen_of(const Mpeg1Scalefactors& s, const int cell) {
    return cell < first_slen2_cell(s.short_block) ? kSlen1[s.scalefac_compress & 15]
                                                  : kSlen2[s.scalefac_compress & 15];
}

// the part of a cell's step exponent that the scalefactors carry, in quarter steps
int exponent(const bool short_block, const bool scale, const bool preflag, const int cell, const int value) {
    const int pre = (!short_block && preflag) ? kPretab[cell] : 0;
    return (value + pre) << (scale ? 2 : 1);
}

int cheapest_compress(const int max1, const int n1, const int max2, const int n2, int& bits) {
    int best = -1;
    for (int k = 0; k < 16; ++k) {
        if (n1 > 0 && max1 >= (1 << kSlen1[k])) continue;
        if (n2 > 0 && max2 >= (1 << kSlen2[k])) continue;
        const int b = kSlen1[k] * n1 + kSlen2[k] * n2;
        if (best < 0 || b < bits) {
            best = k;
            bits = b;
        }
    }
    return best;
}

// a long granule sending only the scfsi groups in mask
int long_cost(const uint8_t* v, const unsigned mask, int& compress) {
    int max1 = 0, n1 = 0, max2 = 0, n2 = 0;
    for (int grp = 0; grp < 4; ++grp) {
        if (!(mask & (1u << grp))) continue;
        for (int b = kGroupFirst[grp]; b < kGroupFirst[grp + 1]; ++b) {
            if (grp < 2) {
                max1 = std::max<int>(max1, v[b]);
                ++n1;
            } else {
                max2 = std::max<int>(max2, v[b]);
                ++n2;
            }
        }
    }
    int bits = 0;
    compress = cheapest_compress(max1, n1, max2, n2, bits);
    return bits;
}

int short_cost(const uint8_t* v, int& compress) {
    int max1 = 0, max2 = 0;
    for (int c = 0; c < 18; ++c) max1 = std::max<int>(max1, v[c]);
    for (int c = 18; c < kShortCells; ++c) max2 = std::max<int>(max2, v[c]);
    int bits = 0;
    compress = cheapest_compress(max1, 18, max2, 18, bits);
    return bits;
}

struct Candidate {
    bool scale = false;
    bool preflag = false;
    uint8_t v[kMaxScalefactorCells] = {};  // inactive cells left at 0
};

// every scalefac_scale/preflag pair under which each active cell keeps its exponent
int candidates(const Mpeg1Scalefactors& s, Candidate out[4]) {
    const int cells = mpeg1_cell_count(s);
    int n = 0;
    for (int scale = 0; scale < 2; ++scale) {
        for (int pf = 0; pf < 2; ++pf) {
            // short blocks ignore preflag: the bit stays as it came
            if (s.short_block && pf == 1) continue;
            Candidate q;
            q.scale = scale != 0;
            q.preflag = s.short_block ? s.preflag : pf != 0;
            const int shift = scale ? 2 : 1;
            bool ok = true;
            for (int c = 0; c < cells && ok; ++c) {
                if (!s.active[c]) continue;
                const int e = exponent(s.short_block, s.scalefac_scale, s.preflag, c, s.values[c]);
                const int pre = (!s.short_block && q.preflag) ? kPretab[c] : 0;
                const int v = (e >> shift) - pre;
                ok = (e & ((1 << shift) - 1)) == 0 && v >= 0 && v <= (c < first_slen2_cell(s.short_block) ? 15 : 7);
                if (ok) q.v[c] = static_cast<uint8_t>(v);
            }
            if (ok) out[n++] = q;
        }
    }
    return n;
}

int resolved_value(const Mpeg1ScalefactorChannel& c, const int gr, const int cell) {
    const Mpeg1Scalefactors& s = c.gr[gr];
    if (gr == 1 && !s.short_block && c.scfsi[group_of(cell)]) return c.gr[0].values[cell];
    return s.values[cell];
}

// what a decoder hears in every active cell is unchanged, and every sent value fits
bool same_steps(const Mpeg1ScalefactorChannel& a, const Mpeg1ScalefactorChannel& b) {
    for (int gr = 0; gr < 2; ++gr) {
        const Mpeg1Scalefactors& sa = a.gr[gr];
        const Mpeg1Scalefactors& sb = b.gr[gr];
        if (sa.short_block != sb.short_block) return false;
        if (sa.short_block && sa.preflag != sb.preflag) return false;
        for (int cell = 0; cell < mpeg1_cell_count(sb); ++cell) {
            const int v = resolved_value(b, gr, cell);
            if (v >= (1 << slen_of(gr == 1 && b.scfsi[group_of(cell)] && !sb.short_block ? b.gr[0] : sb, cell)))
                return false;
            if (!sa.active[cell]) continue;
            if (exponent(sa.short_block, sa.scalefac_scale, sa.preflag, cell, resolved_value(a, gr, cell)) !=
                exponent(sb.short_block, sb.scalefac_scale, sb.preflag, cell, v))
                return false;
        }
    }
    return true;
}

}  // namespace

int mpeg1_cell_count(const Mpeg1Scalefactors& s) { return s.short_block ? kShortCells : kLongCells; }

void mpeg1_read_scalefactors(BitstreamReader& reader, Mpeg1ScalefactorChannel& c, const int gr) {
    Mpeg1Scalefactors& s = c.gr[gr];
    for (int cell = 0; cell < mpeg1_cell_count(s); ++cell) {
        if (gr == 1 && !s.short_block && c.scfsi[group_of(cell)]) {
            s.values[cell] = c.gr[0].values[cell];
        } else {
            s.values[cell] = static_cast<uint8_t>(reader.read_bits(slen_of(s, cell)));
        }
    }
}

void mpeg1_write_scalefactors(BitstreamWriter& writer, const Mpeg1ScalefactorChannel& c, const int gr) {
    const Mpeg1Scalefactors& s = c.gr[gr];
    for (int cell = 0; cell < mpeg1_cell_count(s); ++cell) {
        if (gr == 1 && !s.short_block && c.scfsi[group_of(cell)]) continue;
        writer.write_bits(s.values[cell], slen_of(s, cell));
    }
}

int mpeg1_scalefactor_bits(const Mpeg1ScalefactorChannel& c, const int gr) {
    const Mpeg1Scalefactors& s = c.gr[gr];
    int bits = 0;
    for (int cell = 0; cell < mpeg1_cell_count(s); ++cell) {
        if (gr == 1 && !s.short_block && c.scfsi[group_of(cell)]) continue;
        bits += slen_of(s, cell);
    }
    return bits;
}

bool mpeg1_mark_active(Mpeg1Scalefactors& s, const Spectrum& coeffs, const uint32_t samplerate) {
    int sr = 0;
    switch (samplerate) {
        case 44100: sr = 0; break;
        case 48000: sr = 1; break;
        case 32000: sr = 2; break;
        default: return false;
    }
    const auto nonzero = [&coeffs](const int from, const int to) {
        for (int k = from; k < to; ++k) {
            if (coeffs[static_cast<size_t>(k)] != 0) return true;
        }
        return false;
    };
    std::fill(std::begin(s.active), std::end(s.active), false);
    if (s.short_block) {
        // short-block coefficients arrive band by band, and within a band window by window
        const int* edge = kShortEdges[sr];
        for (int band = 0; band < 12; ++band) {
            const int width = edge[band + 1] - edge[band];
            for (int win = 0; win < 3; ++win) {
                const int from = 3 * edge[band] + win * width;
                s.active[band * 3 + win] = nonzero(from, from + width);
            }
        }
    } else {
        for (int band = 0; band < kLongCells; ++band) {
            s.active[band] = nonzero(kLongEdges[sr][band], kLongEdges[sr][band + 1]);
        }
    }
    s.any_nonzero = nonzero(0, kNumCoefficients);
    return true;
}

bool mpeg1_recode_scalefactors(Mpeg1ScalefactorChannel& c, const bool allow_scfsi) {
    const bool joint = !c.gr[0].short_block && !c.gr[1].short_block;
    // granule 1 reusing a short granule 0 is read differently by every decoder
    if (!joint && (c.scfsi[0] || c.scfsi[1] || c.scfsi[2] || c.scfsi[3])) return false;

    const int before = mpeg1_scalefactor_bits(c, 0) + mpeg1_scalefactor_bits(c, 1);
    Candidate cand[2][4];
    const int count[2] = {candidates(c.gr[0], cand[0]), candidates(c.gr[1], cand[1])};
    Mpeg1ScalefactorChannel best = c;
    int best_bits = before;

    if (joint) {
        const unsigned patterns = allow_scfsi ? 16u : 1u;
        for (int i0 = 0; i0 < count[0]; ++i0) {
            for (int i1 = 0; i1 < count[1]; ++i1) {
                const Candidate& q0 = cand[0][i0];
                const Candidate& q1 = cand[1][i1];
                for (unsigned pattern = 0; pattern < patterns; ++pattern) {
                    uint8_t v0[kLongCells];
                    uint8_t v1[kLongCells];
                    bool ok = true;
                    for (int b = 0; b < kLongCells && ok; ++b) {
                        const bool a0 = c.gr[0].active[b];
                        const bool a1 = c.gr[1].active[b];
                        v0[b] = a0 ? q0.v[b] : 0;
                        v1[b] = a1 ? q1.v[b] : 0;
                        if (pattern & (1u << group_of(b))) {
                            // granule 1 hears granule 0's value; a free cell there takes the one it needs
                            if (a0 && a1 && v0[b] != v1[b]) ok = false;
                            if (!a0 && a1) v0[b] = v1[b];
                            v1[b] = v0[b];
                        }
                    }
                    if (!ok) continue;
                    int k0 = 0, k1 = 0;
                    const int bits = long_cost(v0, 0xFu, k0) + long_cost(v1, ~pattern & 0xFu, k1);
                    if (bits >= best_bits) continue;
                    best_bits = bits;
                    const Candidate* q[2] = {&q0, &q1};
                    const uint8_t* v[2] = {v0, v1};
                    const int k[2] = {k0, k1};
                    for (int gr = 0; gr < 2; ++gr) {
                        best.gr[gr].scalefac_scale = q[gr]->scale;
                        best.gr[gr].preflag = q[gr]->preflag;
                        best.gr[gr].scalefac_compress = static_cast<uint8_t>(k[gr]);
                        std::copy(v[gr], v[gr] + kLongCells, best.gr[gr].values);
                    }
                    for (int grp = 0; grp < 4; ++grp) best.scfsi[grp] = (pattern >> grp) & 1u;
                }
            }
        }
    } else {
        if (count[0] == 0 || count[1] == 0) return false;
        Mpeg1ScalefactorChannel trial = c;
        int bits = 0;
        for (int gr = 0; gr < 2; ++gr) {
            Mpeg1Scalefactors& s = trial.gr[gr];
            int gr_best = -1;
            for (int i = 0; i < count[gr]; ++i) {
                int k = 0;
                const int b = s.short_block ? short_cost(cand[gr][i].v, k) : long_cost(cand[gr][i].v, 0xFu, k);
                if (gr_best >= 0 && b >= gr_best) continue;
                gr_best = b;
                s.scalefac_scale = cand[gr][i].scale;
                s.preflag = cand[gr][i].preflag;
                s.scalefac_compress = static_cast<uint8_t>(k);
                std::copy(cand[gr][i].v, cand[gr][i].v + mpeg1_cell_count(s), s.values);
            }
            bits += gr_best;
        }
        if (bits < best_bits) {
            best_bits = bits;
            best = trial;
        }
    }

    if (best_bits >= before || !same_steps(c, best)) return false;
    c = best;
    return true;
}

}  // namespace mp3packer
