add_subdirectory(core)
add_subdirectory(sim)
add_subdirectory(mapdata)

if(PATRIARCH_BUILD_ENGINE)
    add_subdirectory(engine)
    add_subdirectory(ui)
    add_subdirectory(game)
endif()
