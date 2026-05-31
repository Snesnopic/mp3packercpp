#include <filesystem>
#include <iostream>
#include "../core/include/packer.hpp"

void printUsage() {
    std::cout << "Usage: mp3packercpp [-z] [-t threads] <input.mp3> <output.mp3>\n";
    std::cout << "  -z         : Recompress frames to find optimal Huffman settings (takes time)\n";
    std::cout << "  -t threads : Number of threads to use (default: auto)\n";
}
int main(int argc, const char* argv[]) {
    bool recompress_huffman = false;
    int num_threads = 0;
    std::filesystem::path input;
    std::filesystem::path output;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-z") {
            recompress_huffman = true;
        } else if (arg == "-t") {
            if (i + 1 < argc) {
                num_threads = std::stoi(argv[++i]);
            } else {
                std::cerr << "Missing value for -t\n";
                printUsage();
                return EXIT_FAILURE;
            }
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

    if (input.empty() || output.empty()) {
        printUsage();
        return EXIT_FAILURE;
    }

    try {
        mp3packer::Packer packer;
        packer.recompress_huffman = recompress_huffman;
        packer.num_threads = num_threads > 0 ? num_threads : 0;
        packer.process(input.string(), output.string());
        std::cout << "Successfully packed " << input.string() << " into " << output.string() << "\n";
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
