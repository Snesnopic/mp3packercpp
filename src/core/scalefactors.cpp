#include "scalefactors.hpp"

#include <algorithm>
#include <array>

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

// MPEG-2/2.5 bands (ISO 13818-3 B.2, and the 8 kHz tables every MPEG-2.5 decoder
// shares). 11025/12000 take 22050's long bands but 16000's short ones.
constexpr int kLsfLongEdges[3][23] = {
    {0, 6, 12, 18, 24, 30, 36, 44, 54, 66, 80, 96, 116, 140, 168, 200, 238, 284, 336, 396, 464, 522, 576},
    {0, 6, 12, 18, 24, 30, 36, 44, 54, 66, 80, 96, 114, 136, 162, 194, 232, 278, 332, 394, 464, 540, 576},
    {0, 12, 24, 36, 48, 60, 72, 88, 108, 132, 160, 192, 232, 280, 336, 400, 476, 566, 568, 570, 572, 574, 576},
};
constexpr int kLsfShortEdges[4][14] = {
    {0, 4, 8, 12, 18, 24, 32, 42, 56, 74, 100, 132, 174, 192},
    {0, 4, 8, 12, 18, 26, 36, 48, 62, 80, 104, 136, 180, 192},
    {0, 4, 8, 12, 18, 26, 36, 48, 62, 80, 104, 134, 174, 192},
    {0, 8, 16, 24, 36, 52, 72, 96, 124, 160, 162, 164, 166, 192},
};

// LSF partition sizes, indexed by [block category][row][partition] (ISO 13818-3
// table B.1). Block category is 0 for long blocks, 1 for pure short blocks and 2
// for mixed blocks.
constexpr int kLsfCounts[3][6][4] = {
    {{6, 5, 5, 5}, {6, 5, 7, 3}, {11, 10, 0, 0}, {7, 7, 7, 0}, {6, 6, 6, 3}, {8, 8, 5, 0}},
    {{9, 9, 9, 9}, {9, 9, 12, 6}, {18, 18, 0, 0}, {12, 12, 12, 0}, {12, 9, 9, 6}, {15, 12, 9, 0}},
    {{6, 9, 9, 9}, {6, 9, 12, 6}, {15, 18, 0, 0}, {6, 15, 12, 0}, {6, 12, 9, 6}, {6, 18, 9, 0}},
};
// the largest slen each partition of rows 0..2 can declare
constexpr int kLsfSlenLimit[3][4] = {{4, 4, 3, 3}, {4, 4, 3, 0}, {3, 2, 0, 0}};

// The LSF scalefac_compress field is a compound index selecting both a row of
// partition sizes and the slen used within each partition. Each entry packs four
// 3-bit lengths in bits 0..11 and the row in bits 12..14.
struct LsfSlenTables {
    std::array<uint32_t, 512> normal{};
    std::array<uint32_t, 256> intensity{};
};

// The six ranges map one to one onto the six rows of kLsfCounts, in order:
// normal 0..399, 400..499, 500..511 (the last is how LSF signals preflag), then
// intensity 0..179, 180..243, 244..255. They used to be numbered 0, 1, 1, 2, 3, 4,
// so everything from 500 on took the row before its own: -z sized those
// scalefactors wrongly, decoded the spectrum from the wrong bit and re-encoded the
// garbage, and the granule round trip — self-consistent — let it through. LAME never
// emits preflag or intensity stereo in LSF, which is why no corpus caught it, and
// go-mp3packer carries the same table. Rows checked against ISO 13818-3 and
// minimp3's partition walk.
const LsfSlenTables& lsf_slen_tables() {
    static const LsfSlenTables tables = [] {
        LsfSlenTables t;
        for (int i = 0; i < 5; ++i)
            for (int j = 0; j < 5; ++j)
                for (int k = 0; k < 4; ++k)
                    for (int l = 0; l < 4; ++l)
                        t.normal[static_cast<size_t>(l + k * 4 + j * 16 + i * 80)] =
                            static_cast<uint32_t>(i | j << 3 | k << 6 | l << 9);
        for (int i = 0; i < 5; ++i)
            for (int j = 0; j < 5; ++j)
                for (int k = 0; k < 4; ++k)
                    t.normal[static_cast<size_t>(k + j * 4 + i * 20 + 400)] =
                        static_cast<uint32_t>(i | j << 3 | k << 6 | 1 << 12);
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 3; ++j)
                t.normal[static_cast<size_t>(j + i * 3 + 500)] = static_cast<uint32_t>(i | j << 3 | 2 << 12);
        for (int i = 0; i < 5; ++i)
            for (int j = 0; j < 6; ++j)
                for (int k = 0; k < 6; ++k)
                    t.intensity[static_cast<size_t>(k + j * 6 + i * 36)] =
                        static_cast<uint32_t>(i | j << 3 | k << 6 | 3 << 12);
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                for (int k = 0; k < 4; ++k)
                    t.intensity[static_cast<size_t>(k + j * 4 + i * 16 + 180)] =
                        static_cast<uint32_t>(i | j << 3 | k << 6 | 4 << 12);
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 3; ++j)
                t.intensity[static_cast<size_t>(j + i * 3 + 244)] = static_cast<uint32_t>(i | j << 3 | 5 << 12);
        return t;
    }();
    return tables;
}

