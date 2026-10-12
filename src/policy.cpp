#include "cachesim/policy.hpp"

#include <sstream>
#include <stdexcept>

namespace cachesim {
namespace {

// ---------------------------------------------------------------------------
// LRU and FIFO: one intrusive doubly linked list per set.
//
// prev_/next_ hold way indices inside the set, stored in flat arrays of
// size sets * ways (no heap node per line). head_ is the most recently
// used (or most recently inserted, for FIFO) way and tail_ is the victim.
// Every operation is O(1), which matters for fully associative caches with
// thousands of ways.
// ---------------------------------------------------------------------------
class ListPolicy final : public ReplacementPolicy {
public:
    ListPolicy(std::size_t sets, std::uint32_t ways, bool move_on_hit)
        : ways_(ways),
          move_on_hit_(move_on_hit),
          prev_(sets * ways, kNil),
          next_(sets * ways, kNil),
          head_(sets, kNil),
          tail_(sets, kNil) {}

    void on_hit(std::size_t set, std::uint32_t way, std::uint64_t) override {
        if (move_on_hit_ && head_[set] != way) {
            unlink(set, way);
            push_front(set, way);
        }
    }

    void on_fill(std::size_t set, std::uint32_t way, std::uint64_t) override {
        if (linked(set, way)) unlink(set, way);
        push_front(set, way);
    }

    std::uint32_t victim(std::size_t set) override { return tail_[set]; }

    std::string describe_set(std::size_t set) const override {
        std::ostringstream os;
        os << (move_on_hit_ ? "MRU" : "newest");
        for (std::uint32_t w = head_[set]; w != kNil; w = next_[set * ways_ + w]) os << ' ' << w;
        os << (move_on_hit_ ? " LRU" : " oldest");
        return os.str();
    }

    PolicyKind kind() const noexcept override {
        return move_on_hit_ ? PolicyKind::LRU : PolicyKind::FIFO;
    }

private:
    static constexpr std::uint32_t kNil = UINT32_MAX;

    bool linked(std::size_t set, std::uint32_t way) const {
        return head_[set] == way || prev_[set * ways_ + way] != kNil;
    }

    void unlink(std::size_t set, std::uint32_t way) {
        const std::size_t i = set * ways_ + way;
        const std::uint32_t p = prev_[i];
        const std::uint32_t n = next_[i];
        if (p != kNil) next_[set * ways_ + p] = n; else head_[set] = n;
        if (n != kNil) prev_[set * ways_ + n] = p; else tail_[set] = p;
        prev_[i] = next_[i] = kNil;
    }

    void push_front(std::size_t set, std::uint32_t way) {
        const std::size_t i = set * ways_ + way;
        const std::uint32_t h = head_[set];
        prev_[i] = kNil;
        next_[i] = h;
        if (h != kNil) prev_[set * ways_ + h] = way; else tail_[set] = way;
        head_[set] = way;
    }

    std::uint32_t ways_;
    bool move_on_hit_;
    std::vector<std::uint32_t> prev_, next_, head_, tail_;
};

// ---------------------------------------------------------------------------
// Random replacement with a deterministic seed.
// ---------------------------------------------------------------------------
class RandomPolicy final : public ReplacementPolicy {
public:
    RandomPolicy(std::uint32_t ways, std::uint64_t seed) : ways_(ways), rng_(seed) {}
    void on_hit(std::size_t, std::uint32_t, std::uint64_t) override {}
    void on_fill(std::size_t, std::uint32_t, std::uint64_t) override {}
    std::uint32_t victim(std::size_t) override {
        return static_cast<std::uint32_t>(rng_.below(ways_));
    }
    std::string describe_set(std::size_t) const override { return "random"; }
    PolicyKind kind() const noexcept override { return PolicyKind::Random; }

private:
    std::uint32_t ways_;
    XorShift64 rng_;
};

// ---------------------------------------------------------------------------
// Tree pseudo-LRU. A set of W ways keeps W-1 bits arranged as a complete
// binary tree in heap order (children of node n are 2n+1 and 2n+2).
// Bit 0 means "the victim is in the left subtree", bit 1 means "right".
// An access flips the bits on its path to point away from it.
// This is what most real L1 and L2 caches implement: W-1 bits per set
// instead of log2(W!) bits for true LRU.
// ---------------------------------------------------------------------------
class TreePlruPolicy final : public ReplacementPolicy {
public:
    TreePlruPolicy(std::size_t sets, std::uint32_t ways)
        : ways_(ways), nodes_(ways > 1 ? ways - 1 : 0), bits_(sets * nodes_, 0) {}

    void on_hit(std::size_t set, std::uint32_t way, std::uint64_t) override { touch(set, way); }
    void on_fill(std::size_t set, std::uint32_t way, std::uint64_t) override { touch(set, way); }

    std::uint32_t victim(std::size_t set) override {
        const std::uint8_t* b = bits_.data() + set * nodes_;
        std::uint32_t node = 0, lo = 0, hi = ways_;
        while (hi - lo > 1) {
            const std::uint32_t mid = lo + (hi - lo) / 2;
            if (b[node] == 0) { node = 2 * node + 1; hi = mid; }
            else              { node = 2 * node + 2; lo = mid; }
        }
        return lo;
    }

