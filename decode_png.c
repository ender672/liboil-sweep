/*
 * One-shot PNG -> raw RGBA decoder used to populate the sweep cache. Each
 * harness invocation would otherwise re-decode the same 8 MB PNG; the sweep
 * runs this once up front and points every harness at the raw buffer via
 * HARNESS_RAW + HARNESS_RAW_W/H.
 *
 * Usage: decode_png <in.png> <out.raw> <out.meta>
 * Writes raw file: width*height*4 bytes of RGBA8.
 * Writes meta file: single line "<width> <height>\n".
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <png.h>

int main(int argc, char *argv[])
{
	png_structp rpng;
	png_infop rinfo;
	FILE *in, *out;
	int w, h, i;
	size_t row_stride;
	unsigned char *buffer, **rows;

	if (argc != 4) {
		fprintf(stderr, "usage: %s <in.png> <out.raw> <out.meta>\n",
			argv[0]);
		return 2;
	}

	rpng = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
	if (setjmp(png_jmpbuf(rpng))) {
		fprintf(stderr, "PNG decode error\n");
		return 1;
	}
	rinfo = png_create_info_struct(rpng);

	in = fopen(argv[1], "rb");
	if (!in) { perror(argv[1]); return 1; }
	png_init_io(rpng, in);
	png_read_info(rpng, rinfo);

	if (png_get_color_type(rpng, rinfo) != PNG_COLOR_TYPE_RGBA) {
		fprintf(stderr, "input must be RGBA\n");
		return 1;
	}
	w = png_get_image_width(rpng, rinfo);
	h = png_get_image_height(rpng, rinfo);
	row_stride = (size_t)w * 4;

	buffer = malloc((size_t)h * row_stride);
	rows = malloc((size_t)h * sizeof(*rows));
	if (!buffer || !rows) { fprintf(stderr, "alloc\n"); return 1; }
	for (i = 0; i < h; i++) rows[i] = buffer + (size_t)i * row_stride;
	png_read_image(rpng, rows);
	png_destroy_read_struct(&rpng, &rinfo, NULL);
	fclose(in);

	out = fopen(argv[2], "wb");
	if (!out) { perror(argv[2]); return 1; }
	if (fwrite(buffer, 1, (size_t)h * row_stride, out) !=
			(size_t)h * row_stride) {
		fprintf(stderr, "short write %s\n", argv[2]);
		return 1;
	}
	fclose(out);

	out = fopen(argv[3], "w");
	if (!out) { perror(argv[3]); return 1; }
	fprintf(out, "%d %d\n", w, h);
	fclose(out);

	free(buffer);
	free(rows);
	return 0;
}
