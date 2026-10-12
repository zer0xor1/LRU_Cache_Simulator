#!/usr/bin/env python3
"""Regenerate the README / portfolio charts from real simulator runs.

    python3 tools/make_charts.py --bin build

Runs `cachesim` and `tracegen` from the build directory, writes the raw
numbers to docs/data/*.csv and draws every chart twice (light and dark)
as hand-written SVG in docs/images/. No plotting library is needed.
"""
from __future__ import annotations

import argparse
import csv
import io
import json
import math
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# ---------------------------------------------------------------------------
# Theme tokens. Categorical slots were checked with a CVD validator for both
# surfaces (adjacent-pair delta E >= 8). Text never uses series colours.
# ---------------------------------------------------------------------------
THEMES = {
    "light": dict(surface="#fcfcfb", ink="#0b0b0b", ink2="#52514e", muted="#6f6d68",
                  grid="#e8e7e1", axis="#c3c2b7",
                  series=["#2a78d6", "#eb6834", "#1baf7a", "#eda100"]),
    "dark": dict(surface="#141719", ink="#e6e8e9", ink2="#b4bbbf", muted="#8a949c",
                 grid="#252a2e", axis="#3a4147",
                 series=["#3987e5", "#d95926", "#199e70", "#c98500"]),
}
FONT = "system-ui, -apple-system, 'Segoe UI', Roboto, Helvetica, Arial, sans-serif"


def esc(s: str) -> str:
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;").replace('"', "&quot;")


def size_label(b: int) -> str:
    if b >= 1 << 20:
        return f"{b >> 20}M"
    if b >= 1 << 10:
        return f"{b >> 10}K"
    return str(b)


def pct(v: float, digits: int = 1) -> str:
    return f"{v * 100:.{digits}f}%"


class Svg:
    def __init__(self, w: int, h: int, theme: dict, title: str, desc: str):
        self.w, self.h, self.t = w, h, theme
        self.parts = [
            f'<svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="{h}" viewBox="0 0 {w} {h}" '
            f'role="img" aria-labelledby="t d" font-family="{FONT}">',
            f"<title id=\"t\">{esc(title)}</title><desc id=\"d\">{esc(desc)}</desc>",
            f'<rect width="{w}" height="{h}" rx="10" fill="{theme["surface"]}"/>',
        ]

    def add(self, s: str) -> None:
        self.parts.append(s)

    def text(self, x, y, s, size=12, color=None, anchor="start", weight=400, extra=""):
        color = color or self.t["ink2"]
        self.add(f'<text x="{x:.1f}" y="{y:.1f}" font-size="{size}" fill="{color}" text-anchor="{anchor}" '
                 f'font-weight="{weight}" {extra}>{esc(s)}</text>')

    def line(self, x1, y1, x2, y2, color, width=1.0):
        self.add(f'<line x1="{x1:.1f}" y1="{y1:.1f}" x2="{x2:.1f}" y2="{y2:.1f}" stroke="{color}" '
                 f'stroke-width="{width}" shape-rendering="crispEdges"/>')

    def header(self, title: str, subtitle: str):
        self.text(24, 32, title, size=15, color=self.t["ink"], weight=600)
        self.text(24, 52, subtitle, size=12, color=self.t["ink2"])

    def legend(self, items, x, y, kind="line"):
        """items: list of (label, colour). Line keys for line charts, squares for bars."""
        cx = x
        for label, colour in items:
            if kind == "line":
                self.add(f'<line x1="{cx}" y1="{y - 4}" x2="{cx + 18}" y2="{y - 4}" stroke="{colour}" '
                         f'stroke-width="2" stroke-linecap="round"/>')
                self.add(f'<circle cx="{cx + 9}" cy="{y - 4}" r="4" fill="{colour}" stroke="{self.t["surface"]}" '
                         f'stroke-width="2"/>')
                cx += 26
            else:
                self.add(f'<rect x="{cx}" y="{y - 10}" width="12" height="12" rx="3" fill="{colour}"/>')
                cx += 18
            self.text(cx, y, label, size=12, color=self.t["ink2"])
            cx += 8 + 7 * len(label)

    def save(self, path: str):
        self.add("</svg>")
        with open(path, "w", encoding="utf-8") as f:
            f.write("\n".join(self.parts) + "\n")


