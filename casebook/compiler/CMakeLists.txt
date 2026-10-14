cmake_minimum_required(VERSION 3.16)

# cbc is a HOST tool. It uses the standard library freely, and is deliberately excluded from every freestanding
# gate -- the split is the point: an engineer authors text, the compiler emits a device header, and only the
# generated artefact goes near a microcontroller.

add_library(quorum_cbc STATIC
    src/parse.cpp
    src/lower.cpp
    src/merge.cpp
    src/emit.cpp
    src/blob.cpp
    src/symbolic.cpp
)
target_include_directories(quorum_cbc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(quorum_cbc PUBLIC quorum_solver quorum_arbiter)
target_compile_features(quorum_cbc PUBLIC cxx_std_17)

add_executable(cbc src/main.cpp)
target_link_libraries(cbc PRIVATE quorum_cbc)

add_executable(test_cbc test/test_cbc.cpp)
target_link_libraries(test_cbc PRIVATE quorum_cbc)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    foreach(t quorum_cbc cbc test_cbc)
        target_compile_options(${t} PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Werror)
    endforeach()
endif()

add_test(NAME compiler COMMAND test_cbc)

# ── Equivalence against the hand-written casebook ───────────────────────────
#
# The pump casebook exists twice: hand-written in C++, and as 46 lines of source. Generating the header at build
# time and comparing the two across all 16,384 states is the strongest available check that the compiler means what
# the author meant.
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/gen)

add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/gen/pump_generated.hpp
    COMMAND cbc ${CMAKE_CURRENT_SOURCE_DIR}/../pump/pump.cb --quiet
                --header ${CMAKE_CURRENT_BINARY_DIR}/gen/pump_generated.hpp
    DEPENDS cbc ${CMAKE_CURRENT_SOURCE_DIR}/../pump/pump.cb
    COMMENT "cbc: compiling pump.cb"
    VERBATIM
)

add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/gen/tank_generated.hpp
    COMMAND cbc ${CMAKE_CURRENT_SOURCE_DIR}/../tank/tank.cb --quiet
                --header ${CMAKE_CURRENT_BINARY_DIR}/gen/tank_generated.hpp
    DEPENDS cbc ${CMAKE_CURRENT_SOURCE_DIR}/../tank/tank.cb
    COMMENT "cbc: compiling tank.cb"
    VERBATIM
)

add_executable(test_equivalence
    test/test_equivalence.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/gen/pump_generated.hpp
    ${CMAKE_CURRENT_BINARY_DIR}/gen/tank_generated.hpp
)
target_link_libraries(test_equivalence PRIVATE quorum_solver quorum_arbiter)
target_include_directories(test_equivalence PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}/gen
    ${CMAKE_CURRENT_SOURCE_DIR}/../pump
)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    # The generated header must survive the same warnings as hand-written device code. A compiler that emits code
    # it would reject from a human is not finished.
    target_compile_options(test_equivalence PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Werror)
endif()

add_test(NAME compiler_equivalence COMMAND test_equivalence)

# ── The palette guarantee ───────────────────────────────────────────────────
# ── Symbolic validation agrees with the device solver ───────────────────────
add_executable(test_symbolic test/test_symbolic.cpp)
target_link_libraries(test_symbolic PRIVATE quorum_cbc)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(test_symbolic PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Werror)
endif()
add_test(NAME compiler_symbolic COMMAND test_symbolic ${CMAKE_CURRENT_SOURCE_DIR}/..)

add_executable(test_palette test/test_palette.cpp)
target_link_libraries(test_palette PRIVATE quorum_cbc)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(test_palette PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Werror)
endif()

add_test(NAME compiler_palette
    COMMAND test_palette ${CMAKE_CURRENT_SOURCE_DIR}/../pump/pump.cb
                         ${CMAKE_CURRENT_SOURCE_DIR}/../tank/tank.cb)

# End-to-end: compile the real casebooks and validate them exhaustively. If the compiler ever stops reproducing a
# working casebook, this fails.
add_test(NAME compiler_pump_e2e
    COMMAND cbc ${CMAKE_CURRENT_SOURCE_DIR}/../pump/pump.cb
            --header ${CMAKE_CURRENT_BINARY_DIR}/pump_e2e.hpp
            --af ${CMAKE_CURRENT_BINARY_DIR}/pump.af
            --apx ${CMAKE_CURRENT_BINARY_DIR}/pump.apx)

