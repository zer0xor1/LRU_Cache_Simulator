#include <set>

#include "cachesim/simulator.hpp"
#include "test_framework.hpp"

using namespace cachesim;

namespace {

SimConfig single(std::uint64_t size, std::uint64_t block, std::uint64_t assoc, PolicyKind p = PolicyKind::LRU) {
    SimConfig c;
    c.levels[0] = LevelConfig{"L1", size, block, assoc, p};
    c.levels[0].hit_latency = 4;
    c.memory_latency = 100;
    return c;
}

SimConfig two_level() {
    SimConfig c;
    c.levels.clear();
    c.levels.push_back(LevelConfig{"L1", 512, 64, 2, PolicyKind::LRU});
    c.levels.push_back(LevelConfig{"L2", 4096, 64, 4, PolicyKind::LRU});
    c.levels[0].hit_latency = 2;
    c.levels[1].hit_latency = 10;
    c.memory_latency = 100;
    return c;
}

Trace random_trace(std::uint64_t seed, std::size_t n, std::uint64_t span, double write_frac) {
    XorShift64 rng(seed);
    Trace t;
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint64_t addr = rng.below(span) * 8;
        const bool w = static_cast<double>(rng.below(1000)) < write_frac * 1000.0;
        t.accesses.push_back({addr, 8, w ? Op::Write : Op::Read});
    }
    return t;
}

}  // namespace

TEST(accesses_crossing_a_line_are_split) {
    Simulator sim(single(1024, 64, 2));
    Trace t;
    t.accesses.push_back({0x3C, 8, Op::Read});   // bytes 0x3C..0x43 span two lines
    t.accesses.push_back({0x40, 8, Op::Read});   // inside the second line
    t.accesses.push_back({0x80, 256, Op::Read}); // four whole lines
    sim.run(t);
    CHECK_EQ(sim.stats().records, 3u);
    CHECK_EQ(sim.stats().cpu_accesses, 2u + 1u + 4u);
    CHECK_EQ(sim.stats().split_records, 2u);
    CHECK_EQ(sim.stats().levels[0].misses(), 6u);
    CHECK_EQ(sim.stats().levels[0].hits(), 1u);
}

TEST(instruction_fetches_skipped_unless_enabled) {
    Trace t;
    t.accesses.push_back({0x400, 4, Op::InstrFetch});
    t.accesses.push_back({0x1000, 4, Op::Read});
    {
        Simulator sim(single(1024, 64, 2));
        sim.run(t);
        CHECK_EQ(sim.stats().skipped_ifetch, 1u);
        CHECK_EQ(sim.stats().cpu_accesses, 1u);
    }
    {
        SimConfig c = single(1024, 64, 2);
        c.include_ifetch = true;
        Simulator sim(c);
        sim.run(t);
        CHECK_EQ(sim.stats().skipped_ifetch, 0u);
        CHECK_EQ(sim.stats().cpu_accesses, 2u);
        CHECK_EQ(sim.stats().levels[0].reads, 2u);
    }
}

TEST(three_c_direct_mapped_ping_pong_is_conflict) {
    // Blocks 0 and 16 map to set 0 of a 16-set direct-mapped cache. A fully
    // associative cache of the same size would keep both, so every miss
    // after the first two is a conflict miss.
    Simulator sim(single(16 * 64, 64, 1));
    Trace t;
    for (int i = 0; i < 10; ++i) t.accesses.push_back({static_cast<std::uint64_t>(i % 2 ? 16 * 64 : 0), 1, Op::Read});
    sim.run(t);
    const LevelStats& s = sim.stats().levels[0];
    CHECK_EQ(s.misses(), 10u);
    CHECK_EQ(s.compulsory, 2u);
    CHECK_EQ(s.capacity, 0u);
    CHECK_EQ(s.conflict, 8u);
}

