add_library(patriarch_game STATIC
    include/pat/game/world_view.hpp
    world_view.cpp)
add_library(patriarch::game ALIAS patriarch_game)

target_include_directories(patriarch_game PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
# The bridge from simulation snapshots to UI data: reads immutable worlds, never changes them.
target_link_libraries(patriarch_game PUBLIC patriarch::sim patriarch::ui patriarch::core)

patriarch_set_target_options(patriarch_game)
set_target_properties(patriarch_game PROPERTIES FOLDER "src")