add_test(NAME compiler_tank_e2e
    COMMAND cbc ${CMAKE_CURRENT_SOURCE_DIR}/../tank/tank.cb
            --header ${CMAKE_CURRENT_BINARY_DIR}/tank_e2e.hpp)

# ── The whole stack, from raw instrument numbers ─────────────────────────────
#
# Everything else here starts from a presence mask, which means somebody has already decided which propositions are
# asserted. This one starts with milliamps and a clock, using the binding tables cbc generated, and asks whether the
# machine would have stopped the transfer at CAPECO.
add_executable(tank_capeco
    ../tank/tank_capeco.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/gen/tank_generated.hpp
)
target_link_libraries(tank_capeco PRIVATE quorum_solver quorum_arbiter quorum_witness quorum_binding)
target_include_directories(tank_capeco PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/gen)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(tank_capeco PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Werror)
endif()

add_test(NAME tank_capeco COMMAND tank_capeco)

# Is the emitted witness bound exact? Firmware sizes a fixed-capacity object on the strength of it, so the claim is
# machine-checked over every state of both casebooks rather than left as a comment.
add_executable(test_bound
    ../../witness/test/test_bound.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/gen/pump_generated.hpp
    ${CMAKE_CURRENT_BINARY_DIR}/gen/tank_generated.hpp
)
target_link_libraries(test_bound PRIVATE quorum_solver quorum_arbiter quorum_witness quorum_binding)
target_include_directories(test_bound PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/gen)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(test_bound PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Werror)
endif()
add_test(NAME witness_bound COMMAND test_bound)

# ── Third casebook: Piper Alpha permit-to-work ──────────────────────────────
#
# A third problem shape -- records and authority, rather than competing hypotheses or disagreeing sources. Also the
# sharpest available test of the closed-world/assumption distinction, since the accident WAS that confusion.
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/gen/permit_generated.hpp
    COMMAND cbc ${CMAKE_CURRENT_SOURCE_DIR}/../permit/permit.cb --quiet
                --header ${CMAKE_CURRENT_BINARY_DIR}/gen/permit_generated.hpp
    DEPENDS cbc ${CMAKE_CURRENT_SOURCE_DIR}/../permit/permit.cb
    COMMENT "cbc: compiling permit.cb"
    VERBATIM
)
add_executable(permit_scenarios
    ../permit/permit_scenarios.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/gen/permit_generated.hpp
)
target_link_libraries(permit_scenarios PRIVATE quorum_solver quorum_arbiter quorum_witness quorum_binding)
target_include_directories(permit_scenarios PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/gen)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(permit_scenarios PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Werror)
endif()
add_test(NAME permit_scenarios COMMAND permit_scenarios)

add_test(NAME compiler_permit_e2e
    COMMAND cbc ${CMAKE_CURRENT_SOURCE_DIR}/../permit/permit.cb
            --header ${CMAKE_CURRENT_BINARY_DIR}/permit_e2e.hpp)

# Diagnostics quality. Whether anybody other than the tool's author can use the language is decided by whether the
# compiler says WHERE and WHAT TO DO. Each case is a mistake a real author would make.
add_executable(test_diagnostics test/test_diagnostics.cpp)
target_link_libraries(test_diagnostics PRIVATE quorum_cbc)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(test_diagnostics PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Werror)
endif()
add_test(NAME compiler_diagnostics COMMAND test_diagnostics)

# The authoring guide's worked example. Registered as a test so the guide cannot rot: if the language changes and the
# example stops compiling or stops validating cleanly, the build fails and the documentation gets fixed with it.
add_test(NAME authoring_example
    COMMAND cbc ${CMAKE_CURRENT_SOURCE_DIR}/../example/gearbox.cb
            --header ${CMAKE_CURRENT_BINARY_DIR}/gearbox_generated.hpp)

