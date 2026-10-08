// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

// ntd_unit_tests.cpp -- black-box unit tests for the ntd compatibility surface.
//
// Each expectation encodes the C++17 contract for the corresponding std:: type (ntd mirrors a
// subset of std). Where useful, a real std container is used as a reference model for
// differential (randomized) testing. In a default allocator build ntd IS std,
// which validates the test oracle itself. With a custom allocator, the same tests
// exercise the distinct standard-template/ntd::allocator combination. Conditional
// expectations retain the contract for the reserved NTD_OWN_STDLIB surface.
//
// Enumerated surface under test:
//   vector<T>      : ctors(default,n,n+v,init_list,copy,move), =, [], front, back, data,
//                    begin/end, empty, size, capacity, reserve, resize(n)/(n,v), push_back,
//                    emplace_back, pop_back, clear, insert(pos,first,last), assign(n,v)/(first,last)
//   deque<T>       : ctors, =, [], front, back, begin/end, empty, size, push_back, emplace_back,
//                    erase(first,last), resize(n), clear   (+ stable element addresses)
//   list<T>        : move-only; push_back, push_front, front, back, begin/end, rbegin/rend,
//                    empty, size, erase(it), pop_back, splice, clear
//   array<T,N>     : [], data, begin/end, size, fill
//   string         : ctors, =, append/+= , [], c_str, data, begin/end, size, length, empty,
//                    clear, push_back, ==, !=
//   string_view    : ctors(default,cstr,ptr+len,from string), [], data, begin/end, size,
//                    length, empty
//   optional<T>    : ctors, =(value/opt/nullopt), *, value, has_value, bool, reset, value_or
//   pair<A,B>      : first, second, ctors, make_pair
//   tuple<Ts...>   : get<>, make_tuple, tie
//   unordered_map  : emplace, insert, try_emplace, [], at(present key), find, erase(key/it),
//                    count, clear, size, empty, iterate   (contains: ntd-only, guarded)
//   unordered_set  : insert, emplace, find, erase, count, clear, size, empty, iterate
//   set            : constructors, copy/move, insert, find, clear, size, empty, ordered iterate,
//                    comparator equivalence, immutable/stable iterators
//   unique_ptr<T>  : make_unique, get, *, ->, bool, release, reset, move,
//                    address-preserving base conversion, =(nullptr)
//   shared_ptr<T>  : make_shared, get, *, ->, bool, use_count, reset, copy, move
//   function<Sig>  : from callable, (), bool, =(nullptr), copy, move
//   hash<K>/equal_to<K> : ()
//   algorithm      : min/max(2-arg, init_list), lower_bound, upper_bound, sort, copy, fill,
//                    fill_n, reverse, transform
//   cstring        : memcpy, memmove, memset, memcmp, strlen, strnlen
//   utility        : swap, pair, make_pair

#include <cstdio>
#include <cstdint>
#include <cstring> // std::strcmp/std::strlen for oracle comparisons
// independent reference models (real std):
#include <vector>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <set>
#include <string>
#include <type_traits>
#include <iterator>

#include "ntd_stdlib/vector.h"
#include "ntd_stdlib/deque.h"
#include "ntd_stdlib/list.h"
#include "ntd_stdlib/array.h"
#include "ntd_stdlib/string.h"
#include "ntd_stdlib/string_view.h"
#include "ntd_stdlib/optional.h"
#include "ntd_stdlib/utility.h"
#include "ntd_stdlib/tuple.h"
#include "ntd_stdlib/unordered_map.h"
#include "ntd_stdlib/unordered_set.h"
#include "ntd_stdlib/set.h"
#include "ntd_stdlib/memory.h"
#include "ntd_stdlib/functional.h"
#include "ntd_stdlib/algorithm.h"
#include "ntd_stdlib/iterator.h"
#include "ntd_stdlib/cstring.h"
#include "ntd_stdlib/file_io.h"
#include "test_host_allocator.h"

static int g_fail = 0;
static int g_checks = 0;
#define CHECK(cond)                                                                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        ++g_checks;                                                                                                    \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            ++g_fail;                                                                                                  \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                                              \
        }                                                                                                              \
    } while (0)

// Lifetime tracker: verifies ctor/dtor/copy/move balance (no leaks / correct element lifetime).
struct Tracked
{
    static int live, copies, moves;
    int v;
    Tracked(int x = 0) : v(x)
    {
        ++live;
    }
    Tracked(const Tracked& o) : v(o.v)
    {
        ++live;
        ++copies;
    }
    Tracked(Tracked&& o) noexcept : v(o.v)
    {
        o.v = -1;
        ++live;
        ++moves;
    }
    Tracked& operator=(const Tracked& o)
    {
        v = o.v;
        ++copies;
        return *this;
    }
    Tracked& operator=(Tracked&& o) noexcept
    {
        v = o.v;
        o.v = -1;
        ++moves;
        return *this;
    }
    ~Tracked()
    {
        --live;
    }
    static void reset()
    {
        live = copies = moves = 0;
    }
};
int Tracked::live = 0, Tracked::copies = 0, Tracked::moves = 0;

#if defined(_MSC_VER)
    #pragma warning(push)
    // C4324: over-alignment intentionally pads this struct; that is what we test.
    #pragma warning(disable : 4324)
#endif
struct alignas(64) OverAligned
{
    int value;
    explicit OverAligned(int v = 0) : value(v)
    {
    }
};
#if defined(_MSC_VER)
    #pragma warning(pop)
#endif

struct DestructionPoison
{
    int value;
    explicit DestructionPoison(int v) : value(v)
    {
    }
    DestructionPoison(const DestructionPoison& other) : value(other.value)
    {
    }
    DestructionPoison& operator=(const DestructionPoison& other)
    {
        value = other.value;
        return *this;
    }
    ~DestructionPoison()
    {
        value = -1;
    }
};

struct UniqueBase
{
    static int destroyed;
    virtual ~UniqueBase()
    {
        ++destroyed;
    }
};
int UniqueBase::destroyed = 0;

struct UniqueDerived : UniqueBase
{
    static int destroyed;
    ~UniqueDerived() override
    {
        ++destroyed;
    }
};
int UniqueDerived::destroyed = 0;

// Deterministic PRNG for differential tests (no <random> dependency).
struct Rng
{
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed)
    {
    }
    uint32_t next()
    {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        return (uint32_t)(s >> 33);
    }
    uint32_t operator()(uint32_t n)
    {
        return n ? next() % n : 0;
    }
};

// Copies share one cursor, like an input iterator backed by a stream.
struct SinglePassIntRange
{
    struct State
    {
        const int* current;
        const int* end;
    };
    struct Iterator
    {
        using iterator_category = std::input_iterator_tag;
        using value_type = int;
        using difference_type = ptrdiff_t;
        using pointer = const int*;
        using reference = int;

