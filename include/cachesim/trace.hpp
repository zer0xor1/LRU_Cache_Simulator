// Memory trace loading.
//
// Every line is auto-detected, so one file may mix formats:
//
//   0x018ADB20              bare hex address -> read (original project format)
//   018ADB20                bare hex without 0x -> read
//   R 0x7ff0 8              R/W with optional size (decimal)
//   W 7ff0,4                same, comma separated
//    L 04f6b868,8           Valgrind Lackey / CMU cachelab: I, L, S, M
//    M 0421c7f0,4           M (modify) = load then store
//   0 1234                  Dinero "din" format: 0 read, 1 write, 2 ifetch
//   # comment               ignored, as are blank lines and "==123==" banners
#pragma once

#include <cstdint>
#include <functional>
#include <iosfwd>
#include <string>
#include <string_view>
#include <vector>

#include "cachesim/types.hpp"

namespace cachesim {

struct Trace {
    std::string source;
    std::vector<Access> accesses;     // empty when the trace was streamed
    std::uint64_t records = 0;        // accesses parsed (M counts as two)
    std::uint64_t text_lines = 0;     // lines in the input, including comments
    std::uint64_t max_addr = 0;
    std::uint64_t reads = 0, writes = 0, ifetches = 0;

    /// 32 if every address fits in 32 bits, otherwise 64.
    unsigned address_bits() const noexcept { return max_addr <= 0xFFFFFFFFULL ? 32 : 64; }
};

/// Largest access size accepted in a trace record.
inline constexpr std::uint32_t kMaxAccessSize = 1u << 20;

/// Parse trace text. Throws ParseError (from config.hpp) with a line number.
Trace parse_trace(std::string_view text, const std::string& source = "<trace>");

/// Load a trace file; "-" reads standard input. Throws on I/O or parse errors.
Trace load_trace(const std::string& path);

/// Parse a trace chunk by chunk and call `on_access` for every access,
/// without keeping the accesses. Memory use does not grow with the trace,
/// so multi-gigabyte Valgrind traces can be piped through stdin. Returns
/// the counters (with `accesses` left empty).
Trace stream_trace(const std::string& path, const std::function<void(const Access&)>& on_access,
                   std::size_t chunk_bytes = 1 << 20);

/// Write accesses in the canonical "R 0xADDR SIZE" format.
void write_trace(std::ostream& os, const std::vector<Access>& accesses);

}  // namespace cachesim