# Casebooks the compiler must REFUSE. Each reaches a Verdict outcome that no correct casebook ever reaches, which is
# why the three real casebooks cannot test it -- all three report Conflicted and Unsatisfied in 0 states.
add_test(NAME compiler_rejects_broken
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/../fixtures/expect_rejected.sh
            $<TARGET_FILE:cbc> ${CMAKE_CURRENT_SOURCE_DIR}/../fixtures)

# ── The emitted palette header ──────────────────────────────────────────────
#
# A different artefact from the merged Lowered the other palette tests examine. An off-by-one in an observation
# offset, or a policy pointing at the wrong index, would pass every structural check and be catastrophic in the field.
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/gen/palette_generated.hpp
    COMMAND cbc ${CMAKE_CURRENT_SOURCE_DIR}/../pump/pump.cb
                ${CMAKE_CURRENT_SOURCE_DIR}/../tank/tank.cb
                --palette --quiet --header ${CMAKE_CURRENT_BINARY_DIR}/gen/palette_generated.hpp
    DEPENDS cbc ${CMAKE_CURRENT_SOURCE_DIR}/../pump/pump.cb ${CMAKE_CURRENT_SOURCE_DIR}/../tank/tank.cb
    COMMENT "cbc: compiling a pump + tank palette"
    VERBATIM
)
add_executable(test_palette_header
    test/test_palette_header.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/gen/palette_generated.hpp
    ${CMAKE_CURRENT_BINARY_DIR}/gen/pump_generated.hpp
    ${CMAKE_CURRENT_BINARY_DIR}/gen/tank_generated.hpp
)
target_link_libraries(test_palette_header PRIVATE quorum_solver quorum_arbiter quorum_binding)
target_include_directories(test_palette_header PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/gen)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(test_palette_header PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Werror)
endif()
add_test(NAME compiler_palette_header COMMAND test_palette_header)

# ── FMEA severity through ASPIC+ preference ordering ────────────────────────
#
# A controlled experiment: pump_severity.cb is pump.cb plus four severity annotations and nothing else, so the
# difference between their reports is attributable to the annotations alone. The assertions are aimed at DIRECTION,
# not at the reduction in ambiguity -- any mechanism that makes the machine commit more often reduces ambiguity,
# including a dangerous one.
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/gen/pump_severity_generated.hpp
    COMMAND cbc ${CMAKE_CURRENT_SOURCE_DIR}/../pump/pump_severity.cb --quiet
                --header ${CMAKE_CURRENT_BINARY_DIR}/gen/pump_severity_generated.hpp
    DEPENDS cbc ${CMAKE_CURRENT_SOURCE_DIR}/../pump/pump_severity.cb
    COMMENT "cbc: compiling pump_severity.cb"
    VERBATIM
)
add_executable(test_severity
    ../pump/pump_severity_test.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/gen/pump_generated.hpp
    ${CMAKE_CURRENT_BINARY_DIR}/gen/pump_severity_generated.hpp
)
target_link_libraries(test_severity PRIVATE quorum_solver quorum_arbiter quorum_binding)
target_include_directories(test_severity PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/gen)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(test_severity PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Werror)
endif()
add_test(NAME severity_preference COMMAND test_severity)

# How much does grounded leave undecided? Measured before designing the advisory tier, because the answer decides
# whether an EXACT enumeration is possible -- and an advisory tier that might have missed an extension cannot support
# any claim about the safety tier. Fails if the undecided set ever grows past subset enumeration.
add_executable(test_undecided_profile
    test/test_undecided_profile.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/gen/pump_generated.hpp
    ${CMAKE_CURRENT_BINARY_DIR}/gen/tank_generated.hpp
    ${CMAKE_CURRENT_BINARY_DIR}/gen/permit_generated.hpp)
target_link_libraries(test_undecided_profile PRIVATE quorum_solver quorum_arbiter quorum_binding)
target_include_directories(test_undecided_profile PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/gen)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(test_undecided_profile PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Werror)
endif()
add_test(NAME advisory_search_space COMMAND test_undecided_profile)