int group_of(const int band) { return band < 6 ? 0 : band < 11 ? 1 : band < 16 ? 2 : 3; }

int first_slen2_cell(const bool short_block) { return short_block ? 18 : 11; }

int slen_of(const Mpeg1Scalefactors& s, const int cell) {
    return cell < first_slen2_cell(s.short_block) ? kSlen1[s.scalefac_compress & 15]
                                                  : kSlen2[s.scalefac_compress & 15];
}

bool preflag_of(const Mpeg1Scalefactors& s) { return s.preflag; }
bool preflag_of(const LsfScalefactors& s) { return s.scalefac_compress >= 500; }

// the largest value any coding could give a cell; the cost function decides the rest
int value_limit(const Mpeg1Scalefactors& s, const int cell) { return cell < first_slen2_cell(s.short_block) ? 15 : 7; }
int value_limit(const LsfScalefactors&, int) { return 15; }

int cell_count(const Mpeg1Scalefactors& s) { return mpeg1_cell_count(s); }
int cell_count(const LsfScalefactors& s) { return lsf_cell_count(s); }

int lsf_category(const LsfScalefactors& s) { return s.short_block ? 1 : 0; }

// The part of a cell's step exponent, in quarter steps, that the scalefactors and
// subblock_gain carry. Every decoder sums the two terms as integers before it builds
// the step, so any split of the same total is heard the same.
int exponent(const bool short_block, const bool scale, const bool preflag, const uint8_t* subblock_gain,
             const int cell, const int value) {
    if (short_block) return (subblock_gain[cell % 3] << 3) + (value << (scale ? 2 : 1));
    return (value + (preflag ? kPretab[cell] : 0)) << (scale ? 2 : 1);
}

