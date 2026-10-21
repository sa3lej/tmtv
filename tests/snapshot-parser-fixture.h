/* Real terminal parser -> grid codec, shared by full host and relay tests. */
#include "../tmate-snapshot.h"
#ifndef TMATE_SERVER_BUILD
static int
snapshot_parser_frame(msgpack_sbuffer *sb)
{
	static const u_char input[] =
	    "\033[38;2;18;52;86m\033[48;5;234m\033[58:2::171:205:239m"
	    "\033[9;53;4:3me\xcc\x81\xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x92\xbbZ"
	    "\033[0m\r\nA\tB";
	struct screen s;
	struct input_ctx *ictx;
	int result;
	screen_init(&s, 40, 4, 100);
	ictx = input_init(NULL, NULL, NULL, NULL);
	input_parse_screen(ictx, &s, NULL, NULL, input, sizeof input - 1);
	result = tmtv_snapshot_grid_encode(s.grid, 100, sb);
	input_free(ictx);
	screen_free(&s);
	return (result);
}

#endif

static int
snapshot_parser_expected(struct grid *gd)
{
	struct grid_cell gc;
	if (gd == NULL || gd->sx != 40 || gd->sy != 4) return (0);
	grid_get_cell(gd, 0, 0, &gc);
	if (gc.fg != (COLOUR_FLAG_RGB|0x123456) ||
	    gc.bg != (COLOUR_FLAG_256|234) || gc.us != (COLOUR_FLAG_RGB|0xabcdef) ||
	    gc.attr != (GRID_ATTR_STRIKETHROUGH|GRID_ATTR_OVERLINE|GRID_ATTR_UNDERSCORE_3) ||
	    gc.data.size != 3 || gc.data.width != 1 ||
	    memcmp(gc.data.data, "e\xcc\x81", 3)) return (0);
	grid_get_cell(gd, 1, 0, &gc);
	if (gc.data.size != 11 || gc.data.width != 2 ||
	    memcmp(gc.data.data, "\xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x92\xbb", 11)) return (0);
	grid_get_cell(gd, 2, 0, &gc);
	if (!(gc.flags & GRID_FLAG_PADDING)) return (0);
	grid_get_cell(gd, 3, 0, &gc);
	if (gc.data.size != 1 || gc.data.data[0] != 'Z') return (0);
	grid_get_cell(gd, 1, 1, &gc);
	return ((gc.flags & GRID_FLAG_TAB) && gc.data.width == 7);
}
