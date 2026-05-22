#include "huffman.hpp"
#include "huffman_tables.hpp"
#include <map>
#include <algorithm>
#include <iostream>
#include <array>

namespace mp3packer {

struct SymbolCode {
    uint32_t code = 0;
    int length = 0;
    bool valid = false;
};

class HuffmanTableManager {
public:
    static const std::array<SymbolCode, 256>& get_encoding_map(int table_idx) {
        static std::array<std::array<SymbolCode, 256>, 34> cache;
        static std::array<bool, 34> initialized{};
        if (!initialized[table_idx]) {
            cache[table_idx] = build_map(table_idx);
            initialized[table_idx] = true;
        }
        return cache[table_idx];
    }

private:
    static std::array<SymbolCode, 256> build_map(int table_idx) {
        std::array<SymbolCode, 256> arr{};
        for(auto& s : arr) s.valid = false;
        
        const int16_t* tab = huffman_tables[table_idx].table;
        if (*tab == 0) {
            arr[0] = {0, 0, true};
            return arr;
        }
        
        struct State { const int16_t* pos; uint32_t code; int length; };
        std::vector<State> stack;
        stack.push_back({tab, 0, 0});
        
        while (!stack.empty()) {
            State s = stack.back();
            stack.pop_back();
            
            int16_t val = *s.pos;
            if (val < 0) {
                stack.push_back({s.pos + 1, static_cast<uint32_t>(s.code << 1), static_cast<uint8_t>(s.length + 1)});
                stack.push_back({s.pos + 1 - val, static_cast<uint32_t>((s.code << 1) | 1), static_cast<uint8_t>(s.length + 1)});
            } else if (val < 256) {
                arr[val] = {s.code, static_cast<uint8_t>(s.length), true};
            }
        }
        return arr;
    }
};

static std::vector<int> get_sf_bands(int samplerate) {
    if (samplerate == 48000) return { 0, 4, 8, 12, 16, 20, 24, 30, 36, 42, 50, 60, 72, 88, 106, 128, 156, 190, 230, 276, 330, 384, 576 };
    if (samplerate == 44100) return { 0, 4, 8, 12, 16, 20, 24, 30, 36, 44, 52, 62, 74, 90, 110, 134, 162, 196, 238, 288, 342, 418, 576 };
    if (samplerate == 32000) return { 0, 4, 8, 12, 16, 20, 24, 30, 36, 44, 54, 66, 82, 102, 126, 156, 194, 240, 296, 364, 448, 550, 576 };
    if (samplerate == 24000) return { 0, 6, 12, 18, 24, 30, 36, 44, 54, 66, 80, 96, 114, 136, 162, 194, 232, 278, 332, 394, 464, 540, 576 };
    if (samplerate == 22050 || samplerate == 16000 || samplerate == 12000 || samplerate == 11025)
        return { 0, 6, 12, 18, 24, 30, 36, 44, 54, 66, 80, 96, 116, 140, 168, 200, 238, 284, 336, 396, 464, 522, 576 };
    if (samplerate == 8000) return { 0, 12, 24, 36, 48, 60, 72, 88, 108, 132, 160, 192, 232, 280, 336, 400, 476, 566, 568, 570, 572, 574, 576 };
    return { 0, 4, 8, 12, 16, 20, 24, 30, 36, 44, 52, 62, 74, 90, 110, 134, 162, 196, 238, 288, 342, 418, 576 };
}

static std::vector<int> get_sf_bands_short(int samplerate) {
    if (samplerate == 48000) return { 0, 4, 8, 12, 16, 22, 28, 38, 50, 64, 80, 100, 126, 192 };
    if (samplerate == 44100) return { 0, 4, 8, 12, 16, 22, 30, 40, 52, 66, 84, 106, 136, 192 };
    if (samplerate == 32000) return { 0, 4, 8, 12, 16, 22, 30, 42, 58, 78, 104, 138, 180, 192 };
    if (samplerate == 24000) return { 0, 4, 8, 12, 18, 26, 36, 48, 62, 80, 104, 136, 180, 192 };
    if (samplerate == 22050 || samplerate == 16000 || samplerate == 12000 || samplerate == 11025)
        return { 0, 4, 8, 12, 18, 24, 32, 42, 56, 74, 100, 132, 174, 192 };
    if (samplerate == 8000) return { 0, 8, 16, 24, 36, 52, 72, 96, 124, 160, 162, 164, 166, 192 };
    return { 0, 4, 8, 12, 16, 22, 30, 40, 52, 66, 84, 106, 136, 192 };
}

HuffmanOptimizer::HuffmanOptimizer() = default;

std::vector<int16_t> HuffmanOptimizer::decode_quantized_coefficients(const HuffmanConfig& config, BitstreamReader& reader, int samplerate) {
    std::vector<int16_t> coeffs(576, 0);
    int out_off = 0;
    const auto sf_bands = get_sf_bands(samplerate);

    auto decode_symbol = [&](const int table_idx) {
        if (table_idx == 0) return static_cast<int16_t>(0);
        const int16_t* tab = huffman_tables[table_idx].table;
        int16_t got;
        while ((got = *tab++) < 0) {
            if (reader.read_bits(1)) tab -= got;
        }
        return got;
    };

    auto decode_region = [&](int count_pairs, int table_idx) {
        int linbits = huffman_tables[table_idx].linbits;
        for (int i = 0; i < count_pairs && out_off < 575; ++i) {
            int got = decode_symbol(table_idx);
            int y = got & 0xF;
            int x = (got >> 4) & 0xF;
            if (x > 0) {
                if (x == 15 && linbits > 0) x += reader.read_bits(linbits);
                if (reader.read_bits(1)) x = -x;
            }
            if (y > 0) {
                if (y == 15 && linbits > 0) y += reader.read_bits(linbits);
                if (reader.read_bits(1)) y = -y;
            }
            coeffs[out_off++] = x;
            coeffs[out_off++] = y;
        }
    };

    int r0_pairs, r1_pairs, r2_pairs;
    if (config.window_switching_flag) {
        int region0;
        if (config.block_type == 2 && !config.mixed_block_flag) {
            auto sf_bands_short = get_sf_bands_short(samplerate);
            region0 = (sf_bands_short[3] / 2) * 3;
        } else {
            region0 = sf_bands[8] / 2;
        }
        r0_pairs = std::min(region0, config.big_values);
        r1_pairs = std::max(0, config.big_values - region0);
        r2_pairs = 0;
    } else {
        r0_pairs = std::min(config.big_values, sf_bands[config.region0_count + 1] / 2);
        r1_pairs = std::min(config.big_values, sf_bands[config.region0_count + config.region1_count + 2] / 2) - r0_pairs;
        r2_pairs = config.big_values - r0_pairs - r1_pairs;
    }

    decode_region(r0_pairs, config.table0);
    decode_region(r1_pairs, config.table1);
    decode_region(r2_pairs, config.table2);

    int count1_table = config.count1_table_select ? 33 : 32;
    while (out_off <= 572 && !reader.eof()) {
        int got = decode_symbol(count1_table);
        if (got == 0 && reader.eof()) break;
        coeffs[out_off++] = (got & 8) ? (reader.read_bits(1) ? -1 : 1) : 0;
        coeffs[out_off++] = (got & 4) ? (reader.read_bits(1) ? -1 : 1) : 0;
        coeffs[out_off++] = (got & 2) ? (reader.read_bits(1) ? -1 : 1) : 0;
        coeffs[out_off++] = (got & 1) ? (reader.read_bits(1) ? -1 : 1) : 0;
    }
    return coeffs;
}

HuffmanConfig HuffmanOptimizer::find_best_config(const std::vector<int16_t>& coeffs, const HuffmanConfig& orig_config, int samplerate) {
    if (orig_config.window_switching_flag) {
        return orig_config;
    }

    HuffmanConfig best = orig_config;
    uint32_t min_total_bits = 0xFFFFFFFF;
    auto sf_bands = get_sf_bands(samplerate);

    // 1. Pre-calculate bit costs for each coefficient pair in every table
    std::vector<std::array<int, 32>> pair_costs(288);
    for (int p = 0; p < 288; ++p) {
        int x = std::abs(coeffs[2 * p]);
        int y = std::abs(coeffs[2 * p + 1]);
        for (int t = 0; t < 32; ++t) {
            if (max_quant_per_table[t] == -1) { pair_costs[p][t] = 10000; continue; }
            if (x > max_quant_per_table[t] || y > max_quant_per_table[t]) { pair_costs[p][t] = 10000; continue; }
            const auto& map = HuffmanTableManager::get_encoding_map(t);
            int abs_x = std::min(15, x), abs_y = std::min(15, y);
            int idx = (abs_x << 4) | abs_y;
            if (!map[idx].valid) { pair_costs[p][t] = 10000; continue; }
            int bits = map[idx].length;
            if (abs_x == 15) bits += huffman_tables[t].linbits;
            if (x != 0) bits += 1;
            if (abs_y == 15) bits += huffman_tables[t].linbits;
            if (y != 0) bits += 1;
            pair_costs[p][t] = bits;
        }
    }

    int last_bv_pair = 0;
    for (int p = 287; p >= 0; --p) {
        if (std::abs(coeffs[2 * p]) > 1 || std::abs(coeffs[2 * p + 1]) > 1) {
            last_bv_pair = p + 1;
            break;
        }
    }

    int last_nonzero_coeff = 0;
    for (int i = 575; i >= 0; --i) {
        if (coeffs[i] != 0) {
            last_nonzero_coeff = i + 1;
            break;
        }
    }

    int max_possible_bv = (last_nonzero_coeff + 1) / 2;
    if (max_possible_bv < last_bv_pair) max_possible_bv = last_bv_pair;

    std::vector<std::array<uint32_t, 32>> prefix_costs(max_possible_bv + 1);
    for (int t = 0; t < 32; ++t) prefix_costs[0][t] = 0;
    
    for (int p = 0; p < max_possible_bv; ++p) {
        for (int t = 0; t < 32; ++t) {
            uint32_t c = pair_costs[p][t];
            prefix_costs[p + 1][t] = std::min(prefix_costs[p][t] + c, (uint32_t)10000000);
        }
    }

    auto get_best_region = [&](int start_p, int end_p, int& best_t) -> uint32_t {
        if (start_p >= end_p) { best_t = 0; return 0; }
        uint32_t best_cost = 0xFFFFFFFF;
        best_t = 0;
        for (int t = 0; t < 32; ++t) {
            uint32_t cost = prefix_costs[end_p][t] - prefix_costs[start_p][t];
            if (cost < best_cost) {
                best_cost = cost;
                best_t = t;
            }
        }
        return best_cost;
    };

    const auto& c1_arr_32 = HuffmanTableManager::get_encoding_map(32);
    const auto& c1_arr_33 = HuffmanTableManager::get_encoding_map(33);
    
    std::vector<uint32_t> c1_min_bits(max_possible_bv + 1, 0xFFFFFFFF);
    std::vector<bool> c1_best_is_33(max_possible_bv + 1, false);

    for (int bv = last_bv_pair; bv <= max_possible_bv; ++bv) {
        int c1_bits_32 = 0, c1_bits_33 = 0;
        int cur = bv * 2;
        bool c1_possible = true;
        while (cur <= 572) {
            if (cur >= last_nonzero_coeff) break;
            int v = coeffs[cur], w = coeffs[cur+1], x = coeffs[cur+2], y = coeffs[cur+3];
            int symbol = ((v != 0) << 3) | ((w != 0) << 2) | ((x != 0) << 1) | (y != 0);
            if (!c1_arr_32[symbol].valid || c1_arr_32[symbol].length == 0) { c1_possible = false; break; }
            
            c1_bits_32 += c1_arr_32[symbol].length;
            c1_bits_33 += c1_arr_33[symbol].length;
            
            if (v != 0) { c1_bits_32++; c1_bits_33++; }
            if (w != 0) { c1_bits_32++; c1_bits_33++; }
            if (x != 0) { c1_bits_32++; c1_bits_33++; }
            if (y != 0) { c1_bits_32++; c1_bits_33++; }
            
            cur += 4;
        }
        if (c1_possible) {
            if (c1_bits_33 < c1_bits_32) {
                c1_min_bits[bv] = c1_bits_33;
                c1_best_is_33[bv] = true;
            } else {
                c1_min_bits[bv] = c1_bits_32;
                c1_best_is_33[bv] = false;
            }
        }
    }

    for (int bv = last_bv_pair; bv <= max_possible_bv; ++bv) {
        if (c1_min_bits[bv] == 0xFFFFFFFF) continue;
        
        for (int r0_idx = 0; r0_idx < 16; ++r0_idx) {
            int r0_end = std::min(bv, sf_bands[r0_idx + 1] / 2);
            int t0;
            uint32_t r0_bits = get_best_region(0, r0_end, t0);
            if (r0_bits >= 10000) continue;

            for (int r1_idx = 0; r1_idx < 8; ++r1_idx) {
                int r1_end = std::min(bv, sf_bands[r0_idx + r1_idx + 2] / 2);
                int t1;
                uint32_t r1_bits = get_best_region(r0_end, r1_end, t1);
                if (r1_bits >= 10000) continue;

                int t2;
                uint32_t r2_bits = get_best_region(r1_end, bv, t2);
                if (r2_bits >= 10000) continue;

                uint32_t total = r0_bits + r1_bits + r2_bits + c1_min_bits[bv];
                if (total < min_total_bits) {
                    min_total_bits = total;
                    best = {r0_idx, r1_idx, bv, t0, t1, t2, false, c1_best_is_33[bv], 0, 0};
                }
            }
        }
    }
    
    // Calculate cost of orig_config manually to see why it was rejected or overpriced
    int orig_bv = orig_config.big_values;
    if (!orig_config.window_switching_flag && orig_bv <= max_possible_bv) {
        int r0 = std::min(orig_bv, sf_bands[orig_config.region0_count + 1] / 2);
        int r1 = std::min(orig_bv, sf_bands[orig_config.region0_count + orig_config.region1_count + 2] / 2);
        uint32_t c0 = prefix_costs[r0][orig_config.table0] - prefix_costs[0][orig_config.table0];
        uint32_t c1 = prefix_costs[r1][orig_config.table1] - prefix_costs[r0][orig_config.table1];
        uint32_t c2 = prefix_costs[orig_bv][orig_config.table2] - prefix_costs[r1][orig_config.table2];
        
        int c1_cost = 0;
        int cur = orig_bv * 2;
        int count1_table = orig_config.count1_table_select ? 33 : 32;
        const auto& c1_arr = HuffmanTableManager::get_encoding_map(count1_table);
        while (cur <= 572) {
            if (cur >= last_nonzero_coeff) break;
            int v = coeffs[cur], w = coeffs[cur+1], x = coeffs[cur+2], y = coeffs[cur+3];
            int symbol = ((v != 0) << 3) | ((w != 0) << 2) | ((x != 0) << 1) | (y != 0);
            if (!c1_arr[symbol].valid || c1_arr[symbol].length == 0) { c1_cost += 10000; break; }
            c1_cost += c1_arr[symbol].length + (v!=0) + (w!=0) + (x!=0) + (y!=0);
            cur += 4;
        }
        
        uint32_t orig_cost = c0 + c1 + c2 + c1_cost;
        if (orig_cost >= 10000) orig_cost = 99999;
    }
    
    return best;
}

void HuffmanOptimizer::encode_quantized_coefficients(const std::vector<int16_t>& coeffs, const HuffmanConfig& config, BitstreamWriter& writer, int samplerate) {
    int cur = 0;
    auto sf_bands = get_sf_bands(samplerate);

    auto encode_pair = [&](int x, int y, int table_idx) {
        if (table_idx == 0) return;
        int linbits = huffman_tables[table_idx].linbits;
        int abs_x = std::min(15, std::abs(x));
        int abs_y = std::min(15, std::abs(y));
        const auto& map = HuffmanTableManager::get_encoding_map(table_idx);
        int idx = (abs_x << 4) | abs_y;
        if (map[idx].valid) {
            writer.write_bits(map[idx].code, map[idx].length);
            if (abs_x == 15 && linbits > 0) writer.write_bits(std::abs(x) - 15, linbits);
            if (x != 0) writer.write_bits(x < 0 ? 1 : 0, 1);
            if (abs_y == 15 && linbits > 0) writer.write_bits(std::abs(y) - 15, linbits);
            if (y != 0) writer.write_bits(y < 0 ? 1 : 0, 1);
        }
    };

    int r0_pairs, r1_pairs, r2_pairs;
    if (config.window_switching_flag) {
        int region0;
        if (config.block_type == 2 && !config.mixed_block_flag) {
            auto sf_bands_short = get_sf_bands_short(samplerate);
            region0 = (sf_bands_short[3] / 2) * 3;
        } else {
            region0 = sf_bands[8] / 2;
        }
        r0_pairs = std::min(region0, config.big_values);
        r1_pairs = std::max(0, config.big_values - region0);
        r2_pairs = 0;
    } else {
        r0_pairs = std::min(config.big_values, sf_bands[config.region0_count + 1] / 2);
        r1_pairs = std::min(config.big_values, sf_bands[config.region0_count + config.region1_count + 2] / 2) - r0_pairs;
        r2_pairs = config.big_values - r0_pairs - r1_pairs;
    }

    for (int i = 0; i < r0_pairs; ++i) { encode_pair(coeffs[cur], coeffs[cur+1], config.table0); cur += 2; }
    for (int i = 0; i < r1_pairs; ++i) { encode_pair(coeffs[cur], coeffs[cur+1], config.table1); cur += 2; }
    for (int i = 0; i < r2_pairs; ++i) { encode_pair(coeffs[cur], coeffs[cur+1], config.table2); cur += 2; }

    int last_nonzero = 0;
    for (int i = 575; i >= 0; --i) {
        if (coeffs[i] != 0) { last_nonzero = i + 1; break; }
    }

    int count1_table = config.count1_table_select ? 33 : 32;
    const auto& c1_map = HuffmanTableManager::get_encoding_map(count1_table);
    while (cur <= 572) {
        if (cur >= last_nonzero) break;
        int v = coeffs[cur], w = coeffs[cur+1], x = coeffs[cur+2], y = coeffs[cur+3];
        int symbol = ((v != 0) << 3) | ((w != 0) << 2) | ((x != 0) << 1) | (y != 0);
        if (!c1_map[symbol].valid || c1_map[symbol].length == 0) break;
        writer.write_bits(c1_map[symbol].code, c1_map[symbol].length);
        if (v != 0) writer.write_bits(v < 0 ? 1 : 0, 1);
        if (w != 0) writer.write_bits(w < 0 ? 1 : 0, 1);
        if (x != 0) writer.write_bits(x < 0 ? 1 : 0, 1);
        if (y != 0) writer.write_bits(y < 0 ? 1 : 0, 1);
        cur += 4;
    }
}

} // namespace mp3packer
