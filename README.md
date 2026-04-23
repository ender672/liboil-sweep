# liboil-sweep

Benchmark-sweep harness for [liboil](https://github.com/ender672/liboil). Walks the commit history of a liboil checkout, builds the library at each revision with an era-appropriate C harness, and writes timing data to `benchmarks.csv`. Useful for visualizing how per-(color_space, backend, scale_ratio) timings evolve across the library's history.

## Prerequisites

- A liboil checkout. The sweep walks its `master` branch.
- A benchmark input PNG, **RGBA**, placed at `$LIBOIL_REPO/USE_THIS_PNG_FOR_BENCHMARKS.png` (or point to another path via `SWEEP_PNG`). Any reasonably-sized real-world photo works; the loader rejects non-RGBA inputs (`harness_png.h:45`).
- A C compiler with `libpng` and `libm`.
- Python 3 (for chart generation via `plot.py`).

On x86_64, SSE2 / AVX2 flags are auto-added per backend. On aarch64, NEON is used when the rev ships a NEON backend.

## Usage

```sh
# Full sweep:
LIBOIL_REPO=/path/to/liboil ./sweep.sh

# More iterations per rev (takes min across runs — reduces timing noise):
LIBOIL_REPO=/path/to/liboil OILITERATIONS=100 ./sweep.sh

# Re-run a subset; merges results into an existing benchmarks.csv:
LIBOIL_REPO=/path/to/liboil SWEEP_RANGE="A..B" ./sweep.sh

# Custom PNG input:
LIBOIL_REPO=/path/to/liboil SWEEP_PNG=/path/to/img.png ./sweep.sh

# Generate interactive HTML charts from benchmarks.csv:
python3 plot.py benchmarks.csv -o charts
# → charts/interactive.html (thumbnail gallery), charts/chart.html (detail view)
```

A full sweep takes a while — hundreds of per-rev builds, each with its own era-specific harness. Per-rev failures land in `errors.log` and are surfaced as annotated gaps in the generated charts.

## How it works

See [`CLAUDE.md`](./CLAUDE.md) for internals: era detection, the per-era harnesses, and the v4-era SIMD handling (including the embedded-SSE range and why some revisions need a second nosimd pass).

## License

MIT — see [`LICENSE`](./LICENSE).
