# Dependency-free tests: each is a program that returns non-zero on failure.
add_executable(coder_test coder_test.cpp)
target_link_libraries(coder_test PRIVATE mp3packercpp_core)
add_test(NAME coder COMMAND coder_test)

add_executable(bitstream_test bitstream_test.cpp)
target_include_directories(bitstream_test PRIVATE ${CMAKE_SOURCE_DIR}/src/core/include)
add_test(NAME bitstream COMMAND bitstream_test)