# Cross-directory consumers cannot list these generated headers as sources: CMake only resolves a custom command's
# OUTPUT to a source file within the same directory scope. A clean configure fails with "Cannot find source file",
# while an incremental one succeeds because the file happens to already exist -- so this breaks only in CI and only
# on a fresh checkout. Depend on this target instead.
# The fourth problem shape: redundancy and voting, which is how every safety instrumented function in process industry
# is built. vote_manual.cb is the same casebook with the 2-of-3 condition written out by hand, kept so `vote k of` can
# be proved equal to its expansion rather than assumed equal.
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/gen/vote_generated.hpp
    COMMAND cbc ${CMAKE_CURRENT_SOURCE_DIR}/../vote/vote.cb --quiet
                --header ${CMAKE_CURRENT_BINARY_DIR}/gen/vote_generated.hpp
    DEPENDS cbc ${CMAKE_CURRENT_SOURCE_DIR}/../vote/vote.cb
    COMMENT "cbc: vote.cb")

add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/gen/vote_manual_generated.hpp
    COMMAND cbc ${CMAKE_CURRENT_SOURCE_DIR}/../vote/vote_manual.cb --quiet
                --header ${CMAKE_CURRENT_BINARY_DIR}/gen/vote_manual_generated.hpp
    DEPENDS cbc ${CMAKE_CURRENT_SOURCE_DIR}/../vote/vote_manual.cb
    COMMENT "cbc: vote_manual.cb")

add_executable(test_vote_equivalence
    ../vote/test_vote_equivalence.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/gen/vote_generated.hpp
    ${CMAKE_CURRENT_BINARY_DIR}/gen/vote_manual_generated.hpp)
target_link_libraries(test_vote_equivalence PRIVATE quorum_solver quorum_arbiter quorum_binding)
target_include_directories(test_vote_equivalence PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/gen)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(test_vote_equivalence PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Werror)
endif()
add_test(NAME casebook_vote_equivalence COMMAND test_vote_equivalence)

# The casebook must also keep validating cleanly on its own.
add_test(NAME casebook_vote COMMAND cbc ${CMAKE_CURRENT_SOURCE_DIR}/../vote/vote.cb)

# A casebook that ignores `<channel>_unavailable` loses the safe action when the sensor dies mid-alarm.
# Deliberately defective; cbc warns about it, and this proves the warning is load-bearing rather than advice.
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/gen/blind_generated.hpp
    COMMAND cbc ${CMAKE_CURRENT_SOURCE_DIR}/../fixtures/blind_channel/blind.cb --quiet
                --header ${CMAKE_CURRENT_BINARY_DIR}/gen/blind_generated.hpp
    DEPENDS cbc ${CMAKE_CURRENT_SOURCE_DIR}/../fixtures/blind_channel/blind.cb
    COMMENT "cbc: compiling the deliberately blind fixture"
    VERBATIM
)

add_executable(blind_loses_safety ../fixtures/blind_channel/blind_loses_safety.cpp)
add_dependencies(blind_loses_safety quorum_generated_headers)
target_include_directories(blind_loses_safety PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}/gen
    ${CMAKE_CURRENT_SOURCE_DIR}/../../solver/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../arbiter/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../binding/include)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(blind_loses_safety PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Werror)
endif()
add_test(NAME casebook_blind_channel_loses_safety COMMAND blind_loses_safety)

add_custom_target(quorum_generated_headers DEPENDS
    ${CMAKE_CURRENT_BINARY_DIR}/gen/blind_generated.hpp
    ${CMAKE_CURRENT_BINARY_DIR}/gen/pump_conditions_generated.hpp
    ${CMAKE_CURRENT_BINARY_DIR}/gen/vote_generated.hpp
    ${CMAKE_CURRENT_BINARY_DIR}/gen/vote_manual_generated.hpp
    ${CMAKE_CURRENT_BINARY_DIR}/gen/pump_generated.hpp
    ${CMAKE_CURRENT_BINARY_DIR}/gen/pump_severity_generated.hpp
    ${CMAKE_CURRENT_BINARY_DIR}/gen/tank_generated.hpp
    ${CMAKE_CURRENT_BINARY_DIR}/gen/permit_generated.hpp
    ${CMAKE_CURRENT_BINARY_DIR}/gen/palette_generated.hpp)

