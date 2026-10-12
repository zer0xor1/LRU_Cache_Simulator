// A small open-addressing hash map with 64-bit integer keys.
//
// Why not std::unordered_map? The simulator does one lookup per access in
// the fully associative paths (the 3C shadow cache and caches with many
// ways). std::unordered_map allocates a node per element and chases a
// pointer per lookup. This table stores slots in one flat array, uses
// linear probing and Fibonacci hashing, and deletes with backward shifting
// (no tombstones), so probe chains never degrade over time.
#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace cachesim {

template <typename V>
class FlatHashMap {
public:
    FlatHashMap() = default;
    explicit FlatHashMap(std::size_t expected) { reserve(expected); }

    /// Make room for `n` elements without rehashing (load factor <= 0.5).
    void reserve(std::size_t n) {
        std::size_t need = 16;
        while (need < n * 2) need <<= 1;
        if (need > slots_.size()) rehash(need);
    }

    std::size_t size() const noexcept { return size_; }
    bool empty() const noexcept { return size_ == 0; }

    V* find(std::uint64_t key) noexcept {
        if (slots_.empty()) return nullptr;
        for (std::size_t i = home(key);; i = (i + 1) & mask_) {
            Slot& s = slots_[i];
            if (!s.used) return nullptr;
            if (s.key == key) return &s.value;
        }
    }
    const V* find(std::uint64_t key) const noexcept {
        return const_cast<FlatHashMap*>(this)->find(key);
    }
    bool contains(std::uint64_t key) const noexcept { return find(key) != nullptr; }

    /// Insert or overwrite. Returns true if the key was new.
    bool insert_or_assign(std::uint64_t key, V value) {
        if ((size_ + 1) * 2 > slots_.size()) rehash(slots_.empty() ? 16 : slots_.size() * 2);
        for (std::size_t i = home(key);; i = (i + 1) & mask_) {
            Slot& s = slots_[i];
            if (!s.used) {
                s.used = true;
                s.key = key;
                s.value = std::move(value);
                ++size_;
                return true;
            }
            if (s.key == key) {
                s.value = std::move(value);
                return false;
            }
        }
    }

    /// Insert only if absent. Returns true if the key was new.
    bool insert(std::uint64_t key, V value) {
        if (find(key) != nullptr) return false;
        return insert_or_assign(key, std::move(value));
    }

    /// Remove `key`. Uses backward-shift deletion so no tombstones are left.
    bool erase(std::uint64_t key) noexcept {
        if (slots_.empty()) return false;
        std::size_t i = home(key);
        for (;; i = (i + 1) & mask_) {
            if (!slots_[i].used) return false;
            if (slots_[i].key == key) break;
        }
        // Shift later members of the probe chain back into the hole when
        // their home slot does not lie cyclically inside (hole, j].
        std::size_t j = i;
        for (;;) {
            j = (j + 1) & mask_;
            if (!slots_[j].used) break;
            const std::size_t k = home(slots_[j].key);
            const bool stays = (i <= j) ? (i < k && k <= j) : (i < k || k <= j);
            if (stays) continue;
            slots_[i] = std::move(slots_[j]);
            i = j;
        }
        slots_[i].used = false;
        --size_;
        return true;
    }

    void clear() noexcept {
        for (auto& s : slots_) s.used = false;
        size_ = 0;
    }

private:
    struct Slot {
        std::uint64_t key = 0;
        V value{};
        bool used = false;
    };

    std::size_t home(std::uint64_t key) const noexcept {
        // Fibonacci hashing: multiply by 2^64 / phi and keep the top bits.
        return static_cast<std::size_t>((key * 0x9E3779B97F4A7C15ULL) >> shift_);
    }

    void rehash(std::size_t new_cap) {
        std::vector<Slot> old = std::move(slots_);
        slots_.assign(new_cap, Slot{});
        mask_ = new_cap - 1;
        unsigned bits = 0;
        while ((std::size_t{1} << bits) < new_cap) ++bits;
        shift_ = 64 - bits;
        size_ = 0;
        for (auto& s : old) {
            if (s.used) insert_or_assign(s.key, std::move(s.value));
        }
    }

    std::vector<Slot> slots_;
    std::size_t mask_ = 0;
    std::size_t size_ = 0;
    unsigned shift_ = 64;
};

}  // namespace cachesim
