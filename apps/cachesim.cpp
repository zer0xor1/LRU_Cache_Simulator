// cachesim: command-line front end.
//
//   cachesim traces/example.trc                       # 32 KiB, 64 B lines, 8-way LRU
//   cachesim -s 16K -b 64 -a 4 -p plru traces/example.trc
//   cachesim configs/example.cfg traces/example.trc   # original two-file usage
//   cachesim -c configs/intel_like.cfg --json trace.trc
//   cachesim --sweep --sizes 1K-1M --assocs 1,2,4,8,full traces/example.trc > sweep.csv
//   valgrind --tool=lackey --trace-mem=yes ./prog 2>&1 | cachesim -
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "cachesim/config.hpp"
#include "cachesim/report.hpp"
#include "cachesim/simulator.hpp"
#include "cachesim/trace.hpp"

using namespace cachesim;

namespace {

constexpr const char* kUsage = R"(usage: cachesim [options] TRACE
       cachesim [options] CONFIG TRACE

Simulate a CPU cache hierarchy on a memory trace. TRACE may be "-" for stdin.

Cache geometry (first level; also override values from a config file):
  -s, --size SIZE          total size, e.g. 32768, 32K, 1M        (default 32K)
  -b, --block SIZE         line size in bytes, power of two        (default 64)
  -a, --assoc N|full       ways per set; 1 = direct mapped         (default 8)
  -p, --policy NAME        lru | fifo | random | plru | srrip | opt (default lru)
      --write-through      write-through instead of write-back
      --no-write-allocate  store misses bypass the cache
  -l, --latency N          hit latency in cycles                   (default 4)
  -m, --mem-latency N      memory latency in cycles                (default 200)
  -c, --config FILE        INI config with one or more levels ([L1], [L2], ...)

Simulation:
      --ifetch             include instruction fetches (Lackey 'I' records)
      --no-3c              skip compulsory/capacity/conflict classification
      --seed N             seed for the random policy               (default 42)
      --batch              load the whole trace before simulating (the default
                           streams it in constant memory, except for OPT)

Output:
      --json               machine-readable JSON report
  -v, --verbose            print every access and what each level did
  -q, --quiet              print only the miss rate of each level

Design-space sweep (prints CSV, runs configurations in parallel):
      --sweep              sweep the first level over the lists below
      --sizes LIST         e.g. 4K,8K,16K or 1K-1M (powers of two)  (default 1K-1M)
      --blocks LIST        e.g. 32,64,128                          (default: --block)
      --assocs LIST        e.g. 1,2,4,8,full                       (default 1,2,4,8,16,full)
      --policies LIST      e.g. lru,fifo,plru,opt                  (default: --policy)
  -j, --jobs N             worker threads                          (default: all cores)
  -o, --output FILE        write CSV to FILE instead of stdout

  -h, --help               show this help
      --version            show the version
)";

struct Options {
    std::optional<std::string> config_path;
    std::optional<std::uint64_t> size, block, assoc;
    std::optional<PolicyKind> policy;
    bool write_through = false;
    bool no_write_allocate = false;
    std::optional<std::uint32_t> latency, mem_latency;
    bool ifetch = false;
    bool no_3c = false;
    std::optional<std::uint64_t> seed;
    bool json = false, verbose = false, quiet = false;
    bool sweep = false;
    bool batch = false;
    std::string sizes = "1K-1M", blocks, assocs = "1,2,4,8,16,full", policies;
    unsigned jobs = 0;
    std::string output;
    std::vector<std::string> positional;
};

[[noreturn]] void usage_error(const std::string& msg) {
    std::cerr << "cachesim: " << msg << "\nTry 'cachesim --help'.\n";
    std::exit(1);
}

std::uint64_t need_size(const std::string& opt, const std::string& v) {
    auto s = parse_size(v);
    if (!s) usage_error("invalid value for " + opt + ": '" + v + "'");
    return *s;
}