TEST(three_c_cyclic_scan_is_capacity) {
    // Scan 5 blocks in a loop through a 4-line LRU cache: every miss after
    // the first pass is a capacity miss (LRU's worst case).
    Simulator sim(single(4 * 64, 64, 0));
    Trace t;
    for (int pass = 0; pass < 6; ++pass)
        for (std::uint64_t b = 0; b < 5; ++b) t.accesses.push_back({b * 64, 1, Op::Read});
    sim.run(t);
    const LevelStats& s = sim.stats().levels[0];
    CHECK_EQ(s.misses(), 30u);
    CHECK_EQ(s.compulsory, 5u);
    CHECK_EQ(s.capacity, 25u);
    CHECK_EQ(s.conflict, 0u);
}

TEST(three_c_invariants_on_random_traces) {
    for (std::uint64_t seed = 1; seed <= 8; ++seed) {
        const Trace t = random_trace(seed, 5000, 600, 0.0);
        // A fully associative LRU cache has no conflict misses by definition.
        {
            Simulator sim(single(64 * 64, 64, 0));
            sim.run(t);
            CHECK_EQ(sim.stats().levels[0].conflict, 0u);
        }
        // Compulsory misses = distinct blocks touched (read-only trace).
        Simulator sim(single(32 * 64, 64, 2));
        sim.run(t);
        std::set<std::uint64_t> blocks;
        for (const auto& a : t.accesses) blocks.insert(a.addr / 64);
        const LevelStats& s = sim.stats().levels[0];
        CHECK_EQ(s.compulsory, blocks.size());
        CHECK_EQ(s.compulsory + s.capacity + s.conflict, s.misses());
    }
}

TEST(amat_single_level_matches_formula) {
    const Trace t = random_trace(3, 4000, 300, 0.0);
    Simulator sim(single(16 * 64, 64, 4));
    sim.run(t);
    const LevelStats& s = sim.stats().levels[0];
    CHECK_NEAR(sim.stats().amat(), 4.0 + s.miss_rate() * 100.0, 1e-9);
}

TEST(amat_two_levels_by_hand) {
    // Three reads of block 0: L1 miss + L2 miss, then two L1 hits.
    Simulator sim(two_level());
    Trace t;
    for (int i = 0; i < 3; ++i) t.accesses.push_back({0, 1, Op::Read});
    sim.run(t);
    // (2 + 10 + 100) + 2 + 2 = 116 cycles over 3 accesses.
    CHECK_EQ(sim.stats().total_cycles, 116u);
    CHECK_NEAR(sim.stats().amat(), 116.0 / 3.0, 1e-12);
}

TEST(hierarchy_traffic_is_conserved_write_back) {
    for (std::uint64_t seed = 1; seed <= 6; ++seed) {
        const Trace t = random_trace(seed, 20000, 4000, 0.3);
        Simulator sim(two_level());
        sim.run(t);
        const auto& l1 = sim.stats().levels[0];
        const auto& l2 = sim.stats().levels[1];
        const auto& mem = sim.stats().memory;
        CHECK_EQ(l2.reads, l1.misses());              // every L1 miss fetches a line
        CHECK_EQ(l2.writes, l1.writebacks_out);       // every dirty L1 victim lands in L2
        CHECK_EQ(l2.writebacks_in, l1.writebacks_out);
        // Only L2 read misses fetch from memory: a write-back miss carries a
        // whole line (same line size), so it allocates without a fetch.
        CHECK_EQ(mem.reads, l2.read_misses);
        CHECK(l2.write_misses > 0);
        CHECK_EQ(mem.writes, l2.writebacks_out);
        CHECK_EQ(mem.bytes_read, mem.reads * 64);
        CHECK(l1.writebacks_out > 0);
    }
}

