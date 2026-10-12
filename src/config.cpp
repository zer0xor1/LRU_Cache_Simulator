#include "cachesim/config.hpp"

#include <cctype>
#include <cstdio>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>

namespace cachesim {
namespace {

std::string_view trim(std::string_view s) noexcept {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

/// Lowercase and keep only letters and digits: "Cache Size" -> "cachesize".
std::string normalize(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        const auto u = static_cast<unsigned char>(c);
        if (std::isalnum(u)) out.push_back(static_cast<char>(std::tolower(u)));
    }
    return out;
}

bool one_of(const std::string& k, std::initializer_list<const char*> names) {
    for (const char* n : names) {
        if (k == n) return true;
    }
    return false;
}

std::optional<std::uint64_t> parse_uint(std::string_view s) noexcept {
    s = trim(s);
    if (s.empty()) return std::nullopt;
    int base = 10;
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        s.remove_prefix(2);
    }
    std::uint64_t v = 0;
    for (char c : s) {
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (base == 16 && c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (base == 16 && c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return std::nullopt;
        if (v > (std::numeric_limits<std::uint64_t>::max() - static_cast<std::uint64_t>(d)) /
                    static_cast<std::uint64_t>(base)) {
            return std::nullopt;
        }
        v = v * static_cast<std::uint64_t>(base) + static_cast<std::uint64_t>(d);
    }
    return v;
}

std::optional<bool> parse_bool(std::string_view s) {
    const std::string k = normalize(s);
    if (one_of(k, {"yes", "y", "true", "on", "1"})) return true;
    if (one_of(k, {"no", "n", "false", "off", "0"})) return false;
    return std::nullopt;
}

std::optional<WritePolicy> parse_write_policy(std::string_view s) {
    const std::string k = normalize(s);
    if (one_of(k, {"writeback", "wb", "back", "copyback"})) return WritePolicy::WriteBack;
    if (one_of(k, {"writethrough", "wt", "through"})) return WritePolicy::WriteThrough;
    return std::nullopt;
}

std::optional<AllocPolicy> parse_alloc_policy(std::string_view s) {
    const std::string k = normalize(s);
    if (one_of(k, {"writeallocate", "wa", "allocate", "fetchonwrite"})) return AllocPolicy::WriteAllocate;
    if (one_of(k, {"nowriteallocate", "nwa", "noallocate", "writearound", "around", "writenoallocate"})) {
        return AllocPolicy::NoWriteAllocate;
    }
    if (auto b = parse_bool(s)) return *b ? AllocPolicy::WriteAllocate : AllocPolicy::NoWriteAllocate;
    return std::nullopt;
}

enum class Section { Level, Memory, Simulation };

}  // namespace

ParseError::ParseError(const std::string& source, std::size_t line, const std::string& msg)
    : std::runtime_error(line ? source + ":" + std::to_string(line) + ": " + msg : source + ": " + msg),
      line_(line) {}

std::uint32_t default_latency(std::size_t level_index) noexcept {
    static constexpr std::uint32_t table[] = {4, 12, 40, 60};
    return level_index < 4 ? table[level_index] : 80;
}

std::optional<std::uint64_t> parse_size(std::string_view s) noexcept {
    s = trim(s);
    if (s.empty()) return std::nullopt;
    // Split the numeric part from an optional unit suffix.
    std::size_t i = 0;
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        i = 2;
        while (i < s.size() && std::isxdigit(static_cast<unsigned char>(s[i]))) ++i;
    } else {
        while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
    }
    auto num = parse_uint(s.substr(0, i));
    if (!num) return std::nullopt;
    std::string unit;
    for (char c : trim(s.substr(i))) unit.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    unsigned shift = 0;
    if (unit.empty() || unit == "b") shift = 0;
    else if (unit == "k" || unit == "kb" || unit == "kib") shift = 10;
    else if (unit == "m" || unit == "mb" || unit == "mib") shift = 20;
    else if (unit == "g" || unit == "gb" || unit == "gib") shift = 30;
    else return std::nullopt;
    if (shift && *num > (std::numeric_limits<std::uint64_t>::max() >> shift)) return std::nullopt;
    return *num << shift;
}

std::optional<std::uint64_t> parse_assoc(std::string_view s) noexcept {
    std::string k;
    for (char c : s) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            k.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
    }
    if (k == "full" || k == "fully" || k == "fa" || k == "fullyassociative") return 0;
    auto v = parse_uint(s);
    if (!v || *v == 0) return std::nullopt;
    return v;
}

