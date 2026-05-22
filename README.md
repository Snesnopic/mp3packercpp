# mp3packercpp

mp3packercpp is a modern C++17 port of the original mp3packer utility. It focuses on providing lossless MP3 repacking and aggressive Huffman tree recompression to minimize the file size of MP3 audio without any loss in audio quality.

While the original tool was written in OCaml, this port aims to provide a native, easily compilable C++ alternative. Please note that not all features from the original mp3packer have been ported; this project focuses strictly on the core compression and bit reservoir constraint solving logic, including the exhaustive Huffman recompression.

## Building from source

The project uses CMake for its build system. A C++17 compliant compiler is required.

To build the project with default settings:

```bash
mkdir build
cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
cmake --build . --config Release
```

### OpenMP support

The aggressive Huffman recompression (enabled via the `-z` flag) is computationally intensive. You can speed up this process significantly across multiple CPU cores by enabling OpenMP.

To compile with OpenMP support, pass the `USE_OPENMP` flag during the CMake configuration step:

```bash
cmake -DCMAKE_BUILD_TYPE=Release -DUSE_OPENMP=ON ..
cmake --build . --config Release
```

Make sure your compiler supports OpenMP and that the required runtime libraries are installed on your system.

## Original project and pre-built binaries

The original mp3packer was written in OCaml. The source code for the original version can be found [here](https://github.com/Snesnopic/mp3packer/tree/main).

Additionally, an effort to modernize the original OCaml codebase for OCaml 5.3.0 and the Dune build system is available in [this branch](https://github.com/Snesnopic/mp3packer/tree/fix/arm64-porting). Pre-built binaries of the original OCaml version can also be easily downloaded from that repository branch.
