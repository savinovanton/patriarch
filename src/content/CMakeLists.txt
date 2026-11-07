add_library(patriarch_content STATIC
    include/pat/content/economy_content.hpp
    economy_content.cpp)
add_library(patriarch::content ALIAS patriarch_content)

target_include_directories(patriarch_content PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
# Glue between loaded .pd content (patriarch::data) and the simulation's rule structures
# (patriarch::sim). It is a separate module because data must depend on core only and sim must not
# depend on data; it is not part of the engine, so headless tools and tests can use it.
target_link_libraries(patriarch_content PUBLIC patriarch::data patriarch::sim patriarch::core)

patriarch_set_target_options(patriarch_content)
set_target_properties(patriarch_content PROPERTIES FOLDER "src")
