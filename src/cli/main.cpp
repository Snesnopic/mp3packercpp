#include <filesystem>
#include <iostream>
#include "../core/include/packer.hpp"
#include "../core/include/logger.hpp"
#include "../core/include/huffman.hpp"

void printUsage() {
    std::cout << "Usage: mp3packercpp [-z] [-t threads] [--cbr] [-b kbps] [--ib] [-v] <input.mp3> [output.mp3]\n";
    std::cout << "  Without output.mp3 the input is repacked in place.\n";
    std::cout << "  -z         : Recompress frames to find optimal Huffman settings\n";
    std::cout << "  -t threads : Number of threads to use (default: auto)\n";
    std::cout << "  --cbr      : Constant-bitrate output at the lowest bitrate that fits\n";
    std::cout << "  -b kbps    : Floor for the constant bitrate, rounded up to a valid one\n";
    std::cout << "               (implies --cbr; with --cbr the higher of the two wins)\n";
    std::cout << "  --ib       : Print the lowest constant bitrate this file fits in, and exit\n";
    std::cout << "  --keep-crc : Keep (and recompute) the frame CRC where the input had one\n";
    std::cout << "  --preserve-xing : Keep the Xing/Info frame whole, patching only its numbers\n";
    std::cout << "  -v         : Verbose output (default: silent on success)\n";
}
int main(int argc, const char* argv[]) {
    bool recompress_huffman = false;
    bool verbose = false;
    bool constant_bitrate = false;
    bool report_bitrate = false;
    bool keep_crc = false;
    bool preserve_xing = false;
    int min_bitrate = 0;
    int num_threads = 0;
    std::filesystem::path input;
    std::filesystem::path output;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--version") {
            std::cout << MP3PACKERCPP_VERSION << "\n";
            return EXIT_SUCCESS;
        } else if (arg == "-h" || arg == "--help") {
            printUsage();
            return EXIT_SUCCESS;
        } else if (arg == "-z") {
            recompress_huffman = true;
        } else if (arg == "-v") {
            verbose = true;
        } else if (arg == "--keep-crc") {
            keep_crc = true;
        } else if (arg == "--preserve-xing") {
            preserve_xing = true;
        } else if (arg == "--cbr") {
            constant_bitrate = true;
        } else if (arg == "--ib") {
            report_bitrate = true;
            constant_bitrate = true;
        } else if (arg == "-t" || arg == "-b") {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for " << arg << "\n";
                printUsage();
                return EXIT_FAILURE;
            }
            const std::string value = argv[++i];
            int parsed = 0;
            try {
                parsed = std::stoi(value);
            } catch (const std::exception&) {
                std::cerr << "Not a number: " << arg << " " << value << "\n";
                return EXIT_FAILURE;
            }
            if (arg == "-t") num_threads = parsed; else min_bitrate = parsed;
        } else if (input.empty()) {
            input = arg;
        } else if (output.empty()) {
            output = arg;
        } else {
            std::cerr << "Unknown argument or too many arguments: " << arg << "\n";
            printUsage();
            return EXIT_FAILURE;
        }
    }

    if (input.empty()) {
        printUsage();
        return EXIT_FAILURE;
    }
    // --ib answers a question about the input and writes nothing; otherwise a
    // missing output means repacking in place, which is safe because the output
    // is written through a temporary and renamed over the input only once it is
    // complete.
    if (output.empty() && !report_bitrate) {
        output = input;
    }

    mp3packer::Logger::verbose = verbose;

    try {
        mp3packer::Packer packer;
        packer.recompress_huffman = recompress_huffman;
        packer.num_threads = num_threads > 0 ? num_threads : 0;
        packer.constant_bitrate = constant_bitrate;
        packer.min_bitrate_kbps = min_bitrate;
        packer.report_bitrate_only = report_bitrate;
        packer.keep_crc = keep_crc;
        packer.preserve_xing = preserve_xing;

        int chosen_bitrate = 0;
        packer.process(input.string(), output.string(), &chosen_bitrate);

        if (report_bitrate) {
            std::cout << chosen_bitrate << "\n";
        } else if (verbose) {
            std::cout << "Successfully packed " << input.string() << " into " << output.string();
            if (chosen_bitrate > 0) std::cout << " at " << chosen_bitrate << " kbps CBR";
            std::cout << "\n";
            if (recompress_huffman) {
                std::cout << "Cost kernels: " << mp3packer::active_simd_kernel() << "\n";
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
