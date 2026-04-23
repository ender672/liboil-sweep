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
 *
 * Uses the shared harness_png.h loader; struct bench_image's `opts` field is
 * this era's `filler` flag.
 */
#include "harness_png.h"
#include "resample.h"

static void run_one(struct bench_image image, int filler, double ratio,
	const char *cs_name)
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
			filler);
		yscaler_init(&ys, (uint32_t)image.height, out_h,
			(size_t)out_w * image.cmp);

		for (i = 0; i < out_h; i++) {
			while ((tmp = yscaler_next(&ys))) {
				uint8_t *psl = xscaler_psl_pos0(&xs);
				memcpy(psl, p, in_row_stride);
				p += in_row_stride;
				xscaler_scale(&xs, tmp);
			}
			yscaler_scale(&ys, outbuf, i, image.cmp, filler);
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

static void bench_cs(const char *path, int cmp, int filler, int gray,
	const char *cs_name)
{
	struct bench_image image = load_png(path, cmp, filler, gray);
	double ratios[] = { 0.01, 0.125, 0.8, 2.14 };
	size_t ri;
	for (ri = 0; ri < sizeof(ratios)/sizeof(ratios[0]); ri++) {
		run_one(image, filler, ratios[ri], cs_name);
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
