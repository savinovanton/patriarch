find_package(doctest CONFIG REQUIRED)

add_executable(patriarch_tests
    main.cpp
    archive_test.cpp
    arena_test.cpp
    assert_test.cpp
    calendar_test.cpp
    character_schema_test.cpp
    chronicle_test.cpp
    command_log_test.cpp
    command_test.cpp
    content_loader_test.cpp
    core_smoke.cpp
    date_test.cpp
    demography_content_test.cpp
    demography_test.cpp
    dynasty_standing_test.cpp
    economy_administrator_test.cpp
    economy_policy_test.cpp
    economy_recruits_test.cpp
    economy_invariants_test.cpp
    economy_mood_test.cpp
    economy_construction_test.cpp
    economy_content_test.cpp
    economy_population_test.cpp
    economy_production_test.cpp
    economy_rules_test.cpp
    economy_save_test.cpp
    economy_settlement_test.cpp
    economy_taxes_test.cpp
    economy_unrest_test.cpp
    economy_world_test.cpp
    fixed_test.cpp
    golden_test.cpp
    ids_test.cpp
    credits_content_test.cpp
    kinship_test.cpp
    localization_test.cpp
    log_test.cpp
    mapdata_test.cpp
    m1_content_test.cpp
    m1_definitions_test.cpp
    m1_hot_reload_test.cpp
    mortality_test.cpp
    names_content_test.cpp
    names_test.cpp
    opinion_test.cpp
    province_bindings_test.cpp
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
    succession_test.cpp
    slot_map_test.cpp
    sparse_set_test.cpp
    spsc_queue_test.cpp
    static_vec_test.cpp
    ui_data_test.cpp
    string_interner_test.cpp
    traits_content_test.cpp
    traits_test.cpp
    world_model_test.cpp
    world_test.cpp)

target_link_libraries(patriarch_tests PRIVATE
    patriarch::core
    patriarch::sim
    patriarch::mapdata
    patriarch::data
    patriarch::loc
    patriarch::content
    doctest::doctest)

if(PATRIARCH_BUILD_TOOLS)
    # contentlint's library part (E4-T08) is tested through its API with in-memory corpora.
    target_sources(patriarch_tests PRIVATE contentlint_test.cpp)
    target_link_libraries(patriarch_tests PRIVATE patriarch::contentlint)
endif()

if(PATRIARCH_BUILD_ENGINE)
    target_sources(patriarch_tests PRIVATE
        camera_terrain_test.cpp
        character_window_test.cpp
        credits_window_test.cpp
        frame_clock_test.cpp
        game_bridge_test.cpp
        labels_test.cpp
        renderer_test.cpp
        time_bar_test.cpp
        ui_components_test.cpp
        hud_test.cpp
        sky_test.cpp
        ui_images_test.cpp
        ui_theme_test.cpp
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
