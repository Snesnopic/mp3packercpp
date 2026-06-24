#include "include/packer.hpp"
#include "include/huffman.hpp"
#include "include/logger.hpp"
#include "include/mp3_reader.hpp"
#include "include/huffman.hpp"
#include "include/bitstream.hpp"
#include <iostream>
#include <algorithm>
#include <vector>
#include <fstream>
#include <stdexcept>
#include <atomic>
#include <thread>

namespace mp3packer {

Packer::Packer() = default;

// Takes the original raw side_info bytes and sets only the main_data_begin field.
static std::vector<uint8_t> patch_mdb(const std::vector<uint8_t>& raw, MpegVersion version, uint16_t new_mdb) {
    std::vector<uint8_t> out = raw;
    if (version == MpegVersion::MPEG1) {
        // main_data_begin is 9 bits (bits 0-8 MSB first)
        out[0] = static_cast<uint8_t>((new_mdb >> 1) & 0xFF);
        out[1] = static_cast<uint8_t>((out[1] & 0x7F) | ((new_mdb & 1) << 7));
    } else {
        // main_data_begin is 8 bits
        out[0] = static_cast<uint8_t>(new_mdb & 0xFF);
    }
    return out;
}

static std::vector<uint8_t> serialize_side_info(const Mp3Header& h, const SideInfo& si) {
    BitstreamWriter writer;
    const int n_ch = (h.channel_mode == ChannelMode::Mono) ? 1 : 2;
    const int n_gr = (h.version == MpegVersion::MPEG1) ? 2 : 1;

    writer.write_bits(si.main_data_begin, h.version == MpegVersion::MPEG1 ? 9 : 8);
    writer.write_bits(si.private_bits, h.channel_mode == ChannelMode::Mono ? 5 : 3);
    
    if (h.version == MpegVersion::MPEG1) {
        for (int ch = 0; ch < n_ch; ++ch)
            for (int b = 0; b < 4; ++b)
                writer.write_bits(si.scfsi[ch][b], 1);
    }

    for (int gr = 0; gr < n_gr; ++gr) {
        for (int ch = 0; ch < n_ch; ++ch) {
            const GrChInfo& g = si.gr[gr][ch];
            writer.write_bits(g.part2_3_length, 12);
            writer.write_bits(g.big_values, 9);
            writer.write_bits(g.global_gain, 8);
            writer.write_bits(g.scalefac_compress, h.version == MpegVersion::MPEG1 ? 4 : 9);
            writer.write_bits(g.window_switching_flag, 1);
            if (g.window_switching_flag) {
                writer.write_bits(g.block_type, 2);
                writer.write_bits(g.mixed_block_flag, 1);
                writer.write_bits(g.table_select[0], 5);
                writer.write_bits(g.table_select[1], 5);
                for (const uint8_t i : g.subblock_gain) {
                    writer.write_bits(i, 3);
                }
            } else {
                writer.write_bits(g.table_select[0], 5);
                writer.write_bits(g.table_select[1], 5);
                writer.write_bits(g.table_select[2], 5);
                writer.write_bits(g.region0_count, 4);
                writer.write_bits(g.region1_count, 3);
            }
            // preflag, scalefac_scale, count1table_select follow for both window types
            if (h.version == MpegVersion::MPEG1) writer.write_bits(g.preflag, 1);
            writer.write_bits(g.scalefac_scale, 1);
            writer.write_bits(g.count1table_select, 1);
        }
    }
    while (writer.tell_bit() % 8 != 0) writer.write_bits(0, 1);
    return writer.data();
}

static uint32_t get_samplerate_index(const MpegVersion v, const uint32_t sr) {
    if (v == MpegVersion::MPEG1) {
        if (sr == 44100) return 0;
        if (sr == 48000) return 1;
        if (sr == 32000) return 2;
    } else if (v == MpegVersion::MPEG2) {
        if (sr == 22050) return 0;
        if (sr == 24000) return 1;
        if (sr == 16000) return 2;
    } else {
        if (sr == 11025) return 0;
        if (sr == 12000) return 1;
        if (sr == 8000)  return 2;
    }
    return 0;
}

static bool is_xing_frame(const Mp3Frame& frame) {
    std::vector<uint8_t> full_frame;
    full_frame.insert(full_frame.end(), frame.side_info_raw.begin(), frame.side_info_raw.end());
    full_frame.insert(full_frame.end(), frame.main_data_raw.begin(), frame.main_data_raw.end());
    if (full_frame.size() < 100) return false;
    const std::string s(full_frame.begin(), full_frame.begin() + std::min(static_cast<size_t>(100), full_frame.size()));
    return (s.find("Xing") != std::string::npos || s.find("Info") != std::string::npos);
}

struct ChosenBitrate {
    bool padding;
    int data_size;
    int index;
};

static ChosenBitrate bytes_to_bitrate(MpegVersion version, uint32_t samplerate, ChannelMode channel_mode, int bytes) {
    static const int mpeg1_bitrates[] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320};
    static const int mpeg2_bitrates[] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160};
    
    const int* bitrates = (version == MpegVersion::MPEG1) ? mpeg1_bitrates : mpeg2_bitrates;
    int side_info_size = (version == MpegVersion::MPEG1) ?
                         (channel_mode == ChannelMode::Mono ? 17 : 32) :
                         (channel_mode == ChannelMode::Mono ? 9 : 17);
                         
    for (int index = 1; index <= 14; ++index) {
        int br = bitrates[index];
        // unpadded frame size
        int unpadded_frame_size = 0;
        if (version == MpegVersion::MPEG1) {
            unpadded_frame_size = (144 * br * 1000 / static_cast<int>(samplerate));
        } else {
            unpadded_frame_size = (72 * br * 1000 / static_cast<int>(samplerate));
        }
        
        int unpadded_data_size = unpadded_frame_size - 4 - side_info_size;
        if (unpadded_data_size >= bytes) {
            return {false, unpadded_data_size, index};
        }
        if (unpadded_data_size + 1 >= bytes) {
            return {true, unpadded_data_size + 1, index};
        }
    }
    
    // fallback to max bitrate if not found
    int max_br = bitrates[14];
    int max_frame_size = (version == MpegVersion::MPEG1) ?
                         (144 * max_br * 1000 / static_cast<int>(samplerate)) + 1 :
                         (72 * max_br * 1000 / static_cast<int>(samplerate)) + 1;
    return {true, max_frame_size - 4 - side_info_size, 14};
}

