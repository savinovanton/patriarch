find_package(SDL3 CONFIG REQUIRED)

add_executable(patriarch_spike main.cpp)
target_link_libraries(patriarch_spike PRIVATE patriarch::engine SDL3::SDL3)
patriarch_set_target_options(patriarch_spike)
patriarch_copy_asan_runtime(patriarch_spike)
set_target_properties(patriarch_spike PROPERTIES FOLDER "apps")

if(PATRIARCH_BUILD_TESTS)
    # Headless smoke run: opens a hidden window on SDL's dummy video driver, pumps 30 frames, exits 0.
    add_test(NAME spike_smoke COMMAND patriarch_spike --hidden --frames 30)
    set_tests_properties(spike_smoke PROPERTIES TIMEOUT 60 ENVIRONMENT "SDL_VIDEODRIVER=dummy")
endif()
