add_library(patriarch_core STATIC
    include/pat/core/assert.hpp
    include/pat/core/build_info.hpp
    include/pat/core/log.hpp
    include/pat/core/profile.hpp
    include/pat/core/result.hpp
    assert.cpp
    build_info.cpp
    log.cpp)
add_library(patriarch::core ALIAS patriarch_core)

target_include_directories(patriarch_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_features(patriarch_core PUBLIC cxx_std_20)
target_compile_definitions(patriarch_core PRIVATE
    PATRIARCH_VERSION_STRING="${PROJECT_VERSION}")

option(PATRIARCH_ENABLE_TRACY "Enable Tracy profiling zones (requires the Tracy client)" OFF)
if(PATRIARCH_ENABLE_TRACY)
    find_package(Tracy CONFIG REQUIRED)
    target_link_libraries(patriarch_core PUBLIC Tracy::TracyClient)
    target_compile_definitions(patriarch_core PUBLIC PATRIARCH_ENABLE_TRACY TRACY_ENABLE)
endif()

patriarch_set_target_options(patriarch_core)
set_target_properties(patriarch_core PROPERTIES FOLDER "src")
