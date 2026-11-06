add_library(patriarch_core STATIC
    include/pat/core/assert.hpp
    include/pat/core/build_info.hpp
    include/pat/core/log.hpp
    include/pat/core/detail/sigmoid_table.hpp
    include/pat/core/detail/wide_math.hpp
    include/pat/core/arena.hpp
    include/pat/core/bytes.hpp
    include/pat/core/fixed.hpp
    include/pat/core/hash_stream.hpp
    include/pat/core/ids.hpp
    include/pat/core/profile.hpp
    include/pat/core/result.hpp
    include/pat/core/rng.hpp
    include/pat/core/slot_map.hpp
    include/pat/core/sparse_set.hpp
    include/pat/core/spsc_queue.hpp
    include/pat/core/static_vec.hpp
    include/pat/core/string_interner.hpp
    include/pat/core/compress.hpp
    arena.cpp
    assert.cpp
    build_info.cpp
    compress.cpp
    log.cpp
    rng.cpp
    string_interner.cpp)
add_library(patriarch::core ALIAS patriarch_core)

target_include_directories(patriarch_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_features(patriarch_core PUBLIC cxx_std_20)
target_compile_definitions(patriarch_core PRIVATE
    PATRIARCH_VERSION_STRING="${PROJECT_VERSION}")

find_package(zstd CONFIG REQUIRED)
target_link_libraries(patriarch_core PRIVATE zstd::libzstd)

option(PATRIARCH_ENABLE_TRACY "Enable Tracy profiling zones (requires the Tracy client)" OFF)
if(PATRIARCH_ENABLE_TRACY)
    find_package(Tracy CONFIG REQUIRED)
    target_link_libraries(patriarch_core PUBLIC Tracy::TracyClient)
    target_compile_definitions(patriarch_core PUBLIC PATRIARCH_ENABLE_TRACY TRACY_ENABLE)
endif()

patriarch_set_target_options(patriarch_core)
set_target_properties(patriarch_core PROPERTIES FOLDER "src")
