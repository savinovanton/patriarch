add_library(patriarch_content STATIC
    include/pat/content/credits_content.hpp
    include/pat/content/demography_content.hpp
    include/pat/content/economy_content.hpp
    include/pat/content/load_errors.hpp
    include/pat/content/m1_content.hpp
    include/pat/content/m1_hot_reload.hpp
    include/pat/content/m1_scenario.hpp
    include/pat/content/names_content.hpp
    include/pat/content/province_bindings.hpp
    include/pat/content/traits_content.hpp
    credits_content.cpp
    demography_content.cpp
    economy_content.cpp
    load_errors.cpp
    m1_content.cpp
    m1_hot_reload.cpp
    m1_scenario.cpp
    names_content.cpp
    province_bindings.cpp
    traits_content.cpp)
add_library(patriarch::content ALIAS patriarch_content)

target_include_directories(patriarch_content PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
# Glue between loaded definitions, simulation rules and map keys. Keeping it above data/sim/mapdata
# preserves their boundaries: data and mapdata depend only on core, sim never depends on mapdata or
# the data loader. Headless tools and tests use this module without the engine.
target_link_libraries(patriarch_content PUBLIC
    patriarch::data patriarch::sim patriarch::core patriarch::mapdata patriarch::loc)

patriarch_set_target_options(patriarch_content)
set_target_properties(patriarch_content PROPERTIES FOLDER "src")