std::uint32_t need_u32(const std::string& opt, const std::string& v) {
    auto s = parse_size(v);
    if (!s || *s > 1'000'000'000ULL) usage_error("invalid value for " + opt + ": '" + v + "'");
    return static_cast<std::uint32_t>(*s);
}

Options parse_args(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        std::string value;
        bool has_inline = false;
        if (arg.rfind("--", 0) == 0) {
            const auto eq = arg.find('=');
            if (eq != std::string::npos) {
                value = arg.substr(eq + 1);
                arg = arg.substr(0, eq);
                has_inline = true;
            }
        }
        auto next = [&]() -> std::string {
            if (has_inline) return value;
            if (i + 1 >= argc) usage_error("option " + arg + " needs a value");
            return argv[++i];
        };

        if (arg == "-h" || arg == "--help") {
            std::cout << kUsage;
            std::exit(0);
        } else if (arg == "--version") {
            std::cout << "cachesim " << kVersion << "\n";
            std::exit(0);
        } else if (arg == "-s" || arg == "--size") {
            o.size = need_size(arg, next());
        } else if (arg == "-b" || arg == "--block") {
            o.block = need_size(arg, next());
        } else if (arg == "-a" || arg == "--assoc") {
            const std::string v = next();
            auto a = parse_assoc(v);
            if (!a) usage_error("invalid associativity '" + v + "'");
            o.assoc = *a;
        } else if (arg == "-p" || arg == "--policy") {
            const std::string v = next();
            auto p = parse_policy(v);
            if (!p) usage_error("unknown policy '" + v + "' (use lru, fifo, random, plru, srrip or opt)");
            o.policy = *p;
        } else if (arg == "--write-through") {
            o.write_through = true;
        } else if (arg == "--no-write-allocate") {
            o.no_write_allocate = true;
        } else if (arg == "-l" || arg == "--latency") {
            o.latency = need_u32(arg, next());
        } else if (arg == "-m" || arg == "--mem-latency") {
            o.mem_latency = need_u32(arg, next());
        } else if (arg == "-c" || arg == "--config") {
            o.config_path = next();
        } else if (arg == "--ifetch") {
            o.ifetch = true;
        } else if (arg == "--batch") {
            o.batch = true;
        } else if (arg == "--no-3c") {
            o.no_3c = true;
        } else if (arg == "--seed") {
            auto v = parse_size(next());
            if (!v) usage_error("invalid seed");
            o.seed = *v;
        } else if (arg == "--json") {
            o.json = true;
        } else if (arg == "-v" || arg == "--verbose") {
            o.verbose = true;
        } else if (arg == "-q" || arg == "--quiet") {
            o.quiet = true;
        } else if (arg == "--sweep") {
            o.sweep = true;
        } else if (arg == "--sizes") {
            o.sizes = next();
        } else if (arg == "--blocks") {
            o.blocks = next();
        } else if (arg == "--assocs") {
            o.assocs = next();
        } else if (arg == "--policies") {
            o.policies = next();
        } else if (arg == "-j" || arg == "--jobs") {
            o.jobs = need_u32(arg, next());
        } else if (arg == "-o" || arg == "--output") {
            o.output = next();
        } else if (arg.size() > 1 && arg[0] == '-' && arg != "-") {
            usage_error("unknown option '" + arg + "'");
        } else {
            o.positional.push_back(arg);
        }
    }
    return o;
}

SimConfig build_config(const Options& o, std::string& trace_path) {
    if (o.positional.empty()) usage_error("missing TRACE argument");
    if (o.positional.size() > 2) usage_error("too many arguments");
    std::optional<std::string> cfg_path = o.config_path;
    if (o.positional.size() == 2) {
        if (cfg_path) usage_error("give the config either with -c or as the first argument, not both");
        cfg_path = o.positional[0];
        trace_path = o.positional[1];
    } else {
        trace_path = o.positional[0];
    }

    SimConfig cfg;
    if (cfg_path) cfg = load_config(*cfg_path);
    LevelConfig& l1 = cfg.levels.front();
    if (o.size) l1.size_bytes = *o.size;
    if (o.block) l1.block_bytes = *o.block;
    if (o.assoc) l1.assoc = *o.assoc;
    if (o.policy) l1.policy = *o.policy;
    if (o.write_through) l1.write_policy = WritePolicy::WriteThrough;
    if (o.no_write_allocate) l1.alloc_policy = AllocPolicy::NoWriteAllocate;
    if (o.latency) l1.hit_latency = *o.latency;
    if (o.mem_latency) cfg.memory_latency = *o.mem_latency;
    if (o.ifetch) cfg.include_ifetch = true;
    if (o.no_3c) cfg.classify_misses = false;
    if (o.seed) cfg.seed = *o.seed;
    return cfg;
}

