# The compiler's refusal LIMITS must be enforced, and the error must say why.
#
# Found by mutation testing: raising `observation_count > 30` to `> 300`, and `combinations > 64` to `> 640`, both
# left the ENTIRE 42-test suite passing. These guards prevent silent corruption -- past 31 observations
# binding::bit() returns 0 and those observations never assert -- so an untested guard is a guard that can be
# quietly removed.
#
# Asserting the MESSAGE as well as the refusal, because an error that does not explain the mask width teaches the
# author nothing and invites them to raise the cap.
execute_process(COMMAND ${CBC} ${OBS} OUTPUT_VARIABLE oo ERROR_VARIABLE oe RESULT_VARIABLE ores)
if(ores EQUAL 0)
    message(FATAL_ERROR "cbc ACCEPTED 31 observations. The instantiation mask is 32 bits and binding::bit() "
                        "returns 0 past index 31, so those observations would silently never assert.\n----\n${oo}${oe}")
endif()
if(NOT "${oo}${oe}" MATCHES "32 bits")
    message(FATAL_ERROR "the refusal must explain the 32-bit mask, or an author will simply raise the cap\n----\n${oo}${oe}")
endif()

execute_process(COMMAND ${CBC} ${VOTE} OUTPUT_VARIABLE vo ERROR_VARIABLE ve RESULT_VARIABLE vres)
if(vres EQUAL 0)
    message(FATAL_ERROR "cbc ACCEPTED `vote 5 of` over ten transmitters -- C(10,5) = 252 branch claims, well past "
                        "the 64 cap.\n----\n${vo}${ve}")
endif()
message(STATUS "OK: both compiler refusal limits enforced, and both errors explain themselves")
