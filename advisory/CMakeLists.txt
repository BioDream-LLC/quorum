cmake_minimum_required(VERSION 3.16)

# HOST ONLY. This tier may allocate, may recurse, and has no WCET -- preferred and stable semantics are NP-hard, and
# an unbounded computation cannot be a safety function. It is deliberately NOT subject to the freestanding gates the
# device components are, and must never be linked into firmware.
add_library(quorum_advisory INTERFACE)
target_include_directories(quorum_advisory INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(quorum_advisory INTERFACE cxx_std_17)
target_link_libraries(quorum_advisory INTERFACE quorum_solver)

add_executable(test_two_tier test/test_two_tier.cpp)
add_dependencies(test_two_tier quorum_generated_headers)
target_link_libraries(test_two_tier PRIVATE quorum_advisory quorum_arbiter quorum_binding)
target_include_directories(test_two_tier PRIVATE ${CMAKE_BINARY_DIR}/casebook/compiler/gen)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(test_two_tier PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Werror)
endif()
add_test(NAME advisory_two_tier COMMAND test_two_tier)

# Two results arrived from opposite directions with suspiciously tidy arithmetic: severity resolved 48 of the pump's 86
# ambiguous states, and preferred semantics leaves 38 of 224 contested. If those are the same 48, a compile-time
# annotation buys what an NP-hard runtime search buys, and the advisory tier's justification narrows.
add_executable(test_severity_vs_search test/test_severity_vs_search.cpp)
add_dependencies(test_severity_vs_search quorum_generated_headers)
target_link_libraries(test_severity_vs_search PRIVATE quorum_advisory quorum_arbiter)
target_include_directories(test_severity_vs_search PRIVATE ${CMAKE_BINARY_DIR}/casebook/compiler/gen)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(test_severity_vs_search PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Werror)
endif()
add_test(NAME advisory_severity_vs_search COMMAND test_severity_vs_search)

# The product's most important output path: what a human is told when the machine will not decide. `Indeterminate` is
# the correct verdict and nearly useless as something to show an operator, so this asserts the explanation names the
# competing readings, names only what actually differs, and names the observation that would settle it.
add_executable(test_explain test/test_explain.cpp)
add_dependencies(test_explain quorum_generated_headers)
target_link_libraries(test_explain PRIVATE quorum_advisory quorum_arbiter)
target_include_directories(test_explain PRIVATE ${CMAKE_BINARY_DIR}/casebook/compiler/gen)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(test_explain PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Werror)
endif()
add_test(NAME advisory_explain COMMAND test_explain)
