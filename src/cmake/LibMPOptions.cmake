include_guard(GLOBAL)

option(LIBMP_BUILD_USE_EXTERNAL_FLATBUFFERS "Use a host-provided FlatBuffers core" OFF)
option(LIBMP_BUILD_USE_EXTERNAL_LZ4 "Do not compile LibMP's bundled LZ4 sources" OFF)
option(LIBMP_BUILD_EXCLUDE_CAPTURE_BUILDER
    "Exclude the LibMP capture-builder API and implementation" OFF)
option(LIBMP_BUILD_EXCLUDE_LZ4_ENCODER
    "Exclude LZ4 frame compression while retaining decompression" OFF)
option(LIBMP_BUILD_TRUSTED_INPUTS_ONLY
    "Expose only trusted-input session APIs and omit FlatBuffers graph verification" OFF)
option(LIBMP_BUILD_V1_GLOBAL_INDEX_COMPATIBILITY
    "Accept v1 global indexes containing legacy 32-bit data offsets" ON)
option(LIBMP_BUILD_AMALGAMATED
    "Compile LibMP from generated single-header/single-source files" OFF)
option(LIBMP_BUILD_ENABLE_EXCEPTIONS "Compile LibMP with C++ exceptions" OFF)
option(LIBMP_BUILD_ENABLE_RTTI "Compile LibMP with C++ RTTI" OFF)
option(LIBMP_BUILD_NO_CXX_STDLIB_TEST
    "Build the autonomous no-C++-standard-library link/run test" OFF)
option(LIBMP_BUILD_NO_LIBC_ALLOC_LINK_TEST
    "Force-link LibMP while rejecting direct libc allocator references" OFF)
set(LIBMP_BUILD_EXTERNAL_FLATBUFFERS_INCLUDE_DIR "" CACHE PATH
    "Optional include root containing flatbuffers/flatbuffers.h")

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    option(LIBMP_BUILD_TESTS "Build and register LibMP native tests" OFF)
    option(LIBMP_BUILD_AMALGAMATION_MATRIX_TESTS
        "Add the isolated stdlib/allocator/API amalgamation compiler matrix" OFF)
    if(LIBMP_BUILD_AMALGAMATION_MATRIX_TESTS AND NOT LIBMP_BUILD_TESTS)
        message(FATAL_ERROR
            "LIBMP_BUILD_AMALGAMATION_MATRIX_TESTS requires LIBMP_BUILD_TESTS=ON")
    endif()
endif()