std::string format_size(std::uint64_t bytes) {
    struct Unit { std::uint64_t scale; const char* name; };
    static constexpr Unit units[] = {{1ULL << 30, "GiB"}, {1ULL << 20, "MiB"}, {1ULL << 10, "KiB"}};
    for (const auto& u : units) {
        if (bytes >= u.scale) {
            char buf[48];
            if (bytes % u.scale == 0) {
                std::snprintf(buf, sizeof buf, "%llu %s", static_cast<unsigned long long>(bytes / u.scale), u.name);
            } else {
                std::snprintf(buf, sizeof buf, "%.2f %s", static_cast<double>(bytes) / static_cast<double>(u.scale), u.name);
            }
            return buf;
        }
    }
    return std::to_string(bytes) + " B";
}

SimConfig parse_config(std::string_view text, const std::string& source) {
    SimConfig cfg;
    cfg.levels.clear();
    Section section = Section::Level;
    bool level_open = false;  // keys before any [section] create an implicit L1
    std::set<std::string> names;

    auto open_level = [&](std::string name, std::size_t line_no) {
        if (name.empty()) name = "L" + std::to_string(cfg.levels.size() + 1);
        if (!names.insert(normalize(name)).second) {
            throw ParseError(source, line_no, "duplicate level [" + name + "]");
        }
        LevelConfig lvl;
        lvl.name = name;
        lvl.hit_latency = default_latency(cfg.levels.size());
        cfg.levels.push_back(lvl);
        level_open = true;
    };

    std::size_t line_no = 0;
    std::size_t pos = 0;
    while (pos <= text.size()) {
        const std::size_t eol = text.find('\n', pos);
        std::string_view raw = text.substr(pos, eol == std::string_view::npos ? std::string_view::npos : eol - pos);
        pos = (eol == std::string_view::npos) ? text.size() + 1 : eol + 1;
        ++line_no;

        if (const auto c = raw.find_first_of("#;"); c != std::string_view::npos) raw = raw.substr(0, c);
        std::string_view line = trim(raw);
        if (line.empty()) continue;

        if (line.front() == '[') {
            if (line.back() != ']') throw ParseError(source, line_no, "section header must end with ']'");
            const std::string_view name = trim(line.substr(1, line.size() - 2));
            const std::string k = normalize(name);
            if (one_of(k, {"memory", "mem", "dram", "mainmemory"})) {
                section = Section::Memory;
            } else if (one_of(k, {"simulation", "sim", "global", "options"})) {
                section = Section::Simulation;
            } else {
                if (k.empty()) throw ParseError(source, line_no, "empty section name");
                section = Section::Level;
                open_level(std::string(name), line_no);
            }
            continue;
        }

        const std::size_t eq = line.find_first_of("=:");
        if (eq == std::string_view::npos) {
            throw ParseError(source, line_no, "expected 'key = value', got '" + std::string(line) + "'");
        }
        const std::string key = normalize(line.substr(0, eq));
        const std::string_view value = trim(line.substr(eq + 1));
        auto bad = [&](const char* what) {
            return ParseError(source, line_no, std::string("invalid ") + what + " '" + std::string(value) + "'");
        };

        // Keys that are valid in any section.
        if (key == "seed") {
            auto v = parse_uint(value);
            if (!v) throw bad("seed");
            cfg.seed = *v;
            continue;
        }
        if (one_of(key, {"ifetch", "includeifetch", "instructions"})) {
            auto v = parse_bool(value);
            if (!v) throw bad("boolean");
            cfg.include_ifetch = *v;
            continue;
        }
        if (one_of(key, {"classify", "3c", "classifymisses", "missclassification"})) {
            auto v = parse_bool(value);
            if (!v) throw bad("boolean");
            cfg.classify_misses = *v;
            continue;
        }
        if (key == "memorylatency" ||
            (section == Section::Memory && one_of(key, {"latency", "accesstime", "cycles", "hitlatency"}))) {
            auto v = parse_uint(value);
            if (!v || *v > 1'000'000) throw bad("latency");
            cfg.memory_latency = static_cast<std::uint32_t>(*v);
            continue;
        }
        if (section != Section::Level) {
            throw ParseError(source, line_no, "unknown key '" + std::string(line.substr(0, eq)) + "' in this section");
        }

        if (!level_open) open_level("L1", line_no);
        LevelConfig& lvl = cfg.levels.back();

        if (one_of(key, {"cachesize", "size", "capacity", "totalsize"})) {
            auto v = parse_size(value);
            if (!v) throw bad("size");
            lvl.size_bytes = *v;
        } else if (one_of(key, {"blocksize", "block", "linesize", "line", "blockbytes"})) {
            auto v = parse_size(value);
            if (!v) throw bad("block size");
            lvl.block_bytes = *v;
        } else if (one_of(key, {"associativity", "assoc", "ways", "setassociativity"})) {
            auto v = parse_assoc(value);
            if (!v) throw bad("associativity");
            lvl.assoc = *v;
        } else if (one_of(key, {"replacement", "policy", "replacementpolicy", "eviction"})) {
            auto v = parse_policy(value);
            if (!v) throw bad("policy (use lru, fifo, random, plru, srrip or opt)");
            lvl.policy = *v;
        } else if (one_of(key, {"writepolicy", "write", "writemode"})) {
            auto v = parse_write_policy(value);
            if (!v) throw bad("write policy (use write-back or write-through)");
            lvl.write_policy = *v;
        } else if (one_of(key, {"writeallocate", "allocate", "allocation", "allocatepolicy", "writemiss"})) {
            auto v = parse_alloc_policy(value);
            if (!v) throw bad("allocation policy (use yes or no)");
            lvl.alloc_policy = *v;
        } else if (one_of(key, {"hitlatency", "latency", "hittime", "cycles", "accesstime"})) {
            auto v = parse_uint(value);
            if (!v || *v > 1'000'000) throw bad("latency");
            lvl.hit_latency = static_cast<std::uint32_t>(*v);
        } else if (key == "name") {
            lvl.name = std::string(value);
        } else {
            throw ParseError(source, line_no, "unknown key '" + std::string(trim(line.substr(0, eq))) + "'");
        }
    }

    if (cfg.levels.empty()) throw ParseError(source, 0, "no cache level defined");
    return cfg;
}

