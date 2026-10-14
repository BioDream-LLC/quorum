# Dropping a preference-dependent attack from a strictly-less-severe claim must change outcomes, conservatively.
#
# ASPIC+ says a rebuttal from a less-preferred argument does not succeed. This compiler implements that with FMEA
# severity as the preference order -- a load-bearing semantic that was documented and never independently checked.
#
# Measured: the two fixtures differ ONLY in the rebutting claim's severity.
#   sev_lo  benign at severity 2, wear at 9  -> attack dropped -> stop in 2 of 4 states
#   sev_hi  benign at severity 9, wear at 9  -> attack stands  -> stop in 1 of 4 states
#
# The direction matters as much as the numbers: dropping the weaker attack makes the safety function act MORE
# often, not less. If a future change reverses that, this test must fail loudly.
execute_process(COMMAND ${CBC} ${LO} OUTPUT_VARIABLE lo ERROR_VARIABLE loe)
execute_process(COMMAND ${CBC} ${HI} OUTPUT_VARIABLE hi ERROR_VARIABLE hie)
if(NOT "${lo}${loe}" MATCHES "cancelled by FMEA severity")
    message(FATAL_ERROR "a severity-2 rebuttal against a severity-9 claim must be cancelled\n----\n${lo}${loe}")
endif()
if(NOT "${lo}${loe}" MATCHES "stop +2 +50\\.0%")
    message(FATAL_ERROR "with the weak rebuttal dropped, stop should hold in 2 of 4 states\n----\n${lo}${loe}")
endif()
if("${hi}${hie}" MATCHES "cancelled by FMEA severity")
    message(FATAL_ERROR "an EQUAL-severity rebuttal must NOT be cancelled\n----\n${hi}${hie}")
endif()
if(NOT "${hi}${hie}" MATCHES "stop +1 +25\\.0%")
    message(FATAL_ERROR "with the rebuttal standing, stop should hold in only 1 of 4 states\n----\n${hi}${hie}")
endif()
message(STATUS "OK: dropping a less-severe rebuttal moves stop from 1/4 to 2/4 states -- conservative direction")
