#include "cachesim/cache.hpp"
#include "test_framework.hpp"

using namespace cachesim;

namespace {
LevelConfig make(std::uint64_t size, std::uint64_t block, std::uint64_t assoc, PolicyKind p = PolicyKind::LRU) {
    LevelConfig c;
    c.size_bytes = size;
    c.block_bytes = block;
    c.assoc = assoc;
    c.policy = p;
    return c;
}
}  // namespace

TEST(geometry_matches_the_original_program) {
    // Same numbers the original printed for 32768 / 64 / 4.
    CacheLevel L(make(32768, 64, 4), 1);
    const Geometry& g = L.geometry();
    CHECK_EQ(g.sets, 128u);
    CHECK_EQ(g.ways, 4u);
    CHECK_EQ(g.offset_bits, 6u);
    CHECK_EQ(g.index_bits, 7u);
    CHECK_EQ(g.tag_bits(32), 19u);
    CHECK(g.pow2_sets);
}

TEST(address_split_power_of_two_sets) {
    CacheLevel L(make(32768, 64, 4), 1);
    const std::uint64_t addr = 0x018ADB20;
    const std::uint64_t block = L.block_of(addr);
    CHECK_EQ(block, addr >> 6);
    CHECK_EQ(L.set_of(block), (addr >> 6) & 127u);
    CHECK_EQ(L.tag_of(block), addr >> 13);
}

TEST(address_split_non_power_of_two_sets) {
    // 160 KiB / 1 KiB / 8-way = 20 sets. The original used only 16 of them.
    CacheLevel L(make(163840, 1024, 8), 1);
    CHECK_EQ(L.geometry().sets, 20u);
    CHECK(!L.geometry().pow2_sets);
    CHECK_EQ(L.geometry().index_bits, 5u);
    for (std::uint64_t b = 0; b < 100; ++b) {
        CHECK_EQ(L.set_of(b), b % 20);
        CHECK_EQ(L.tag_of(b), b / 20);
    }
    // All 20 sets are reachable.
    for (std::uint64_t b = 16; b < 20; ++b) {
        const auto r = L.access(b, false);
        CHECK_EQ(r.set, b);
    }
}

TEST(hit_miss_and_fill_order) {
    CacheLevel L(make(256, 64, 2), 1);  // 2 sets x 2 ways
    auto r = L.access(0, false);
    CHECK(!r.hit);
    CHECK(r.filled);
    CHECK_EQ(r.way, 0u);
    r = L.access(2, false);  // same set 0
    CHECK(!r.hit);
    CHECK_EQ(r.way, 1u);
    CHECK(!r.evicted);
    r = L.access(0, false);
    CHECK(r.hit);
    CHECK_EQ(r.way, 0u);
    r = L.access(4, false);  // set 0 full: evict LRU = block 2
    CHECK(r.evicted);
    CHECK_EQ(r.evicted_block, 2u);
    CHECK(L.contains(0));
    CHECK(L.contains(4));
    CHECK(!L.contains(2));
    CHECK_EQ(L.set_contents(1).size(), 0u);
}

TEST(write_back_marks_dirty_and_reports_dirty_victims) {
    CacheLevel L(make(64, 64, 1), 1);  // a single line
    L.access(1, true);
    CHECK(L.is_dirty(1));
    auto r = L.access(2, false);
    CHECK(r.evicted);
    CHECK(r.evicted_dirty);
    CHECK_EQ(r.evicted_block, 1u);
    CHECK(!L.is_dirty(2));
    r = L.access(3, false);
    CHECK(r.evicted);
    CHECK(!r.evicted_dirty);  // clean victim, no write-back
}

TEST(write_through_never_dirty) {
    LevelConfig c = make(64, 64, 1);
    c.write_policy = WritePolicy::WriteThrough;
    CacheLevel L(c, 1);
    L.access(1, true);
    CHECK(L.contains(1));  // write-allocate is still on
    CHECK(!L.is_dirty(1));
    CHECK(!L.access(2, false).evicted_dirty);
}

TEST(no_write_allocate_store_miss_bypasses) {
    LevelConfig c = make(128, 64, 2);
    c.alloc_policy = AllocPolicy::NoWriteAllocate;
    CacheLevel L(c, 1);
    auto r = L.access(5, true);
    CHECK(!r.hit);
    CHECK(!r.filled);
    CHECK(!L.contains(5));
    L.access(5, false);  // a load allocates
    r = L.access(5, true);  // now a store hits and dirties the line
    CHECK(r.hit);
    CHECK(L.is_dirty(5));
}

TEST(fully_associative_uses_hash_index) {
    // 256 ways > kIndexThreshold, so lookups go through the hash index.
    CacheLevel L(make(256 * 64, 64, 0), 1);
    CHECK_EQ(L.geometry().sets, 1u);
    CHECK_EQ(L.geometry().ways, 256u);
    for (std::uint64_t b = 0; b < 256; ++b) CHECK(!L.access(b * 977, false).hit);
    for (std::uint64_t b = 0; b < 256; ++b) CHECK(L.access(b * 977, false).hit);
    const auto r = L.access(999999, false);  // evicts block 0 (LRU after the re-touch pass)
    CHECK(r.evicted);
    CHECK_EQ(r.evicted_block, 0u);
    CHECK(!L.contains(0));
    CHECK(L.contains(999999));
}
