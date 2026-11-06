find_package(Threads REQUIRED)

add_library(patriarch_mapdata STATIC
    include/pat/mapdata/map_data.hpp
    include/pat/mapdata/map_format.hpp
    include/pat/mapdata/map_gen.hpp
    include/pat/mapdata/map_stats.hpp
    include/pat/mapdata/map_validate.hpp
    map_format.cpp
    map_gen.cpp
    map_validate.cpp)
add_library(patriarch::mapdata ALIAS patriarch_mapdata)

target_include_directories(patriarch_mapdata PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
# Map data depends on core only: it is shared by the offline tools and the game, never by sim.
# The generator (map_gen.cpp) is offline tooling and uses floating point and threads.
target_link_libraries(patriarch_mapdata PUBLIC patriarch::core PRIVATE Threads::Threads)

patriarch_set_target_options(patriarch_mapdata)
set_target_properties(patriarch_mapdata PROPERTIES FOLDER "src")
