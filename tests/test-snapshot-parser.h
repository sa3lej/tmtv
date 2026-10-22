/* Every split point passes through real host/relay parser state and screen APIs. */
static void
parser_sequence(const char *bytes)
{
	struct screen source;
	struct window_pane target;
	struct colour_palette source_palette, target_palette;
	struct input_ctx *si, *ti;
	struct input_snapshot *parser;
	struct tmtv_snapshot_pane *pane;
	msgpack_sbuffer screen_frame, parser_frame, left, right;
	msgpack_unpacked un;
	size_t split, size = strlen(bytes);
	for (split = 0; split <= size; split++) {
		memset(&target, 0, sizeof target); target.sx = 80; target.sy = 24;
		screen_init(&source, 80, 24, 2000); screen_init(&target.base, 80, 24, 2000);
		colour_palette_init(&source_palette); colour_palette_init(&target_palette);
		si = input_init(NULL, NULL, &source_palette, NULL); ti = input_init(NULL, NULL, &target_palette, NULL);
		input_parse_screen(si, &source, NULL, NULL, (const u_char *)"SEED", 4);
		input_parse_screen(si, &source, NULL, NULL, (const u_char *)bytes, split);
		msgpack_sbuffer_init(&screen_frame); msgpack_sbuffer_init(&parser_frame);
		ASSERT_EQ(tmtv_snapshot_pane_encode(0, &source, 100, &screen_frame), 0);
		ASSERT_EQ(input_snapshot_encode(si, &parser_frame), 0);
		msgpack_unpacked_init(&un);
		ASSERT_EQ(msgpack_unpack_next(&un, screen_frame.data, screen_frame.size, NULL), MSGPACK_UNPACK_SUCCESS);
		pane = tmtv_snapshot_pane_decode(un.data); ASSERT_NOT_NULL(pane);
		ASSERT_EQ(tmtv_snapshot_pane_apply(&target, pane), 0); tmtv_snapshot_pane_free(pane);
		ASSERT_EQ(msgpack_unpack_next(&un, parser_frame.data, parser_frame.size, NULL), MSGPACK_UNPACK_SUCCESS);
		parser = input_snapshot_decode(un.data); ASSERT_NOT_NULL(parser);
		input_snapshot_apply(ti, parser); input_snapshot_free(parser);
		input_parse_screen(si, &source, NULL, NULL, (const u_char *)bytes + split, size - split);
		input_parse_screen(ti, &target.base, NULL, NULL, (const u_char *)bytes + split, size - split);
		msgpack_sbuffer_init(&left); msgpack_sbuffer_init(&right);
		ASSERT_EQ(tmtv_snapshot_pane_encode(0, &source, 100, &left), 0);
		ASSERT_EQ(tmtv_snapshot_pane_encode(0, &target.base, 100, &right), 0);
		ASSERT_EQ(left.size, right.size); ASSERT(memcmp(left.data, right.data, left.size) == 0);
		ASSERT_STR_EQ(source.title, target.base.title);
		msgpack_sbuffer_destroy(&left); msgpack_sbuffer_destroy(&right);
		msgpack_sbuffer_destroy(&screen_frame); msgpack_sbuffer_destroy(&parser_frame);
		msgpack_unpacked_destroy(&un); input_free(si); input_free(ti);
		colour_palette_free(&source_palette); colour_palette_free(&target_palette);
		screen_free(&source); screen_free(&target.base);
	}
}
TEST(parser_snapshot_preserves_split_sgr_and_saved_cursor)
{
	parser_sequence("\033[38;2;18;52;86;9;53mABC\0337\033[32mNO\0338RESTORED\033[4:3mUNDERLINE\033]4;1;#112233\007\033[31mPALETTE\033]9;4;1;42\007");
}
TEST(parser_snapshot_preserves_partial_unicode_and_repeat)
{
	parser_sequence("e\xcc\x81\xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x92\xbb\033[2b");
}
TEST(parser_snapshot_preserves_character_sets)
{
	parser_sequence("\033(0lqk\033(BASCII\033)0\016x\017end");
}
TEST(parser_snapshot_preserves_osc_and_c0_without_replaying)
{
	parser_sequence("\033[1;\0102HXY\033]2;partial-title\007\033[22;0t\033]2;inner\007\033[23;0tAFTER");
}
TEST(parser_snapshot_preserves_partial_dcs)
{
	parser_sequence("\033P$qm\033\\AFTER");
}
TEST(parser_snapshot_does_not_read_inert_completed_large_osc)
{
	char seq[300];
	memset(seq, 'T', sizeof seq); memcpy(seq, "\033]2;", 4);
	seq[sizeof seq - 2] = '\007'; seq[sizeof seq - 1] = 0;
	/* Test specifically after the parser shrinks its completed OSC buffer. */
	struct screen s; struct input_ctx *i; msgpack_sbuffer sb;
	screen_init(&s, 80, 24, 0); i = input_init(NULL, NULL, NULL, NULL);
	input_parse_screen(i, &s, NULL, NULL, (u_char *)seq, strlen(seq));
	msgpack_sbuffer_init(&sb); ASSERT_EQ(input_snapshot_encode(i, &sb), 0);
	msgpack_sbuffer_destroy(&sb); input_free(i); screen_free(&s);
}

