cmake_minimum_required(VERSION 3.16)

# The prebuilt-runtime path: one firmware per board that loads any validated casebook from flash (cbc --blob).
# Header-only device code, like the engine: no heap, no exceptions, nothing from the hosted standard library.
add_library(quorum_runtime INTERFACE)
target_include_directories(quorum_runtime INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(quorum_runtime INTERFACE quorum_solver quorum_arbiter quorum_binding)
target_compile_features(quorum_runtime INTERFACE cxx_std_17)

set(QUORUM_STRICT -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Werror)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_library(quorum_runtime_freestanding_check STATIC test/freestanding_check.cpp)
    target_link_libraries(quorum_runtime_freestanding_check PRIVATE quorum_runtime)
    target_compile_options(quorum_runtime_freestanding_check PRIVATE
        -ffreestanding -fno-exceptions -fno-rtti -fno-threadsafe-statics ${QUORUM_STRICT})
endif()

# Blob equivalence: for every reference casebook, cbc writes the header and the blob from the same source in one run,
# and the test requires the two paths to decide every state identically.
#   name | source (relative to casebook/) | generated namespace | declares channels | declares timed readings
set(QUORUM_BLOB_CASES
    "pump|pump/pump.cb|centrifugal_pump|0"
    "pump_severity|pump/pump_severity.cb|centrifugal_pump_severity|0"
    "pump_conditions|pump/pump_conditions.cb|centrifugal_pump_conditions|0"
    "tank|tank/tank.cb|tank_409|1"
    "permit|permit/permit.cb|piper_permit|1"
    "vote|vote/vote.cb|vessel_hl_trip|1"
    "vote_manual|vote/vote_manual.cb|vessel_hl_trip_manual|1"
    "blind|fixtures/blind_channel/blind.cb|blind|1"
    "held|fixtures/held/held.cb|held_demo|1"
    "gearbox|example/gearbox.cb|gearbox_demo|1"
    "timed|fixtures/timed/timed.cb|timed_demo|0|1"
    "chained|fixtures/timed/chained.cb|chained_demo|0|1"
    "startup|fixtures/timed/startup.cb|startup_demo|0|1"
)
set(CB_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../casebook)
foreach(case ${QUORUM_BLOB_CASES})
    string(REPLACE "|" ";" f "${case}")
    list(GET f 0 name)
    list(GET f 1 src)
    list(GET f 2 ns)
    list(GET f 3 ch)
    list(LENGTH f nf)
    set(tm 0)
    if(nf GREATER 4)
        list(GET f 4 tm)
    endif()
    set(out ${CMAKE_CURRENT_BINARY_DIR}/blob/${name})
    add_custom_command(
        OUTPUT ${out}/generated.hpp ${out}/casebook.qcb
        COMMAND ${CMAKE_COMMAND} -E make_directory ${out}
        COMMAND cbc ${CB_ROOT}/${src} --quiet --header ${out}/generated.hpp --blob ${out}/casebook.qcb
        DEPENDS cbc ${CB_ROOT}/${src}
        COMMENT "cbc: header and blob for ${name}")
    add_executable(test_blob_${name} test/test_blob.cpp ${out}/generated.hpp ${out}/casebook.qcb)
    target_link_libraries(test_blob_${name} PRIVATE quorum_runtime)
    target_include_directories(test_blob_${name} PRIVATE ${out} ${CMAKE_SOURCE_DIR}/binding/include)
    target_compile_definitions(test_blob_${name} PRIVATE
        QUORUM_HEADER="generated.hpp" QUORUM_NS=quorum::casebook::gen::${ns} QUORUM_HAS_CHANNELS=${ch}
        QUORUM_HAS_TIMERS=${tm})
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(test_blob_${name} PRIVATE ${QUORUM_STRICT})
    endif()
    add_test(NAME runtime_blob_${name} COMMAND test_blob_${name} ${out}/casebook.qcb)
endforeach()

# The AI guard: every reason, then every state x every proposal of the pump and tank images.
add_executable(test_guard test/test_guard.cpp)
target_link_libraries(test_guard PRIVATE quorum_runtime)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(test_guard PRIVATE ${QUORUM_STRICT})
endif()
add_dependencies(test_guard test_blob_pump test_blob_tank)
add_test(NAME runtime_guard_pump COMMAND test_guard ${CMAKE_CURRENT_BINARY_DIR}/blob/pump/casebook.qcb)
add_test(NAME runtime_guard_tank COMMAND test_guard ${CMAKE_CURRENT_BINARY_DIR}/blob/tank/casebook.qcb)

# Modbus RTU: framing, exceptions, every pump state through the registers, the guard, raw channels with a hold.
add_executable(test_modbus test/test_modbus.cpp)
target_link_libraries(test_modbus PRIVATE quorum_runtime)
target_include_directories(test_modbus PRIVATE ${CMAKE_SOURCE_DIR}/binding/include)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(test_modbus PRIVATE ${QUORUM_STRICT})
endif()
add_dependencies(test_modbus test_blob_pump test_blob_held test_blob_timed)
add_test(NAME runtime_modbus COMMAND test_modbus ${CMAKE_CURRENT_BINARY_DIR}/blob/pump/casebook.qcb
                                                ${CMAKE_CURRENT_BINARY_DIR}/blob/held/casebook.qcb
                                                ${CMAKE_CURRENT_BINARY_DIR}/blob/timed/casebook.qcb)

# The bench script cbc writes for each timed rulebook, played against the device's Modbus map on its test clock.
add_executable(test_timed_script test/test_timed_script.cpp)
target_link_libraries(test_timed_script PRIVATE quorum_runtime)
target_include_directories(test_timed_script PRIVATE ${CMAKE_SOURCE_DIR}/binding/include)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(test_timed_script PRIVATE ${QUORUM_STRICT})
endif()
foreach(case ${QUORUM_BLOB_CASES})
    string(REPLACE "|" ";" f "${case}")
    list(LENGTH f nf)
    if(nf LESS 5)
        continue()
    endif()
    list(GET f 4 tm)
    if(NOT tm)
        continue()
    endif()
    list(GET f 0 name)
    list(GET f 1 src)
    set(out ${CMAKE_CURRENT_BINARY_DIR}/blob/${name})
    add_custom_command(OUTPUT ${out}/sheet.tsv
        COMMAND cbc ${CB_ROOT}/${src} --quiet --commissioning ${out}/sheet.tsv
        DEPENDS cbc ${CB_ROOT}/${src}
        COMMENT "cbc: commissioning sheet and bench script for ${name}")
    add_custom_target(sheet_${name} ALL DEPENDS ${out}/sheet.tsv)
    add_dependencies(test_timed_script test_blob_${name} sheet_${name})
    add_test(NAME runtime_timed_script_${name} COMMAND test_timed_script ${out}/casebook.qcb ${out}/sheet.tsv)
    add_test(NAME runtime_timed_script_late_${name}
             COMMAND test_timed_script ${out}/casebook.qcb ${out}/sheet.tsv --late 50)
endforeach()

# qblob: the host companion. Uses the device loader itself.
add_executable(qblob tools/qblob.cpp)
target_link_libraries(qblob PRIVATE quorum_runtime)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(qblob PRIVATE ${QUORUM_STRICT})
endif()
add_test(NAME runtime_qblob_pump COMMAND qblob sweep ${CMAKE_CURRENT_BINARY_DIR}/blob/pump/casebook.qcb)

# A palette image: pump + tank on one device, against the compiled palette header from the same cbc run.
set(pal ${CMAKE_CURRENT_BINARY_DIR}/blob/palette)
add_custom_command(
    OUTPUT ${pal}/palette_generated.hpp ${pal}/palette.qcb
    COMMAND ${CMAKE_COMMAND} -E make_directory ${pal}
    COMMAND cbc ${CB_ROOT}/pump/pump.cb ${CB_ROOT}/tank/tank.cb --palette --quiet
            --header ${pal}/palette_generated.hpp --blob ${pal}/palette.qcb
    DEPENDS cbc ${CB_ROOT}/pump/pump.cb ${CB_ROOT}/tank/tank.cb
    COMMENT "cbc: palette header and blob")
add_executable(test_palette_blob test/test_palette_blob.cpp ${pal}/palette_generated.hpp ${pal}/palette.qcb)
target_link_libraries(test_palette_blob PRIVATE quorum_runtime)
target_include_directories(test_palette_blob PRIVATE ${pal})
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(test_palette_blob PRIVATE ${QUORUM_STRICT})
endif()
add_test(NAME runtime_blob_palette COMMAND test_palette_blob ${pal}/palette.qcb)

# Non-interference, on the image: each member's DEVICE line from the palette equals its line alone.
add_test(NAME runtime_qblob_palette
         COMMAND bash -c "set -e; $<TARGET_FILE:qblob> sweep ${pal}/palette.qcb > ${pal}/sweep.txt; \
grep -qx 'DEVICE centrifugal-pump states 16384 asserts 1024 fnv1a 0xe5408b85' ${pal}/sweep.txt; \
grep -qx 'DEVICE tank-409 states 32768 asserts 7680 fnv1a 0x90bc4385' ${pal}/sweep.txt; cat ${pal}/sweep.txt")
set_tests_properties(runtime_qblob_palette PROPERTIES DEPENDS runtime_blob_palette)
