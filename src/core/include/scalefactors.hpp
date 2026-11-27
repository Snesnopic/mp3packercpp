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
    uint8_t subblock_gain[3] = {}; // short blocks: (gain << 3) joins each cell of its window
    uint8_t scalefac_compress = 0;
    uint8_t values[kMaxScalefactorCells] = {};
    bool active[kMaxScalefactorCells] = {};     // the cell holds a nonzero coefficient
    bool last_band_active[3] = {};  // short: band 12 has no scalefactor, so a window whose
                                    // band 12 is heard must keep its subblock_gain
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
// its exponent as the same integer — (value + preflag * pretab) << (scalefac_scale + 1)
// on long blocks, (subblock_gain << 3) + (value << (scalefac_scale + 1)) on short ones —
// and global_gain never changes, so a decoder that builds the step from two float
// factors (minimp3) outputs the same PCM as one that builds it from the integer
// (ffmpeg, mpg123). scfsi is chosen only with both granules long and allow_scfsi.
// Leaves c untouched and returns false unless bits are saved.
bool mpeg1_recode_scalefactors(Mpeg1ScalefactorChannel& c, bool allow_scfsi);

// The four partitions an MPEG-2/2.5 scalefac_compress selects: each one's slen and
// how many cells it holds (ISO 13818-3 2.4.3.2). category: 0 long, 1 short, 2 mixed;
// intensity: the right channel under intensity stereo, whose field reads differently.
struct LsfPartition {
    int slen[4] = {};
    int count[4] = {};
};
LsfPartition lsf_partition(int scalefac_compress, bool intensity, int category);

// One MPEG-2/2.5 granule's scalefactors: one granule per frame, no scfsi, preflag
// is scalefac_compress 500 and up. Never the right channel under intensity stereo,
// and never a mixed block.
struct LsfScalefactors {
    bool short_block = false;
    bool scalefac_scale = false;
    uint8_t subblock_gain[3] = {};
    uint16_t scalefac_compress = 0;
    uint8_t values[kMaxScalefactorCells] = {};
    bool active[kMaxScalefactorCells] = {};
    bool last_band_active[3] = {};
};

int lsf_cell_count(const LsfScalefactors& s);
void lsf_read_scalefactors(BitstreamReader& reader, LsfScalefactors& s);
void lsf_write_scalefactors(BitstreamWriter& writer, const LsfScalefactors& s);
int lsf_scalefactor_bits(const LsfScalefactors& s);

// false for a sample rate MPEG-2/2.5 does not have
bool lsf_mark_active(LsfScalefactors& s, const Spectrum& coeffs, uint32_t samplerate);

// As mpeg1_recode_scalefactors, for one LSF granule: scalefac_scale, preflag (long
// blocks), subblock_gain (short blocks), free cells and scalefac_compress. A short
// block keeps whether it is in the preflag range, which decoders ignore there.
bool lsf_recode_scalefactors(LsfScalefactors& s);

}  // namespace mp3packer

#endif
