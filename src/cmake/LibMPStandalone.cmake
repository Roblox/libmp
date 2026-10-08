include_guard(GLOBAL)

function(libmp_add_executable target)
    add_executable(${target} ${ARGN})
    target_link_libraries(${target} PRIVATE libmp)
    target_compile_features(${target} PRIVATE cxx_std_17)
    libmp_enable_strict_warnings(${target})
endfunction()

function(libmp_use_public_header target)
    if(LIBMP_BUILD_AMALGAMATED)
        target_compile_definitions(${target} PRIVATE LIBMP_AMALGAMATED=1)
        add_dependencies(${target} libmp_amalgamation)
    endif()
endfunction()

function(libmp_add_autonomy_link_executable target source)
    add_executable(${target} ${source})
    target_compile_features(${target} PRIVATE cxx_std_17)
    libmp_enable_strict_warnings(${target})
    libmp_configure_cpp_runtime_features(${target})
    libmp_use_public_header(${target})
endfunction()

function(libmp_force_link_complete_archive target)
    if(MSVC)
        target_link_libraries(${target} PRIVATE libmp)
        target_link_options(${target} PRIVATE
            "/WHOLEARCHIVE:$<TARGET_FILE:libmp>")
    elseif(APPLE)
        target_link_libraries(${target} PRIVATE libmp)
        target_link_options(${target} PRIVATE
            "LINKER:-force_load,$<TARGET_FILE:libmp>")
    else()
        target_link_libraries(${target} PRIVATE
            "-Wl,--whole-archive" libmp "-Wl,--no-whole-archive")
    endif()
endfunction()

if(LIBMP_BUILD_NO_CXX_STDLIB_TEST)
    if(WIN32 AND NOT MSVC)
        # The Windows gate uses /NODEFAULTLIB and therefore requires a
        # toolchain targeting the MSVC ABI.
        message(FATAL_ERROR
            "LIBMP_BUILD_NO_CXX_STDLIB_TEST on Windows requires an MSVC-compatible toolchain")
    endif()

    libmp_add_autonomy_link_executable(libmp_no_cxx_stdlib_smoke
        tests/cpp/no_cxx_stdlib_smoke.cpp
    )
    libmp_force_link_complete_archive(libmp_no_cxx_stdlib_smoke)

    if(MSVC)
        # Keep the C runtime and compiler ABI support, but reject every
        # release/debug and static/dynamic MSVC Standard Library variant.
        target_link_options(libmp_no_cxx_stdlib_smoke PRIVATE
            /NODEFAULTLIB:msvcprt.lib
            /NODEFAULTLIB:msvcprtd.lib
            /NODEFAULTLIB:libcpmt.lib
            /NODEFAULTLIB:libcpmtd.lib
        )
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_link_options(libmp_no_cxx_stdlib_smoke PRIVATE -nostdlib++)
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        # GCC has no -nostdlib++ spelling. This is its equivalent while
        # retaining the C runtime and compiler support libraries.
        target_link_options(libmp_no_cxx_stdlib_smoke PRIVATE -nodefaultlibs)
        target_link_libraries(libmp_no_cxx_stdlib_smoke PRIVATE c m gcc_s gcc)
    else()
        message(FATAL_ERROR
            "LIBMP_BUILD_NO_CXX_STDLIB_TEST requires MSVC, Clang, or GCC")
    endif()
endif()

