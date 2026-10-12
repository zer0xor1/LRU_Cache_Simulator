# Design notes

This document explains how the simulator is built and why. It is written to be read
top to bottom in about fifteen minutes.

## 1. The model

A cache level has `size` bytes in lines of `block` bytes, grouped into sets of `ways` lines.

```text
lines = size / block
sets  = lines / ways
block number = address >> log2(block)
set index    = block number mod sets        (a bit mask when sets is a power of two)
tag          = block number / sets          (a shift when sets is a power of two)
```

The engine stores the whole **block number** in each line rather than the tag. It is just as
cheap to compare, it is unique across sets, and it gives the victim's address for free when a
dirty line must be written back. Tags are only computed for display.

Non-power-of-two set counts are allowed and use `mod`. The original program instead took
`floor(log2(sets))` index bits, which silently ignored the extra sets (see
[AUDIT.md](AUDIT.md)).

## 2. One cache level (`src/cache.cpp`)

### Storage

```text
blocks_[set * ways + way]   uint64 block numbers, set after set
dirty_ [set * ways + way]   one byte per line
fill_count_[set]            how many ways of the set are valid
```

This is structure-of-arrays. An 8-way set is 64 bytes of block numbers: one host cache line.
A lookup is a short linear scan with no pointer chasing.

**No valid bits.** The simulator never invalidates a line (there is no coherence or
back-invalidation in this model). A set therefore fills in way order 0, 1, 2, ... and its
valid lines are always ways `0 .. fill_count-1`. The scan stops at `fill_count`, and a miss in
a non-full set goes to way `fill_count` without asking the policy. This also gives every
policy a deterministic fill order, which the differential tests rely on.

**Hash index for very associative caches.** Above 32 ways a linear scan costs too much (a fully
associative 32 KiB cache has 512 ways). Those levels also keep a block-to-line index in
`FlatHashMap`, an open-addressing table:

* keys and values in one flat array of slots (one memory access per probe, no nodes),
* Fibonacci hashing (`key * 2^64/phi >> shift`) so clustered block numbers spread out,
* linear probing with load factor at most 0.5,
* backward-shift deletion: when a key is erased, later members of its probe chain move back
  into the hole. No tombstones, so probe chains never get longer over time. This matters
  because a cache erases one key on every eviction, forever.

The table is tested against `std::unordered_map` with random insert, erase and find
sequences on tiny tables that force wrap-around.

### Access

```text
access(block, is_write):
    way = find(set, block)
    hit  -> policy.on_hit; if write-back and store: dirty = 1
    miss -> if store and no-write-allocate: return (bypass)
            way = fill_count < ways ? fill_count++ : policy.victim(set)
            remember victim block and dirty bit (for the write-back)
            install block; dirty = store && write-back; policy.on_fill
```

## 3. Replacement policies (`src/policy.cpp`)

A policy only sees `(set, way)` and three events: `on_hit`, `on_fill` and `victim`. It never
sees tags. All per-line state lives in flat arrays indexed by `set * ways + way`.

| Policy | State per set | Hit | Victim | Notes |
|---|---|---|---|---|
| LRU | doubly linked list in `prev`/`next` arrays, `head`, `tail` | O(1) move to head | O(1) tail | No allocation. Same cost at 4 ways or 4,096. |
| FIFO | the same list | nothing | O(1) tail | Shows Belady's anomaly (tested). |
| Random | one xorshift64* generator | nothing | O(1) | Deterministic for a seed on every platform, unlike `std::uniform_int_distribution`. |
| Tree PLRU | `ways - 1` bits as a binary tree in heap order | O(log ways) | O(log ways) | Needs a power-of-two number of ways. |
| SRRIP | 2-bit RRPV per line | set RRPV to 0 | O(ways), ageing loop | Inserts at RRPV 2, so one-time lines leave first. |
| OPT | next-use position per line | store next use | O(ways) arg-max | Needs the whole trace; first level only. |

### Tree PLRU, worked example

Four ways need three bits. Node 0 is the root; nodes 1 and 2 cover ways {0,1} and {2,3}. A bit
of 0 means "the victim is on the left", 1 means "on the right". Touching a way sets the bits on
its path to point **away** from it.

```text
fill A,B,C,D (ways 0..3)  -> bits 000   (each touch flipped its path; the last was way 3)
touch A (way 0)           -> root=1 (victim right), node1=1 -> bits 110
miss E: root=1 -> right subtree; node2=0 -> left -> way 2 (C)
```

True LRU would evict B here. PLRU picks C. That is the price of using 3 bits instead of the
`log2(4!) ≈ 5` bits exact LRU needs, and it is exactly what `tests/test_policies.cpp` checks.

### SRRIP, why it resists scans

Lines enter with RRPV 2 ("re-reference expected in a while"), a hit sets RRPV 0 ("soon"), and
the victim is any line at 3 ("not soon"). If none is at 3, every line in the set ages by one
and the search repeats. A burst of one-time lines enters at 2 and ages to 3 before a reused
line (at 0) does, so a scan cannot flush hot data the way it does under LRU. On the hot/cold
workload in the README chart, SRRIP gets 23.5% misses against LRU's 31.3% (OPT: 20.2%).

### OPT (Belady's MIN)

`Simulator::compute_next_use` walks the line-access stream backwards with a hash map from
block to "next position seen", producing `next[i]` for every access in O(n). Each line stores
the next-use position of its latest access. The victim is the line with the largest value.
Lines that are never used again hold `kNever` (`UINT64_MAX`) and are evicted first.

