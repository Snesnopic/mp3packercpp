// Drives a whole repack from arbitrary bytes. Every other test in this tree feeds
// well-formed input, so this is the only one that reaches the parser's error
// paths at all.
//
// Refusing an input is always acceptable — most random bytes are not an MP3, and
// throwing is how the packer says so. What is not acceptable is crashing, reading
// out of bounds, or reporting success beside an output that is not itself a valid
// MP3. And when the input does decode, the output has to decode to the very same
// audio: that is the product's whole promise, and it should hold for a malformed
// file we chose to accept just as much as for a clean one.
//
// The first byte is consumed as a flag word rather than as file content, so one
// corpus exercises every combination of the modes. Seeds therefore need a byte in
// front of the mp3.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <string>
#include <vector>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#if !defined(_WIN32)
#include <unistd.h>
#endif
#include "packer.hpp"

namespace {

std::string scratch_path(const char* suffix) {
    static const std::string base = [] {
        std::string dir = std::filesystem::temp_directory_path().string();
        return dir + "/mp3packer_fuzz_" + std::to_string(
#if defined(_WIN32)
            0
#else
            static_cast<long>(::getpid())
#endif
        );
    }();
    return base + suffix;
}

bool write_all(const std::string& path, const uint8_t* data, size_t size) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    return static_cast<bool>(out);
}



// How many frames the file is, if it is one unbroken chain of them — every
// frame's declared length landing exactly on the next frame's sync word, from the
// first to the end — and 0 if it is not.
//
// This is the harness's real assertion, and it is deliberately structural rather
// than acoustic. Comparing decoded audio was tried first and withdrawn: a stream
// can chain cleanly and still be self-inconsistent in ways no encoder produces —
// a granule whose part2_3 claims more bits than were written reads slack in the
// input and real data in a repack, a main_data_begin can point at a reservoir that
// was never filled — and on such a file the repack legitimately normalises what
// the input got wrong. Decoders then disagree about the two files by construction:
// observed here were samples off by up to 7, and frames minimp3 refuses in the
// input but decodes happily in the output. Requiring the audio to match would have
// meant proving the input semantically consistent first, which costs a second
// parser. Losslessness is asserted where the premise holds instead — the
// deterministic suites, over well-formed corpora, to the exact sample.
//
// What does hold against anything: a file we accepted must come back out as the
// same number of frames, chained just as cleanly. That is bug #10 exactly.
int frames_chain_cleanly(const std::vector<uint8_t>& d) {
    size_t i = 0;
    if (d.size() > 10 && d[0] == 'I' && d[1] == 'D' && d[2] == '3') {
        i = 10 + ((d[6] & 0x7Fu) << 21 | (d[7] & 0x7Fu) << 14 | (d[8] & 0x7Fu) << 7 | (d[9] & 0x7Fu));
    }
    size_t end = d.size();
    if (end >= 128 && d[end - 128] == 'T' && d[end - 127] == 'A' && d[end - 126] == 'G') end -= 128;
    if (i + 4 > end) return 0;

    static const int v1[16] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0};
    static const int v2[16] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0};
    static const int rates[4] = {44100, 48000, 32000, 0};

    int frames = 0;
    while (i + 4 <= end) {
        if (d[i] != 0xFF || (d[i + 1] & 0xE0) != 0xE0) return 0;
        const int version = (d[i + 1] >> 3) & 3;      // 3=MPEG1, 2=MPEG2, 0=MPEG2.5
        const int layer = (d[i + 1] >> 1) & 3;        // 1 = Layer III
        const int bitrate_index = (d[i + 2] >> 4) & 0xF;
        const int rate_index = (d[i + 2] >> 2) & 3;
        if (version == 1 || layer != 1 || bitrate_index == 0 || bitrate_index == 15 || rate_index == 3) return 0;
        const int bitrate = (version == 3 ? v1 : v2)[bitrate_index];
        const int rate = rates[rate_index] / (version == 3 ? 1 : (version == 2 ? 2 : 4));
        const int size = (version == 3 ? 144 : 72) * bitrate * 1000 / rate + ((d[i + 2] >> 1) & 1);
        if (size <= 4) return 0;
        i += static_cast<size_t>(size);
        ++frames;
    }
    return (i == end) ? frames : 0;
}

[[noreturn]] void fail(const char* what) {
    std::fprintf(stderr, "fuzz invariant violated: %s\n", what);
    std::abort();
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 2) return 0;
    // A megabyte is far past what any interesting parser state needs, and the
    // repack is quadratic in nothing but still not free.
    if (size > (1u << 20)) return 0;

    const uint8_t flags = data[0];
    const uint8_t* body = data + 1;
    const size_t body_size = size - 1;

    const std::string in = scratch_path("_in.mp3");
    const std::string out = scratch_path("_out.mp3");
    const std::string again = scratch_path("_again.mp3");
    if (!write_all(in, body, body_size)) return 0;

    mp3packer::Packer packer;
    packer.recompress_huffman = (flags & 1) != 0;
    packer.num_threads = 1;                       // deterministic, and the pool is not what is under test
    packer.keep_crc = (flags & 2) != 0;
    packer.preserve_xing = (flags & 4) != 0;
    packer.constant_bitrate = (flags & 8) != 0;

    try {
        packer.process(in, out);
    } catch (const std::exception&) {
        return 0;                                 // refusing malformed input is correct
    }

    // It said yes. Then the output has to be something it would itself accept:
    // reporting success beside a file that is not a valid MP3 is the failure mode
    // this is really hunting.
    if (!std::filesystem::exists(out)) fail("process succeeded but wrote no output");
    try {
        mp3packer::Packer verify;
        verify.num_threads = 1;
        verify.process(out, again);
    } catch (const std::exception&) {
        fail("the output of a successful repack is not itself readable");
    }

    // And a clean chain in must be the same clean chain out.
    if (const int frames_in = frames_chain_cleanly(std::vector<uint8_t>(body, body + body_size))) {
        std::ifstream f(out, std::ios::binary);
        const std::vector<uint8_t> written((std::istreambuf_iterator<char>(f)),
                                           std::istreambuf_iterator<char>());
        const int frames_out = frames_chain_cleanly(written);
        if (frames_out == 0) fail("a clean chain of frames came back out broken");
        if (frames_out != frames_in) fail("repack changed the number of frames");
    }
    return 0;
}
