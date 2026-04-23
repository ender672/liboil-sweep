#!/bin/bash
#
# Per-revision harness builder + runner.
#
# Usage: run.sh <era> <sha> <date>
#
# Invoked by sweep.sh with cwd set to the per-rev worktree (so `.c` and `.h`
# source paths here resolve against the rev's tree). Environment inputs:
#
#   SWEEP_DIR  - path to sweep/ in the main repo (harness .c files live here)
#   PNG        - absolute path to the benchmark input PNG
#   OILITERATIONS - forwarded to the harness
#   SWEEP_CFLAGS / CC  - optional compiler overrides
#
# Writes CSV rows to stdout:
#   date,git_revision,color_space,backend,scale_ratio,time_ms
#
# SWEEP_NO_SIMD=1 forces a scalar-only build by (a) not compiling the SIMD
# sibling .c files, (b) defining -DOIL_NO_SIMD (which the rev's internal
# header uses to gate OIL_USE_SSE2 / OIL_USE_NEON), and (c) for pre-OIL_NO_SIMD
# revs, sed-patching `#if defined(__x86_64__)` gates in oil_resample.c so the
# scalar fallbacks are compiled+taken. Only v4 era is affected.

set -e
set -o pipefail

era=$1
sha=$2
date=$3

if [ -z "$era" ] || [ -z "$sha" ] || [ -z "$date" ]; then
	echo "usage: run.sh <era> <sha> <date>" >&2
	exit 2
fi

if [ -z "$SWEEP_DIR" ] || [ -z "$PNG" ]; then
	echo "run.sh: SWEEP_DIR and PNG env vars must be set" >&2
	exit 2
fi

if [ ! -f "$PNG" ]; then
	echo "missing input PNG: $PNG" >&2
	exit 2
fi

CC=${CC:-cc}
CFLAGS=${SWEEP_CFLAGS:-"-O2 -march=native"}
ARCH=$(uname -m)

# Per-rev build artifacts land in CWD (the worktree root), not in sweep/.
rm -f oil_resample.o sse2.o avx2.o neon.o resample.o yscaler.o harness probe.h

# Safety: harness_v4.c uses `#include "probe.h"` (quoted), which the C
# preprocessor resolves relative to the file that includes it — i.e.
# $SWEEP_DIR — BEFORE consulting -I paths. An old/stale probe.h there
# would silently shadow the freshly-generated one in CWD. Delete it.
rm -f "$SWEEP_DIR/probe.h"

