# Merging a shared cause must be shown to REDUCE how often the safety function acts.
#
# The SHARED CAUSE report originally told authors to "merge them into a single claim that NEEDS the deviations
# together". Compiling both versions showed that advice reduces the trip from 3 of 8 states to 1 of 8: two
# independently sufficient conditions become one conjunctive necessary condition.
#
# This test pins the measurement the advice now quotes. If the engine's semantics ever change so that merging no
# longer reduces the trip rate, the wording in hazop2cb.cpp is wrong and must be rewritten.
execute_process(COMMAND ${CBC} ${SPLIT} OUTPUT_VARIABLE so ERROR_VARIABLE se)
execute_process(COMMAND ${CBC} ${MERGED} OUTPUT_VARIABLE mo ERROR_VARIABLE me)
if(NOT "${so}${se}" MATCHES "shutdown +3 +37\\.5%")
    message(FATAL_ERROR "split casebook should trip in 3 of 8 states\n----\n${so}${se}")
endif()
if(NOT "${mo}${me}" MATCHES "shutdown +1 +12\\.5%")
    message(FATAL_ERROR "merged casebook should trip in 1 of 8 states\n----\n${mo}${me}")
endif()
message(STATUS "OK: merging a shared cause cuts the trip from 3/8 states to 1/8 -- the advice must state the tradeoff")
