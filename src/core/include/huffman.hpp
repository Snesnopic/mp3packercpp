#ifndef MP3PACKERCPP_HUFFMAN_HPP
#define MP3PACKERCPP_HUFFMAN_HPP

#include "bitstream.hpp"
#include <vector>

namespace mp3packer {

/**
 * @brief Represents the configuration of Huffman regions and tables for a granule.
 */
struct HuffmanConfig {
    int region0_count = 0;             ///< Number of scalefactor bands in region 0
    int region1_count = 0;             ///< Number of scalefactor bands in region 1
    int big_values = 0;                ///< Number of large value pairs (max 288)
    int table0 = 0;                    ///< Huffman table index for region 0
    int table1 = 0;                    ///< Huffman table index for region 1
    int table2 = 0;                    ///< Huffman table index for region 2
    bool count1_table_select = false;  ///< True if count1 region uses table 33 (false uses 32)
    bool window_switching_flag = false;///< True if block uses short windows
    int block_type = 0;                ///< 0: normal, 1: start, 2: short, 3: end
    int mixed_block_flag = 0;          ///< True if mixed block mode is enabled
};

/**
 * @brief Handles decoding, optimizing, and re-encoding of MP3 Huffman data.
 */
class HuffmanOptimizer {
public:
    HuffmanOptimizer();

    /**
     * @brief Decodes raw bitstream data into 576 quantized spectral coefficients.
     *
     * The part2_3 bit limit is checked only at the start of each big-values pair and
     * each count1 quad, never mid-symbol. This matches mp3packer's decode behaviour.
     *
     * @param config The original Huffman configuration from the side info.
     * @param reader Bitstream reader positioned at the start of the Huffman data
     *               (i.e. after scalefactors, at bit offset part2_length).
     * @param samplerate Sampling rate of the frame.
     * @param max_huffman_bits Maximum bits to consume (part2_3_length - part2_length).
     *                         Pass -1 to decode until big_values/count1 are exhausted.
     * @return A vector of 576 integer coefficients (zero-padded beyond the last decoded value).
     */
    static std::vector<int16_t> decode_quantized_coefficients(const HuffmanConfig& config, BitstreamReader& reader, int samplerate, int max_huffman_bits = -1);

    /**
     * @brief Performs brute-force search to find the optimal Huffman table combination.
     * @param coeffs The 576 decoded coefficients.
     * @param orig_config The original configuration (used to preserve block types).
     * @param samplerate Sampling rate of the frame.
     * @return A new HuffmanConfig that yields the smallest bit size for the coefficients.
     */
    static HuffmanConfig find_best_config(const std::vector<int16_t>& coeffs, const HuffmanConfig& orig_config, int samplerate);

    /**
     * @brief Re-encodes the coefficients into a bitstream using a given configuration.
     * @param coeffs The 576 coefficients to encode.
     * @param config The (usually optimized) Huffman configuration.
     * @param writer Bitstream writer to output the compressed bits.
     * @param samplerate Sampling rate of the frame.
     */
    static void encode_quantized_coefficients(const std::vector<int16_t>& coeffs, const HuffmanConfig& config, BitstreamWriter& writer, int samplerate);

};

} // namespace mp3packer

#endif // MP3PACKERCPP_HUFFMAN_HPP