template <class S>
int exponent(const S& s, const int cell, const int value) {
    return exponent(s.short_block, s.scalefac_scale, preflag_of(s), s.subblock_gain, cell, value);
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

int bit_length(const int max) {
    int n = 0;
    while ((1 << n) <= max) ++n;
    return n;
}

// the cheapest LSF scalefac_compress that holds v with preflag as given, or -1
int lsf_cheapest(const uint8_t* v, const int category, const bool preflag, int& bits) {
    int best = -1;
    for (int row = preflag ? 2 : 0; row < (preflag ? 3 : 2); ++row) {
        int slen[4] = {};
        int b = 0, cell = 0;
        bool fits = true;
        for (int p = 0; p < 4; ++p) {
            int max = 0;
            for (int k = 0; k < kLsfCounts[category][row][p]; ++k) max = std::max<int>(max, v[cell++]);
            slen[p] = bit_length(max);
            fits = fits && slen[p] <= kLsfSlenLimit[row][p];
            b += slen[p] * kLsfCounts[category][row][p];
        }
        if (!fits) continue;
        const int sfc = row == 0   ? ((slen[0] * 5 + slen[1]) << 4) + (slen[2] << 2) + slen[3]
                        : row == 1 ? 400 + ((slen[0] * 5 + slen[1]) << 2) + slen[2]
                                   : 500 + slen[0] * 3 + slen[1];
        if (best < 0 || b < bits) {
            best = sfc;
            bits = b;
        }
    }
    return best;
}

// Which cells hold a nonzero coefficient, given the band edges; returns whether any
// coefficient at all is nonzero.
bool mark_cells(const bool short_block, const int* long_edges, const int* short_edges, const Spectrum& coeffs,
                bool* active, bool* last_band_active) {
    const auto nonzero = [&coeffs](const int from, const int to) {
        for (int k = from; k < to; ++k) {
            if (coeffs[static_cast<size_t>(k)] != 0) return true;
        }
        return false;
    };
    std::fill(active, active + kMaxScalefactorCells, false);
    std::fill(last_band_active, last_band_active + 3, false);
    if (short_block) {
        // short-block coefficients arrive band by band, and within a band window by window
        const int* edge = short_edges;
        for (int band = 0; band < 12; ++band) {
            const int width = edge[band + 1] - edge[band];
            for (int win = 0; win < 3; ++win) {
                const int from = 3 * edge[band] + win * width;
                active[band * 3 + win] = nonzero(from, from + width);
            }
        }
        const int width = edge[13] - edge[12];
        for (int win = 0; win < 3; ++win) {
            const int from = 3 * edge[12] + win * width;
            last_band_active[win] = nonzero(from, from + width);
        }
    } else {
        for (int band = 0; band < kLongCells; ++band) {
            active[band] = nonzero(long_edges[band], long_edges[band + 1]);
        }
    }
    return nonzero(0, kNumCoefficients);
}

struct Candidate {
    bool scale = false;
    bool preflag = false;
    uint8_t subblock_gain[3] = {};
    uint8_t v[kMaxScalefactorCells] = {};  // inactive cells left at 0
};

// every scalefac_scale/preflag pair under which each active cell keeps its exponent
template <class S>
int candidates(const S& s, Candidate out[4]) {
    const int cells = cell_count(s);
    int e[kMaxScalefactorCells] = {};
    for (int c = 0; c < cells; ++c) {
        if (s.active[c]) e[c] = exponent(s, c, s.values[c]);
    }
    int n = 0;
    for (int scale = 0; scale < 2; ++scale) {
        for (int pf = 0; pf < 2; ++pf) {
            // short blocks ignore preflag: the bit stays as it came
            if (s.short_block && pf == 1) continue;
            Candidate q;
            q.scale = scale != 0;
            q.preflag = s.short_block ? preflag_of(s) : pf != 0;
            std::copy(s.subblock_gain, s.subblock_gain + 3, q.subblock_gain);
            if (s.short_block) {
                // each window's subblock_gain takes as much of its heard cells' exponents
                // as all of them allow: that only lowers what is left to the scalefactors
                for (int w = 0; w < 3; ++w) {
                    if (s.last_band_active[w]) continue;
                    int gain = 7;
                    bool heard = false;
                    for (int c = w; c < cells; c += 3) {
                        if (!s.active[c]) continue;
                        heard = true;
                        gain = std::min(gain, e[c] >> 3);
                    }
                    if (heard) q.subblock_gain[w] = static_cast<uint8_t>(gain);
                }
            }
            const int shift = scale ? 2 : 1;
            bool ok = true;
            for (int c = 0; c < cells && ok; ++c) {
                if (!s.active[c]) continue;
                const int rest = e[c] - (s.short_block ? q.subblock_gain[c % 3] << 3 : 0);
                const int pre = (!s.short_block && q.preflag) ? kPretab[c] : 0;
                const int v = (rest >> shift) - pre;
                ok = rest >= 0 && (rest & ((1 << shift) - 1)) == 0 && v >= 0 && v <= value_limit(s, c);
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
        for (int w = 0; w < 3; ++w) {
            if (sa.short_block && sa.last_band_active[w] && sa.subblock_gain[w] != sb.subblock_gain[w]) return false;
        }
        for (int cell = 0; cell < mpeg1_cell_count(sb); ++cell) {
            const int v = resolved_value(b, gr, cell);
            if (v >= (1 << slen_of(gr == 1 && b.scfsi[group_of(cell)] && !sb.short_block ? b.gr[0] : sb, cell)))
                return false;
            if (!sa.active[cell]) continue;
            if (exponent(sa, cell, resolved_value(a, gr, cell)) != exponent(sb, cell, v)) return false;
        }
    }
    return true;
}

bool same_steps(const LsfScalefactors& a, const LsfScalefactors& b) {
    if (a.short_block != b.short_block) return false;
    if (a.short_block && preflag_of(a) != preflag_of(b)) return false;
    for (int w = 0; w < 3; ++w) {
        if (a.short_block && a.last_band_active[w] && a.subblock_gain[w] != b.subblock_gain[w]) return false;
    }
    const LsfPartition p = lsf_partition(b.scalefac_compress, false, lsf_category(b));
    int cell = 0;
    for (int part = 0; part < 4; ++part) {
        for (int k = 0; k < p.count[part]; ++k, ++cell) {
            if (b.values[cell] >= (1 << p.slen[part])) return false;
            if (a.active[cell] && exponent(a, cell, a.values[cell]) != exponent(b, cell, b.values[cell])) return false;
        }
    }
    return cell == lsf_cell_count(b);
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
    s.any_nonzero = mark_cells(s.short_block, kLongEdges[sr], kShortEdges[sr], coeffs, s.active, s.last_band_active);
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
                        std::copy(q[gr]->subblock_gain, q[gr]->subblock_gain + 3, best.gr[gr].subblock_gain);
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
                std::copy(cand[gr][i].subblock_gain, cand[gr][i].subblock_gain + 3, s.subblock_gain);
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

LsfPartition lsf_partition(const int scalefac_compress, const bool intensity, const int category) {
    const auto& tables = lsf_slen_tables();
    uint32_t packed = intensity ? tables.intensity[static_cast<size_t>(scalefac_compress >> 1) & 0xFF]
                                : tables.normal[static_cast<size_t>(scalefac_compress) & 0x1FF];
    LsfPartition p;
    const int* counts = kLsfCounts[category][(packed >> 12) & 7];
    for (int i = 0; i < 4; ++i) {
        p.slen[i] = static_cast<int>(packed & 7);
        p.count[i] = counts[i];
        packed >>= 3;
    }
    return p;
}

int lsf_cell_count(const LsfScalefactors& s) { return s.short_block ? kShortCells : kLongCells; }

void lsf_read_scalefactors(BitstreamReader& reader, LsfScalefactors& s) {
    const LsfPartition p = lsf_partition(s.scalefac_compress, false, lsf_category(s));
    int cell = 0;
    for (int part = 0; part < 4; ++part) {
        for (int k = 0; k < p.count[part]; ++k) s.values[cell++] = static_cast<uint8_t>(reader.read_bits(p.slen[part]));
    }
}

void lsf_write_scalefactors(BitstreamWriter& writer, const LsfScalefactors& s) {
    const LsfPartition p = lsf_partition(s.scalefac_compress, false, lsf_category(s));
    int cell = 0;
    for (int part = 0; part < 4; ++part) {
        for (int k = 0; k < p.count[part]; ++k) writer.write_bits(s.values[cell++], p.slen[part]);
    }
}

int lsf_scalefactor_bits(const LsfScalefactors& s) {
    const LsfPartition p = lsf_partition(s.scalefac_compress, false, lsf_category(s));
    int bits = 0;
    for (int part = 0; part < 4; ++part) bits += p.slen[part] * p.count[part];
    return bits;
}

bool lsf_mark_active(LsfScalefactors& s, const Spectrum& coeffs, const uint32_t samplerate) {
    int lr = 0, sr = 0;
    switch (samplerate) {
        case 22050: lr = 0; sr = 0; break;
        case 24000: lr = 1; sr = 1; break;
        case 16000:
        case 11025:
        case 12000: lr = 0; sr = 2; break;
        case 8000: lr = 2; sr = 3; break;
        default: return false;
    }
    mark_cells(s.short_block, kLsfLongEdges[lr], kLsfShortEdges[sr], coeffs, s.active, s.last_band_active);
    return true;
}

bool lsf_recode_scalefactors(LsfScalefactors& s) {
    const int before = lsf_scalefactor_bits(s);
    Candidate cand[4];
    const int count = candidates(s, cand);
    LsfScalefactors best = s;
    int best_bits = before;
    for (int i = 0; i < count; ++i) {
        int bits = 0;
        const int sfc = lsf_cheapest(cand[i].v, lsf_category(s), cand[i].preflag, bits);
        if (sfc < 0 || bits >= best_bits) continue;
        best_bits = bits;
        best.scalefac_scale = cand[i].scale;
        best.scalefac_compress = static_cast<uint16_t>(sfc);
        std::copy(cand[i].subblock_gain, cand[i].subblock_gain + 3, best.subblock_gain);
        std::copy(cand[i].v, cand[i].v + lsf_cell_count(s), best.values);
    }
    if (best_bits >= before || !same_steps(s, best)) return false;
    s = best;
    return true;
}

}  // namespace mp3packer
