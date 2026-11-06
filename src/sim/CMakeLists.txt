add_library(patriarch_sim STATIC
    include/pat/sim/calendar.hpp
    include/pat/sim/command.hpp
    include/pat/sim/command_log.hpp
    include/pat/sim/date.hpp
    include/pat/sim/entity_table.hpp
    include/pat/sim/scheduled_queue.hpp
    include/pat/sim/sim.hpp
    include/pat/sim/simulation.hpp
    include/pat/sim/system.hpp
    include/pat/sim/task_executor.hpp
    include/pat/sim/work_budget.hpp
    include/pat/sim/world.hpp
    command.cpp
    command_log.cpp
    date.cpp
    sim.cpp
    simulation.cpp
    world.cpp)
add_library(patriarch::sim ALIAS patriarch_sim)

target_include_directories(patriarch_sim PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
# sim must stay free of engine/ui/platform dependencies (ADR-0001).
target_link_libraries(patriarch_sim PUBLIC patriarch::core)

patriarch_set_target_options(patriarch_sim)
set_target_properties(patriarch_sim PROPERTIES FOLDER "src")