case "$era" in
v4)
	# Generate probe.h listing HAS_CS_* / HAS_SSE2 / HAS_AVX2 / HAS_NEON
	{
		for cs in G GA RGB RGBX RGBA ARGB CMYK \
			RGB_NOGAMMA RGBA_NOGAMMA RGBX_NOGAMMA; do
			if grep -q "OIL_CS_${cs}[[:space:]]*=" oil_resample.h; then
				echo "#define HAS_CS_${cs} 1"
			fi
		done
		# Only advertise SIMD backends that also match the host arch,
		# and skip them entirely when SWEEP_NO_SIMD=1.
		if [ "${SWEEP_NO_SIMD:-0}" != 1 ]; then
			case "$ARCH" in
			x86_64)   avail_simd="sse2 avx2" ;;
			aarch64|arm64) avail_simd="neon" ;;
			*)        avail_simd="" ;;
			esac
			for simd in $avail_simd; do
				up=$(echo "$simd" | tr a-z A-Z)
				# Require both a header declaration AND an
				# implementation sibling. HEAD declares an
				# `oil_scale_in_avx2` prototype but no longer
				# ships oil_resample_avx2.c, so the header
				# check alone would advertise a backend that
				# then fails to link.
				if grep -q "oil_scale_in_${simd}" oil_resample.h \
				   && [ -f "oil_resample_${simd}.c" ]; then
					echo "#define HAS_${up} 1"
				fi
			done
		fi
	} > probe.h

	# Build the core object
	core_src=oil_resample.c
	core_cflags=""
	if [ "${SWEEP_NO_SIMD:-0}" = 1 ]; then
		core_cflags="-DOIL_NO_SIMD"
		# Pre-internal.h revs (e.g. 787bf8e): gate is raw
		# `#if defined(__x86_64__)` in oil_resample.c itself.
		# Sed-patch a working copy so the SSE paths are skipped and
		# the scalar paths (guarded by `#if !defined(__x86_64__)`)
		# compile unconditionally.
		if ! [ -f oil_resample_internal.h ] \
			&& grep -q 'defined(__x86_64__)' oil_resample.c; then
			# Flip SSE-gates off and scalar-gates on, and redirect
			# stray `_oil_scale_in` dead-code calls from the SSE
			# variant to the now-always-compiled scalar variant.
			sed -e 's/#if defined(__x86_64__)/#if 0/g' \
				-e 's/#if !defined(__x86_64__)/#if 1/g' \
				-e 's/scale_down_rgb_sse(/scale_down_rgb(/g' \
				oil_resample.c > oil_resample_nosimd.c
			core_src=oil_resample_nosimd.c
		fi
	fi
	$CC $CFLAGS $core_cflags -I. -c $core_src -o oil_resample.o
	objs="oil_resample.o"

	# Build SIMD siblings only when SIMD is enabled
	if [ "${SWEEP_NO_SIMD:-0}" != 1 ]; then
		if [ "$ARCH" = x86_64 ] && [ -f oil_resample_sse2.c ]; then
			$CC $CFLAGS -msse2 -c oil_resample_sse2.c -o sse2.o
			objs="$objs sse2.o"
		fi
		if [ "$ARCH" = x86_64 ] && [ -f oil_resample_avx2.c ]; then
			$CC $CFLAGS -mavx2 -mfma -c oil_resample_avx2.c \
				-o avx2.o
			objs="$objs avx2.o"
		fi
		if { [ "$ARCH" = aarch64 ] || [ "$ARCH" = arm64 ]; } \
			&& [ -f oil_resample_neon.c ]; then
			$CC $CFLAGS -c oil_resample_neon.c -o neon.o
			objs="$objs neon.o"
		fi
	fi

	# -I. picks up the rev's oil_resample.h; harness_v4.c includes probe.h
	# which we generated in CWD above.
	$CC $CFLAGS -I. "$SWEEP_DIR/harness_v4.c" $objs \
		-o harness -lpng -lm

	# Classify "embedded SSE2" era — oil_resample.c runs SSE2 on x86_64 but
	# the public header does NOT yet export a separate oil_scale_in_sse2
	# entry point, so the harness only reports one entry ("scalar") even
	# though the code is really SSE2. Two sub-ranges, distinguished by how
	# SSE is wired in:
	#
	#   embedded_sse=1 (PRIMARY): SSE is gated by `#if defined(__x86_64__)`
	#       or OIL_USE_SSE2. SWEEP_NO_SIMD=1 can disable it cleanly, so
	#       we do a second -DOIL_NO_SIMD pass below to recover real scalar
	#       timings. Normal builds get "scalar" relabeled to "sse2".
	#
	#   embedded_sse=2 (GREY): SSE intrinsics (`_mm_*` / `__m128i`) are
	#       inlined into hot paths with NO preprocessor gate. A -DOIL_NO_SIMD
	#       rebuild produces the same hybrid timings, so: relabel only, no
	#       second pass. (The 2026-03-30 window; ~6 revs.)
	#
	# Revs with a runtime `use_sse` flag (default off) would also match the
	# OIL_USE_SSE2 grep, but the harness never flips the flag so the actual
	# path is genuine scalar. No clean source-level discriminator; in
	# practice the overlap is historically benign.
	embedded_sse=0
	if [ "$ARCH" = x86_64 ] \
	   && ! grep -q 'oil_scale_in_sse2' oil_resample.h; then
		if grep -qE '__x86_64__|OIL_USE_SSE2' oil_resample.c; then
			embedded_sse=1
		elif grep -qE '_mm_[a-z]|__m128i' oil_resample.c; then
			embedded_sse=2
		fi
	fi
	;;

v3c)
	# late-v3: enum oil_colorspace + preprocess_xscaler + fix_ratio
	$CC $CFLAGS -c resample.c -o resample.o
	$CC $CFLAGS -I. "$SWEEP_DIR/harness_v3c.c" resample.o \
		-o harness -lpng -lm
	;;

