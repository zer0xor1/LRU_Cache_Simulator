# Benchmarks

## Method

* Trace: `traces/example.trc`, 695,521 read addresses (7.6 MB of text).
* Both programs compiled with `g++ -O2` (GCC 13.3).
* Wall clock for the whole process (start, read, parse, simulate, print, exit), median of 5
  runs, measured by `tools/bench.py`.
* Machine: 2-vCPU cloud VM (Intel Xeon, 2.1 GHz), Linux 6.18. It is a shared, fairly slow
  machine, so absolute numbers on a laptop will be lower. The ratio is the useful part.

Reproduce:

```sh
make bench          # or: python3 tools/bench.py --bin build --runs 5
```

## End to end

| Config (size / line / ways) | Original | Rewrite (with 3C) | Rewrite (`--no-3c`) | Speed-up | Same misses |
|---|---:|---:|---:|---:|:---:|
| 32768 / 64 / 4 | 495 ms | 50 ms | 28 ms | 17.7x | yes (101,379) |
| 32768 / 64 / 2 | 504 ms | 48 ms | 27 ms | 18.7x | yes (101,781) |
| 16384 / 64 / 2 | 477 ms | 51 ms | 27 ms | 17.9x | yes (102,744) |
| 16384 / 64 / 8 | 515 ms | 55 ms | 30 ms | 17.3x | yes (102,236) |

The speed-up column compares like with like: the original has no miss classification, so it
is measured against `--no-3c`. The 3C classification adds a shadow fully associative cache and
a set of seen blocks per level, which roughly doubles simulation time and is on by default
because the extra insight is usually worth 20 ms.

## Where the time goes

`--batch` loads the trace before simulating and reports the two phases separately:

```text
$ build/cachesim --batch --no-3c traces/example.trc
time      parse 22.1 ms, simulate 18.7 ms (37.2 M accesses/s)
```

| Change | Effect on this machine |
|---|---|
| Table-driven single-pass parser instead of tokenizing into strings | parse 45 ms -> 14 ms |
| Read the file with one `fread` into a pre-sized buffer | read 13 ms -> 9 ms |
| Stream (parse and simulate chunk by chunk) instead of building a vector | no 11 MB access vector; flat memory |
| Flat structure-of-arrays sets, no valid bits | 8-way lookup touches one or two host lines |
| O(1) intrusive LRU list and open-addressing hash index | fully associative caches cost the same per access as 8-way |

## Memory

Streaming keeps memory flat for any trace length. Piping 20 million random reads over a
64 MiB region through stdin into a 1 MiB, 16-way cache:

```sh
build/tracegen random --bytes 64M --count 20000000 | build/cachesim -s 1M -a 16 -
```

peaked at 55 MB RSS. Most of that is the 3C bookkeeping: about a million distinct blocks in the
`seen` set plus the 16,384-line shadow cache. A batch run of the same trace would need 320 MB
for the access vector alone.

## Policy cost

Simulation time on `example.trc`, 32 KiB 4-way, `--no-3c`, one run each (noisy):

| LRU | FIFO | Random | PLRU | SRRIP | OPT |
|---:|---:|---:|---:|---:|---:|
| 21 ms | 15 ms | 15 ms | 21 ms | 20 ms | 47 ms |

OPT includes the backward next-use pass over all 695,521 accesses.
