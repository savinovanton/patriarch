cmake_minimum_required(VERSION 3.25)

project(patriarch
    VERSION 0.0.1
    DESCRIPTION "Patriarch - dynasty strategy set in early 18th century Russia"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

option(PATRIARCH_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(PATRIARCH_SANITIZE_ADDRESS "Build with AddressSanitizer (MSVC)" OFF)
option(PATRIARCH_BUILD_TESTS "Build unit tests" ON)

include(cmake/PatriarchCompileOptions.cmake)

add_subdirectory(src)

if(PATRIARCH_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
