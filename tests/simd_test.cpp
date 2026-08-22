// The cost kernels compute the same two answers in up to four different ways —
// portable, NEON, SSE4.1, AVX2 — and only one of them runs on any given machine.
// This checks the ones this processor can execute against the portable form they
// are held against, which is the only way the wider paths get covered at all on a
// machine that does not run them by default.
#include <cstdio>
#include "huffman.hpp"

int main() {
    std::printf("active kernels: %s\n", mp3packer::active_simd_kernel());
    if (const char* bad = mp3packer::verify_simd_kernels()) {
        std::printf("FAIL: %s kernels disagree with the portable ones\n", bad);
        return 1;
    }
    std::printf("ok: every kernel form this cpu runs agrees with the portable one\n");
    return 0;
}
