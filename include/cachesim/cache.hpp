// One level of set-associative cache.
//
// Storage is structure-of-arrays: the block numbers of a set sit next to
// each other in memory, so a lookup in an 8-way set touches one or two
// host cache lines. Ways fill in order (0, 1, 2, ...) and lines are never
// invalidated, so a set's valid lines are exactly ways [0, fill_count).
// That removes the valid-bit check from the hot loop.
//
// For very associative caches (more than kIndexThreshold ways, e.g. a
// fully associative cache) a linear scan would cost O(ways) per access, so
// the level also keeps a hash index from block number to line.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "cachesim/config.hpp"
#include "cachesim/flat_hash_map.hpp"
#include "cachesim/policy.hpp"

namespace cachesim {

struct Geometry {
    std::uint64_t size_bytes = 0;
    std::uint64_t block_bytes = 0;
    std::uint32_t ways = 0;
    std::uint64_t sets = 0;
    unsigned offset_bits = 0;
    unsigned index_bits = 0;      // ceil(log2(sets)); exact when sets is a power of two
    bool pow2_sets = true;

    /// Tag width for a given address width (e.g. 32 or 64).
    unsigned tag_bits(unsigned address_bits) const noexcept {
        const unsigned used = offset_bits + index_bits;
        return address_bits > used ? address_bits - used : 0;
    }
};

class CacheLevel {
public:
    static constexpr std::uint32_t kIndexThreshold = 32;

    struct Result {
        bool hit = false;
        bool filled = false;          // a block was installed (miss with allocation)
        bool evicted = false;         // a valid block was replaced
        bool evicted_dirty = false;   // ...and it was dirty (needs a write-back)
        std::uint64_t evicted_block = 0;
        std::uint64_t set = 0;
        std::uint32_t way = 0;        // way hit or filled (undefined if neither)
    };

    /// `cfg` must already be validated.
    CacheLevel(const LevelConfig& cfg, std::uint64_t seed);

    CacheLevel(const CacheLevel&) = delete;
    CacheLevel& operator=(const CacheLevel&) = delete;
    CacheLevel(CacheLevel&&) noexcept = default;
    CacheLevel& operator=(CacheLevel&&) noexcept = default;

    /// Look up `block` (= byte address >> offset_bits) and update state.
    /// `next_use` is the position of the next reference to this block and
    /// only matters for OPT.
    Result access(std::uint64_t block, bool is_write, std::uint64_t next_use = kNever);

    std::uint64_t block_of(std::uint64_t addr) const noexcept { return addr >> geo_.offset_bits; }
    std::uint64_t set_of(std::uint64_t block) const noexcept {
        return geo_.pow2_sets ? (block & set_mask_) : (block % geo_.sets);
    }
    std::uint64_t tag_of(std::uint64_t block) const noexcept {
        return geo_.pow2_sets ? (block >> geo_.index_bits) : (block / geo_.sets);
    }

    bool contains(std::uint64_t block) const noexcept;
    bool is_dirty(std::uint64_t block) const noexcept;

    /// Blocks currently stored in `set`, by way. Invalid ways are omitted.
    std::vector<std::uint64_t> set_contents(std::uint64_t set) const;
    std::string describe_policy_state(std::uint64_t set) const { return policy_->describe_set(set); }

    const LevelConfig& config() const noexcept { return cfg_; }
    const Geometry& geometry() const noexcept { return geo_; }

private:
    // Returns the way holding `block` in `set`, or -1.
    std::int64_t find_way(std::uint64_t set, std::uint64_t block) const noexcept;

    LevelConfig cfg_;
    Geometry geo_;
    std::uint64_t set_mask_ = 0;
    bool write_back_ = true;
    bool write_allocate_ = true;
    bool use_index_ = false;

    std::vector<std::uint64_t> blocks_;      // sets * ways block numbers
    std::vector<std::uint8_t> dirty_;        // sets * ways dirty bits
    std::vector<std::uint32_t> fill_count_;  // valid ways per set
    FlatHashMap<std::uint32_t> index_;       // block -> line, only if use_index_
    std::unique_ptr<ReplacementPolicy> policy_;
};

}  // namespace cachesim
