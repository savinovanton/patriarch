add_library(patriarch_sim STATIC
    include/pat/sim/sim.hpp
    sim.cpp)
add_library(patriarch::sim ALIAS patriarch_sim)

target_include_directories(patriarch_sim PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
# sim must stay free of engine/ui/platform dependencies (ADR-0001).
target_link_libraries(patriarch_sim PUBLIC patriarch::core)

patriarch_set_target_options(patriarch_sim)
set_target_properties(patriarch_sim PROPERTIES FOLDER "src")
