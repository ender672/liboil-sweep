#!/bin/bash
#
# Walk master's first-parent history oldest → newest, run the per-era harness
# at each rev, accumulate results in sweep/benchmarks.csv.
#
# Usage:
#   LIBOIL_REPO=/path/to/liboil ./sweep.sh                    # full master sweep
#   LIBOIL_REPO=... OILITERATIONS=100 ./sweep.sh              # more iterations
#   LIBOIL_REPO=... SWEEP_RANGE="A..B" ./sweep.sh             # re-run subset;
#       merges into existing CSV (rows for matched SHAs are replaced, not
#       duplicated). Any `git rev-list` range spec works.
#   LIBOIL_REPO=... SWEEP_PNG=/path/to/img.png ./sweep.sh     # override default PNG
#   LIBOIL_REPO=... ./sweep.sh --down                         # only downscales (ratio < 1)
#   LIBOIL_REPO=... ./sweep.sh --up                           # only upscales (ratio >= 1)
#
# Checkouts happen in a throwaway git worktree so the user's working tree
# (including untracked/gitignored files) is never touched.

set -o pipefail

RATIO_FILTER=""
while [ $# -gt 0 ]; do
	case "$1" in
	--down)
		[ -n "$RATIO_FILTER" ] && {
			echo "sweep: --down and --up are mutually exclusive" >&2
			exit 2
		}
		RATIO_FILTER=down
		;;
	--up)
		[ -n "$RATIO_FILTER" ] && {
			echo "sweep: --down and --up are mutually exclusive" >&2
			exit 2
		}
		RATIO_FILTER=up
		;;
	*)
		echo "sweep: unknown arg: $1" >&2
		exit 2
		;;
	esac
	shift
done
export SWEEP_RATIO_FILTER=$RATIO_FILTER

: "${LIBOIL_REPO:?LIBOIL_REPO must be set to a liboil checkout path}"
REPO=$LIBOIL_REPO
SWEEP_DIR=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
cd "$REPO" || exit 2

CSV=$SWEEP_DIR/benchmarks.csv
ERR=$SWEEP_DIR/errors.log
PNG=${SWEEP_PNG:-$REPO/USE_THIS_PNG_FOR_BENCHMARKS.png}

if [ ! -f "$PNG" ]; then
	echo "sweep: missing benchmark input PNG at $PNG" >&2
	exit 2
fi

WORKTREE=$(mktemp -d -t liboil-sweep-XXXXXX)
echo "sweep: worktree at $WORKTREE" >&2

# Collect revs to benchmark and decide CSV/ERR handling.
# Default (SWEEP_RANGE unset): full master sweep, fresh CSV + ERR.
# SWEEP_RANGE set: preserve existing CSV/ERR, drop rows for in-range SHAs
# so the upcoming run replaces them instead of duplicating.
if [ -n "${SWEEP_RANGE:-}" ]; then
	if [ ! -f "$CSV" ]; then
		echo "sweep: SWEEP_RANGE set but no existing CSV at $CSV" >&2
		exit 2
	fi
	REVS=$(git rev-list --first-parent --reverse "$SWEEP_RANGE")
	if [ -z "$REVS" ]; then
		echo "sweep: SWEEP_RANGE=$SWEEP_RANGE matched no commits" >&2
		exit 2
	fi
	shas_tmp=$(mktemp)
	printf '%s\n' "$REVS" > "$shas_tmp"
	awk -F, -v shalist_file="$shas_tmp" '
		BEGIN {
			while ((getline s < shalist_file) > 0) drop[s] = 1
			close(shalist_file)
		}
		NR == 1 || !($2 in drop) { print }
	' "$CSV" > "$CSV.tmp" && mv "$CSV.tmp" "$CSV"
	rm -f "$shas_tmp"
	echo "sweep: SWEEP_RANGE=$SWEEP_RANGE ($(printf '%s\n' "$REVS" | wc -l) revs); merging into $CSV" >&2
