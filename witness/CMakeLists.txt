cmake_minimum_required(VERSION 3.16)

# Header-only. `derivation.hpp` is device code; `render.hpp` is host-only and says so.
add_library(quorum_witness INTERFACE)
target_include_directories(quorum_witness INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(quorum_witness INTERFACE quorum_solver)
target_compile_features(quorum_witness INTERFACE cxx_std_17)

# Freestanding gate for derivation.hpp ONLY. render.hpp is deliberately absent: it uses <cstdio> by design, and
# including it here would defeat the purpose of the gate.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_library(quorum_witness_freestanding_check STATIC test/freestanding_check.cpp)
    target_link_libraries(quorum_witness_freestanding_check PRIVATE quorum_witness)
    target_compile_options(quorum_witness_freestanding_check PRIVATE
        -ffreestanding -fno-exceptions -fno-rtti -fno-threadsafe-statics
        -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Werror)
endif()
