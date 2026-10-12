// The cache hierarchy simulator: L1 -> L2 -> ... -> memory.
//
// Model (non-inclusive, non-exclusive, like most textbook simulators):
//   * A CPU access is split into one access per L1 line it touches.
//   * A miss fetches the line from the next level (a Fill access).
//   * A dirty victim is written to the next level (a Writeback access).
//     If that level misses and has the same line size, it allocates the
//     line without fetching it, because the whole line is being written.
//   * Write-through levels forward every store (a WriteThrough access).
//   * No-write-allocate levels forward a store miss without filling.
//   * Latency: each Demand/Fill access costs the level's hit latency, plus
//     the latency of the level below on a miss. Write-backs and
//     write-throughs are buffered and do not add latency. AMAT is the total
//     demand latency divided by the number of CPU line accesses.
//   * 3C classification: per level, a miss is compulsory if the block was
//     never in that level before, capacity if a fully associative LRU cache
//     of the same size would also miss, and conflict otherwise.
#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "cachesim/cache.hpp"
#include "cachesim/config.hpp"
#include "cachesim/flat_hash_map.hpp"
#include "cachesim/trace.hpp"

namespace cachesim {

struct LevelStats {
    std::uint64_t reads = 0;          // all read accesses arriving (demand + fills)
    std::uint64_t writes = 0;         // all write accesses arriving
    std::uint64_t read_misses = 0;
    std::uint64_t write_misses = 0;
    std::uint64_t writebacks_in = 0;  // subset of writes: dirty victims from above
    std::uint64_t evictions = 0;      // valid lines replaced
    std::uint64_t writebacks_out = 0; // dirty lines replaced (sent below)
    std::uint64_t compulsory = 0;
    std::uint64_t capacity = 0;
    std::uint64_t conflict = 0;

    std::uint64_t accesses() const noexcept { return reads + writes; }
    std::uint64_t misses() const noexcept { return read_misses + write_misses; }
    std::uint64_t hits() const noexcept { return accesses() - misses(); }
    double miss_rate() const noexcept {
        return accesses() ? static_cast<double>(misses()) / static_cast<double>(accesses()) : 0.0;
    }
    double hit_rate() const noexcept { return accesses() ? 1.0 - miss_rate() : 0.0; }
};

struct MemoryStats {
    std::uint64_t reads = 0;          // line reads (fills from the last level)
    std::uint64_t writes = 0;         // line writes (write-backs / write-throughs)
    std::uint64_t bytes_read = 0;
    std::uint64_t bytes_written = 0;
};

struct SimStats {
    std::uint64_t records = 0;          // trace records processed (M counts as 2)
    std::uint64_t skipped_ifetch = 0;   // 'I' records ignored (see include_ifetch)
    std::uint64_t cpu_accesses = 0;     // line-granular L1 accesses
    std::uint64_t split_records = 0;    // records that crossed a line boundary
    std::uint64_t total_cycles = 0;     // demand latency summed over cpu_accesses
    std::vector<LevelStats> levels;
    MemoryStats memory;

    double amat() const noexcept {
        return cpu_accesses ? static_cast<double>(total_cycles) / static_cast<double>(cpu_accesses) : 0.0;
    }
};

/// One level's reaction to one access. Collected only in verbose mode.
struct LevelEvent {
    std::uint8_t level = 0;
    AccessKind kind = AccessKind::Demand;
    bool is_write = false;
    bool hit = false;
    bool evicted = false;
    bool evicted_dirty = false;
    MissType miss_type = MissType::None;
    std::uint64_t block = 0;
    std::uint64_t set = 0;
    std::uint64_t evicted_block = 0;
};

class Simulator {
public:
    /// Validates the config (throws ConfigError).
    explicit Simulator(const SimConfig& cfg);

    using RecordHook = std::function<void(const Access&)>;

    /// Simulate a whole trace. Required for OPT, which needs the future.
    /// `after_each`, if given, is called after every record (verbose mode).
    void run(const Trace& trace, const RecordHook& after_each = nullptr);

    /// Stream one access. Not available when the first level uses OPT.
    void access(const Access& a);

    /// If set, every level touched by an access appends a LevelEvent.
    void set_event_sink(std::vector<LevelEvent>* sink) noexcept { events_ = sink; }

    const SimStats& stats() const noexcept { return stats_; }
    const SimConfig& config() const noexcept { return cfg_; }
    const CacheLevel& level(std::size_t i) const { return levels_.at(i); }
    std::size_t level_count() const noexcept { return levels_.size(); }

    /// Next-use positions for OPT: for each L1 line access i (in run order),
    /// the index of the next L1 line access to the same block, or kNever.
    static std::vector<std::uint64_t> compute_next_use(const Trace& trace, std::uint64_t block_bytes,
                                                       bool include_ifetch);

private:
    void access_impl(const Access& a);
    std::uint64_t access_level(std::size_t i, std::uint64_t addr, bool is_write, AccessKind kind,
                               std::uint64_t next_use);

    SimConfig cfg_;
    std::vector<CacheLevel> levels_;
    std::vector<CacheLevel> shadows_;            // fully associative LRU twins (3C)
    std::vector<FlatHashMap<std::uint8_t>> seen_;  // blocks ever filled, per level (3C)
    SimStats stats_;
    std::vector<LevelEvent>* events_ = nullptr;
    const std::vector<std::uint64_t>* next_use_ = nullptr;
    std::uint64_t l1_position_ = 0;  // L1 line accesses so far (OPT clock)
    std::uint64_t run_base_ = 0;     // l1_position_ when the current run() began
};

}  // namespace cachesim