        State* state;
        bool sentinel;
        int operator*() const
        {
            return *state->current;
        }
        Iterator& operator++()
        {
            ++state->current;
            return *this;
        }
        Iterator operator++(int)
        {
            Iterator old = *this;
            ++*this;
            return old;
        }
        bool operator==(const Iterator& other) const
        {
            if (sentinel != other.sentinel)
                return state->current == state->end;
            return sentinel || state->current == other.state->current;
        }
        bool operator!=(const Iterator& other) const
        {
            return !(*this == other);
        }
    };

    State state;
    SinglePassIntRange(const int* first, const int* last) : state{first, last}
    {
    }
    Iterator begin()
    {
        return Iterator{&state, false};
    }
    Iterator end()
    {
        return Iterator{&state, true};
    }
};

// ---------------------------------------------------------------------- vector
static void test_vector()
{
#if NTD_ROUTED_STD_TYPES
    static_assert(std::is_same<typename ntd::vector<int>::allocator_type, ntd::allocator<int>>::value,
                  "std-backed ntd::vector must use ntd::allocator");
#endif
    ntd::vector<int> v;
    CHECK(v.size() == 0);
    CHECK(v.empty());

    for (int i = 0; i < 1000; ++i)
        v.push_back(i);
    CHECK(v.size() == 1000);
    CHECK(!v.empty());
    CHECK(v[0] == 0 && v[999] == 999);
    CHECK(v.front() == 0 && v.back() == 999);
    CHECK(v.data() != nullptr && v.data()[500] == 500);
    {
        long sum = 0;
        for (int x : v)
            sum += x;
        CHECK(sum == (long)999 * 1000 / 2);
    }

    ntd::vector<int> r;
    r.reserve(100);
    CHECK(r.capacity() >= 100 && r.size() == 0);

    ntd::vector<int> e;
    int& ref = e.emplace_back(42);
    CHECK(ref == 42 && e.back() == 42 && e.size() == 1);

    v.pop_back();
    CHECK(v.size() == 999 && v.back() == 998);

    ntd::vector<int> z;
    z.resize(5);
    CHECK(z.size() == 5 && z[0] == 0 && z[4] == 0); // value-initialized
    z.resize(8, 7);
    CHECK(z.size() == 8 && z[4] == 0 && z[7] == 7);
    z.resize(2);
    CHECK(z.size() == 2 && z[0] == 0);
    z.clear();
    CHECK(z.empty());

    ntd::vector<int> a(4);
    CHECK(a.size() == 4 && a[0] == 0);
    ntd::vector<int> b(3, 9);
    CHECK(b.size() == 3 && b[0] == 9 && b[2] == 9);
    ntd::vector<int> il{1, 2, 3};
    CHECK(il.size() == 3 && il[0] == 1 && il[2] == 3);

    ntd::vector<int> c = il; // copy is deep
    CHECK(c.size() == 3 && c[2] == 3);
    c[0] = 99;
    CHECK(il[0] == 1);
    ntd::vector<int> m = ntd::move(c);
    CHECK(m.size() == 3 && m[0] == 99);

    ntd::vector<int> as;
    as.assign(3, 5);
    CHECK(as.size() == 3 && as[2] == 5);
    int arr[] = {7, 8, 9};
    as.assign(arr, arr + 3);
    CHECK(as.size() == 3 && as[0] == 7 && as[2] == 9);

    ntd::vector<int> ins{1, 2, 5};
    int mid[] = {3, 4};
    ins.insert(ins.begin() + 2, mid, mid + 2);
    CHECK(ins.size() == 5 && ins[2] == 3 && ins[3] == 4 && ins[4] == 5);

    ntd::vector<int> selfInsert{1, 2, 3, 4};
    selfInsert.insert(selfInsert.end(), selfInsert.begin(), selfInsert.end());
    CHECK(selfInsert.size() == 8 && selfInsert[0] == 1 && selfInsert[4] == 1 && selfInsert[7] == 4);

    int streamedValues[] = {2, 3, 4};
    SinglePassIntRange streamed(streamedValues, streamedValues + 3);
    ntd::vector<int> singlePass{1, 5};
    singlePass.insert(singlePass.begin() + 1, streamed.begin(), streamed.end());
    CHECK(singlePass.size() == 5 && singlePass[0] == 1 && singlePass[1] == 2 && singlePass[3] == 4 &&
          singlePass[4] == 5);

    ntd::vector<int> selfPush{42};
    selfPush.push_back(selfPush[0]);
    CHECK(selfPush.size() == 2 && selfPush[0] == 42 && selfPush[1] == 42);

    Tracked::reset();
    {
        ntd::vector<Tracked> selfEmplace;
        selfEmplace.reserve(1);
        selfEmplace.emplace_back(17);
        selfEmplace.emplace_back(selfEmplace[0]);
        CHECK(selfEmplace.size() == 2 && selfEmplace[0].v == 17 && selfEmplace[1].v == 17);
    }
    CHECK(Tracked::live == 0);

    ntd::vector<int> selfResize{7};
    selfResize.resize(5, selfResize[0]);
    CHECK(selfResize.size() == 5 && selfResize[4] == 7);

    // assign(n, value) where value aliases an element of *this. The embedded
    // ntd::vector copies the value before reallocating (see points_into_storage
    // in vector.h). When ntd::vector IS std::vector (passthrough/routed flavors),
    // this exercises the host STL: libstdc++/libc++ honor the self-alias, but
    // Microsoft's STL reads the reference after it has been invalidated and
    // yields garbage. Detect that library specifically: clang-cl also defines
    // _MSC_VER and need not use Microsoft's STL.
#if !defined(_MSVC_STL_VERSION) || NTD_OWN_STDLIB
    ntd::vector<int> selfAssign{8, 9};
    selfAssign.assign(3, selfAssign[1]);
    CHECK(selfAssign.size() == 3 && selfAssign[0] == 9 && selfAssign[2] == 9);
#endif

    // Standard vector::assign does not permit its input range to come from the
    // same container. This stronger behavior is an ntd::vector contract only.
#if NTD_OWN_STDLIB
    ntd::vector<int> selfRangeAssign{1, 2, 3, 4};
    selfRangeAssign.assign(selfRangeAssign.begin() + 1, selfRangeAssign.end());
    CHECK(selfRangeAssign.size() == 3 && selfRangeAssign[0] == 2 && selfRangeAssign[2] == 4);
#endif

    // element lifetime: destroyed exactly once, no leaks
    Tracked::reset();
    {
        ntd::vector<Tracked> tv;
        for (int i = 0; i < 50; ++i)
            tv.push_back(Tracked(i));
        CHECK(Tracked::live == 50);
        CHECK(tv[25].v == 25);
    }
    CHECK(Tracked::live == 0);

    // differential vs std::vector
    Rng rng(12345);
    ntd::vector<int> nv;
    std::vector<int> sv;
    for (int i = 0; i < 6000; ++i)
    {
        uint32_t op = rng(3);
        if (op == 0 || nv.empty())
        {
            int x = (int)rng(1000);
            nv.push_back(x);
            sv.push_back(x);
        }
        else if (op == 1)
        {
            nv.pop_back();
            sv.pop_back();
        }
        else
        {
            size_t idx = rng((uint32_t)nv.size());
            int x = (int)rng(1000);
            nv[idx] = x;
            sv[idx] = x;
        }
    }
    bool same = nv.size() == sv.size();
    for (size_t i = 0; same && i < nv.size(); ++i)
        same = (nv[i] == sv[i]);
    CHECK(same);
}

