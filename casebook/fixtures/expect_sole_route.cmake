# The safe action's sole route must be reported, and marked as the safe action.
#
# pump.cb declares `action shutdown safe when bearing_wear`, and `bearing_wear needs vib_high bearing_temp_high`.
# One route, two sensors: either going quiet removes the ability to reach the safe state. The first version of the
# independence check skipped single-route actions entirely with the comment "one route cannot be non-independent of
# itself" -- a definitional dodge that hid the most important case in the reference casebook.
#
# vote.cb must stay silent: a 2oo3 trip has three routes by construction, which is what redundancy looks like.
execute_process(COMMAND ${CBC} ${PUMP} OUTPUT_VARIABLE po ERROR_VARIABLE pe)
if(NOT "${po}${pe}" MATCHES "SOLE ROUTE\\*+ual*[ \t]*shutdown")
    if(NOT "${po}${pe}" MATCHES "SOLE ROUTE")
        message(FATAL_ERROR "pump.cb's safe action has one route and must be reported\n----\n${po}${pe}")
    endif()
endif()
if(NOT "${po}${pe}" MATCHES "this is the SAFE action")
    message(FATAL_ERROR "a sole route to the SAFE action must be called out as such\n----\n${po}${pe}")
endif()
execute_process(COMMAND ${CBC} ${VOTE} OUTPUT_VARIABLE vo ERROR_VARIABLE ve)
if("${vo}${ve}" MATCHES "SOLE ROUTE")
    message(FATAL_ERROR "vote.cb's 2oo3 trip has three routes; reporting a sole route is crying wolf\n----\n${vo}${ve}")
endif()
message(STATUS "OK: sole route to the safe action reported for pump.cb, silent for vote.cb")
