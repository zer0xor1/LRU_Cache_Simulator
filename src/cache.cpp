#include "cachesim/cache.hpp"

namespace cachesim {

CacheLevel::CacheLevel(const LevelConfig& cfg, std::uint64_t seed)
    : cfg_(cfg),
      write_back_(cfg.write_policy == WritePolicy::WriteBack),
      write_allocate_(cfg.alloc_policy == AllocPolicy::WriteAllocate) {
    geo_.size_bytes = cfg.size_bytes;
    geo_.block_bytes = cfg.block_bytes;
    geo_.ways = static_cast<std::uint32_t>(cfg.ways());
    geo_.sets = cfg.sets();
    geo_.offset_bits = log2_floor(cfg.block_bytes);
    geo_.pow2_sets = is_pow2(geo_.sets);
    geo_.index_bits = log2_ceil(geo_.sets);
    set_mask_ = geo_.sets - 1;

    const std::size_t lines = static_cast<std::size_t>(geo_.sets) * geo_.ways;
    blocks_.assign(lines, 0);
    dirty_.assign(lines, 0);
    fill_count_.assign(static_cast<std::size_t>(geo_.sets), 0);
    use_index_ = geo_.ways > kIndexThreshold;
    if (use_index_) index_.reserve(lines);
    policy_ = make_policy(cfg.policy, static_cast<std::size_t>(geo_.sets), geo_.ways, seed);
}

std::int64_t CacheLevel::find_way(std::uint64_t set, std::uint64_t block) const noexcept {
    if (use_index_) {
        const std::uint32_t* line = index_.find(block);
        return line ? static_cast<std::int64_t>(*line - set * geo_.ways) : -1;
    }
    const std::uint64_t* p = blocks_.data() + set * geo_.ways;
    const std::uint32_t n = fill_count_[set];
    for (std::uint32_t w = 0; w < n; ++w) {
        if (p[w] == block) return w;
    }
    return -1;
}

CacheLevel::Result CacheLevel::access(std::uint64_t block, bool is_write, std::uint64_t next_use) {
    Result r;
    r.set = set_of(block);
    const std::int64_t found = find_way(r.set, block);

    if (found >= 0) {
        r.hit = true;
        r.way = static_cast<std::uint32_t>(found);
        if (is_write && write_back_) dirty_[r.set * geo_.ways + r.way] = 1;
        policy_->on_hit(r.set, r.way, next_use);
        return r;
    }

    // Miss. A store under no-write-allocate goes around the cache.
    if (is_write && !write_allocate_) return r;

    std::uint32_t& count = fill_count_[r.set];
    if (count < geo_.ways) {
        r.way = count++;
    } else {
        r.way = policy_->victim(r.set);
        const std::size_t line = r.set * geo_.ways + r.way;
        r.evicted = true;
        r.evicted_block = blocks_[line];
        r.evicted_dirty = dirty_[line] != 0;
        if (use_index_) index_.erase(r.evicted_block);
    }

    const std::size_t line = r.set * geo_.ways + r.way;
    blocks_[line] = block;
    dirty_[line] = (is_write && write_back_) ? 1 : 0;
    if (use_index_) index_.insert_or_assign(block, static_cast<std::uint32_t>(line));
    policy_->on_fill(r.set, r.way, next_use);
    r.filled = true;
    return r;
}

bool CacheLevel::contains(std::uint64_t block) const noexcept {
    return find_way(set_of(block), block) >= 0;
}

bool CacheLevel::is_dirty(std::uint64_t block) const noexcept {
    const std::uint64_t set = set_of(block);
    const std::int64_t w = find_way(set, block);
    return w >= 0 && dirty_[set * geo_.ways + static_cast<std::uint64_t>(w)] != 0;
}

std::vector<std::uint64_t> CacheLevel::set_contents(std::uint64_t set) const {
    const std::uint64_t* p = blocks_.data() + set * geo_.ways;
    return std::vector<std::uint64_t>(p, p + fill_count_[set]);
}

}  // namespace cachesim
