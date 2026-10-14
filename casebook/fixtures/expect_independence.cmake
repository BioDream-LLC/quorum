# The protection-layer independence check must fire on pump.cb and stay silent on pump_conditions.cb.
#
# From the CCPS Independent Protection Layer criteria (via the U. Michigan SAFEChE LOPA tutorial): a layer
# must function independently of all other layers used for the same scenario. In pump.cb every one of the
# four routes to `maintain` needs `vib_high`, so one quiet accelerometer disables the whole protection --
# four diagnoses wearing one sensor. pump_conditions.cb adds `bearing_hot needs bearing_temp_high` and
# `npsh_eroded needs suction_low`, which route around the accelerometer and make the layers independent.
#
# Both directions are asserted. A check that only ever fires is as useless as one that never does, and
# pump_conditions is the evidence that the reported defect is fixable rather than intrinsic.
execute_process(COMMAND ${CBC} ${PUMP} OUTPUT_VARIABLE o1 ERROR_VARIABLE e1)
if(NOT "${o1}${e1}" MATCHES "COMMON CAUSE[ \t]+maintain[ \t]+all 4 routes depend on: vib_high")
    message(FATAL_ERROR
        "pump.cb should report `maintain` as common-cause on vib_high: all four diagnoses need the\n"
        "accelerometer, so one dead sensor disables the whole maintain protection.\n----\n${o1}${e1}")
endif()

execute_process(COMMAND ${CBC} ${COND} OUTPUT_VARIABLE o2 ERROR_VARIABLE e2)
if("${o2}${e2}" MATCHES "COMMON CAUSE[ \t]+maintain")
    message(FATAL_ERROR
        "pump_conditions.cb should NOT report `maintain` as common-cause: bearing_hot and npsh_eroded\n"
        "route around the accelerometer, which is exactly what makes the layers independent.\n----\n${o2}${e2}")
endif()
message(STATUS "OK: independence violation reported for pump.cb, absent for pump_conditions.cb")
