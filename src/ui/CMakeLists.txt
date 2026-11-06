add_library(patriarch_ui STATIC
    include/pat/ui/character_snapshot.hpp
    include/pat/ui/character_window.hpp
    character_snapshot.cpp
    character_window.cpp)
add_library(patriarch::ui ALIAS patriarch_ui)

target_include_directories(patriarch_ui PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
# Game UI windows: they read snapshots (plain data) and never depend on sim (ADR-0001).
target_link_libraries(patriarch_ui PUBLIC patriarch::engine patriarch::core)

patriarch_set_target_options(patriarch_ui)
set_target_properties(patriarch_ui PROPERTIES FOLDER "src")
