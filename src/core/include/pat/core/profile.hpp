#pragma once

// Profiling zone macros. They compile to nothing unless PATRIARCH_ENABLE_TRACY is defined
// (CMake option PATRIARCH_ENABLE_TRACY, which also links the Tracy client).
//
//   PAT_PROFILE_SCOPE("economy.monthly");   // named zone until end of scope
//   PAT_PROFILE_FUNCTION();                 // zone named after the function
//   PAT_PROFILE_FRAME();                    // end of a render/UI frame (Tracy's main frame set)
//   PAT_PROFILE_FRAME_NAMED("sim.tick");    // end of a separate frame set (one per simulation tick)
//
// Zone and frame names must be string literals. How to build and capture: docs/dev/profiling.md.

#if defined(PATRIARCH_ENABLE_TRACY)
#include <tracy/Tracy.hpp>
#define PAT_PROFILE_SCOPE(name) ZoneScopedN(name)
#define PAT_PROFILE_FUNCTION() ZoneScoped
#define PAT_PROFILE_FRAME() FrameMark
#define PAT_PROFILE_FRAME_NAMED(name) FrameMarkNamed(name)
#else
#define PAT_PROFILE_SCOPE(name) ((void)0)
#define PAT_PROFILE_FUNCTION() ((void)0)
#define PAT_PROFILE_FRAME() ((void)0)
#define PAT_PROFILE_FRAME_NAMED(name) ((void)0)
#endif
