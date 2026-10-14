cmake_minimum_required(VERSION 3.16)

add_library(quorum_binding INTERFACE)
target_include_directories(quorum_binding INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(quorum_binding INTERFACE cxx_std_17)

add_executable(test_binding test/test_binding.cpp)
target_link_libraries(test_binding PRIVATE quorum_binding)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(test_binding PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Werror)
endif()

add_test(NAME binding COMMAND test_binding)

# Freestanding gate. No hosted runtime, no exceptions, no RTTI, no thread-safe statics -- and no libm, so any
# accidental floating-point arithmetic fails here rather than on a part without an FPU.
add_library(quorum_binding_freestanding_check OBJECT test/freestanding_check.cpp)
target_link_libraries(quorum_binding_freestanding_check PRIVATE quorum_binding)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(quorum_binding_freestanding_check PRIVATE
        -ffreestanding -fno-exceptions -fno-rtti -fno-threadsafe-statics
        -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Werror)
endif()
