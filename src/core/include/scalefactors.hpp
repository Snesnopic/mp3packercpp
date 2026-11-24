#ifndef MP3PACKERCPP_SCALEFACTORS_HPP
#define MP3PACKERCPP_SCALEFACTORS_HPP

#include "bitstream.hpp"
#include "huffman.hpp"
#include <cstdint>

namespace mp3packer {

// 12 short bands x 3 windows
constexpr int kMaxScalefactorCells = 36;

// One MPEG-1 granule's scalefactors as a decoder resolves them. Mixed blocks are
// not represented: the packer copies them through untouched.
struct Mpeg1Scalefactors {
    bool short_block = false;      // block_type 2: band * 3 + window, else 21 long bands
    bool scalefac_scale = false;
    bool preflag = false;          // decoders read it on long blocks only
    uint8_t scalefac_compress = 0;
    uint8_t values[kMaxScalefactorCells] = {};
    bool active[kMaxScalefactorCells] = {};     // the cell holds a nonzero coefficient
    bool any_nonzero = false;
};

struct Mpeg1ScalefactorChannel {
    Mpeg1Scalefactors gr[2];
    bool scfsi[4] = {};            // granule 1 reuses granule 0's values for this group
};

int mpeg1_cell_count(const Mpeg1Scalefactors& s);
void mpeg1_read_scalefactors(BitstreamReader& reader, Mpeg1ScalefactorChannel& c, int gr);
void mpeg1_write_scalefactors(BitstreamWriter& writer, const Mpeg1ScalefactorChannel& c, int gr);
int mpeg1_scalefactor_bits(const Mpeg1ScalefactorChannel& c, int gr);

// false for a sample rate MPEG-1 does not have
bool mpeg1_mark_active(Mpeg1Scalefactors& s, const Spectrum& coeffs, uint32_t samplerate);

// Re-codes a channel's scalefactors into the fewest bits. Every active cell keeps
// its exponent as the same integer, (value + preflag * pretab) << (scalefac_scale + 1),
// and global_gain and subblock_gain never change, so a decoder that builds the step
// from two float factors (minimp3) outputs the same PCM as one that builds it from
// the integer (ffmpeg, mpg123). scfsi is chosen only with both granules long and
// allow_scfsi. Leaves c untouched and returns false unless bits are saved.
bool mpeg1_recode_scalefactors(Mpeg1ScalefactorChannel& c, bool allow_scfsi);

}  // namespace mp3packer

#endif