else
	REVS=$(git rev-list --first-parent --reverse master)
	echo "date,git_revision,color_space,backend,scale_ratio,time_ms" > "$CSV"
	: > "$ERR"
fi

cleanup() {
	# Best-effort: drop the worktree. `git worktree remove --force` is
	# happy even if HEAD is detached or the dir has uncommitted changes.
	if [ -n "$WORKTREE" ] && [ -d "$WORKTREE" ]; then
		git worktree remove --force "$WORKTREE" 2>/dev/null || \
			rm -rf "$WORKTREE"
	fi
}
trap cleanup EXIT INT TERM

# Create a detached worktree seeded at the current master. Per-rev checkouts
# happen inside it and never affect the caller's working tree.
if ! git worktree add --detach -q "$WORKTREE" master 2>>"$ERR"; then
	echo "sweep: failed to create worktree" >&2
	exit 2
fi

export OILITERATIONS=${OILITERATIONS:-1}
export SWEEP_DIR
export PNG
echo "sweep: OILITERATIONS=$OILITERATIONS" >&2
[ -n "$SWEEP_RATIO_FILTER" ] && echo "sweep: ratio filter: $SWEEP_RATIO_FILTER" >&2

# Decode the input PNG once into a raw RGBA cache so each harness invocation
# can skip libpng entirely. Saves ~50 ms per harness call — at ~120 calls per
# v4 rev that's ~6 s/rev of pure decode cost, regardless of OILITERATIONS.
# Cache invalidates on PNG or decoder-source mtime; the harness falls back to
# libpng if HARNESS_RAW isn't set (e.g. running a harness by hand).
CACHE_DIR=$SWEEP_DIR/.cache
mkdir -p "$CACHE_DIR"
RAW_CACHE=$CACHE_DIR/input.raw
RAW_META=$CACHE_DIR/input.meta
DECODER=$CACHE_DIR/decode_png
if [ ! -x "$DECODER" ] || [ "$SWEEP_DIR/decode_png.c" -nt "$DECODER" ]; then
	if ! ${CC:-cc} -O2 "$SWEEP_DIR/decode_png.c" -o "$DECODER" -lpng 2>>"$ERR"; then
		echo "sweep: failed to build decode_png; see $ERR" >&2
		exit 2
	fi
fi
if [ ! -f "$RAW_CACHE" ] || [ ! -f "$RAW_META" ] \
	|| [ "$PNG" -nt "$RAW_CACHE" ] || [ "$DECODER" -nt "$RAW_CACHE" ]; then
	if ! "$DECODER" "$PNG" "$RAW_CACHE" "$RAW_META" 2>>"$ERR"; then
		echo "sweep: failed to decode $PNG into raw cache" >&2
		exit 2
	fi
	echo "sweep: cached raw RGBA at $RAW_CACHE" >&2
fi
read -r RAW_W RAW_H < "$RAW_META"
export HARNESS_RAW=$RAW_CACHE
export HARNESS_RAW_W=$RAW_W
export HARNESS_RAW_H=$RAW_H

# Extract the (possibly multi-line) yscaler_scale declaration from a header
# and normalize whitespace so the result fits on one line.
_yscaler_scale_sig() {
	awk '/yscaler_scale\(/,/\);/' "$1" | tr '\n\t' '  '
}

