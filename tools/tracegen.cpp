// tracegen: synthetic memory traces with known cache behaviour.
//
// Each workload emits the exact address sequence that a simple C loop would
// produce (data accesses only), so you can see textbook cache effects:
//
//   tracegen matmul --n 64              > naive.trc     # i-j-k matrix multiply
//   tracegen matmul --n 64 --tile 16    > tiled.trc     # blocked (tiled) version
//   tracegen transpose --n 256 --tile 32
//   tracegen seq --bytes 48K --passes 8                 # working-set loop
//   tracegen random --bytes 4M --count 200000
//   tracegen chase --nodes 8192 --count 200000          # pointer chasing
//   tracegen conflict --stride 4K --addrs 9 --count 9000
//   tracegen stream --n 100000                          # STREAM triad a = b + s*c
//
// Output format is "R 0xADDR SIZE" / "W 0xADDR SIZE", one per line. Pipe
// it straight into the simulator:  tracegen matmul --n 64 | cachesim -
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "cachesim/config.hpp"   // parse_size
#include "cachesim/policy.hpp"   // XorShift64

using cachesim::XorShift64;

namespace {

constexpr const char* kUsage = R"(usage: tracegen WORKLOAD [options] [-o FILE]

workloads and their options (sizes accept K/M/G suffixes):
  seq        --bytes 32K --stride 8 --passes 4        sequential loop over an array
  random     --bytes 1M --count 100000 --seed 1       uniform random 8-byte reads
  matmul     --n 64 [--tile T] [--order ijk|ikj]      C = A x B, doubles, row-major
  transpose  --n 256 [--tile T]                       B = transpose(A), doubles
  chase      --nodes 4096 --node-size 64 --count 100000 --seed 1
                                                      pointer chasing a random cycle
  conflict   --stride 4K --addrs 9 --count 9000       addresses that share one set
  stream     --n 100000                               STREAM triad: a[i] = b[i] + s*c[i]
  hotcold    --hot 16K --cold 4M --hot-pct 90 --count 100000 --seed 1
                                                      skewed reuse (most hits in a hot set)
)";

class Emitter {
public:
    explicit Emitter(std::FILE* f) : f_(f) { buf_.reserve(1 << 16); }
    ~Emitter() { flush(); }
    void read(std::uint64_t a, unsigned size = 8) { put('R', a, size); }
    void write(std::uint64_t a, unsigned size = 8) { put('W', a, size); }
    std::uint64_t count() const { return n_; }

private:
    void put(char op, std::uint64_t a, unsigned size) {
        char line[48];
        const int len = std::snprintf(line, sizeof line, "%c 0x%llx %u\n", op, static_cast<unsigned long long>(a), size);
        buf_.append(line, static_cast<std::size_t>(len));
        ++n_;
        if (buf_.size() > (1 << 16) - 64) flush();
    }
    void flush() {
        if (!buf_.empty()) std::fwrite(buf_.data(), 1, buf_.size(), f_);
        buf_.clear();
    }
    std::FILE* f_;
    std::string buf_;
    std::uint64_t n_ = 0;
};

struct Args {
    std::string workload;
    std::map<std::string, std::string> kv;
    std::string out;

    std::uint64_t num(const std::string& key, std::uint64_t def) const {
        auto it = kv.find(key);
        if (it == kv.end()) return def;
        auto v = cachesim::parse_size(it->second);
        if (!v) {
            std::fprintf(stderr, "tracegen: invalid value for --%s: '%s'\n", key.c_str(), it->second.c_str());
            std::exit(1);
        }
        return *v;
    }
    std::string str(const std::string& key, const std::string& def) const {
        auto it = kv.find(key);
        return it == kv.end() ? def : it->second;
    }
};

// Arrays are laid out back to back, the way one malloc'd block after another
// would be, with one line of padding between them.
constexpr std::uint64_t kBase = 0x10000000;
std::uint64_t after(std::uint64_t start, std::uint64_t bytes) { return start + ((bytes + 63) / 64) * 64 + 64; }

