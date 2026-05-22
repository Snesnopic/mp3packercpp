#include "packer.hpp"
#include "huffman.hpp"
#include "logger.hpp"
#include "mp3_reader.hpp"
#include "huffman.hpp"
#include "bitstream.hpp"
#include <iostream>
#include <algorithm>
#include <vector>
#include <fstream>
#include <stdexcept>

namespace mp3packer {

Packer::Packer() = default;

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
                for (const int i : g.subblock_gain) {
                    writer.write_bits(i, 3);
                }
            } else {
                writer.write_bits(g.table_select[0], 5);
                writer.write_bits(g.table_select[1], 5);
                writer.write_bits(g.table_select[2], 5);
                writer.write_bits(g.region0_count, 4);
                writer.write_bits(g.region1_count, 3);
            }
            if (h.version == MpegVersion::MPEG1) writer.write_bits(g.preflag, 1);
            writer.write_bits(g.scalefac_scale, 1);
            writer.write_bits(g.count1table_select, 1);
        }
    }
    while (writer.tell_bit() % 8 != 0) writer.write_bits(0, 1);
    return writer.data();
}

static uint32_t get_samplerate_index(const MpegVersion v, const int sr) {
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
    if (frame.main_data_raw.size() < 100) return false;
    const std::string s(frame.main_data_raw.begin(), frame.main_data_raw.begin() + std::min(static_cast<size_t>(100), frame.main_data_raw.size()));
    return (s.find("Xing") != std::string::npos || s.find("Info") != std::string::npos);
}

struct ChosenBitrate {
    bool padding;
    int data_size;
    int index;
};

