# The number of threads must not change anything cbc writes: exhaustive validation merges the per-thread ranges in
# state order so that the worst-case state, the examples and every fingerprint come out the same as one thread's.
# Run with -DCBC=<cbc> -DCB=<casebook> -DOUT=<dir>.
foreach(t 1 3 16)
  set(ENV{QUORUM_THREADS} ${t})
  execute_process(COMMAND ${CBC} ${CB} --header ${OUT}/threads_${t}.hpp --blob ${OUT}/threads_${t}.bin
                  OUTPUT_FILE ${OUT}/threads_${t}.txt ERROR_VARIABLE err RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "cbc failed with ${t} threads: ${err}")
  endif()
  file(READ ${OUT}/threads_${t}.txt txt)
  string(REGEX REPLACE "checking [0-9]+ states on [0-9]+ threads[^\n]*\n" "" txt "${txt}")
  string(REPLACE "threads_${t}." "threads_N." txt "${txt}")
  file(WRITE ${OUT}/threads_${t}.norm "${txt}")
endforeach()
foreach(t 3 16)
  foreach(ext hpp bin norm)
    execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files ${OUT}/threads_1.${ext} ${OUT}/threads_${t}.${ext}
                    RESULT_VARIABLE same)
    if(NOT same EQUAL 0)
      message(FATAL_ERROR "threads_${t}.${ext} differs from the single-threaded result")
    endif()
  endforeach()
endforeach()
