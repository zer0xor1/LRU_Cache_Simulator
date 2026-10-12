#include <unordered_map>

#include "cachesim/flat_hash_map.hpp"
#include "cachesim/policy.hpp"  // XorShift64
#include "test_framework.hpp"

using namespace cachesim;

TEST(flat_map_basic_operations) {
    FlatHashMap<std::uint32_t> m;
    CHECK(m.empty());
    CHECK(m.find(7) == nullptr);
    CHECK(!m.erase(7));
    CHECK(m.insert(7, 70));
    CHECK(!m.insert(7, 71));  // insert keeps the old value
    REQUIRE(m.find(7) != nullptr);
    CHECK_EQ(*m.find(7), 70u);
    CHECK(!m.insert_or_assign(7, 72));
    CHECK_EQ(*m.find(7), 72u);
    CHECK(m.insert_or_assign(0, 1));  // key 0 is an ordinary key
    CHECK(m.insert_or_assign(UINT64_MAX, 2));
    CHECK_EQ(m.size(), 3u);
    CHECK(m.erase(7));
    CHECK(!m.contains(7));
    CHECK(m.contains(0));
    CHECK(m.contains(UINT64_MAX));
    CHECK_EQ(m.size(), 2u);
}

// Many keys that collide in a tiny table exercise wrap-around probing and
// backward-shift deletion.
TEST(flat_map_matches_unordered_map_under_random_ops) {
    for (std::uint64_t seed = 1; seed <= 20; ++seed) {
        XorShift64 rng(seed);
        FlatHashMap<std::uint64_t> m;
        std::unordered_map<std::uint64_t, std::uint64_t> ref;
        const std::uint64_t key_space = 8 + rng.below(200);  // small space -> lots of hits/erases
        for (int step = 0; step < 4000; ++step) {
            const std::uint64_t k = rng.below(key_space) * 0x10000;  // clustered keys
            switch (rng.below(3)) {
                case 0: {
                    const std::uint64_t v = rng.next();
                    CHECK_EQ(m.insert_or_assign(k, v), ref.count(k) == 0);
                    ref[k] = v;
                    break;
                }
                case 1:
                    CHECK_EQ(m.erase(k), ref.erase(k) == 1);
                    break;
                default: {
                    const auto* got = m.find(k);
                    const auto it = ref.find(k);
                    CHECK_EQ(got != nullptr, it != ref.end());
                    if (got && it != ref.end()) CHECK_EQ(*got, it->second);
                }
            }
            CHECK_EQ(m.size(), ref.size());
        }
        // Final full comparison.
        for (const auto& [k, v] : ref) {
            const auto* got = m.find(k);
            REQUIRE(got != nullptr);
            CHECK_EQ(*got, v);
        }
    }
}

TEST(flat_map_reserve_keeps_contents) {
    FlatHashMap<std::uint32_t> m;
    for (std::uint32_t i = 0; i < 1000; ++i) m.insert(i * 7919ULL, i);
    m.reserve(100000);
    for (std::uint32_t i = 0; i < 1000; ++i) {
        const auto* v = m.find(i * 7919ULL);
        REQUIRE(v != nullptr);
        CHECK_EQ(*v, i);
    }
    m.clear();
    CHECK(m.empty());
    CHECK(!m.contains(0));
}