void Packer::process(const std::string& input_file, const std::string& output_file) const {
    DEBUG_LOG("Reading input file " << input_file << "...");
    Mp3Reader reader(input_file);

    std::vector<Mp3Frame> all_frames;
    while (auto opt_frame = reader.read_next_frame()) {
        all_frames.push_back(*opt_frame);
    }

    if (all_frames.empty()) {
        throw std::runtime_error("No valid MP3 frames found!");
    }

    bool has_xing = false;
    // removed unused xing_frame here
    if (is_xing_frame(all_frames[0])) {
        has_xing = true;
        // do not erase it! we must keep it to preserve the lame tag!
        // all_frames.erase(all_frames.begin());
        DEBUG_LOG("Detected XING header frame at beginning. Preserving it.");
    }
    
    size_t N = all_frames.size();
    DEBUG_LOG("Optimizing " << N << " audio frames...");

    std::vector<std::vector<uint8_t>> optimized_main_data(N);
    std::vector<SideInfo> optimized_side_info(N);
    std::vector<bool> frame_was_optimized(N, false);
    
    std::vector<uint8_t> global_res;
    std::vector<size_t> frame_main_starts(N);
    for (size_t i = 0; i < N; ++i) {
        frame_main_starts[i] = global_res.size();
        global_res.insert(global_res.end(), all_frames[i].main_data_raw.begin(), all_frames[i].main_data_raw.end());
    }

    std::atomic<size_t> current_frame{0};
    unsigned int active_threads = std::max(1u, std::thread::hardware_concurrency());

    auto worker = [&]() {
        while (true) {
            size_t i = current_frame.fetch_add(1);
            if (i >= N) break;

            if (has_xing && i == 0) {
                // output frames are always written without CRC, so "Xing"/"Info" must be at
                // output mdr[0..3].
                //   No-CRC file: copy mdr verbatim (extended headers like "Lavf" at byte 120+
                //                contain gapless timing info that decoders rely on).
                //   CRC file:    "Xi"/"In" is in si[-2:], "ng"/"fo" at mdr[0].
                //                Prepend si[-2:] and copy standard Xing content (through LAME
                //                extension if present), trimming trailing zeros to save space.
                const auto& mdr = all_frames[i].main_data_raw;
                if (!all_frames[i].header.has_crc) {
                    optimized_main_data[i].assign(mdr.begin(), mdr.end());
                } else {
                    // CRC: recover "Xi"/"In" from the last 2 bytes of the original side_info
                    const auto& sir = all_frames[i].side_info_raw;
                    uint8_t b0 = (sir.size() >= 2) ? sir[sir.size() - 2] : 0x58;
                    uint8_t b1 = (sir.size() >= 1) ? sir[sir.size() - 1] : 0x69;
                    // mdr[0..1]="ng"/"fo", content (flags, counts...) at mdr[2..]
                    size_t keep = 2;
                    if (mdr.size() >= 6) {
                        uint32_t fl = (static_cast<uint32_t>(mdr[2]) << 24) |
                                      (static_cast<uint32_t>(mdr[3]) << 16) |
                                      (static_cast<uint32_t>(mdr[4]) << 8) |
                                      static_cast<uint32_t>(mdr[5]);
                        keep += 4; // flags word
                        if (fl & 1) keep += 4;   // frame count
                        if (fl & 2) keep += 4;   // byte count
                        if (fl & 4) keep += 100; // TOC
                        if (fl & 8) keep += 4;   // quality
                        if (mdr.size() >= keep + 4 &&
                            mdr[keep] == 'L' && mdr[keep+1] == 'A' &&
                            mdr[keep+2] == 'M' && mdr[keep+3] == 'E')
                            keep += 36; // LAME extension tag
                    }
                    keep = std::min(keep, mdr.size());
                    optimized_main_data[i] = {b0, b1};
                    optimized_main_data[i].insert(optimized_main_data[i].end(),
                                                  mdr.begin(), mdr.begin() + static_cast<ptrdiff_t>(keep));
                }
                SideInfo clean_si{};
                optimized_side_info[i] = clean_si;
                continue;
            }
        auto& frame = all_frames[i];

        int n_ch = (frame.header.channel_mode == ChannelMode::Mono) ? 1 : 2;
        int n_gr = (frame.header.version == MpegVersion::MPEG1) ? 2 : 1;

        BitstreamReader data_reader(global_res);
        size_t frame_main_start = frame_main_starts[i];
        size_t start_byte = (frame_main_start >= static_cast<size_t>(frame.side_info.main_data_begin)) ? (frame_main_start - static_cast<size_t>(frame.side_info.main_data_begin)) : 0;
        data_reader.seek_bit(start_byte * 8);

        BitstreamWriter writer;
        SideInfo side_copy = frame.side_info; // backup in case we fallback
        bool optimization_failed = false;
        
        try {
            if (!this->recompress_huffman) {
                optimization_failed = true; // fallback to raw copy of original data
            } else {
                for (int gr = 0; gr < n_gr; ++gr) {
                for (int ch = 0; ch < n_ch; ++ch) {
                    GrChInfo& g = frame.side_info.gr[gr][ch];
                    size_t gc_orig_start = data_reader.tell_bit();

                    // read scalefactors
                    int slen1 = 0, slen2 = 0;
                    if (frame.header.version == MpegVersion::MPEG1) {
                        static const int slen1_tab[] = {0, 0, 0, 0, 3, 1, 1, 1, 2, 2, 2, 3, 3, 3, 4, 4};
                        static const int slen2_tab[] = {0, 1, 2, 3, 0, 1, 2, 3, 1, 2, 3, 1, 2, 3, 2, 3};
                        slen1 = slen1_tab[g.scalefac_compress];
                        slen2 = slen2_tab[g.scalefac_compress];
                    }
                    
                    std::vector<int> scfs;
                    if (g.window_switching_flag && g.block_type == 2) {
                        if (g.mixed_block_flag) {
                            // 8 long-window sfb (0-7) at slen1
                            for (int k = 0; k < 8; ++k) scfs.push_back(data_reader.read_bits(slen1));
                            // short sfb 3-5: 3 bands × 3 windows at slen1
                            for (int k = 0; k < 9; ++k) scfs.push_back(data_reader.read_bits(slen1));
                            // short sfb 6-11: 6 bands × 3 windows at slen2
                            for (int k = 0; k < 18; ++k) scfs.push_back(data_reader.read_bits(slen2));
                        } else {
                            for (int k = 0; k < 18; ++k) scfs.push_back(data_reader.read_bits(slen1));
                            for (int k = 18; k < 36; ++k) scfs.push_back(data_reader.read_bits(slen2));
                        }
                    } else {
                        for (int k = 0; k < 6; ++k) {
                            if (gr == 0 || !frame.side_info.scfsi[ch][0]) scfs.push_back(data_reader.read_bits(slen1));
                            else scfs.push_back(0);
                        }
                        for (int k = 6; k < 11; ++k) {
                            if (gr == 0 || !frame.side_info.scfsi[ch][1]) scfs.push_back(data_reader.read_bits(slen1));
                            else scfs.push_back(0);
                        }
                        for (int k = 11; k < 16; ++k) {
                            if (gr == 0 || !frame.side_info.scfsi[ch][2]) scfs.push_back(data_reader.read_bits(slen2));
                            else scfs.push_back(0);
                        }
                        for (int k = 16; k < 21; ++k) {
                            if (gr == 0 || !frame.side_info.scfsi[ch][3]) scfs.push_back(data_reader.read_bits(slen2));
                            else scfs.push_back(0);
                        }
                    }

                    // decode & optimize huffman
                    HuffmanConfig orig_cfg = {
                        g.region0_count,
                        g.region1_count,
                        g.big_values,
                        g.table_select[0],
                        g.table_select[1],
                        g.table_select[2],
                        g.count1table_select,
                        g.window_switching_flag,
                        g.block_type,
                        g.mixed_block_flag
                    };
                    int scalefac_bits = static_cast<int>(data_reader.tell_bit() - gc_orig_start);
                    int max_huff_bits = side_copy.gr[gr][ch].part2_3_length - scalefac_bits;
                    auto coeffs = HuffmanOptimizer::decode_quantized_coefficients(orig_cfg, data_reader, frame.header.samplerate, max_huff_bits);
                    auto best_cfg = HuffmanOptimizer::find_best_config(coeffs, orig_cfg, frame.header.samplerate);
                    
                    // re-encode
                    size_t out_start = writer.tell_bit();
                    if (g.window_switching_flag && g.block_type == 2) {
                        if (g.mixed_block_flag) {
                            for (int k = 0; k < 8; ++k) writer.write_bits(scfs[k], slen1);
                            for (int k = 8; k < 17; ++k) writer.write_bits(scfs[k], slen1);
                            for (int k = 17; k < 35; ++k) writer.write_bits(scfs[k], slen2);
                        } else {
                            for (int k = 0; k < 18; ++k) writer.write_bits(scfs[k], slen1);
                            for (int k = 18; k < 36; ++k) writer.write_bits(scfs[k], slen2);
                        }
                    } else {
                        for (int k = 0; k < 6; ++k) {
                            if (gr == 0 || !frame.side_info.scfsi[ch][0]) writer.write_bits(scfs[k], slen1);
                        }
                        for (int k = 6; k < 11; ++k) {
                            if (gr == 0 || !frame.side_info.scfsi[ch][1]) writer.write_bits(scfs[k], slen1);
                        }
                        for (int k = 11; k < 16; ++k) {
                            if (gr == 0 || !frame.side_info.scfsi[ch][2]) writer.write_bits(scfs[k], slen2);
                        }
                        for (int k = 16; k < 21; ++k) {
                            if (gr == 0 || !frame.side_info.scfsi[ch][3]) writer.write_bits(scfs[k], slen2);
                        }
                    }
                    
                    size_t scalefac_end_bit = writer.tell_bit();
                    HuffmanOptimizer::encode_quantized_coefficients(coeffs, best_cfg, writer, frame.header.samplerate);

                    // round-trip verification: re-decode the bytes we just wrote and compare
                    {
                        int huff_bits = static_cast<int>(writer.tell_bit() - scalefac_end_bit);
                        BitstreamReader vrdr(writer.data());
                        vrdr.seek_bit(scalefac_end_bit);
                        auto vcoeffs = HuffmanOptimizer::decode_quantized_coefficients(best_cfg, vrdr, frame.header.samplerate, huff_bits);
                        for (int ci = 0; ci < 576; ++ci) {
                            if (vcoeffs[ci] != coeffs[ci]) {
                                optimization_failed = true;
                                break;
                            }
                        }
                    }

                    g.big_values = best_cfg.big_values;
                    g.region0_count = best_cfg.region0_count;
                    g.region1_count = best_cfg.region1_count;
                    g.table_select[0] = best_cfg.table0;
                    g.table_select[1] = best_cfg.table1;
                    g.table_select[2] = best_cfg.table2;
                    g.count1table_select = best_cfg.count1_table_select;
                    
                    int orig_part2_3 = side_copy.gr[gr][ch].part2_3_length;
                    g.part2_3_length = static_cast<uint16_t>(writer.tell_bit() - out_start);

                    data_reader.seek_bit(gc_orig_start + orig_part2_3);
                }
            }
            }
        } catch (const std::exception&) {
            optimization_failed = true;
        }

        int total_orig_bits = 0;
        for (int gr = 0; gr < n_gr; ++gr) {
            for (int ch = 0; ch < n_ch; ++ch) {
                total_orig_bits += side_copy.gr[gr][ch].part2_3_length;
            }
        }
        int total_orig_bytes = (total_orig_bits + 7) / 8;

        std::vector<uint8_t> new_main = writer.data();
        if (optimization_failed || new_main.size() >= static_cast<size_t>(total_orig_bytes)) {
            // fallback: no improvement or failure — preserve original side_info bits via patch_mdb
            new_main.assign(global_res.begin() + start_byte, global_res.begin() + start_byte + total_orig_bytes);
            frame.side_info = side_copy;
        } else {
            // strictly smaller: use compressed data and re-serialize updated side_info
            frame_was_optimized[i] = true;
        }

        optimized_main_data[i] = new_main;
        optimized_side_info[i] = frame.side_info;
        }
    };

    std::vector<std::thread> threads;
    for (unsigned int t = 0; t < active_threads; ++t) {
        threads.emplace_back(worker);
    }
    for (auto& thread : threads) {
        thread.join();
    }
    
    DEBUG_LOG("Huffman optimization completed. Running reservoir constraint solver...");
    
    // backward pass (constraint solver for carry-over reservoir)
    MpegVersion version = all_frames[0].header.version;
    uint32_t samplerate = all_frames[0].header.samplerate;
    ChannelMode channel_mode = all_frames[0].header.channel_mode;
    
    DEBUG_LOG("Reservoir carryover calculated. Writing output stream...");
    
    std::ofstream out(output_file, std::ios::binary);
    if (!out) {
        throw std::runtime_error("Could not open output file: " + output_file);
    }
    
    // write start junk (id3v2)
    const auto& start_junk = reader.get_start_junk();
    if (!start_junk.empty()) {
        out.write(reinterpret_cast<const char*>(start_junk.data()), static_cast<std::streamsize>(start_junk.size()));
    }
    
    // track where the xing frame starts in the file
    const size_t xing_header_file_pos = has_xing ? static_cast<size_t>(out.tellp()) : SIZE_MAX;
    
    // backward pass constraint solver
    std::vector<int> required_carryover(N, 0);
    int current_req = 0;
    for (int i = static_cast<int>(N) - 1; i >= 0; --i) {
        int space_needed = static_cast<int>(optimized_main_data[i].size()) + current_req;
        
        // we allow the forward pass to use up to 320 kbps if needed, so the backward pass should assume max capacity.
        ChosenBitrate max_br = bytes_to_bitrate(version, samplerate, channel_mode, 9999);
        int max_data_per_frame = max_br.data_size;
        
        current_req = std::max(0, space_needed - max_data_per_frame);
        required_carryover[i] = current_req;
    }
    std::vector<uint8_t> global_main_data;
    int current_reservoir = 0; // free space in past payloads
    
    std::vector<ChosenBitrate> chosen_bitrates(N);
    
    // pass 1: build global_main_data and calculate main_data_begin
    for (size_t i = 0; i < N; ++i) {
        auto& side = optimized_side_info[i];

        int max_reservoir = (version == MpegVersion::MPEG1) ? 511 : 255;

        ChosenBitrate bitrate_use;
        if (has_xing && i == 0) {
            // use minimum bitrate that fits the trimmed Xing tag content
            int needed = static_cast<int>(optimized_main_data[0].size());
            bitrate_use = bytes_to_bitrate(version, samplerate, channel_mode, needed);
        } else {
            int req_for_next = (i + 1 < N) ? required_carryover[i + 1] : 0;
            int bytes_needed = std::max(0, static_cast<int>(optimized_main_data[i].size()) + req_for_next - current_reservoir);
            bitrate_use = bytes_to_bitrate(version, samplerate, channel_mode, bytes_needed);
        }

        int data_size = bitrate_use.data_size;

        // if this frame would push reservoir past 511, absorb excess as ancillary zeros
        // (guaranteed to fit: excess = proj - max <= data_size - main_size since current_reservoir <= max)
        int projected_reservoir = current_reservoir + data_size - static_cast<int>(optimized_main_data[i].size());
        if (projected_reservoir > max_reservoir) {
            int excess = projected_reservoir - max_reservoir;
            optimized_main_data[i].resize(optimized_main_data[i].size() + excess, 0x00);
            projected_reservoir = max_reservoir;
        }

        // for the Xing frame, pad to data_size before pushing so pass 2
        // doesn't read audio bytes to fill the remainder of the payload
        if (has_xing && i == 0 && static_cast<int>(optimized_main_data[0].size()) < data_size)
            optimized_main_data[0].resize(static_cast<size_t>(data_size), 0x00);

        side.main_data_begin = static_cast<uint16_t>(current_reservoir);
        global_main_data.insert(global_main_data.end(), optimized_main_data[i].begin(), optimized_main_data[i].end());
        current_reservoir = projected_reservoir;
        chosen_bitrates[i] = bitrate_use;

        if (has_xing && i == 0)
            current_reservoir = 0;
    }
    
    if (current_reservoir > 0) {
        std::vector<uint8_t> end_pad(static_cast<size_t>(current_reservoir), 0x00);
        global_main_data.insert(global_main_data.end(), end_pad.begin(), end_pad.end());
    }
    
    // pass 2: write physical file
    // find the last frame that actually contains non-zero data
    size_t N_out = N;
    for (int i = static_cast<int>(N) - 1; i >= 0; --i) {
        if (chosen_bitrates[i].index == 14) {
            break;
        }
        if (optimized_main_data[i].size() > 0) {
            break;
        }
        if (i == 0) break;
        N_out = static_cast<size_t>(i);
    }
    
    size_t global_main_data_ptr = 0;

    // audio frame byte positions in the output file (index 0 = first audio frame, i.e. frame 1 if has_xing)
    std::vector<size_t> audio_frame_file_pos;

    for (size_t i = 0; i < N_out; ++i) {
        const auto& frame = all_frames[i];
        const auto& side = optimized_side_info[i];

        if (has_xing && i > 0)
            audio_frame_file_pos.push_back(static_cast<size_t>(out.tellp()));

        ChosenBitrate bitrate_use = chosen_bitrates[i];

        uint32_t h = 0xFFE00000;
        h |= (static_cast<uint32_t>(version == MpegVersion::MPEG1 ? 3 : 2) << 19);
        h |= (1 << 17) | (1 << 16);
        h |= (static_cast<uint32_t>(bitrate_use.index) << 12) | (get_samplerate_index(version, samplerate) << 10);
        h |= static_cast<uint32_t>(bitrate_use.padding ? 1 : 0) << 9 | (static_cast<uint32_t>(channel_mode) << 6);
        uint32_t mode_ext = 0;
        if (channel_mode == ChannelMode::JointStereo) {
            mode_ext = (frame.header.ms_stereo ? 2 : 0) | (frame.header.is_stereo ? 1 : 0);
        }
        h |= mode_ext << 4;
        h |= static_cast<uint32_t>(frame.header.copyright ? 1 : 0) << 3;
        h |= static_cast<uint32_t>(frame.header.original ? 1 : 0) << 2;
        h |= static_cast<uint32_t>(frame.header.emphasis);

        uint8_t head[4];
        head[0] = static_cast<uint8_t>(h >> 24); head[1] = static_cast<uint8_t>(h >> 16); head[2] = static_cast<uint8_t>(h >> 8); head[3] = static_cast<uint8_t>(h);

        std::vector<uint8_t> new_side;
        if (has_xing && i == 0) {
            new_side.assign(frame.side_info_raw.size(), 0);
        } else if (frame_was_optimized[i]) {
            new_side = serialize_side_info(frame.header, side);
        } else {
            new_side = patch_mdb(frame.side_info_raw, version, side.main_data_begin);
        }

        out.write(reinterpret_cast<const char*>(head), 4);
        out.write(reinterpret_cast<const char*>(new_side.data()), static_cast<std::streamsize>(new_side.size()));

        int data_size = bitrate_use.data_size;
        int avail = static_cast<int>(global_main_data.size() - global_main_data_ptr);
        int write_len = std::min(data_size, avail);

        if (write_len > 0) {
            out.write(reinterpret_cast<const char*>(&global_main_data[global_main_data_ptr]), write_len);
            global_main_data_ptr += static_cast<size_t>(write_len);
        }

        if (data_size > write_len) {
            std::vector<uint8_t> pad(static_cast<size_t>(data_size - write_len), 0x00);
            out.write(reinterpret_cast<const char*>(pad.data()), static_cast<std::streamsize>(pad.size()));
        }
    }
    
    // save position before end_junk to calculate total mp3 stream size
    size_t mp3_end_pos = static_cast<size_t>(out.tellp());
    
    // write end junk (id3v1)
    const auto& end_junk = reader.get_end_junk();
    if (!end_junk.empty()) {
        out.write(reinterpret_cast<const char*>(end_junk.data()), static_cast<std::streamsize>(end_junk.size()));
    }
    
    // patch xing header fields (bytes count + TOC) if present
    if (has_xing) {
        // optimized_main_data[0] always has "Xing"/"Info" at bytes 0-3, flags at 4-7
        // (regardless of whether the source was CRC or no-CRC)
        const auto& xing_out = optimized_main_data[0];
        if (xing_out.size() >= 8) {
            uint32_t flags = (static_cast<uint32_t>(xing_out[4]) << 24) |
                             (static_cast<uint32_t>(xing_out[5]) << 16) |
                             (static_cast<uint32_t>(xing_out[6]) << 8) |
                             static_cast<uint32_t>(xing_out[7]);

            int side_info_size = (version == MpegVersion::MPEG1) ?
                                 (channel_mode == ChannelMode::Mono ? 17 : 32) :
                                 (channel_mode == ChannelMode::Mono ? 9 : 17);
            // base = start of output main_data = after header(4) + side_info
            size_t main_data_base = xing_header_file_pos + 4 + static_cast<size_t>(side_info_size);
            // in output: 'Xing'(4) + flags(4) = offset 8
            size_t field_offset = 8;

            size_t total_mp3_bytes = mp3_end_pos - xing_header_file_pos;

            if (flags & 1) field_offset += 4; // frames field: skip it
            if (flags & 2) {
                out.seekp(static_cast<std::streamoff>(main_data_base + field_offset), std::ios::beg);
                uint8_t buf[4];
                buf[0] = (total_mp3_bytes >> 24) & 0xFF;
                buf[1] = (total_mp3_bytes >> 16) & 0xFF;
                buf[2] = (total_mp3_bytes >> 8) & 0xFF;
                buf[3] = (total_mp3_bytes >> 0) & 0xFF;
                out.write(reinterpret_cast<const char*>(buf), 4);
                field_offset += 4;
            }

            if ((flags & 4) && !audio_frame_file_pos.empty()) {
                // recalculate 100-entry TOC; toc[0] is always 0 (LAME convention)
                size_t n_audio = audio_frame_file_pos.size();
                uint8_t toc[100];
                toc[0] = 0;
                for (int k = 1; k < 100; ++k) {
                    size_t fi = (static_cast<size_t>(k) * n_audio) / 100;
                    if (fi >= n_audio) fi = n_audio - 1;
                    size_t pos = audio_frame_file_pos[fi] - xing_header_file_pos;
                    int v = static_cast<int>(256.0 * static_cast<double>(pos) / static_cast<double>(total_mp3_bytes));
                    toc[k] = static_cast<uint8_t>(std::min(v, 255));
                }
                out.seekp(static_cast<std::streamoff>(main_data_base + field_offset), std::ios::beg);
                out.write(reinterpret_cast<const char*>(toc), 100);
            }
        }
    }
}

} // namespace mp3packer
