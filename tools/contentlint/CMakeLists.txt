# contentlint (E4-T08): checks content roots and localisation tables. The library part is separate so
# that tests/contentlint_test.cpp can drive it with in-memory corpora. No engine dependency.
add_library(patriarch_contentlint STATIC lint.hpp lint.cpp cli.cpp)
add_library(patriarch::contentlint ALIAS patriarch_contentlint)
target_include_directories(patriarch_contentlint PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(patriarch_contentlint PUBLIC patriarch::data patriarch::loc patriarch::content patriarch::core)
patriarch_set_target_options(patriarch_contentlint)
set_target_properties(patriarch_contentlint PROPERTIES FOLDER "tools")

add_executable(contentlint main.cpp)
target_link_libraries(contentlint PRIVATE patriarch::contentlint)
patriarch_set_target_options(contentlint)
patriarch_copy_asan_runtime(contentlint)
set_target_properties(contentlint PROPERTIES FOLDER "tools")

if(PATRIARCH_BUILD_TESTS)
    set(CL_CORPUS "${PROJECT_SOURCE_DIR}/tests/data/contentlint")

    # The real game content is clean. Keys of the loc table are not wired to the UI yet (the UI task
    # replaces the literals), so ui.* is exempt from the unused-key check until then.
    add_test(NAME contentlint_data_clean
        COMMAND contentlint --root "${PROJECT_SOURCE_DIR}/data" --strict --ignore-unused ui.)
    # A small complete corpus (content, two languages, every key referenced) passes with --strict.
    add_test(NAME contentlint_good_corpus
        COMMAND contentlint --root "${CL_CORPUS}/good" --no-rules --strict)
    set_tests_properties(contentlint_data_clean contentlint_good_corpus PROPERTIES TIMEOUT 60)

    # Each error corpus must make the linter exit with 1 (the doctest suite checks the exact codes).
    foreach(bad IN ITEMS schema reference loc_duplicate loc_plural)
        add_test(NAME contentlint_detects_${bad}
            COMMAND contentlint --root "${CL_CORPUS}/bad/${bad}" --no-rules --strict)
        set_tests_properties(contentlint_detects_${bad} PROPERTIES WILL_FAIL TRUE TIMEOUT 60)
    endforeach()
endif()