void gen_seq(const Args& a, Emitter& e) {
    const std::uint64_t bytes = a.num("bytes", 32 * 1024);
    const std::uint64_t stride = a.num("stride", 8);
    const std::uint64_t passes = a.num("passes", 4);
    for (std::uint64_t p = 0; p < passes; ++p)
        for (std::uint64_t off = 0; off < bytes; off += stride) e.read(kBase + off, 8);
}

void gen_random(const Args& a, Emitter& e) {
    const std::uint64_t bytes = a.num("bytes", 1 << 20);
    const std::uint64_t count = a.num("count", 100000);
    XorShift64 rng(a.num("seed", 1));
    for (std::uint64_t i = 0; i < count; ++i) e.read(kBase + rng.below(bytes / 8) * 8, 8);
}

void gen_matmul(const Args& a, Emitter& e) {
    const std::uint64_t n = a.num("n", 64);
    const std::uint64_t tile = a.num("tile", 0);
    const std::string order = a.str("order", "ijk");
    const std::uint64_t es = 8;  // double
    const std::uint64_t bytes = n * n * es;
    const std::uint64_t A = kBase, B = after(A, bytes), C = after(B, bytes);
    auto at = [&](std::uint64_t base, std::uint64_t r, std::uint64_t c) { return base + (r * n + c) * es; };

    if (tile == 0 && order == "ijk") {
        // for i, j: sum = 0; for k: sum += A[i][k] * B[k][j]; C[i][j] = sum;
        for (std::uint64_t i = 0; i < n; ++i)
            for (std::uint64_t j = 0; j < n; ++j) {
                for (std::uint64_t k = 0; k < n; ++k) {
                    e.read(at(A, i, k));
                    e.read(at(B, k, j));
                }
                e.write(at(C, i, j));
            }
    } else if (tile == 0 && order == "ikj") {
        // for i, k: r = A[i][k]; for j: C[i][j] += r * B[k][j];
        for (std::uint64_t i = 0; i < n; ++i)
            for (std::uint64_t k = 0; k < n; ++k) {
                e.read(at(A, i, k));
                for (std::uint64_t j = 0; j < n; ++j) {
                    e.read(at(C, i, j));
                    e.read(at(B, k, j));
                    e.write(at(C, i, j));
                }
            }
    } else if (tile > 0) {
        // Blocked i-j-k: work on T x T tiles that fit in the cache together.
        for (std::uint64_t ii = 0; ii < n; ii += tile)
            for (std::uint64_t jj = 0; jj < n; jj += tile)
                for (std::uint64_t kk = 0; kk < n; kk += tile)
                    for (std::uint64_t i = ii; i < std::min(ii + tile, n); ++i)
                        for (std::uint64_t j = jj; j < std::min(jj + tile, n); ++j) {
                            e.read(at(C, i, j));
                            for (std::uint64_t k = kk; k < std::min(kk + tile, n); ++k) {
                                e.read(at(A, i, k));
                                e.read(at(B, k, j));
                            }
                            e.write(at(C, i, j));
                        }
    } else {
        std::fprintf(stderr, "tracegen: --order must be ijk or ikj\n");
        std::exit(1);
    }
}

void gen_transpose(const Args& a, Emitter& e) {
    const std::uint64_t n = a.num("n", 256);
    const std::uint64_t tile = a.num("tile", 0);
    const std::uint64_t es = 8;
    const std::uint64_t A = kBase, B = after(A, n * n * es);
    const std::uint64_t t = tile ? tile : n;
    for (std::uint64_t ii = 0; ii < n; ii += t)
        for (std::uint64_t jj = 0; jj < n; jj += t)
            for (std::uint64_t i = ii; i < std::min(ii + t, n); ++i)
                for (std::uint64_t j = jj; j < std::min(jj + t, n); ++j) {
                    e.read(A + (i * n + j) * es);
                    e.write(B + (j * n + i) * es);
                }
}

