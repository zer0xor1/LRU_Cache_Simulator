# Traces

| File | What it is | Accesses |
|---|---|---:|
| `example.trc` | The trace shipped with the original project: 32-bit read addresses, one per line. | 695,521 |
| `real/sum_matrix_lackey.trc` | A real trace of `examples/programs/sum_matrix.c`, captured with Valgrind Lackey (data accesses only). | 118,695 |
| `samples/matmul_naive_n32.trc` | 32x32 matrix multiply, naive i-j-k order. | 66,560 |
| `samples/matmul_tiled_n32_t8.trc` | The same multiply with 8x8 tiles. | 73,728 |
| `samples/loop_working_set.trc` | A 48 KiB array scanned 8 times (one access per line). | 6,144 |
| `samples/conflict_thrash.trc` | Nine addresses 4 KiB apart, repeated. They share one set in most L1 shapes. | 900 |
| `samples/stream_triad.trc` | STREAM triad `a[i] = b[i] + s * c[i]`. | 12,288 |
| `samples/pointer_chase.trc` | Following a random linked cycle through 2,048 nodes. | 20,000 |

The `samples/` files are made by `tracegen`, so you can make bigger ones:

```sh
build/tracegen matmul --n 128 --tile 16 -o big.trc
build/tracegen chase --nodes 65536 --count 1000000 | build/cachesim -s 32K -a 8 -
```

## Formats

Each line is detected on its own, so formats can be mixed.

```text
0x018ADB20        bare hex address: a read (the original project's format)
R 0x7ff0 8        R or W, address, optional size in bytes
W 7ff0,4          the same with a comma
 L 04f6b868,8     Valgrind Lackey / CMU cachelab: I, L, S, M (M = load + store)
0 1234            Dinero "din": 0 read, 1 write, 2 instruction fetch
# comment         ignored, as are blank lines and "==123==" banners
```

An access that crosses a line boundary is split into one access per line.
Instruction fetches (`I`) are ignored unless you pass `--ifetch`.

## Capturing your own trace

```sh
gcc -O1 -g -o prog prog.c
valgrind --tool=lackey --trace-mem=yes --log-file=prog.trc ./prog
build/cachesim -c configs/intel_like.cfg prog.trc
```

Lackey traces get large quickly (millions of lines per second of runtime). The simulator
streams them in constant memory, so you can also pipe Valgrind straight in:
`valgrind --tool=lackey --trace-mem=yes ./prog 2>&1 >/dev/null | build/cachesim -`.
