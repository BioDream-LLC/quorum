# The burden-of-proof report must name a bare excuse.
execute_process(COMMAND ${CBC} ${CB} OUTPUT_VARIABLE out ERROR_VARIABLE err)
set(all "${out}${err}")
if(NOT all MATCHES "HOLDS BY DEFAULT[ \t]+bare")
    message(FATAL_ERROR
        "the burden-of-proof report did not name the bare excuse.\n"
        "A bare excuse holds in every state and therefore makes its target NEVER hold; naming it is the\n"
        "whole point of this report, and claim reach only shows the symptom.\n----\n${all}")
endif()
if(NOT all MATCHES "NEVER HOLDS")
    message(FATAL_ERROR "expected claim reach to also report NEVER HOLDS\n----\n${all}")
endif()
message(STATUS "OK: a bare excuse is named by the burden-of-proof report")
