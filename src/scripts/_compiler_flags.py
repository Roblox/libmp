# This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

"""Shared compiler warning policy for direct Python-driven C++ builds."""


GNU_LIKE_STRICT_WARNINGS = (
    "-Wall",
    "-Wextra",
    "-Werror",
    "-Wundef",
)

GNU_LIKE_LIBRARY_WARNINGS = (
    "-Wcast-qual",
    "-Wconversion",
    "-Wdouble-promotion",
    "-Wextra-semi",
    "-Wimplicit-fallthrough",
    "-Wnull-dereference",
    "-Wshadow",
    "-Wshift-overflow",
    "-Wsign-conversion",
)

CLANG_LIBRARY_WARNINGS = (
    "-Wbitwise-op-parentheses",
    "-Wcovered-switch-default",
    "-Wlogical-op-parentheses",
    "-Wunreachable-code",
)

GNU_LIBRARY_WARNINGS = (
    "-Wlogical-op",
    "-Wparentheses",
)

MSVC_STRICT_WARNINGS = (
    "/external:anglebrackets",
    "/external:W0",
    "/W4",
    "/WX",
    "/we4668",
)

MSVC_LIBRARY_WARNINGS = (
    "/we4018",
    "/we4146",
    "/we4244",
    "/we4245",
    "/we4267",
    "/we4293",
    "/we4305",
    "/we4365",
    "/we4389",
    "/we4456",
    "/we4457",
    "/we4458",
    "/we4459",
    "/we4554",
    "/we4702",
)


def library_warning_flags(compiler_id):
    """Return the CMake-equivalent warning policy for a compiler family."""
    compiler_id = compiler_id.lower()
    if compiler_id == "msvc":
        return (*MSVC_STRICT_WARNINGS, *MSVC_LIBRARY_WARNINGS)
    if "clang" in compiler_id or compiler_id in ("emcc", "emscripten"):
        return (
            *GNU_LIKE_STRICT_WARNINGS,
            *GNU_LIKE_LIBRARY_WARNINGS,
            *CLANG_LIBRARY_WARNINGS,
        )
    if compiler_id in ("gnu", "gcc"):
        return (
            *GNU_LIKE_STRICT_WARNINGS,
            *GNU_LIKE_LIBRARY_WARNINGS,
            *GNU_LIBRARY_WARNINGS,
        )
    return GNU_LIKE_STRICT_WARNINGS
