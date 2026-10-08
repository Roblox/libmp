include_guard(GLOBAL)

function(libmp_enable_strict_warnings target)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(${target} PRIVATE
            -Wall
            -Wextra
            -Werror
            -Wundef
        )
    elseif(MSVC)
        target_compile_options(${target} PRIVATE
            # Keep warnings in angle-bracket system headers out of /WX without
            # marking LibMP's own public headers as external.
            /external:anglebrackets
            /external:W0
            /W4
            /WX
            /we4668
        )
    endif()
endfunction()

function(libmp_configure_cpp_runtime_features target)
    if(MSVC)
        if(LIBMP_BUILD_ENABLE_EXCEPTIONS)
            target_compile_options(${target} PRIVATE /EHsc)
        else()
            # /EHs-c- stops the compiler from emitting exception machinery for our
            # code, but the MSVC STL headers still contain try/catch blocks that
            # trigger C4530 (promoted to an error by /WX). _HAS_EXCEPTIONS=0 is the
            # MSVC-side equivalent of -fno-exceptions: it makes the standard library
            # headers drop those exception paths entirely.
            target_compile_options(${target} PRIVATE /EHs-c-)
            target_compile_definitions(${target} PRIVATE _HAS_EXCEPTIONS=0)
        endif()
        if(LIBMP_BUILD_ENABLE_RTTI)
            target_compile_options(${target} PRIVATE /GR)
        else()
            target_compile_options(${target} PRIVATE /GR-)
        endif()
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        if(LIBMP_BUILD_ENABLE_EXCEPTIONS)
            target_compile_options(${target} PRIVATE -fexceptions)
        else()
            target_compile_options(${target} PRIVATE -fno-exceptions)
        endif()
        if(LIBMP_BUILD_ENABLE_RTTI)
            target_compile_options(${target} PRIVATE -frtti)
        else()
            target_compile_options(${target} PRIVATE -fno-rtti)
        endif()
    endif()
endfunction()

function(libmp_enable_library_warnings target)
    libmp_enable_strict_warnings(${target})

    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(${target} PRIVATE
            -Wcast-qual
            -Wconversion
            -Wdouble-promotion
            -Wextra-semi
            -Wimplicit-fallthrough
            -Wnull-dereference
            -Wshadow
            -Wshift-overflow
            -Wsign-conversion
        )
    endif()

    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(${target} PRIVATE
            -Wbitwise-op-parentheses
            -Wcovered-switch-default
            -Wlogical-op-parentheses
            -Wunreachable-code
        )
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options(${target} PRIVATE
            -Wlogical-op
            -Wparentheses
        )
    elseif(MSVC)
        target_compile_options(${target} PRIVATE
            /we4018
            /we4146
            /we4244
            /we4245
            /we4267
            /we4293
            /we4305
            /we4365
            /we4389
            /we4456
            /we4457
            /we4458
            /we4459
            /we4554
            /we4702
        )
    endif()
endfunction()
