# qcert: the independent certificate checker. Plain C99 and no quorum headers, on purpose -- see qcert.c.
enable_language(C)
add_executable(qcert qcert.c)
set_target_properties(qcert PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(qcert PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    foreach(cb pump/pump.cb tank/tank.cb permit/permit.cb fixtures/consistency/flow_current.cb)
        get_filename_component(stem ${cb} NAME_WE)
        add_test(NAME qcert_${stem}
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tamper_test.py $<TARGET_FILE:cbc>
                    $<TARGET_FILE:qcert> ${CMAKE_SOURCE_DIR}/casebook/${cb} ${CMAKE_CURRENT_BINARY_DIR}/work_${stem})
    endforeach()
endif()
