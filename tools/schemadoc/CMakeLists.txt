add_executable(schemadoc main.cpp)
target_link_libraries(schemadoc PRIVATE patriarch::data)
patriarch_set_target_options(schemadoc)
patriarch_copy_asan_runtime(schemadoc)
set_target_properties(schemadoc PROPERTIES FOLDER "tools")

if(PATRIARCH_BUILD_TESTS)
    # The committed reference must equal the generator output; a different file must be rejected (exit 1).
    add_test(NAME schemadoc_check
        COMMAND schemadoc --check "${PROJECT_SOURCE_DIR}/docs/dev/data-reference.md")
    add_test(NAME schemadoc_check_detects_diff
        COMMAND schemadoc --check "${PROJECT_SOURCE_DIR}/README.md")
    set_tests_properties(schemadoc_check_detects_diff PROPERTIES WILL_FAIL TRUE)
    set_tests_properties(schemadoc_check schemadoc_check_detects_diff PROPERTIES TIMEOUT 30)
endif()
