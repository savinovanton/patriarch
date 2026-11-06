find_package(doctest CONFIG REQUIRED)

add_executable(patriarch_tests
    main.cpp
    arena_test.cpp
    assert_test.cpp
    calendar_test.cpp
    command_log_test.cpp
    command_test.cpp
    core_smoke.cpp
    date_test.cpp
    fixed_test.cpp
    ids_test.cpp
    log_test.cpp
    profile_test.cpp
    result_test.cpp
    rng_test.cpp
    sim_smoke.cpp
    simulation_test.cpp
    slot_map_test.cpp
    sparse_set_test.cpp
    spsc_queue_test.cpp
    static_vec_test.cpp
    string_interner_test.cpp
    world_test.cpp)

target_link_libraries(patriarch_tests PRIVATE
    patriarch::core
    patriarch::sim
    doctest::doctest)

# Use the real <ostream>/<string> so doctest can stringify std::string_view etc.
target_compile_definitions(patriarch_tests PRIVATE DOCTEST_CONFIG_USE_STD_HEADERS)

patriarch_set_target_options(patriarch_tests)
set_target_properties(patriarch_tests PROPERTIES FOLDER "tests")

if(MSVC AND PATRIARCH_SANITIZE_ADDRESS)
    # MSVC's ASan runtime DLLs live next to cl.exe and are not on PATH by default.
    get_filename_component(_patriarch_cl_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    add_custom_command(TARGET patriarch_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_patriarch_cl_dir}/clang_rt.asan_dynamic-x86_64.dll"
            "${_patriarch_cl_dir}/clang_rt.asan_dbg_dynamic-x86_64.dll"
            "$<TARGET_FILE_DIR:patriarch_tests>"
        VERBATIM)
endif()

include(CTest)
include(${doctest_DIR}/doctest.cmake)
doctest_discover_tests(patriarch_tests)
