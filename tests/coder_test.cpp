// Tests that the Huffman search only ever returns a coding that reproduces the
// spectrum it was given.
//
// The case that motivated this: a count1 quadruple cannot start inside the last
// one, so a big_values that leaves one, two or three coefficients above the final
// quadruple leaves them uncoded. The cost model used to treat such a tail as free,
// which made those big_values look cheapest whenever the top of the spectrum was
// coded at all; the encoder then dropped the coefficients. In the packer that shows
// up as a frame failing round-trip verification and being written verbatim, so the
// whole frame loses its recompression.
#include "huffman.hpp"
#include <cstdio>
using namespace mp3packer;

static bool round_trips(const Spectrum& s, const HuffmanConfig& orig, int* bits_out, int* bv_out) {
    const HuffmanConfig best = HuffmanOptimizer::find_best_config(s, orig, 44100);
    BitstreamWriter w;
    HuffmanOptimizer::encode_quantized_coefficients(s, best, w, 44100);
    const int bits = static_cast<int>(w.tell_bit());
    Spectrum back{};
    BitstreamReader r(w.data());
    HuffmanOptimizer::decode_quantized_coefficients(back, best, r, 44100, bits);
    *bits_out = bits;
    *bv_out = best.big_values;
    return back == s;
}

// Every entry of the big-value probe, against a walk of the same tree one bit at a
// time from the root. The decoder reads the table and never the tree for 99% of
// codewords, so a single wrong entry would decode a pair to the wrong magnitudes or
// desynchronise the rest of the region.
static int check_probe_tables() {
    // The trees are the ones in huffman_tables.hpp, reached here through the same
    // library the decoder uses; walking them needs only their shape.
    extern const int16_t* huffman_tree_for_test(int idx);
    const int bits = HuffmanOptimizer::probe_bits();
    int failures = 0;
    for (int idx = 0; idx < 32 && failures < 10; ++idx) {
        const int16_t* tab = huffman_tree_for_test(idx);
        for (unsigned prefix = 0; prefix < (1u << bits); ++prefix) {
            // Walk the tree, taking the prefix's bits most significant first.
            int node = 0, used = 0, sym = -1;
            while (used <= bits) {
                const int16_t v = tab[node];
                if (v >= 0) { sym = v; break; }
                if (used == bits) break; // longer than the probe
                ++node;
                if (prefix >> (bits - 1 - used) & 1) node -= v;
                ++used;
            }
            int x = -1, y = -1, length = -1, signs = -1;
            const bool resolved = HuffmanOptimizer::probe_pair(idx, prefix, x, y, length, signs);
            if (sym < 0) {
                if (resolved) {
                    printf("table %d prefix %u: resolved a codeword longer than the probe\n", idx, prefix);
                    ++failures;
                }
                continue;
            }
            const int wx = (sym >> 4) & 0xF, wy = sym & 0xF;
            if (!resolved || x != wx || y != wy || length != used ||
                signs != (wx != 0) + (wy != 0)) {
                printf("table %d prefix %u: entry (%d,%d) len %d signs %d, tree says (%d,%d) len %d\n",
                       idx, prefix, x, y, length, signs, wx, wy, used);
                ++failures;
            }
        }
    }
    printf("probe tables vs trees:       %s\n", failures ? "MISMATCH" : "all entries agree");
    return failures;
}

int main() {
    HuffmanConfig orig{};
    orig.big_values = 288;
    orig.region0_count = 7;
    orig.region1_count = 13;
    orig.table0 = orig.table1 = orig.table2 = 1;
    int failures = 0;

    // Case 1: a lone non-zero at the very top of the spectrum.
    {
        Spectrum s{};
        for (int i = 0; i < 40; ++i) s[i] = static_cast<int16_t>((i % 5) - 2); // some big values
        s[574] = 1;
        int bits = 0, bv = 0;
        const bool ok = round_trips(s, orig, &bits, &bv);
        printf("top coefficient at 574:      big_values=%3d bits=%4d %s\n", bv, bits, ok ? "round-trips" : "DROPPED COEFFICIENTS");
        failures += !ok;
    }

    // Case 2: both of the top two coded.
    {
        Spectrum s{};
        for (int i = 0; i < 40; ++i) s[i] = static_cast<int16_t>((i % 7) - 3);
        s[574] = -1;
        s[575] = 1;
        int bits = 0, bv = 0;
        const bool ok = round_trips(s, orig, &bits, &bv);
        printf("top coefficients at 574,575: big_values=%3d bits=%4d %s\n", bv, bits, ok ? "round-trips" : "DROPPED COEFFICIENTS");
        failures += !ok;
    }

    // Case 3: a dense tail of ones, ending at 575.
    {
        Spectrum s{};
        for (int i = 0; i < 20; ++i) s[i] = static_cast<int16_t>((i % 3) - 1);
        for (int i = 560; i < 576; ++i) s[i] = static_cast<int16_t>((i % 2) ? 1 : -1);
        int bits = 0, bv = 0;
        const bool ok = round_trips(s, orig, &bits, &bv);
        printf("dense tail through 575:      big_values=%3d bits=%4d %s\n", bv, bits, ok ? "round-trips" : "DROPPED COEFFICIENTS");
        failures += !ok;
    }

    failures += check_probe_tables();

    printf(failures ? "FAIL (%d)\n" : "ok\n", failures);
    return failures != 0;
}
