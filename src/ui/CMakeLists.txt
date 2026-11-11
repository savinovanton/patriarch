add_library(patriarch_ui STATIC
    include/pat/ui/character_snapshot.hpp
    include/pat/ui/character_window.hpp
    include/pat/ui/component_gallery.hpp
    include/pat/ui/credits_window.hpp
    include/pat/ui/explanation.hpp
    include/pat/ui/focus_visibility.hpp
    include/pat/ui/hud_snapshot.hpp
    include/pat/ui/hud_window.hpp
    include/pat/ui/text.hpp
    include/pat/ui/theme.hpp
    include/pat/ui/tooltip_manager.hpp
    include/pat/ui/time_bar.hpp
    include/pat/ui/tooltip_state.hpp
    character_snapshot.cpp
    character_window.cpp
    component_gallery.cpp
    credits_window.cpp
    explanation.cpp
    focus_visibility.cpp
    hud_window.cpp
    text.cpp
    theme.cpp
    time_bar.cpp
    tooltip_manager.cpp
    tooltip_state.cpp)
add_library(patriarch::ui ALIAS patriarch_ui)

target_include_directories(patriarch_ui PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
# Game UI windows: they read snapshots (plain data) and never depend on sim (ADR-0001). Every visible
# string comes from the string tables through patriarch::loc (docs/dev/localization.md).
target_link_libraries(patriarch_ui PUBLIC patriarch::engine patriarch::loc patriarch::data patriarch::core)

patriarch_set_target_options(patriarch_ui)
set_target_properties(patriarch_ui PROPERTIES FOLDER "src")
