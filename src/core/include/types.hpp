#ifndef MP3PACKERCPP_TYPES_HPP
#define MP3PACKERCPP_TYPES_HPP

#include <cstddef>
#include <cstdint>

namespace mp3packer {

/**
 * @brief Specifies the MPEG audio version.
 */
enum class MpegVersion : uint8_t { MPEG1, MPEG2, MPEG25 };

/**
 * @brief Specifies the audio channel mode.
 */
enum class ChannelMode : uint8_t { Stereo, JointStereo, DualChannel, Mono };

/**
 * @brief Specifies the emphasis mode used for the audio.
 */
enum class Emphasis : uint8_t { None, _50_15, Reserved, CCITT };

/**
 * @brief Holds physical and logical information about a specific bitrate configuration.
 */
struct BitrateInfo {
    uint16_t bitrate_kbps = 0; ///< Nominal bitrate in kilobits per second
    uint16_t frame_size = 0;   ///< Size of the frame in bytes (including padding)
    uint16_t data_size = 0;    ///< Size of the data payload excluding headers
    bool padding = false;      ///< True if padding bit is set for this frame
    uint8_t index = 0;         ///< Index of the bitrate in the MPEG bitrate table
};

/**
 * @brief Represents the decoded 32-bit MPEG audio frame header.
 */
struct Mp3Header {
    MpegVersion version = MpegVersion::MPEG1; ///< MPEG version
    bool has_crc = false;                      ///< True if a CRC check follows the header
    BitrateInfo bitrate;                       ///< Information about frame bitrate and sizes
    uint32_t samplerate = 0;                   ///< Sampling frequency in Hz
    bool padding = false;                      ///< Padding bit
    bool private_bit = false;                  ///< Private bit for application-specific triggers
    ChannelMode channel_mode = ChannelMode::Mono; ///< Audio channel mode
    bool ms_stereo = false;   ///< True if Middle/Side stereo is enabled (Joint Stereo)
    bool is_stereo = false;   ///< True if Intensity stereo is enabled (Joint Stereo)
    bool copyright = false;   ///< Copyright bit
    bool original = false;    ///< Original/Copy bit
    Emphasis emphasis = Emphasis::None; ///< Emphasis indicator
};

/**
 * @brief Decoding information for a specific granule and channel.
 */
struct GrChInfo {
    uint16_t part2_3_length = 0;    ///< Length of scalefactors + Huffman data in bits (12-bit field)
    uint16_t big_values = 0;        ///< Number of large spectral value pairs (9-bit field, max 288)
    uint8_t global_gain = 0;        ///< Quantization step size (8-bit field)
    uint16_t scalefac_compress = 0; ///< Bits used for scalefactors (4-bit MPEG1, 9-bit MPEG2)
    bool window_switching_flag = false; ///< True if block type is not normal
    uint8_t block_type = 0;         ///< 0: normal, 1: start, 2: short, 3: end
    bool mixed_block_flag = false;  ///< True if low frequencies use long blocks, high use short
    uint8_t table_select[3] = {0, 0, 0}; ///< Huffman table indices for the three regions (5-bit each)
    uint8_t subblock_gain[3] = {0, 0, 0};///< Gain offset for each short-block window (3-bit each)
    uint8_t region0_count = 0;      ///< Size of the first scalefactor band region (4-bit field)
    uint8_t region1_count = 0;      ///< Size of the second scalefactor band region (3-bit field)
    bool preflag = false;           ///< Pre-emphasis flag
    bool scalefac_scale = false;    ///< Scale of the scalefactors
    bool count1table_select = false;///< True if count1 region uses Huffman table 33 (false → 32)
};

/**
 * @brief Side information block following the header (or CRC).
 *
 * Contains structural pointers and decoding info for the main data.
 */
struct SideInfo {
    uint16_t main_data_begin = 0; ///< Bytes to backstep into the bit reservoir (9-bit MPEG1, 8-bit MPEG2)
    uint8_t private_bits = 0;     ///< Application-specific private bits
    bool scfsi[2][4] = {{false, false, false, false}, {false, false, false, false}}; ///< Scalefactor selection info [channel][band]
    GrChInfo gr[2][2];            ///< Granule and channel specific decoding info [granule][channel]
};

/// Largest side info there is: MPEG-1 stereo.
constexpr size_t kMaxSideInfoSize = 32;

/**
 * @brief Encapsulates a complete MP3 frame during the processing pipeline.
 *
 * A frame describes its bytes rather than owning copies of them. The reader holds
 * the whole file, so the payload is a span into that, and the side info — at most
 * 32 bytes — sits inline. Copying both into vectors per frame cost two allocations
 * and a copy of the entire audio for a program that only ever reads them.
 */
struct Mp3Frame {
    Mp3Header header{};    ///< Parsed frame header
    SideInfo side_info{};  ///< Parsed side information

    uint8_t side_info_raw[kMaxSideInfoSize] = {}; ///< Raw bytes of the side info section
    uint8_t side_info_size = 0;                   ///< How many of those bytes there are

    size_t frame_offset = 0;     ///< Where the frame's header starts in the reader's data
    size_t main_data_offset = 0; ///< Where the payload starts in the reader's data
    size_t main_data_size = 0;   ///< Declared payload length, which may run past the data
    size_t main_data_avail = 0;  ///< How much of it the file actually holds
};

} // namespace mp3packer

#endif // MP3PACKERCPP_TYPES_HPP
