find_package(SDL3 CONFIG REQUIRED)

add_library(patriarch_engine STATIC
    include/pat/engine/dpi.hpp
    include/pat/engine/events.hpp
    include/pat/engine/frame_clock.hpp
    include/pat/engine/window.hpp
    frame_clock.cpp
    window.cpp)
add_library(patriarch::engine ALIAS patriarch_engine)

target_include_directories(patriarch_engine PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
# engine may depend on core only; it must never depend on sim (and sim never on engine) - ADR-0001.
# SDL3 is private: no SDL types appear in engine headers.
target_link_libraries(patriarch_engine PUBLIC patriarch::core PRIVATE SDL3::SDL3)

patriarch_set_target_options(patriarch_engine)
set_target_properties(patriarch_engine PROPERTIES FOLDER "src")
