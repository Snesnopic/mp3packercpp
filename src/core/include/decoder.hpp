#ifndef MP3PACKERCPP_DECODER_HPP
#define MP3PACKERCPP_DECODER_HPP

#include <cstdint>
#include <string>
#include <vector>

namespace mp3packer {

/**
 * @brief Decoded MP3 audio as interleaved 16-bit PCM.
 */
struct PcmAudio {
    std::vector<int16_t> samples; // interleaved, channels included
    int channels = 0;
    int sample_rate = 0;
};

/**
 * @brief Decodes an MP3 file to raw interleaved 16-bit PCM using minimp3.
 *
 * Intended for verifying that a repacked file still decodes to the exact
 * same audio as the source (the Packer only rewrites Huffman codes, so a
 * correct repack must always decode identically).
 *
 * @param input_file Path to the MP3 file.
 * @return The decoded PCM audio.
 * @throws std::runtime_error on decode failure.
 */
PcmAudio decode_pcm(const std::string& input_file);

} // namespace mp3packer

#endif // MP3PACKERCPP_DECODER_HPP
