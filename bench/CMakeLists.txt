find_package(benchmark CONFIG REQUIRED)

add_executable(bench main.cpp support.cpp rng_bench.cpp)
target_link_libraries(bench PRIVATE patriarch::core benchmark::benchmark)
patriarch_set_target_options(bench)
patriarch_copy_asan_runtime(bench)
set_target_properties(bench PROPERTIES FOLDER "bench")

if(PATRIARCH_BUILD_TESTS)
    find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)
    add_test(NAME bench_smoke
        COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/smoke_test.py"
            "$<TARGET_FILE:bench>" "$<TARGET_FILE_DIR:bench>/bench-smoke.json")
    set_tests_properties(bench_smoke PROPERTIES LABELS bench TIMEOUT 60)
endif()

target_sources(bench PRIVATE telemetry_bench.cpp)
target_link_libraries(bench PRIVATE patriarch::sim)
if(PATRIARCH_BUILD_TESTS)
    add_test(NAME bench_telemetry COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/telemetry_smoke_test.py" "$<TARGET_FILE:bench>" "$<TARGET_FILE_DIR:bench>/bench-telemetry.json")
    set_tests_properties(bench_telemetry PROPERTIES LABELS "bench;telemetry" TIMEOUT 60)
endif()