v3b)
	# middle-v3: xscaler_init(cmp,filler) + fix_ratio, no enum
	$CC $CFLAGS -c resample.c -o resample.o
	$CC $CFLAGS -I. "$SWEEP_DIR/harness_v3b.c" resample.o \
		-o harness -lpng -lm
	;;

v2a|v2b|v2c|v2d|v2e|v3a)
	# v2/v3a era: separate or merged yscaler, xscale() free fn.
	# Each sub-era has its own harness matching that era's function
	# signatures. harness_png.h is included from each and must be on -I.
	case "$era" in
	v2a) harness=$SWEEP_DIR/harness_v2a.c ;;
	v2b) harness=$SWEEP_DIR/harness_v2b.c ;;
	v2c) harness=$SWEEP_DIR/harness_v2.c  ;;
	v2d) harness=$SWEEP_DIR/harness_v2d.c ;;
	v2e) harness=$SWEEP_DIR/harness_v2e.c ;;
	v3a) harness=$SWEEP_DIR/harness_v3a.c ;;
	esac
	$CC $CFLAGS -c resample.c -o resample.o
	objs="resample.o"
	if [ -f yscaler.c ]; then
		$CC $CFLAGS -c yscaler.c -o yscaler.o
		objs="$objs yscaler.o"
	fi
	$CC $CFLAGS -I. -I"$SWEEP_DIR" "$harness" $objs \
		-o harness -lpng -lm
	;;

v1)
	echo "era v1 harness not implemented (single rev, not worth it)" >&2
	exit 3
	;;

*)
	echo "unknown era: $era" >&2
	exit 2
	;;
esac

# Run harness; prepend date + sha to each CSV row. If the v4 build
# detected embedded-SSE (no split sse2 backend but intrinsics in the
# main source), relabel "scalar" -> "sse2" so the plot line is continuous.
# Exception: a PRIMARY rev (embedded_sse=1) built with SWEEP_NO_SIMD=1
# really did produce scalar output (gates were flipped), so leave it alone.
# GREY revs (embedded_sse=2) can't have SSE turned off, so always relabel.
relabel_emb=0
case "${embedded_sse:-0}" in
1) [ "${SWEEP_NO_SIMD:-0}" != 1 ] && relabel_emb=1 ;;
2) relabel_emb=1 ;;
esac
./harness "$PNG" | \
	awk -v d="$date" -v r="$sha" -v emb="$relabel_emb" '
		BEGIN { OFS = "," }
		NF {
			if (emb == 1) sub(/,scalar,/, ",sse2,")
			print d, r, $0
		}'

# Embedded-SSE range: first pass produced `sse2` rows (via relabel). Do a
# second pass with -DOIL_NO_SIMD to recover genuine scalar timings so the
# scalar line stays continuous across the range. Skip if we're already
# in a NOSIMD-only sweep.
if [ "${embedded_sse:-0}" = 1 ] && [ "${SWEEP_NO_SIMD:-0}" != 1 ]; then
	rm -f oil_resample.o harness
	nosimd_src=oil_resample.c
	if ! [ -f oil_resample_internal.h ] \
	   && grep -q 'defined(__x86_64__)' oil_resample.c; then
		# Pre-internal.h revs: flip raw __x86_64__ gates in-source.
		sed -e 's/#if defined(__x86_64__)/#if 0/g' \
			-e 's/#if !defined(__x86_64__)/#if 1/g' \
			-e 's/scale_down_rgb_sse(/scale_down_rgb(/g' \
			oil_resample.c > oil_resample_nosimd.c
		nosimd_src=oil_resample_nosimd.c
	fi
	$CC $CFLAGS -DOIL_NO_SIMD -I. -c "$nosimd_src" -o oil_resample.o
	$CC $CFLAGS -I. "$SWEEP_DIR/harness_v4.c" oil_resample.o \
		-o harness -lpng -lm
	./harness "$PNG" | \
		awk -v d="$date" -v r="$sha" 'BEGIN{OFS=","} NF {print d,r,$0}'
fi