    std::string describe_set(std::size_t set) const override {
        std::string s = "tree bits ";
        for (std::uint32_t n = 0; n < nodes_; ++n) s.push_back(bits_[set * nodes_ + n] ? '1' : '0');
        if (nodes_ == 0) s += "(none)";
        return s;
    }

    PolicyKind kind() const noexcept override { return PolicyKind::PLRU; }

private:
    void touch(std::size_t set, std::uint32_t way) {
        std::uint8_t* b = bits_.data() + set * nodes_;
        std::uint32_t node = 0, lo = 0, hi = ways_;
        while (hi - lo > 1) {
            const std::uint32_t mid = lo + (hi - lo) / 2;
            if (way < mid) { b[node] = 1; node = 2 * node + 1; hi = mid; }
            else           { b[node] = 0; node = 2 * node + 2; lo = mid; }
        }
    }

    std::uint32_t ways_;
    std::uint32_t nodes_;
    std::vector<std::uint8_t> bits_;
};

// ---------------------------------------------------------------------------
// SRRIP with 2-bit re-reference prediction values (RRPV).
// Insert with RRPV = 2 ("long re-reference"), promote to 0 on a hit, evict
// the first way with RRPV = 3, ageing the whole set until one exists.
// Scan-resistant: a one-time streaming pass cannot flush hot lines.
// ---------------------------------------------------------------------------
class SrripPolicy final : public ReplacementPolicy {
public:
    static constexpr std::uint8_t kMax = 3;
    static constexpr std::uint8_t kInsert = 2;

    SrripPolicy(std::size_t sets, std::uint32_t ways) : ways_(ways), rrpv_(sets * ways, kMax) {}

    void on_hit(std::size_t set, std::uint32_t way, std::uint64_t) override {
        rrpv_[set * ways_ + way] = 0;
    }
    void on_fill(std::size_t set, std::uint32_t way, std::uint64_t) override {
        rrpv_[set * ways_ + way] = kInsert;
    }
    std::uint32_t victim(std::size_t set) override {
        std::uint8_t* r = rrpv_.data() + set * ways_;
        for (;;) {
            for (std::uint32_t w = 0; w < ways_; ++w) {
                if (r[w] >= kMax) return w;
            }
            for (std::uint32_t w = 0; w < ways_; ++w) ++r[w];
        }
    }
    std::string describe_set(std::size_t set) const override {
        std::string s = "rrpv";
        for (std::uint32_t w = 0; w < ways_; ++w) {
            s.push_back(' ');
            s.push_back(static_cast<char>('0' + rrpv_[set * ways_ + w]));
        }
        return s;
    }
    PolicyKind kind() const noexcept override { return PolicyKind::SRRIP; }

private:
    std::uint32_t ways_;
    std::vector<std::uint8_t> rrpv_;
};

// ---------------------------------------------------------------------------
// Belady's OPT (MIN). Each line remembers the position of its next
// reference; the victim is the line referenced farthest in the future.
// The simulator precomputes next-use positions in one backward pass over
// the trace, so this needs the whole trace up front.
// ---------------------------------------------------------------------------
class OptPolicy final : public ReplacementPolicy {
public:
    OptPolicy(std::size_t sets, std::uint32_t ways) : ways_(ways), next_use_(sets * ways, kNever) {}

    void on_hit(std::size_t set, std::uint32_t way, std::uint64_t next_use) override {
        next_use_[set * ways_ + way] = next_use;
    }
    void on_fill(std::size_t set, std::uint32_t way, std::uint64_t next_use) override {
        next_use_[set * ways_ + way] = next_use;
    }
    std::uint32_t victim(std::size_t set) override {
        const std::uint64_t* n = next_use_.data() + set * ways_;
        std::uint32_t best = 0;
        for (std::uint32_t w = 1; w < ways_; ++w) {
            if (n[w] > n[best]) best = w;
        }
        return best;
    }
    std::string describe_set(std::size_t set) const override {
        std::ostringstream os;
        os << "next use";
        for (std::uint32_t w = 0; w < ways_; ++w) {
            const std::uint64_t v = next_use_[set * ways_ + w];
            os << ' ';
            if (v == kNever) os << "never"; else os << v;
        }
        return os.str();
    }
    PolicyKind kind() const noexcept override { return PolicyKind::OPT; }

private:
    std::uint32_t ways_;
    std::vector<std::uint64_t> next_use_;
};

}  // namespace

std::unique_ptr<ReplacementPolicy> make_policy(PolicyKind kind, std::size_t sets,
                                               std::uint32_t ways, std::uint64_t seed) {
    if (sets == 0 || ways == 0) throw std::invalid_argument("policy needs at least one set and one way");
    switch (kind) {
        case PolicyKind::LRU: return std::make_unique<ListPolicy>(sets, ways, true);
        case PolicyKind::FIFO: return std::make_unique<ListPolicy>(sets, ways, false);
        case PolicyKind::Random: return std::make_unique<RandomPolicy>(ways, seed);
        case PolicyKind::PLRU:
            if (!is_pow2(ways)) {
                throw std::invalid_argument("PLRU needs a power-of-two number of ways");
            }
            return std::make_unique<TreePlruPolicy>(sets, ways);
        case PolicyKind::SRRIP: return std::make_unique<SrripPolicy>(sets, ways);
        case PolicyKind::OPT: return std::make_unique<OptPolicy>(sets, ways);
    }
    throw std::invalid_argument("unknown replacement policy");
}

}  // namespace cachesim
