add_executable(simcli simcli/main.cpp)
target_link_libraries(simcli PRIVATE patriarch::sim)
patriarch_set_target_options(simcli)
patriarch_copy_asan_runtime(simcli)
set_target_properties(simcli PROPERTIES FOLDER "tools")

if(PATRIARCH_BUILD_TESTS)
    # End-to-end determinism check through the command line tool: two direct runs, a threaded run
    # and a replay must all agree. Fails the test run on any mismatch.
    add_test(NAME simcli_determinism COMMAND simcli --verify-determinism --days 800 --seed 7)
    set_tests_properties(simcli_determinism PROPERTIES TIMEOUT 120)
endif()

add_executable(mapgen mapgen/main.cpp)
target_link_libraries(mapgen PRIVATE patriarch::mapdata)
patriarch_set_target_options(mapgen)
patriarch_copy_asan_runtime(mapgen)
set_target_properties(mapgen PROPERTIES FOLDER "tools")

if(PATRIARCH_BUILD_TESTS)
    # End to end: generate a mid-size map, write it, reload it and validate it (nonzero exit on any failure).
    add_test(NAME mapgen_roundtrip
        COMMAND ${CMAKE_COMMAND}
            -DMAPGEN=$<TARGET_FILE:mapgen>
            -DOUT=${CMAKE_CURRENT_BINARY_DIR}/mapgen_roundtrip.pmd
            -P ${CMAKE_CURRENT_SOURCE_DIR}/mapgen/roundtrip.cmake)
    set_tests_properties(mapgen_roundtrip PROPERTIES TIMEOUT 300)
endif()

add_subdirectory(stressgen)
add_subdirectory(tick_spike)
add_subdirectory(trigger_spike)
add_subdirectory(snapshot_spike)
