// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

// Smoke tests for the ntd wrappers + allocator. They use only ntd:: names and
// therefore also specify the surface reserved for NTD_OWN_STDLIB. Embedded-
// allocator invariants (leak check) run only when the allocator is enabled.
//
// Built as CMake target `ntd_smoke_tests`; returns non-zero on any failure.

#include "ntd_stdlib/ntd.h"
#include "test_host_allocator.h"

#include <cstdint>
#include <cstdio>

namespace
{
int g_fails = 0;
int g_live = 0;

struct Obj
{
    int v;
    Obj() : v(0)
    {
        ++g_live;
    }
    Obj(int x) : v(x)
    {
        ++g_live;
    }
    Obj(const Obj& o) : v(o.v)
    {
        ++g_live;
    }
    Obj(Obj&& o) noexcept : v(o.v)
    {
        ++g_live;
    }
    Obj& operator=(const Obj&) = default;
    Obj& operator=(Obj&&) = default;
    ~Obj()
    {
        --g_live;
    }
};
} // namespace

#define CHECK(c)                                                                                                       \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(c))                                                                                                      \
        {                                                                                                              \
            std::printf("FAIL %d: %s\n", __LINE__, #c);                                                                \
            ++g_fails;                                                                                                 \
        }                                                                                                              \
    } while (0)

int main()
{
    // In setter-based external modes, install the host allocator before anything
    // below allocates. No-op otherwise (link-time hooks / embedded / default).
    test_install_host_allocator();
    {
        // vector: growth, resize, copy/move, non-trivial destruction
        ntd::vector<int> v;
        for (int i = 0; i < 5000; ++i)
            v.push_back(i);
        CHECK(v.size() == 5000 && v[4999] == 4999 && v.back() == 4999);
        v.resize(10);
        CHECK(v.size() == 10 && v[9] == 9);
        v.assign(4, 7);
        CHECK(v.size() == 4 && v[3] == 7);
        {
            ntd::vector<Obj> ov;
            for (int i = 0; i < 200; ++i)
                ov.emplace_back(i);
            CHECK(ov.size() == 200 && ov[199].v == 199 && g_live == 200);
        }
        CHECK(g_live == 0);

        // unordered_map: emplace, find, erase, operator[], piecewise + move value
        ntd::unordered_map<uint32_t, int> m;
        for (uint32_t i = 0; i < 5000; ++i)
            m.emplace(i, (int)i);
        CHECK(m.size() == 5000 && m.find(1234)->second == 1234 && m.find(99999) == m.end());
        CHECK(m.erase(1234) == 1 && m.find(1234) == m.end());
        m[7] = 70;
        CHECK(m[7] == 70);
        ntd::unordered_map<uint32_t, ntd::vector<int>> mv;
        {
            ntd::vector<int> tmp;
            tmp.push_back(9);
            auto r =
                mv.emplace(ntd::piecewise_construct, ntd::forward_as_tuple(3u), ntd::forward_as_tuple(ntd::move(tmp)));
            CHECK(r.second && r.first->second[0] == 9);
        }

        // unordered_set: insert (structured binding), find, erase, iterate
        ntd::unordered_set<uint32_t> s;
        for (uint32_t i = 0; i < 3000; ++i)
        {
            auto [it, ins] = s.insert(i);
            (void)it;
            CHECK(ins);
        }
        CHECK(s.size() == 3000 && s.find(2999) != s.end() && s.erase(0) == 1);

        // deque: stable addresses across growth + resize + iterate
        ntd::deque<int> dq;
        dq.push_back(0);
        int* p0 = &dq[0];
        for (int i = 1; i < 2000; ++i)
            dq.push_back(i);
        CHECK(&dq[0] == p0 && dq.size() == 2000 && dq[1999] == 1999);

        // list: LRU idiom (push_front / splice-to-front / reverse-iterator erase)
        ntd::list<int> l;
        l.push_front(1);
        l.push_front(2);
        l.push_front(3);
        auto it = ntd::next(l.begin(), 2);
        l.splice(l.begin(), l, it);
        CHECK(l.front() == 1);
        auto rit = l.rbegin();
        l.erase(ntd::next(rit).base());
        CHECK(l.size() == 2);

        // optional / smart ptrs / function / pair / tuple
        ntd::optional<int> o;
        CHECK(!o);
        o = 5;
        CHECK(o && *o == 5);
        auto up = ntd::make_unique<Obj>(11);
        CHECK(up->v == 11 && g_live == 1);
        up.reset();
        CHECK(g_live == 0);
        auto sp = ntd::make_shared<int>(4);
        auto sp2 = sp;
        CHECK(sp.use_count() == 2 && *sp2 == 4);
        ntd::function<int(int)> f = [](int x) { return x * 2; };
        CHECK(f && f(21) == 42);
        auto pr = ntd::make_pair(1, 2);
        auto tp = ntd::make_tuple(1, 2, 3);
        CHECK(pr.first == 1 && ntd::get<2>(tp) == 3);

        // string: ctor, assign(ptr,len) [the FlatBuffers UnPack path], operator=, append.
        {
            ntd::string str("abc");
            CHECK(str.size() == 3 && str.length() == 3 && !str.empty() && str == ntd::string("abc"));
            str.assign("hello world", 5); // length-limited: FB unpacks strings via assign(ptr, len)
            CHECK(str.size() == 5 && str == ntd::string("hello"));
            const char emb[3] = {'a', '\0', 'b'};
            str.assign(emb, 3); // length-preserving (copies the embedded NUL, not strlen)
            CHECK(str.size() == 3 && str[0] == 'a' && str[1] == '\0' && str[2] == 'b' && str == ntd::string(emb, 3));
            ntd::string str2;
            str2 = str; // copy operator=
            CHECK(str2 == str && str2.size() == 3);
            str2 = "xyz"; // const char* operator=
            CHECK(str2 == ntd::string("xyz"));
            ntd::string str3;
            str3 = ntd::move(str2); // move operator=
            CHECK(str3 == ntd::string("xyz"));
            str3 += "!"; // append
            CHECK(str3 == ntd::string("xyz!"));
        }
    }

#if NTD_EMBEDDED_ALLOC
    CHECK(ntd::mem::BytesInUse() == 0);                         // everything above freed
    CHECK(ntd::mem::ReservedBytes() >= ntd::mem::BytesInUse()); // OS reservation always covers in-use
    std::printf("ntd_smoke_tests: embedded heap peak %.1f KB, reserved %.1f KB\n", ntd::mem::PeakBytesInUse() / 1024.0,
                ntd::mem::ReservedBytes() / 1024.0);
#endif
    std::printf("ntd_smoke_tests: %d failures\n", g_fails);
    std::printf(g_fails ? "NTD SMOKE TESTS FAILED\n" : "NTD SMOKE TESTS OK\n");
    return g_fails ? 1 : 0;
}
