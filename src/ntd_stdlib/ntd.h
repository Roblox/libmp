// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

#pragma once

// Convenience umbrella that pulls in every ntd wrapper. Supported builds use
// standard primitives; with a custom ntd::mem backend, allocator-aware standard
// types use ntd::allocator. NTD_OWN_STDLIB retains reserved implementation points
// for a standalone primitive set. This file is just an include list.
//
// Prefer including the specific headers you need (e.g. "ntd_stdlib/vector.h");
// this umbrella exists for convenience and for code that wants "all of ntd".

#include "ntd_config.h"
#include "allocator.h" // ntd::mem (present in every mode)

#include "type_traits.h"
#include "utility.h" // move/forward/swap, pair, index_sequence
#include "tuple.h"
#include "iterator.h"
#include "algorithm.h"
#include "limits.h"
#include "cstring.h"
#include "array.h"
#include "optional.h"
#include "memory.h"     // unique_ptr, shared_ptr
#include "functional.h" // function, hash, equal_to, less
#include "vector.h"
#include "set.h"
#include "string.h"
#include "string_view.h"
#include "unordered_map.h"
#include "unordered_set.h"
#include "deque.h"
#include "list.h"
