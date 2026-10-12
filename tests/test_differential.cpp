// Differential testing: the optimized simulator against a deliberately
// naive reference model, on hundreds of random configurations and traces.
//
// The reference shares no code with the engine. It keeps each set as a
// plain vector, finds victims by linear search over timestamps, keeps PLRU
// bits in a std::map keyed by subtree range, and answers OPT queries with
// binary search over per-block position lists.
#include <algorithm>
#include <list>
#include <map>
#include <set>
#include <vector>

#include "cachesim/simulator.hpp"
#include "test_framework.hpp"

using namespace cachesim;

namespace {

struct RefStats {
    std::uint64_t reads = 0, writes = 0, read_misses = 0, write_misses = 0;
    std::uint64_t evictions = 0, writebacks = 0;
    std::uint64_t mem_reads = 0, mem_writes = 0;
    std::uint64_t compulsory = 0, capacity = 0, conflict = 0;
};

class RefCache {
public:
    RefCache(const LevelConfig& c, std::uint64_t seed)
        : cfg_(c), sets_(c.sets()), ways_(c.ways()), lines_(sets_), plru_(sets_), rng_(seed) {}

    void run(const std::vector<std::pair<std::uint64_t, bool>>& stream) {
        // Positions of every block, for OPT.
        std::map<std::uint64_t, std::vector<std::size_t>> positions;
        for (std::size_t i = 0; i < stream.size(); ++i) positions[stream[i].first].push_back(i);

        for (std::size_t pos = 0; pos < stream.size(); ++pos) {
            const auto [block, is_write] = stream[pos];
            access(block, is_write, pos, positions);
        }
    }

    RefStats stats;

private:
    struct Line {
        std::uint64_t block;
        bool dirty;
        std::uint64_t last_use;
        std::uint64_t inserted;
        int rrpv;
    };

    bool wb() const { return cfg_.write_policy == WritePolicy::WriteBack; }
    bool wa() const { return cfg_.alloc_policy == AllocPolicy::WriteAllocate; }

    void plru_touch(std::uint64_t set, std::uint64_t way) {
        std::uint64_t lo = 0, hi = ways_;
        while (hi - lo > 1) {
            const std::uint64_t mid = (lo + hi) / 2;
            bool& bit = plru_[set][{lo, hi}];
            if (way < mid) { bit = true; hi = mid; }   // victim side is now right
            else           { bit = false; lo = mid; }  // victim side is now left
        }
    }
    std::uint64_t plru_victim(std::uint64_t set) {
        std::uint64_t lo = 0, hi = ways_;
        while (hi - lo > 1) {
            const std::uint64_t mid = (lo + hi) / 2;
            if (plru_[set][{lo, hi}]) lo = mid; else hi = mid;
        }
        return lo;
    }

    std::uint64_t next_use(std::uint64_t block, std::size_t pos,
                           const std::map<std::uint64_t, std::vector<std::size_t>>& positions) const {
        const auto& v = positions.at(block);
        const auto it = std::upper_bound(v.begin(), v.end(), pos);
        return it == v.end() ? UINT64_MAX : *it;
    }

    std::uint64_t victim(std::uint64_t set, std::size_t pos,
                         const std::map<std::uint64_t, std::vector<std::size_t>>& positions) {
        auto& ls = lines_[set];
        std::uint64_t best = 0;
        switch (cfg_.policy) {
            case PolicyKind::LRU:
                for (std::uint64_t w = 1; w < ways_; ++w) if (ls[w].last_use < ls[best].last_use) best = w;
                return best;
            case PolicyKind::FIFO:
                for (std::uint64_t w = 1; w < ways_; ++w) if (ls[w].inserted < ls[best].inserted) best = w;
                return best;
            case PolicyKind::Random:
                return rng_.below(ways_);
            case PolicyKind::PLRU:
                return plru_victim(set);
            case PolicyKind::SRRIP:
                for (;;) {
                    for (std::uint64_t w = 0; w < ways_; ++w) if (ls[w].rrpv == 3) return w;
                    for (auto& l : ls) ++l.rrpv;
                }
            case PolicyKind::OPT: {
                std::uint64_t far = 0;
                for (std::uint64_t w = 0; w < ways_; ++w) {
                    const std::uint64_t n = next_use(ls[w].block, pos, positions);
                    if (w == 0 || n > far) { far = n; best = w; }
                }
                return best;
            }
        }
        return 0;
    }

    // Fully associative LRU twin for the 3C model, as a plain std::list.
    bool shadow_access(std::uint64_t block, bool is_write) {
        auto it = std::find(shadow_.begin(), shadow_.end(), block);
        if (it != shadow_.end()) {
            shadow_.erase(it);
            shadow_.push_front(block);
            return true;
        }
        if (is_write && !wa()) return false;
        shadow_.push_front(block);
        if (shadow_.size() > cfg_.lines()) shadow_.pop_back();
        return false;
    }

