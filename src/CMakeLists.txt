cmake_minimum_required(VERSION 3.15)

if(COMMAND add_thirdparty_library)
    message(FATAL_ERROR
        "LibMP's primary CMake entry point supports standalone library builds only")
endif()

project(libmp CXX)

include("${CMAKE_CURRENT_LIST_DIR}/cmake/LibMPOptions.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/cmake/LibMPAmalgamation.cmake")
libmp_resolve_sources(libmp_sources libmp_amalgamated_dir)

add_library(libmp STATIC ${libmp_sources})
set_target_properties(libmp PROPERTIES OUTPUT_NAME mp)

if(LIBMP_BUILD_AMALGAMATED)
    add_dependencies(libmp libmp_amalgamation)
    target_include_directories(libmp PUBLIC
        ${libmp_amalgamated_dir}
    )
else()
    target_include_directories(libmp PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
    )
endif()

if(LIBMP_BUILD_USE_EXTERNAL_FLATBUFFERS)
    target_compile_definitions(libmp PUBLIC CFG_USE_EXTERNAL_FLATBUFFERS=1)
    if(TARGET flatbuffers::flatbuffers)
        target_link_libraries(libmp PUBLIC flatbuffers::flatbuffers)
    elseif(TARGET flatbuffers)
        target_link_libraries(libmp PUBLIC flatbuffers)
    elseif(LIBMP_BUILD_EXTERNAL_FLATBUFFERS_INCLUDE_DIR)
        target_include_directories(libmp SYSTEM PUBLIC
            ${LIBMP_BUILD_EXTERNAL_FLATBUFFERS_INCLUDE_DIR}
        )
    endif()
else()
    if(NOT LIBMP_BUILD_AMALGAMATED)
        target_include_directories(libmp SYSTEM PUBLIC
            ${CMAKE_CURRENT_SOURCE_DIR}/dependencies
        )
    endif()
endif()

if(LIBMP_BUILD_USE_EXTERNAL_LZ4)
    target_compile_definitions(libmp PRIVATE CFG_USE_EXTERNAL_LZ4=1)
endif()
if(LIBMP_BUILD_EXCLUDE_CAPTURE_BUILDER)
    target_compile_definitions(libmp PUBLIC LIBMP_EXCLUDE_CAPTURE_BUILDER=1)
endif()
if(LIBMP_BUILD_EXCLUDE_LZ4_ENCODER)
    target_compile_definitions(libmp PUBLIC CFG_EXCLUDE_LZ4_ENCODER=1)
endif()
if(LIBMP_BUILD_TRUSTED_INPUTS_ONLY)
    target_compile_definitions(libmp PUBLIC LIBMP_TRUSTED_INPUTS_ONLY=1)
endif()
if(LIBMP_BUILD_V1_GLOBAL_INDEX_COMPATIBILITY)
    target_compile_definitions(libmp PUBLIC
        ENABLE_V1CONTAINER_GLOBALINDEX_U32OFFSET_COMPATIBILITY=1)
else()
    target_compile_definitions(libmp PUBLIC
        ENABLE_V1CONTAINER_GLOBALINDEX_U32OFFSET_COMPATIBILITY=0)
endif()

target_compile_features(libmp PUBLIC cxx_std_17)

if(LIBMP_BUILD_COMPILE_DEFINITIONS)
    target_compile_definitions(libmp PUBLIC
        ${LIBMP_BUILD_COMPILE_DEFINITIONS}
    )
endif()

include("${CMAKE_CURRENT_LIST_DIR}/cmake/LibMPCompiler.cmake")
libmp_enable_library_warnings(libmp)
libmp_configure_cpp_runtime_features(libmp)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    include("${CMAKE_CURRENT_LIST_DIR}/cmake/LibMPStandalone.cmake")
endif()
