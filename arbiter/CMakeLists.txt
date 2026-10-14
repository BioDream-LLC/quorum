cmake_minimum_required(VERSION 3.16)

# Header-only, like the engine. The arbiter is device code: no heap, no exceptions, nothing from the hosted
# standard library.
add_library(quorum_arbiter INTERFACE)
target_include_directories(quorum_arbiter INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(quorum_arbiter INTERFACE quorum_solver)
target_compile_features(quorum_arbiter INTERFACE cxx_std_17)

# Freestanding gate, same reasoning as the engine's: an accidental `#include <vector>` here must fail on a
# workstation rather than on a Cortex-M.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_library(quorum_arbiter_freestanding_check STATIC test/freestanding_check.cpp)
    target_link_libraries(quorum_arbiter_freestanding_check PRIVATE quorum_arbiter)
    target_compile_options(quorum_arbiter_freestanding_check PRIVATE
        -ffreestanding -fno-exceptions -fno-rtti -fno-threadsafe-statics
        -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Werror)
endif()

# Adversarial fixtures: every Verdict outcome reached deliberately, including Faulted, which no realistic casebook
# ever reaches and which therefore had no test until these were written.
add_executable(test_outcomes test/test_outcomes.cpp)
target_link_libraries(test_outcomes PRIVATE quorum_arbiter)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(test_outcomes PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Werror)
endif()
add_test(NAME arbiter_outcomes COMMAND test_outcomes)