// ----------------------------------------------------------------------- deque
static void test_deque()
{
    ntd::deque<int> d;
    CHECK(d.empty() && d.size() == 0);
    for (int i = 0; i < 2000; ++i)
        d.push_back(i);
    CHECK(d.size() == 2000 && d[0] == 0 && d[1999] == 1999);
    CHECK(d.front() == 0 && d.back() == 1999);
    {
        long sum = 0;
        for (int x : d)
            sum += x;
        CHECK(sum == (long)1999 * 2000 / 2);
    }

    int& er = d.emplace_back(12345);
    CHECK(er == 12345 && d.back() == 12345);

    // stable element addresses across growth (chunked deque guarantee)
    ntd::deque<int> s;
    s.push_back(0);
    int* p0 = &s[0];
    for (int i = 1; i < 5000; ++i)
        s.push_back(i);
    CHECK(&s[0] == p0); // first element never moves
    CHECK(s[0] == 0 && s[4999] == 4999);

    // erase(first, last) range
    ntd::deque<int> e;
    for (int i = 0; i < 10; ++i)
        e.push_back(i);
    e.erase(e.begin() + 2, e.begin() + 5); // removes 2,3,4
    CHECK(e.size() == 7 && e[0] == 0 && e[1] == 1 && e[2] == 5 && e[6] == 9);
    e.erase(e.begin() + 2, e.begin() + 2); // empty erase must not self-move elements
    CHECK(e.size() == 7 && e[2] == 5 && e[6] == 9);

    // resize + clear
    ntd::deque<int> z;
    for (int i = 0; i < 100; ++i)
        z.push_back(i);
    z.resize(10);
    CHECK(z.size() == 10 && z[9] == 9);
    z.clear();
    CHECK(z.empty());

    // copy/move + element lifetime
    Tracked::reset();
    {
        ntd::deque<Tracked> td;
        for (int i = 0; i < 300; ++i)
            td.push_back(Tracked(i));
        CHECK(Tracked::live == 300);
        CHECK(td.begin()->v == 0);
        const ntd::deque<Tracked>& const_td = td;
        CHECK(const_td.begin()->v == 0);
        ntd::deque<Tracked> cp = td;
        CHECK(Tracked::live == 600 && cp[299].v == 299);
        ntd::deque<Tracked> mv = ntd::move(cp);
        CHECK(mv[299].v == 299);
    }
    CHECK(Tracked::live == 0);

    // differential vs std::deque
    Rng rng(555);
    ntd::deque<int> nd;
    std::deque<int> sd;
    for (int i = 0; i < 4000; ++i)
    {
        uint32_t op = rng(4);
        if (op < 3 || nd.empty())
        {
            int x = (int)rng(1000);
            nd.push_back(x);
            sd.push_back(x);
        }
        else
        {
            size_t cnt = 1 + rng((uint32_t)nd.size());
            size_t k = cnt > nd.size() ? nd.size() : cnt;
            nd.erase(nd.begin(), nd.begin() + (long)k);
            sd.erase(sd.begin(), sd.begin() + (long)k);
        }
    }
    bool same = nd.size() == sd.size();
    for (size_t i = 0; same && i < nd.size(); ++i)
        same = (nd[i] == sd[i]);
    CHECK(same);
}

// ------------------------------------------------------------------------ list
static void test_list()
{
    ntd::list<int> l;
    CHECK(l.empty() && l.size() == 0);
    for (int i = 0; i < 5; ++i)
        l.push_back(i); // 0 1 2 3 4
    l.push_front(-1);   // -1 0 1 2 3 4
    CHECK(l.size() == 6 && l.front() == -1 && l.back() == 4);

    {
        int expect[] = {-1, 0, 1, 2, 3, 4};
        int i = 0;
        bool ok = true;
        for (auto it = l.begin(); it != l.end(); ++it, ++i)
            ok = ok && (*it == expect[i]);
        CHECK(ok && i == 6);
    }

    // reverse iteration
    {
        int expect[] = {4, 3, 2, 1, 0, -1};
        int i = 0;
        bool ok = true;
        for (auto it = l.rbegin(); it != l.rend(); ++it, ++i)
            ok = ok && (*it == expect[i]);
        CHECK(ok && i == 6);
    }

    // erase(it): remove the element with value 1
    for (auto it = l.begin(); it != l.end(); ++it)
        if (*it == 1)
        {
            l.erase(it);
            break;
        }
    {
        bool found1 = false;
        for (int x : l)
            if (x == 1)
                found1 = true;
        CHECK(!found1 && l.size() == 5);
    }

    l.pop_back(); // drops 4
    CHECK(l.back() == 3 && l.size() == 4);

    // move (list is move-only)
    ntd::list<int> m = ntd::move(l);
    CHECK(m.size() == 4 && m.front() == -1);

    // splice one node from other into m
    ntd::list<int> other;
    other.push_back(777);
    m.splice(m.begin(), other, other.begin());
    CHECK(m.front() == 777 && other.empty() && m.size() == 5);

    // Same-list splice at the node itself and at its immediate successor are
    // required no-ops.  The former is also the page-cache's repeated-MRU-hit
    // path in DataContextCachingProxy.
    auto first = m.begin();
    auto second = ntd::next(first);
    m.splice(first, m, first);
    CHECK(m.size() == 5 && m.begin() == first && m.front() == 777);
    m.splice(second, m, first);
    CHECK(m.size() == 5 && m.begin() == first && *ntd::next(m.begin()) == *second);

    l.clear();
    CHECK(l.empty());

    // element lifetime
    Tracked::reset();
    {
        ntd::list<Tracked> t;
        for (int i = 0; i < 100; ++i)
            t.push_back(Tracked(i));
        CHECK(Tracked::live == 100);
        CHECK(t.begin()->v == 0);
        CHECK(t.rbegin()->v == 99);
    }
    CHECK(Tracked::live == 0);
}

// ----------------------------------------------------------------------- array
static void test_array()
{
    ntd::array<int, 4> a{};
    CHECK(a.size() == 4);
    for (size_t i = 0; i < a.size(); ++i)
        a[i] = (int)i * 10;
    CHECK(a[0] == 0 && a[3] == 30);
    CHECK(a.data()[2] == 20);
    {
        int sum = 0;
        for (int x : a)
            sum += x;
        CHECK(sum == 60);
    }
    a.fill(7);
    CHECK(a[0] == 7 && a[1] == 7 && a[3] == 7);
    CHECK(a.end() - a.begin() == 4);
}

