/*
 * Benchmark harness for the "v3a" liboil API window (2016-01 .. 2016-02):
 *   yscaler is merged into resample.h; `opts` is gone (no OIL_FILLER).
 *
 *   yscaler_init(ys, in_h, out_h, scanline_len)        -- 4 args, returns int
 *   yscaler_next(ys) -> uint8_t*
 *   yscaler_scale(ys, out, pos)                        -- 3 args, returns int
 *   yscaler_free(ys)
 *   xscale(in, in_w, out, out_w, cmp)                  -- 5 args, returns int
 *
 * RGBX is not meaningful (no OIL_FILLER), so we skip it here.
 */
#include "harness_png.h"
#include "resample.h"

static void run_one(struct bench_image image, double ratio, const char *cs_name)
{
	uint32_t out_w, out_h;
	clock_t t_min = 0;
	int k;
	unsigned char *outbuf;
	size_t outbuf_size;
	size_t in_row_stride = (size_t)image.width * image.cmp;

	compute_out_dims(image.width, image.height, ratio, &out_w, &out_h);

	outbuf_size = (size_t)out_w * image.cmp;
	outbuf = malloc(outbuf_size);
	if (!outbuf) { fprintf(stderr, "outbuf alloc\n"); exit(1); }

	for (k = 0; k < g_iters; k++) {
		struct yscaler ys;
		unsigned char *p = image.buffer;
		uint8_t *tmp;
		uint32_t i;
		clock_t t0 = clock();

		yscaler_init(&ys, (uint32_t)image.height, out_h,
			(size_t)out_w * image.cmp);

		for (i = 0; i < out_h; i++) {
			while ((tmp = yscaler_next(&ys))) {
				xscale(p, (uint32_t)image.width, tmp,
					out_w, (uint8_t)image.cmp);
				p += in_row_stride;
			}
			yscaler_scale(&ys, outbuf, i);
		}

		{
			clock_t t = clock() - t0;
			yscaler_free(&ys);
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

static void bench_cs(const char *path, int cmp, int gray,
	const char *cs_name)
{
	struct bench_image image = load_png(path, cmp, 0, gray);
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

	bench_cs(argv[1], 1, 1, "G");
	bench_cs(argv[1], 2, 1, "GA");
	bench_cs(argv[1], 3, 0, "RGB");
	bench_cs(argv[1], 4, 0, "RGBA");
	return 0;
}
