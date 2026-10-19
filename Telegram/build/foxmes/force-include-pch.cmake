# ccache cannot cache an MSVC compile that uses a precompiled header, and the
# sources do not include what the header gives them. This file replaces each
# precompiled header with a forced include of the same header, so that every
# compile sees the same code and ccache can cache it.
function(target_precompile_headers target)
    foreach(header IN LISTS ARGN)
        if(header MATCHES "^(PRIVATE|PUBLIC|INTERFACE)$")
            continue()
        endif()
        target_compile_options(${target} PRIVATE
            "$<$<COMPILE_LANGUAGE:CXX>:/FI${header}>")
    endforeach()
endfunction()
