// Human-readable, JSON and CSV output.
#pragma once

#include <iosfwd>
#include <string>
#include <vector>

#include "cachesim/simulator.hpp"
#include "cachesim/trace.hpp"

namespace cachesim {

inline constexpr const char* kVersion = "2.0.0";

struct RunInfo {
    const Trace* trace = nullptr;
    double parse_ms = 0.0;
    double simulate_ms = 0.0;   // includes reading and parsing when streamed
    bool streamed = false;      // trace was parsed and simulated in one pass
};

/// "1,234,567"
std::string with_commas(std::uint64_t v);

/// "WB+WA", "WT+NWA", ...
std::string write_mode_short(const LevelConfig& l);

/// "4-way", "direct", "full"
std::string assoc_label(const LevelConfig& l);

void print_text_report(std::ostream& os, const Simulator& sim, const RunInfo& info);
void print_json_report(std::ostream& os, const Simulator& sim, const RunInfo& info);

/// Escape a string for inclusion in JSON.
std::string json_escape(const std::string& s);

// ---- design-space sweep rows -------------------------------------------

struct SweepRow {
    LevelConfig l1;
    LevelStats stats;
    double amat = 0.0;
    std::uint64_t memory_bytes = 0;
};

void print_csv_header(std::ostream& os);
void print_csv_row(std::ostream& os, const std::string& trace_name, const SweepRow& row);

}  // namespace cachesim
