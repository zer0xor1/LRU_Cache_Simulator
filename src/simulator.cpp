#include "cachesim/simulator.hpp"

#include <stdexcept>

namespace cachesim {
namespace {

/// Call fn(line_number) for every line of 2^offset_bits bytes that the
/// access touches. Most accesses touch exactly one line.
template <typename F>
inline void for_each_line(const Access& a, unsigned offset_bits, F&& fn) {
    const std::uint64_t size = a.size ? a.size : 1;
    const std::uint64_t last_byte = (size - 1 > UINT64_MAX - a.addr) ? UINT64_MAX : a.addr + size - 1;
    const std::uint64_t first = a.addr >> offset_bits;
    const std::uint64_t last = last_byte >> offset_bits;
    for (std::uint64_t b = first;; ++b) {
        fn(b, first != last);
        if (b == last) break;
    }
}

}  // namespace

Simulator::Simulator(const SimConfig& cfg) : cfg_(cfg) {
    validate(cfg_);
    const std::size_t n = cfg_.levels.size();
    levels_.reserve(n);
    for (std::size_t i = 0; i < n; ++i) levels_.emplace_back(cfg_.levels[i], cfg_.seed + i);
    if (cfg_.classify_misses) {
        shadows_.reserve(n);
        seen_.resize(n);
        for (std::size_t i = 0; i < n; ++i) {
            LevelConfig fa = cfg_.levels[i];
            fa.assoc = 0;  // fully associative, same size, same write policies
            fa.policy = PolicyKind::LRU;
            shadows_.emplace_back(fa, cfg_.seed);
            seen_[i].reserve(static_cast<std::size_t>(fa.lines()));
        }
    }
    stats_.levels.resize(n);
}

std::vector<std::uint64_t> Simulator::compute_next_use(const Trace& trace, std::uint64_t block_bytes,
                                                       bool include_ifetch) {
    const unsigned ob = log2_floor(block_bytes);
    std::vector<std::uint64_t> blocks;
    blocks.reserve(trace.accesses.size());
    for (const Access& a : trace.accesses) {
        if (a.op == Op::InstrFetch && !include_ifetch) continue;
        for_each_line(a, ob, [&](std::uint64_t b, bool) { blocks.push_back(b); });
    }
    // One backward pass: remember where each block is used next.
    std::vector<std::uint64_t> next(blocks.size(), kNever);
    FlatHashMap<std::uint64_t> upcoming;
    for (std::size_t i = blocks.size(); i-- > 0;) {
        if (const std::uint64_t* p = upcoming.find(blocks[i])) next[i] = *p;
        upcoming.insert_or_assign(blocks[i], i);
    }
    return next;
}

void Simulator::run(const Trace& trace, const RecordHook& after_each) {
    std::vector<std::uint64_t> next_use;
    if (cfg_.levels.front().policy == PolicyKind::OPT) {
        next_use = compute_next_use(trace, cfg_.levels.front().block_bytes, cfg_.include_ifetch);
        // Positions are global across run() calls, so shift by the start.
        run_base_ = l1_position_;
        for (auto& v : next_use) {
            if (v != kNever) v += run_base_;
        }
        next_use_ = &next_use;
    }
    try {
        if (after_each) {
            for (const Access& a : trace.accesses) {
                access_impl(a);
                after_each(a);
            }
        } else {
            for (const Access& a : trace.accesses) access_impl(a);
        }
    } catch (...) {
        next_use_ = nullptr;
        throw;
    }
    next_use_ = nullptr;
}

void Simulator::access(const Access& a) {
    if (cfg_.levels.front().policy == PolicyKind::OPT) {
        throw std::logic_error("OPT needs the whole trace in advance; use Simulator::run()");
    }
    access_impl(a);
}

void Simulator::access_impl(const Access& a) {
    ++stats_.records;
    if (a.op == Op::InstrFetch && !cfg_.include_ifetch) {
        ++stats_.skipped_ifetch;
        return;
    }
    const bool is_write = a.op == Op::Write;
    const unsigned ob = levels_.front().geometry().offset_bits;
    const std::uint64_t first_position = l1_position_;
    for_each_line(a, ob, [&](std::uint64_t block, bool split) {
        if (split && l1_position_ == first_position) ++stats_.split_records;
        const std::uint64_t nu = next_use_ ? (*next_use_)[l1_position_ - run_base_] : kNever;
        ++l1_position_;
        ++stats_.cpu_accesses;
        stats_.total_cycles += access_level(0, block << ob, is_write, AccessKind::Demand, nu);
    });
}

std::uint64_t Simulator::access_level(std::size_t i, std::uint64_t addr, bool is_write, AccessKind kind,
                                      std::uint64_t next_use) {
    if (i == levels_.size()) {
        const std::uint64_t bytes = levels_.back().geometry().block_bytes;
        MemoryStats& m = stats_.memory;
        if (is_write) {
            ++m.writes;
            m.bytes_written += bytes;
        } else {
            ++m.reads;
            m.bytes_read += bytes;
        }
        return cfg_.memory_latency;
    }

    CacheLevel& level = levels_[i];
    LevelStats& st = stats_.levels[i];
    const std::uint64_t block = level.block_of(addr);
    const CacheLevel::Result r = level.access(block, is_write, next_use);

    if (is_write) {
        ++st.writes;
        if (!r.hit) ++st.write_misses;
        if (kind == AccessKind::Writeback) ++st.writebacks_in;
    } else {
        ++st.reads;
        if (!r.hit) ++st.read_misses;
    }
    if (r.evicted) {
        ++st.evictions;
        if (r.evicted_dirty) ++st.writebacks_out;
    }

    MissType miss_type = MissType::None;
    if (cfg_.classify_misses) {
        const bool shadow_hit = shadows_[i].access(block, is_write).hit;
        FlatHashMap<std::uint8_t>& seen = seen_[i];
        if (!r.hit) {
            if (!seen.contains(block)) {
                miss_type = MissType::Compulsory;
                ++st.compulsory;
            } else if (!shadow_hit) {
                miss_type = MissType::Capacity;
                ++st.capacity;
            } else {
                miss_type = MissType::Conflict;
                ++st.conflict;
            }
        }
        if (r.filled) seen.insert(block, 1);
    }

    if (events_) {
        LevelEvent e;
        e.level = static_cast<std::uint8_t>(i);
        e.kind = kind;
        e.is_write = is_write;
        e.hit = r.hit;
        e.evicted = r.evicted;
        e.evicted_dirty = r.evicted_dirty;
        e.miss_type = miss_type;
        e.block = block;
        e.set = r.set;
        e.evicted_block = r.evicted_block;
        events_->push_back(e);
    }

    const unsigned ob = level.geometry().offset_bits;
    const std::uint64_t line_addr = block << ob;
    const bool write_through = level.config().write_policy == WritePolicy::WriteThrough;
    std::uint64_t latency = level.config().hit_latency;

    if (r.hit) {
        if (is_write && write_through) access_level(i + 1, line_addr, true, AccessKind::WriteThrough, kNever);
    } else if (!r.filled) {
        // Store miss under no-write-allocate: send the store down, no fill.
        access_level(i + 1, line_addr, true, AccessKind::WriteThrough, kNever);
    } else {
        // A write-back of a whole line of our own size overwrites every byte,
        // so there is nothing to fetch. Any other allocating miss fetches.
        const bool full_line_write = kind == AccessKind::Writeback && i > 0 &&
                                     levels_[i - 1].geometry().block_bytes == level.geometry().block_bytes;
        if (!full_line_write) latency += access_level(i + 1, line_addr, false, AccessKind::Fill, kNever);
        if (is_write && write_through) access_level(i + 1, line_addr, true, AccessKind::WriteThrough, kNever);
    }
    // The victim sits in a write buffer while the fill is served, then drains.
    if (r.evicted_dirty) {
        access_level(i + 1, r.evicted_block << ob, true, AccessKind::Writeback, kNever);
    }
    return latency;
}

}  // namespace cachesim
