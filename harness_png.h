/*
 * Shared PNG loader used by every harness. Included (not linked) because the
 * era-specific scaling logic around it has preprocessor variations that are
 * simpler to specialize per-harness than to parametrize.
 *
 * The returned struct holds only what the loader actually produced: the pixel
 * buffer, its dimensions, and the per-pixel byte count after transforms.
 * Era-specific values (v2 OIL_FILLER opts, v3b filler flag, v3c/v4 colorspace
 * enum) are the caller's concern and are carried alongside, not in here.
 *
 * Fast path: if HARNESS_RAW / HARNESS_RAW_W / HARNESS_RAW_H env vars are set,
 * load RGBA pixels directly from the raw cache file and apply cs transforms
 * (gray / strip_alpha / RGBX filler) as cheap byte ops here. Skips libpng
 * entirely — this is the hot path during a sweep where the same PNG is reused
 * across hundreds of harness invocations. The slow path (libpng decode) stays
 * as a fallback when the env vars are absent so harnesses remain runnable
 * standalone.
 */
#ifndef HARNESS_PNG_H
#define HARNESS_PNG_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <setjmp.h>
#include <stdint.h>
#include <png.h>

struct bench_image {
	unsigned char *buffer;
	int width;
	int height;
	int cmp;
};

/*
 * Rec.709 luma weights scaled to /256: 0.2126 R + 0.7152 G + 0.0722 B ≈
 * (54*R + 183*G + 19*B) / 256. Used for gray=1 transforms in place of
 * libpng's png_set_rgb_to_gray. Exact luma doesn't matter for benchmark
 * timing (liboil's interpolation cost is insensitive to pixel values), but
 * we stay close to a standard so CSV rows generated via the raw cache are
 * comparable to older libpng-decoded rows.
 */
static inline unsigned char harness_rgb_to_gray(unsigned r, unsigned g,
	unsigned b)
{
	return (unsigned char)((r * 54 + g * 183 + b * 19 + 128) >> 8);
}

/*
 * Read the full RGBA raster (either from the raw cache or via libpng) into
 * *rgba_out (malloc'd, caller frees) and set *w_out/*h_out. The PNG path is
 * only consulted when the cache env vars are unset.
 */
static void harness_read_rgba(const char *png_path, unsigned char **rgba_out,
	int *w_out, int *h_out)
{
	const char *raw = getenv("HARNESS_RAW");
	const char *w_env = getenv("HARNESS_RAW_W");
	const char *h_env = getenv("HARNESS_RAW_H");

	if (raw && w_env && h_env) {
		FILE *f;
		int w = atoi(w_env);
		int h = atoi(h_env);
		size_t n = (size_t)w * (size_t)h * 4;
		unsigned char *buf = malloc(n);
		if (!buf) { fprintf(stderr, "alloc rgba\n"); exit(1); }
		f = fopen(raw, "rb");
		if (!f) { fprintf(stderr, "open %s\n", raw); exit(1); }
		if (fread(buf, 1, n, f) != n) {
			fprintf(stderr, "short read %s\n", raw); exit(1);
		}
		fclose(f);
		*rgba_out = buf;
		*w_out = w;
		*h_out = h;
		return;
	}

	{
		png_structp rpng;
		png_infop rinfo;
		FILE *input;
		unsigned char **rows;
		int w, h, i;
		size_t row_stride;
		unsigned char *buf;

		rpng = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL,
			NULL);
		if (setjmp(png_jmpbuf(rpng))) {
			fprintf(stderr, "PNG error\n"); exit(1);
		}
		input = fopen(png_path, "rb");
		if (!input) { fprintf(stderr, "open %s\n", png_path); exit(1); }
		rinfo = png_create_info_struct(rpng);
		png_init_io(rpng, input);
		png_read_info(rpng, rinfo);
		if (png_get_color_type(rpng, rinfo) != PNG_COLOR_TYPE_RGBA) {
			fprintf(stderr, "Input must be RGBA.\n"); exit(1);
		}
		w = png_get_image_width(rpng, rinfo);
		h = png_get_image_height(rpng, rinfo);
		row_stride = (size_t)w * 4;
		buf = malloc((size_t)h * row_stride);
		rows = malloc((size_t)h * sizeof(*rows));
		if (!buf || !rows) { fprintf(stderr, "alloc\n"); exit(1); }
		for (i = 0; i < h; i++) rows[i] = buf + (size_t)i * row_stride;
		png_read_image(rpng, rows);
		png_destroy_read_struct(&rpng, &rinfo, NULL);
		free(rows);
		fclose(input);
		*rgba_out = buf;
		*w_out = w;
		*h_out = h;
	}
}

static struct bench_image load_png(const char *path, int cmp, int opts, int gray)
{
	struct bench_image bi;
	unsigned char *rgba;
	int w, h;
	size_t npix, i;

	harness_read_rgba(path, &rgba, &w, &h);
	bi.width = w;
	bi.height = h;
	bi.cmp = cmp;
	npix = (size_t)w * (size_t)h;

	if (gray && cmp == 1) {
		bi.buffer = malloc(npix);
		if (!bi.buffer) { fprintf(stderr, "alloc G\n"); exit(1); }
		for (i = 0; i < npix; i++) {
			bi.buffer[i] = harness_rgb_to_gray(
				rgba[4*i+0], rgba[4*i+1], rgba[4*i+2]);
		}
		free(rgba);
	} else if (gray && cmp == 2) {
		bi.buffer = malloc(npix * 2);
		if (!bi.buffer) { fprintf(stderr, "alloc GA\n"); exit(1); }
		for (i = 0; i < npix; i++) {
			bi.buffer[2*i+0] = harness_rgb_to_gray(
				rgba[4*i+0], rgba[4*i+1], rgba[4*i+2]);
			bi.buffer[2*i+1] = rgba[4*i+3];
		}
		free(rgba);
	} else if (cmp == 3) {
		bi.buffer = malloc(npix * 3);
		if (!bi.buffer) { fprintf(stderr, "alloc RGB\n"); exit(1); }
		for (i = 0; i < npix; i++) {
			bi.buffer[3*i+0] = rgba[4*i+0];
			bi.buffer[3*i+1] = rgba[4*i+1];
			bi.buffer[3*i+2] = rgba[4*i+2];
		}
		free(rgba);
	} else if (cmp == 4 && opts) {
		/* RGBX: source alpha replaced with 0xff filler. Done in-place
		 * over the already-allocated RGBA buffer. */
		bi.buffer = rgba;
		for (i = 0; i < npix; i++) bi.buffer[4*i+3] = 0xff;
	} else {
		/* RGBA / ARGB / CMYK / RGBA_NOGAMMA: pass RGBA bytes through.
		 * liboil interprets them per-cs; byte order isn't rearranged. */
		bi.buffer = rgba;
	}
	return bi;
}

static int g_iters;

#endif
