# Migrates a PMD1 map to a PMD2 set (mapgen verifies the round trip itself), then loads both files of
# the set again with --info (which validates). Any failure is a nonzero exit.
file(REMOVE_RECURSE ${OUT})
execute_process(COMMAND ${MAPGEN} --migrate ${IN} ${OUT} RESULT_VARIABLE migrate_result)
if(NOT migrate_result EQUAL 0)
    message(FATAL_ERROR "mapgen --migrate failed (${migrate_result})")
endif()
foreach(name physical.pmd provinces.pmd)
    execute_process(COMMAND ${MAPGEN} --info ${OUT}/${name} RESULT_VARIABLE info_result)
    if(NOT info_result EQUAL 0)
        message(FATAL_ERROR "mapgen --info ${name} failed (${info_result})")
    endif()
endforeach()
file(REMOVE_RECURSE ${OUT})
