find_package(SDL3 CONFIG REQUIRED)
find_package(RmlUi CONFIG REQUIRED)

add_library(patriarch_engine STATIC
    include/pat/engine/camera.hpp
    include/pat/engine/dpi.hpp
    include/pat/engine/events.hpp
    include/pat/engine/frame_clock.hpp
    include/pat/engine/math.hpp
    include/pat/engine/renderer.hpp
    include/pat/engine/terrain.hpp
    include/pat/engine/ui.hpp
    include/pat/engine/window.hpp
    shaders/quad.hlsl
    shaders/terrain.hlsl
    shaders/ui.hlsl
    camera.cpp
    frame_clock.cpp
    terrain.cpp
    ui.cpp
    window.cpp)
add_library(patriarch::engine ALIAS patriarch_engine)

target_include_directories(patriarch_engine PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
# engine may depend on core and mapdata only; it must never depend on sim (and sim never on engine) - ADR-0001.
# SDL3 and the graphics API are private: no SDL/D3D types appear in engine headers.
target_link_libraries(patriarch_engine PUBLIC patriarch::core patriarch::mapdata RmlUi::RmlUi PRIVATE SDL3::SDL3)

# Directory with the shader sources, for hot reload in development builds (RendererDesc::shader_dir).
set(PATRIARCH_ENGINE_SHADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/shaders" CACHE INTERNAL "")

if(WIN32)
    # HLSL sources are also embedded into the library so the game runs without loose shader files.
    set(PATRIARCH_EMBEDDED_SHADERS "")
    foreach(shader quad terrain ui)
        file(READ "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${shader}.hlsl" PATRIARCH_HLSL_TEXT)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${shader}.hlsl")
        string(SUBSTRING "${shader}" 0 1 _first)
        string(TOUPPER "${_first}" _first)
        string(SUBSTRING "${shader}" 1 -1 _rest)
        # Raw string literals have a length limit in MSVC; split large sources into chunks if this ever fails.
        string(APPEND PATRIARCH_EMBEDDED_SHADERS
            "extern const char* const k${_first}${_rest}Hlsl;
const char* const k${_first}${_rest}Hlsl = R\"HLSLSRC(${PATRIARCH_HLSL_TEXT})HLSLSRC\";
")
    endforeach()
    file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/embedded_shaders.cpp"
        CONTENT "namespace pat::engine::embedded {
@PATRIARCH_EMBEDDED_SHADERS@}  // namespace pat::engine::embedded
"
        @ONLY)
    target_sources(patriarch_engine PRIVATE
        d3d11/d3d11_common.cpp
        d3d11/d3d11_map_view.cpp
        d3d11/d3d11_renderer.cpp
        d3d11/d3d11_ui_renderer.cpp
        "${CMAKE_CURRENT_BINARY_DIR}/embedded_shaders.cpp")
    target_link_libraries(patriarch_engine PRIVATE d3d11 dxgi d3dcompiler)
else()
    target_sources(patriarch_engine PRIVATE null_renderer.cpp)
endif()

patriarch_set_target_options(patriarch_engine)
set_target_properties(patriarch_engine PROPERTIES FOLDER "src")
set_source_files_properties(shaders/quad.hlsl shaders/terrain.hlsl shaders/ui.hlsl PROPERTIES HEADER_FILE_ONLY TRUE)
