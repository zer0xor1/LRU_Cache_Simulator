# Web visualizer

`index.html` is an interactive, step-by-step view of one cache level. It needs no build step and
no network: open it straight from disk, or serve the folder.

* Pick a size, line size, associativity, replacement policy and write policy.
* Pick a preset trace (each one shows a classic effect) or paste your own.
* Step forwards and backwards, play, or run to the end. Arrow keys and space work too.
* Each access shows the address split into tag, set index and offset bits, a plain-English
  explanation of what happened, the cache contents with policy state, and the 3C breakdown.

`cachesim.js` is the engine. It is a JavaScript port of the C++ single-level engine, and
`test/crosscheck.mjs` proves they agree:

```sh
node web/test/crosscheck.mjs build/cachesim
# 90 trace/configuration pairs identical between C++ and JavaScript
```

## Deep links

Settings can be passed in the URL hash, for example
`index.html#trace=conflict&assoc=4&policy=PLRU`. Keys: `trace` (`seq`, `conflict`, `loop`,
`hot`, `writes`, `matrix`, `custom`), `size`, `block`, `assoc` (`1`, `2`, `4`, `8`, `full`),
`policy` (`LRU`, `FIFO`, `RANDOM`, `PLRU`, `SRRIP`, `OPT`), `write` (`wb`, `wt`), `allocate=no`.

## Putting it on a Next.js portfolio

1. Copy `index.html` and `cachesim.js` to `public/cachesim/` in the Next.js project.
2. Copy `CacheSimEmbed.tsx` into your components folder.
3. Render `<CacheSimEmbed />` on the project page. Props: `trace`, `policy`, `assoc`, `height`.

The page uses a quiet dark palette (graphite background, muted steel, sage and sand accents)
so it sits inside a dark site without fighting it. All colours are CSS variables at the top
of `index.html` if you want to match your theme exactly.
