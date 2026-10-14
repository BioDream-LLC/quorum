cmake_minimum_required(VERSION 3.16)

# HOST ONLY. Learning a casebook from data allocates, has no WCET, and must never be linked into firmware. It is the
# offline half of a two-stage design: learn here, then validate and compile what was learned. A casebook is never
# changed on a device -- one that rewrites itself in the field has no proven step bound and no exhaustive validation.
add_library(quorum_learn INTERFACE)
target_include_directories(quorum_learn INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(quorum_learn INTERFACE cxx_std_17)
target_link_libraries(quorum_learn INTERFACE quorum_solver)

set(QUORUM_LEARN_WARNINGS -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Werror)

add_executable(test_aacbr test/test_aacbr.cpp)
target_link_libraries(test_aacbr PRIVATE quorum_learn)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(test_aacbr PRIVATE ${QUORUM_LEARN_WARNINGS})
endif()
add_test(NAME learn_aacbr COMMAND test_aacbr)

# Real-data evaluation. Asserts only what must hold for the result to mean anything -- total labellings, solves inside
# the proven bound, and beating the majority class. Accuracy against the other baselines is reported, not asserted.
add_executable(eval_learn eval/eval_learn.cpp)
target_link_libraries(eval_learn PRIVATE quorum_learn)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(eval_learn PRIVATE ${QUORUM_LEARN_WARNINGS})
endif()
add_test(NAME learn_eval_voting
         COMMAND eval_learn voting ${CMAKE_CURRENT_SOURCE_DIR}/data/house-votes-84.data)
add_test(NAME learn_eval_tictactoe
         COMMAND eval_learn tictactoe ${CMAKE_CURRENT_SOURCE_DIR}/data/tic-tac-toe.data)

add_executable(test_discretize test/test_discretize.cpp)
target_link_libraries(test_discretize PRIVATE quorum_learn)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(test_discretize PRIVATE ${QUORUM_LEARN_WARNINGS})
endif()
add_test(NAME learn_discretize COMMAND test_discretize)

# The pre-registered experiments (PREREGISTRATION.md): v2 eval_hybrid on AI4I, v3 eval_monks, v4 eval_synth.
#
# Each is built twice.
#   eval_X       PORTABLE: random choices from portable_random.hpp and no fused multiply-add, so the output is the
#                same on every compiler and CPU. Checked against the portable re-run records (2026-10-06).
#   eval_X_orig  the experiment exactly as run on 2026-09-30: standard-library shuffle and distributions, default
#                floating-point contraction. Its output depends on the standard library, so the original records
#                are only checked where they were made: GCC 9 (libstdc++ 9).
find_package(Threads REQUIRED)
foreach(e eval_hybrid eval_monks eval_synth)
    add_executable(${e} eval/${e}.cpp)
    add_executable(${e}_orig eval/${e}.cpp)
    target_compile_definitions(${e}_orig PRIVATE QUORUM_LEARN_STD_RANDOM=1)
    foreach(t ${e} ${e}_orig)
        target_link_libraries(${t} PRIVATE quorum_learn Threads::Threads)
        if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
            target_compile_options(${t} PRIVATE ${QUORUM_LEARN_WARNINGS})
        endif()
    endforeach()
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        # One rounding for a*b+c everywhere: an aarch64 compiler fuses it into one instruction by default, an x86-64
        # one without -march does not, and that alone moved v3's numbers on a Raspberry Pi 5.
        target_compile_options(${e} PRIVATE -ffp-contract=off)
    endif()
endforeach()

# The learning piece of the compiler, end to end: learned model -> .cb -> cbc passes -> every state against the learner.
add_executable(test_learned_casebook test/test_learned_casebook.cpp)
target_link_libraries(test_learned_casebook PRIVATE quorum_learn quorum_cbc)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(test_learned_casebook PRIVATE ${QUORUM_LEARN_WARNINGS})
endif()
set(QUORUM_LEARNED_DIR ${CMAKE_CURRENT_BINARY_DIR}/learned)
file(MAKE_DIRECTORY ${QUORUM_LEARNED_DIR})
add_test(NAME learn_casebook_e2e
         COMMAND test_learned_casebook ${CMAKE_CURRENT_SOURCE_DIR}/data/monks ${QUORUM_LEARNED_DIR})
set_tests_properties(learn_casebook_e2e PROPERTIES FIXTURES_SETUP learned_casebooks)
# ...and the cbc binary itself, full validation pass, over every casebook the learner wrote -- as for pump.cb.
#
# Two of the six are genuinely defeasible -- learned exceptions that something can argue against -- and cbc accepts
# them. The other four are pure rule lists: three are the casebook alone (the learner chose k = 0 and added nothing)
# and one added rules but no exceptions. cbc REFUSES those, for the reason it refuses a raw FMEA import: "nothing is
# defeasible ... this is a decision tree". That refusal is correct and is asserted here, for exactly that reason and no
# other, rather than worked around: a learner that may only add hard rules has not given the device anything to argue.
add_test(NAME learn_cbc_accepts_defeasible
         COMMAND sh -c "for cb in monks-1-knows-2 monks-3-knows-1; do \"$0\" \"$1/$cb.cb\" --quiet || exit 1; done"
                 $<TARGET_FILE:cbc> ${QUORUM_LEARNED_DIR})
add_test(NAME learn_cbc_refuses_rule_lists
         COMMAND sh -c "for cb in monks-1-knows-1 monks-1-knows-3 monks-3-knows-2 monks-3-knows-3; do out=$(\"$0\" \"$1/$cb.cb\" --quiet 2>&1); rc=$?; echo \"$cb: $out\"; [ $rc -ne 0 ] && [ $(echo \"$out\" | grep -c '  FAIL') -eq 1 ] && echo \"$out\" | grep -q 'FAIL  at least one claim can be argued against' || exit 1; done"
                 $<TARGET_FILE:cbc> ${QUORUM_LEARNED_DIR})
set_tests_properties(learn_cbc_accepts_defeasible learn_cbc_refuses_rule_lists PROPERTIES
                     FIXTURES_REQUIRED learned_casebooks)

# The device path, on the host: the learner writes the casebook at BUILD time, cbc compiles it to the same header the
# Pico firmware uses, and a host program runs that header over every state against the learner's own fingerprint.
set(QUORUM_LEARNED_CB ${QUORUM_LEARNED_DIR}/monks-1-knows-2.cb)
set(QUORUM_LEARNED_EXPECTED ${QUORUM_LEARNED_DIR}/monks-1-knows-2.expected)
set(QUORUM_LEARNED_HPP ${QUORUM_LEARNED_DIR}/monks_learned_generated.hpp)
add_custom_command(
    OUTPUT ${QUORUM_LEARNED_CB} ${QUORUM_LEARNED_EXPECTED}
    COMMAND test_learned_casebook ${CMAKE_CURRENT_SOURCE_DIR}/data/monks ${QUORUM_LEARNED_DIR}
    DEPENDS test_learned_casebook ${CMAKE_CURRENT_SOURCE_DIR}/data/monks/monks-1.train
    COMMENT "learn: writing learned casebooks")
add_custom_command(
    OUTPUT ${QUORUM_LEARNED_HPP}
    COMMAND cbc ${QUORUM_LEARNED_CB} --header ${QUORUM_LEARNED_HPP} --quiet
    DEPENDS cbc ${QUORUM_LEARNED_CB}
    COMMENT "cbc: compiling the learned casebook")
add_executable(learned_header_check test/learned_header_check.cpp ${QUORUM_LEARNED_HPP})
target_include_directories(learned_header_check PRIVATE ${QUORUM_LEARNED_DIR})
target_link_libraries(learned_header_check PRIVATE quorum_solver quorum_arbiter)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(learned_header_check PRIVATE ${QUORUM_LEARN_WARNINGS})
endif()
add_test(NAME learn_device_header COMMAND learned_header_check ${QUORUM_LEARNED_EXPECTED})

# The pre-registered tests, run in full and compared byte for byte with the outcome recorded in PREREGISTRATION.md.
# A change that moves any reported number fails here, so the record cannot drift away from the code silently.
function(quorum_learn_record name recorded)
    add_test(NAME ${name}
             COMMAND sh -c "\"$0\" \"$@\" > \"${CMAKE_CURRENT_BINARY_DIR}/${name}.out\" && cmp -s \"${CMAKE_CURRENT_BINARY_DIR}/${name}.out\" \"${recorded}\" || { diff \"${recorded}\" \"${CMAKE_CURRENT_BINARY_DIR}/${name}.out\" | head -20; exit 1; }"
                     ${ARGN})
endfunction()

# The original records, 2026-09-30. Reproducible only with the standard library they were made with.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 10
   AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    quorum_learn_record(learn_prereg_v2_ai4i ${CMAKE_CURRENT_SOURCE_DIR}/eval/eval_hybrid-v2-2026-09-30.txt
        $<TARGET_FILE:eval_hybrid_orig> ${CMAKE_CURRENT_SOURCE_DIR}/data/ai4i2020.csv --quick --binding learned)
    quorum_learn_record(learn_prereg_v3_monks ${CMAKE_CURRENT_SOURCE_DIR}/eval/eval_monks-v3-2026-09-30.txt
        $<TARGET_FILE:eval_monks_orig> ${CMAKE_CURRENT_SOURCE_DIR}/data/monks)
    quorum_learn_record(learn_prereg_v4_synth ${CMAKE_CURRENT_SOURCE_DIR}/eval/eval_synth-v4-2026-09-30.txt
        $<TARGET_FILE:eval_synth_orig>)
else()
    message(STATUS "learn: original 2026-09-30 records need GCC 9 on x86-64; checking the portable re-run only")
endif()

# The portable re-run, 2026-10-06: the same experiments, same seeds, same criteria, portable random choices.
# Byte-identical on GCC 9 and GCC 14 (x86-64) and GCC 14 on a Raspberry Pi 5 (aarch64).
quorum_learn_record(learn_rerun_v2_ai4i ${CMAKE_CURRENT_SOURCE_DIR}/eval/eval_hybrid-v2-portable-2026-10-06.txt
    $<TARGET_FILE:eval_hybrid> ${CMAKE_CURRENT_SOURCE_DIR}/data/ai4i2020.csv --quick --binding learned)
quorum_learn_record(learn_rerun_v3_monks ${CMAKE_CURRENT_SOURCE_DIR}/eval/eval_monks-v3-portable-2026-10-06.txt
    $<TARGET_FILE:eval_monks> ${CMAKE_CURRENT_SOURCE_DIR}/data/monks)
quorum_learn_record(learn_rerun_v4_synth ${CMAKE_CURRENT_SOURCE_DIR}/eval/eval_synth-v4-portable-2026-10-06.txt
    $<TARGET_FILE:eval_synth>)

# learn2cb: draft a casebook from a records table. Used by quorum studio.
add_executable(learn2cb tools/learn2cb.cpp)
target_link_libraries(learn2cb PRIVATE quorum_learn)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(learn2cb PRIVATE ${QUORUM_LEARN_WARNINGS})
endif()
add_test(NAME learn2cb_ai4i
         COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/test/check_learn2cb.sh $<TARGET_FILE:learn2cb> $<TARGET_FILE:cbc>
                 ${CMAKE_CURRENT_SOURCE_DIR}/data/ai4i2020.csv)
