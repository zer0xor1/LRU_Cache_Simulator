// Regression against the original program's output on traces/example.trc.
//
// The expected numbers were produced by compiling the unmodified original
// code (legacy/original, g++ -O2) and running it on each configuration.
// For every configuration where the original was correct, the new engine
// must reproduce its hit and miss counts exactly.
#include <fstream>

#include "cachesim/simulator.hpp"
#include "test_framework.hpp"

using namespace cachesim;

namespace {

const Trace* example_trace() {
    static const Trace* t = []() -> const Trace* {
        const std::string path = std::string(CACHESIM_SOURCE_DIR) + "/traces/example.trc";
        std::ifstream probe(path);
        if (!probe) return nullptr;
        return new Trace(load_trace(path));
    }();
    return t;
}

struct Case {
    std::uint64_t size, block, assoc, hits, misses;
};

// hits/misses printed by the original program.
constexpr Case kOriginal[] = {
    {32768, 64, 4, 594142, 101379},   // README sample run 1
    {32768, 64, 2, 593740, 101781},   // README sample run 2
    {16384, 64, 2, 592777, 102744},   // README sample run 3
    {16384, 64, 8, 593285, 102236},   // README sample run 4
    {4096, 32, 4, 524821, 170700},    // Capture.PNG (see below)
    {131072, 1024, 8, 651224, 44297}, // cmd_screenshot.PNG (see below)
    {1024, 64, 1, 585298, 110223},    // direct mapped
    {4096, 64, 64, 591243, 104278},   // fully associative, 64 ways
};

std::uint64_t misses_for(std::uint64_t size, std::uint64_t block, std::uint64_t assoc) {
    SimConfig c;
    c.levels[0] = LevelConfig{"L1", size, block, assoc, PolicyKind::LRU};
    Simulator sim(c);
    sim.run(*example_trace());
    return sim.stats().levels[0].misses();
}

}  // namespace

TEST(regression_matches_original_lru_results) {
    const Trace* t = example_trace();
    if (!t) {
        std::cout << "    (traces/example.trc not found, skipping)\n";
        return;
    }
    REQUIRE(t->accesses.size() == 695521);
    for (const Case& k : kOriginal) {
        SimConfig c;
        c.levels[0] = LevelConfig{"L1", k.size, k.block, k.assoc, PolicyKind::LRU};
        Simulator sim(c);
        sim.run(*t);
        CHECK_EQ(sim.stats().levels[0].hits(), k.hits);
        CHECK_EQ(sim.stats().levels[0].misses(), k.misses);
    }
}

TEST(regression_documents_original_set_index_bug) {
    // The original computed index bits as floor(log2(sets)), so a cache with
    // a non-power-of-two set count silently used fewer sets:
    //   * 8096 B / 32 B / 4-way (Capture.PNG): 63 sets, only 32 used,
    //     so it behaved exactly like a 4096 B cache.
    //   * 163840 B / 1024 B / 8-way (example.cfg): 20 sets, only 16 used,
    //     so it behaved exactly like a 131072 B cache (44,297 misses).
    // The new engine indexes with block mod sets and uses all 20 sets.
    if (!example_trace()) return;
    const std::uint64_t fixed = misses_for(163840, 1024, 8);
    CHECK_EQ(fixed, 42189u);
    CHECK(fixed < 44297u);
    // 8096 is not a multiple of 32 x 4 at all; the new engine refuses it.
    SimConfig bad;
    bad.levels[0] = LevelConfig{"L1", 8096, 32, 4, PolicyKind::LRU};
    CHECK_THROWS(validate(bad));
}

TEST(regression_policy_ordering_on_example_trace) {
    if (!example_trace()) return;
    // OPT is a lower bound for every other policy on the same geometry.
    std::uint64_t opt = 0;
    {
        SimConfig c;
        c.levels[0] = LevelConfig{"L1", 8192, 64, 4, PolicyKind::OPT};
        Simulator sim(c);
        sim.run(*example_trace());
        opt = sim.stats().levels[0].misses();
    }
    for (PolicyKind p : {PolicyKind::LRU, PolicyKind::FIFO, PolicyKind::Random, PolicyKind::PLRU, PolicyKind::SRRIP}) {
        SimConfig c;
        c.levels[0] = LevelConfig{"L1", 8192, 64, 4, p};
        Simulator sim(c);
        sim.run(*example_trace());
        CHECK(opt <= sim.stats().levels[0].misses());
    }
}
