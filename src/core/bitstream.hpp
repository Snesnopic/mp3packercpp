#ifndef MP3PACKERCPP_BITSTREAM_HPP
#define MP3PACKERCPP_BITSTREAM_HPP

#include <vector>
#include <cstdint>
#include <stdexcept>

namespace mp3packer {

class BitstreamReader {
public:
    explicit BitstreamReader(const std::vector<uint8_t>& data) : data_(data), bit_pos_(0) {}

    uint32_t read_bits(const int num_bits) {
        if (num_bits == 0) return 0;
        if (num_bits > 32) throw std::invalid_argument("Cannot read more than 32 bits");

        uint32_t result = 0;
        for (int i = 0; i < num_bits; ++i) {
            size_t byte_idx = bit_pos_ / 8;
            int bit_idx = 7 - (bit_pos_ % 8);
            
            if (byte_idx >= data_.size()) {
                result <<= (num_bits - i);
                bit_pos_ += static_cast<size_t>(num_bits - i);
                return result;
            }

            if (data_[byte_idx] & (1 << bit_idx)) {
                result |= (1 << (num_bits - 1 - i));
            }
            bit_pos_++;
        }
        return result;
    }

    void seek_bit(size_t pos) { bit_pos_ = pos; }
    [[nodiscard]] size_t tell_bit() const { return bit_pos_; }
    [[nodiscard]] bool eof() const { return (bit_pos_ / 8) >= data_.size(); }

private:
    const std::vector<uint8_t>& data_;
    size_t bit_pos_;
};

class BitstreamWriter {
public:
    BitstreamWriter() : bit_pos_(0) {}

    void write_bits(const uint32_t value, const int num_bits) {
        if (num_bits == 0) return;
        if (num_bits > 32) throw std::invalid_argument("Cannot write more than 32 bits");

        for (int i = 0; i < num_bits; ++i) {
            size_t byte_idx = bit_pos_ / 8;
            int bit_idx = 7 - (bit_pos_ % 8);

            if (byte_idx >= data_.size()) {
                data_.push_back(0);
            }

            if (value & (1 << (num_bits - 1 - i))) {
                data_[byte_idx] |= (1 << bit_idx);
            }
            bit_pos_++;
        }
    }

    [[nodiscard]] const std::vector<uint8_t>& data() const { return data_; }
    [[nodiscard]] size_t tell_bit() const { return bit_pos_; }

private:
    std::vector<uint8_t> data_;
    size_t bit_pos_;
};

} // namespace mp3packer

#endif // MP3PACKERCPP_BITSTREAM_HPP
