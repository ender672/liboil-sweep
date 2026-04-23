# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this repo is

A benchmark-sweep harness for [liboil's](https://github.com/ender672/liboil) image-resampling code. It walks the git history of a sibling `liboil` checkout commit-by-commit, builds the library at each revision with an era-appropriate C harness, runs it against a fixed PNG, and writes one CSV for plotting. The point is to visualize how `time_ms` evolves per (color_space, backend, scale_ratio) across the library's history.

This repo contains **only** the sweep infrastructure — it is not itself a buildable C library. The liboil source lives in a separate checkout and is never modified; per-rev checkouts happen in a throwaway `git worktree`.

## Architecture: the era system

liboil's public API has mutated substantially over its history. To benchmark a single rev you need a harness whose `#include`s and function signatures match that rev's headers. The sweep does this by **detecting an "era" per commit** and picking a matching harness.

`sweep.sh::detect_era` inspects the checked-out tree (`oil_resample.h` / `resample.h` / `yscaler.h` contents) and emits one of: `v2a`, `v2b`, `v2c`, `v2d`, `v2e`, `v3a`, `v3b`, `v3c`, `v4`, `v1`, or `unknown`. Each era corresponds to a harness source file in this repo:

| Era    | Harness              | Distinguishing API shape                              |
|--------|----------------------|-------------------------------------------------------|
| v2a    | `harness_v2a.c`      | `yscaler.h` with `enum oil_fmt` 2-arg `yscaler_scale` |
| v2b    | `harness_v2b.c`      | `yscaler.h`, 4-arg `yscaler_scale(ys,out,cmp,opts)`   |
| v2c    | `harness_v2.c`       | `yscaler.h`, 5-arg (has `uint32_t width`)             |
| v2d    | `harness_v2d.c`      | `yscaler.h`, 6-arg (has `uint32_t pos`)               |
| v2e    | `harness_v2e.c`      | yscaler merged into `resample.h`, 6-arg w/ `width`    |
| v3a    | `harness_v3a.c`      | yscaler merged, 3-arg `yscaler_scale(ys,out,pos)`     |
| v3b    | `harness_v3b.c`      | `resample.h` with `xscaler_init(cmp,filler)`          |
| v3c    | `harness_v3c.c`      | `resample.h` with `preprocess_xscaler_init`           |
| v4     | `harness_v4.c`       | `oil_resample.h` with `oil_scale_init`                |
| v1     | (none — skipped)     | `resample.h` with `padded_sl_init`                    |

All harnesses share `harness_png.h` (included, not linked — there are enough per-era preprocessor variations in the surrounding scaling logic that specialization is simpler than parameterization). v3c/v4 key off `enum oil_colorspace` and translate cs → (cmp, opts, gray) before calling `load_png`; cs is then carried through run_one as a separate argument rather than in the shared `struct bench_image`.

When touching harnesses: each `harness_v*.c` is pinned to its era's API. Do not try to unify them. When adding a new era, extend `detect_era` and add a `case` in `run.sh`.

## v4 era: SIMD handling

`run.sh`'s v4 branch is doing the most work because v4 is where SIMD backends appear. It:

1. Generates `probe.h` at build time by grepping the rev's `oil_resample.h` for `OIL_CS_*` enums and `oil_scale_in_<simd>` prototypes, plus checking the corresponding `oil_resample_<simd>.c` file exists. This drives `#ifdef HAS_*` gates in `harness_v4.c` so the harness only calls backends the rev actually ships. (Stale `probe.h` in `$SWEEP_DIR` is deleted up front — it would shadow the fresh one via quoted-include resolution.)
2. Compiles `oil_resample.c` plus any SIMD siblings matching the host arch, then links `harness_v4.c`.
3. Detects the **embedded-SSE range** — a historical window where `oil_resample.c` uses SSE2 internally on x86_64 but the public header does not yet export `oil_scale_in_sse2`. Two sub-modes:
   - `embedded_sse=1` (PRIMARY): SSE is gated by `__x86_64__` or `OIL_USE_SSE2`. The normal pass relabels `scalar` → `sse2` in the CSV, then a **second** `-DOIL_NO_SIMD` pass is done to recover genuine scalar timings. For pre-`oil_resample_internal.h` revs, the nosimd pass `sed`-patches `__x86_64__` gates in-source.
   - `embedded_sse=2` (GREY): SSE intrinsics are inlined with no preprocessor gate — `-DOIL_NO_SIMD` produces identical timings, so relabel only, no second pass.

If you see discontinuities in the `scalar` or `sse2` lines on the plot in the v4 range, the embedded-SSE classification is the first thing to look at.

## Skipped revs

`SKIP_REVS` in `sweep.sh` lists commits known to be unbenchmarkable WIP. Currently `7865fd4` ("add SSE support") is the only entry — its `oil_scale_out` hardcodes RGB-downscale math and early-returns before general dispatch, so anything but an RGB downscale segfaults. Its successor `b3a3711` is fine.

## Output files

- `benchmarks.csv` — header `date,git_revision,color_space,backend,scale_ratio,time_ms`. Appended during a sweep; `*.bak` files are historical backups from prior runs.
- `errors.log` — per-rev stderr from failing builds/runs, plus one-line summaries (`<sha> <iso_date> <reason>`). `plot.py` uses the summary lines to annotate missing points.
- `charts/` — `index.html` (uPlot gallery), `chart.html` (single-chart detail view), `data.js` (dataset). Regenerate via `plot.py`. Deployed at https://liboil-bench.netlify.app.
