#ifndef MP3PACKERCPP_PACKER_HPP
#define MP3PACKERCPP_PACKER_HPP

#include <vector>
#include <string>

namespace mp3packer {

/**
 * @brief Orchestrates the end-to-end processing of MP3 files.
 *
 * It utilizes the Mp3Reader to parse the input, delegates optimization to
 * HuffmanOptimizer, computes the new bit reservoir constraints in a two-pass
 * system, and writes the final bit-identical, space-optimized MP3 file.
 */
class Packer {
public:
    Packer();

    /** @brief If true, applies aggressive Huffman tree re-encoding (brute-force). */
    bool recompress_huffman = false;
    
    /**
     * @brief Processes an input MP3 file and writes the optimized result to an output file.
     * @param input_file Path to the source MP3 file.
     * @param output_file Path where the optimized MP3 file should be saved.
     */
    void process(const std::string& input_file, const std::string& output_file) const;
};

} // namespace mp3packer

#endif // MP3PACKERCPP_PACKER_HPP
