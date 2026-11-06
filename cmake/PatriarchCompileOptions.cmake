# Common compile options for all first-party targets.
# Usage: patriarch_set_target_options(<target>)

function(patriarch_set_target_options target)
    if(MSVC)
        target_compile_options(${target} PRIVATE
            /W4
            /permissive-
            /utf-8
            /Zc:__cplusplus
            /Zc:preprocessor
            /MP
            /external:anglebrackets
            /external:W0)
        if(PATRIARCH_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
        if(PATRIARCH_SANITIZE_ADDRESS)
            # ASan is incompatible with /RTC1 and incremental linking.
            target_compile_options(${target} PRIVATE /fsanitize=address /Zi)
            target_link_options(${target} PRIVATE /INCREMENTAL:NO)
        endif()
        target_compile_definitions(${target} PRIVATE
            NOMINMAX
            WIN32_LEAN_AND_MEAN
            _CRT_SECURE_NO_WARNINGS)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
        if(PATRIARCH_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
        if(PATRIARCH_SANITIZE_ADDRESS)
            target_compile_options(${target} PRIVATE -fsanitize=address -fno-omit-frame-pointer)
            target_link_options(${target} PRIVATE -fsanitize=address)
        endif()
    endif()
endfunction()

# MSVC's ASan runtime DLLs live next to cl.exe and are not on PATH. Every executable built with
# ASan needs them beside it: call this for each executable target (tests, tools).
function(patriarch_copy_asan_runtime target)
    if(MSVC AND PATRIARCH_SANITIZE_ADDRESS)
        get_filename_component(_patriarch_cl_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_patriarch_cl_dir}/clang_rt.asan_dynamic-x86_64.dll"
                "${_patriarch_cl_dir}/clang_rt.asan_dbg_dynamic-x86_64.dll"
                "$<TARGET_FILE_DIR:${target}>"
            VERBATIM)
    endif()
endfunction()

if(MSVC AND PATRIARCH_SANITIZE_ADDRESS)
    # /RTC1 (default in Debug) conflicts with /fsanitize=address.
    string(REGEX REPLACE "/RTC1" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
    string(REGEX REPLACE "/RTC1" "" CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")
endif()
