// Replacement policies.
//
// A policy only sees (set, way) coordinates. The cache owns the tags and
// always fills invalid ways first (way 0, 1, 2, ... in order), so a policy
// is asked for a victim only when its set is full.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "cachesim/types.hpp"

namespace cachesim {

class ReplacementPolicy {
public:
    virtual ~ReplacementPolicy() = default;

    /// The line at (set, way) was hit. `next_use` is only used by OPT.
    virtual void on_hit(std::size_t set, std::uint32_t way, std::uint64_t next_use) = 0;

    /// A new block was just placed at (set, way). The way may be a victim
    /// returned by victim() or a previously invalid way.
    virtual void on_fill(std::size_t set, std::uint32_t way, std::uint64_t next_use) = 0;

    /// Pick the way to evict from a full set. Must not change hit/miss
    /// semantics on its own; on_fill() is always called right after.
    virtual std::uint32_t victim(std::size_t set) = 0;

    /// Human-readable per-set state, used by verbose mode and tests.
    /// Example for LRU: "MRU 2 0 3 1 LRU".
    virtual std::string describe_set(std::size_t set) const = 0;

    virtual PolicyKind kind() const noexcept = 0;
};

/// Build a policy for a cache with `sets` sets of `ways` ways.
/// Throws std::invalid_argument for unsupported shapes (e.g. PLRU needs
/// a power-of-two number of ways).
std::unique_ptr<ReplacementPolicy> make_policy(PolicyKind kind, std::size_t sets,
                                               std::uint32_t ways, std::uint64_t seed);

/// xorshift64* generator. Tiny, fast and fully deterministic across
/// platforms (std::mt19937 distributions are not).
class XorShift64 {
public:
    explicit XorShift64(std::uint64_t seed) noexcept : s_(seed ? seed : 0x9E3779B97F4A7C15ULL) {}
    std::uint64_t next() noexcept {
        s_ ^= s_ >> 12;
        s_ ^= s_ << 25;
        s_ ^= s_ >> 27;
        return s_ * 0x2545F4914F6CDD1DULL;
    }
    /// Uniform-ish integer in [0, n). Modulo bias is negligible for n << 2^64.
    std::uint64_t below(std::uint64_t n) noexcept { return next() % n; }

private:
    std::uint64_t s_;
};

}  // namespace cachesim