def hbar_path(x0, y, length, thick, r=4.0):
    """Horizontal bar from x0 rightwards: square at the baseline, rounded data end."""
    r = min(r, length / 2, thick / 2)
    x1 = x0 + length
    return (f"M{x0:.1f},{y:.1f} H{x1 - r:.1f} Q{x1:.1f},{y:.1f} {x1:.1f},{y + r:.1f} "
            f"V{y + thick - r:.1f} Q{x1:.1f},{y + thick:.1f} {x1 - r:.1f},{y + thick:.1f} H{x0:.1f} Z")


# ---------------------------------------------------------------------------
# Running the tools
# ---------------------------------------------------------------------------
class Tools:
    def __init__(self, bindir: str, tmp: str):
        exe = ".exe" if os.name == "nt" else ""
        self.cachesim = os.path.join(bindir, "cachesim" + exe)
        self.tracegen = os.path.join(bindir, "tracegen" + exe)
        self.tmp = tmp
        for p in (self.cachesim, self.tracegen):
            if not os.path.exists(p):
                sys.exit(f"missing {p}; build first (cmake --build build)")

    def gen(self, name: str, *args: str) -> str:
        path = os.path.join(self.tmp, name + ".trc")
        if not os.path.exists(path):
            subprocess.run([self.tracegen, *args, "-o", path], check=True, stderr=subprocess.DEVNULL)
        return path

    def run_json(self, trace: str, *args: str) -> dict:
        out = subprocess.run([self.cachesim, "--json", *args, trace], check=True, capture_output=True, text=True)
        return json.loads(out.stdout)

    def sweep(self, trace: str, *args: str) -> list[dict]:
        out = subprocess.run([self.cachesim, "--sweep", *args, trace], check=True, capture_output=True, text=True)
        return list(csv.DictReader(io.StringIO(out.stdout)))


