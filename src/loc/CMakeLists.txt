add_library(patriarch_loc STATIC
    include/pat/loc/localizer.hpp
    include/pat/loc/message.hpp
    include/pat/loc/plural.hpp
    localizer.cpp
    message.cpp
    plural.cpp)
add_library(patriarch::loc ALIAS patriarch_loc)

target_include_directories(patriarch_loc PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
# Localisation (E4-T06) reads string tables with the .pd parser and the ContentSource interface of
# patriarch::data; data itself stays free of it (it depends on core only). Integer arithmetic only.
target_link_libraries(patriarch_loc PUBLIC patriarch::data patriarch::core)

patriarch_set_target_options(patriarch_loc)
set_target_properties(patriarch_loc PROPERTIES FOLDER "src")
