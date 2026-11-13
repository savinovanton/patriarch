add_library(patriarch_game STATIC
    include/pat/game/credits_view.hpp
    include/pat/game/economy_view.hpp
    include/pat/game/hud_view.hpp
    include/pat/game/map_overview.hpp
    include/pat/game/province_view.hpp
    include/pat/game/tooltip_view.hpp
    include/pat/game/world_view.hpp
    credits_view.cpp
    economy_number.cpp
    hud_view.cpp
    map_overview.cpp
    province_economy_view.cpp
    province_view.cpp
    treasury_view.cpp
    tooltip_view.cpp
    world_view.cpp)
add_library(patriarch::game ALIAS patriarch_game)

target_include_directories(patriarch_game PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
# The bridge from simulation snapshots to UI data: reads immutable worlds, never changes them.
target_link_libraries(patriarch_game PUBLIC
    patriarch::sim patriarch::ui patriarch::content patriarch::mapdata patriarch::core)

patriarch_set_target_options(patriarch_game)
set_target_properties(patriarch_game PROPERTIES FOLDER "src")