def write_csv(path: str, rows: list[dict]):
    with open(path, "w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        w.writeheader()
        w.writerows(rows)


# ---------------------------------------------------------------------------
# Chart 1: miss rate vs cache size for several associativities
# ---------------------------------------------------------------------------
def line_chart(path, theme, title, subtitle, xs, series, ylog=False, ymax=None, ymin=0.0, xlabel="", ylabel="",
               label_at=None, desc=""):
    """xs: list of byte sizes (log2 axis). series: list of (name, [values 0..1])."""
    t = THEMES[theme]
    W, H = 760, 380
    L, R, T, B = 64, 24, 96, 52
    pw, ph = W - L - R, H - T - B
    svg = Svg(W, H, t, title, desc)
    svg.header(title, subtitle)
    svg.legend([(n, t["series"][i]) for i, (n, _) in enumerate(series)], 24, 78)

    lx0, lx1 = math.log2(xs[0]), math.log2(xs[-1])
    X = lambda b: L + (math.log2(b) - lx0) / (lx1 - lx0) * pw
    if ylog:
        lo = min(v for _, vs in series for v in vs if v > 0)
        y0 = 10 ** math.floor(math.log10(lo))
        y1 = 1.0
        ly0, ly1 = math.log10(y0), math.log10(y1)
        Y = lambda v: T + ph - (math.log10(max(v, y0)) - ly0) / (ly1 - ly0) * ph
        ticks = [10 ** e for e in range(int(ly0), int(ly1) + 1)]
        tick_label = lambda v: f"{v * 100:g}%"
    else:
        top = ymax or max(v for _, vs in series for v in vs) * 1.1
        step = 0.05 if top - ymin > 0.12 else 0.02
        top = math.ceil(top / step) * step
        bot = math.floor(ymin / step) * step
        Y = lambda v: T + ph - (v - bot) / (top - bot) * ph
        ticks = [bot + i * step for i in range(int(round((top - bot) / step)) + 1)]
        tick_label = lambda v: f"{v * 100:.0f}%"

    for v in ticks:
        y = Y(v)
        svg.line(L, y, L + pw, y, t["grid"])
        svg.text(L - 8, y + 4, tick_label(v), size=11, color=t["muted"], anchor="end",
                 extra='style="font-variant-numeric: tabular-nums"')
    svg.line(L, T + ph, L + pw, T + ph, t["axis"])
    for b in xs:
        svg.text(X(b), T + ph + 18, size_label(b), size=11, color=t["muted"], anchor="middle")
    if xlabel:
        svg.text(L + pw / 2, H - 10, xlabel, size=11, color=t["muted"], anchor="middle")
    if ylabel:
        svg.text(16, T + ph / 2, ylabel, size=11, color=t["muted"], anchor="middle",
                 extra=f'transform="rotate(-90 16 {T + ph / 2:.1f})"')

    for i, (name, vs) in enumerate(series):
        c = t["series"][i]
        pts = " ".join(f"{X(b):.1f},{Y(v):.1f}" for b, v in zip(xs, vs))
        svg.add(f'<polyline points="{pts}" fill="none" stroke="{c}" stroke-width="2" stroke-linejoin="round" '
                f'stroke-linecap="round"/>')
        for b, v in zip(xs, vs):
            svg.add(f'<circle cx="{X(b):.1f}" cy="{Y(v):.1f}" r="4" fill="{c}" stroke="{t["surface"]}" '
                    f'stroke-width="2"><title>{esc(name)}, {size_label(b)}: {pct(v, 2)}</title></circle>')
    if label_at:
        for i, (name, vs) in enumerate(series):
            k, dy = label_at[i]
            svg.text(X(xs[k]) + 8, Y(vs[k]) + dy, f"{name} {pct(vs[k])}", size=11, color=t["ink2"])
    svg.save(path)


# ---------------------------------------------------------------------------
# Chart 2: policies, small multiples of horizontal bars (one hue: the
# category is on the axis, so colour carries nothing extra)
# ---------------------------------------------------------------------------
def policy_chart(path, theme, title, subtitle, facets, desc=""):
    """facets: list of (facet title, facet note, [(policy, miss_rate)])"""
    t = THEMES[theme]
    cols = 2
    rows = math.ceil(len(facets) / cols)
    W = 760
    fw, fh = 340, 188
    H = 80 + rows * (fh + 12)
    svg = Svg(W, H, t, title, desc)
    svg.header(title, subtitle)
    for idx, (ftitle, fnote, bars) in enumerate(facets):
        ox = 24 + (idx % cols) * (fw + 32)
        oy = 80 + (idx // cols) * (fh + 12)
        svg.text(ox, oy + 14, ftitle, size=13, color=t["ink"], weight=600)
        svg.text(ox, oy + 30, fnote, size=11, color=t["muted"])
        label_w, bar_x = 58, ox + 58
        avail = fw - label_w - 52
        top = max(v for _, v in bars)
        thick, gap = 16, 8
        y = oy + 44
        svg.line(bar_x, y - 4, bar_x, y + len(bars) * (thick + gap) - gap + 4, t["axis"])
        for name, v in bars:
            length = max(2.0, v / top * avail) if top > 0 else 2.0
            colour = t["series"][0]
            svg.text(bar_x - 8, y + 12, name, size=11, color=t["ink2"], anchor="end")
            svg.add(f'<path d="{hbar_path(bar_x, y, length, thick)}" fill="{colour}">'
                    f"<title>{esc(ftitle)}: {name} {pct(v, 2)}</title></path>")
            svg.text(bar_x + length + 6, y + 12, pct(v), size=11, color=t["ink2"],
                     extra='style="font-variant-numeric: tabular-nums"')
            y += thick + gap
    svg.save(path)


# ---------------------------------------------------------------------------
# Chart 3: 3C breakdown, stacked horizontal bars
# ---------------------------------------------------------------------------
def three_c_chart(path, theme, title, subtitle, rows_data, desc=""):
    """rows_data: list of (label, compulsory, capacity, conflict, accesses)"""
    t = THEMES[theme]
    W = 760
    thick, gap = 20, 12
    T = 96
    H = T + len(rows_data) * (thick + gap) + 40
    svg = Svg(W, H, t, title, desc)
    svg.header(title, subtitle)
    names = ["compulsory", "capacity", "conflict"]
    svg.legend([(n, t["series"][i]) for i, n in enumerate(names)], 24, 78, kind="box")
    L = 104
    pw = W - L - 90
    top = max(c + p + f for _, c, p, f, _ in rows_data) / rows_data[0][4]
    top = math.ceil(top * 100 / 5) * 5 / 100
    X = lambda frac: L + frac / top * pw
    # gridlines every 5%
    k = 0.0
    while k <= top + 1e-9:
        svg.line(X(k), T - 6, X(k), T + len(rows_data) * (thick + gap) - gap + 4, t["grid"])
        svg.text(X(k), T + len(rows_data) * (thick + gap) + 12, f"{k * 100:.0f}%", size=11, color=t["muted"],
                 anchor="middle")
        k += 0.05
    y = T
    for label, comp, cap, conf, acc in rows_data:
        svg.text(L - 10, y + 14, label, size=12, color=t["ink2"], anchor="end")
        x = X(0)
        segs = [(comp, 0), (cap, 1), (conf, 2)]
        segs = [(v, i) for v, i in segs if v > 0]
        for j, (v, i) in enumerate(segs):
            w = v / acc / top * pw
            last = j == len(segs) - 1
            draw_w = w - (0 if last else 2)  # 2px surface gap between segments
            if draw_w <= 0:
                x += w
                continue
            colour = t["series"][i]
            tip = f"{label}: {names[i]} {v:,} misses ({pct(v / acc, 2)} of accesses)"
            if last:
                svg.add(f'<path d="{hbar_path(x, y, draw_w, thick)}" fill="{colour}"><title>{esc(tip)}</title></path>')
            else:
                svg.add(f'<rect x="{x:.1f}" y="{y}" width="{draw_w:.1f}" height="{thick}" fill="{colour}">'
                        f"<title>{esc(tip)}</title></rect>")
            x += w
        total = (comp + cap + conf) / acc
        svg.text(x + 8, y + 14, pct(total), size=11, color=t["ink2"], extra='style="font-variant-numeric: tabular-nums"')
        y += thick + gap
    svg.text(L + pw / 2, H - 8, "miss rate (share of all accesses)", size=11, color=t["muted"], anchor="middle")
    svg.save(path)


# ---------------------------------------------------------------------------
def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--bin", default=os.path.join(ROOT, "build"), help="directory with cachesim and tracegen")
    ap.add_argument("--out", default=os.path.join(ROOT, "docs"), help="docs directory")
    args = ap.parse_args()

    img_dir = os.path.join(args.out, "images")
    data_dir = os.path.join(args.out, "data")
    os.makedirs(img_dir, exist_ok=True)
    os.makedirs(data_dir, exist_ok=True)

    with tempfile.TemporaryDirectory() as tmp:
        tools = Tools(args.bin, tmp)
        example = os.path.join(ROOT, "traces", "example.trc")
        real = os.path.join(ROOT, "traces", "real", "sum_matrix_lackey.trc")

        # 1. size x associativity on the original trace
        rows = tools.sweep(example, "--sizes", "1K-1M", "--assocs", "1,2,8,full")
        for r in rows:  # keep the CSV independent of where the repo lives
            r["trace"] = os.path.relpath(r["trace"], ROOT).replace(os.sep, "/")
        write_csv(os.path.join(data_dir, "example_size_assoc.csv"), rows)
        xs = sorted({int(r["size_bytes"]) for r in rows})
        names = {"1": "direct-mapped", "2": "2-way", "8": "8-way", "full": "fully associative"}
        series = []
        for a in ["1", "2", "8", "full"]:
            by = {int(r["size_bytes"]): float(r["miss_rate"]) for r in rows if r["assoc"] == a}
            series.append((names[a], [by[x] for x in xs]))
        for th in THEMES:
            line_chart(os.path.join(img_dir, f"miss_rate_vs_size-{th}.svg"), th,
                       "Miss rate vs cache size (original example.trc, LRU, 64 B lines)",
                       "695,521 reads. Flat until the 1.5 MB working set starts to fit; more ways help most at large sizes.",
                       xs, series, ymax=0.17, ymin=0.06, xlabel="cache size (bytes)", ylabel="miss rate (axis starts at 6%)",
                       desc="Line chart of L1 miss rate against cache size from 1 KiB to 1 MiB for direct-mapped, "
                            "2-way, 8-way and fully associative caches. Data in docs/data/example_size_assoc.csv.")

        # 2. matrix multiply: loop order and tiling
        variants = [("naive i-j-k", ["matmul", "--n", "64"]),
                    ("loop order i-k-j", ["matmul", "--n", "64", "--order", "ikj"]),
                    ("tiled 8x8", ["matmul", "--n", "64", "--tile", "8"])]
        sizes = [2048, 4096, 8192, 16384, 32768, 65536]
        mm_rows, mm_series = [], []
        for name, gen_args in variants:
            trace = tools.gen(name.replace(" ", "_"), *gen_args)
            vals = []
            for s in sizes:
                res = tools.run_json(trace, "-s", str(s), "-a", "8", "--no-3c")
                mr = res["results"]["levels"][0]["miss_rate"]
                vals.append(mr)
                mm_rows.append({"variant": name, "size_bytes": s, "miss_rate": mr,
                                "accesses": res["results"]["levels"][0]["accesses"]})
            mm_series.append((name, vals))
        write_csv(os.path.join(data_dir, "matmul_tiling.csv"), mm_rows)
        for th in THEMES:
            line_chart(os.path.join(img_dir, f"matmul_tiling-{th}.svg"), th,
                       "Matrix multiply 64x64 (doubles): same math, very different miss rates",
                       "8-way LRU L1, 64 B lines, synthetic traces from tracegen. Log scale.",
                       sizes, mm_series, ylog=True, xlabel="L1 size (bytes)", ylabel="miss rate (log scale)",
                       label_at=[(2, -10), (2, -10), (3, 18)],
                       desc="Line chart, log scale: naive i-j-k matrix multiply misses about 50% until the cache "
                            "holds a full matrix; i-k-j order misses about 4%; 8x8 tiling about 1%. "
                            "Data in docs/data/matmul_tiling.csv.")

        # 3. replacement policies on four workloads (8 KiB, 4-way)
        workloads = [
            ("Loop 25% larger than cache", "10 KiB array scanned 50 times",
             tools.gen("loop10k", "seq", "--bytes", "10K", "--stride", "64", "--passes", "50")),
            ("Hot set + random cold data", "80% of reads in 6 KiB, rest in 1 MiB",
             tools.gen("hotcold", "hotcold", "--hot", "6K", "--cold", "1M", "--hot-pct", "80", "--count", "200000")),
            ("Real program (Valgrind)", "sum_matrix.c, row then column walk", real),
            ("Original example.trc", "695,521 reads from the forked project", example),
        ]
        pol_rows, facets = [], []
        for ftitle, fnote, trace in workloads:
            bars = []
            for p in ["LRU", "FIFO", "RANDOM", "PLRU", "SRRIP", "OPT"]:
                res = tools.run_json(trace, "-s", "8K", "-a", "4", "-p", p, "--no-3c")
                mr = res["results"]["levels"][0]["miss_rate"]
                bars.append((p, mr))
                pol_rows.append({"workload": ftitle, "policy": p, "miss_rate": mr})
            facets.append((ftitle, fnote, bars))
        write_csv(os.path.join(data_dir, "policies.csv"), pol_rows)
        for th in THEMES:
            policy_chart(os.path.join(img_dir, f"policies-{th}.svg"), th,
                         "Replacement policies: miss rate on an 8 KiB, 4-way cache",
                         "OPT (Belady) is the lower bound. Shorter is better.", facets,
                         desc="Four small bar charts comparing LRU, FIFO, Random, tree PLRU, SRRIP and OPT miss "
                              "rates. LRU thrashes on a loop slightly larger than the cache; SRRIP wins on hot/cold "
                              "data. Data in docs/data/policies.csv.")

        # 4. 3C breakdown for the real program across associativity
        c_rows, bars = [], []
        for a, label in [("1", "direct"), ("2", "2-way"), ("4", "4-way"), ("8", "8-way"), ("16", "16-way"),
                         ("full", "fully assoc.")]:
            res = tools.run_json(real, "-s", "8K", "-a", a)
            lv = res["results"]["levels"][0]
            bars.append((label, lv["compulsory"], lv["capacity"], lv["conflict"], lv["accesses"]))
            c_rows.append({"assoc": a, "accesses": lv["accesses"], "compulsory": lv["compulsory"],
                           "capacity": lv["capacity"], "conflict": lv["conflict"], "miss_rate": lv["miss_rate"]})
        write_csv(os.path.join(data_dir, "three_c_sum_matrix.csv"), c_rows)
        for th in THEMES:
            three_c_chart(os.path.join(img_dir, f"three_c-{th}.svg"), th,
                          "Why the real program misses: the 3C breakdown (8 KiB, LRU)",
                          "Walking a 128x128 matrix by column jumps 1 KiB per step and hits only a few sets.",
                          bars,
                          desc="Stacked bars of compulsory, capacity and conflict misses for a real Valgrind trace "
                               "at associativity 1 to fully associative. Conflict misses dominate until the cache "
                               "is fully associative. Data in docs/data/three_c_sum_matrix.csv.")
    print(f"charts written to {img_dir}, data to {data_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