    void access(std::uint64_t block, bool is_write, std::size_t pos,
                const std::map<std::uint64_t, std::vector<std::size_t>>& positions) {
        const std::uint64_t set = block % sets_;
        auto& ls = lines_[set];
        if (is_write) ++stats.writes; else ++stats.reads;
        const bool shadow_hit = shadow_access(block, is_write);

        for (std::uint64_t w = 0; w < ls.size(); ++w) {
            if (ls[w].block == block) {  // hit
                ls[w].last_use = clock_++;
                ls[w].rrpv = 0;
                if (is_write && wb()) ls[w].dirty = true;
                if (is_write && !wb()) ++stats.mem_writes;
                plru_touch(set, w);
                return;
            }
        }
        // miss
        if (is_write) ++stats.write_misses; else ++stats.read_misses;
        if (!seen_.count(block)) ++stats.compulsory;
        else if (!shadow_hit) ++stats.capacity;
        else ++stats.conflict;

        if (is_write && !wa()) {
            ++stats.mem_writes;
            ++clock_;
            return;
        }
        std::uint64_t w;
        if (ls.size() < ways_) {
            w = ls.size();
            ls.push_back({});
        } else {
            w = victim(set, pos, positions);
            ++stats.evictions;
            if (ls[w].dirty) {
                ++stats.writebacks;
                ++stats.mem_writes;
            }
        }
        ls[w] = Line{block, is_write && wb(), clock_, clock_, 2};
        ++clock_;
        plru_touch(set, w);
        seen_.insert(block);
        ++stats.mem_reads;
        if (is_write && !wb()) ++stats.mem_writes;
    }

    LevelConfig cfg_;
    std::uint64_t sets_, ways_;
    std::vector<std::vector<Line>> lines_;
    std::vector<std::map<std::pair<std::uint64_t, std::uint64_t>, bool>> plru_;
    std::list<std::uint64_t> shadow_;
    std::set<std::uint64_t> seen_;
    XorShift64 rng_;
    std::uint64_t clock_ = 0;
};

/// Random trace with some locality: a hot region, strided runs and noise.
Trace make_trace(XorShift64& rng, std::size_t n, std::uint64_t block) {
    Trace t;
    const std::uint64_t span = block * (16 + rng.below(400));
    std::uint64_t cursor = rng.below(span);
    for (std::size_t i = 0; i < n; ++i) {
        std::uint64_t addr;
        switch (rng.below(4)) {
            case 0: addr = rng.below(span / 4 + 1); break;                 // hot region
            case 1: cursor += block; addr = cursor % span; break;          // stride
            default: addr = rng.below(span);                               // noise
        }
        static constexpr std::uint32_t sizes[] = {1, 1, 4, 8, 16};
        const std::uint32_t size = sizes[rng.below(5)];
        const Op op = rng.below(10) < 3 ? Op::Write : Op::Read;
        t.accesses.push_back({addr, size, op});
    }
    return t;
}

std::vector<std::pair<std::uint64_t, bool>> line_stream(const Trace& t, std::uint64_t block) {
    std::vector<std::pair<std::uint64_t, bool>> s;
    for (const auto& a : t.accesses) {
        const std::uint64_t first = a.addr / block, last = (a.addr + a.size - 1) / block;
        for (std::uint64_t b = first; b <= last; ++b) s.push_back({b, a.op == Op::Write});
    }
    return s;
}

}  // namespace

TEST(differential_vs_reference_model) {
    XorShift64 rng(2024);
    const PolicyKind policies[] = {PolicyKind::LRU, PolicyKind::FIFO, PolicyKind::Random,
                                   PolicyKind::PLRU, PolicyKind::SRRIP, PolicyKind::OPT};
    const std::uint64_t blocks[] = {1, 4, 16, 64};
    const std::uint64_t set_choices[] = {1, 2, 3, 4, 5, 8, 16};
    const std::uint64_t way_choices[] = {1, 2, 3, 4, 8, 16, 40};  // 40 > 32 exercises the hash index

    int cases = 0;
    for (int iter = 0; iter < 600; ++iter) {
        LevelConfig c;
        c.policy = policies[iter % 6];
        c.block_bytes = blocks[rng.below(4)];
        const std::uint64_t sets = set_choices[rng.below(7)];
        std::uint64_t ways = way_choices[rng.below(7)];
        if (c.policy == PolicyKind::PLRU && !is_pow2(ways)) ways = 8;
        c.assoc = rng.below(6) == 0 && sets == 1 ? 0 : ways;  // sometimes say "full"
        c.size_bytes = c.block_bytes * sets * ways;
        c.write_policy = rng.below(2) ? WritePolicy::WriteBack : WritePolicy::WriteThrough;
        c.alloc_policy = rng.below(2) ? AllocPolicy::WriteAllocate : AllocPolicy::NoWriteAllocate;

        SimConfig sc;
        sc.levels[0] = c;
        sc.seed = 1 + rng.below(1000);
        const Trace trace = make_trace(rng, 1500, c.block_bytes);

        Simulator sim(sc);
        sim.run(trace);
        RefCache ref(c, sc.seed);
        ref.run(line_stream(trace, c.block_bytes));

        const LevelStats& s = sim.stats().levels[0];
        const MemoryStats& m = sim.stats().memory;
        const RefStats& r = ref.stats;
        const bool ok = s.reads == r.reads && s.writes == r.writes && s.read_misses == r.read_misses &&
                        s.write_misses == r.write_misses && s.evictions == r.evictions &&
                        s.writebacks_out == r.writebacks && m.reads == r.mem_reads && m.writes == r.mem_writes &&
                        s.compulsory == r.compulsory && s.capacity == r.capacity && s.conflict == r.conflict;
        CHECK(ok);
        if (!ok) {
            std::cerr << "    mismatch: policy=" << to_string(c.policy) << " block=" << c.block_bytes
                      << " sets=" << sets << " ways=" << ways << " " << to_string(c.write_policy) << " "
                      << to_string(c.alloc_policy) << "\n      sim misses " << s.misses() << " ref "
                      << (r.read_misses + r.write_misses) << ", sim mem " << m.reads << "/" << m.writes << " ref "
                      << r.mem_reads << "/" << r.mem_writes << ", 3C sim " << s.compulsory << "/" << s.capacity << "/"
                      << s.conflict << " ref " << r.compulsory << "/" << r.capacity << "/" << r.conflict << "\n";
            return;
        }
        ++cases;
    }
    CHECK_EQ(cases, 600);
}
