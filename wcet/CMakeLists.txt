cmake_minimum_required(VERSION 3.16)

# A static bound on the safety function, derived from the Cortex-M33 binary rather than measured. Host-only analysis:
# it computes what the device WILL do, it does not run on the device.
add_executable(test_static_bound test/test_static_bound.cpp)
add_dependencies(test_static_bound quorum_generated_headers)
target_link_libraries(test_static_bound PRIVATE quorum_solver quorum_arbiter quorum_binding)
target_include_directories(test_static_bound PRIVATE ${CMAKE_BINARY_DIR}/casebook/compiler/gen)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(test_static_bound PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Werror)
endif()
add_test(NAME wcet_static_bound COMMAND test_static_bound)