TEST(hierarchy_write_through_no_allocate) {
    SimConfig c = two_level();
    c.levels[0].write_policy = WritePolicy::WriteThrough;
    c.levels[0].alloc_policy = AllocPolicy::NoWriteAllocate;
    const Trace t = random_trace(9, 20000, 4000, 0.3);
    Simulator sim(c);
    sim.run(t);
    const auto& l1 = sim.stats().levels[0];
    const auto& l2 = sim.stats().levels[1];
    CHECK_EQ(l1.writebacks_out, 0u);         // write-through lines are never dirty
    CHECK_EQ(l2.writes, l1.writes);           // every store is forwarded
    CHECK_EQ(l2.reads, l1.read_misses);       // only load misses fetch
    CHECK_EQ(l2.writebacks_in, 0u);
}

TEST(hierarchy_write_through_with_allocate) {
    SimConfig c = two_level();
    c.levels[0].write_policy = WritePolicy::WriteThrough;
    const Trace t = random_trace(10, 20000, 4000, 0.3);
    Simulator sim(c);
    sim.run(t);
    const auto& l1 = sim.stats().levels[0];
    const auto& l2 = sim.stats().levels[1];
    CHECK_EQ(l2.writes, l1.writes);
    CHECK_EQ(l2.reads, l1.misses());  // store misses still fetch the line
}

TEST(opt_requires_whole_trace) {
    Simulator sim(single(1024, 64, 2, PolicyKind::OPT));
    CHECK_THROWS(sim.access({0, 1, Op::Read}));
    Trace t;
    t.accesses.push_back({0, 1, Op::Read});
    sim.run(t);  // fine
    CHECK_EQ(sim.stats().cpu_accesses, 1u);
}

TEST(next_use_positions) {
    Trace t;
    for (std::uint64_t a : {0u, 64u, 0u, 128u, 64u}) t.accesses.push_back({a, 1, Op::Read});
    const auto nu = Simulator::compute_next_use(t, 64, false);
    REQUIRE(nu.size() == 5);
    CHECK_EQ(nu[0], 2u);
    CHECK_EQ(nu[1], 4u);
    CHECK_EQ(nu[2], kNever);
    CHECK_EQ(nu[3], kNever);
    CHECK_EQ(nu[4], kNever);
}

TEST(streaming_matches_batch_for_online_policies) {
    const Trace t = random_trace(21, 8000, 2000, 0.25);
    for (PolicyKind p : {PolicyKind::LRU, PolicyKind::FIFO, PolicyKind::PLRU, PolicyKind::SRRIP, PolicyKind::Random}) {
        SimConfig c = two_level();
        c.levels[0].policy = p;
        Simulator a(c), b(c);
        a.run(t);
        for (const auto& acc : t.accesses) b.access(acc);
        CHECK_EQ(a.stats().levels[0].misses(), b.stats().levels[0].misses());
        CHECK_EQ(a.stats().levels[1].misses(), b.stats().levels[1].misses());
        CHECK_EQ(a.stats().total_cycles, b.stats().total_cycles);
    }
}

TEST(verbose_events_describe_each_level) {
    Simulator sim(two_level());
    std::vector<LevelEvent> ev;
    sim.set_event_sink(&ev);
    Trace t;
    t.accesses.push_back({0x100, 4, Op::Write});
    sim.run(t);
    REQUIRE(ev.size() == 2);
    CHECK_EQ(ev[0].level, 0u);
    CHECK(ev[0].kind == AccessKind::Demand);
    CHECK(ev[0].is_write);
    CHECK(!ev[0].hit);
    CHECK(ev[0].miss_type == MissType::Compulsory);
    CHECK_EQ(ev[1].level, 1u);
    CHECK(ev[1].kind == AccessKind::Fill);
    CHECK(!ev[1].is_write);
}

TEST(memory_only_counts_last_level_traffic) {
    Simulator sim(single(64, 64, 1));
    Trace t;
    t.accesses.push_back({0, 1, Op::Write});   // miss, fill, dirty
    t.accesses.push_back({64, 1, Op::Read});   // miss, evicts dirty block 0
    sim.run(t);
    CHECK_EQ(sim.stats().memory.reads, 2u);
    CHECK_EQ(sim.stats().memory.writes, 1u);
    CHECK_EQ(sim.stats().levels[0].writebacks_out, 1u);
}
