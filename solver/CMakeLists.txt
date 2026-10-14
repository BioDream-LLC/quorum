cmake_minimum_required(VERSION 3.16)
project(quorum_solver LANGUAGES CXX)

# C++17 exactly. MISRA C++:2023 is defined against ISO/IEC 14882:2017, so staying on 17 keeps the coding
# standard and the language version aligned. Do not bump this casually.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ─────────────────────────────────────────────────────────────────────────────
# The library is header-only. Capacity is a template parameter, so there is nothing to compile separately and
# every bound is known to the optimiser at the call site.
# ─────────────────────────────────────────────────────────────────────────────
add_library(quorum_solver INTERFACE)
target_include_directories(quorum_solver INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(quorum_solver INTERFACE cxx_std_17)

# ─────────────────────────────────────────────────────────────────────────────
# Host tests. These may use the standard library freely; the headers under include/ may not.
# ─────────────────────────────────────────────────────────────────────────────
enable_testing()

add_executable(test_grounded test/test_grounded.cpp)
target_link_libraries(test_grounded PRIVATE quorum_solver)
target_include_directories(test_grounded PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/test)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(test_grounded PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Werror)
endif()

add_test(NAME grounded COMMAND test_grounded)

# ─────────────────────────────────────────────────────────────────────────────
# Freestanding compile check.
#
# Compiles the headers with exceptions, RTTI and the hosted standard library all switched off. This is the gate
# that keeps an innocent-looking `#include <vector>` out of the device build -- it will fail here long before it
# fails on a Cortex-M.
# ─────────────────────────────────────────────────────────────────────────────
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_library(quorum_solver_freestanding_check STATIC test/freestanding_check.cpp)
    target_link_libraries(quorum_solver_freestanding_check PRIVATE quorum_solver)
    target_compile_options(quorum_solver_freestanding_check PRIVATE
        -ffreestanding
        -fno-exceptions
        -fno-rtti
        -fno-threadsafe-statics
        -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Werror)
endif()
