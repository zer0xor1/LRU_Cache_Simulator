// Cross-check the JavaScript engine (web/cachesim.js) against the C++ binary.
//
//   node web/test/crosscheck.mjs [path/to/cachesim]
//
// Runs the same traces and configurations through both implementations and
// compares every counter. Exits non-zero on the first mismatch.
import { execFileSync } from "node:child_process";
import { readFileSync, existsSync } from "node:fs";
import { createRequire } from "node:module";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const require = createRequire(import.meta.url);
const CacheSim = require("../cachesim.js");
const root = join(dirname(fileURLToPath(import.meta.url)), "..", "..");
const bin = process.argv[2] || process.env.CACHESIM_BIN || join(root, "build", "cachesim");
if (!existsSync(bin)) {
  console.error(`cachesim binary not found at ${bin}; build first or pass its path`);
  process.exit(2);
}

const traces = [
  "traces/example.trc",
  "traces/real/sum_matrix_lackey.trc",
  "traces/samples/matmul_naive_n32.trc",
  "traces/samples/pointer_chase.trc",
  "traces/samples/stream_triad.trc",
  "traces/samples/conflict_thrash.trc",
];
const configs = [];
for (const policy of CacheSim.POLICIES) {
  configs.push({ size: 8192, block: 64, assoc: 4, policy, writeBack: true, writeAllocate: true });
  configs.push({ size: 4096, block: 32, assoc: 2, policy, writeBack: false, writeAllocate: false });
}
configs.push({ size: 2048, block: 64, assoc: "full", policy: "LRU", writeBack: true, writeAllocate: true });
configs.push({ size: 64 * 40 * 3, block: 64, assoc: 40, policy: "SRRIP", writeBack: true, writeAllocate: false });
configs.push({ size: 20 * 1024, block: 1024, assoc: 1, policy: "FIFO", writeBack: false, writeAllocate: true });

const fields = [
  ["reads", "reads"], ["writes", "writes"], ["read_misses", "readMisses"], ["write_misses", "writeMisses"],
  ["evictions", "evictions"], ["writebacks_out", "writebacks"], ["compulsory", "compulsory"],
  ["capacity", "capacity"], ["conflict", "conflict"],
];

let checked = 0;
for (const t of traces) {
  const path = join(root, t);
  const accesses = CacheSim.parseTrace(readFileSync(path, "utf8"));
  for (const c of configs) {
    const args = ["--json", "--batch", "-s", String(c.size), "-b", String(c.block), "-a", String(c.assoc), "-p", c.policy];
    if (!c.writeBack) args.push("--write-through");
    if (!c.writeAllocate) args.push("--no-write-allocate");
    const cpp = JSON.parse(execFileSync(bin, [...args, path], { maxBuffer: 1 << 26 }).toString());
    const lvl = cpp.results.levels[0];
    const mem = cpp.results.memory;
    const js = CacheSim.simulate(accesses, c);
    const diffs = fields.filter(([a, b]) => lvl[a] !== js[b]).map(([a, b]) => `${a}: cpp ${lvl[a]} js ${js[b]}`);
    if (mem.reads !== js.memReads) diffs.push(`memory reads: cpp ${mem.reads} js ${js.memReads}`);
    if (mem.writes !== js.memWrites) diffs.push(`memory writes: cpp ${mem.writes} js ${js.memWrites}`);
    if (diffs.length) {
      console.error(`MISMATCH ${t} ${JSON.stringify(c)}\n  ${diffs.join("\n  ")}`);
      process.exit(1);
    }
    checked++;
  }
  console.log(`ok  ${t} (${configs.length} configurations)`);
}
console.log(`\n${checked} trace/configuration pairs identical between C++ and JavaScript`);
