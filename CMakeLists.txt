cmake_minimum_required(VERSION 3.10)
if(POLICY CMP0074)
  cmake_policy(SET CMP0074 NEW)
endif()
project(mp3packercpp)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Enable strict warnings
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    add_compile_options(-Wall -Wextra -Wpedantic)
elseif(MSVC)
    add_compile_options(/W4)
endif()

# Default to Release if not specified
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Choose the type of build." FORCE)
endif()

# Release flags
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -DNDEBUG")

# Debug flags
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    set(CMAKE_CXX_FLAGS_DEBUG "-O0 -g3 -fsanitize=address -fno-omit-frame-pointer")
    set(CMAKE_EXE_LINKER_FLAGS_DEBUG "-fsanitize=address")
elseif(MSVC)
    # MSVC support for ASan requires specific setup, but these are standard flags
    set(CMAKE_CXX_FLAGS_DEBUG "/Zi /Od /fsanitize=address")
endif()

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# --- Core Library ---
file(GLOB CORE_SOURCES "src/core/*.cpp")
add_library(mp3packercpp_core STATIC ${CORE_SOURCES})
target_include_directories(mp3packercpp_core PUBLIC src/core)

find_package(OpenMP)
if(OpenMP_CXX_FOUND)
    target_link_libraries(mp3packercpp_core PUBLIC OpenMP::OpenMP_CXX)
endif()

# --- CLI Executable ---
add_executable(mp3packercpp src/cli/main.cpp)
target_link_libraries(mp3packercpp PRIVATE mp3packercpp_core)
target_include_directories(mp3packercpp PRIVATE src/cli)

include(GNUInstallDirs)
install(TARGETS mp3packercpp DESTINATION ${CMAKE_INSTALL_BINDIR})
