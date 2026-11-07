find_package(doctest CONFIG REQUIRED)

add_executable(patriarch_tests
    main.cpp
    arena_test.cpp
    assert_test.cpp
    calendar_test.cpp
    command_log_test.cpp
    command_test.cpp
    content_loader_test.cpp
    core_smoke.cpp
    date_test.cpp
    demography_test.cpp
    economy_content_test.cpp
    economy_rules_test.cpp
    fixed_test.cpp
    golden_test.cpp
    ids_test.cpp
    log_test.cpp
    mapdata_test.cpp
    pd_parser_test.cpp
    profile_test.cpp
    replay_test.cpp
    result_test.cpp
    rng_test.cpp
    runner_test.cpp
    save_test.cpp
    schema_doc_test.cpp
    schema_test.cpp
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
    patriarch::mapdata
    patriarch::data
    patriarch::content
    doctest::doctest)

if(PATRIARCH_BUILD_ENGINE)
    target_sources(patriarch_tests PRIVATE
        camera_terrain_test.cpp
        character_window_test.cpp
        frame_clock_test.cpp
        game_bridge_test.cpp
        labels_test.cpp
        renderer_test.cpp
        time_bar_test.cpp
        hud_test.cpp
        sky_test.cpp
        ui_images_test.cpp
        tooltip_manager_test.cpp
        tooltip_state_test.cpp
        ui_test.cpp
        window_test.cpp)
    target_link_libraries(patriarch_tests PRIVATE patriarch::engine patriarch::ui patriarch::game)
    target_compile_definitions(patriarch_tests PRIVATE
        PATRIARCH_ENGINE_SHADER_DIR="${PATRIARCH_ENGINE_SHADER_DIR}"
        PATRIARCH_SPIKE_UI_DIR="${CMAKE_SOURCE_DIR}/apps/spike/ui"
        PATRIARCH_ASSETS_DIR="${CMAKE_SOURCE_DIR}/assets")
endif()

# Use the real <ostream>/<string> so doctest can stringify std::string_view etc.
target_compile_definitions(patriarch_tests PRIVATE DOCTEST_CONFIG_USE_STD_HEADERS
    PATRIARCH_TEST_DATA_DIR="${CMAKE_SOURCE_DIR}/tests/data"
    PATRIARCH_DATA_DIR="${CMAKE_SOURCE_DIR}/data"
    PATRIARCH_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

patriarch_set_target_options(patriarch_tests)
set_target_properties(patriarch_tests PROPERTIES FOLDER "tests")

patriarch_copy_asan_runtime(patriarch_tests)

include(CTest)
include(${doctest_DIR}/doctest.cmake)
doctest_discover_tests(patriarch_tests)

target_sources(patriarch_tests PRIVATE telemetry_test.cpp)