if(LIBMP_BUILD_NO_LIBC_ALLOC_LINK_TEST)
    libmp_add_autonomy_link_executable(libmp_no_libc_alloc_link
        tests/cpp/no_libc_alloc_link.cpp
    )
    libmp_force_link_complete_archive(libmp_no_libc_alloc_link)

    # The wrappers are deliberately undefined. Any direct reference to a
    # listed allocation API in the consumer or force-loaded archive is a
    # link error. The same typed manifest also supplies the pre-main scan.
    set(LIBMP_AUTONOMY_SYMBOLS_FILE
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/autonomy-symbols.txt")
    set(LIBMP_AUTONOMY_SYMBOLS_SCRIPT
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/_autonomy_symbols.py")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${LIBMP_AUTONOMY_SYMBOLS_FILE}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${LIBMP_AUTONOMY_SYMBOLS_SCRIPT}")
    if(NOT Python3_Interpreter_FOUND)
        find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)
    endif()
    execute_process(
        COMMAND
            "${Python3_EXECUTABLE}"
            "${LIBMP_AUTONOMY_SYMBOLS_SCRIPT}"
            --system Linux
        RESULT_VARIABLE LIBMP_AUTONOMY_SYMBOLS_RESULT
        OUTPUT_VARIABLE LIBMP_AUTONOMY_SYMBOLS_OUTPUT
        ERROR_VARIABLE LIBMP_AUTONOMY_SYMBOLS_ERROR
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(NOT LIBMP_AUTONOMY_SYMBOLS_RESULT EQUAL 0)
        message(FATAL_ERROR
            "Failed to read allocator autonomy symbols: ${LIBMP_AUTONOMY_SYMBOLS_ERROR}")
    endif()
    string(REPLACE "\n" ";" LIBMP_AUTONOMY_SYMBOLS
        "${LIBMP_AUTONOMY_SYMBOLS_OUTPUT}")
    foreach(LIBMP_AUTONOMY_SYMBOL IN LISTS LIBMP_AUTONOMY_SYMBOLS)
        target_link_options(libmp_no_libc_alloc_link PRIVATE
            "-Wl,--wrap=${LIBMP_AUTONOMY_SYMBOL}")
    endforeach()
endif()

function(libmp_add_public_example target)
    libmp_add_executable(${target} ${ARGN})
    libmp_use_public_header(${target})
endfunction()

# First-party C++ test in tests/cpp/: exercises the canonical (internal) headers and is
# registered with CTest under its own name.
function(libmp_add_cpp_test target)
    libmp_add_canonical_executable(${target} ${ARGN})
    add_test(NAME ${target} COMMAND ${target})
endfunction()

function(libmp_add_canonical_executable target)
    libmp_add_executable(${target} ${ARGN})
    # Repository-internal tests intentionally exercise the canonical headers,
    # even when the library itself is compiled from the generated pair.
    target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    if(NOT LIBMP_BUILD_USE_EXTERNAL_FLATBUFFERS)
        target_include_directories(${target} SYSTEM PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/dependencies
        )
    endif()
endfunction()

# Keep the user-facing example limited to the regular std:: API and free of
# application-specific allocator hooks.
if(NOT libmp_public_ntd_api
        AND NOT libmp_ntd_external_pages
        AND NOT libmp_ntd_external_malloc
        AND NOT libmp_trusted_inputs_only)
    libmp_add_public_example(libmp_examples
        ../examples/cpp/examples.cpp
    )
endif()

if(LIBMP_BUILD_TESTS)
    enable_testing()
    if(TARGET libmp_examples)
        add_test(NAME libmp_examples_missing_input
            COMMAND libmp_examples "${CMAKE_CURRENT_BINARY_DIR}/does-not-exist.gprx")
        set_tests_properties(libmp_examples_missing_input PROPERTIES WILL_FAIL TRUE)
    endif()
    if(NOT Python3_Interpreter_FOUND)
        find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)
    endif()

    libmp_add_canonical_executable(ntd_smoke_tests
        tests/cpp/ntd_smoke_tests.cpp
    )
    libmp_add_canonical_executable(ntd_unit_tests
        tests/cpp/ntd_unit_tests.cpp
    )
    libmp_add_canonical_executable(ntd_alloc_tests
        tests/cpp/ntd_alloc_tests.cpp
    )
    if(libmp_ntd_embedded_alloc)
        libmp_add_canonical_executable(ntd_alloc_no_cache_tests
            tests/cpp/ntd_alloc_no_cache_tests.cpp
        )
    endif()

    # Proves the std and ntd flavors of cpp_api coexist in one binary (one TU includes
    # cpp_api.h, the other cpp_api_internal.h; linking both is the proof).
    libmp_add_canonical_executable(cpp_api_dual_flavor_test
        tests/cpp/cpp_api_dual_ext.cpp
        tests/cpp/cpp_api_dual_int.cpp
        tests/cpp/cpp_api_dual_main.cpp
    )
    libmp_use_public_header(cpp_api_dual_flavor_test)

    add_test(NAME ntd_smoke_tests COMMAND ntd_smoke_tests)
    add_test(NAME ntd_unit_tests COMMAND ntd_unit_tests)
    add_test(NAME ntd_alloc_tests COMMAND ntd_alloc_tests)
    if(libmp_ntd_embedded_alloc)
        add_test(NAME ntd_alloc_no_cache_tests COMMAND ntd_alloc_no_cache_tests)
    endif()
    add_test(NAME cpp_api_dual_flavor_test COMMAND cpp_api_dual_flavor_test)

    # First-party library security/behavior tests.
    libmp_add_cpp_test(control_channel_tests tests/cpp/control_channel_tests.cpp)
    libmp_add_cpp_test(codec_tests tests/cpp/codec_tests.cpp)
    libmp_add_cpp_test(getdatainrange_tests tests/cpp/getdatainrange_tests.cpp)
    libmp_add_cpp_test(service_boundary_tests tests/cpp/service_boundary_tests.cpp)
    libmp_add_cpp_test(data_context_tests tests/cpp/data_context_tests.cpp)
    libmp_add_cpp_test(c_api_tests tests/cpp/c_api_tests.cpp)
    libmp_add_cpp_test(capture_roundtrip_tests tests/cpp/capture_roundtrip_tests.cpp)
    libmp_add_cpp_test(gprx_container_tests tests/cpp/gprx_container_tests.cpp)
    libmp_add_cpp_test(kv_store_tests tests/cpp/kv_store_tests.cpp)

    # Cross-language parity: C++ and Luau form the required baseline; other built wrappers
    # join the same byte-identical comparison when their runtimes are available. The driver
    # skips when the baseline is unavailable, so it remains safe in a native-only build.
    # Its default capture is reconstructed from the committed source-data fixture.
    libmp_add_canonical_executable(parity_tool tests/tools/parity_tool.cpp)
    # Built with the test/tool targets, but intentionally not registered with CTest: this
    # is a comparative performance runner, not a pass/fail regression test.
    libmp_add_canonical_executable(libmp_benchmark tests/benchmark/native_benchmark.cpp)
    add_test(
        NAME cross_language_parity
        COMMAND
            "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/parity/run_parity.py"
            --parity-tool "$<TARGET_FILE:parity_tool>"
    )
    set_tests_properties(cross_language_parity PROPERTIES SKIP_RETURN_CODE 77)
    add_test(
        NAME check_symbols_parser_tests
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/selftests/check_symbols_tests.py"
    )
    add_test(
        NAME msvc_env_tests
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/selftests/msvc_env_tests.py"
    )
    add_test(
        NAME parity_runner_tests
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/selftests/parity_runner_tests.py"
    )
    add_test(
        NAME flatbuffer_generator_tests
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/selftests/gen_flatbuffers_tests.py"
    )
    add_test(
        NAME fuzz_runner_tests
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/selftests/fuzz_runner_tests.py"
    )
    add_test(
        NAME amalgamation_generator_tests
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/selftests/amalgamate_tests.py"
    )
    if(LIBMP_BUILD_AMALGAMATION_MATRIX_TESTS)
        set(LIBMP_AMALGAMATION_COMPILE_TEST_ARGS
            --cxx "${CMAKE_CXX_COMPILER}"
            --compiler-id "${CMAKE_CXX_COMPILER_ID}"
        )
        # CMake separates arguments supplied alongside CXX (for example a Clang
        # --gcc-install-dir selection) from CMAKE_CXX_FLAGS. Preserve both when
        # the matrix invokes the compiler directly.
        set(LIBMP_AMALGAMATION_CXX_FLAGS_TEXT
            "${CMAKE_CXX_COMPILER_ARG1} ${CMAKE_CXX_FLAGS}")
        separate_arguments(LIBMP_AMALGAMATION_CXX_FLAGS
            NATIVE_COMMAND "${LIBMP_AMALGAMATION_CXX_FLAGS_TEXT}")
        foreach(LIBMP_CXX_FLAG IN LISTS LIBMP_AMALGAMATION_CXX_FLAGS)
            list(APPEND LIBMP_AMALGAMATION_COMPILE_TEST_ARGS
                "--cxx-flag=${LIBMP_CXX_FLAG}")
        endforeach()

        # add_test() resolves $<CONFIG> when CTest selects a configuration. Add
        # that configuration's compiler flags to the direct invocation for both
        # single-config generators and Visual Studio/Xcode.
        if(CMAKE_CONFIGURATION_TYPES)
            set(LIBMP_AMALGAMATION_CONFIGURATIONS ${CMAKE_CONFIGURATION_TYPES})
        elseif(CMAKE_BUILD_TYPE)
            set(LIBMP_AMALGAMATION_CONFIGURATIONS ${CMAKE_BUILD_TYPE})
        else()
            set(LIBMP_AMALGAMATION_CONFIGURATIONS)
        endif()
        foreach(LIBMP_CONFIGURATION IN LISTS LIBMP_AMALGAMATION_CONFIGURATIONS)
            string(TOUPPER "${LIBMP_CONFIGURATION}" LIBMP_CONFIGURATION_UPPER)
            separate_arguments(LIBMP_AMALGAMATION_CONFIGURATION_FLAGS
                NATIVE_COMMAND "${CMAKE_CXX_FLAGS_${LIBMP_CONFIGURATION_UPPER}}")
            foreach(LIBMP_CXX_FLAG IN LISTS LIBMP_AMALGAMATION_CONFIGURATION_FLAGS)
                list(APPEND LIBMP_AMALGAMATION_COMPILE_TEST_ARGS
                    "$<$<CONFIG:${LIBMP_CONFIGURATION}>:--cxx-flag=${LIBMP_CXX_FLAG}>")
            endforeach()
        endforeach()

        if(MSVC)
            list(APPEND LIBMP_AMALGAMATION_COMPILE_TEST_ARGS --msvc-style)
            if(CMAKE_GENERATOR_PLATFORM)
                list(APPEND LIBMP_AMALGAMATION_COMPILE_TEST_ARGS
                    --msvc-architecture "${CMAKE_GENERATOR_PLATFORM}")
            endif()
        endif()
        add_test(
            NAME amalgamation_matrix_tests
            COMMAND
                "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/scripts/selftests/amalgamation_matrix_tests.py"
                ${LIBMP_AMALGAMATION_COMPILE_TEST_ARGS}
            COMMAND_EXPAND_LISTS
        )
    endif()
endif()
