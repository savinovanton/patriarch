add_subdirectory(core)
add_subdirectory(sim)

if(PATRIARCH_BUILD_ENGINE)
    add_subdirectory(engine)
endif()