void gen_chase(const Args& a, Emitter& e) {
    const std::uint64_t nodes = a.num("nodes", 4096);
    const std::uint64_t node_size = a.num("node-size", 64);
    const std::uint64_t count = a.num("count", 100000);
    XorShift64 rng(a.num("seed", 1));
    // Sattolo's algorithm: a random permutation that is one single cycle.
    std::vector<std::uint64_t> next(nodes);
    for (std::uint64_t i = 0; i < nodes; ++i) next[i] = i;
    for (std::uint64_t i = nodes - 1; i > 0; --i) std::swap(next[i], next[rng.below(i)]);
    std::uint64_t cur = 0;
    for (std::uint64_t i = 0; i < count; ++i) {
        e.read(kBase + cur * node_size, 8);
        cur = next[cur];
    }
}

void gen_conflict(const Args& a, Emitter& e) {
    const std::uint64_t stride = a.num("stride", 4096);
    const std::uint64_t addrs = a.num("addrs", 9);
    const std::uint64_t count = a.num("count", 9000);
    for (std::uint64_t i = 0; i < count; ++i) e.read(kBase + (i % addrs) * stride, 8);
}

void gen_stream(const Args& a, Emitter& e) {
    const std::uint64_t n = a.num("n", 100000);
    const std::uint64_t bytes = n * 8;
    const std::uint64_t A = kBase, B = after(A, bytes), C = after(B, bytes);
    for (std::uint64_t i = 0; i < n; ++i) {
        e.read(B + i * 8);
        e.read(C + i * 8);
        e.write(A + i * 8);
    }
}

void gen_hotcold(const Args& a, Emitter& e) {
    const std::uint64_t hot = a.num("hot", 16 * 1024);
    const std::uint64_t cold = a.num("cold", 4 << 20);
    const std::uint64_t pct = a.num("hot-pct", 90);
    const std::uint64_t count = a.num("count", 100000);
    XorShift64 rng(a.num("seed", 1));
    const std::uint64_t cold_base = after(kBase, hot);
    for (std::uint64_t i = 0; i < count; ++i) {
        if (rng.below(100) < pct) e.read(kBase + rng.below(hot / 8) * 8);
        else e.read(cold_base + rng.below(cold / 8) * 8);
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2 || !std::strcmp(argv[1], "-h") || !std::strcmp(argv[1], "--help")) {
        std::fputs(kUsage, argc < 2 ? stderr : stdout);
        return argc < 2 ? 1 : 0;
    }
    Args args;
    args.workload = argv[1];
    for (int i = 2; i < argc; ++i) {
        std::string k = argv[i];
        if ((k == "-o" || k == "--out") && i + 1 < argc) {
            args.out = argv[++i];
        } else if (k.rfind("--", 0) == 0 && i + 1 < argc) {
            args.kv[k.substr(2)] = argv[++i];
        } else {
            std::fprintf(stderr, "tracegen: unexpected argument '%s'\n", k.c_str());
            return 1;
        }
    }

    std::FILE* f = stdout;
    if (!args.out.empty()) {
        f = std::fopen(args.out.c_str(), "wb");
        if (!f) {
            std::fprintf(stderr, "tracegen: cannot write '%s'\n", args.out.c_str());
            return 1;
        }
    }
    std::uint64_t emitted = 0;
    {
        Emitter e(f);
        const std::string& w = args.workload;
        if (w == "seq") gen_seq(args, e);
        else if (w == "random") gen_random(args, e);
        else if (w == "matmul") gen_matmul(args, e);
        else if (w == "transpose") gen_transpose(args, e);
        else if (w == "chase") gen_chase(args, e);
        else if (w == "conflict") gen_conflict(args, e);
        else if (w == "stream") gen_stream(args, e);
        else if (w == "hotcold") gen_hotcold(args, e);
        else {
            std::fprintf(stderr, "tracegen: unknown workload '%s'\n%s", w.c_str(), kUsage);
            return 1;
        }
        emitted = e.count();
    }
    if (f != stdout) {
        std::fclose(f);
        std::fprintf(stderr, "tracegen: wrote %llu accesses to %s\n", static_cast<unsigned long long>(emitted),
                     args.out.c_str());
    }
    return 0;
}
