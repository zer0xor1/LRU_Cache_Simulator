#include "cachesim/report.hpp"

#include <cmath>
#include <cstdio>
#include <ostream>

namespace cachesim {
namespace {

std::string pad(const std::string& s, std::size_t w, bool right = false) {
    if (s.size() >= w) return s;
    return right ? std::string(w - s.size(), ' ') + s : s + std::string(w - s.size(), ' ');
}

std::string fixed(double v, int decimals) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.*f", decimals, v);
    return buf;
}

std::string pct(std::uint64_t part, std::uint64_t whole) {
    if (whole == 0) return "-";
    return fixed(100.0 * static_cast<double>(part) / static_cast<double>(whole), 3) + "%";
}

double rate(std::uint64_t part, std::uint64_t whole) {
    return whole ? static_cast<double>(part) / static_cast<double>(whole) : 0.0;
}

std::string json_num(double v) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.6f", v);
    return buf;
}

}  // namespace

std::string with_commas(std::uint64_t v) {
    std::string digits = std::to_string(v);
    std::string out;
    const std::size_t n = digits.size();
    for (std::size_t i = 0; i < n; ++i) {
        out.push_back(digits[i]);
        const std::size_t left = n - 1 - i;
        if (left && left % 3 == 0) out.push_back(',');
    }
    return out;
}

std::string write_mode_short(const LevelConfig& l) {
    std::string s = l.write_policy == WritePolicy::WriteBack ? "WB" : "WT";
    s += l.alloc_policy == AllocPolicy::WriteAllocate ? "+WA" : "+NWA";
    return s;
}

std::string assoc_label(const LevelConfig& l) {
    if (l.assoc == 0 || l.ways() == l.lines()) return "full";
    if (l.ways() == 1) return "direct";
    return std::to_string(l.ways()) + "-way";
}

std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(c);
                }
        }
    }
    return out;
}