if(NOT DEFINED LIBMP_BUILD_COMPILE_DEFINITIONS)
    find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)
    execute_process(
        COMMAND
            "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/scripts/_utils.py"
            --print-defines-cmake
        RESULT_VARIABLE libmp_user_config_result
        OUTPUT_VARIABLE LIBMP_BUILD_COMPILE_DEFINITIONS
        ERROR_VARIABLE libmp_user_config_error
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(NOT libmp_user_config_result EQUAL 0)
        message(FATAL_ERROR "Failed to resolve LibMP user config: ${libmp_user_config_error}")
    endif()
endif()

# Python/WASM express these choices as C++ definitions, while direct CMake users
# use the cache options above. Normalize both entry points so source selection and
# the exported preprocessor state cannot disagree.
function(libmp_apply_boolean_define option_name define_name)
    set(value "${${option_name}}")
    foreach(definition IN LISTS LIBMP_BUILD_COMPILE_DEFINITIONS)
        if(definition STREQUAL "${define_name}" OR definition STREQUAL "${define_name}=1")
            set(value ON)
        elseif(definition STREQUAL "${define_name}=0")
            set(value OFF)
        elseif(definition MATCHES "^${define_name}=")
            message(FATAL_ERROR "${define_name} must be bare, 0, or 1")
        endif()
    endforeach()
    set(remaining_defines ${LIBMP_BUILD_COMPILE_DEFINITIONS})
    list(FILTER remaining_defines EXCLUDE REGEX "^${define_name}(=(0|1))?$")
    set(${option_name} "${value}" PARENT_SCOPE)
    set(LIBMP_BUILD_COMPILE_DEFINITIONS "${remaining_defines}" PARENT_SCOPE)
endfunction()

libmp_apply_boolean_define(
    LIBMP_BUILD_USE_EXTERNAL_FLATBUFFERS CFG_USE_EXTERNAL_FLATBUFFERS)
libmp_apply_boolean_define(LIBMP_BUILD_USE_EXTERNAL_LZ4 CFG_USE_EXTERNAL_LZ4)
libmp_apply_boolean_define(
    LIBMP_BUILD_EXCLUDE_CAPTURE_BUILDER LIBMP_EXCLUDE_CAPTURE_BUILDER)
libmp_apply_boolean_define(
    LIBMP_BUILD_EXCLUDE_LZ4_ENCODER CFG_EXCLUDE_LZ4_ENCODER)
libmp_apply_boolean_define(
    LIBMP_BUILD_TRUSTED_INPUTS_ONLY LIBMP_TRUSTED_INPUTS_ONLY)
libmp_apply_boolean_define(
    LIBMP_BUILD_V1_GLOBAL_INDEX_COMPATIBILITY
    ENABLE_V1CONTAINER_GLOBALINDEX_U32OFFSET_COMPATIBILITY)
set(libmp_trusted_inputs_only "${LIBMP_BUILD_TRUSTED_INPUTS_ONLY}")

function(libmp_read_boolean_define output_name define_name default_value)
    set(value "${default_value}")
    foreach(definition IN LISTS LIBMP_BUILD_COMPILE_DEFINITIONS)
        if(definition STREQUAL "${define_name}" OR definition STREQUAL "${define_name}=1")
            set(value ON)
        elseif(definition STREQUAL "${define_name}=0")
            set(value OFF)
        elseif(definition MATCHES "^${define_name}=")
            message(FATAL_ERROR "${define_name} must be bare, 0, or 1")
        endif()
    endforeach()
    set(${output_name} "${value}" PARENT_SCOPE)
endfunction()

libmp_read_boolean_define(libmp_ntd_own_stdlib NTD_OWN_STDLIB OFF)
libmp_read_boolean_define(libmp_public_ntd_api LIBMP_PUBLIC_NTD_API OFF)
libmp_read_boolean_define(libmp_ntd_own_fileio NTD_OWN_FILEIO OFF)
libmp_read_boolean_define(libmp_route_dependency_allocations
    CFG_ROUTE_DEPENDENCY_ALLOCATIONS_TO_NTD ON)
libmp_read_boolean_define(libmp_ntd_embedded_alloc NTD_EMBEDDED_ALLOC OFF)
libmp_read_boolean_define(libmp_ntd_external_pages NTD_EXTERNAL_PAGES OFF)
libmp_read_boolean_define(libmp_ntd_external_malloc NTD_EXTERNAL_MALLOC OFF)

if(LIBMP_BUILD_NO_CXX_STDLIB_TEST)
    if(NOT libmp_ntd_own_stdlib OR NOT libmp_public_ntd_api OR NOT libmp_ntd_own_fileio)
        message(FATAL_ERROR
            "LIBMP_BUILD_NO_CXX_STDLIB_TEST requires NTD_OWN_STDLIB=1, "
            "LIBMP_PUBLIC_NTD_API=1, and NTD_OWN_FILEIO=1")
    endif()
    if(NOT libmp_route_dependency_allocations)
        message(FATAL_ERROR
            "LIBMP_BUILD_NO_CXX_STDLIB_TEST requires dependency allocation routing")
    endif()
endif()

if(LIBMP_BUILD_NO_LIBC_ALLOC_LINK_TEST)
    if(WIN32 OR APPLE)
        message(FATAL_ERROR
            "LIBMP_BUILD_NO_LIBC_ALLOC_LINK_TEST requires a GNU-style Unix linker")
    endif()
    if(NOT libmp_ntd_embedded_alloc OR libmp_ntd_external_pages OR libmp_ntd_external_malloc)
        message(FATAL_ERROR
            "LIBMP_BUILD_NO_LIBC_ALLOC_LINK_TEST requires the embedded allocator "
            "with its native page source")
    endif()
endif()