detect_era() {
	if [ -f oil_resample.h ] && grep -q 'oil_scale_init' oil_resample.h; then
		echo v4
	elif [ -f resample.h ] && grep -q 'preprocess_xscaler_init' resample.h; then
		echo v3c
	elif [ -f resample.h ] && grep -q 'xscaler_init' resample.h; then
		echo v3b
	elif [ -f yscaler.h ] && grep -q 'yscaler_init' yscaler.h; then
		# v2 era: yscaler.h exists. Discriminate by yscaler_scale sig.
		if grep -q 'enum oil_fmt fmt)' yscaler.h; then
			echo v2a   # fmt-based, 2-arg yscaler_scale
		else
			local ysig
			ysig=$(_yscaler_scale_sig yscaler.h)
			if echo "$ysig" | grep -q 'uint32_t pos'; then
				echo v2d   # 6-arg yscaler_scale with pos
			elif echo "$ysig" | grep -q 'uint32_t width'; then
				echo v2c   # 5-arg yscaler_scale
			else
				echo v2b   # 4-arg yscaler_scale(ys,out,cmp,opts)
			fi
		fi
	elif [ -f resample.h ] && grep -q 'yscaler_init' resample.h; then
		# yscaler was merged into resample.h. Two API shapes:
		#   v2e: 6-arg yscaler_scale with pos (transitional, same API as v2d)
		#   v3a: 3-arg yscaler_scale(ys,out,pos); xscale dropped `opts`
		local ysig
		ysig=$(_yscaler_scale_sig resample.h)
		if echo "$ysig" | grep -q 'uint32_t width'; then
			echo v2e
		else
			echo v3a
		fi
	elif [ -f resample.h ] && grep -q 'padded_sl_init' resample.h; then
		echo v1
	else
		echo unknown
	fi
}

total=$(printf '%s\n' "$REVS" | wc -l)
i=0

# Revs known to be unbenchmarkable WIP checkpoints. 7865fd4 is the
# "add SSE support" commit — its oil_scale_out hardcodes the RGB-downscale
# math and early-returns before the general dispatch, so any upscale ratio
# segfaults and only RGB gives meaningful scalar numbers. Its successor
# (b3a3711) restores proper dispatch and is cleanly benchmarkable.
SKIP_REVS=" 7865fd4ed7fae93aebae56f788e0ccf3f98bce33 "

# Iterate in a process-substitution to keep vars in the same shell
while IFS= read -r sha; do
	i=$((i+1))
	date=$(git show -s --format=%cI "$sha")

	case "$SKIP_REVS" in *" $sha "*)
		echo "[$i/$total] $sha skipped-wip" >&2
		echo "$sha $date skipped-wip" >> "$ERR"
		continue
		;;
	esac

	# Clean prior rev's artifacts from the worktree. -xdff includes
	# ignored files (build output) so the next checkout won't hit
	# "untracked file would be overwritten". Scoped to the worktree so
	# it can't touch the caller's files.
	(cd "$WORKTREE" && git clean -xdff -q) 2>>"$ERR" || true

	if ! (cd "$WORKTREE" && git checkout --detach -q "$sha") 2>>"$ERR"; then
		echo "[$i/$total] $sha checkout-fail" >&2
		echo "$sha $date checkout-fail" >> "$ERR"
		continue
	fi

	era=$( cd "$WORKTREE" && detect_era )
	case "$era" in
	unknown)
		echo "[$i/$total] $sha era=unknown (pre-library rev)" >&2
		echo "$sha $date era-unknown" >> "$ERR"
		continue
		;;
	v4|v3c|v3b|v2a|v2b|v2c|v2d|v2e|v3a)
		;;
	v1)
		echo "[$i/$total] $sha era=$era (skip: v1 not benchmarked)" >&2
		echo "$sha $date era-$era-skipped" >> "$ERR"
		continue
		;;
	esac

	if ! ( cd "$WORKTREE" && "$SWEEP_DIR/run.sh" "$era" "$sha" "$date" ) \
		>>"$CSV" 2>>"$ERR"; then
		echo "[$i/$total] $sha era=$era build-or-run-fail" >&2
		echo "$sha $date build-or-run-fail" >> "$ERR"
		continue
	fi

	echo "[$i/$total] $sha era=$era ok" >&2
done < <(printf '%s\n' "$REVS")

echo "sweep: complete; CSV=$CSV errors=$ERR" >&2
