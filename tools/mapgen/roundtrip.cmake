# Generates a 2048x1024 map with 400 provinces, then loads it again with --info (which validates).
execute_process(COMMAND ${MAPGEN} --out ${OUT} --width 2048 --height 1024 --provinces 400 --seed 7
                RESULT_VARIABLE gen_result)
if(NOT gen_result EQUAL 0)
    message(FATAL_ERROR "mapgen generation failed (${gen_result})")
endif()
execute_process(COMMAND ${MAPGEN} --info ${OUT} RESULT_VARIABLE info_result)
file(REMOVE ${OUT})
if(NOT info_result EQUAL 0)
    message(FATAL_ERROR "mapgen --info failed (${info_result})")
endif()
