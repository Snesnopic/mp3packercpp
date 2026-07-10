#include "decoder.hpp"

#include <stdexcept>
#include <cstdlib>

#define MINIMP3_IMPLEMENTATION
#include "minimp3_ex.h"

namespace mp3packer {

PcmAudio decode_pcm(const std::string& input_file) {
    mp3dec_t dec;
    mp3dec_file_info_t info{};

    if (mp3dec_load(&dec, input_file.c_str(), &info, nullptr, nullptr) != 0) {
        throw std::runtime_error("minimp3: failed to decode " + input_file);
    }

    PcmAudio result;
    result.channels = info.channels;
    result.sample_rate = info.hz;
    if (info.buffer && info.samples > 0) {
        result.samples.assign(info.buffer, info.buffer + info.samples);
    }
    std::free(info.buffer);

    return result;
}

} // namespace mp3packer
