/*
 * Benchmark harness for the "late v3" liboil API
 * (a43868e 2018-03-03 .. fb6d969^ 2019-01-30)
 *
 *   enum oil_colorspace + preprocess_xscaler + yscaler_scale(ys, out, pos)
 *   + fix_ratio + CS_TO_CMP
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
	enum oil_colorspace cs;
};

static struct bench_image load_png(const char *path, enum oil_colorspace cs)
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
		fprintf(stderr, "Input must be RGBA.\n");
		exit(1);
	}

	switch (cs) {
	case OIL_CS_G:
		png_set_rgb_to_gray(rpng, 1, -1, -1);
		png_set_strip_alpha(rpng);
		break;
	case OIL_CS_GA:
		png_set_rgb_to_gray(rpng, 1, -1, -1);
		break;
	case OIL_CS_RGB:
		png_set_strip_alpha(rpng);
		break;
	case OIL_CS_RGBX:
		png_set_strip_alpha(rpng);
		png_set_filler(rpng, 0xffff, PNG_FILLER_AFTER);
		break;
	default:
		/* RGBA / CMYK: keep RGBA bytes as-is */
		break;
	}

	bi.width = png_get_image_width(rpng, rinfo);
	bi.height = png_get_image_height(rpng, rinfo);
	bi.cs = cs;

	row_stride = (size_t)bi.width * (size_t)CS_TO_CMP(cs);
	buf_len = (size_t)bi.height * row_stride;
	bi.buffer = malloc(buf_len);
	buf_ptrs = malloc(bi.height * sizeof(unsigned char *));
	if (!bi.buffer || !buf_ptrs) {
		fprintf(stderr, "Unable to allocate buffers.\n");
		exit(1);
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
	size_t in_row_stride = (size_t)image.width * (size_t)CS_TO_CMP(image.cs);

	out_w = (uint32_t)round(image.width * ratio);
	out_h = 500000;
	fix_ratio((uint32_t)image.width, (uint32_t)image.height, &out_w, &out_h);

	outbuf_size = (size_t)out_w * (size_t)CS_TO_CMP(image.cs);
	outbuf = malloc(outbuf_size);
	if (!outbuf) { fprintf(stderr, "outbuf alloc\n"); exit(1); }

	for (k = 0; k < g_iters; k++) {
		struct preprocess_xscaler pxs;
		struct yscaler ys;
		unsigned char *p = image.buffer;
		uint16_t *tmp;
		uint32_t i;
		clock_t t0 = clock();

		preprocess_xscaler_init(&pxs, (uint32_t)image.width,
			out_w, image.cs);
		yscaler_init(&ys, (uint32_t)image.height, out_h,
			pxs.xs.width_out, image.cs);

		for (i = 0; i < out_h; i++) {
			while ((tmp = yscaler_next(&ys))) {
				preprocess_xscaler_scale(&pxs, p, tmp);
				p += in_row_stride;
			}
			yscaler_scale(&ys, outbuf, i);
		}

		{
			clock_t t = clock() - t0;
			yscaler_free(&ys);
			preprocess_xscaler_free(&pxs);
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

static void bench_cs(const char *path, enum oil_colorspace cs,
	const char *cs_name)
{
	struct bench_image image = load_png(path, cs);
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

	bench_cs(argv[1], OIL_CS_G, "G");
	bench_cs(argv[1], OIL_CS_GA, "GA");
	bench_cs(argv[1], OIL_CS_RGB, "RGB");
	bench_cs(argv[1], OIL_CS_RGBX, "RGBX");
	bench_cs(argv[1], OIL_CS_RGBA, "RGBA");
	bench_cs(argv[1], OIL_CS_CMYK, "CMYK");
	return 0;
}
