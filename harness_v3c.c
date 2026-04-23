/*
 * Benchmark harness for the "late v3" liboil API
 * (a43868e 2018-03-03 .. fb6d969^ 2019-01-30)
 *
 *   enum oil_colorspace + preprocess_xscaler + yscaler_scale(ys, out, pos)
 *   + fix_ratio + CS_TO_CMP
 *
 * Uses the shared harness_png.h loader; cs is passed alongside the image
 * rather than being carried in the struct.
 */
#include "harness_png.h"
#include "resample.h"

static void cs_to_png_params(enum oil_colorspace cs, int *cmp, int *opts,
	int *gray)
{
	*cmp = CS_TO_CMP(cs);
	*opts = 0;
	*gray = 0;
	switch (cs) {
	case OIL_CS_G:    *gray = 1; break;
	case OIL_CS_GA:   *gray = 1; break;
	case OIL_CS_RGBX: *opts = 1; break;
	default: break; /* RGB / RGBA / CMYK: nothing extra */
	}
}

static void run_one(struct bench_image image, enum oil_colorspace cs,
	double ratio, const char *cs_name)
{
	uint32_t out_w, out_h;
	clock_t t_min = 0;
	int k;
	unsigned char *outbuf;
	size_t outbuf_size;
	size_t in_row_stride = (size_t)image.width * (size_t)CS_TO_CMP(cs);

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
		printf("%s,%s,%g,%.3f\n", cs_name, "scalar", ratio, ms);
		fflush(stdout);
	}

	free(outbuf);
}

static void bench_cs(const char *path, enum oil_colorspace cs,
	const char *cs_name)
{
	int cmp, opts, gray;
	cs_to_png_params(cs, &cmp, &opts, &gray);
	struct bench_image image = load_png(path, cmp, opts, gray);
	double ratios[] = { 0.01, 0.125, 0.8, 2.14 };
	size_t ri;

	for (ri = 0; ri < sizeof(ratios)/sizeof(ratios[0]); ri++) {
		run_one(image, cs, ratios[ri], cs_name);
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
