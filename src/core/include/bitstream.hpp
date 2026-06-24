#ifndef MP3PACKERCPP_BITSTREAM_HPP
#define MP3PACKERCPP_BITSTREAM_HPP

#include <vector>
#include <stdexcept>
#include <cstdint>

namespace mp3packer {

/**
 * @brief Utility class for reading arbitrary bits from a byte stream.
 */
class BitstreamReader {
public:
    /**
     * @brief Constructs a BitstreamReader wrapping the provided data vector.
     * @param data Reference to the vector of bytes to read from.
     */
    explicit BitstreamReader(const std::vector<uint8_t>& data) : data_(data) {}

    /**
     * @brief Reads a specified number of bits from the stream.
     * @param num_bits Number of bits to read (maximum 32).
     * @return The read bits as a 32-bit unsigned integer.
     */
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

    /**
     * @brief Seeks to a specific bit position in the stream.
     * @param pos Absolute bit position to seek to.
     */
    void seek_bit(size_t pos) { bit_pos_ = pos; }

    /**
     * @brief Gets the current bit position.
     * @return The absolute bit index the reader is currently at.
     */
    [[nodiscard]] size_t tell_bit() const { return bit_pos_; }

private:
    const std::vector<uint8_t>& data_; ///< Reference to the byte stream being read
    size_t bit_pos_ = 0;               ///< Current absolute bit position
};

/**
 * @brief Utility class for writing arbitrary bits to a dynamic byte stream.
 */
class BitstreamWriter {
public:
    BitstreamWriter() = default;

    /**
     * @brief Writes a specified number of bits to the stream.
     * @param value The value to write.
     * @param num_bits The number of bits to extract from the value (maximum 32).
     */
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

    /**
     * @brief Gets the underlying byte vector containing the written data.
     * @return Constant reference to the data vector.
     */
    [[nodiscard]] const std::vector<uint8_t>& data() const { return data_; }

    /**
     * @brief Gets the current bit position.
     * @return The absolute bit index the writer is currently at.
     */
    [[nodiscard]] size_t tell_bit() const { return bit_pos_; }

private:
    std::vector<uint8_t> data_; ///< Dynamic byte stream being written
    size_t bit_pos_ = 0;        ///< Current absolute bit position
};

} // namespace mp3packer

#endif // MP3PACKERCPP_BITSTREAM_HPP