double ms_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

// ---- verbose mode ----------------------------------------------------------

void print_events(std::ostream& os, const Access& a, const std::vector<LevelEvent>& events, const Simulator& sim) {
    char head[64];
    std::snprintf(head, sizeof head, "%s 0x%llx %u", std::string(to_string(a.op)).c_str(),
                  static_cast<unsigned long long>(a.addr), a.size);
    os << head;
    if (events.empty()) {
        os << "  (skipped)\n";
        return;
    }
    for (const LevelEvent& e : events) {
        const LevelConfig& c = sim.level(e.level).config();
        os << "  | " << c.name;
        if (e.kind != AccessKind::Demand) os << ' ' << to_string(e.kind);
        os << ' ' << (e.hit ? "hit" : "miss");
        if (e.miss_type != MissType::None) os << '(' << to_string(e.miss_type) << ')';
        os << " set " << e.set;
        if (e.evicted) {
            char buf[48];
            std::snprintf(buf, sizeof buf, " evict 0x%llx",
                          static_cast<unsigned long long>(e.evicted_block << sim.level(e.level).geometry().offset_bits));
            os << buf << (e.evicted_dirty ? " (dirty)" : "");
        }
    }
    os << '\n';
}

// ---- sweep mode -------------------------------------------------------------

std::vector<std::uint64_t> parse_size_list(const std::string& s, const char* what) {
    std::vector<std::uint64_t> out;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ',')) {
        const auto dash = item.find('-');
        if (dash != std::string::npos && dash > 0) {
            auto lo = parse_size(item.substr(0, dash));
            auto hi = parse_size(item.substr(dash + 1));
            if (!lo || !hi || *lo == 0 || *lo > *hi) usage_error(std::string("invalid range in ") + what + ": '" + item + "'");
            for (std::uint64_t v = *lo; v <= *hi; v *= 2) {
                out.push_back(v);
                if (v > (UINT64_MAX >> 1)) break;
            }
        } else {
            auto v = parse_size(item);
            if (!v) usage_error(std::string("invalid value in ") + what + ": '" + item + "'");
            out.push_back(*v);
        }
    }
    if (out.empty()) usage_error(std::string("empty list for ") + what);
    return out;
}

