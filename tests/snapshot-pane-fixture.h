/* Production terminal parser creates a realistic active/saved screen pair. */
#include "../tmate-snapshot.h"
static void
pane_fixture_screen(struct screen *s)
{
	static const u_char input[] =
	    "\033[38;2;18;52;86m\033[48;5;234m\033[58:2::171:205:239m"
	    "\033[9;53;4:3mMAIN\033[?1049h\033[H\033[0mALT"
	    "\033[2;20r\033[?2004h\033[?1004h\033[3g"
	    "\033[5G\033H\033[13G\033H\033[4;11H";
	struct input_ctx *ictx;
	screen_init(s, 80, 24, 2000);
	ictx = input_init(NULL, NULL, NULL, NULL);
	input_parse_screen(ictx, s, NULL, NULL, input, sizeof input - 1);
	input_free(ictx);
	s->cstyle = SCREEN_CURSOR_BAR; s->default_cstyle = SCREEN_CURSOR_BLOCK;
	s->ccolour = COLOUR_FLAG_RGB|0x6789ab; s->default_ccolour = -1;
	s->default_mode = MODE_WRAP|MODE_KCURSOR;
	screen_set_title(s, "saved \342\230\205", 1); screen_push_title(s);
	screen_set_title(s, "active \\033]52; literal", 1);
	screen_set_path(s, "/home/lex/Byvattnet \\ backups", 1);
}

static int
pane_fixture_expected(struct screen *s)
{
	struct grid_cell gc;
	if (strcmp(s->title, "active \\\\033]52; literal") != 0 ||
	    s->path == NULL || strcmp(s->path, "/home/lex/Byvattnet \\\\ backups") != 0 ||
	    s->ntitles != 1 || strcmp(screen_title_at(s, 0), "saved \342\230\205") != 0) return (0);
	if (s->saved_grid == NULL || s->cx != 10 || s->cy != 3 ||
	    s->saved_cx != 4 || s->saved_cy != 0 ||
	    s->rupper != 1 || s->rlower != 19 ||
	    !(s->mode & MODE_BRACKETPASTE) || !(s->mode & MODE_FOCUSON) ||
	    (s->grid->flags & GRID_HISTORY) || !(s->saved_flags & GRID_HISTORY) ||
	    s->cstyle != SCREEN_CURSOR_BAR || s->default_cstyle != SCREEN_CURSOR_BLOCK ||
	    s->ccolour != (COLOUR_FLAG_RGB|0x6789ab) || s->default_ccolour != -1 ||
	    s->default_mode != (MODE_WRAP|MODE_KCURSOR)) return (0);
	grid_get_cell(s->grid, 0, 0, &gc);
	if (gc.data.data[0] != 'A') return (0);
	grid_get_cell(s->saved_grid, 0, 0, &gc);
	if (gc.data.data[0] != 'M' || gc.fg != (COLOUR_FLAG_RGB|0x123456)) return (0);
	if (!bit_test(s->tabs, 4) || !bit_test(s->tabs, 12) || bit_test(s->tabs, 8)) return (0);
	return (s->saved_cell.fg == (COLOUR_FLAG_RGB|0x123456) &&
	    s->saved_cell.us == (COLOUR_FLAG_RGB|0xabcdef) &&
	    s->saved_cell.attr == (GRID_ATTR_STRIKETHROUGH|GRID_ATTR_OVERLINE|GRID_ATTR_UNDERSCORE_3));
}

static int
pane_fixture_exit(struct screen *s)
{
	static const u_char exit[] = "\033[?1049lX";
	struct input_ctx *ictx = input_init(NULL, NULL, NULL, NULL);
	struct grid_cell gc;
	input_parse_screen(ictx, s, NULL, NULL, exit, sizeof exit - 1);
	input_free(ictx);
	grid_get_cell(s->grid, 4, s->grid->hsize, &gc);
	return (s->saved_grid == NULL && s->cx == 5 && s->cy == 0 &&
	    (s->grid->flags & GRID_HISTORY) && gc.data.data[0] == 'X' &&
	    gc.fg == (COLOUR_FLAG_RGB|0x123456) && gc.us == (COLOUR_FLAG_RGB|0xabcdef));
}
