/*
 * Benchmark harness for the "v2e" liboil API window (768312d, 2016-01-23..):
 *   Same function signatures as v2d, but yscaler is merged into resample.h
 *   (no yscaler.h file).
 *
 *   yscaler_init(ys, in_h, out_h, scanline_len)        -- 4 args
 *   yscaler_scale(ys, out, width, cmp, opts, pos)      -- 6 args
 *   xscale(in, in_w, out, out_w, cmp, opts)            -- 6 args
 *
 * Runs one (cs, ratio) cell per invocation and prints just the best-of-N
 * time in ms. The caller (run.sh) iterates the matrix and assembles the
 * CSV row.
 *
 * Usage: harness <png> <cs> <ratio>
 */
#include "harness_png.h"
#include "resample.h"

static int parse_cs(const char *name, int *cmp, int *opts, int *gray)
{
	*opts = 0;
	*gray = 0;
	if (!strcmp(name, "G"))    { *cmp = 1; *gray = 1;            return 1; }
	if (!strcmp(name, "GA"))   { *cmp = 2; *gray = 1;            return 1; }
	if (!strcmp(name, "RGB"))  { *cmp = 3;                       return 1; }
	if (!strcmp(name, "RGBX")) { *cmp = 4; *opts = 1;            return 1; }
	if (!strcmp(name, "RGBA")) { *cmp = 4;                       return 1; }
	return 0;
}

int main(int argc, char *argv[])
{
	const char *env;
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
	if (!parse_cs(argv[2], &cmp, &opts, &gray)) {
		fprintf(stderr, "unsupported cs: %s\n", argv[2]);
		return 2;
	}
	ratio = atof(argv[3]);

	env = getenv("OILITERATIONS");
	g_iters = env ? atoi(env) : 1;
	if (g_iters < 1) g_iters = 1;

	image = load_png(argv[1], cmp, opts, gray);
	in_row_stride = (size_t)image.width * image.cmp;
	{
		double _w = round(image.width * ratio);
		double _h = round(image.height * ratio);
		out_w = (uint32_t)(_w < 1 ? 1 : _w);
		out_h = (uint32_t)(_h < 1 ? 1 : _h);
	}
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
			(uint32_t)((size_t)out_w * image.cmp));

		for (i = 0; i < out_h; i++) {
			while ((tmp = yscaler_next(&ys))) {
				xscale(p, (long)image.width, tmp,
					(long)out_w, image.cmp, opts);
				p += in_row_stride;
			}
			yscaler_scale(&ys, outbuf, out_w,
				(uint8_t)image.cmp, (uint8_t)opts, i);
		}

		{
			clock_t t = clock() - t0;
			yscaler_free(&ys);
			if (!t_min || t < t_min) t_min = t;
		}
	}

	{
		double ms = (double)t_min * 1000.0 / CLOCKS_PER_SEC;
		printf("%.3f\n", ms);
		fflush(stdout);
	}
	free(outbuf);
	harness_rgba_free(image.buffer);
	return 0;
}
