add_library(patriarch_data STATIC
    include/pat/data/ai_schemas.hpp
    ai_schemas.cpp
    include/pat/data/content_loader.hpp
    include/pat/data/content_registry.hpp
    include/pat/data/content_schemas.hpp
    include/pat/data/content_source.hpp
    include/pat/data/court_schemas.hpp
    include/pat/data/crisis_schemas.hpp
    include/pat/data/credits_schemas.hpp
    include/pat/data/demography_schemas.hpp
    include/pat/data/economy_schemas.hpp
    include/pat/data/event_schemas.hpp
    include/pat/data/example_schemas.hpp
    include/pat/data/m1_schemas.hpp
    include/pat/data/marriage_schemas.hpp
    include/pat/data/names_schemas.hpp
    include/pat/data/pd_parser.hpp
    include/pat/data/pd_tree.hpp
    include/pat/data/schema.hpp
    include/pat/data/schema_doc.hpp
    content_loader.cpp
    content_registry.cpp
    content_schemas.cpp
    content_source.cpp
    court_schemas.cpp
    crisis_schemas.cpp
    credits_schemas.cpp
    demography_schemas.cpp
    economy_schemas.cpp
    event_schemas.cpp
    example_schemas.cpp
    m1_schemas.cpp
    marriage_schemas.cpp
    names_schemas.cpp
    pd_internal.hpp
    pd_lexer.cpp
    pd_parser.cpp
    pd_tree.cpp
    schema.cpp
    schema_doc.cpp)
add_library(patriarch::data ALIAS patriarch_data)

target_include_directories(patriarch_data PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
# Data-file parsing depends on core only; it is shared by the game, the tools and (later) the rule compiler.
# No floating point: decimals are kept as an exact mantissa and scale (determinism, NFR-DET-1).
target_link_libraries(patriarch_data PUBLIC patriarch::core)

patriarch_set_target_options(patriarch_data)
set_target_properties(patriarch_data PROPERTIES FOLDER "src")
