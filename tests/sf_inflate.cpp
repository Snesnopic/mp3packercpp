// Fixture maker for the scalefactor re-coding. Every scalefactor cell that no decoder
// hears (all of its coefficients zero, in every granule that reads it) gets the
// largest value its field holds, in place, so the file decodes exactly as before
// while its scalefactors are as far from the cheapest coding as they can get without
// moving a bit. MPEG-1 and MPEG-2/2.5 alike, except that --intensity is MPEG-1 only
// (an LSF right channel reads scalefac_compress differently under intensity stereo,
// so the bit cannot be set without moving bits). With --intensity the joint-stereo
// frames also get the intensity stereo bit, and the right channel's free cells become
// intensity positions (1..6), which a repack must keep as they are. With --subblock
// nothing is filled: instead
// each short-block window moves as much of its subblock_gain into its heard cells'
// scalefactors as their fields hold, or, with no subblock_gain to move, as much of
// what all its heard cells share into subblock_gain — 8 quarter steps per unit either
// way, which a decoder that sums the two terms as one integer hears the same.
// usage: sf_inflate IN OUT [--intensity | --subblock]; prints the cells written and
// how many of them short, or for --subblock the windows moved and the cells rewritten
#include "huffman.hpp"
#include "mp3_reader.hpp"
#include "scalefactors.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

using namespace mp3packer;

namespace {

const int kSlen1[16] = {0, 0, 0, 0, 3, 1, 1, 1, 2, 2, 2, 3, 3, 3, 4, 4};
const int kSlen2[16] = {0, 1, 2, 3, 0, 1, 2, 3, 1, 2, 3, 1, 2, 3, 2, 3};

int group_of(int b) { return b < 6 ? 0 : b < 11 ? 1 : b < 16 ? 2 : 3; }

size_t scalefactor_bits(const GrChInfo& g, bool gr1, const bool scfsi[4]) {
    const size_t s1 = static_cast<size_t>(kSlen1[g.scalefac_compress & 15]);
    const size_t s2 = static_cast<size_t>(kSlen2[g.scalefac_compress & 15]);
    if (g.window_switching_flag && g.block_type == 2) return (g.mixed_block_flag ? 17 : 18) * s1 + 18 * s2;
    size_t bits = 0;
    const size_t width[4] = {6 * s1, 5 * s1, 5 * s2, 5 * s2};
    for (int grp = 0; grp < 4; ++grp)
        if (!(gr1 && scfsi[grp])) bits += width[grp];
    return bits;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: sf_inflate IN OUT [--intensity | --subblock]\n");
        return 2;
    }
    const std::string mode = argc > 3 ? argv[3] : "";
    const bool intensity = mode == "--intensity";
    const bool subblock = mode == "--subblock";
    Mp3Reader reader(argv[1]);
    std::vector<Mp3Frame> frames;
    while (auto f = reader.read_next_frame()) frames.push_back(*f);
    std::vector<uint8_t> file = reader.data();

    // the reservoir as one stream, and where each of its bytes sits in the file
    std::vector<uint8_t> res;
    std::vector<size_t> where, starts;
    for (const auto& f : frames) {
        if (f.main_data_avail < f.main_data_size) break;
        starts.push_back(res.size());
        for (size_t k = 0; k < f.main_data_size; ++k) {
            res.push_back(file[f.main_data_offset + k]);
            where.push_back(f.main_data_offset + k);
        }
    }
    const auto put = [&](size_t bit, int width, uint32_t value) {
        for (int k = 0; k < width; ++k, ++bit) {
            const uint8_t mask = static_cast<uint8_t>(0x80 >> (bit & 7));
            uint8_t& byte = file[where[bit >> 3]];
            byte = ((value >> (width - 1 - k)) & 1) ? (byte | mask) : (byte & ~mask);
        }
    };

    const auto put_file = [&](size_t bit, int width, uint32_t value) {
        for (int k = 0; k < width; ++k, ++bit) {
            const uint8_t mask = static_cast<uint8_t>(0x80 >> (bit & 7));
            uint8_t& byte = file[bit >> 3];
            byte = ((value >> (width - 1 - k)) & 1) ? (byte | mask) : (byte & ~mask);
        }
    };

