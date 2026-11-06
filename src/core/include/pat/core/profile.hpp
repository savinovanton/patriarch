#pragma once

// Profiling zone macros. They compile to nothing unless PATRIARCH_ENABLE_TRACY is defined
// (CMake option PATRIARCH_ENABLE_TRACY, which also links the Tracy client).
//
//   PAT_PROFILE_SCOPE("economy.monthly");   // named zone until end of scope
//   PAT_PROFILE_FUNCTION();                 // zone named after the function
//   PAT_PROFILE_FRAME();                    // marks the end of a frame / tick
//
// Zone names must be string literals.

#if defined(PATRIARCH_ENABLE_TRACY)
#include <tracy/Tracy.hpp>
#define PAT_PROFILE_SCOPE(name) ZoneScopedN(name)
#define PAT_PROFILE_FUNCTION() ZoneScoped
#define PAT_PROFILE_FRAME() FrameMark
#else
#define PAT_PROFILE_SCOPE(name) ((void)0)
#define PAT_PROFILE_FUNCTION() ((void)0)
#define PAT_PROFILE_FRAME() ((void)0)
#endif
