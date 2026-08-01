// Differential test: BitstreamReader/BitstreamWriter against straightforward
// byte-at-a-time implementations of the same contract.
//
// Two behaviours here are easy to break and expensive to debug from output bytes:
// a read that runs past the end of the data does not zero-fill — the bits it found
// stay at the top of the field and are shifted up again by the number it did not —
// and data() may be called mid-stream, materialising the pending bits without
// committing them, because the packer reads back what it has written to verify a
// granule and then carries on writing.
#include "bitstream.hpp"
#include <cstdio>
#include <random>

using namespace mp3packer;

class OldReader {
public:
    explicit OldReader(const std::vector<uint8_t>& d) : data_(d) {}
    uint32_t read_bits(int num_bits) {
        if (num_bits == 0) return 0;
        uint32_t result = 0;
        for (int i = 0; i < num_bits; ++i) {
            const size_t byte_idx = bit_pos_ / 8;
            const int bit_idx = 7 - (bit_pos_ % 8);
            if (byte_idx >= data_.size()) { result <<= (num_bits - i); bit_pos_ += size_t(num_bits - i); return result; }
            if (data_[byte_idx] & (1 << bit_idx)) result |= (1 << (num_bits - 1 - i));
            bit_pos_++;
        }
        return result;
    }
    void seek_bit(size_t p) { bit_pos_ = p; }
    size_t tell_bit() const { return bit_pos_; }
private:
    const std::vector<uint8_t>& data_;
    size_t bit_pos_ = 0;
};

class OldWriter {
public:
    void write_bits(uint32_t value, int num_bits) {
        for (int i = 0; i < num_bits; ++i) {
            const size_t byte_idx = bit_pos_ / 8;
            const int bit_idx = 7 - (bit_pos_ % 8);
            if (byte_idx >= data_.size()) data_.push_back(0);
            if (value & (1u << (num_bits - 1 - i))) data_[byte_idx] |= (1 << bit_idx);
            bit_pos_++;
        }
    }
    const std::vector<uint8_t>& data() const { return data_; }
    size_t tell_bit() const { return bit_pos_; }
private:
    std::vector<uint8_t> data_;
    size_t bit_pos_ = 0;
};

int main() {
    std::mt19937 rng(1);
    int fails = 0;
    for (int trial = 0; trial < 3000 && fails < 10; ++trial) {
        // random data, random reads (including past the end)
        std::vector<uint8_t> data(rng() % 40);
        for (auto& b : data) b = uint8_t(rng());
        OldReader a(data);
        BitstreamReader b(data);
        for (int i = 0; i < 50; ++i) {
            if (rng() % 8 == 0) { size_t p = rng() % (data.size() * 8 + 40); a.seek_bit(p); b.seek_bit(p); }
            int n = int(rng() % 33);
            uint32_t va = a.read_bits(n), vb = b.read_bits(n);
            if (va != vb || a.tell_bit() != b.tell_bit()) {
                printf("READ trial %d i %d size %zu n %d: %u vs %u pos %zu vs %zu\n",
                       trial, i, data.size(), n, va, vb, a.tell_bit(), b.tell_bit());
                ++fails; break;
            }
        }

        // random writes, compared as bytes plus a mid-stream data() read-back
        OldWriter wa;
        BitstreamWriter wb;
        for (int i = 0; i < 60; ++i) {
            int n = int(rng() % 33);
            uint32_t v = rng();
            if (n < 32) v &= (1u << n) - 1;
            wa.write_bits(v, n);
            wb.write_bits(v, n);
            if (rng() % 10 == 0) {
                const auto& da = wa.data();
                const auto& db = wb.data();
                if (da != db || wa.tell_bit() != wb.tell_bit()) {
                    printf("WRITE-MID trial %d i %d: sizes %zu vs %zu bits %zu vs %zu\n",
                           trial, i, da.size(), db.size(), wa.tell_bit(), wb.tell_bit());
                    for (size_t k = 0; k < std::min(da.size(), db.size()); ++k)
                        if (da[k] != db[k]) { printf("  first diff at byte %zu: %02x vs %02x\n", k, da[k], db[k]); break; }
                    ++fails; break;
                }
            }
        }
        if (wa.data() != wb.data()) { printf("WRITE-END trial %d differs\n", trial); ++fails; }
    }
    printf(fails ? "FAILURES: %d\n" : "ok\n", fails);
    return fails != 0;
}
