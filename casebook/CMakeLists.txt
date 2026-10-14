cmake_minimum_required(VERSION 3.16)

# Hand-written casebooks. These are experiments and regression tests at the same time: each asserts the verdicts
# it expects, so a change that alters an engineering conclusion fails the build.

add_executable(pump_casebook pump/pump_casebook.cpp)
target_link_libraries(pump_casebook PRIVATE quorum_solver quorum_arbiter)
target_include_directories(pump_casebook PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pump)

add_executable(pump_exhaustive pump/pump_exhaustive.cpp)
target_link_libraries(pump_exhaustive PRIVATE quorum_solver quorum_arbiter)
target_include_directories(pump_exhaustive PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pump)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    foreach(t pump_casebook pump_exhaustive)
        target_compile_options(${t} PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Werror)
    endforeach()
endif()

add_subdirectory(compiler)

add_test(NAME casebook_pump COMMAND pump_casebook)
add_test(NAME casebook_pump_exhaustive COMMAND pump_exhaustive)

add_executable(pump_witness pump/pump_witness.cpp)
target_link_libraries(pump_witness PRIVATE quorum_solver quorum_arbiter quorum_witness)
target_include_directories(pump_witness PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pump)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(pump_witness PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Werror)
endif()
add_test(NAME casebook_pump_witness COMMAND pump_witness)

add_executable(tank_validate tank/tank_validate.cpp)
target_link_libraries(tank_validate PRIVATE quorum_solver quorum_arbiter quorum_witness)
target_include_directories(tank_validate PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tank)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(tank_validate PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Werror)
endif()
add_test(NAME casebook_tank COMMAND tank_validate)
