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
 * Runs one (cs, ratio) cell per invocation and prints just the best-of-N
 * time in ms. The caller (run.sh) iterates the matrix and assembles the
 * CSV row.
 *
 * Usage: harness <png> <cs> <ratio>
 */
#include "harness_png.h"
#include "resample.h"

static int parse_cs(const char *name, int *cmp, int *filler, int *gray)
{
	*filler = 0;
	*gray = 0;
	if (!strcmp(name, "G"))    { *cmp = 1; *gray = 1;              return 1; }
	if (!strcmp(name, "GA"))   { *cmp = 2; *gray = 1;              return 1; }
	if (!strcmp(name, "RGB"))  { *cmp = 3;                         return 1; }
	if (!strcmp(name, "RGBX")) { *cmp = 4; *filler = 1;            return 1; }
	if (!strcmp(name, "RGBA")) { *cmp = 4;                         return 1; }
	return 0;
}

int main(int argc, char *argv[])
{
	const char *env;
	int cmp, filler, gray;
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
	if (!parse_cs(argv[2], &cmp, &filler, &gray)) {
		fprintf(stderr, "unsupported cs: %s\n", argv[2]);
		return 2;
	}
	ratio = atof(argv[3]);

	env = getenv("OILITERATIONS");
	g_iters = env ? atoi(env) : 1;
	if (g_iters < 1) g_iters = 1;

	image = load_png(argv[1], cmp, filler, gray);
	in_row_stride = (size_t)image.width * image.cmp;
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
		printf("%.3f\n", ms);
		fflush(stdout);
	}
	free(outbuf);
	harness_rgba_free(image.buffer);
	return 0;
}
