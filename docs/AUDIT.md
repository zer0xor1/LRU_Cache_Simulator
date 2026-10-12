# Audit of the original program

Before rewriting, the original code (`legacy/original/`) was compiled and run as shipped.
This file records what was found, with the commands used, so every claim can be checked.

Build used for all runs:

```sh
g++ -std=c++11 -O2 legacy/original/Cache.cpp legacy/original/Main.cpp -o original
```

## What works

On power-of-two geometries the original's LRU results are correct. The rewrite reproduces
all of them exactly (`tests/test_regression.cpp`):

| Size / line / ways | Hits | Misses | Hit rate |
|---|---:|---:|---:|
| 32768 / 64 / 4 | 594,142 | 101,379 | 0.85424 |
| 32768 / 64 / 2 | 593,740 | 101,781 | 0.853662 |
| 16384 / 64 / 2 | 592,777 | 102,744 | 0.852278 |
| 16384 / 64 / 8 | 593,285 | 102,236 | 0.853008 |
| 4096 / 32 / 4 | 524,821 | 170,700 | 0.754572 |
| 131072 / 1024 / 8 | 651,224 | 44,297 | 0.936311 |
| 1024 / 64 / 1 | 585,298 | 110,223 | 0.841525 |
| 4096 / 64 / 64 | 591,243 | 104,278 | 0.850072 |

## Problems found

### 1. Use of an erased iterator (undefined behaviour)

In `Cache::checkEntry`, the "set not full, hit" branch erases the matching list node and
pushes the tag to the back, but does not update the stored iterator. The function then
dereferences that iterator (`**currentIt`) to check its result. That reads freed memory on
every hit in a set that is not full yet.

It happened to work in the release build. A checked build shows it:

```sh
g++ -std=c++11 -O1 -g -fsanitize=address,undefined -D_GLIBCXX_DEBUG \
    legacy/original/Cache.cpp legacy/original/Main.cpp -o original_debug
# config 1024 / 64 / 4:
Error: attempt to dereference a singular iterator.
```

**Now:** no iterators into containers that change. Sets are flat arrays and the LRU list
lives in index arrays. The whole suite runs clean under ASan and UBSan.

### 2. Non-power-of-two set counts silently shrink the cache

Index bits were computed as `log(sets)/log(2)` truncated to an int, and the set index came from
that many address bits. With 20 sets that is 4 bits, so only sets 0 to 15 were ever used.

The project's own `example.cfg` (163840 / 1024 / 8, which is 20 sets) is affected. It printed
44,297 misses, exactly the same as a 131072-byte cache with 16 sets. The screenshot config
(8096 / 32 / 4, which is 63 sets) printed exactly the 4096-byte result.

**Now:** the set index is `block mod sets`, so all 20 sets are used (42,189 misses on the same
trace). The report adds a note that real hardware usually needs a power of two. Sizes that are
not a multiple of `block x ways` (like 8096 with 32-byte lines and 4 ways) are rejected with a
message, instead of being rounded down silently.

### 3. Crash when the cache is smaller than one set

For 128 / 64 / 4 there are 2 lines but 4 ways, so `sets = 0`. Then `log(0)` gives minus
infinity, the bit widths become garbage (tag bits printed as -2147483622), and the program
dies with `std::out_of_range` from `bitset`.

**Now:** `validate()` rejects it: `L1: associativity 4 is larger than the 2 lines in the cache
(size must be >= block x ways)`.

### 4. Floating-point bit widths

`offset = log(block_size)/log(2)` is computed in floating point and truncated. For powers of
two this usually rounds correctly, but it is fragile and unnecessary.

**Now:** integer `log2` on validated powers of two.

### 5. Trace reading stops at the first short line

`readMemory` breaks out of the loop at any line shorter than 2 characters. A blank line in
the middle of a trace silently drops everything after it.

**Now:** blank lines, comments and Valgrind banners are skipped; every other line must parse or
the tool reports the file and line number.

### 6. Slow per-access string work

Each access converts a `bitset<32>` to a `std::string` and back several times (`to_string`,
the `bitset(string, pos, n)` constructor, `stoi`). The whole trace is also copied into a
second array of structs before simulating. About 0.5 s for 0.7 million accesses.

**Now:** shifts and masks on 64-bit integers, a single-pass parser, and streaming. About 18x
faster end to end (see [BENCHMARKS.md](BENCHMARKS.md)).

### 7. Smaller issues

* `bank` is allocated with `malloc` (no constructors run for its `bitset` members) and freed in
  the destructor even if `testMem` never ran, which frees an uninitialised pointer.
* `delete` is called on pointers right after setting them to `nullptr` (harmless but
  misleading), and an unused `unordered_map` member is kept.
* Only 32-bit addresses; only reads; only LRU.
* File names are typed at an interactive prompt, so the program cannot be scripted.
* The `cachesimulator/` folder was an older Dev-C++ draft that included `Set.h` and `Block.h`,
  which were not in the repository, so it did not compile. It was dropped.
