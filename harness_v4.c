/*
 * Benchmark harness for the v4 liboil API (oil_resample.h / oil_scale_init /
 * enum oil_colorspace). SIMD backends are advertised by probe.h via HAS_SSE2
 * / HAS_AVX2 / HAS_NEON; colorspaces via HAS_CS_*.
 *
 * Uses the shared harness_png.h loader; cs is passed alongside the image
 * rather than being carried in the struct. cs→(cmp,opts,gray) mapping below
 * only references enum values whose HAS_CS_* macro is defined.
 */
#include "harness_png.h"
#include "oil_resample.h"
#include "probe.h"

static void cs_to_png_params(enum oil_colorspace cs, int *cmp, int *opts,
	int *gray)
{
	*cmp = OIL_CMP(cs);
	*opts = 0;
	*gray = 0;
	switch (cs) {
#ifdef HAS_CS_G
	case OIL_CS_G: *gray = 1; break;
#endif
#ifdef HAS_CS_GA
	case OIL_CS_GA: *gray = 1; break;
#endif
#ifdef HAS_CS_RGBX
	case OIL_CS_RGBX: *opts = 1; break;
#endif
#ifdef HAS_CS_RGBX_NOGAMMA
	case OIL_CS_RGBX_NOGAMMA: *opts = 1; break;
#endif
	default:
		/* RGB / RGBA / ARGB / CMYK / *_NOGAMMA RGB(A): nothing extra.
		 * strip_alpha for cmp==3 is handled inside load_png. */
		break;
	}
}

#define RUN(IN_FN, OUT_FN, BACKEND_LABEL) do {                             \
	clock_t t_min = 0;                                                 \
	int _k;                                                            \
	for (_k = 0; _k < g_iters; _k++) {                                 \
		struct oil_scale os;                                       \
		unsigned char *p = image.buffer;                           \
		clock_t _t0 = clock();                                     \
		oil_scale_init(&os, image.height, out_h,                   \
			image.width, out_w, cs);                           \
		{                                                          \
			int _i;                                            \
			for (_i = 0; _i < out_h; _i++) {                   \
				int _s;                                    \
				for (_s = oil_scale_slots(&os); _s > 0;    \
					_s--) {                            \
					IN_FN(&os, p);                     \
					p += in_row_stride;                \
				}                                          \
				OUT_FN(&os, outbuf);                       \
			}                                                  \
		}                                                          \
		{                                                          \
			clock_t _t = clock() - _t0;                        \
			oil_scale_free(&os);                               \
			if (!t_min || _t < t_min) t_min = _t;              \
		}                                                          \
	}                                                                  \
	{                                                                  \
		double ms = (double)t_min * 1000.0 / CLOCKS_PER_SEC;       \
		printf("%s,%s,%g,%.3f\n",                                  \
			cs_name, BACKEND_LABEL, ratio, ms);                \
		fflush(stdout);                                            \
	}                                                                  \
} while (0)

static void bench_cs(const char *path, enum oil_colorspace cs,
	const char *cs_name)
{
	int cmp, opts, gray;
	cs_to_png_params(cs, &cmp, &opts, &gray);
	struct bench_image image = load_png(path, cmp, opts, gray);
	double ratios[] = { 0.01, 0.125, 0.8, 2.14 };
	size_t ri;
	size_t in_row_stride = (size_t)image.width * (size_t)OIL_CMP(cs);
	unsigned char *outbuf = NULL;
	size_t outbuf_size = 0;

	for (ri = 0; ri < sizeof(ratios) / sizeof(ratios[0]); ri++) {
		double ratio = ratios[ri];
		int out_w = (int)round(image.width * ratio);
		int out_h = 500000;
		size_t need;

		oil_fix_ratio(image.width, image.height, &out_w, &out_h);

		need = (size_t)out_w * (size_t)OIL_CMP(cs);
		if (need > outbuf_size) {
			free(outbuf);
			outbuf = malloc(need);
			if (!outbuf) {
				fprintf(stderr, "outbuf alloc failed\n");
				exit(1);
			}
			outbuf_size = need;
		}

		RUN(oil_scale_in, oil_scale_out, "scalar");
#ifdef HAS_SSE2
		RUN(oil_scale_in_sse2, oil_scale_out_sse2, "sse2");
#endif
#ifdef HAS_AVX2
		RUN(oil_scale_in_avx2, oil_scale_out_avx2, "avx2");
#endif
#ifdef HAS_NEON
		RUN(oil_scale_in_neon, oil_scale_out_neon, "neon");
#endif
	}

	free(outbuf);
	free(image.buffer);
}

int main(int argc, char *argv[])
{
	const char *iterenv;

	if (argc != 2) {
		fprintf(stderr, "usage: %s <png>\n", argv[0]);
		return 1;
	}

	iterenv = getenv("OILITERATIONS");
	g_iters = iterenv ? atoi(iterenv) : 1;
	if (g_iters < 1) g_iters = 1;

	oil_global_init();

#ifdef HAS_CS_G
	bench_cs(argv[1], OIL_CS_G, "G");
#endif
#ifdef HAS_CS_GA
	bench_cs(argv[1], OIL_CS_GA, "GA");
#endif
#ifdef HAS_CS_RGB
	bench_cs(argv[1], OIL_CS_RGB, "RGB");
#endif
#ifdef HAS_CS_RGBX
	bench_cs(argv[1], OIL_CS_RGBX, "RGBX");
#endif
#ifdef HAS_CS_RGBA
	bench_cs(argv[1], OIL_CS_RGBA, "RGBA");
#endif
#ifdef HAS_CS_ARGB
	bench_cs(argv[1], OIL_CS_ARGB, "ARGB");
#endif
#ifdef HAS_CS_CMYK
	bench_cs(argv[1], OIL_CS_CMYK, "CMYK");
#endif
#ifdef HAS_CS_RGB_NOGAMMA
	bench_cs(argv[1], OIL_CS_RGB_NOGAMMA, "RGB_NOGAMMA");
#endif
#ifdef HAS_CS_RGBA_NOGAMMA
	bench_cs(argv[1], OIL_CS_RGBA_NOGAMMA, "RGBA_NOGAMMA");
#endif
#ifdef HAS_CS_RGBX_NOGAMMA
	bench_cs(argv[1], OIL_CS_RGBX_NOGAMMA, "RGBX_NOGAMMA");
#endif

	return 0;
}
