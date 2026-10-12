// Replacement policies checked against hand-worked and textbook examples.
#include "cachesim/simulator.hpp"
#include "test_framework.hpp"

#include <vector>

using namespace cachesim;

namespace {

Trace reads(const std::vector<std::uint64_t>& addrs) {
    Trace t;
    for (auto a : addrs) t.accesses.push_back({a, 1, Op::Read});
    return t;
}

// A fully associative cache of `frames` one-byte blocks behaves exactly like
// page replacement with `frames` page frames: block number = page number.
std::uint64_t page_faults(const std::vector<std::uint64_t>& refs, std::uint64_t frames, PolicyKind p) {
    SimConfig c;
    c.levels[0].size_bytes = frames;
    c.levels[0].block_bytes = 1;
    c.levels[0].assoc = 0;
    c.levels[0].policy = p;
    Simulator sim(c);
    sim.run(reads(refs));
    return sim.stats().levels[0].misses();
}

// Reference string from Silberschatz, Galvin and Gagne, "Operating System
// Concepts", chapter 9 (virtual memory), with 3 frames.
const std::vector<std::uint64_t> kSilberschatz = {7, 0, 1, 2, 0, 3, 0, 4, 2, 3, 0, 3, 2, 1, 2, 0, 1, 7, 0, 1};

}  // namespace

TEST(textbook_fifo_15_faults) { CHECK_EQ(page_faults(kSilberschatz, 3, PolicyKind::FIFO), 15u); }
TEST(textbook_lru_12_faults) { CHECK_EQ(page_faults(kSilberschatz, 3, PolicyKind::LRU), 12u); }
TEST(textbook_opt_9_faults) { CHECK_EQ(page_faults(kSilberschatz, 3, PolicyKind::OPT), 9u); }

TEST(beladys_anomaly_for_fifo) {
    // More frames, more faults: the classic FIFO anomaly.
    const std::vector<std::uint64_t> refs = {1, 2, 3, 4, 1, 2, 5, 1, 2, 3, 4, 5};
    CHECK_EQ(page_faults(refs, 3, PolicyKind::FIFO), 9u);
    CHECK_EQ(page_faults(refs, 4, PolicyKind::FIFO), 10u);
    // LRU is a stack algorithm, so it never shows the anomaly.
    CHECK(page_faults(refs, 4, PolicyKind::LRU) <= page_faults(refs, 3, PolicyKind::LRU));
}

TEST(opt_is_never_worse_than_other_policies) {
    XorShift64 rng(99);
    for (int round = 0; round < 30; ++round) {
        std::vector<std::uint64_t> refs;
        for (int i = 0; i < 300; ++i) refs.push_back(rng.below(12));
        for (std::uint64_t frames : {2u, 3u, 4u, 8u}) {
            const auto opt = page_faults(refs, frames, PolicyKind::OPT);
            CHECK(opt <= page_faults(refs, frames, PolicyKind::LRU));
            CHECK(opt <= page_faults(refs, frames, PolicyKind::FIFO));
            CHECK(opt <= page_faults(refs, frames, PolicyKind::Random));
            CHECK(opt <= page_faults(refs, frames, PolicyKind::SRRIP));
            if (frames == 2 || frames == 4 || frames == 8) CHECK(opt <= page_faults(refs, frames, PolicyKind::PLRU));
        }
    }
}

namespace {
// One set of 4 ways with 1-byte blocks: block number == address.
CacheLevel four_way(PolicyKind p) {
    LevelConfig c;
    c.size_bytes = 4;
    c.block_bytes = 1;
    c.assoc = 4;
    c.policy = p;
    return CacheLevel(c, 1);
}
constexpr std::uint64_t A = 0xA, B = 0xB, C = 0xC, D = 0xD, E = 0xE;
}  // namespace

TEST(lru_list_order) {
    CacheLevel L = four_way(PolicyKind::LRU);
    for (auto x : {A, B, C, D}) L.access(x, false);
    CHECK_EQ(L.describe_policy_state(0), std::string("MRU 3 2 1 0 LRU"));
    L.access(A, false);  // A (way 0) becomes MRU
    CHECK_EQ(L.describe_policy_state(0), std::string("MRU 0 3 2 1 LRU"));
    const auto r = L.access(E, false);
    CHECK(r.evicted);
    CHECK_EQ(r.evicted_block, B);  // least recently used
}

