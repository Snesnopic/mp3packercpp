cmake_minimum_required(VERSION 3.10)
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

set(CMAKE_BUILD_TYPE Release)
set(CMAKE_CXX_FLAGS "-O3 -march=native")

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# --- Core Library ---
file(GLOB CORE_SOURCES "src/core/*.cpp")
add_library(mp3packercpp_core STATIC ${CORE_SOURCES})
target_include_directories(mp3packercpp_core PUBLIC src/core)

# --- CLI Executable ---
add_executable(mp3packercpp src/cli/main.cpp)
target_link_libraries(mp3packercpp PRIVATE mp3packercpp_core)
target_include_directories(mp3packercpp PRIVATE src/cli)

include(GNUInstallDirs)
install(TARGETS mp3packercpp DESTINATION ${CMAKE_INSTALL_BINDIR})
