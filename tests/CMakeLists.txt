# Dependency-free tests: each is a program that returns non-zero on failure.
add_executable(coder_test coder_test.cpp)
target_link_libraries(coder_test PRIVATE mp3packercpp_core)
add_test(NAME coder COMMAND coder_test)

add_executable(bitstream_test bitstream_test.cpp)
target_include_directories(bitstream_test PRIVATE ${CMAKE_SOURCE_DIR}/src/core/include)
add_test(NAME bitstream COMMAND bitstream_test)

add_executable(simd_test simd_test.cpp)
target_link_libraries(simd_test PRIVATE mp3packercpp_core)
add_test(NAME simd COMMAND simd_test)

# The fuzz target needs a compiler with libFuzzer; Apple's clang does not ship the
# runtime, so this stays opt-in rather than being built with everything else.
# Point CMAKE_CXX_COMPILER at an LLVM clang++ and configure with
# -DMP3PACKERCPP_BUILD_FUZZER=ON.
option(MP3PACKERCPP_BUILD_FUZZER "Build the libFuzzer target for Packer::process" OFF)
if(MP3PACKERCPP_BUILD_FUZZER)
    add_executable(fuzz_process fuzz_process.cpp)
    target_link_libraries(fuzz_process PRIVATE mp3packercpp_core)
    target_include_directories(fuzz_process PRIVATE ${CMAKE_SOURCE_DIR}/third_party/minimp3)
    target_compile_options(fuzz_process PRIVATE -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer -g)
    target_link_options(fuzz_process PRIVATE -fsanitize=fuzzer,address,undefined)
endif()

add_executable(search_test search_test.cpp)
target_link_libraries(search_test PRIVATE mp3packercpp_core)
add_test(NAME search COMMAND search_test)
