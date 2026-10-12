// Core value types shared by every part of the simulator.
#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace cachesim {

/// Kind of memory reference found in a trace.
enum class Op : std::uint8_t { Read, Write, InstrFetch };

/// One memory reference from a trace. `size` is the number of bytes touched.
/// An access that crosses a line boundary is split into one access per line.
struct Access {
    std::uint64_t addr = 0;
    std::uint32_t size = 1;
    Op op = Op::Read;
};

/// Replacement policies supported by every cache level.
enum class PolicyKind : std::uint8_t {
    LRU,     // least recently used, O(1) intrusive list per set
    FIFO,    // first in, first out (insertion order, hits do not reorder)
    Random,  // uniform random victim (deterministic xorshift seed)
    PLRU,    // tree pseudo-LRU, the policy most real L1/L2 caches use
    SRRIP,   // static re-reference interval prediction (Jaleel et al., ISCA 2010)
    OPT,     // Belady's optimal policy (needs the future, so first level only)
};

enum class WritePolicy : std::uint8_t { WriteBack, WriteThrough };
enum class AllocPolicy : std::uint8_t { WriteAllocate, NoWriteAllocate };

/// Why an access reached a level. Only Demand and Fill accesses are on the
/// critical path; write-backs and write-throughs are assumed to be buffered.
enum class AccessKind : std::uint8_t {
    Demand,        // load/store issued by the CPU (first level only)
    Fill,          // line fetch caused by a miss in the level above
    Writeback,     // dirty victim evicted from the level above
    WriteThrough,  // store forwarded by a write-through or no-write-allocate level
};

/// The 3C miss model (Hill and Smith, 1989).
enum class MissType : std::uint8_t { None, Compulsory, Capacity, Conflict };

/// Sentinel used for "this block is never referenced again".
inline constexpr std::uint64_t kNever = UINT64_MAX;

std::string_view to_string(Op op) noexcept;
std::string_view to_string(PolicyKind p) noexcept;
std::string_view to_string(WritePolicy w) noexcept;
std::string_view to_string(AllocPolicy a) noexcept;
std::string_view to_string(AccessKind k) noexcept;
std::string_view to_string(MissType m) noexcept;

/// Case-insensitive parse of a policy name ("lru", "PLRU", "belady", ...).
std::optional<PolicyKind> parse_policy(std::string_view s) noexcept;

// ---- small bit helpers --------------------------------------------------

constexpr bool is_pow2(std::uint64_t x) noexcept { return x != 0 && (x & (x - 1)) == 0; }

/// floor(log2(x)) for x > 0, computed with integers only.
constexpr unsigned log2_floor(std::uint64_t x) noexcept {
    unsigned r = 0;
    while (x >>= 1) ++r;
    return r;
}

/// ceil(log2(x)) for x > 0.
constexpr unsigned log2_ceil(std::uint64_t x) noexcept {
    return x <= 1 ? 0 : log2_floor(x - 1) + 1;
}

}  // namespace cachesim