static ChosenBitrate bytes_to_bitrate(MpegVersion version, int samplerate, ChannelMode channel_mode, int bytes) {
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
            unpadded_frame_size = (144 * br * 1000 / samplerate);
        } else {
            unpadded_frame_size = (72 * br * 1000 / samplerate);
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
                         (144 * max_br * 1000 / samplerate) + 1 :
                         (72 * max_br * 1000 / samplerate) + 1;
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
    
    std::vector<uint8_t> global_res;
    std::vector<size_t> frame_main_starts(N);
    for (size_t i = 0; i < N; ++i) {
        frame_main_starts[i] = global_res.size();
        global_res.insert(global_res.end(), all_frames[i].main_data_raw.begin(), all_frames[i].main_data_raw.end());
    }

    #pragma omp parallel for
    for (int i = 0; i < static_cast<int>(N); ++i) {
        if (has_xing && i == 0) {
            optimized_main_data[i] = all_frames[i].main_data_raw;
            optimized_side_info[i] = all_frames[i].side_info;
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
                        if (g.window_switching_flag && g.block_type == 2) {
                            if (g.mixed_block_flag) {
                                static const int slen1_tab[] = {0, 1, 2, 2, 0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3};
                                static const int slen2_tab[] = {0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3};
                                slen1 = slen1_tab[g.scalefac_compress];
                                slen2 = slen2_tab[g.scalefac_compress];
                            } else {
                                static const int slen1_tab[] = {0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3};
                                static const int slen2_tab[] = {0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3};
                                slen1 = slen1_tab[g.scalefac_compress];
                                slen2 = slen2_tab[g.scalefac_compress];
                            }
                        } else {
                            static const int slen1_tab[] = {0, 0, 0, 0, 3, 1, 1, 1, 2, 2, 2, 3, 3, 3, 4, 4};
                            static const int slen2_tab[] = {0, 1, 2, 3, 0, 1, 2, 3, 1, 2, 3, 1, 2, 3, 2, 3};
                            slen1 = slen1_tab[g.scalefac_compress];
                            slen2 = slen2_tab[g.scalefac_compress];
                        }
                    }
                    
                    std::vector<int> scfs;
                    if (g.window_switching_flag && g.block_type == 2) {
                        if (g.mixed_block_flag) { // NOLINT(bugprone-branch-clone)
                            for (int k = 0; k < 8; ++k) scfs.push_back(data_reader.read_bits(slen1));
                            for (int k = 8; k < 11; ++k) scfs.push_back(data_reader.read_bits(slen1));
                            for (int k = 11; k < 17; ++k) scfs.push_back(data_reader.read_bits(slen2));
                        } else {
                            for (int k = 0; k < 18; ++k) scfs.push_back(data_reader.read_bits(slen1));
                            for (int k = 18; k < 36; ++k) scfs.push_back(data_reader.read_bits(slen2));
                        }
                    } else {
                        for (int k = 0; k < 6; ++k) {
                            if (gr == 0 || frame.side_info.scfsi[ch][0] == 0) scfs.push_back(data_reader.read_bits(slen1));
                            else scfs.push_back(0);
                        }
                        for (int k = 6; k < 11; ++k) {
                            if (gr == 0 || frame.side_info.scfsi[ch][1] == 0) scfs.push_back(data_reader.read_bits(slen1));
                            else scfs.push_back(0);
                        }
                        for (int k = 11; k < 16; ++k) {
                            if (gr == 0 || frame.side_info.scfsi[ch][2] == 0) scfs.push_back(data_reader.read_bits(slen2));
                            else scfs.push_back(0);
                        }
                        for (int k = 16; k < 21; ++k) {
                            if (gr == 0 || frame.side_info.scfsi[ch][3] == 0) scfs.push_back(data_reader.read_bits(slen2));
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
                        g.count1table_select != 0,
                        g.window_switching_flag != 0,
                        g.block_type,
                        g.mixed_block_flag != 0
                    };
                    auto coeffs = HuffmanOptimizer::decode_quantized_coefficients(orig_cfg, data_reader, frame.header.samplerate);
                    auto best_cfg = HuffmanOptimizer::find_best_config(coeffs, orig_cfg, frame.header.samplerate);
                    
                    // re-encode
                    size_t out_start = writer.tell_bit();
                    if (g.window_switching_flag && g.block_type == 2) {
                        if (g.mixed_block_flag) {
                            for (int k = 0; k < 8; ++k) writer.write_bits(scfs[k], slen1);
                            for (int k = 8; k < 11; ++k) writer.write_bits(scfs[k], slen1);
                            for (int k = 11; k < 17; ++k) writer.write_bits(scfs[k], slen2);
                        } else {
                            for (int k = 0; k < 18; ++k) writer.write_bits(scfs[k], slen1);
                            for (int k = 18; k < 36; ++k) writer.write_bits(scfs[k], slen2);
                        }
                    } else {
                        for (int k = 0; k < 6; ++k) {
                            if (gr == 0 || frame.side_info.scfsi[ch][0] == 0) writer.write_bits(scfs[k], slen1);
                        }
                        for (int k = 6; k < 11; ++k) {
                            if (gr == 0 || frame.side_info.scfsi[ch][1] == 0) writer.write_bits(scfs[k], slen1);
                        }
                        for (int k = 11; k < 16; ++k) {
                            if (gr == 0 || frame.side_info.scfsi[ch][2] == 0) writer.write_bits(scfs[k], slen2);
                        }
                        for (int k = 16; k < 21; ++k) {
                            if (gr == 0 || frame.side_info.scfsi[ch][3] == 0) writer.write_bits(scfs[k], slen2);
                        }
                    }
                    HuffmanOptimizer::encode_quantized_coefficients(coeffs, best_cfg, writer, frame.header.samplerate);
                    
                    // update side info
                    g.big_values = best_cfg.big_values;
                    g.region0_count = best_cfg.region0_count;
                    g.region1_count = best_cfg.region1_count;
                    g.table_select[0] = best_cfg.table0;
                    g.table_select[1] = best_cfg.table1;
                    g.table_select[2] = best_cfg.table2;
                    g.count1table_select = best_cfg.count1_table_select ? 1 : 0;
                    
                    g.part2_3_length = static_cast<int>(writer.tell_bit() - out_start);
                    
                    data_reader.seek_bit(gc_orig_start + frame.side_info.gr[gr][ch].part2_3_length);
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
        if (optimization_failed || new_main.size() > static_cast<size_t>(total_orig_bytes)) {
            // fallback to original main data
            new_main.assign(global_res.begin() + start_byte, global_res.begin() + start_byte + total_orig_bytes);
            frame.side_info = side_copy;
        }

        optimized_main_data[i] = new_main;
        optimized_side_info[i] = frame.side_info;
    }
    
    DEBUG_LOG("Huffman optimization completed. Running reservoir constraint solver...");
    
    // backward pass (constraint solver for carry-over reservoir)
    MpegVersion version = all_frames[0].header.version;
    int samplerate = all_frames[0].header.samplerate;
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
    size_t xing_header_file_pos = out.tellp(); // usually right after id3v2
    if (!has_xing) xing_header_file_pos = 0;
    
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
        
        if (current_reservoir > max_reservoir) {
            int stuffing_len = current_reservoir - max_reservoir;
            for (int j = 0; j < stuffing_len; ++j) {
                global_main_data.push_back(0xFF); // stuffing byte
            }
            current_reservoir = max_reservoir;
        }
        
        side.main_data_begin = static_cast<uint16_t>(current_reservoir);
        global_main_data.insert(global_main_data.end(), optimized_main_data[i].begin(), optimized_main_data[i].end());
        
        // vbr logic: pick the smallest bitrate that gives us enough data_size
        // we must ensure that we leave at least required_carryover[i + 1] in the reservoir for the next frames!
        int req_for_next = (i + 1 < N) ? required_carryover[i + 1] : 0;
        int bytes_to_store = std::max(0, static_cast<int>(optimized_main_data[i].size()) + req_for_next - current_reservoir);
        ChosenBitrate bitrate_use = bytes_to_bitrate(version, samplerate, channel_mode, bytes_to_store);
        
        int data_size = bitrate_use.data_size;
        current_reservoir = current_reservoir + data_size - static_cast<int>(optimized_main_data[i].size());
        
        chosen_bitrates[i] = bitrate_use;
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
    
    for (size_t i = 0; i < N_out; ++i) {
        const auto& frame = all_frames[i];
        const auto& side = optimized_side_info[i];
        
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

        std::vector<uint8_t> new_side = serialize_side_info(frame.header, side);
        
        if (has_xing && i == 0) {
            new_side = all_frames[i].side_info_raw;
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
    
    // patch xing header 'bytes' field if present
    if (has_xing && xing_header_file_pos != 0) {
        // scope reduced
        
        // find where the bytes field is in the xing frame
        const auto& xing_main_data = all_frames[0].main_data_raw;
        if (xing_main_data.size() >= 12) {
            uint32_t flags = (static_cast<uint32_t>(xing_main_data[4]) << 24) | 
                             (static_cast<uint32_t>(xing_main_data[5]) << 16) | 
                             (static_cast<uint32_t>(xing_main_data[6]) << 8) | 
                             static_cast<uint32_t>(xing_main_data[7]);
                             
            if (flags & 2) { // bytes field is present
                size_t bytes_offset = 8;
                if (flags & 1) { // frames field is also present before bytes
                    bytes_offset += 4;
                }
                
                // calculate physical file position of the bytes field
                // it is after the mp3 header (4 bytes) + side info size
                    int side_info_size = (version == MpegVersion::MPEG1) ? 
                                         (channel_mode == ChannelMode::Mono ? 17 : 32) :
                                         (channel_mode == ChannelMode::Mono ? 9 : 17);
                                     
                size_t physical_bytes_pos = xing_header_file_pos + 4 + static_cast<size_t>(side_info_size) + bytes_offset;
                
                // seek back and write the new size
                size_t total_mp3_bytes = mp3_end_pos - xing_header_file_pos; // size of the mp3 frame stream
                out.seekp(static_cast<std::streamoff>(physical_bytes_pos), std::ios::beg);
                uint8_t bytes_buf[4];
                bytes_buf[0] = (total_mp3_bytes >> 24) & 0xFF;
                bytes_buf[1] = (total_mp3_bytes >> 16) & 0xFF;
                bytes_buf[2] = (total_mp3_bytes >> 8) & 0xFF;
                bytes_buf[3] = (total_mp3_bytes >> 0) & 0xFF;
                out.write(reinterpret_cast<const char*>(bytes_buf), 4);
                
                DEBUG_LOG("Patched Xing header Bytes field to " << total_mp3_bytes << " bytes.");
            }
        }
    }
}

} // namespace mp3packer