// ---------------------------------------------------------------------- string
static void test_string()
{
    ntd::string s;
    CHECK(s.empty() && s.size() == 0 && s.length() == 0);

    s.append("hello");
    CHECK(s.size() == 5 && std::strcmp(s.c_str(), "hello") == 0);
    s += ' ';
    s += "world";
    CHECK(s.size() == 11 && std::strcmp(s.c_str(), "hello world") == 0);
    CHECK(s[0] == 'h' && s[10] == 'd');
    CHECK(s.data()[4] == 'o');

    ntd::string a("abc");
    ntd::string b(a); // copy
    CHECK(std::strcmp(b.c_str(), "abc") == 0);
    b.push_back('d');
    CHECK(std::strcmp(b.c_str(), "abcd") == 0 && std::strcmp(a.c_str(), "abc") == 0);

    ntd::string mv = ntd::move(b); // move
    CHECK(std::strcmp(mv.c_str(), "abcd") == 0);

    ntd::string n(5, 'x');
    CHECK(n.size() == 5 && std::strcmp(n.c_str(), "xxxxx") == 0);

    ntd::string p("literal");
    CHECK(std::strlen(p.c_str()) == p.size()); // c_str is NUL-terminated at size()

    ntd::string q("count", 3); // (ptr, len)
    CHECK(q.size() == 3 && std::strcmp(q.c_str(), "cou") == 0);

    // == / !=
    ntd::string e1("same"), e2("same"), e3("diff");
    CHECK(e1 == e2);
    CHECK(e1 != e3);

    s.clear();
    CHECK(s.empty() && s.size() == 0);

    // assign(ptr, len)
    ntd::string as;
    as.assign("abcdef", 4);
    CHECK(as.size() == 4 && std::strcmp(as.c_str(), "abcd") == 0);
    as.assign("", 0);
    CHECK(as.empty() && as.c_str()[0] == '\0');
    as = "nonempty";
    as = "";
    CHECK(as.empty() && as.c_str()[0] == '\0');
    as = "nonempty";
    const ntd::string emptyString;
    as = emptyString;
    CHECK(as.empty() && as.c_str()[0] == '\0');

    ntd::string self("abcdefghijklmnopqrstuvwxyz");
    self += self;
    CHECK(self.size() == 52 && std::strcmp(self.c_str(), "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz") == 0);

    ntd::string overlap("abcdef");
    overlap.assign(overlap.data() + 1, 4);
    CHECK(overlap.size() == 4 && std::strcmp(overlap.c_str(), "bcde") == 0);
    overlap = &overlap[2];
    CHECK(overlap.size() == 2 && std::strcmp(overlap.c_str(), "de") == 0);

    // iterate
    ntd::string it("xyz");
    int cnt = 0;
    char last = 0;
    for (char ch : it)
    {
        ++cnt;
        last = ch;
    }
    CHECK(cnt == 3 && last == 'z');

    // grow across many appends, compare to std::string
    std::string ref;
    ntd::string big;
    for (int i = 0; i < 500; ++i)
    {
        big.push_back((char)('a' + i % 26));
        ref.push_back((char)('a' + i % 26));
    }
    CHECK(big.size() == ref.size() && std::strcmp(big.c_str(), ref.c_str()) == 0);
}

// ----------------------------------------------------------------- string_view
static void test_string_view()
{
    ntd::string_view sv;
    CHECK(sv.empty() && sv.size() == 0);

    ntd::string_view a("hello");
    CHECK(a.size() == 5 && a[0] == 'h' && a[4] == 'o');
    CHECK(a.data() != nullptr && a.length() == 5);
    {
        int cnt = 0;
        for (char ch : a)
        {
            (void)ch;
            ++cnt;
        }
        CHECK(cnt == 5);
    }

    ntd::string_view b("hello world", 5);
    CHECK(b.size() == 5 && b[4] == 'o');

    ntd::string str("from_string");
    ntd::string_view c(str);
    CHECK(c.size() == str.size() && c[0] == 'f');
}

// -------------------------------------------------------------------- optional
static void test_optional()
{
    ntd::optional<int> e;
    CHECK(!e.has_value());
    CHECK(!(bool)e);
    CHECK(e.value_or(42) == 42);

    ntd::optional<int> o(7);
    CHECK(o.has_value() && (bool)o);
    CHECK(*o == 7 && o.value() == 7);
    CHECK(o.value_or(42) == 7);

    o = 9;
    CHECK(*o == 9);
    *o = 11;
    CHECK(o.value() == 11);

    o.reset();
    CHECK(!o.has_value());

    o = ntd::nullopt;
    CHECK(!o.has_value());

    // optional-to-optional construction and assignment (the converting operator=(U&&) is
    // SFINAE-guarded so it does not shadow the copy/move paths).
    ntd::optional<int> src(123);
    ntd::optional<int> cc(src); // copy-construct from lvalue optional
    CHECK(cc.has_value() && *cc == 123 && src.has_value());
    ntd::optional<int> mc(ntd::move(src)); // move-construct
    CHECK(mc.has_value() && *mc == 123);
    ntd::optional<int> ca;
    ca = cc; // copy-assign from optional
    CHECK(ca.has_value() && *ca == 123);
    ntd::optional<int> ma;
    ma = ntd::move(cc); // move-assign from optional
    CHECK(ma.has_value() && *ma == 123);
    ntd::optional<int> empty;
    ca = empty; // assigning an empty optional clears it
    CHECK(!ca.has_value());

    ntd::optional<DestructionPoison> self(DestructionPoison(77));
    self = *self;
    CHECK(self->value == 77);

    // engaged optional destroys its value
    Tracked::reset();
    {
        ntd::optional<Tracked> t;
        CHECK(Tracked::live == 0);
        t = Tracked(5);
        CHECK(Tracked::live == 1 && t.value().v == 5);
        t.reset();
        CHECK(Tracked::live == 0);
    }
    CHECK(Tracked::live == 0);
}

