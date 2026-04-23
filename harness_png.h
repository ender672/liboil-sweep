/*
 * Shared PNG loader for the v2-era benchmark harnesses. Included (not linked)
 * because the older libraries have pre-compiler-defines variations that are
 * simpler to specialize per-harness than to parametrize.
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
	int opts;
};

static struct bench_image load_png(const char *path, int cmp, int opts, int gray)
{
	struct bench_image bi;
	png_structp rpng;
	png_infop rinfo;
	FILE *input;
	size_t row_stride, buf_len;
	unsigned char **buf_ptrs;
	int i;

	rpng = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
	if (setjmp(png_jmpbuf(rpng))) { fprintf(stderr, "PNG error\n"); exit(1); }

	input = fopen(path, "rb");
	if (!input) { fprintf(stderr, "open %s\n", path); exit(1); }
	rinfo = png_create_info_struct(rpng);
	png_init_io(rpng, input);
	png_read_info(rpng, rinfo);

	if (png_get_color_type(rpng, rinfo) != PNG_COLOR_TYPE_RGBA) {
		fprintf(stderr, "Input must be RGBA.\n"); exit(1);
	}

	if (gray) png_set_rgb_to_gray(rpng, 1, -1, -1);
	if (cmp == 1 || cmp == 3) png_set_strip_alpha(rpng);
	if (cmp == 4 && opts) {
		png_set_strip_alpha(rpng);
		png_set_filler(rpng, 0xffff, PNG_FILLER_AFTER);
	}

	bi.width = png_get_image_width(rpng, rinfo);
	bi.height = png_get_image_height(rpng, rinfo);
	bi.cmp = cmp;
	bi.opts = opts;

	row_stride = (size_t)bi.width * cmp;
	buf_len = (size_t)bi.height * row_stride;
	bi.buffer = malloc(buf_len);
	buf_ptrs = malloc(bi.height * sizeof(unsigned char *));
	if (!bi.buffer || !buf_ptrs) { fprintf(stderr, "alloc\n"); exit(1); }
	for (i = 0; i < bi.height; i++) {
		buf_ptrs[i] = bi.buffer + i * row_stride;
	}
	png_read_image(rpng, buf_ptrs);
	png_destroy_read_struct(&rpng, &rinfo, NULL);
	free(buf_ptrs);
	fclose(input);
	return bi;
}

static int g_iters;

static void compute_out_dims(int in_w, int in_h, double ratio,
	uint32_t *out_w, uint32_t *out_h)
{
	double w = round(in_w * ratio);
	double h = round(in_h * ratio);
	if (w < 1) w = 1;
	if (h < 1) h = 1;
	*out_w = (uint32_t)w;
	*out_h = (uint32_t)h;
}

#endif