# Pass 6: a claim that cannot reach an action. Not grounds for rejection -- the casebook behaves correctly -- but the
# report must say so, and a diagnostic nobody tests is one that quietly stops working. Found for real in tank-409.
add_test(NAME casebook_inert_claim_reported
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/../fixtures/expect_reported.sh
            $<TARGET_FILE:cbc>
            ${CMAKE_CURRENT_SOURCE_DIR}/../fixtures/inert.cb
            "NO PATH")

# The fingerprint must actually detect a modified header. Tests the FAILING path by mutating a real generated header --
# an integrity check is the easiest place in a codebase to write something vacuous, because the happy path always agrees
# with itself. This project already shipped one vacuous check (the duplicate-`idle` test) before catching it.
add_test(NAME casebook_fingerprint_tamper
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/../fixtures/tamper_check.sh
            ${CMAKE_CURRENT_BINARY_DIR}/gen/pump_generated.hpp
            quorum::casebook::gen::centrifugal_pump
            ${CMAKE_SOURCE_DIR}/solver/include
            ${CMAKE_SOURCE_DIR}/arbiter/include
            ${CMAKE_SOURCE_DIR}/binding/include)

# The .af/.apx export is the bridge to independent third-party verification -- the only available escape from the fact
# that every other test here compares my code against my other code. Nobody had checked the bridge: if the exported file
# describes a different framework than the device runs, an independent solver would confirm the wrong thing convincingly.
add_executable(test_export test/test_export.cpp)
add_dependencies(test_export quorum_generated_headers)
target_link_libraries(test_export PRIVATE quorum_solver quorum_arbiter quorum_binding)
target_include_directories(test_export PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/gen)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(test_export PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Werror)
endif()

# Depends on the files compiler_pump_e2e writes, so it must run after it.
add_test(NAME compiler_export
    COMMAND test_export ${CMAKE_CURRENT_BINARY_DIR}/pump.af ${CMAKE_CURRENT_BINARY_DIR}/pump.apx)
set_tests_properties(compiler_export PROPERTIES DEPENDS compiler_pump_e2e)

# Exports an instantiated framework in ICCMA i23 format so an independent solver can check quorum's answers. Unlike
# `cbc --af` this includes the closed-world edges, which depend on the cycle's readings -- so any state can be checked,
# not just the one where everything is asserted.
add_executable(dump_af test/dump_af.cpp)
add_dependencies(dump_af quorum_generated_headers)
target_link_libraries(dump_af PRIVATE quorum_solver quorum_arbiter quorum_binding)
target_include_directories(dump_af PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/gen)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(dump_af PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Werror)
endif()

# The FMEA importer, and the reason it exists as a DRAFT generator rather than a compiler. An FMEA records what fails; it
# does not record what would explain a reading away, and that second part is where the safety value lives.
add_executable(fmea2cb ../fmea/fmea2cb.cpp)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(fmea2cb PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Werror)
endif()

# The draft a real FMEA table produces must be REFUSED, because it has no defeaters. Importing that table is what revealed
# cbc would previously accept a defeater-free casebook with 100% Decided and every check green -- a decision tree wearing
# an argumentation engine.
add_test(NAME casebook_fmea_draft_refused
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/../fmea/check_draft_refused.sh
            $<TARGET_FILE:fmea2cb> $<TARGET_FILE:cbc>
            ${CMAKE_CURRENT_SOURCE_DIR}/../fmea/motor_fmea.csv)

# Controlled experiment: pump_conditions.cb is pump.cb plus three CONDITION claims and nothing else. It exists because
# comparing pump.cb against ordinary prioritised alarm annunciation showed pump.cb responds to no single symptom -- a hot
# bearing alone got `continue` while the alarm system said `shutdown`.
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/gen/pump_conditions_generated.hpp
    COMMAND cbc ${CMAKE_CURRENT_SOURCE_DIR}/../pump/pump_conditions.cb --quiet
                --header ${CMAKE_CURRENT_BINARY_DIR}/gen/pump_conditions_generated.hpp
    DEPENDS cbc ${CMAKE_CURRENT_SOURCE_DIR}/../pump/pump_conditions.cb
    COMMENT "cbc: pump_conditions.cb")

add_executable(pump_vs_alarms
    ../pump/pump_vs_alarms.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/gen/pump_generated.hpp
    ${CMAKE_CURRENT_BINARY_DIR}/gen/pump_conditions_generated.hpp)
