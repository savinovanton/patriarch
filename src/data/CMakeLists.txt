add_library(patriarch_data STATIC
    include/pat/data/example_schemas.hpp
    include/pat/data/pd_parser.hpp
    include/pat/data/pd_tree.hpp
    include/pat/data/schema.hpp
    example_schemas.cpp
    pd_internal.hpp
    pd_lexer.cpp
    pd_parser.cpp
    pd_tree.cpp
    schema.cpp)
add_library(patriarch::data ALIAS patriarch_data)

target_include_directories(patriarch_data PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
# Data-file parsing depends on core only; it is shared by the game, the tools and (later) the rule compiler.
# No floating point: decimals are kept as an exact mantissa and scale (determinism, NFR-DET-1).
target_link_libraries(patriarch_data PUBLIC patriarch::core)

patriarch_set_target_options(patriarch_data)
set_target_properties(patriarch_data PROPERTIES FOLDER "src")
