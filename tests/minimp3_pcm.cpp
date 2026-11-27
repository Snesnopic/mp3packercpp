// Decodes an MP3 with the vendored minimp3, float output, and writes the samples to
// stdout. minimp3 is the decoder that builds each step from two float factors, so it
// is the one a scalefactor re-coding must match bit for bit; test_matrix.sh compares
// its output alongside ffmpeg's. usage: minimp3_pcm IN > OUT.f32
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_FLOAT_OUTPUT
#include "minimp3_ex.h"
#include <cstdio>
#include <cstdlib>

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: minimp3_pcm IN > OUT.f32\n");
        return 2;
    }
    mp3dec_t dec;
    mp3dec_file_info_t info;
    if (mp3dec_load(&dec, argv[1], &info, nullptr, nullptr) != 0) return 1;
    const size_t written = fwrite(info.buffer, sizeof(float), info.samples, stdout);
    free(info.buffer);
    return written == info.samples ? 0 : 1;
}