SimConfig load_config(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open config file '" + path + "'");
    std::ostringstream ss;
    ss << in.rdbuf();
    return parse_config(ss.str(), path);
}

void validate(const SimConfig& cfg) {
    if (cfg.levels.empty()) throw ConfigError("at least one cache level is required");
    if (cfg.levels.size() > 8) throw ConfigError("at most 8 cache levels are supported");
    constexpr std::uint64_t kMaxLines = 1ULL << 26;  // 64 Mi lines keeps memory use sane

    for (std::size_t i = 0; i < cfg.levels.size(); ++i) {
        const LevelConfig& l = cfg.levels[i];
        const std::string who = l.name + ": ";
        if (l.block_bytes == 0 || !is_pow2(l.block_bytes)) {
            throw ConfigError(who + "block size must be a power of two (got " + std::to_string(l.block_bytes) + ")");
        }
        if (l.size_bytes == 0) throw ConfigError(who + "cache size must be positive");
        if (l.size_bytes % l.block_bytes != 0) {
            throw ConfigError(who + "cache size " + std::to_string(l.size_bytes) +
                              " is not a multiple of the block size " + std::to_string(l.block_bytes));
        }
        const std::uint64_t lines = l.lines();
        if (lines > kMaxLines) {
            throw ConfigError(who + "too many lines (" + std::to_string(lines) + "); the limit is " +
                              std::to_string(kMaxLines));
        }
        if (l.assoc > lines) {
            throw ConfigError(who + "associativity " + std::to_string(l.assoc) + " is larger than the " +
                              std::to_string(lines) + " lines in the cache (size must be >= block x ways)");
        }
        if (lines % l.ways() != 0) {
            throw ConfigError(who + "cache size must be a multiple of block size x associativity (" +
                              std::to_string(l.block_bytes * l.ways()) + " bytes)");
        }
        if (l.policy == PolicyKind::PLRU && !is_pow2(l.ways())) {
            throw ConfigError(who + "PLRU needs a power-of-two number of ways (got " + std::to_string(l.ways()) + ")");
        }
        if (l.policy == PolicyKind::OPT && i != 0) {
            throw ConfigError(who + "OPT is only supported in the first level, because only that level's "
                                    "future access stream is known before the run");
        }
        if (i > 0 && l.block_bytes < cfg.levels[i - 1].block_bytes) {
            throw ConfigError(who + "block size must be >= the block size of " + cfg.levels[i - 1].name);
        }
    }
}

std::vector<std::string> config_notes(const SimConfig& cfg) {
    std::vector<std::string> notes;
    for (std::size_t i = 0; i < cfg.levels.size(); ++i) {
        const LevelConfig& l = cfg.levels[i];
        if (l.sets() > 0 && !is_pow2(l.sets())) {
            notes.push_back(l.name + " has " + std::to_string(l.sets()) +
                            " sets (not a power of two), so the set index is block_number mod " +
                            std::to_string(l.sets()) + ". Real hardware usually needs a power of two.");
        }
        if (i > 0 && l.size_bytes < cfg.levels[i - 1].size_bytes) {
            notes.push_back(l.name + " is smaller than " + cfg.levels[i - 1].name + ".");
        }
    }
    return notes;
}

}  // namespace cachesim
