cmake_minimum_required(VERSION 3.13)

# quorum on a Raspberry Pi Pico 2 (RP2350).
#
# A Pico SDK project rather than the bare-metal harness in ../. The bare-metal build exists to measure footprint
# honestly -- no libc, no vendor code, so every byte `size` reports belongs to quorum. But for a program that has to
# actually BOOT and talk to a UART, the SDK handles the parts that cannot be verified without hardware: the second
# stage bootloader, the RP2350 image definition block, PLL and clock setup, and pin muxing. Getting any of those
# subtly wrong produces a board that does nothing, with no way to tell why.
#
# Build (nothing here has been run on hardware; see RUNNING_ON_HARDWARE.md):
#
#   export PICO_SDK_PATH=/path/to/pico-sdk
#   cmake -S . -B build -DPICO_BOARD=pico2 -DCMAKE_BUILD_TYPE=Release
#   cmake --build build
#   # then copy build/quorum_wcet.uf2 to the board in BOOTSEL mode

set(PICO_PLATFORM rp2350)
include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(quorum_wcet CXX C ASM)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

pico_sdk_init()

# The generated casebook headers. Produced by the host build, which must be run first -- cbc is a host tool and its
# exhaustive validation is what finds the worst-case state this firmware measures.
set(QUORUM_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(QUORUM_GEN ${QUORUM_ROOT}/build/casebook/compiler/gen)

if(NOT EXISTS ${QUORUM_GEN}/pump_generated.hpp)
    message(FATAL_ERROR
        "Generated casebook headers not found in ${QUORUM_GEN}.\n"
        "Run the host build first:\n"
        "  cd ${QUORUM_ROOT} && cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j")
endif()

add_executable(quorum_wcet main.cpp)

target_include_directories(quorum_wcet PRIVATE
    ${QUORUM_ROOT}/solver/include
    ${QUORUM_ROOT}/arbiter/include
    ${QUORUM_ROOT}/witness/include
    ${QUORUM_ROOT}/binding/include
    ${QUORUM_ROOT}/bench/include
    ${QUORUM_GEN}
)

# No exceptions, no RTTI, and -Os: the same discipline the device components are gated with on the host, so the
# firmware is built the way the library is designed to be used rather than the way a demo would be.
target_compile_options(quorum_wcet PRIVATE
    -Os
    -fno-exceptions -fno-rtti -fno-threadsafe-statics
    -Wall -Wextra -Wconversion -Wsign-conversion -Wshadow
)

target_link_libraries(quorum_wcet
    pico_stdlib
    hardware_clocks
)

# UART, not USB. A USB CDC stack would add several kilobytes of vendor code and, more importantly, its interrupts and
# polling would perturb the very cycle counts this program exists to measure.
# BOTH enabled. USB is what makes this a zero-extra-hardware measurement: plug the board into the same USB port you
# flashed it from and read the results. UART stays on as well, for a board wired into a rig or a case where USB does
# not enumerate.
#
# This was originally USB-off, on the reasoning that a CDC stack's interrupts would perturb the cycle counts. That was
# a real concern answered the wrong way: it added a hardware requirement (a serial adapter, and headers soldered to
# reach GP0/GP1) while only removing the interrupts I happened to think of. bench::InterruptGuard masks interrupts
# around each timed window instead, which fixes the measurement rather than the wiring.
pico_enable_stdio_usb(quorum_wcet 1)
pico_enable_stdio_uart(quorum_wcet 1)

pico_add_extra_outputs(quorum_wcet)

# ── a LEARNED casebook (optional) ───────────────────────────────────────────────────────────────────────────────
# Built only if the host has produced it: `ctest -R learn_casebook_e2e` writes the .cb files, then
#   ./build/casebook/compiler/cbc build/learn/learned/monks-1-knows-2.cb \
#       --header build/learn/learned/monks_learned_generated.hpp
# The firmware prints a line in the same format as build/learn/learned/monks-1-knows-2.expected; equal lines mean the
# device decided every state exactly as the learner did.
set(QUORUM_LEARNED ${QUORUM_ROOT}/build/learn/learned)
if(EXISTS ${QUORUM_LEARNED}/monks_learned_generated.hpp)
    add_executable(quorum_learned learned.cpp)
    target_include_directories(quorum_learned PRIVATE
        ${QUORUM_ROOT}/solver/include
        ${QUORUM_ROOT}/arbiter/include
        ${QUORUM_ROOT}/bench/include
        ${QUORUM_LEARNED}
    )
    target_compile_definitions(quorum_learned PRIVATE
        QUORUM_LEARNED_HEADER="monks_learned_generated.hpp"
        QUORUM_LEARNED_NAMESPACE=quorum::casebook::gen::monks_1_knows_2
    )
    target_compile_options(quorum_learned PRIVATE
        -Os -fno-exceptions -fno-rtti -fno-threadsafe-statics
        -Wall -Wextra -Wconversion -Wsign-conversion -Wshadow
    )
    target_link_libraries(quorum_learned pico_stdlib hardware_clocks)
    pico_enable_stdio_usb(quorum_learned 1)
    pico_enable_stdio_uart(quorum_learned 1)
    pico_add_extra_outputs(quorum_learned)
endif()

# ── quorum_runtime: one prebuilt firmware for ANY validated casebook ────────────────────────────────────────────────
# The casebook is not compiled in; it is loaded at boot from the flash region in runtime/include/quorum/runtime/
# layout.hpp, where the desktop tool (or `quorum-studio image`) places the output of `cbc --blob`. With no image there,
# the device reports "NO CASEBOOK" and does not decide.
add_executable(quorum_runtime runtime.cpp)
target_include_directories(quorum_runtime PRIVATE
    ${QUORUM_ROOT}/solver/include
    ${QUORUM_ROOT}/arbiter/include
    ${QUORUM_ROOT}/binding/include
    ${QUORUM_ROOT}/bench/include
    ${QUORUM_ROOT}/runtime/include
)
target_compile_options(quorum_runtime PRIVATE
    -Os -fno-exceptions -fno-rtti -fno-threadsafe-statics
    -Wall -Wextra -Wconversion -Wsign-conversion -Wshadow
)
target_link_libraries(quorum_runtime pico_stdlib hardware_clocks)
pico_enable_stdio_usb(quorum_runtime 1)
pico_enable_stdio_uart(quorum_runtime 1)
pico_add_extra_outputs(quorum_runtime)