// ------------------------------------------ std implementation + ntd allocator
static void test_std_backed_allocator_routing()
{
#if NTD_ROUTED_STD_TYPES
    static_assert(std::is_same<typename ntd::string::allocator_type, ntd::allocator<char>>::value,
                  "std-backed ntd::string must use ntd::allocator");
    static_assert(std::is_same<typename ntd::unordered_map<int, int>::allocator_type,
                               ntd::allocator<std::pair<const int, int>>>::value,
                  "std-backed ntd::unordered_map must use ntd::allocator");
    static_assert(std::is_same<typename ntd::deque<int>::allocator_type, ntd::allocator<int>>::value,
                  "std-backed ntd::deque must use ntd::allocator");
    static_assert(std::is_same<typename ntd::list<int>::allocator_type, ntd::allocator<int>>::value,
                  "std-backed ntd::list must use ntd::allocator");
    static_assert(std::is_same<typename ntd::set<int>::allocator_type, ntd::allocator<int>>::value,
                  "std-backed ntd::set must use ntd::allocator");
    static_assert(std::is_same<typename ntd::unordered_set<int>::allocator_type, ntd::allocator<int>>::value,
                  "std-backed ntd::unordered_set must use ntd::allocator");
    static_assert(!std::is_same<ntd::vector<int>, std::vector<int>>::value,
                  "allocator-routed ntd containers must be distinct from default std containers");

    #if NTD_EMBEDDED_ALLOC
    const size_t baseline = ntd::mem::BytesInUse();
    {
        ntd::vector<int> vector(128, 1);
        ntd::deque<int> deque;
        ntd::list<int> list;
        ntd::set<int> set;
        ntd::unordered_map<int, int> map;
        ntd::unordered_map<ntd::string, int> stringMap;
        ntd::unordered_set<int> unorderedSet;
        ntd::string string(128, 'x');
        ntd::vector<OverAligned> alignedVector(2);
        ntd::function<int(int)> function = [](int value) { return value + 1; };
        auto unique = ntd::make_unique<Tracked>(9);
        auto shared = ntd::make_shared<Tracked>(10);

        deque.push_back(2);
        list.push_back(3);
        set.insert(4);
        map.emplace(5, 6);
        stringMap.emplace(ntd::string("key"), 6);
        unorderedSet.insert(7);
        CHECK(function(8) == 9 && unique->v == 9 && shared->v == 10 && stringMap.find(ntd::string("key"))->second == 6);
        CHECK(reinterpret_cast<uintptr_t>(alignedVector.data()) % alignof(OverAligned) == 0);
        CHECK(ntd::mem::BytesInUse() > baseline);
    }
    CHECK(ntd::mem::BytesInUse() == baseline);
    #endif
#endif
}

// ---------------------------------------------------------------- pair / tuple
static void test_pair_tuple()
{
    ntd::pair<int, const char*> p(1, "a");
    CHECK(p.first == 1 && std::strcmp(p.second, "a") == 0);

    auto mp = ntd::make_pair(5, 6.0);
    CHECK(mp.first == 5 && mp.second == 6.0);

    ntd::pair<int, int> p2(3, 4);
    p2.first = 30;
    CHECK(p2.first == 30 && p2.second == 4);

    auto t = ntd::make_tuple(1, 2.5, 'c');
    CHECK(ntd::get<0>(t) == 1);
    CHECK(ntd::get<1>(t) == 2.5);
    CHECK(ntd::get<2>(t) == 'c');
    ntd::get<0>(t) = 100;
    CHECK(ntd::get<0>(t) == 100);

    int x = 0;
    double y = 0;
    ntd::tie(x, y) = ntd::make_tuple(7, 8.5);
    CHECK(x == 7 && y == 8.5);
}

// --------------------------------------------------------------- unordered_map
static void test_unordered_map()
{
    ntd::unordered_map<int, int> m;
    CHECK(m.empty() && m.size() == 0);

    m[1] = 10;
    m[2] = 20;
    m[3] = 30;
    CHECK(m.size() == 3 && m[2] == 20);
    m[2] = 25; // operator[] overwrites
    CHECK(m[2] == 25 && m.size() == 3);

    auto ins = m.insert(ntd::pair<const int, int>(4, 40));
    CHECK(ins.second && ins.first->second == 40 && m.size() == 4);
    auto ins2 = m.insert(ntd::pair<const int, int>(4, 99)); // dup: no overwrite
    CHECK(!ins2.second && m[4] == 40);

    auto te = m.try_emplace(5, 50);
    CHECK(te.second && m[5] == 50);
    auto te2 = m.try_emplace(5, 999); // existing: no overwrite
    CHECK(!te2.second && m[5] == 50);

    CHECK(m.at(1) == 10); // at() on present key
    CHECK(m.count(1) == 1 && m.count(999) == 0);

    auto f = m.find(3);
    CHECK(f != m.end() && f->second == 30);
    CHECK(m.find(12345) == m.end());

    auto assigned = m.end();
    assigned = m.begin();
    CHECK(assigned != m.end());
    auto beforeIncrement = assigned++;
    CHECK(beforeIncrement != m.end());

    ntd::unordered_map<ntd::string, int> strings;
    strings.emplace(ntd::string("key"), 42);
    CHECK(strings.find(ntd::string("key")) != strings.end());
    CHECK(strings.find(ntd::string("key"))->second == 42);

    CHECK(m.erase(1) == 1 && m.find(1) == m.end());
    CHECK(m.erase(1) == 0); // already gone

    {
        long keys = 0;
        size_t n = 0;
        for (auto it = m.begin(); it != m.end(); ++it)
        {
            keys += it->first;
            ++n;
        }
        CHECK(n == m.size() && keys == 14);
    }

#if NTD_OWN_STDLIB
    CHECK(m.contains(2) && !m.contains(1)); // contains() is ntd-only under C++17
#endif

    m.clear();
    CHECK(m.empty());

    // value lifetime (mapped Tracked)
    Tracked::reset();
    {
        ntd::unordered_map<int, Tracked> tm;
        for (int i = 0; i < 200; ++i)
            tm.try_emplace(i, Tracked(i));
        CHECK(Tracked::live == 200 && tm.at(150).v == 150);
    }
    CHECK(Tracked::live == 0);

    // differential vs std::unordered_map
    Rng rng(999);
    ntd::unordered_map<int, int> nm;
    std::unordered_map<int, int> sm;
    for (int i = 0; i < 9000; ++i)
    {
        int k = (int)rng(400);
        uint32_t op = rng(3);
        if (op < 2)
        {
            int val = (int)rng(1000000);
            nm[k] = val;
            sm[k] = val;
        }
        else
        {
            nm.erase(k);
            sm.erase(k);
        }
    }
    bool ok = nm.size() == sm.size();
    for (auto& kv : sm)
    {
        auto it = nm.find(kv.first);
        if (it == nm.end() || it->second != kv.second)
        {
            ok = false;
            break;
        }
    }
    for (auto it = nm.begin(); ok && it != nm.end(); ++it)
    {
        auto s = sm.find(it->first);
        if (s == sm.end() || s->second != it->second)
            ok = false;
    }
    CHECK(ok);
}

