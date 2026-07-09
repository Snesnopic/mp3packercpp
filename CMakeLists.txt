cmake_minimum_required(VERSION 3.12..4.3)

project(mp3packercpp VERSION 1.1.1 LANGUAGES CXX)

if(MSVC)
    # force static msvc runtime linkage (/mt for release, /mtd for debug)
    if(POLICY CMP0091)
        cmake_policy(SET CMP0091 NEW)
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
endif()

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
            src/core/packer.cpp)

target_include_directories(mp3packercpp_core PUBLIC src/core/include)
target_link_libraries(mp3packercpp_core PRIVATE Threads::Threads)

# --- CLI Executable ---
add_executable(mp3packercpp src/cli/main.cpp)
target_link_libraries(mp3packercpp PRIVATE mp3packercpp_core)
target_include_directories(mp3packercpp PRIVATE src/cli)
target_compile_definitions(mp3packercpp PRIVATE MP3PACKERCPP_VERSION="${PROJECT_VERSION}")

if(MSVC)
    # re-assert static runtime on both targets directly, in case find_package(Threads)
    # interfered with the CMAKE_MSVC_RUNTIME_LIBRARY variable set above
    set_target_properties(mp3packercpp_core mp3packercpp PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

include(GNUInstallDirs)
install(TARGETS mp3packercpp DESTINATION ${CMAKE_INSTALL_BINDIR})
