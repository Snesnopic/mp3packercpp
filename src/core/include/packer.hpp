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
    
    /** @brief Number of threads to use. If 0, uses auto-detection. */
    unsigned int num_threads = 0;

    /**
     * @brief Emit a constant-bitrate stream instead of letting each frame shrink.
     *
     * The bitrate is the lowest one every frame still fits in *after* the repack,
     * worked out from that same repack rather than a separate pass. Frames only
     * grow, and only into padding: the audio is untouched.
     */
    bool constant_bitrate = false;

    /**
     * @brief Floor for the constant bitrate, in kbps; rounded up to a valid one.
     *
     * Setting this implies constant-bitrate output. Combined with
     * @ref constant_bitrate the higher of the two wins.
     */
    int min_bitrate_kbps = 0;

    /**
     * @brief Keep the frame CRC on frames that arrived with one, recomputing it.
     *
     * Off by default: dropping it frees two bytes of payload per frame, which is
     * what every release so far has done. Turning it on is more faithful to the
     * input at that cost. Frames that had no CRC never gain one.
     */
    bool keep_crc = false;

    /**
     * @brief Keep the Xing/Info frame as it arrived, patching only its numeric fields.
     *
     * By default the frame is rebuilt around the fields we understand and shrunk to
     * the smallest bitrate that holds them, which drops any trailing extension we do
     * not recognise. With this set it keeps its original length and its full
     * contents; only the byte count and the seek table, which the repack genuinely
     * changes, are written over.
     */
    bool preserve_xing = false;

    /**
     * @brief Work out the constant bitrate but write nothing.
     *
     * @ref process still parses and repacks — the answer comes out of that same
     * work — but stops before writing the output file.
     */
    bool report_bitrate_only = false;

    /**
     * @brief Processes an input MP3 file and writes the optimized result to an output file.
     * @param input_file Path to the source MP3 file.
     * @param output_file Path where the optimized MP3 file should be saved. Ignored
     *        when @ref report_bitrate_only is set.
     * @param out_bitrate_kbps If non-null, receives the constant bitrate chosen, or 0
     *        when the output is variable-bitrate.
     */
    void process(const std::string& input_file, const std::string& output_file,
                 int* out_bitrate_kbps = nullptr) const;
};

} // namespace mp3packer

#endif // MP3PACKERCPP_PACKER_HPP