TEST(parser_snapshot_absent_palette_clears_stale_overrides)
{
	struct colour_palette palette;
	struct input_ctx *target;
	struct input_snapshot *plan;
	msgpack_sbuffer sb;
	msgpack_unpacked un;
	colour_palette_init(&palette); palette.fg = COLOUR_FLAG_RGB | 0x112233;
	palette.palette = xcalloc(256, sizeof *palette.palette);
	palette.default_palette = xcalloc(256, sizeof *palette.default_palette);
	target = input_init(NULL, NULL, &palette, NULL);
	msgpack_sbuffer_init(&sb); msgpack_unpacked_init(&un);
	ASSERT_EQ(input_snapshot_encode(NULL, &sb), 0);
	ASSERT_EQ(msgpack_unpack_next(&un, sb.data, sb.size, NULL), MSGPACK_UNPACK_SUCCESS);
	plan = input_snapshot_decode(un.data); ASSERT_NOT_NULL(plan);
	input_snapshot_apply(target, plan); input_snapshot_free(plan);
	ASSERT_EQ(palette.fg, 8); ASSERT_EQ(palette.bg, 8);
	ASSERT(palette.palette == NULL && palette.default_palette == NULL);
	input_free(target); colour_palette_free(&palette);
	msgpack_unpacked_destroy(&un); msgpack_sbuffer_destroy(&sb);
}

TEST(parser_snapshot_preserves_hyperlink_and_saved_link_at_every_split)
{
	parser_sequence("\033]8;id=named;https://example.invalid/a\007LINK\0337\033]8;;\007PLAIN\0338RESTORED\033]8;;\007\033]8;;https://example.invalid/anon\007ANON\033]8;;\007");
}

TEST(parser_snapshot_preserves_full_32_byte_cluster_and_continuation)
{
	struct screen screen;
	struct input_ctx *input;
	struct grid_cell gc;
	char sequence[37];
	u_int x;
	sequence[0] = 'a';
	for (x = 1; x < 29; x += 2) { sequence[x] = 0xcc; sequence[x+1] = 0x81; }
	memcpy(sequence + 29, "\xe2\x83\x9d", 3); /* U+20DD */
	memcpy(sequence + 32, "_END", 5);
	screen_init(&screen, 80, 24, 0); input = input_init(NULL, NULL, NULL, NULL);
	input_parse_screen(input, &screen, NULL, NULL, (u_char *)sequence, 36);
	grid_get_cell(screen.grid, 0, 0, &gc);
	ASSERT_EQ(gc.data.size, 32); ASSERT_EQ(gc.data.width, 1);
	ASSERT(memcmp(gc.data.data, sequence, 32) == 0);
	input_free(input); screen_free(&screen);
	parser_sequence(sequence);
}
