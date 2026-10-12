/*
 * cachesim.js: the single-level cache engine used by the web visualizer.
 *
 * It mirrors the C++ engine's semantics exactly (fill order, policies,
 * write policies, 3C classification, Random's xorshift64* sequence), and
 * web/test/crosscheck.mjs verifies that against the cachesim binary.
 *
 * Works as a classic <script> (exposes window.CacheSim) and in Node
 * (module.exports), so the page also opens straight from disk.
 * Addresses must stay below 2^53 (plenty for the visualizer).
 */
(function (root, factory) {
  if (typeof module === "object" && module.exports) module.exports = factory();
  else root.CacheSim = factory();
})(typeof self !== "undefined" ? self : this, function () {
  "use strict";

  const POLICIES = ["LRU", "FIFO", "RANDOM", "PLRU", "SRRIP", "OPT"];
  const NEVER = Infinity;
  const MASK64 = (1n << 64n) - 1n;

  const log2 = (x) => Math.round(Math.log2(x));

  /** xorshift64*, bit-for-bit identical to cachesim::XorShift64. */
  class XorShift64 {
    constructor(seed) {
      this.s = BigInt.asUintN(64, BigInt(seed || 0)) || 0x9e3779b97f4a7c15n;
    }
    next() {
      let s = this.s;
      s ^= s >> 12n;
      s = (s ^ (s << 25n)) & MASK64;
      s ^= s >> 27n;
      this.s = s;
      return (s * 0x2545f4914f6cdd1dn) & MASK64;
    }
    below(n) {
      return Number(this.next() % BigInt(n));
    }
  }

  // ---------------------------------------------------------------------
  // Trace parsing (same grammar as src/trace.cpp)
  // ---------------------------------------------------------------------
  const OPS = { L: "R", R: "R", 0: "R", S: "W", W: "W", 1: "W", I: "I", 2: "I", M: "M" };
  const HEX = /^(0[xX])?([0-9a-fA-F]{1,16})$/;
  const MAX_ACCESS_SIZE = 1 << 20; // kMaxAccessSize in trace.hpp

  function parseHex(tok) {
    const m = HEX.exec(tok);
    if (!m) return null;
    const digits = m[2];
    return digits.length <= 13 ? parseInt(digits, 16) : Number(BigInt("0x" + digits));
  }

  function parseTrace(text) {
    const accesses = [];
    const lines = text.split("\n");
    for (let i = 0; i < lines.length; i++) {
      const line = lines[i].replace(/\r$/, "");
      const trimmed = line.trim();
      if (!trimmed || trimmed[0] === "#" || /^(==|--|\/\/)/.test(trimmed)) continue;
      const tok = trimmed.split(/[\s,]+/).filter(Boolean);
      let op = "R";
      let rest = tok;
      if (tok.length > 1 && tok[0].length === 1) {
        const c = tok[0].toUpperCase();
        if (c in OPS) {
          op = OPS[c];
          rest = tok.slice(1);
        } else if (!/[0-9a-f]/i.test(c)) {
          throw new Error(`line ${i + 1}: unknown operation '${tok[0]}'`);
        }
      }
      if (rest.length > 2) throw new Error(`line ${i + 1}: too many fields`);
      const addr = parseHex(rest[0]);
      if (addr === null) throw new Error(`line ${i + 1}: invalid hex address '${rest[0]}'`);
      let size = 1;
      if (rest.length === 2) {
        const s = /^0[xX]/.test(rest[1]) ? parseHex(rest[1]) : /^\d+$/.test(rest[1]) ? Number(rest[1]) : null;
        if (s === null) throw new Error(`line ${i + 1}: invalid access size '${rest[1]}'`);
        if (s > MAX_ACCESS_SIZE) throw new Error(`line ${i + 1}: access size too large '${rest[1]}'`);
        size = Math.max(1, s);
      }
      if (op === "M") {
        accesses.push({ addr, size, op: "R" }, { addr, size, op: "W" });
      } else {
        accesses.push({ addr, size, op });
      }
    }
    return accesses;
  }

  // ---------------------------------------------------------------------
  // Configuration
  // ---------------------------------------------------------------------
  function normalizeConfig(c) {
    const cfg = Object.assign(
      { size: 256, block: 16, assoc: 2, policy: "LRU", writeBack: true, writeAllocate: true, seed: 42 },
      c || {}
    );
    cfg.policy = String(cfg.policy).toUpperCase();
    const lines = cfg.size / cfg.block;
    cfg.lines = lines;
    cfg.ways = cfg.assoc === 0 || cfg.assoc === "full" ? lines : Number(cfg.assoc);
    cfg.sets = lines / cfg.ways;
    return cfg;
  }

  function validate(c) {
    const cfg = normalizeConfig(c);
    const pow2 = (x) => Number.isInteger(x) && x > 0 && (x & (x - 1)) === 0;
    if (!pow2(cfg.block)) throw new Error("Block size must be a power of two.");
    if (!(cfg.size > 0) || cfg.size % cfg.block !== 0) throw new Error("Cache size must be a multiple of the block size.");
    if (cfg.ways > cfg.lines) throw new Error(`A ${cfg.ways}-way cache needs at least ${cfg.ways} lines; this one has ${cfg.lines}.`);
    if (!Number.isInteger(cfg.sets) || cfg.sets < 1) throw new Error("Cache size must be a multiple of block size x ways.");
    if (!POLICIES.includes(cfg.policy)) throw new Error(`Unknown policy ${cfg.policy}.`);
    if (cfg.policy === "PLRU" && !pow2(cfg.ways)) throw new Error("Tree PLRU needs a power-of-two number of ways.");
    return cfg;
  }

  // ---------------------------------------------------------------------
  // One cache level
  // ---------------------------------------------------------------------
  class CacheModel {
    constructor(config) {
      const cfg = validate(config);
      this.cfg = cfg;
      const n = cfg.sets * cfg.ways;
      this.offsetBits = log2(cfg.block);
      this.pow2Sets = (cfg.sets & (cfg.sets - 1)) === 0;
      this.indexBits = cfg.sets <= 1 ? 0 : Math.ceil(Math.log2(cfg.sets));
      this.blocks = new Array(n).fill(0);
      this.dirty = new Uint8Array(n);
      this.fill = new Uint32Array(cfg.sets);
      this.stamp = new Float64Array(n); // LRU: last use; FIFO: insertion time
      this.rrpv = new Uint8Array(n).fill(3);
      this.nextUse = new Float64Array(n).fill(NEVER);
      this.plru = new Uint8Array(cfg.sets * Math.max(cfg.ways - 1, 0));
      this.rng = new XorShift64(cfg.seed);
      this.index = cfg.ways > 32 ? new Map() : null;
      this.clock = 0;
      // 3C: a fully associative LRU twin of the same size, plus "ever filled".
      this.shadow = new Map();
      this.seen = new Set();
      this.stats = {
        reads: 0, writes: 0, readMisses: 0, writeMisses: 0, evictions: 0, writebacks: 0,
        compulsory: 0, capacity: 0, conflict: 0, memReads: 0, memWrites: 0,
      };
    }

    blockOf(addr) { return Math.floor(addr / this.cfg.block); }
    setOf(block) { return block % this.cfg.sets; }
    tagOf(block) { return Math.floor(block / this.cfg.sets); }

    findWay(set, block) {
      if (this.index) {
        const line = this.index.get(block);
        return line === undefined ? -1 : line - set * this.cfg.ways;
      }
      const base = set * this.cfg.ways;
      for (let w = 0; w < this.fill[set]; w++) if (this.blocks[base + w] === block) return w;
      return -1;
    }

    plruTouch(set, way) {
      const W = this.cfg.ways, b = this.plru, off = set * (W - 1);
      let node = 0, lo = 0, hi = W;
      while (hi - lo > 1) {
        const mid = lo + ((hi - lo) >> 1);
        if (way < mid) { b[off + node] = 1; node = 2 * node + 1; hi = mid; }
        else { b[off + node] = 0; node = 2 * node + 2; lo = mid; }
      }
    }

    victim(set) {
      const W = this.cfg.ways, base = set * W;
      switch (this.cfg.policy) {
        case "LRU":
        case "FIFO": {
          let best = 0;
          for (let w = 1; w < W; w++) if (this.stamp[base + w] < this.stamp[base + best]) best = w;
          return best;
        }
        case "RANDOM":
          return this.rng.below(W);
        case "PLRU": {
          const off = set * (W - 1);
          let node = 0, lo = 0, hi = W;
          while (hi - lo > 1) {
            const mid = lo + ((hi - lo) >> 1);
            if (this.plru[off + node] === 0) { node = 2 * node + 1; hi = mid; }
            else { node = 2 * node + 2; lo = mid; }
          }
          return lo;
        }
        case "SRRIP":
          for (;;) {
            for (let w = 0; w < W; w++) if (this.rrpv[base + w] >= 3) return w;
            for (let w = 0; w < W; w++) this.rrpv[base + w]++;
          }
        case "OPT": {
          let best = 0;
          for (let w = 1; w < W; w++) if (this.nextUse[base + w] > this.nextUse[base + best]) best = w;
          return best;
        }
      }
      return 0;
    }

    shadowAccess(block, isWrite) {
      if (this.shadow.has(block)) {
        this.shadow.delete(block);
        this.shadow.set(block, true);
        return true;
      }
      if (isWrite && !this.cfg.writeAllocate) return false;
      this.shadow.set(block, true);
      if (this.shadow.size > this.cfg.lines) this.shadow.delete(this.shadow.keys().next().value);
      return false;
    }

    /**
     * Access one block. Returns an event describing what happened, used by
     * the visualizer for highlighting and the explanation sentence.
     */
    access(block, isWrite, nextUse = NEVER) {
      const cfg = this.cfg, W = cfg.ways, st = this.stats;
      const set = this.setOf(block);
      const ev = {
        block, set, tag: this.tagOf(block), isWrite, hit: false, way: -1, filled: false,
        evicted: false, evictedBlock: 0, evictedDirty: false, missType: null, memWrite: false,
      };
      if (isWrite) st.writes++; else st.reads++;
      const shadowHit = this.shadowAccess(block, isWrite);
      const found = this.findWay(set, block);
      this.clock++;

      if (found >= 0) {
        const line = set * W + found;
        ev.hit = true;
        ev.way = found;
        if (isWrite && cfg.writeBack) this.dirty[line] = 1;
        if (isWrite && !cfg.writeBack) { st.memWrites++; ev.memWrite = true; }
        if (cfg.policy === "LRU") this.stamp[line] = this.clock;
        this.rrpv[line] = 0;
        this.nextUse[line] = nextUse;
        if (cfg.policy === "PLRU") this.plruTouch(set, found);
        return ev;
      }

      if (isWrite) st.writeMisses++; else st.readMisses++;
      if (!this.seen.has(block)) { ev.missType = "compulsory"; st.compulsory++; }
      else if (!shadowHit) { ev.missType = "capacity"; st.capacity++; }
      else { ev.missType = "conflict"; st.conflict++; }

      if (isWrite && !cfg.writeAllocate) {
        st.memWrites++;
        ev.memWrite = true;
        return ev;
      }

      let way;
      if (this.fill[set] < W) {
        way = this.fill[set]++;
      } else {
        way = this.victim(set);
        const line = set * W + way;
        ev.evicted = true;
        ev.evictedBlock = this.blocks[line];
        ev.evictedDirty = this.dirty[line] === 1;
        st.evictions++;
        if (ev.evictedDirty) { st.writebacks++; st.memWrites++; }
        if (this.index) this.index.delete(ev.evictedBlock);
      }
      const line = set * W + way;
      this.blocks[line] = block;
      this.dirty[line] = isWrite && cfg.writeBack ? 1 : 0;
      this.stamp[line] = this.clock;
      this.rrpv[line] = 2;
      this.nextUse[line] = nextUse;
      if (cfg.policy === "PLRU") this.plruTouch(set, way);
      if (this.index) this.index.set(block, line);
      this.seen.add(block);
      st.memReads++;
      if (isWrite && !cfg.writeBack) { st.memWrites++; ev.memWrite = true; }
      ev.way = way;
      ev.filled = true;
      return ev;
    }

    /** Per-set view for rendering: one entry per way (null when empty). */
    snapshot(set) {
      const W = this.cfg.ways, base = set * W, out = [];
      const valid = this.fill[set];
      // Recency rank among valid lines: 0 = newest.
      const order = [];
      for (let w = 0; w < valid; w++) order.push(w);
      order.sort((a, b) => this.stamp[base + b] - this.stamp[base + a]);
      const rank = new Array(W).fill(-1);
      order.forEach((w, i) => (rank[w] = i));
      for (let w = 0; w < W; w++) {
        if (w >= valid) { out.push(null); continue; }
        const line = base + w;
        out.push({
          block: this.blocks[line], tag: this.tagOf(this.blocks[line]), dirty: this.dirty[line] === 1,
          rank: rank[w], rrpv: this.rrpv[line], nextUse: this.nextUse[line],
        });
      }
      return out;
    }

    plruBits(set) {
      const n = this.cfg.ways - 1;
      return Array.from(this.plru.subarray(set * n, set * n + n)).join("");
    }
  }

  // ---------------------------------------------------------------------
  // Whole-trace helpers
  // ---------------------------------------------------------------------

  /** Split accesses into line accesses (skipping ifetch unless asked). */
  function lineStream(accesses, block, includeIfetch = false) {
    const out = [];
    for (let i = 0; i < accesses.length; i++) {
      const a = accesses[i];
      if (a.op === "I" && !includeIfetch) continue;
      const first = Math.floor(a.addr / block);
      const last = Math.floor((a.addr + Math.max(1, a.size) - 1) / block);
      for (let b = first; b <= last; b++) out.push({ block: b, isWrite: a.op === "W", record: i });
    }
    return out;
  }

  /** next[i] = index of the next access to the same block, or Infinity. */
  function nextUses(stream) {
    const next = new Array(stream.length).fill(NEVER);
    const upcoming = new Map();
    for (let i = stream.length - 1; i >= 0; i--) {
      const b = stream[i].block;
      if (upcoming.has(b)) next[i] = upcoming.get(b);
      upcoming.set(b, i);
    }
    return next;
  }

  function simulate(accesses, config, opts = {}) {
    const model = new CacheModel(config);
    const stream = lineStream(accesses, model.cfg.block, opts.includeIfetch);
    const next = model.cfg.policy === "OPT" ? nextUses(stream) : null;
    for (let i = 0; i < stream.length; i++) model.access(stream[i].block, stream[i].isWrite, next ? next[i] : NEVER);
    const s = model.stats;
    const accessesN = s.reads + s.writes, misses = s.readMisses + s.writeMisses;
    return Object.assign({}, s, { accesses: accessesN, misses, hits: accessesN - misses,
      missRate: accessesN ? misses / accessesN : 0 });
  }

  return { POLICIES, NEVER, XorShift64, parseTrace, validate, normalizeConfig, CacheModel, lineStream, nextUses, simulate };
});
