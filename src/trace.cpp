#include "cachesim/trace.hpp"

#include <array>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <vector>

#include "cachesim/config.hpp"  // ParseError

namespace cachesim {
namespace {

// Lookup table: hex digit value, or -1. Built once at compile time.
constexpr std::array<std::int8_t, 256> make_hex_table() {
    std::array<std::int8_t, 256> t{};
    for (auto& v : t) v = -1;
    for (int c = '0'; c <= '9'; ++c) t[static_cast<std::size_t>(c)] = static_cast<std::int8_t>(c - '0');
    for (int c = 'a'; c <= 'f'; ++c) t[static_cast<std::size_t>(c)] = static_cast<std::int8_t>(c - 'a' + 10);
    for (int c = 'A'; c <= 'F'; ++c) t[static_cast<std::size_t>(c)] = static_cast<std::int8_t>(c - 'A' + 10);
    return t;
}
constexpr std::array<std::int8_t, 256> kHex = make_hex_table();

inline int hexval(char c) noexcept { return kHex[static_cast<unsigned char>(c)]; }
inline bool is_blank(char c) noexcept { return c == ' ' || c == '\t' || c == '\r' || c == '\v' || c == '\f'; }
inline bool is_sep(char c) noexcept { return is_blank(c) || c == ','; }

/// Map a one-character operation code to an Op. Returns false if unknown.
/// 'M' (modify) is reported through `modify`.
inline bool op_from_char(char c, Op& op, bool& modify) noexcept {
    modify = false;
    switch (c) {
        case 'L': case 'l': case 'R': case 'r': case '0': op = Op::Read; return true;
        case 'S': case 's': case 'W': case 'w': case '1': op = Op::Write; return true;
        case 'I': case 'i': case '2': op = Op::InstrFetch; return true;
        case 'M': case 'm': op = Op::Read; modify = true; return true;
        default: return false;
    }
}

// Parse every line in `text` (which must end at a line boundary or at the
// end of input) and hand each access to `sink`. A single forward pass with
// no per-line allocation; it is on the critical path for large traces.
// `line_no` and the counters in `t` carry over between calls, so the same
// code serves whole-file parsing and chunked streaming.
template <typename Sink>
void parse_lines(std::string_view text, const std::string& source, std::uint64_t& line_no, Trace& t, Sink&& sink) {
    const char* p = text.data();
    const char* const begin = p;
    const char* const end = p + text.size();

    auto fail = [&](const char* inside_line, const std::string& why) {
        const char* b = inside_line;
        while (b > begin && b[-1] != '\n') --b;
        const char* e = inside_line;
        while (e < end && *e != '\n') ++e;
        std::string snippet(b, static_cast<std::size_t>(e - b));
        while (!snippet.empty() && is_blank(snippet.back())) snippet.pop_back();
        if (snippet.size() > 60) snippet = snippet.substr(0, 60) + "...";
        throw ParseError(source, static_cast<std::size_t>(line_no), why + ": '" + snippet + "'");
    };
    auto skip_rest_of_line = [&]() {
        const void* nl = std::memchr(p, '\n', static_cast<std::size_t>(end - p));
        p = nl ? static_cast<const char*>(nl) + 1 : end;
    };

    while (p < end) {
        ++line_no;
        while (p < end && is_blank(*p)) ++p;
        if (p == end) break;
        if (*p == '\n') { ++p; continue; }

        // Comments and Valgrind banners ("==1234== ...", "--1234-- ...").
        const char c0 = *p;
        if (c0 == '#' || ((c0 == '=' || c0 == '-' || c0 == '/') && p + 1 < end && p[1] == c0)) {
            skip_rest_of_line();
            continue;
        }

        // Optional one-character operation followed by a separator.
        Op op = Op::Read;
        bool modify = false;
        const char* line_start = p;
        if (p + 1 < end && is_sep(p[1]) && op_from_char(c0, op, modify)) {
            p += 1;
            while (p < end && is_sep(*p)) ++p;
            if (p == end || *p == '\n') {
                // Just one token: it was a bare one-digit address after all.
                p = line_start;
                op = Op::Read;
                modify = false;
            }
        } else if (p + 1 < end && is_sep(p[1]) && hexval(c0) < 0) {
            fail(p, "unknown operation '" + std::string(1, c0) + "'");
        }

        // Address: optional 0x, then 1..16 hex digits.
        if (p + 1 < end && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
        std::uint64_t addr = 0;
        int digits = 0;
        for (int v; p < end && (v = hexval(*p)) >= 0; ++p, ++digits) {
            addr = (addr << 4) | static_cast<std::uint64_t>(v);
        }
        if (digits == 0 || digits > 16 || (p < end && !is_sep(*p) && *p != '\n')) {
            fail(p < end ? p : end - 1, "invalid hex address");
        }

        // Optional size (decimal, or hex with 0x).
        std::uint64_t size = 1;
        while (p < end && is_sep(*p)) ++p;
        if (p < end && *p != '\n') {
            // Saturate just above the limit so long numbers cannot wrap around
            // to a small, accepted size.
            constexpr std::uint64_t kSat = std::uint64_t{kMaxAccessSize} + 1;
            std::uint64_t v = 0;
            int n = 0;
            if (p + 1 < end && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
                p += 2;
                for (int d; p < end && (d = hexval(*p)) >= 0; ++p, ++n) {
                    v = v >= kSat ? kSat : (v << 4) | static_cast<std::uint64_t>(d);
                }
            } else {
                for (; p < end && *p >= '0' && *p <= '9'; ++p, ++n) {
                    v = v >= kSat ? kSat : v * 10 + static_cast<std::uint64_t>(*p - '0');
                }
            }
            if (n == 0 || (p < end && !is_sep(*p) && *p != '\n')) fail(p < end ? p : end - 1, "invalid access size");
            if (v > kMaxAccessSize) fail(p - 1, "access size too large");
            size = v ? v : 1;
            while (p < end && is_sep(*p)) ++p;
            if (p < end && *p != '\n') fail(p, "too many fields");
        }
        if (p < end) ++p;  // consume '\n'

        const std::uint64_t last = (size - 1 > UINT64_MAX - addr) ? UINT64_MAX : addr + size - 1;
        if (last > t.max_addr) t.max_addr = last;
        const auto sz = static_cast<std::uint32_t>(size);
        if (modify) {
            ++t.reads;
            ++t.writes;
            t.records += 2;
            sink(Access{addr, sz, Op::Read});
            sink(Access{addr, sz, Op::Write});
        } else {
            if (op == Op::Read) ++t.reads;
            else if (op == Op::Write) ++t.writes;
            else ++t.ifetches;
            ++t.records;
            sink(Access{addr, sz, op});
        }
    }
}

std::FILE* open_input(const std::string& path) {
    if (path == "-") return stdin;
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) throw std::runtime_error("cannot open trace file '" + path + "'");
    return f;
}

std::string display_name(const std::string& path) { return path == "-" ? "<stdin>" : path; }

}  // namespace

Trace parse_trace(std::string_view text, const std::string& source) {
    Trace t;
    t.source = source;
    t.accesses.reserve(text.size() / 11 + 16);  // ~11 bytes per line in the original format
    std::uint64_t line_no = 0;
    parse_lines(text, source, line_no, t, [&](const Access& a) { t.accesses.push_back(a); });
    t.text_lines = line_no;
    return t;
}

Trace load_trace(const std::string& path) {
    std::FILE* f = open_input(path);
    std::string data;
    // Read in one go when the size is known; otherwise grow chunk by chunk.
    if (f != stdin && std::fseek(f, 0, SEEK_END) == 0) {
        const long n = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        if (n > 0) {
            data.resize(static_cast<std::size_t>(n));
            data.resize(std::fread(data.data(), 1, data.size(), f));
        }
    }
    std::vector<char> buf(1 << 16);
    std::size_t got;
    while ((got = std::fread(buf.data(), 1, buf.size(), f)) > 0) data.append(buf.data(), got);
    const bool err = std::ferror(f) != 0;
    if (f != stdin) std::fclose(f);
    if (err) throw std::runtime_error("error while reading '" + display_name(path) + "'");
    return parse_trace(data, display_name(path));
}

Trace stream_trace(const std::string& path, const std::function<void(const Access&)>& on_access,
                   std::size_t chunk_bytes) {
    std::FILE* f = open_input(path);
    Trace t;
    t.source = display_name(path);
    std::uint64_t line_no = 0;
    std::vector<char> chunk(chunk_bytes ? chunk_bytes : (1 << 20));
    std::string pending;  // bytes after the last complete line
    try {
        std::size_t got;
        while ((got = std::fread(chunk.data(), 1, chunk.size(), f)) > 0) {
            pending.append(chunk.data(), got);
            const std::size_t last_nl = pending.rfind('\n');
            if (last_nl == std::string::npos) continue;  // a very long line: keep reading
            parse_lines(std::string_view(pending.data(), last_nl + 1), t.source, line_no, t, on_access);
            pending.erase(0, last_nl + 1);
        }
        if (std::ferror(f)) throw std::runtime_error("error while reading '" + t.source + "'");
        parse_lines(pending, t.source, line_no, t, on_access);
    } catch (...) {
        if (f != stdin) std::fclose(f);
        throw;
    }
    if (f != stdin) std::fclose(f);
    t.text_lines = line_no;
    return t;
}

void write_trace(std::ostream& os, const std::vector<Access>& accesses) {
    char buf[64];
    for (const Access& a : accesses) {
        const char op = a.op == Op::Write ? 'W' : (a.op == Op::InstrFetch ? 'I' : 'R');
        const int n = std::snprintf(buf, sizeof buf, "%c 0x%llx %u\n", op, static_cast<unsigned long long>(a.addr), a.size);
        os.write(buf, n);
    }
}

}  // namespace cachesim