target_link_libraries(pump_vs_alarms PRIVATE quorum_solver quorum_arbiter quorum_binding)
target_include_directories(pump_vs_alarms PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/gen)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(pump_vs_alarms PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Werror)
endif()
add_test(NAME pump_vs_alarms COMMAND pump_vs_alarms)
add_test(NAME casebook_pump_conditions COMMAND cbc ${CMAKE_CURRENT_SOURCE_DIR}/../pump/pump_conditions.cb)

# The 2oo3 truth table, including the single-deviating-transmitter case that a mechanised single-symptom check found
# missing. Both directions of discrepancy must stay detected: the dissenter that reads low (loud, trips) and the one that
# reads high (silent, and therefore the dangerous one).
add_executable(vote_scenarios
    ../vote/vote_scenarios.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/gen/vote_generated.hpp)
target_link_libraries(vote_scenarios PRIVATE quorum_solver quorum_arbiter quorum_binding)
target_include_directories(vote_scenarios PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/gen)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(vote_scenarios PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Werror)
endif()
add_test(NAME casebook_vote_scenarios COMMAND vote_scenarios)

# A bare excuse must be named by the burden-of-proof report, not merely implied by claim reach.
#
# Found by hand-encoding a real HAZOP: two claims came back NEVER HOLDS and the cause was two excuses
# with nothing supporting them. Claim reach reported the symptom; the burden-of-proof report, whose whole
# job is to name the cause, stayed silent because it inspected only claims.
add_test(NAME casebook_bare_excuse_reported
         COMMAND ${CMAKE_COMMAND}
                 -DCBC=$<TARGET_FILE:cbc>
                 -DCB=${CMAKE_CURRENT_SOURCE_DIR}/../fixtures/bare_excuse.cb
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/../fixtures/expect_bare_excuse.cmake)

# Protection-layer independence, in both directions.
#
# Derived from a real standard rather than invented: the CCPS IPL criteria require a protection layer to
# function independently of every other layer used for the same scenario. Found a real violation in pump.cb
# on the first run -- all four routes to `maintain` need `vib_high` -- and the condition claims added to
# pump_conditions.cb for an unrelated reason turn out to fix it.
add_test(NAME casebook_ipl_independence
         COMMAND ${CMAKE_COMMAND}
                 -DCBC=$<TARGET_FILE:cbc>
                 -DPUMP=${CMAKE_CURRENT_SOURCE_DIR}/../pump/pump.cb
                 -DCOND=${CMAKE_CURRENT_SOURCE_DIR}/../pump/pump_conditions.cb
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/../fixtures/expect_independence.cmake)

# hazop2cb -- draft a casebook from an IEC 61882 HAZOP worksheet.
#
# Separate from fmea2cb because the documents are genuinely different shapes, and the HAZOP is the better one: its
# Deviation column is already an observation, and its Safeguards column -- which no FMEA has -- is where a team records
# that the only protection against a deviation is the instrument whose failure causes it. That is CAPECO, in advance.
add_executable(hazop2cb ../fmea/hazop2cb.cpp)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(hazop2cb PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Werror)
endif()

add_test(NAME casebook_hazop_import
         COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/../fmea/check_hazop_import.sh
                 $<TARGET_FILE:hazop2cb> $<TARGET_FILE:cbc>
                 ${CMAKE_CURRENT_SOURCE_DIR}/../fmea/hazop_worksheet.csv)

# lopa2cb -- audit a Layer of Protection Analysis.
#
# The strongest of the three documents, because it states its own arithmetic. A HAZOP records protections in prose,
# so every finding about them rests on reading English -- and twenty-six registered assumptions about how people
# write that English turned out to be wrong. A LOPA asserts a conclusion and shows its working, so a machine can
# redo the working and disagree, and that kind of finding cannot be wrong because a phrase was misread.
add_executable(lopa2cb ../fmea/lopa2cb.cpp)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(lopa2cb PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Werror)
endif()

add_test(NAME casebook_lopa_import
         COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/../fmea/check_lopa_import.sh
                 $<TARGET_FILE:lopa2cb> $<TARGET_FILE:cbc>)

