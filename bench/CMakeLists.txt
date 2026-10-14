cmake_minimum_required(VERSION 3.16)

add_library(quorum_bench INTERFACE)
target_include_directories(quorum_bench INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(quorum_bench INTERFACE cxx_std_17)

# The measurement needs silicon; the arithmetic around it does not, and would be wrong silently. A rig that reports
# 16 million cycles because a 24-bit down-counter was subtracted the wrong way does not look broken -- it looks like
# a slow path.
add_executable(test_cycles test/test_cycles.cpp)
target_link_libraries(test_cycles PRIVATE quorum_bench)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(test_cycles PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Werror)
endif()
add_test(NAME bench_harness COMMAND test_cycles)