void print_text_report(std::ostream& os, const Simulator& sim, const RunInfo& info) {
    const SimConfig& cfg = sim.config();
    const SimStats& st = sim.stats();
    const unsigned addr_bits = info.trace ? info.trace->address_bits() : 64;

    os << "cachesim " << kVersion << "\n\n";
    if (info.trace) {
        const Trace& t = *info.trace;
        os << "trace   " << t.source << "\n"
           << "        " << with_commas(t.records) << " records: " << with_commas(t.reads) << " reads, "
           << with_commas(t.writes) << " writes, " << with_commas(t.ifetches) << " ifetch"
           << (t.ifetches && !cfg.include_ifetch ? " (ignored)" : "") << ", " << addr_bits
           << "-bit addresses\n\n";
    }

    // Configuration table.
    os << pad("level", 8) << pad("size", 11) << pad("line", 7) << pad("assoc", 8) << pad("sets", 9)
       << pad("policy", 8) << pad("write", 8) << pad("tag/idx/off", 13) << "latency\n";
    for (std::size_t i = 0; i < sim.level_count(); ++i) {
        const CacheLevel& L = sim.level(i);
        const LevelConfig& c = L.config();
        const Geometry& g = L.geometry();
        const std::string bits = std::to_string(g.tag_bits(addr_bits)) + "/" + std::to_string(g.index_bits) + "/" +
                                 std::to_string(g.offset_bits);
        os << pad(c.name, 8) << pad(format_size(c.size_bytes), 11) << pad(std::to_string(c.block_bytes) + " B", 7)
           << pad(assoc_label(c), 8) << pad(with_commas(g.sets), 9) << pad(std::string(to_string(c.policy)), 8)
           << pad(write_mode_short(c), 8) << pad(bits, 13) << c.hit_latency << " cyc\n";
    }
    os << pad("memory", 8) << pad("", 11 + 7 + 8 + 9 + 8 + 8 + 13) << cfg.memory_latency << " cyc\n";
    for (const std::string& n : config_notes(cfg)) os << "note: " << n << "\n";
    os << "\n";

    // Results table.
    os << pad("level", 8) << pad("accesses", 13, true) << pad("hits", 13, true) << pad("misses", 12, true)
       << pad("miss rate", 11, true);
    if (cfg.classify_misses) os << pad("compulsory", 12, true) << pad("capacity", 11, true) << pad("conflict", 11, true);
    os << pad("evictions", 11, true) << pad("writebacks", 12, true) << "\n";
    for (std::size_t i = 0; i < sim.level_count(); ++i) {
        const LevelStats& s = st.levels[i];
        os << pad(sim.level(i).config().name, 8) << pad(with_commas(s.accesses()), 13, true)
           << pad(with_commas(s.hits()), 13, true) << pad(with_commas(s.misses()), 12, true)
           << pad(pct(s.misses(), s.accesses()), 11, true);
        if (cfg.classify_misses) {
            os << pad(with_commas(s.compulsory), 12, true) << pad(with_commas(s.capacity), 11, true)
               << pad(with_commas(s.conflict), 11, true);
        }
        os << pad(with_commas(s.evictions), 11, true) << pad(with_commas(s.writebacks_out), 12, true) << "\n";
    }
    os << "\n";

    // Read/write split for the first level (what most people look for).
    const LevelStats& l1 = st.levels.front();
    os << "L1 reads  " << with_commas(l1.reads) << " (miss " << pct(l1.read_misses, l1.reads) << ")   writes "
       << with_commas(l1.writes) << " (miss " << pct(l1.write_misses, l1.writes) << ")\n";
    os << "memory    " << with_commas(st.memory.reads) << " line reads (" << format_size(st.memory.bytes_read) << "), "
       << with_commas(st.memory.writes) << " line writes (" << format_size(st.memory.bytes_written) << ")\n";

    // AMAT, with the textbook formula next to the simulated value.
    os << "AMAT      " << fixed(st.amat(), 2) << " cycles";
    if (sim.level_count() == 1) {
        // Single level: show the textbook formula when it matches exactly
        // (it does unless no-write-allocate store misses are involved).
        const LevelStats& s = st.levels.front();
        const double m = rate(s.misses(), s.accesses());
        const double formula = sim.level(0).config().hit_latency + m * cfg.memory_latency;
        if (std::abs(formula - st.amat()) < 1e-9) {
            os << "  = " << sim.level(0).config().hit_latency << " + " << fixed(m, 4) << " x " << cfg.memory_latency;
        }
    }
    os << "\n";
    if (st.split_records) os << "note: " << with_commas(st.split_records) << " records crossed a line boundary and were split\n";

    // Timing.
    const double sim_s = info.simulate_ms / 1000.0;
    if (info.streamed) {
        os << "time      read + parse + simulate " << fixed(info.simulate_ms, 1) << " ms, streamed";
    } else {
        os << "time      parse " << fixed(info.parse_ms, 1) << " ms, simulate " << fixed(info.simulate_ms, 1) << " ms";
    }
    if (sim_s > 0) os << " (" << fixed(static_cast<double>(st.cpu_accesses) / sim_s / 1e6, 1) << " M accesses/s)";
    os << "\n";
}

