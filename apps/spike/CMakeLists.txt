find_package(SDL3 CONFIG REQUIRED)

add_executable(patriarch_spike main.cpp)
target_link_libraries(patriarch_spike PRIVATE patriarch::engine patriarch::ui patriarch::game
    patriarch::content SDL3::SDL3)
# Development builds load shaders from the source tree so they can be hot-reloaded (F5 or on save).
target_compile_definitions(patriarch_spike PRIVATE
    PATRIARCH_M1_DATA_DIR="${PROJECT_SOURCE_DIR}/data"
    PATRIARCH_SPIKE_SHADER_DIR="${PATRIARCH_ENGINE_SHADER_DIR}"
    PATRIARCH_SPIKE_UI_DIR="${CMAKE_CURRENT_SOURCE_DIR}/ui"
    PATRIARCH_SPIKE_OLIVE_DIR="${CMAKE_SOURCE_DIR}/assets/ui/olive"
    PATRIARCH_SPIKE_ICONS_DIR="${CMAKE_SOURCE_DIR}/assets/ui/patriarch/icons"
    PATRIARCH_SPIKE_FONT_DIR="${CMAKE_SOURCE_DIR}/assets/fonts")
patriarch_set_target_options(patriarch_spike)
patriarch_copy_asan_runtime(patriarch_spike)
set_target_properties(patriarch_spike PROPERTIES FOLDER "apps")

if(PATRIARCH_BUILD_TESTS)
    # Headless smoke run: opens a hidden window on SDL's dummy video driver, pumps 30 frames, exits 0.
    add_test(NAME spike_smoke COMMAND patriarch_spike --hidden --frames 30)
    set_tests_properties(spike_smoke PROPERTIES TIMEOUT 60 ENVIRONMENT "SDL_VIDEODRIVER=dummy")
    add_test(NAME spike_m1_smoke COMMAND patriarch_spike --scenario m1 --hidden --frames 4)
    set_tests_properties(spike_m1_smoke PROPERTIES TIMEOUT 60 ENVIRONMENT "SDL_VIDEODRIVER=dummy")
endif()
