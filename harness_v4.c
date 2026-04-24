/*
 * Benchmark harness for the v4 liboil API (oil_resample.h / oil_scale_init /
 * enum oil_colorspace).
 *
 * Runs exactly one (cs, ratio, backend) cell per invocation and prints the
 * best-of-N time in ms. The caller (run.sh) iterates the matrix and assembles
 * the full CSV row. probe.h still gates which colorspaces and backends are
 * compiled in, based on what the rev's headers declare.
 *
 * Usage: harness <png> <cs_name> <ratio> <backend>
 * Output: a single line, just the ms as %.3f.
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

#ifdef HAS_SCALE_RETURNS_INT
typedef int (*scale_fn)(struct oil_scale *, unsigned char *);
#else
typedef void (*scale_fn)(struct oil_scale *, unsigned char *);
#endif

static int parse_cs(const char *name, enum oil_colorspace *out)
{
#ifdef HAS_CS_G
	if (!strcmp(name, "G"))           { *out = OIL_CS_G;           return 1; }
#endif
#ifdef HAS_CS_GA
	if (!strcmp(name, "GA"))          { *out = OIL_CS_GA;          return 1; }
#endif
#ifdef HAS_CS_RGB
	if (!strcmp(name, "RGB"))         { *out = OIL_CS_RGB;         return 1; }
#endif
#ifdef HAS_CS_RGBX
	if (!strcmp(name, "RGBX"))        { *out = OIL_CS_RGBX;        return 1; }
#endif
#ifdef HAS_CS_RGBA
	if (!strcmp(name, "RGBA"))        { *out = OIL_CS_RGBA;        return 1; }
#endif
#ifdef HAS_CS_ARGB
	if (!strcmp(name, "ARGB"))        { *out = OIL_CS_ARGB;        return 1; }
#endif
#ifdef HAS_CS_CMYK
	if (!strcmp(name, "CMYK"))        { *out = OIL_CS_CMYK;        return 1; }
#endif
#ifdef HAS_CS_RGB_NOGAMMA
	if (!strcmp(name, "RGB_NOGAMMA"))  { *out = OIL_CS_RGB_NOGAMMA;  return 1; }
#endif
#ifdef HAS_CS_RGBA_NOGAMMA
	if (!strcmp(name, "RGBA_NOGAMMA")) { *out = OIL_CS_RGBA_NOGAMMA; return 1; }
#endif
#ifdef HAS_CS_RGBX_NOGAMMA
	if (!strcmp(name, "RGBX_NOGAMMA")) { *out = OIL_CS_RGBX_NOGAMMA; return 1; }
#endif
	return 0;
}

static int parse_backend(const char *name, scale_fn *in_fn, scale_fn *out_fn)
{
	if (!strcmp(name, "scalar")) {
		*in_fn = oil_scale_in;
		*out_fn = oil_scale_out;
		return 1;
	}
#ifdef HAS_SSE2
	if (!strcmp(name, "sse2")) {
		*in_fn = oil_scale_in_sse2;
		*out_fn = oil_scale_out_sse2;
		return 1;
	}
#endif
#ifdef HAS_AVX2
	if (!strcmp(name, "avx2")) {
		*in_fn = oil_scale_in_avx2;
		*out_fn = oil_scale_out_avx2;
		return 1;
	}
#endif
#ifdef HAS_NEON
	if (!strcmp(name, "neon")) {
		*in_fn = oil_scale_in_neon;
		*out_fn = oil_scale_out_neon;
		return 1;
	}
#endif
	return 0;
}

int main(int argc, char *argv[])
{
	const char *iterenv;
	enum oil_colorspace cs;
	scale_fn in_fn, out_fn;
	int cmp, opts, gray;
	struct bench_image image;
	double ratio;
	int out_w, out_h;
	size_t in_row_stride, need;
	unsigned char *outbuf;
	clock_t t_min = 0;
	int k;

	if (argc != 5) {
		fprintf(stderr,
			"usage: %s <png> <cs> <ratio> <backend>\n", argv[0]);
		return 1;
	}

	if (!parse_cs(argv[2], &cs)) {
		fprintf(stderr, "unsupported cs: %s\n", argv[2]);
		return 2;
	}
	ratio = atof(argv[3]);
	if (!parse_backend(argv[4], &in_fn, &out_fn)) {
		fprintf(stderr, "unsupported backend: %s\n", argv[4]);
		return 2;
	}

	iterenv = getenv("OILITERATIONS");
	g_iters = iterenv ? atoi(iterenv) : 1;
	if (g_iters < 1) g_iters = 1;

	oil_global_init();

	cs_to_png_params(cs, &cmp, &opts, &gray);
	image = load_png(argv[1], cmp, opts, gray);

	in_row_stride = (size_t)image.width * (size_t)OIL_CMP(cs);
	out_w = (int)round(image.width * ratio);
	out_h = 500000;
	oil_fix_ratio(image.width, image.height, &out_w, &out_h);
	need = (size_t)out_w * (size_t)OIL_CMP(cs);
	outbuf = malloc(need);
	if (!outbuf) { fprintf(stderr, "outbuf alloc failed\n"); exit(1); }

	for (k = 0; k < g_iters; k++) {
		struct oil_scale os;
		unsigned char *p = image.buffer;
		clock_t t0 = clock();
		clock_t t;
		int i;
		oil_scale_init(&os, image.height, out_h,
			image.width, out_w, cs);
		for (i = 0; i < out_h; i++) {
			int s;
			for (s = oil_scale_slots(&os); s > 0; s--) {
				in_fn(&os, p);
				p += in_row_stride;
			}
			out_fn(&os, outbuf);
		}
		t = clock() - t0;
		oil_scale_free(&os);
		if (!t_min || t < t_min) t_min = t;
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
