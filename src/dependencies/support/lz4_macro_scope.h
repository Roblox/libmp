// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

// Intentionally no include guard: include once with GPRX_LZ4_MACRO_SCOPE_PUSH
// before the bundled sources and once with GPRX_LZ4_MACRO_SCOPE_POP afterward.

#if defined(GPRX_LZ4_MACRO_SCOPE_PUSH) == defined(GPRX_LZ4_MACRO_SCOPE_POP)
    #error "select exactly one LZ4 macro-scope operation"
#endif

#if defined(_MSC_VER)
    #define GPRX_LZ4_MACRO_PRAGMA(value) __pragma(value)
#else
    #define GPRX_LZ4_MACRO_PRAGMA_IMPL(value) _Pragma(#value)
    #define GPRX_LZ4_MACRO_PRAGMA(value) GPRX_LZ4_MACRO_PRAGMA_IMPL(value)
#endif

#if defined(GPRX_LZ4_MACRO_SCOPE_PUSH)
    #define GPRX_LZ4_MACRO_APPLY(name) GPRX_LZ4_MACRO_PRAGMA(push_macro(name))
#else
    #define GPRX_LZ4_MACRO_APPLY(name) GPRX_LZ4_MACRO_PRAGMA(pop_macro(name))
#endif

// The bundled C implementation uses these unprefixed macros internally. Preserve
// the surrounding translation unit's definitions while compiling it inline.
#define GPRX_LZ4_PRIVATE_MACROS(APPLY) \
    APPLY("ADDPOS4") \
    APPLY("ADDPOS8") \
    APPLY("ALLOC") \
    APPLY("ALLOC_AND_ZERO") \
    APPLY("DEBUGLOG") \
    APPLY("DELTANEXTU16") \
    APPLY("FASTLOOP_SAFE_DISTANCE") \
    APPLY("FORCE_INLINE") \
    APPLY("FORWARD_IF_ERROR") \
    APPLY("FREEMEM") \
    APPLY("GB") \
    APPLY("HASH_FUNCTION") \
    APPLY("HASH_UNIT") \
    APPLY("HEX_CMP") \
    APPLY("KB") \
    APPLY("LASTLITERALS") \
    APPLY("MATCH_SAFEGUARD_DISTANCE") \
    APPLY("MAX") \
    APPLY("MB") \
    APPLY("MEM_INIT") \
    APPLY("MEM_MODULE") \
    APPLY("MFLIMIT") \
    APPLY("MIN") \
    APPLY("MINMATCH") \
    APPLY("ML_BITS") \
    APPLY("ML_MASK") \
    APPLY("OPTIMAL_ML") \
    APPLY("PROCESS1") \
    APPLY("PROCESS1_64") \
    APPLY("PROCESS4") \
    APPLY("PROCESS4_64") \
    APPLY("PROCESS8_64") \
    APPLY("RAWLOG") \
    APPLY("RETURN_ERROR") \
    APPLY("RETURN_ERROR_IF") \
    APPLY("RUN_BITS") \
    APPLY("RUN_MASK") \
    APPLY("STEPSIZE") \
    APPLY("TRAILING_LITERALS") \
    APPLY("UPDATABLE") \
    APPLY("WILDCOPYLENGTH") \
    APPLY("_1BIT") \
    APPLY("_2BITS") \
    APPLY("_3BITS") \
    APPLY("_4BITS") \
    APPLY("_8BITS") \
    APPLY("anchor") \
    APPLY("assert") \
    APPLY("expect") \
    APPLY("free") \
    APPLY("ip") \
    APPLY("likely") \
    APPLY("malloc") \
    APPLY("op") \
    APPLY("unlikely") \
    APPLY("LZ4_USER_MEMORY_FUNCTIONS")

GPRX_LZ4_PRIVATE_MACROS(GPRX_LZ4_MACRO_APPLY)

#undef GPRX_LZ4_PRIVATE_MACROS
#undef GPRX_LZ4_MACRO_APPLY
#undef GPRX_LZ4_MACRO_PRAGMA
#if !defined(_MSC_VER)
    #undef GPRX_LZ4_MACRO_PRAGMA_IMPL
#endif
