# The original program

`original/` holds the unmodified source of the project this repository was forked from
([brunogracia/cachesimulator](https://github.com/brunogracia/cachesimulator), 2019, by
Bruno E. Gracia Villalobos and Ryan Walker).

It is kept for two reasons:

1. `tools/bench.py` compiles it and times it against the rewrite on the same trace.
2. `tests/test_regression.cpp` checks that the rewrite reproduces its hit and miss counts
   on every configuration where the original was correct.

It is not part of the build. See [docs/AUDIT.md](../docs/AUDIT.md) for what was wrong with it
and how each problem was fixed.

To run it yourself:

```sh
g++ -std=c++11 -O2 legacy/original/Cache.cpp legacy/original/Main.cpp -o original
printf "configs/legacy_example.cfg\ntraces/example.trc\n" | ./original
```
