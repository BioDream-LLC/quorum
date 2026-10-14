cmake_minimum_required(VERSION 3.16)
project(quorum LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

enable_testing()

add_subdirectory(solver)
add_subdirectory(arbiter)
add_subdirectory(witness)
add_subdirectory(binding)
add_subdirectory(bench)
# advisory AFTER casebook: it depends on the quorum_generated_headers target defined there.
add_subdirectory(casebook)
add_subdirectory(runtime)
add_subdirectory(advisory)
add_subdirectory(wcet)
add_subdirectory(learn)
add_subdirectory(tools/qcert)
