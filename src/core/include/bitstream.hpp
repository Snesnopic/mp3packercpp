#ifndef MP3PACKERCPP_BITSTREAM_HPP
#define MP3PACKERCPP_BITSTREAM_HPP

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace mp3packer {

/**
 * @brief Loads eight bytes most-significant-byte first.
 */
inline uint64_t load_be64(const uint8_t* p) {
    uint64_t v;
    std::memcpy(&v, p, sizeof v);
#if defined(_MSC_VER)
    return _byteswap_uint64(v);
#else
    return __builtin_bswap64(v);
#endif
}

/**
 * @brief Stores eight bytes most-significant-byte first.
 */
inline void store_be64(uint8_t* p, const uint64_t v) {
#if defined(_MSC_VER)
    const uint64_t be = _byteswap_uint64(v);
#else
    const uint64_t be = __builtin_bswap64(v);
#endif
    std::memcpy(p, &be, sizeof be);
}

/**
 * @brief Utility class for reading arbitrary bits from a byte stream.
 *
 * Both directions of the bitstream work through a single 64-bit access per
 * operation rather than walking bytes: no MP3 field is wider than 32 bits, and a
 * whole coefficient pair fits in 47, so one load or read-modify-write covers any
 * of them.
 */
class BitstreamReader {
public:
    /**
     * @brief Constructs a BitstreamReader wrapping the provided data vector.
     * @param data Reference to the vector of bytes to read from.
     */
    explicit BitstreamReader(const std::vector<uint8_t>& data)
        : data_(data.data()), size_(data.size()) {
        // The tail is mirrored into a zero-filled buffer so that a read near the
        // end of the data is the same load as any other: reads past the end yield
        // zero bits, which is how the byte-walking reader behaved, and what a
        // truncated final frame needs.
        last_word_ = static_cast<ptrdiff_t>(size_) - 8;
        tail_from_ = size_ > 7 ? size_ - 7 : 0;
        std::memset(pad_, 0, sizeof pad_);
        if (size_ > tail_from_) std::memcpy(pad_, data_ + tail_from_, size_ - tail_from_);
    }

    /**
     * @brief Returns the 64 bits at an explicit bit position without consuming
     *        them, most significant bit first, zero-filled past the end.
     *
     * Only the top 57 bits are guaranteed to be present, which is more than the 47
     * a single coefficient pair can occupy. Taking a position rather than using the
     * stored one lets a caller decoding a run of symbols keep its position in a
     * register instead of round-tripping it through the reader.
     */
    [[nodiscard]] uint64_t peek_at(const size_t pos) const {
        size_t idx = pos >> 3;
        const uint8_t* b = data_;
        if (static_cast<ptrdiff_t>(idx) > last_word_) {
            b = pad_;
            idx = std::min(idx - tail_from_, static_cast<size_t>(7));
        }
        return load_be64(b + idx) << (pos & 7);
    }

    /**
     * @brief Reads a specified number of bits from the stream.
     * @param num_bits Number of bits to read (maximum 32).
     * @return The read bits as a 32-bit unsigned integer.
     */
    uint32_t read_bits(const int num_bits) {
        if (num_bits == 0) return 0;
        if (num_bits > 32) throw std::invalid_argument("Cannot read more than 32 bits");

        uint32_t result = static_cast<uint32_t>(peek_at(bit_pos_) >> (64 - num_bits));
        const size_t total = size_ * 8;
        if (bit_pos_ + static_cast<size_t>(num_bits) > total) {
            // A read that runs off the end of the data keeps the byte-walking
            // reader's behaviour: the bits it did find stay at the top of the
            // field and are then shifted up again by the number it did not, which
            // is not the same as zero-filling them. Nothing downstream depends on
            // the value — a frame reaching past the reservoir cannot be recompressed
            // either way — but the bytes it eventually writes do.
            const size_t avail = bit_pos_ < total ? total - bit_pos_ : 0;
            const int missing = num_bits - static_cast<int>(avail);
            result = missing >= 32 ? 0 : result << missing;
        }
        bit_pos_ += static_cast<size_t>(num_bits);
        return result;
    }

    /// Total number of bits in the underlying data.
    [[nodiscard]] size_t size_bits() const { return size_ * 8; }

    /**
     * @brief Seeks to a specific bit position in the stream.
     * @param pos Absolute bit position to seek to.
     */
    void seek_bit(const size_t pos) { bit_pos_ = pos; }

    /**
     * @brief Gets the current bit position.
     * @return The absolute bit index the reader is currently at.
     */
    [[nodiscard]] size_t tell_bit() const { return bit_pos_; }

private:
    const uint8_t* data_;  ///< Start of the byte stream being read
    size_t size_;          ///< Length of the byte stream
    size_t bit_pos_ = 0;   ///< Current absolute bit position
    ptrdiff_t last_word_;  ///< Highest byte index a whole word can be loaded from
    size_t tail_from_;     ///< Byte index that pad_[0] stands for
    uint8_t pad_[16];      ///< Zero-filled copy of the final bytes
};

/**
 * @brief Utility class for writing arbitrary bits to a dynamic byte stream.
 *
 * Pending bits are held in a register-sized accumulator and reach the buffer
 * eight bytes at a time, so appending a field costs a shift and an or; nothing is
 * ever read back out of the buffer.
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
        put(static_cast<uint64_t>(value), num_bits);
    }

    /**
     * @brief Appends the low num_bits bits of value, most significant first.
     *
     * A store leaves under eight bits pending, so anything up to 57 bits lands in a
     * single accumulator window; that covers every field in the bitstream, a
     * coefficient pair included.
     */
    void put(const uint64_t value, const int num_bits) {
        if (accumulated_ + num_bits > 64) store(accumulator_, accumulated_);
        accumulator_ |= (value & ((uint64_t{1} << num_bits) - 1)) << (64 - accumulated_ - num_bits);
        accumulated_ += num_bits;
    }

    /**
     * @brief Moves n bits from a reader to this writer without interpreting them.
     *
     * Used for scalefactors, which are re-emitted verbatim: their layout depends on
     * scalefac_compress tables we only need to size, never to decode. The caller
     * must have established that the whole span is inside the reader's data, since
     * a read past the end has a value only field-by-field reading reproduces.
     */
    void copy_from(BitstreamReader& reader, size_t num_bits) {
        while (num_bits >= 32) {
            put(reader.read_bits(32), 32);
            num_bits -= 32;
        }
        if (num_bits > 0) put(reader.read_bits(static_cast<int>(num_bits)), static_cast<int>(num_bits));
    }

    /**
     * @brief Takes the pending bits so a caller can append to them in registers.
     *
     * put() keeps its state in memory, so a long run of fields pays a load and a
     * store of the accumulator per field, plus the store-to-load turnaround between
     * one field and the next. A caller that takes the pending bits here, spills with
     * store() whenever the next field would not fit, and hands them back with
     * resume() pays none of that. The writer's own bits are not valid in between, so
     * nothing else may write to it.
     */
    void pending(uint64_t& acc, int& nacc) const { acc = accumulator_; nacc = accumulated_; }

    /**
     * @brief Commits the accumulator's whole bytes, leaving under eight bits in it.
     *
     * Eight bytes are always stored and only the complete ones committed, so the
     * next store overwrites the remainder: every byte the writer returns is one it
     * wrote itself, which is why the buffer needs slack but needs no zeroing.
     */
    void store(uint64_t& acc, int& nacc) {
        if (buf_.size() < committed_ + kSlack) {
            buf_.resize(std::max(committed_ + kSlack, buf_.size() * 2));
        }
        store_be64(buf_.data() + committed_, acc);
        const int whole = nacc >> 3;
        committed_ += static_cast<size_t>(whole);
        // A field that exactly fills the accumulator leaves nothing over, and
        // shifting a 64-bit value by 64 is undefined rather than zero.
        acc = whole == 8 ? 0 : acc << whole * 8;
        nacc -= whole * 8;
    }

    /**
     * @brief Returns the accumulator to the writer.
     */
    void resume(const uint64_t acc, const int nacc) { accumulator_ = acc; accumulated_ = nacc; }

    /**
     * @brief Gets the underlying byte vector containing the written data.
     *
     * The pending bits are materialised without being committed, so a caller can
     * read back what it has written so far and then carry on writing — which the
     * verification pass does, once per granule.
     *
     * @return Constant reference to the data vector, zero-padded to a whole byte.
     */
    [[nodiscard]] const std::vector<uint8_t>& data() {
        if (accumulated_ > 0) {
            uint64_t acc = accumulator_;
            int nacc = accumulated_;
            const size_t committed = committed_;
            store(acc, nacc);
            committed_ = committed; // the partial byte stays pending
        }
        buf_.resize((tell_bit() + 7) / 8);
        return buf_;
    }

    /**
     * @brief Gets the current bit position.
     * @return The absolute bit index the writer is currently at.
     */
    [[nodiscard]] size_t tell_bit() const { return committed_ * 8 + static_cast<size_t>(accumulated_); }

private:
    /// A store always writes a whole word, of which it commits only the complete
    /// bytes, so the buffer carries this much room past the output.
    static constexpr size_t kSlack = 8;

    std::vector<uint8_t> buf_;   ///< Committed bytes, plus slack for the next store
    uint64_t accumulator_ = 0;   ///< Pending bits, most significant first
    int accumulated_ = 0;        ///< How many of the accumulator's bits are pending
    size_t committed_ = 0;       ///< Bytes already committed to buf_
};

} // namespace mp3packer

#endif // MP3PACKERCPP_BITSTREAM_HPP
