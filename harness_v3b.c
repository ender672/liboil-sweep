/*
 * Benchmark harness for the "middle v3" liboil API
 * (3e30916 2016-07-19 .. a43868e^ 2018-03-02)
 *
 *   xscaler_init(cmp, filler) + yscaler with uint8_t ring buffer
 *   yscaler_scale(ys, out, pos, cmp, filler) + fix_ratio
 *   No enum oil_colorspace — pass raw cmp/filler per call.
 *
 *   The "colorspace" labels (G, GA, RGB, RGBX, RGBA) map to (cmp, filler)
 *   pairs so rows stitch to later eras in the CSV, but note that scaling at
 *   this era has no gamma/premul-alpha semantics.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <setjmp.h>
#include <stdint.h>
#include <png.h>

#include "resample.h"

struct bench_image {
	unsigned char *buffer;
	int width;
	int height;
	uint8_t cmp;
	int filler;
};

static struct bench_image load_png(const char *path, uint8_t cmp, int filler,
	int gray)
{
	struct bench_image bi;
	png_structp rpng;
	png_infop rinfo;
	FILE *input;
	size_t row_stride, buf_len;
	unsigned char **buf_ptrs;
	int i;

	rpng = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
	if (setjmp(png_jmpbuf(rpng))) {
		fprintf(stderr, "PNG Decoding Error.\n");
		exit(1);
	}

	input = fopen(path, "rb");
	if (!input) { fprintf(stderr, "Unable to open %s\n", path); exit(1); }
	rinfo = png_create_info_struct(rpng);
	png_init_io(rpng, input);
	png_read_info(rpng, rinfo);

	if (png_get_color_type(rpng, rinfo) != PNG_COLOR_TYPE_RGBA) {
		fprintf(stderr, "Input must be RGBA.\n"); exit(1);
	}

	/* Transform based on target cmp (and optional gray conversion) */
	if (gray) png_set_rgb_to_gray(rpng, 1, -1, -1);
	if (cmp == 1 || cmp == 3) png_set_strip_alpha(rpng);
	if (cmp == 4 && filler) {
		png_set_strip_alpha(rpng);
		png_set_filler(rpng, 0xffff, PNG_FILLER_AFTER);
	}
	/* else cmp=2 (GA) / cmp=4 with filler=0 (RGBA): take whatever RGBA gives */

	bi.width = png_get_image_width(rpng, rinfo);
	bi.height = png_get_image_height(rpng, rinfo);
	bi.cmp = cmp;
	bi.filler = filler;

	row_stride = (size_t)bi.width * cmp;
	buf_len = (size_t)bi.height * row_stride;
	bi.buffer = malloc(buf_len);
	buf_ptrs = malloc(bi.height * sizeof(unsigned char *));
	if (!bi.buffer || !buf_ptrs) {
		fprintf(stderr, "Unable to allocate buffers.\n"); exit(1);
	}
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

static void run_one(struct bench_image image, double ratio, const char *cs_name)
{
	uint32_t out_w, out_h;
	clock_t t_min = 0;
	int k;
	unsigned char *outbuf;
	size_t outbuf_size;
	size_t in_row_stride = (size_t)image.width * image.cmp;

	out_w = (uint32_t)round(image.width * ratio);
	out_h = 500000;
	fix_ratio((uint32_t)image.width, (uint32_t)image.height, &out_w, &out_h);

	outbuf_size = (size_t)out_w * image.cmp;
	outbuf = malloc(outbuf_size);
	if (!outbuf) { fprintf(stderr, "outbuf alloc\n"); exit(1); }

	for (k = 0; k < g_iters; k++) {
		struct xscaler xs;
		struct yscaler ys;
		unsigned char *p = image.buffer;
		uint8_t *tmp;
		uint32_t i;
		clock_t t0 = clock();

		xscaler_init(&xs, (uint32_t)image.width, out_w, image.cmp,
			image.filler);
		yscaler_init(&ys, (uint32_t)image.height, out_h,
			(size_t)out_w * image.cmp);

		for (i = 0; i < out_h; i++) {
			while ((tmp = yscaler_next(&ys))) {
				uint8_t *psl = xscaler_psl_pos0(&xs);
				memcpy(psl, p, in_row_stride);
				p += in_row_stride;
				xscaler_scale(&xs, tmp);
			}
			yscaler_scale(&ys, outbuf, i, image.cmp, image.filler);
		}

		{
			clock_t t = clock() - t0;
			yscaler_free(&ys);
			xscaler_free(&xs);
			if (!t_min || t < t_min) t_min = t;
		}
	}

	{
		double ms = (double)t_min * 1000.0 / CLOCKS_PER_SEC;
		printf("%s,%s,%g,%.3f\n", cs_name, "scalar", ratio, ms);
		fflush(stdout);
	}
	free(outbuf);
}

static void bench_cs(const char *path, uint8_t cmp, int filler, int gray,
	const char *cs_name)
{
	struct bench_image image = load_png(path, cmp, filler, gray);
	double ratios[] = { 0.01, 0.125, 0.8, 2.14 };
	size_t ri;
	for (ri = 0; ri < sizeof(ratios)/sizeof(ratios[0]); ri++) {
		run_one(image, ratios[ri], cs_name);
	}
	free(image.buffer);
}

int main(int argc, char *argv[])
{
	const char *env;
	if (argc != 2) { fprintf(stderr, "usage: %s <png>\n", argv[0]); return 1; }
	env = getenv("OILITERATIONS");
	g_iters = env ? atoi(env) : 1;
	if (g_iters < 1) g_iters = 1;

	bench_cs(argv[1], 1, 0, 1, "G");
	bench_cs(argv[1], 2, 0, 1, "GA");
	bench_cs(argv[1], 3, 0, 0, "RGB");
	bench_cs(argv[1], 4, 1, 0, "RGBX"); /* filler=1: skip 4th byte */
	bench_cs(argv[1], 4, 0, 0, "RGBA");
	return 0;
}