    long written = 0, written_short = 0;
    // moves one short granule's steps between subblock_gain and its heard cells, given
    // each cell's width, where its scalefactors start and where its subblock_gain sits
    const auto move_subblock = [&](const bool* active, const uint8_t* values, const bool* last_band_active,
                                   const int* width, const uint8_t* subblock_gain, const bool scale,
                                   const size_t sf_bit, const size_t gain_bit) {
        const int unit = scale ? 2 : 4;   // scalefactor steps per subblock_gain step
        for (int w = 0; w < 3; ++w) {
            // band 12 has no scalefactor to take the difference
            if (last_band_active[w]) continue;
            // d > 0 moves d units out of subblock_gain, d < 0 into it
            int d = subblock_gain[w];
            for (; d > 0; --d) {
                bool fits = true;
                for (int cell = w; cell < 36 && fits; cell += 3) fits = !active[cell] || values[cell] + d * unit < (1 << width[cell]);
                if (fits) break;
            }
            if (subblock_gain[w] == 0) {
                int shared = 7;
                bool heard = false;
                for (int cell = w; cell < 36; cell += 3) {
                    if (!active[cell]) continue;
                    heard = true;
                    shared = std::min(shared, values[cell] / unit);
                }
                d = heard ? -shared : 0;
            }
            if (d == 0) continue;
            size_t bit = sf_bit;
            for (int cell = 0; cell < 36; ++cell) {
                if (cell % 3 == w && active[cell]) {
                    put(bit, width[cell], static_cast<uint32_t>(values[cell] + d * unit));
                    ++written_short;
                }
                bit += static_cast<size_t>(width[cell]);
            }
            put_file(gain_bit + static_cast<size_t>(3 * w), 3, static_cast<uint32_t>(subblock_gain[w] - d));
            ++written;
        }
    };
    BitstreamReader r(res);
    for (size_t i = 0; i < starts.size(); ++i) {
        Mp3Frame& f = frames[i];
        // the tag frame holds no audio, and on a CRC file its side info is the tag
        if (i == 0 && f.frame_offset + 64 <= file.size()) {
            const uint8_t* p = file.data() + f.frame_offset;
            bool tag = false;
            for (int k = 4; k < 48 && !tag; ++k) tag = !memcmp(p + k, "Xing", 4) || !memcmp(p + k, "Info", 4);
            if (tag) continue;
        }
        const int n_ch = f.header.channel_mode == ChannelMode::Mono ? 1 : 2;
        if (f.header.version != MpegVersion::MPEG1) {
            if (intensity || starts[i] < f.side_info.main_data_begin) continue;
            r.seek_bit((starts[i] - f.side_info.main_data_begin) * 8);
            const size_t side = (f.frame_offset + 4 + (f.header.has_crc ? 2 : 0)) * 8 + 8 + (n_ch == 1 ? 1 : 2);
            for (int ch = 0; ch < n_ch; ++ch) {
                const GrChInfo& g = f.side_info.gr[0][ch];
                const size_t start = r.tell_bit();
                const bool positions = f.header.is_stereo && ch == 1;
                const int cat = g.window_switching_flag && g.block_type == 2 ? (g.mixed_block_flag ? 2 : 1) : 0;
                const LsfPartition part = lsf_partition(g.scalefac_compress, positions, cat);
                size_t sf = 0;
                for (int k = 0; k < 4; ++k) sf += static_cast<size_t>(part.slen[k] * part.count[k]);
                if (start + g.part2_3_length > res.size() * 8 || sf > g.part2_3_length) {
                    r.seek_bit(start + g.part2_3_length);
                    continue;
                }
                LsfScalefactors s;
                s.short_block = cat == 1;
                s.scalefac_compress = g.scalefac_compress;
                if (cat != 2 && !positions) lsf_read_scalefactors(r, s);
                r.seek_bit(start + sf);
                Spectrum coeffs{};
                const HuffmanConfig cfg = {g.region0_count, g.region1_count, g.big_values, g.table_select[0],
                                           g.table_select[1], g.table_select[2], g.count1table_select,
                                           g.window_switching_flag, g.block_type, g.mixed_block_flag};
                HuffmanOptimizer::decode_quantized_coefficients(coeffs, cfg, r, f.header.samplerate,
                                                                static_cast<int>(g.part2_3_length - sf));
                r.seek_bit(start + g.part2_3_length);
                if (cat == 2 || positions || !lsf_mark_active(s, coeffs, f.header.samplerate)) continue;
                int width[36] = {};
                for (int k = 0, cell = 0; k < 4; ++k)
                    for (int n = 0; n < part.count[k]; ++n) width[cell++] = part.slen[k];
                if (subblock) {
                    if (s.short_block)
                        move_subblock(s.active, s.values, s.last_band_active, width, g.subblock_gain, g.scalefac_scale,
                                      start, side + static_cast<size_t>(63 * ch) + 52);
                    continue;
                }
                size_t bit = start;
                for (int cell = 0; cell < lsf_cell_count(s); ++cell) {
                    if (!s.active[cell] && width[cell] > 0) {
                        put(bit, width[cell], (1u << width[cell]) - 1);
                        ++written;
                        if (s.short_block) ++written_short;
                    }
                    bit += static_cast<size_t>(width[cell]);
                }
            }
            continue;
        }
        const bool joint = f.header.channel_mode == ChannelMode::JointStereo;
        if (intensity && joint) file[f.frame_offset + 3] |= 0x10;
        if (starts[i] < f.side_info.main_data_begin) continue;
        r.seek_bit((starts[i] - f.side_info.main_data_begin) * 8);

        Mpeg1ScalefactorChannel chans[2];
        Spectrum spectra[2][2]{};
        size_t sf_start[2][2] = {};
        bool usable[2] = {true, true};
        for (int gr = 0; gr < 2; ++gr) {
            for (int ch = 0; ch < n_ch; ++ch) {
                const GrChInfo& g = f.side_info.gr[gr][ch];
                const size_t start = r.tell_bit();
                const size_t sf = scalefactor_bits(g, gr == 1, f.side_info.scfsi[ch]);
                sf_start[gr][ch] = start;
                if (start + g.part2_3_length > res.size() * 8 || sf > g.part2_3_length) {
                    usable[ch] = false;
                    r.seek_bit(start + g.part2_3_length);
                    continue;
                }
                const bool mixed = g.window_switching_flag && g.block_type == 2 && g.mixed_block_flag;
                if (mixed) {
                    usable[ch] = false;
                } else {
                    Mpeg1Scalefactors& s = chans[ch].gr[gr];
                    s.short_block = g.window_switching_flag && g.block_type == 2;
                    s.scalefac_compress = static_cast<uint8_t>(g.scalefac_compress & 15);
                    if (gr == 0)
                        for (int k = 0; k < 4; ++k) chans[ch].scfsi[k] = f.side_info.scfsi[ch][k];
                    mpeg1_read_scalefactors(r, chans[ch], gr);
                }
                r.seek_bit(start + sf);
                const HuffmanConfig cfg = {g.region0_count, g.region1_count, g.big_values, g.table_select[0],
                                           g.table_select[1], g.table_select[2], g.count1table_select,
                                           g.window_switching_flag, g.block_type, g.mixed_block_flag};
                HuffmanOptimizer::decode_quantized_coefficients(spectra[gr][ch], cfg, r, f.header.samplerate,
                                                                static_cast<int>(g.part2_3_length - sf));
                r.seek_bit(start + g.part2_3_length);
            }
        }

        for (int ch = 0; ch < n_ch; ++ch) {
            if (!usable[ch]) continue;
            // already intensity positions in the input: not free
            if (f.header.is_stereo && ch == 1) continue;
            Mpeg1ScalefactorChannel& c = chans[ch];
            if (!mpeg1_mark_active(c.gr[0], spectra[0][ch], f.header.samplerate) ||
                !mpeg1_mark_active(c.gr[1], spectra[1][ch], f.header.samplerate))
                continue;
            const bool both_long = !c.gr[0].short_block && !c.gr[1].short_block;
            const bool any_scfsi = c.scfsi[0] || c.scfsi[1] || c.scfsi[2] || c.scfsi[3];
            if (any_scfsi && !both_long) continue;
            if (subblock) {
                for (int gr = 0; gr < 2; ++gr) {
                    const Mpeg1Scalefactors& s = c.gr[gr];
                    const GrChInfo& g = f.side_info.gr[gr][ch];
                    if (!s.short_block) continue;
                    const int s1 = kSlen1[s.scalefac_compress & 15], s2 = kSlen2[s.scalefac_compress & 15];
                    int width[36];
                    for (int cell = 0; cell < 36; ++cell) width[cell] = cell < 18 ? s1 : s2;
                    const size_t side = (f.frame_offset + 4 + (f.header.has_crc ? 2 : 0)) * 8 + 9 +
                                        (n_ch == 1 ? 5 : 3) + static_cast<size_t>(4 * n_ch) +
                                        static_cast<size_t>(59 * (gr * n_ch + ch)) + 47;
                    move_subblock(s.active, s.values, s.last_band_active, width, g.subblock_gain, g.scalefac_scale,
                                  sf_start[gr][ch], side);
                }
                continue;
            }
            const bool positions = intensity && joint && ch == 1;
            for (int gr = 0; gr < 2; ++gr) {
                const Mpeg1Scalefactors& s = c.gr[gr];
                const int s1 = kSlen1[s.scalefac_compress & 15], s2 = kSlen2[s.scalefac_compress & 15];
                size_t bit = sf_start[gr][ch];
                for (int cell = 0; cell < mpeg1_cell_count(s); ++cell) {
                    const int width = cell < (s.short_block ? 18 : 11) ? s1 : s2;
                    if (gr == 1 && !s.short_block && c.scfsi[group_of(cell)]) continue;
                    bool heard = s.active[cell];
                    if (gr == 0 && both_long && c.scfsi[group_of(cell)]) heard = heard || c.gr[1].active[cell];
                    if (!heard && width > 0) {
                        const uint32_t full = (1u << width) - 1;
                        const uint32_t v = positions ? std::min<uint32_t>(full, 1 + static_cast<uint32_t>(cell % 6)) : full;
                        put(bit, width, v);
                        ++written;
                        if (s.short_block) ++written_short;
                    }
                    bit += static_cast<size_t>(width);
                }
            }
        }
    }

    std::ofstream out(argv[2], std::ios::binary);
    out.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
    printf("%ld %ld\n", written, written_short);
    return 0;
}
