add_executable(simcli simcli/main.cpp)
target_link_libraries(simcli PRIVATE patriarch::sim patriarch::content)
target_compile_definitions(simcli PRIVATE PATRIARCH_M1_DATA_DIR="${PROJECT_SOURCE_DIR}/data")
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

add_subdirectory(contentlint)
add_subdirectory(schemadoc)
add_subdirectory(stressgen)
add_subdirectory(tick_spike)
add_subdirectory(trigger_spike)
add_subdirectory(snapshot_spike)

if(PATRIARCH_BUILD_TESTS)
    find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)
    add_test(NAME simcli_telemetry COMMAND Python3::Interpreter "${PROJECT_SOURCE_DIR}/tests/tools/telemetry_cli_test.py" "$<TARGET_FILE:simcli>")
    set_tests_properties(simcli_telemetry PROPERTIES LABELS telemetry TIMEOUT 60)
    add_test(NAME simcli_m1 COMMAND Python3::Interpreter
        "${PROJECT_SOURCE_DIR}/tests/tools/m1_scenario_cli_test.py"
        "$<TARGET_FILE:simcli>" "$<TARGET_FILE:mapgen>" "${PROJECT_SOURCE_DIR}/data"
        a0b801b1a02e5bd6 1b0e8a4035e0476e)
    set_tests_properties(simcli_m1 PROPERTIES TIMEOUT 90)
endif()
