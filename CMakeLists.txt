cmake_minimum_required(VERSION 3.12..4.3)

project(mp3packercpp VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Threads REQUIRED)

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# --- Core Library ---
add_library(mp3packercpp_core STATIC
            src/core/huffman.cpp
            src/core/mp3_reader.cpp
            src/core/packer.cpp
            src/core/crc.cpp)

target_include_directories(mp3packercpp_core PUBLIC src/core/include)
target_link_libraries(mp3packercpp_core PRIVATE Threads::Threads)

# --- CLI Executable ---
add_executable(mp3packercpp src/cli/main.cpp)
target_link_libraries(mp3packercpp PRIVATE mp3packercpp_core)
target_include_directories(mp3packercpp PRIVATE src/cli)

include(GNUInstallDirs)
install(TARGETS mp3packercpp DESTINATION ${CMAKE_INSTALL_BINDIR})
