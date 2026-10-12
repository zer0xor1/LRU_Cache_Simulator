// Cache hierarchy configuration and its INI-style file format.
//
//   # legacy format from the original project still works:
//   Cache Size=32768
//   Block Size=64
//   Associativity=4
//
//   # multi-level format:
//   [L1]
//   size = 32K
//   block = 64
//   assoc = 8          # or "full"
//   policy = plru      # lru | fifo | random | plru | srrip | opt
//   write = write-back # or write-through
//   allocate = yes     # write-allocate on a store miss
//   latency = 4        # hit latency in cycles
//
//   [L2]
//   size = 512K
//   ...
//
//   [memory]
//   latency = 200
#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "cachesim/types.hpp"

namespace cachesim {

struct LevelConfig {
    std::string name = "L1";
    std::uint64_t size_bytes = 32 * 1024;
    std::uint64_t block_bytes = 64;
    std::uint64_t assoc = 8;  // 0 means fully associative
    PolicyKind policy = PolicyKind::LRU;
    WritePolicy write_policy = WritePolicy::WriteBack;
    AllocPolicy alloc_policy = AllocPolicy::WriteAllocate;
    std::uint32_t hit_latency = 4;

    std::uint64_t lines() const noexcept { return block_bytes ? size_bytes / block_bytes : 0; }
    std::uint64_t ways() const noexcept { return assoc == 0 ? lines() : assoc; }
    std::uint64_t sets() const noexcept {
        const std::uint64_t w = ways();
        return w ? lines() / w : 0;
    }
};

struct SimConfig {
    std::vector<LevelConfig> levels{LevelConfig{}};
    std::uint32_t memory_latency = 200;
    std::uint64_t seed = 42;
    bool include_ifetch = false;   // treat Lackey 'I' records as reads
    bool classify_misses = true;   // 3C breakdown (costs a shadow cache per level)
};

/// Error with an optional line number, used for config and trace files.
class ParseError : public std::runtime_error {
public:
    ParseError(const std::string& source, std::size_t line, const std::string& msg);
    std::size_t line() const noexcept { return line_; }

private:
    std::size_t line_;
};

class ConfigError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Default hit latency (cycles) for the n-th level, 0-based: 4, 12, 40, 60...
std::uint32_t default_latency(std::size_t level_index) noexcept;

/// Parse "32768", "32K", "32KB", "32KiB", "1M", "0x8000". Returns nullopt on
/// error or overflow. Suffixes are binary (K = 1024).
std::optional<std::uint64_t> parse_size(std::string_view s) noexcept;

/// Parse an associativity: a positive integer, or "full"/"fa" (returns 0).
std::optional<std::uint64_t> parse_assoc(std::string_view s) noexcept;

/// Format bytes as "32 KiB", "1.5 MiB" or "48 B".
std::string format_size(std::uint64_t bytes);

/// Parse config text. Throws ParseError on syntax errors.
SimConfig parse_config(std::string_view text, const std::string& source = "<config>");

/// Read and parse a config file. Throws ParseError or std::runtime_error.
SimConfig load_config(const std::string& path);

/// Check the configuration is simulatable. Throws ConfigError with a
/// message that says what to change.
void validate(const SimConfig& cfg);

/// Notes that are not errors (e.g. "L1 has 20 sets, not a power of two").
std::vector<std::string> config_notes(const SimConfig& cfg);

}  // namespace cachesim