OPT is only allowed in the first level, because only that level's access stream is known
before the run. Lower levels see a stream that depends on the upper levels' decisions.
This version always allocates on a miss (no bypass), which is the textbook form used for the
page-replacement examples in the tests.

## 4. The hierarchy (`src/simulator.cpp`)

The model is non-inclusive and non-exclusive, like most teaching simulators.

```text
CPU access (addr, size, R/W)
  -> split into L1 lines
  -> L1.access
       hit:   store under write-through? forward the store down (WriteThrough)
       miss:  store under no-write-allocate? forward the store down, no fill
              otherwise fetch the line from L2 (Fill), then install
       dirty victim? write it to L2 (Writeback)
  -> the same logic at L2, L3, ... and finally memory
```

Details that are easy to get wrong:

* **Write-back into the next level.** A dirty victim carries a whole line. If the next level
  misses and has the same line size, it allocates without fetching from below, because every
  byte is being written. If its lines are bigger, it must fetch first.
* **Order.** The fill is served first and the victim drains from a write buffer afterwards.
  This affects only the recency order in the next level, not any count.
* **Latency.** Demand accesses and fills cost each level's hit latency, plus the level below on
  a miss. Write-backs and write-throughs are assumed buffered and cost nothing on the critical
  path. AMAT is total demand cycles divided by CPU line accesses. For one level and no
  bypassing stores this equals the textbook `hit + miss_rate x memory`, and the report prints
  both when they agree.
* **Line sizes must not shrink going down.** Then one upper line always maps to exactly one
  lower line. The validator rejects configs that break this.
* **Split accesses.** An 8-byte load at `0x3C` with 64-byte lines touches two lines and
  becomes two L1 accesses. The report counts how many records were split.

## 5. The 3C classification

For each level the simulator keeps:

* `seen`: every block that was ever installed in that level, and
* a **shadow** cache: the same size and write policies, but fully associative with LRU.

Every access goes to both the real cache and the shadow. For each real miss:

```text
never installed before        -> compulsory
the shadow also missed        -> capacity
the shadow hit                -> conflict
```

This is the per-access form of Hill and Smith's model, so the three counts always add up to
the miss count. Two properties are tested: a fully associative LRU cache has zero conflict
misses, and on a read-only trace the compulsory count equals the number of distinct blocks.
A real cache can beat its shadow (the README shows an 8-way cache beating fully associative
LRU at 1 MiB). Per-access classification never produces negative counts in that case; those
"saved" misses simply never appear.

The shadow uses the hash-indexed path, so it costs O(1) per access. Turning it off with
`--no-3c` roughly halves the simulation time.

## 6. Traces and streaming (`src/trace.cpp`)

The parser is a single forward pass with no per-line allocation: a 256-entry table for hex
digits, `memchr` to skip comments, and no `std::string` per token. Each line's format is
detected on its own (bare hex, R/W, Lackey, Dinero), so files can mix formats.

The same parsing core runs in two modes:

* **Batch** (`load_trace`): read the file once into a buffer and build a vector of accesses.
  Needed for OPT and for sweeps, which replay one trace many times.
* **Streaming** (`stream_trace`): read 1 MiB chunks, parse up to the last complete line, carry
  the partial line into the next chunk. Accesses go straight to the simulator. Memory use does
  not grow with the trace. This is the default whenever OPT is not involved, and it is what
  lets you pipe Valgrind into the tool. A test checks that chunk sizes from 1 byte to 1 MiB
  give results identical to batch mode, including error line numbers.

## 7. Sweeps

`--sweep` loads the trace once, builds every valid configuration, and hands them to a pool of
`std::thread` workers that pull the next job index from an `std::atomic`. Each worker owns its
own `Simulator`, so there is no shared mutable state apart from the job counter and the result
slots (one per job, written by exactly one thread). Results print in a fixed order, so output
does not depend on scheduling.

## 8. Testing strategy

The goal is that a wrong number is caught by more than one independent check.

1. **Hand-worked examples** pin down each policy's exact decisions (LRU order, PLRU bits,
   SRRIP ageing) and the textbook page-replacement results.
2. **Properties** hold for every trace: OPT never loses to another policy; compulsory equals
   distinct blocks; a fully associative LRU cache has no conflict misses; traffic between
   levels is conserved (every L1 miss is one L2 read, every dirty L1 victim is one L2 write).
3. **Differential testing** runs 600 random configurations (1 to 40 ways, 1 to 16 sets, every
   policy and write policy, accesses that cross lines) through both the engine and a naive
   reference model. The reference shares no code: sets are plain vectors, victims come from
   timestamps, PLRU bits live in a `std::map`, OPT uses binary search over position lists.
   All counters must match.
4. **Mutation testing** checks that the tests have teeth. Seven deliberate bugs were injected
   one at a time. Every one made the suite fail.
5. **Regression** against the original program's output on its own trace.
6. **Sanitizers** (ASan, UBSan) and `-Werror` on GCC and Clang.
7. **Cross-language**: the JavaScript engine in `web/` must match the C++ binary on 90
   trace and configuration pairs.

The test framework is about 130 lines in `tests/test_framework.hpp`, so the project builds
with no downloads.

## 9. Limits and possible extensions

* No coherence, prefetching, MSHRs or timing overlap. AMAT is a latency sum, not a cycle model.
* No inclusive or exclusive hierarchy policies yet; adding them means back-invalidation, which
  would bring back valid bits (the `fill_count` shortcut assumes lines are never invalidated).
* OPT could bypass the cache when the incoming line is the furthest-future one; that is a
  small change in `CacheLevel::access`.
* A next-line or stride prefetcher would plug in at the `Fill` path in `access_level`.
