# cbc --commissioning on the pump: one row per reading, the at-rest action, and the action order the device uses.
# Run with -DCBC=<cbc> -DCB=<pump.cb> -DOUT=<dir>.
execute_process(COMMAND ${CBC} ${CB} --quiet --commissioning ${OUT}/pump.sheet RESULT_VARIABLE rc ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "cbc failed: ${err}")
endif()
file(READ ${OUT}/pump.sheet s)
foreach(want "quorum-commissioning 1\n" "fingerprint 0x1a375f9d\n" "actions\tcontinue\tmaintain\tshutdown\n"
             "rest\tcontinue\tDecided\n" "row\t3\tbearing_temp_high\tpresence bit 3\tcontinue\tDecided\tvib_high\tshutdown\tcontinue\t"
             "row\t13\tcurrent_sidebands\t")
  string(FIND "${s}" "${want}" at)
  if(at EQUAL -1)
    message(FATAL_ERROR "the pump sheet lacks: ${want}\n${s}")
  endif()
endforeach()
string(REGEX MATCHALL "\nrow\t" rows "${s}")
list(LENGTH rows n)
if(NOT n EQUAL 14)
  message(FATAL_ERROR "${n} rows, want one per pump reading (14)")
endif()