# The digest a report offers as proof of which revision it describes. Held to the published vectors, and to
# agreement with sha256sum on a real file -- the expected value is computed at configure time so the test itself
# does not shell out.
add_executable(test_sha256 ../fmea/test_sha256.cpp)
target_include_directories(test_sha256 PRIVATE ../fmea)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(test_sha256 PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Werror)
endif()
file(SHA256 ${CMAKE_CURRENT_SOURCE_DIR}/../fmea/lopa_biocontainment.csv QUORUM_LOPA_SHA)
add_test(NAME casebook_sha256
         COMMAND test_sha256 ${CMAKE_CURRENT_SOURCE_DIR}/../fmea/lopa_biocontainment.csv ${QUORUM_LOPA_SHA})

# An FMEA has no Safeguards column, so it is the weakest of the three documents for building a casebook -- and it
# states S, O, D and an RPN, which makes it auditable like a LOPA. MISSION.md's ranking was right about the first and
# wrong as a general claim.
add_test(NAME casebook_fmea_risk
         COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/../fmea/check_fmea_risk.sh $<TARGET_FILE:fmea2cb>)

add_test(NAME casebook_hazop_report
         COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/../fmea/check_hazop_report.sh $<TARGET_FILE:hazop2cb>)

# The review report is the first artefact a customer sees without a terminal, so every claim it makes about itself
# -- determinism, the digest, the reproduction command -- is checked.
add_test(NAME casebook_lopa_report
         COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/../fmea/check_lopa_report.sh $<TARGET_FILE:lopa2cb>)

# The remedy a report suggests must be measured, not reasoned about.
add_test(NAME casebook_merge_tradeoff
         COMMAND ${CMAKE_COMMAND} -DCBC=$<TARGET_FILE:cbc>
                 -DSPLIT=${CMAKE_CURRENT_SOURCE_DIR}/../fixtures/shared_cause/split.cb
                 -DMERGED=${CMAKE_CURRENT_SOURCE_DIR}/../fixtures/shared_cause/merged.cb
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/../fixtures/expect_merge_tradeoff.cmake)

add_test(NAME casebook_sole_route
         COMMAND ${CMAKE_COMMAND} -DCBC=$<TARGET_FILE:cbc>
                 -DPUMP=${CMAKE_CURRENT_SOURCE_DIR}/../pump/pump.cb
                 -DVOTE=${CMAKE_CURRENT_SOURCE_DIR}/../vote/vote.cb
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/../fixtures/expect_sole_route.cmake)

# The compiler's refusal limits. Mutation testing proved both were untested: loosening either left all 42 tests green.
add_test(NAME casebook_limits_refused
         COMMAND ${CMAKE_COMMAND} -DCBC=$<TARGET_FILE:cbc>
                 -DOBS=${CMAKE_CURRENT_SOURCE_DIR}/../fixtures/limits/too_many_observations.cb
                 -DVOTE=${CMAKE_CURRENT_SOURCE_DIR}/../fixtures/limits/vote_too_wide.cb
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/../fixtures/expect_limits_refused.cmake)

# Severity as ASPIC+ preference: documented, never independently checked until ENG-B10 was tested.
add_test(NAME casebook_severity_drop
         COMMAND ${CMAKE_COMMAND} -DCBC=$<TARGET_FILE:cbc>
                 -DLO=${CMAKE_CURRENT_SOURCE_DIR}/../fixtures/severity/sev_lo.cb
                 -DHI=${CMAKE_CURRENT_SOURCE_DIR}/../fixtures/severity/sev_hi.cb
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/../fixtures/expect_severity_drop.cmake)

# Generate each header ONCE. With the Makefile generator, a custom command whose output is listed in several targets
# gets one copy of the rule per target, and a parallel build can run two copies at once, both writing the same file:
# a native -j4 build on a Raspberry Pi 5 got a corrupted tank_generated.hpp that way (two interleaved cbc warnings in
# the log were the clue). Every consumer therefore waits for the one target that owns the headers.
foreach(t test_equivalence tank_capeco test_bound permit_scenarios test_palette_header test_severity test_undecided_profile test_vote_equivalence pump_vs_alarms vote_scenarios)
    add_dependencies(${t} quorum_generated_headers)
endforeach()

