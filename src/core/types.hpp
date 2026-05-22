#ifndef MP3PACKERCPP_TYPES_HPP
#define MP3PACKERCPP_TYPES_HPP

#include <vector>

namespace mp3packer {

/**
 * @brief Specifies the MPEG audio version.
 */
enum class MpegVersion { MPEG1, MPEG2, MPEG25 };

/**
 * @brief Specifies the audio channel mode.
 */
enum class ChannelMode { Stereo, JointStereo, DualChannel, Mono };

/**
 * @brief Specifies the emphasis mode used for the audio.
 */
enum class Emphasis { None, _50_15, Reserved, CCITT };

/**
 * @brief Holds physical and logical information about a specific bitrate configuration.
 */
struct BitrateInfo {
    int bitrate_kbps;  ///< Nominal bitrate in kilobits per second
    int frame_size;    ///< Size of the frame in bytes (calculated including padding)
    int data_size;     ///< Size of the data payload excluding headers
    bool padding;      ///< True if padding bit is set for this frame
    int index;         ///< Index of the bitrate in the MPEG bitrate table
};

/**
 * @brief Represents the decoded 32-bit MPEG audio frame header.
 */
struct Mp3Header {
    MpegVersion version;       ///< MPEG version
    bool has_crc;              ///< True if a CRC check follows the header
    BitrateInfo bitrate;       ///< Information about frame bitrate and sizes
    int samplerate;            ///< Sampling frequency in Hz
    bool padding;              ///< Padding bit
    bool private_bit;          ///< Private bit for application-specific triggers
    ChannelMode channel_mode;  ///< Audio channel mode
    bool ms_stereo;            ///< True if Middle/Side stereo is enabled (Joint Stereo)
    bool is_stereo;            ///< True if Intensity stereo is enabled (Joint Stereo)
    bool copyright;            ///< Copyright bit
    bool original;             ///< Original/Copy bit
    Emphasis emphasis;         ///< Emphasis indicator
};

/**
 * @brief Decoding information for a specific granule and channel.
 */
struct GrChInfo {
    int part2_3_length = 0;        ///< Length of scalefactors and main data in bits
    int big_values = 0;            ///< Number of large spectral values divided by 2
    int global_gain = 0;           ///< Quantization step size
    int scalefac_compress = 0;     ///< Defines the number of bits used for scalefactors
    int window_switching_flag = 0; ///< True if block type is not normal (0)
    int block_type = 0;            ///< 0: normal, 1: start, 2: short, 3: end
    int mixed_block_flag = 0;      ///< True if low frequencies are long blocks and high are short
    int table_select[3] = {0, 0, 0}; ///< Huffman table indices for the three regions
    int subblock_gain[3] = {0, 0, 0};///< Gain offset for short blocks
    int region0_count = 0;         ///< Size of the first scalefactor band region
    int region1_count = 0;         ///< Size of the second scalefactor band region
    int preflag = 0;               ///< Pre-emphasis flag
    int scalefac_scale = 0;        ///< Scale of the scalefactors
    int count1table_select = 0;    ///< Huffman table index for the count1 region
    std::vector<int> scalefactors{}; ///< Decoded scalefactors for the granule/channel
};

/**
 * @brief Side information block following the header (or CRC).
 *
 * Contains structural pointers and decoding info for the main data.
 */
struct SideInfo {
    int main_data_begin = 0; ///< Negative offset in bytes indicating where the main data begins (bit reservoir)
    int private_bits = 0;    ///< Application-specific private bits
    uint8_t scfsi[2][4] = {{0, 0, 0, 0}, {0, 0, 0, 0}}; ///< Scalefactor selection information [channel][band]
    GrChInfo gr[2][2];       ///< Granule and channel specific decoding info [granule][channel]
};

/**
 * @brief Encapsulates a complete MP3 frame during the processing pipeline.
 */
struct Mp3Frame {
    Mp3Header header{};                  ///< Parsed frame header
    SideInfo side_info{};                ///< Parsed side information
    std::vector<uint8_t> side_info_raw{};///< Raw bytes of the side info section
    std::vector<uint8_t> main_data_raw{};///< Raw bytes of the main data (payload)
};

} // namespace mp3packer

#endif // MP3PACKERCPP_TYPES_HPP
