include_guard(GLOBAL)

# Resolve LibMP's canonical translation units, or define the generation rule and
# return the generated pair when LIBMP_BUILD_AMALGAMATED is enabled.
function(libmp_resolve_sources output_sources output_amalgamated_dir)
    # Shared with scripts/amalgamate.py and the WASM build.
    set(source_dirs_file "${CMAKE_CURRENT_SOURCE_DIR}/source-dirs.txt")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${source_dirs_file}")
    file(STRINGS "${source_dirs_file}" source_dirs)

    set(implementation_sources)
    set(amalgamation_inputs)
    foreach(source_dir IN LISTS source_dirs)
        file(GLOB directory_sources CONFIGURE_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/${source_dir}/*.cpp"
        )
        file(GLOB_RECURSE directory_headers CONFIGURE_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/${source_dir}/*.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/${source_dir}/*.inc"
        )
        list(APPEND implementation_sources ${directory_sources})
        list(APPEND amalgamation_inputs ${directory_headers})
    endforeach()

    if(NOT LIBMP_BUILD_AMALGAMATED)
        set(${output_sources} ${implementation_sources} PARENT_SCOPE)
        set(${output_amalgamated_dir} "" PARENT_SCOPE)
        return()
    endif()

    if(NOT Python3_Interpreter_FOUND)
        find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)
    endif()
    file(GLOB_RECURSE dependency_inputs CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/dependencies/*.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/dependencies/*.h"
    )
    list(APPEND amalgamation_inputs ${dependency_inputs})
    list(REMOVE_DUPLICATES amalgamation_inputs)

    set(amalgamation_args)
    if(LIBMP_BUILD_USE_EXTERNAL_FLATBUFFERS)
        list(APPEND amalgamation_args --external-dependency flatbuffers)
    endif()
    if(LIBMP_BUILD_USE_EXTERNAL_LZ4)
        list(APPEND amalgamation_args --external-dependency lz4)
    endif()
    if(LIBMP_BUILD_EXCLUDE_CAPTURE_BUILDER)
        list(APPEND amalgamation_args --define LIBMP_EXCLUDE_CAPTURE_BUILDER=1)
    endif()
    if(LIBMP_BUILD_EXCLUDE_LZ4_ENCODER)
        list(APPEND amalgamation_args --define CFG_EXCLUDE_LZ4_ENCODER=1)
    endif()
    if(LIBMP_BUILD_TRUSTED_INPUTS_ONLY)
        list(APPEND amalgamation_args --define LIBMP_TRUSTED_INPUTS_ONLY=1)
    endif()
    # Specialize annotated implementation regions to the same boolean defines
    # that will be used to compile the generated pair.
    foreach(define IN LISTS LIBMP_BUILD_COMPILE_DEFINITIONS)
        list(APPEND amalgamation_args --define "${define}")
    endforeach()

    set(amalgamated_dir "${CMAKE_CURRENT_BINARY_DIR}/amalgamated")
    set(amalgamated_header "${amalgamated_dir}/libmp.h")
    set(amalgamated_source "${amalgamated_dir}/libmp.cpp")
    set(amalgamated_stamp "${amalgamated_dir}/generated.stamp")
    set(amalgamated_mode_file "${amalgamated_dir}/mode.txt")
    string(JOIN " " amalgamated_mode ${amalgamation_args})
    string(APPEND amalgamated_mode "\n")
    file(MAKE_DIRECTORY "${amalgamated_dir}")
    if(EXISTS "${amalgamated_mode_file}")
        file(READ "${amalgamated_mode_file}" previous_amalgamated_mode)
    endif()
    if(NOT previous_amalgamated_mode STREQUAL amalgamated_mode)
        file(WRITE "${amalgamated_mode_file}" "${amalgamated_mode}")
    endif()

    add_custom_command(
        OUTPUT "${amalgamated_stamp}"
        BYPRODUCTS "${amalgamated_header}" "${amalgamated_source}"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/amalgamate.py"
                --output-dir "${amalgamated_dir}"
                ${amalgamation_args}
        COMMAND "${CMAKE_COMMAND}" -E touch "${amalgamated_stamp}"
        DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/scripts/amalgamate.py"
            "${source_dirs_file}"
            "${amalgamated_mode_file}"
            ${implementation_sources}
            ${amalgamation_inputs}
        COMMENT "Generating LibMP amalgamation"
        VERBATIM
    )
    add_custom_target(libmp_amalgamation DEPENDS "${amalgamated_stamp}")
    set_source_files_properties(
        "${amalgamated_source}" "${amalgamated_header}"
        PROPERTIES GENERATED TRUE
    )

    set(${output_sources} "${amalgamated_source}" "${amalgamated_header}" PARENT_SCOPE)
    set(${output_amalgamated_dir} "${amalgamated_dir}" PARENT_SCOPE)
endfunction()