void print_json_report(std::ostream& os, const Simulator& sim, const RunInfo& info) {
    const SimConfig& cfg = sim.config();
    const SimStats& st = sim.stats();
    const unsigned addr_bits = info.trace ? info.trace->address_bits() : 64;

    os << "{\n  \"version\": \"" << kVersion << "\",\n";
    if (info.trace) {
        const Trace& t = *info.trace;
        os << "  \"trace\": {\"source\": \"" << json_escape(t.source) << "\", \"records\": " << t.records
           << ", \"reads\": " << t.reads << ", \"writes\": " << t.writes << ", \"ifetches\": " << t.ifetches
           << ", \"address_bits\": " << addr_bits << "},\n";
    }
    os << "  \"config\": {\n    \"memory_latency\": " << cfg.memory_latency << ",\n    \"seed\": " << cfg.seed
       << ",\n    \"include_ifetch\": " << (cfg.include_ifetch ? "true" : "false")
       << ",\n    \"classify_misses\": " << (cfg.classify_misses ? "true" : "false") << ",\n    \"levels\": [\n";
    for (std::size_t i = 0; i < sim.level_count(); ++i) {
        const LevelConfig& c = sim.level(i).config();
        const Geometry& g = sim.level(i).geometry();
        os << "      {\"name\": \"" << json_escape(c.name) << "\", \"size_bytes\": " << c.size_bytes
           << ", \"block_bytes\": " << c.block_bytes << ", \"assoc\": " << c.assoc << ", \"ways\": " << g.ways
           << ", \"sets\": " << g.sets << ", \"policy\": \"" << to_string(c.policy) << "\", \"write_policy\": \""
           << to_string(c.write_policy) << "\", \"alloc_policy\": \"" << to_string(c.alloc_policy)
           << "\", \"hit_latency\": " << c.hit_latency << ", \"offset_bits\": " << g.offset_bits
           << ", \"index_bits\": " << g.index_bits << ", \"tag_bits\": " << g.tag_bits(addr_bits) << "}"
           << (i + 1 < sim.level_count() ? "," : "") << "\n";
    }
    os << "    ]\n  },\n";
    os << "  \"results\": {\n    \"records\": " << st.records << ",\n    \"cpu_accesses\": " << st.cpu_accesses
       << ",\n    \"skipped_ifetch\": " << st.skipped_ifetch << ",\n    \"split_records\": " << st.split_records
       << ",\n    \"total_cycles\": " << st.total_cycles << ",\n    \"amat_cycles\": " << json_num(st.amat())
       << ",\n    \"levels\": [\n";
    for (std::size_t i = 0; i < sim.level_count(); ++i) {
        const LevelStats& s = st.levels[i];
        os << "      {\"name\": \"" << json_escape(sim.level(i).config().name) << "\", \"accesses\": " << s.accesses()
           << ", \"reads\": " << s.reads << ", \"writes\": " << s.writes << ", \"hits\": " << s.hits()
           << ", \"misses\": " << s.misses() << ", \"read_misses\": " << s.read_misses
           << ", \"write_misses\": " << s.write_misses << ", \"hit_rate\": " << json_num(s.hit_rate())
           << ", \"miss_rate\": " << json_num(s.miss_rate()) << ", \"compulsory\": " << s.compulsory
           << ", \"capacity\": " << s.capacity << ", \"conflict\": " << s.conflict << ", \"evictions\": " << s.evictions
           << ", \"writebacks_in\": " << s.writebacks_in << ", \"writebacks_out\": " << s.writebacks_out << "}"
           << (i + 1 < sim.level_count() ? "," : "") << "\n";
    }
    os << "    ],\n    \"memory\": {\"reads\": " << st.memory.reads << ", \"writes\": " << st.memory.writes
       << ", \"bytes_read\": " << st.memory.bytes_read << ", \"bytes_written\": " << st.memory.bytes_written
       << "}\n  },\n";
    const double sim_s = info.simulate_ms / 1000.0;
    os << "  \"timing\": {\"streamed\": " << (info.streamed ? "true" : "false") << ", \"parse_ms\": " << json_num(info.parse_ms) << ", \"simulate_ms\": " << json_num(info.simulate_ms)
       << ", \"accesses_per_sec\": " << json_num(sim_s > 0 ? static_cast<double>(st.cpu_accesses) / sim_s : 0.0)
       << "}\n}\n";
}

void print_csv_header(std::ostream& os) {
    os << "trace,size_bytes,block_bytes,assoc,ways,sets,policy,write_policy,alloc_policy,accesses,hits,misses,"
          "miss_rate,compulsory,capacity,conflict,evictions,writebacks,memory_bytes,amat_cycles\n";
}

void print_csv_row(std::ostream& os, const std::string& trace_name, const SweepRow& r) {
    const LevelConfig& c = r.l1;
    const LevelStats& s = r.stats;
    os << trace_name << ',' << c.size_bytes << ',' << c.block_bytes << ',' << (c.assoc == 0 ? std::string("full") : std::to_string(c.assoc))
       << ',' << c.ways() << ',' << c.sets() << ',' << to_string(c.policy) << ',' << to_string(c.write_policy) << ','
       << to_string(c.alloc_policy) << ',' << s.accesses() << ',' << s.hits() << ',' << s.misses() << ','
       << json_num(s.miss_rate()) << ',' << s.compulsory << ',' << s.capacity << ',' << s.conflict << ','
       << s.evictions << ',' << s.writebacks_out << ',' << r.memory_bytes << ',' << json_num(r.amat) << '\n';
}

}  // namespace cachesim
