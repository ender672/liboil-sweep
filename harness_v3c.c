/*
 * Benchmark harness for the "late v3" liboil API
 * (a43868e 2018-03-03 .. fb6d969^ 2019-01-30)
 *
 *   enum oil_colorspace + preprocess_xscaler + yscaler_scale(ys, out, pos)
 *   + fix_ratio + CS_TO_CMP
 *
 * Runs one (cs, ratio) cell per invocation and prints just the best-of-N
 * time in ms. The caller (run.sh) iterates the matrix and assembles the
 * CSV row.
 *
 * Usage: harness <png> <cs> <ratio>
 */
#include "harness_png.h"
#include "resample.h"

static int parse_cs(const char *name, enum oil_colorspace *out, int *cmp,
	int *opts, int *gray)
{
	*opts = 0;
	*gray = 0;
	if (!strcmp(name, "G"))    { *out = OIL_CS_G;    *cmp = CS_TO_CMP(*out); *gray = 1; return 1; }
	if (!strcmp(name, "GA"))   { *out = OIL_CS_GA;   *cmp = CS_TO_CMP(*out); *gray = 1; return 1; }
	if (!strcmp(name, "RGB"))  { *out = OIL_CS_RGB;  *cmp = CS_TO_CMP(*out);            return 1; }
	if (!strcmp(name, "RGBX")) { *out = OIL_CS_RGBX; *cmp = CS_TO_CMP(*out); *opts = 1; return 1; }
	if (!strcmp(name, "RGBA")) { *out = OIL_CS_RGBA; *cmp = CS_TO_CMP(*out);            return 1; }
	if (!strcmp(name, "CMYK")) { *out = OIL_CS_CMYK; *cmp = CS_TO_CMP(*out);            return 1; }
	return 0;
}

int main(int argc, char *argv[])
{
	const char *env;
	enum oil_colorspace cs;
	int cmp, opts, gray;
	double ratio;
	struct bench_image image;
	uint32_t out_w, out_h;
	size_t in_row_stride, outbuf_size;
	unsigned char *outbuf;
	clock_t t_min = 0;
	int k;

	if (argc != 4) {
		fprintf(stderr, "usage: %s <png> <cs> <ratio>\n", argv[0]);
		return 1;
	}
	if (!parse_cs(argv[2], &cs, &cmp, &opts, &gray)) {
		fprintf(stderr, "unsupported cs: %s\n", argv[2]);
		return 2;
	}
	ratio = atof(argv[3]);

	env = getenv("OILITERATIONS");
	g_iters = env ? atoi(env) : 1;
	if (g_iters < 1) g_iters = 1;

	image = load_png(argv[1], cmp, opts, gray);
	in_row_stride = (size_t)image.width * (size_t)CS_TO_CMP(cs);
	out_w = (uint32_t)round(image.width * ratio);
	out_h = 500000;
	fix_ratio((uint32_t)image.width, (uint32_t)image.height, &out_w, &out_h);

	outbuf_size = (size_t)out_w * (size_t)CS_TO_CMP(cs);
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
			out_w, cs);
		yscaler_init(&ys, (uint32_t)image.height, out_h,
			pxs.xs.width_out, cs);

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
		printf("%.3f\n", ms);
		fflush(stdout);
	}
	free(outbuf);
	free(image.buffer);
	return 0;
}