int run_sweep(const Options& o, const SimConfig& base, const Trace& trace) {
    const auto sizes = parse_size_list(o.sizes, "--sizes");
    const auto blocks = o.blocks.empty() ? std::vector<std::uint64_t>{base.levels.front().block_bytes}
                                         : parse_size_list(o.blocks, "--blocks");
    std::vector<std::uint64_t> assocs;
    {
        std::stringstream ss(o.assocs);
        std::string item;
        while (std::getline(ss, item, ',')) {
            auto a = parse_assoc(item);
            if (!a) usage_error("invalid value in --assocs: '" + item + "'");
            assocs.push_back(*a);
        }
    }
    std::vector<PolicyKind> policies;
    if (o.policies.empty()) {
        policies.push_back(base.levels.front().policy);
    } else {
        std::stringstream ss(o.policies);
        std::string item;
        while (std::getline(ss, item, ',')) {
            auto p = parse_policy(item);
            if (!p) usage_error("unknown policy in --policies: '" + item + "'");
            policies.push_back(*p);
        }
    }

    // Build every valid configuration up front.
    std::vector<SimConfig> jobs;
    std::size_t skipped = 0;
    for (PolicyKind p : policies) {
        for (std::uint64_t b : blocks) {
            for (std::uint64_t a : assocs) {
                for (std::uint64_t s : sizes) {
                    SimConfig cfg = base;
                    LevelConfig& l1 = cfg.levels.front();
                    l1.size_bytes = s;
                    l1.block_bytes = b;
                    l1.assoc = a;
                    l1.policy = p;
                    try {
                        validate(cfg);
                        jobs.push_back(std::move(cfg));
                    } catch (const ConfigError&) {
                        ++skipped;  // e.g. 8-way in a 4-line cache
                    }
                }
            }
        }
    }

    std::vector<SweepRow> rows(jobs.size());
    std::atomic<std::size_t> next{0};
    std::mutex err_mu;
    std::string first_error;
    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    const unsigned n_threads = std::max(1u, std::min<unsigned>(o.jobs ? o.jobs : hw, static_cast<unsigned>(jobs.size())));

    const auto t0 = std::chrono::steady_clock::now();
    auto worker = [&]() {
        for (std::size_t i = next++; i < jobs.size(); i = next++) {
            try {
                Simulator sim(jobs[i]);
                sim.run(trace);
                rows[i].l1 = jobs[i].levels.front();
                rows[i].stats = sim.stats().levels.front();
                rows[i].amat = sim.stats().amat();
                rows[i].memory_bytes = sim.stats().memory.bytes_read + sim.stats().memory.bytes_written;
            } catch (const std::exception& e) {
                std::lock_guard<std::mutex> lock(err_mu);
                if (first_error.empty()) first_error = e.what();
            }
        }
    };
    std::vector<std::thread> pool;
    for (unsigned t = 0; t < n_threads; ++t) pool.emplace_back(worker);
    for (auto& th : pool) th.join();
    if (!first_error.empty()) {
        std::cerr << "cachesim: error: " << first_error << "\n";
        return 2;
    }

    std::ofstream file;
    std::ostream* out = &std::cout;
    if (!o.output.empty()) {
        file.open(o.output);
        if (!file) {
            std::cerr << "cachesim: error: cannot write '" << o.output << "'\n";
            return 2;
        }
        out = &file;
    }
    print_csv_header(*out);
    for (const auto& r : rows) print_csv_row(*out, trace.source, r);
    std::cerr << "swept " << rows.size() << " configurations (" << skipped << " invalid combinations skipped) in "
              << static_cast<long long>(ms_since(t0)) << " ms on " << n_threads << " threads\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::ios::sync_with_stdio(false);
    const Options opts = parse_args(argc, argv);
    if (opts.positional.empty()) {
        std::cerr << kUsage;
        return 1;
    }

    try {
        std::string trace_path;
        const SimConfig cfg = build_config(opts, trace_path);
        validate(cfg);  // fail before reading a large trace

        Simulator sim(cfg);
        std::vector<LevelEvent> events;
        if (opts.verbose) sim.set_event_sink(&events);
        auto after_access = [&](const Access& a) {
            if (opts.verbose) {
                print_events(std::cout, a, events, sim);
                events.clear();
            }
        };

        // OPT needs the future and a sweep re-runs the same trace, so those
        // load the whole trace first. Everything else streams: the trace is
        // parsed and simulated chunk by chunk in constant memory.
        const bool batch = opts.batch || opts.sweep || cfg.levels.front().policy == PolicyKind::OPT;
        Trace trace;
        double parse_ms = 0.0, sim_ms = 0.0;
        if (batch) {
            auto t0 = std::chrono::steady_clock::now();
            trace = load_trace(trace_path);
            parse_ms = ms_since(t0);
            if (opts.sweep) return run_sweep(opts, cfg, trace);
            t0 = std::chrono::steady_clock::now();
            sim.run(trace, after_access);
            sim_ms = ms_since(t0);
        } else {
            const auto t0 = std::chrono::steady_clock::now();
            trace = stream_trace(trace_path, [&](const Access& a) {
                sim.access(a);
                after_access(a);
            });
            sim_ms = ms_since(t0);
        }
        if (opts.verbose) std::cout << '\n';

        RunInfo info;
        info.trace = &trace;
        info.parse_ms = parse_ms;
        info.simulate_ms = sim_ms;
        info.streamed = !batch;
        if (opts.json) {
            print_json_report(std::cout, sim, info);
        } else if (opts.quiet) {
            for (std::size_t i = 0; i < sim.level_count(); ++i) {
                const LevelStats& s = sim.stats().levels[i];
                char line[160];
                std::snprintf(line, sizeof line, "%s miss rate %.6f (%llu/%llu)\n", sim.level(i).config().name.c_str(),
                              s.miss_rate(), static_cast<unsigned long long>(s.misses()),
                              static_cast<unsigned long long>(s.accesses()));
                std::cout << line;
            }
        } else {
            print_text_report(std::cout, sim, info);
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "cachesim: error: " << e.what() << "\n";
        return 2;
    }
}
