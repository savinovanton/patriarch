find_package(doctest CONFIG REQUIRED)

add_executable(patriarch_tests
    main.cpp
    core_smoke.cpp
    sim_smoke.cpp)

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
