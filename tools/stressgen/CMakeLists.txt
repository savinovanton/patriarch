add_library(patriarch_stress STATIC
    include/pat/stress/world_generator.hpp
    include/pat/stress/dataset.hpp
    include/pat/stress/economy_generator.hpp
    include/pat/stress/economy_skeleton.hpp
    include/pat/stress/economy_bench.hpp
    world_generator.cpp
    economy_generator.cpp
    economy_skeleton.cpp
    economy_bench.cpp
    dataset.cpp)
target_include_directories(patriarch_stress PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(patriarch_stress PUBLIC patriarch::sim)
patriarch_set_target_options(patriarch_stress)
set_target_properties(patriarch_stress PROPERTIES FOLDER "tools")

add_executable(stressgen main.cpp)
target_link_libraries(stressgen PRIVATE patriarch_stress)
patriarch_set_target_options(stressgen)
patriarch_copy_asan_runtime(stressgen)
set_target_properties(stressgen PROPERTIES FOLDER "tools")

if(PATRIARCH_BUILD_TESTS)
    find_package(doctest CONFIG REQUIRED)
    add_executable(stressgen_tests "${PROJECT_SOURCE_DIR}/tests/main.cpp" dataset_test.cpp economy_test.cpp)
    target_link_libraries(stressgen_tests PRIVATE patriarch_stress doctest::doctest)
    target_compile_definitions(stressgen_tests PRIVATE DOCTEST_CONFIG_USE_STD_HEADERS)
    patriarch_set_target_options(stressgen_tests)
    patriarch_copy_asan_runtime(stressgen_tests)
    set_target_properties(stressgen_tests PROPERTIES FOLDER "tests")
    find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)
    add_test(NAME stressgen_unit COMMAND stressgen_tests)
    add_test(NAME stressgen_cli
        COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/smoke_test.py"
            "$<TARGET_FILE:stressgen>" "$<TARGET_FILE:simcli>")
    set_tests_properties(stressgen_unit stressgen_cli PROPERTIES LABELS stressgen TIMEOUT 60)
    if(PATRIARCH_SANITIZE_ADDRESS)
        # The unit suite includes fifty simulated years on 400 provinces under instrumentation.
        set_tests_properties(stressgen_unit PROPERTIES TIMEOUT 180)
    endif()
endif()