// --------------------------------------------------------------- unordered_set
static void test_unordered_set()
{
    ntd::unordered_set<int> s;
    CHECK(s.empty());
    auto a = s.insert(5);
    CHECK(a.second && s.size() == 1);
    auto b = s.insert(5); // dup
    CHECK(!b.second && s.size() == 1);
    s.insert(6);
    s.insert(7);
    CHECK(s.size() == 3);
    CHECK(s.find(6) != s.end() && s.find(100) == s.end());
    CHECK(s.count(7) == 1 && s.count(100) == 0);
    CHECK(s.erase(6) == 1 && s.find(6) == s.end() && s.size() == 2);

    {
        size_t n = 0;
        for (auto it = s.begin(); it != s.end(); ++it)
            ++n;
        CHECK(n == s.size());
    }

    s.clear();
    CHECK(s.empty());

    // differential vs std::unordered_set
    Rng rng(31337);
    ntd::unordered_set<int> ns;
    std::unordered_set<int> ss;
    for (int i = 0; i < 9000; ++i)
    {
        int k = (int)rng(500);
        uint32_t op = rng(3);
        if (op < 2)
        {
            ns.insert(k);
            ss.insert(k);
        }
        else
        {
            ns.erase(k);
            ss.erase(k);
        }
    }
    bool ok = ns.size() == ss.size();
    for (int k : ss)
        if (ns.find(k) == ns.end())
        {
            ok = false;
            break;
        }
    CHECK(ok);
}

// ------------------------------------------------------------------------- set
struct Descending
{
    bool operator()(int a, int b) const
    {
        return a > b;
    }
};

struct DecadeLess
{
    bool operator()(int a, int b) const
    {
        return a / 10 < b / 10;
    }
};

struct DirectionalLess
{
    explicit DirectionalLess(bool descending) : descending(descending)
    {
    }
    bool operator()(int a, int b) const
    {
        return descending ? a > b : a < b;
    }
    bool descending;
};

struct CountingLess
{
    explicit CountingLess(size_t* comparisons) : comparisons(comparisons)
    {
    }
    bool operator()(int a, int b) const
    {
        ++*comparisons;
        return a < b;
    }
    size_t* comparisons;
};

struct TrackedLess
{
    bool operator()(const Tracked& a, const Tracked& b) const
    {
        return a.v < b.v;
    }
};

static void test_set()
{
    ntd::set<int> s;
    static_assert(std::is_const<typename std::remove_reference<decltype(*s.begin())>::type>::value,
                  "set iterators must not permit key mutation");
    CHECK(s.empty() && s.size() == 0 && s.begin() == s.end());

    auto a = s.insert(5);
    auto b = s.insert(2);
    auto c = s.insert(8);
    auto duplicate = s.insert(5);
    CHECK(a.second && b.second && c.second && !duplicate.second);
    CHECK(s.size() == 3 && *duplicate.first == 5);
    CHECK(s.find(2) != s.end() && s.find(7) == s.end());
    const ntd::set<int>& const_s = s;
    CHECK(const_s.find(8) != const_s.end() && *const_s.find(8) == 8);
    {
        int expected[] = {2, 5, 8};
        size_t i = 0;
        bool ordered = true;
        for (int value : s)
            ordered = ordered && value == expected[i++];
        CHECK(ordered && i == 3);
    }

    // Copies own their elements independently; moves retain the complete value.
    ntd::set<int> copy = s;
    copy.insert(11);
    CHECK(copy.size() == 4 && s.size() == 3 && s.find(11) == s.end());
    ntd::set<int> moved = ntd::move(copy);
    CHECK(moved.size() == 4 && *moved.begin() == 2 && moved.find(11) != moved.end());

    // The supplied comparator controls both ordering and key equivalence.
    ntd::set<int, Descending> descending;
    descending.insert(5);
    descending.insert(2);
    descending.insert(8);
    {
        int expected[] = {8, 5, 2};
        size_t i = 0;
        bool ordered = true;
        for (int value : descending)
            ordered = ordered && value == expected[i++];
        CHECK(ordered && i == 3);
    }

    ntd::set<int, DirectionalLess> stateful(DirectionalLess(true));
    stateful.insert(1);
    stateful.insert(3);
    stateful.insert(2);
    CHECK(*stateful.begin() == 3);

    ntd::set<int, DecadeLess> decades;
    auto eleven = decades.insert(11);
    auto nineteen = decades.insert(19);
    CHECK(eleven.second && !nineteen.second && decades.size() == 1);
    CHECK(*nineteen.first == 11 && *decades.find(15) == 11);

    // Successful insertion does not invalidate existing set iterators.
    ntd::set<int> stable;
    auto held = stable.insert(100).first;
    for (int i = 0; i < 100; ++i)
        stable.insert(i);
    CHECK(held == stable.find(100));

    // Lookup retains logarithmic comparator complexity.
    size_t comparisons = 0;
    ntd::set<int, CountingLess> counted{CountingLess(&comparisons)};
    for (int i = 0; i < 1024; ++i)
        counted.insert(i * 2);
    comparisons = 0;
    CHECK(counted.find(777) == counted.end());
    CHECK(comparisons <= 16);

    // Elements are destroyed exactly once, including duplicates and clear().
    Tracked::reset();
    {
        ntd::set<Tracked, TrackedLess> tracked;
        tracked.insert(Tracked(1));
        tracked.insert(Tracked(2));
        tracked.insert(Tracked(1));
        CHECK(tracked.size() == 2 && Tracked::live == 2);
        tracked.clear();
        CHECK(tracked.empty() && Tracked::live == 0);
    }
    CHECK(Tracked::live == 0);

    // Differential insertion, lookup, and ordering against std::set.
    Rng rng(8080);
    ntd::set<int> ns;
    std::set<int> ss;
    for (int i = 0; i < 3000; ++i)
    {
        int value = static_cast<int>(rng(500));
        auto nr = ns.insert(value);
        auto sr = ss.insert(value);
        CHECK(nr.second == sr.second && *nr.first == *sr.first);
        int lookup = static_cast<int>(rng(700));
        auto nf = ns.find(lookup);
        auto sf = ss.find(lookup);
        CHECK((nf == ns.end()) == (sf == ss.end()));
        if (nf != ns.end() && sf != ss.end())
            CHECK(*nf == *sf);
    }
    bool same = ns.size() == ss.size();
    auto ni = ns.begin();
    auto si = ss.begin();
    for (; same && ni != ns.end() && si != ss.end(); ++ni, ++si)
        same = *ni == *si;
    CHECK(same && ni == ns.end() && si == ss.end());

    s.clear();
    CHECK(s.empty());
}

