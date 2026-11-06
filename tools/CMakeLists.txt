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
