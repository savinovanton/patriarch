add_library(patriarch_core STATIC
    include/pat/core/build_info.hpp
    build_info.cpp)
add_library(patriarch::core ALIAS patriarch_core)

target_include_directories(patriarch_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_features(patriarch_core PUBLIC cxx_std_20)
target_compile_definitions(patriarch_core PRIVATE
    PATRIARCH_VERSION_STRING="${PROJECT_VERSION}")

patriarch_set_target_options(patriarch_core)
set_target_properties(patriarch_core PROPERTIES FOLDER "src")
