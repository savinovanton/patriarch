find_package(SDL3 CONFIG REQUIRED)

add_library(patriarch_engine STATIC
    include/pat/engine/dpi.hpp
    include/pat/engine/events.hpp
    include/pat/engine/frame_clock.hpp
    include/pat/engine/renderer.hpp
    include/pat/engine/window.hpp
    shaders/quad.hlsl
    frame_clock.cpp
    window.cpp)
add_library(patriarch::engine ALIAS patriarch_engine)

target_include_directories(patriarch_engine PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
# engine may depend on core only; it must never depend on sim (and sim never on engine) - ADR-0001.
# SDL3 and the graphics API are private: no SDL/D3D types appear in engine headers.
target_link_libraries(patriarch_engine PUBLIC patriarch::core PRIVATE SDL3::SDL3)

# Directory with the shader sources, for hot reload in development builds (RendererDesc::shader_dir).
set(PATRIARCH_ENGINE_SHADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/shaders" CACHE INTERNAL "")

if(WIN32)
    # The HLSL source is also embedded into the library so the game runs without loose shader files.
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/shaders/quad.hlsl" PATRIARCH_QUAD_HLSL)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/quad.hlsl")
    file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/embedded_shaders.cpp"
        CONTENT "namespace pat::engine::embedded {\nextern const char* const kQuadHlsl;\nconst char* const kQuadHlsl = R\"HLSLSRC(@PATRIARCH_QUAD_HLSL@)HLSLSRC\";\n}  // namespace pat::engine::embedded\n"
        @ONLY)
    target_sources(patriarch_engine PRIVATE
        d3d11/d3d11_renderer.cpp
        "${CMAKE_CURRENT_BINARY_DIR}/embedded_shaders.cpp")
    target_link_libraries(patriarch_engine PRIVATE d3d11 dxgi d3dcompiler)
else()
    target_sources(patriarch_engine PRIVATE null_renderer.cpp)
endif()

patriarch_set_target_options(patriarch_engine)
set_target_properties(patriarch_engine PROPERTIES FOLDER "src")
set_source_files_properties(shaders/quad.hlsl PROPERTIES HEADER_FILE_ONLY TRUE)