TEST(fifo_ignores_hits) {
    CacheLevel L = four_way(PolicyKind::FIFO);
    for (auto x : {A, B, C, D}) L.access(x, false);
    L.access(A, false);  // a hit does not refresh A under FIFO
    const auto r = L.access(E, false);
    CHECK_EQ(r.evicted_block, A);
}

TEST(tree_plru_differs_from_lru) {
    // Fill A,B,C,D then touch A. True LRU would evict B. Tree-PLRU's root
    // bit now points right (away from A), and the right subtree's bit
    // points to C, so PLRU evicts C. Worked by hand in docs/DESIGN.md.
    CacheLevel L = four_way(PolicyKind::PLRU);
    for (auto x : {A, B, C, D}) L.access(x, false);
    CHECK_EQ(L.describe_policy_state(0), std::string("tree bits 000"));
    L.access(A, false);
    CHECK_EQ(L.describe_policy_state(0), std::string("tree bits 110"));
    const auto r = L.access(E, false);
    CHECK_EQ(r.evicted_block, C);
    CHECK(L.contains(B));
}

TEST(srrip_ages_then_evicts) {
    CacheLevel L = four_way(PolicyKind::SRRIP);
    for (auto x : {A, B, C, D}) L.access(x, false);
    CHECK_EQ(L.describe_policy_state(0), std::string("rrpv 2 2 2 2"));
    L.access(A, false);  // hit -> RRPV 0
    CHECK_EQ(L.describe_policy_state(0), std::string("rrpv 0 2 2 2"));
    const auto r = L.access(E, false);  // no RRPV 3 -> age all -> evict first 3 (B)
    CHECK_EQ(r.evicted_block, B);
    CHECK_EQ(L.describe_policy_state(0), std::string("rrpv 1 2 3 3"));
}

TEST(srrip_resists_a_scan) {
    // Hot blocks re-used between scan elements stay cached under SRRIP but
    // are flushed by LRU when the scan is longer than the cache.
    std::vector<std::uint64_t> refs;
    for (int rep = 0; rep < 50; ++rep) {
        refs.push_back(1);
        refs.push_back(2);
        refs.push_back(1);
        refs.push_back(2);
        for (std::uint64_t s = 0; s < 4; ++s) refs.push_back(100 + rep * 4 + s);  // never reused
    }
    CHECK(page_faults(refs, 4, PolicyKind::SRRIP) < page_faults(refs, 4, PolicyKind::LRU));
}

TEST(random_is_deterministic_per_seed) {
    std::vector<std::uint64_t> refs;
    XorShift64 rng(5);
    for (int i = 0; i < 2000; ++i) refs.push_back(rng.below(64));
    auto run = [&](std::uint64_t seed) {
        SimConfig c;
        c.levels[0] = LevelConfig{"L1", 16, 1, 4, PolicyKind::Random};
        c.seed = seed;
        Simulator sim(c);
        sim.run(reads(refs));
        return sim.stats().levels[0].misses();
    };
    CHECK_EQ(run(1), run(1));
    CHECK_EQ(run(2), run(2));
    bool any_diff = false;
    for (std::uint64_t s = 2; s < 10 && !any_diff; ++s) any_diff = run(1) != run(s);
    CHECK(any_diff);
}

TEST(all_policies_agree_when_direct_mapped) {
    // With one way there is no choice to make.
    std::vector<std::uint64_t> refs;
    XorShift64 rng(11);
    for (int i = 0; i < 5000; ++i) refs.push_back(rng.below(4096));
    std::uint64_t expected = 0;
    bool first = true;
    for (PolicyKind p : {PolicyKind::LRU, PolicyKind::FIFO, PolicyKind::Random, PolicyKind::PLRU, PolicyKind::SRRIP,
                         PolicyKind::OPT}) {
        SimConfig c;
        c.levels[0] = LevelConfig{"L1", 1024, 16, 1, p};
        Simulator sim(c);
        sim.run(reads(refs));
        if (first) expected = sim.stats().levels[0].misses();
        CHECK_EQ(sim.stats().levels[0].misses(), expected);
        first = false;
    }
}
