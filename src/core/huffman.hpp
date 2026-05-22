#ifndef MP3PACKERCPP_HUFFMAN_HPP
#define MP3PACKERCPP_HUFFMAN_HPP

#include "bitstream.hpp"
#include <vector>

namespace mp3packer {

struct HuffmanConfig {
    int region0_count = 0; // index
    int region1_count = 0; // index
    int big_values = 0;    // pairs
    int table0 = 0;
    int table1 = 0;
    int table2 = 0;
    bool count1_table_select = false;
    bool window_switching_flag = false;
    int block_type = 0;
    int mixed_block_flag = 0;
};

class HuffmanOptimizer {
public:
    HuffmanOptimizer();

    // Ported logic for -z flag
    static std::vector<int16_t> decode_quantized_coefficients(const HuffmanConfig& config, BitstreamReader& reader, int samplerate);
    static HuffmanConfig find_best_config(const std::vector<int16_t>& coeffs, const HuffmanConfig& orig_config, int samplerate);

    static void encode_quantized_coefficients(const std::vector<int16_t>& coeffs, const HuffmanConfig& config, BitstreamWriter& writer, int samplerate);

};

} // namespace mp3packer

#endif // MP3PACKERCPP_HUFFMAN_HPP