// ------------------------------------------------------------------ unique_ptr
static void test_unique_ptr()
{
#if NTD_TYPES_DISTINCT_FROM_STD
    static_assert(!std::is_constructible<ntd::unique_ptr<Tracked>, Tracked*>::value,
                  "default ntd::unique_ptr must reject allocator-ambiguous raw pointers");
#else
    static_assert(std::is_same<ntd::unique_ptr<Tracked>, std::unique_ptr<Tracked>>::value,
                  "scalar ntd::unique_ptr passthrough must preserve the std type");
    static_assert(std::is_same<ntd::default_delete<Tracked>, std::default_delete<Tracked>>::value,
                  "scalar ntd::default_delete passthrough must preserve the std type");
#endif
    Tracked::reset();
    {
        ntd::unique_ptr<Tracked> p = ntd::make_unique<Tracked>(7);
        CHECK((bool)p && p->v == 7 && (*p).v == 7 && p.get() != nullptr);
        CHECK(Tracked::live == 1);

        ntd::unique_ptr<Tracked> q = ntd::move(p); // move transfers ownership
        CHECK(!p && (bool)q && q->v == 7 && Tracked::live == 1);

        Tracked* raw = q.release(); // release: ownership given up, no destruction
        CHECK(!q && raw != nullptr && Tracked::live == 1);
        ntd::unique_ptr<Tracked> reowned;
#if NTD_TYPES_DISTINCT_FROM_STD
        reowned.reset(ntd::adopt_ntd, raw); // explicitly re-adopt ntd::New storage
#else
        reowned.reset(raw);
#endif
        CHECK(reowned->v == 7 && Tracked::live == 1);
    }
    CHECK(Tracked::live == 0); // scope exit destroyed the owned object

    {
        ntd::unique_ptr<Tracked> r = ntd::make_unique<Tracked>(3);
        CHECK(Tracked::live == 1);
        r.reset(); // reset destroys
        CHECK(!r && Tracked::live == 0);

        ntd::unique_ptr<Tracked> s = ntd::make_unique<Tracked>(1);
        CHECK(Tracked::live == 1);
        s = nullptr; // assign nullptr destroys
        CHECK(!s && Tracked::live == 0);
    }
    CHECK(Tracked::live == 0);

    // Converting default-deleter ownership to a same-address base must preserve
    // the original ntd::New allocation address for deallocation.
    UniqueBase::destroyed = 0;
    UniqueDerived::destroyed = 0;
    {
        auto derived = ntd::make_unique<UniqueDerived>();
        UniqueDerived* allocation = derived.get();
        ntd::unique_ptr<UniqueBase> base = ntd::move(derived);
        CHECK(!derived && base.get() == static_cast<UniqueBase*>(allocation));
        CHECK(static_cast<const void*>(base.get()) == static_cast<const void*>(allocation));
    }
    CHECK(UniqueDerived::destroyed == 1 && UniqueBase::destroyed == 1);

#if NTD_OWN_STDLIB && NTD_EMBEDDED_ALLOC
    auto aligned = ntd::make_unique<OverAligned>(17);
    CHECK(reinterpret_cast<uintptr_t>(aligned.get()) % alignof(OverAligned) == 0);
    CHECK(aligned->value == 17);
#endif

    // Array ownership is explicitly rejected in both ntd modes, avoiding an
    // unpaired global delete[] path for an unsupported feature.
}

// ------------------------------------------------------------------ shared_ptr
static void test_shared_ptr()
{
#if NTD_OWN_STDLIB
    static_assert(!std::is_constructible<ntd::shared_ptr<Tracked>, Tracked*>::value,
                  "ntd::shared_ptr must reject allocator-ambiguous raw pointers");
#endif
    Tracked::reset();
    {
        ntd::shared_ptr<Tracked> p = ntd::make_shared<Tracked>(9);
        CHECK((bool)p && p->v == 9 && p.use_count() == 1 && Tracked::live == 1);
        {
            ntd::shared_ptr<Tracked> q = p; // copy shares ownership
            CHECK(q.use_count() == 2 && p.use_count() == 2 && q->v == 9);
            CHECK(q.get() == p.get());
        }
        CHECK(p.use_count() == 1 && Tracked::live == 1); // q gone

        ntd::shared_ptr<Tracked> m = ntd::move(p); // move
        CHECK(!p && m.use_count() == 1 && m->v == 9);

        m.reset();
        CHECK(!m && Tracked::live == 0);
    }
    CHECK(Tracked::live == 0);

#if NTD_OWN_STDLIB && NTD_EMBEDDED_ALLOC
    auto aligned = ntd::make_shared<OverAligned>(23);
    CHECK(reinterpret_cast<uintptr_t>(aligned.get()) % alignof(OverAligned) == 0);
    CHECK(aligned->value == 23);
#endif
}

// -------------------------------------------------------------------- function
static int free_fn(int a, int b)
{
    return a - b;
}
static void test_function()
{
    ntd::function<int(int, int)> f = [](int a, int b) { return a + b; };
    CHECK((bool)f && f(2, 3) == 5);

    f = free_fn; // reassign to a function pointer
    CHECK(f(10, 4) == 6);

    int base = 100;
    ntd::function<int(int)> cap = [base](int x) { return base + x; }; // capturing lambda
    CHECK(cap(5) == 105);

    ntd::function<int(int)> copy = cap; // copyable
    CHECK(copy(7) == 107);

    ntd::function<int(int)> mv = ntd::move(cap);
    CHECK(mv(8) == 108);

    ntd::function<void()> empty;
    CHECK(!(bool)empty);
    empty = nullptr;
    CHECK(!(bool)empty);
}

// ------------------------------------------------------------- hash / equal_to
static void test_hash_equal()
{
    ntd::hash<int> hi;
    CHECK(hi(42) == hi(42)); // deterministic
    size_t h0 = hi(0), h1 = hi(1);
    CHECK(h0 != h1 || true); // (collisions allowed; determinism is the contract)

    int x = 0;
    ntd::hash<int*> hp;
    CHECK(hp(&x) == hp(&x));

    ntd::hash<ntd::string> hs;
    CHECK(hs(ntd::string("key")) == hs(ntd::string("key")));

    ntd::equal_to<int> eq;
    CHECK(eq(5, 5) && !eq(5, 6));

    // usable as a map's hash/eq (already the default) -- sanity that custom instances compare
    ntd::equal_to<int> eq2;
    CHECK(eq2(7, 7));
}

// ------------------------------------------------------------------- algorithm
static void test_algorithm()
{
    CHECK(ntd::min(3, 5) == 3 && ntd::max(3, 5) == 5);
    CHECK(ntd::min(5, 3) == 3 && ntd::max(5, 3) == 5);
    CHECK(ntd::min({4, 2, 8, 1, 9}) == 1);
    CHECK(ntd::max({4, 2, 8, 1, 9}) == 9);

    int sorted[] = {1, 3, 5, 7, 9};
    int* lb = ntd::lower_bound(sorted, sorted + 5, 5);
    CHECK(lb == sorted + 2 && *lb == 5);
    int* lb2 = ntd::lower_bound(sorted, sorted + 5, 6);
    CHECK(lb2 == sorted + 3 && *lb2 == 7);
    int* ub = ntd::upper_bound(sorted, sorted + 5, 5);
    CHECK(ub == sorted + 3 && *ub == 7);

    struct BoundValue { int key; } boundValues[] = {{1}, {3}, {5}, {5}, {7}};
    BoundValue* heterogeneousLower = ntd::lower_bound(
        boundValues, boundValues + 5, 5,
        [](const BoundValue& element, int value) { return element.key < value; });
    BoundValue* heterogeneousUpper = ntd::upper_bound(
        boundValues, boundValues + 5, 5,
        [](int value, const BoundValue& element) { return value < element.key; });
    CHECK(heterogeneousLower == boundValues + 2);
    CHECK(heterogeneousUpper == boundValues + 4);

    int values[] = {9, 1, 8, 2, 7, 3, 6, 4, 5, 0};
    ntd::sort(values, values + 10);
    for (int i = 0; i < 10; ++i)
        CHECK(values[i] == i);

    ntd::sort(values, values + 10, [](int a, int b) { return a > b; });
    for (int i = 0; i < 10; ++i)
        CHECK(values[i] == 9 - i);

    int duplicateValues[256];
    int duplicateCounts[32] = {};
    uint32_t randomState = 0x12345678u;
    for (int& value : duplicateValues)
    {
        randomState = randomState * 1664525u + 1013904223u;
        value = static_cast<int>((randomState >> 16) & 31u);
        duplicateCounts[value]++;
    }
    ntd::sort(duplicateValues, duplicateValues + 256);
    for (size_t i = 0; i < 256; ++i)
    {
        if (i != 0)
            CHECK(duplicateValues[i - 1] <= duplicateValues[i]);
        duplicateCounts[duplicateValues[i]]--;
    }
    for (int count : duplicateCounts)
        CHECK(count == 0);

    ntd::unique_ptr<int> moveOnlyValues[32];
    for (int i = 0; i < 32; ++i)
        moveOnlyValues[i] = ntd::make_unique<int>(31 - i);
    ntd::sort(
        moveOnlyValues, moveOnlyValues + 32,
        [](const auto& a, const auto& b) { return *a < *b; });
    for (int i = 0; i < 32; ++i)
        CHECK(*moveOnlyValues[i] == i);

    int emptyRangeValue = 0;
    ntd::sort(&emptyRangeValue, &emptyRangeValue);

    int src[] = {1, 2, 3, 4};
    int dst[4] = {0, 0, 0, 0};
    ntd::copy(src, src + 4, dst);
    CHECK(dst[0] == 1 && dst[3] == 4);

    int buf[5];
    ntd::fill(buf, buf + 5, 9);
    CHECK(buf[0] == 9 && buf[4] == 9);
    int buf2[5] = {0};
    ntd::fill_n(buf2, 3, 7);
    CHECK(buf2[0] == 7 && buf2[2] == 7 && buf2[3] == 0);

    int rev[] = {1, 2, 3, 4, 5};
    ntd::reverse(rev, rev + 5);
    CHECK(rev[0] == 5 && rev[4] == 1);

    int in[] = {1, 2, 3};
    int out[3];
    ntd::transform(in, in + 3, out, [](int v) { return v * v; });
    CHECK(out[0] == 1 && out[1] == 4 && out[2] == 9);
}

// --------------------------------------------------------------------- cstring
static void test_cstring()
{
    char dst[8] = {0};
    ntd::memcpy(dst, "abcd", 5);
    CHECK(std::strcmp(dst, "abcd") == 0);

    char m[8] = "aXXXXb";
    ntd::memmove(m + 1, m, 3); // overlapping move
    CHECK(m[0] == 'a' && m[1] == 'a' && m[2] == 'X' && m[3] == 'X');

    char z[4];
    ntd::memset(z, 0, 4);
    CHECK(z[0] == 0 && z[3] == 0);

    CHECK(ntd::memcmp("abc", "abc", 3) == 0);
    CHECK(ntd::memcmp("abc", "abd", 3) < 0);
    CHECK(ntd::memcmp("abd", "abc", 3) > 0);

    CHECK(ntd::strlen("") == 0 && ntd::strlen("hello") == 5);
    char boundedHello[8] = "hello";
    CHECK(ntd::strnlen(boundedHello, sizeof(boundedHello)) == 5);
    CHECK(ntd::strnlen(boundedHello, 3) == 3);
    CHECK(ntd::strnlen(boundedHello, 0) == 0);
}

// -------------------------------------------------------------- utility (swap)
static void test_utility()
{
    int a = 1, b = 2;
    ntd::swap(a, b);
    CHECK(a == 2 && b == 1);

    ntd::vector<int> v1{1, 2, 3}, v2{9};
    ntd::swap(v1, v2);
    CHECK(v1.size() == 1 && v1[0] == 9 && v2.size() == 3 && v2[2] == 3);

    // std::array's swap overload may be declared after ntd's utility header.
    // Keep that late-ADL case covered, including routed standard containers.
    ntd::array<ntd::unordered_set<int>, 2> sets1, sets2;
    sets1[0].insert(4);
    sets2[0].insert(7);
    ntd::swap(sets1, sets2);
    CHECK(sets1[0].count(7) == 1 && sets2[0].count(4) == 1);
}

// -------------------------------------------------------------------- file I/O
static void test_file_io()
{
    const char* path = "ntd_file_io_test.tmp";
    {
        ntd::ofstream out;
        out.open(path, ntd::ios::out | ntd::ios::binary | ntd::ios::trunc);
        CHECK(out.is_open() && out.good());
        out.write("abcdef", 6);
        CHECK(out.good());
        out.close();
        CHECK(!out.is_open() && !out.fail());
    }
    {
        ntd::ifstream in;
        in.open(path, ntd::ios::in | ntd::ios::binary | ntd::ios::ate);
        CHECK(in.is_open() && in.tellg() == 6);
        in.seekg(4, ntd::ios::beg);
        char buf[8] = {};
        in.read(buf, 4); // only two bytes remain
        CHECK(in.gcount() == 2 && in.fail());
        CHECK(buf[0] == 'e' && buf[1] == 'f');
        in.clear();
        in.seekg(0, ntd::ios::beg);
        in.read(buf, 6);
        CHECK(in.gcount() == 6 && in.good());
        CHECK(ntd::memcmp(buf, "abcdef", 6) == 0);
        in.close();
        CHECK(!in.is_open() && !in.fail());
    }
    std::remove(path);
}

int main()
{
    test_install_host_allocator();
    std::printf("ntd_unit_tests: %s\n",
#if NTD_OWN_STDLIB
                "embedded ntd:: (NTD_OWN_STDLIB=1)"
#elif NTD_ROUTED_STD_TYPES
                "std implementations with ntd::allocator"
#else
                "std:: passthrough (NTD_OWN_STDLIB=0) -- validating the test oracle"
#endif
    );
    test_vector();
    test_deque();
    test_list();
    test_array();
    test_string();
    test_string_view();
    test_optional();
    test_std_backed_allocator_routing();
    test_pair_tuple();
    test_unordered_map();
    test_unordered_set();
    test_set();
    test_unique_ptr();
    test_shared_ptr();
    test_function();
    test_hash_equal();
    test_algorithm();
    test_cstring();
    test_utility();
    test_file_io();

    std::printf("ntd_unit_tests: %d checks, %d failures\n", g_checks, g_fail);
    if (g_fail == 0)
        std::printf("NTD UNIT TESTS OK\n");
    return g_fail == 0 ? 0 : 1;
}
