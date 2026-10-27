/* Feed complete production frames, rejecting any bad pane before installation. */
static void
wire_case(int variant, int fragmented)
{
	struct session *s = session_create(NULL, "wire", "/", environ_create(),
	    options_create(global_s_options), NULL);
	struct window_pane *a = create_pane(), *b = create_pane();
	struct grid *ga = a->base.grid, *gb = b->base.grid;
	struct screen source;
	struct input_ctx *input;
	struct tmate_decoder decoder;
	msgpack_sbuffer sb, changed;
	msgpack_packer pk;
	msgpack_unpacked un;
	msgpack_object *v, *first, *second, *parser, *meta, clock[3] = {0}, modes[2] = {0};
	struct grid_cell cell;
	u_int i;
	session_set_current(s, session_attach(s, a->window, 0, NULL));
	session_attach(s, b->window, 1, NULL);
	screen_init(&source, 80, 24, 2000);
	if (variant >= 34) {
		ASSERT(screen_set_title(&a->base, "keep-a", 1));
		ASSERT(screen_set_title(&b->base, "keep-b", 1));
		ASSERT(screen_set_path(&b->base, "/keep", 1)); screen_push_title(&b->base);
	}
	if (variant >= 26 && variant <= 33) {
		const char *link = "\033]8;id=wire;https://example.invalid/wire\007LINK\033]8;;\007";
		struct input_ctx *mi = input_init(NULL, NULL, NULL, NULL);
		input_parse_screen(mi, &source, NULL, NULL, (u_char *)link, strlen(link)); input_free(mi);
#ifdef ENABLE_SIXEL
		struct sixel_image *si = sixel_parse("q#0;2;100;0;0~", strlen("q#0;2;100;0;0~"), 0, 8, 16);
		ASSERT_NOT_NULL(si); image_store(&source, si);
#endif
	}
	if (variant == 25) {
		cell = grid_default_cell; utf8_set(&cell.data, 'H');
		for (i = 0; i < 1700; i++) {
			grid_set_cell(source.grid, 0, source.grid->hsize, &cell);
			grid_scroll_history(source.grid, 8);
		}
	}
	input = input_init(NULL, NULL, NULL, NULL);
	input_parse_screen(input, &source, NULL, NULL, (u_char *)"NEW\033[38;2;17;34;", strlen("NEW\033[38;2;17;34;"));
	if (variant >= 34) {
		ASSERT(screen_set_title(&source, "saved \342\230\205", 1)); screen_push_title(&source);
		ASSERT(screen_set_title(&source, "title \\033]52; literal", 1));
		ASSERT(screen_set_path(&source, "/home/lex/Byvattnet \\ backups", 1));
	}
	msgpack_sbuffer_init(&sb); msgpack_packer_init(&pk, &sb, msgpack_sbuffer_write);
	msgpack_pack_array(&pk, 3); msgpack_pack_int(&pk, TMTV_SNAPSHOT_OPCODE);
	msgpack_pack_int(&pk, TMTV_SNAPSHOT_VERSION); msgpack_pack_array(&pk, 2);
	for (i = 0; i < 2; i++) {
		msgpack_pack_array(&pk, 3);
		ASSERT_EQ(tmtv_snapshot_pane_encode(i ? b->id : a->id, &source, variant == 25 ? 1700 : 100, &sb), 0);
		ASSERT_EQ(input_snapshot_encode(input, &sb), 0); msgpack_pack_array(&pk, 0);
	}
	msgpack_unpacked_init(&un);
	ASSERT_EQ(msgpack_unpack_next(&un, sb.data, sb.size, NULL), MSGPACK_UNPACK_SUCCESS);
	v = un.data.via.array.ptr;
	first = v[2].via.array.ptr[0].via.array.ptr;
	second = v[2].via.array.ptr[1].via.array.ptr;
	parser = second[1].via.array.ptr; meta = second[0].via.array.ptr[12].via.array.ptr;
	switch (variant) {
	case -1: case 25: case 26: break;
	case 34: meta[0].via.bin.ptr = "\033]52;c;SGVsbG8=\007";
		meta[0].via.bin.size = strlen(meta[0].via.bin.ptr); break;
	case 35: meta[1].via.bin.ptr = "/p\033[31m";
		meta[1].via.bin.size = strlen(meta[1].via.bin.ptr); break;
	case 36: meta[3].via.array.ptr[0].via.bin.ptr = "saved\007";
		meta[3].via.array.ptr[0].via.bin.size = 6; break;
	case 37: meta[0].via.bin.ptr = "\377"; meta[0].via.bin.size = 1; break;
	case 38: meta[1].via.bin.ptr = "#(never-run)";
		meta[1].via.bin.size = strlen(meta[1].via.bin.ptr); break;
	case 39: meta[3].via.array.ptr[0].via.bin.ptr = "\342\202";
		meta[3].via.array.ptr[0].via.bin.size = 2; break;
	case 40: break;
	case 0: v[1].via.u64++; break;
	case 1: v[2].via.array.size--; break;
	case 2: second[0].via.array.ptr[1] = first[0].via.array.ptr[1]; break;
	case 3: second[0].via.array.ptr[1].via.u64 = INT_MAX; break;
	case 4: second[0].via.array.ptr[2].via.array.ptr[1].via.u64 = 79; break;
	case 5: second[0].via.array.ptr[0].via.u64++; break;
	case 6: meta[0].via.bin.size = 16385; break;
	case 7: meta[0].type = MSGPACK_OBJECT_NIL; break;
	case 8: meta[1].type = MSGPACK_OBJECT_ARRAY; meta[1].via.array.size = 0; break;
	case 9: meta[2].via.array.ptr[0].via.u64 = 5; break;
	case 10: meta[2].via.array.ptr[1].type = MSGPACK_OBJECT_NEGATIVE_INTEGER;
		meta[2].via.array.ptr[1].via.i64 = -2; break;
	case 11: parser[0].via.u64++; break;
	case 12: parser[4].via.u64 = 17; break;
	case 13: parser[10].type = MSGPACK_OBJECT_BIN;
		parser[10].via.bin.ptr = "x"; parser[10].via.bin.size = 1; break;
	case 14: parser[13].via.u64 = 4; break;
	case 15: parser[3].via.array.ptr[0].via.u64 = WINDOW_MAXIMUM + 1; break;
	case 16: parser[5].via.bin.ptr = "x"; parser[5].via.bin.size = 1; break;
	case 17: parser[1].via.array.ptr[1].via.u64 = 2; break;
	case 18: second[1].type = MSGPACK_OBJECT_NIL; break;
	case 19: second[2].type = MSGPACK_OBJECT_NIL; break;
	case 20: second[0].via.array.ptr[2].via.array.ptr[6].via.array.size--; break;
	case 27: second[0].via.array.ptr[2].via.array.ptr[6].via.array.ptr[0].via.array.ptr[3].via.array.ptr[0].via.array.ptr[7].via.u64 = INT_MAX; break;
	case 28: parser[1].via.array.ptr[0].via.array.ptr[7].via.u64 = INT_MAX; break;
	case 29: second[0].via.array.ptr[13].via.array.ptr[0].via.array.ptr[1].via.bin.ptr = "\033";
		second[0].via.array.ptr[13].via.array.ptr[0].via.array.ptr[1].via.bin.size = 1; break;
#ifdef ENABLE_SIXEL
	case 30: second[0].via.array.ptr[14].via.array.ptr[0].via.array.ptr[2].via.array.ptr[1].via.u64 = 0; break;
	case 31: second[0].via.array.ptr[14].via.array.ptr[0].via.array.ptr[2].via.array.ptr[11].via.array.ptr[0].via.bin.ptr = "\377\377"; break;
	case 32: second[0].via.array.ptr[14].via.array.ptr[0].via.array.ptr[0].via.u64 = WINDOW_MAXIMUM + 1; break;
	case 33: second[0].via.array.ptr[14].via.array.ptr[0].via.array.ptr[2].via.array.ptr[3].via.u64 = 0; break;
#endif
	case 21: second[0].via.array.ptr[3].via.array.ptr[0].via.u64 = 81; break;
	default:
		clock[0].type = MSGPACK_OBJECT_POSITIVE_INTEGER; clock[0].via.u64 = variant == 22 ? 4 : 3;
		clock[1].type = clock[2].type = MSGPACK_OBJECT_NIL;
		for (i = 0; i < 2; i++) { modes[i].type = MSGPACK_OBJECT_ARRAY;
			modes[i].via.array.size = 3; modes[i].via.array.ptr = clock; }
		second[2].type = MSGPACK_OBJECT_ARRAY; second[2].via.array.ptr = modes;
		second[2].via.array.size = variant == 23 ? 2 : 1;
		if (variant == 24) clock[2].type = MSGPACK_OBJECT_POSITIVE_INTEGER;
		break;
	}
	/* Avoid reading past a synthetic overlong BIN while repacking it. */
	char long_title[16385];
	if (variant == 6) { memset(long_title, 'T', sizeof long_title); meta[0].via.bin.ptr = long_title; }
	msgpack_sbuffer_init(&changed); msgpack_packer_init(&pk, &changed, msgpack_sbuffer_write);
	ASSERT_EQ(msgpack_pack_object(&pk, un.data), 0);
	tmate_decoder_init(&decoder, read_message, NULL); decode_buffer(&decoder, &changed, fragmented);
	if (variant == -1 || variant == 26 || variant == 40) {
		ASSERT(a->base.grid != ga && b->base.grid != gb);
		if (variant == 40) {
			ASSERT_STR_EQ(b->base.title, source.title);
			ASSERT_STR_EQ(b->base.path, source.path);
			ASSERT_EQ(b->base.ntitles, 1);
			ASSERT_STR_EQ(screen_title_at(&b->base, 0), screen_title_at(&source, 0));
		}
		grid_get_cell(b->base.grid, 0, 0, &cell); ASSERT_EQ(cell.data.data[0], variant == 26 ? 'L' : 'N');
		if (variant == 26) {
			const char *uri;
			grid_get_cell(b->base.grid, 0, 0, &cell); ASSERT(cell.link != 0);
			ASSERT(hyperlinks_get(b->base.hyperlinks, cell.link, &uri, NULL, NULL));
			ASSERT_STR_EQ(uri, "https://example.invalid/wire");
#ifdef ENABLE_SIXEL
			ASSERT(!TAILQ_EMPTY(&a->base.images) && !TAILQ_EMPTY(&b->base.images));
#endif
		}
		/* The restored parser is immediately ready for following PTY bytes. */
		input_parse_screen(b->ictx, &b->base, NULL, NULL, (u_char *)"51mC", 4);
		grid_get_cell(b->base.grid, variant == 26 ? 7 : 3, 0, &cell); ASSERT_EQ(cell.data.data[0], 'C');
		ASSERT_EQ(cell.fg, COLOUR_FLAG_RGB | 0x112233);
		ASSERT_EQ(input_snapshot_encode(b->ictx, &changed), 0);
	} else {
		ASSERT(a->base.grid == ga && b->base.grid == gb);
		if (variant >= 34) {
			ASSERT_STR_EQ(a->base.title, "keep-a"); ASSERT_STR_EQ(b->base.title, "keep-b");
			ASSERT_STR_EQ(b->base.path, "/keep");
			ASSERT_EQ(b->base.ntitles, 1); ASSERT_STR_EQ(screen_title_at(&b->base, 0), "keep-b");
		}
	}
	/* A rejected complete object does not consume the following message. */
	msgpack_sbuffer_clear(&changed); pack_output(&changed, a->id, "AFTER_BAD_FRAME");
	decode_buffer(&decoder, &changed, fragmented); ASSERT(line_equals(a, 0, "AFTER_BAD_FRAME"));
	msgpack_unpacker_destroy(&decoder.unpacker); msgpack_unpacked_destroy(&un);
	msgpack_sbuffer_destroy(&changed); msgpack_sbuffer_destroy(&sb);
	input_free(input); screen_free(&source);
	session_destroy(s, 0, __func__); destroy_pane(b); destroy_pane(a);
	event_loop(EVLOOP_NONBLOCK);
}
TEST(wire_snapshot_installs_all_panes_and_partial_parser)
{
	wire_case(-1, 0); wire_case(-1, 1);
}
TEST(wire_snapshot_malformed_fields_are_atomic_and_keep_boundaries)
{
	int i;
	for (i = 0; i <= 25; i++) { wire_case(i, 0); wire_case(i, 1); }
}

TEST(wire_snapshot_metadata_is_safe_and_atomic_in_bulk_and_bytewise_transport)
{
	int i;
	for (i = 34; i <= 40; i++) { wire_case(i, 0); wire_case(i, 1); }
}

static void
wire_copy_boundary_case(int view, int invalid, int fragmented)
{
	struct session *s = session_create(NULL, "copy-boundary", "/",
	    environ_create(), options_create(global_s_options), NULL);
	struct window_pane *a = create_pane(), *b = create_pane();
	struct grid *ga = a->base.grid, *gb = b->base.grid;
	struct window_mode_entry *clock;
	struct window_copy_state state;
	struct input_ctx *input = input_init(NULL, NULL, NULL, NULL);
	struct tmate_decoder decoder;
	struct screen source;
	msgpack_sbuffer sb;
	msgpack_packer pk;
	u_int i;

	session_set_current(s, session_attach(s, a->window, 0, NULL));
	session_attach(s, b->window, 1, NULL);
	window_pane_set_mode(b, NULL, &window_clock_mode, NULL, NULL, NULL);
	clock = TAILQ_FIRST(&b->modes);
	screen_init(&source, 80, 24, 0);
	msgpack_sbuffer_init(&sb);
	msgpack_packer_init(&pk, &sb, msgpack_sbuffer_write);
	msgpack_pack_array(&pk, 3);
	msgpack_pack_int(&pk, TMTV_SNAPSHOT_OPCODE);
	msgpack_pack_int(&pk, TMTV_SNAPSHOT_VERSION);
	msgpack_pack_array(&pk, 2);
	for (i = 0; i < 2; i++) {
		msgpack_pack_array(&pk, 3);
		ASSERT_EQ(tmtv_snapshot_pane_encode(i ? b->id : a->id,
		    &source, 0, &sb), 0);
		ASSERT_EQ(input_snapshot_encode(input, &sb), 0);
		msgpack_pack_array(&pk, i ? 1 : 0);
		if (!i)
			continue;
		msgpack_pack_array(&pk, 3);
		msgpack_pack_int(&pk, view ? 2 : 1);
		ASSERT_EQ(tmtv_snapshot_pane_encode(b->id, &source, 0, &sb), 0);
		msgpack_pack_array(&pk, 7);
		msgpack_pack_int(&pk, invalid == 4 ? 1 : 0); /* scroll */
		msgpack_pack_int(&pk, invalid == 1 ? 81 : 80); /* cursor x */
		msgpack_pack_int(&pk, invalid == 3 ? 24 : 6); /* cursor y */
		msgpack_pack_int(&pk, 1); /* selected */
		msgpack_pack_int(&pk, invalid == 2 ? 81 : 80); /* anchor x */
		msgpack_pack_int(&pk, invalid == 5 ? 24 : 0); /* anchor y */
		msgpack_pack_int(&pk, 0); /* rectangle */
	}
	tmate_decoder_init(&decoder, read_message, NULL);
	decode_buffer(&decoder, &sb, fragmented);
	if (invalid) {
		ASSERT(a->base.grid == ga && b->base.grid == gb);
		ASSERT(TAILQ_FIRST(&b->modes) == clock);
	} else {
		ASSERT(a->base.grid != ga && b->base.grid != gb);
		ASSERT(window_copy_get_state(b, &state));
		ASSERT_EQ(state.view_mode, view);
		ASSERT_EQ(state.cx, 80);
		ASSERT_EQ(state.selx, 80);
		ASSERT(window_copy_state_is_valid(b, &state));
		/* Ordinary updates after reconnect must preserve the same edge. */
		msgpack_sbuffer_clear(&sb);
		pack_copy_state(&sb, view, b->id, 0, 79, 6, 1, 80, 0, 0);
		decode_buffer(&decoder, &sb, fragmented);
		ASSERT(window_copy_get_state(b, &state));
		ASSERT_EQ(state.cx, 79);
		ASSERT_EQ(state.selx, 80);
		msgpack_sbuffer_clear(&sb);
		pack_copy_state(&sb, view, b->id, 0, 80, 6, 1, 80, 0, 0);
		decode_buffer(&decoder, &sb, fragmented);
		ASSERT(window_copy_get_state(b, &state));
		ASSERT_EQ(state.cx, 80);
		ASSERT_EQ(state.selx, 80);
	}
	msgpack_sbuffer_clear(&sb);
	pack_output(&sb, a->id, "AFTER_COPY_BOUNDARY");
	decode_buffer(&decoder, &sb, fragmented);
	ASSERT(line_equals(a, 0, "AFTER_COPY_BOUNDARY"));
	msgpack_unpacker_destroy(&decoder.unpacker);
	msgpack_sbuffer_destroy(&sb);
	screen_free(&source);
	input_free(input);
	session_destroy(s, 0, __func__);
	destroy_pane(b);
	destroy_pane(a);
	event_loop(EVLOOP_NONBLOCK);
}

TEST(wire_copy_boundary_matches_snapshot_and_ordinary_updates)
{
	int view, fragmented;
	for (view = 0; view <= 1; view++)
		for (fragmented = 0; fragmented <= 1; fragmented++)
			wire_copy_boundary_case(view, 0, fragmented);
}

TEST(wire_copy_outside_boundary_is_atomic_and_keeps_frame_boundaries)
{
	int view, invalid, fragmented;
	for (view = 0; view <= 1; view++)
		for (invalid = 1; invalid <= 5; invalid++)
			for (fragmented = 0; fragmented <= 1; fragmented++)
				wire_copy_boundary_case(view, invalid, fragmented);
}

TEST(wire_snapshot_aggregate_byte_and_nesting_bounds)
{
	msgpack_object bin[4] = {0}, object, nested[14] = {0};
	u_int i;
	object.type = MSGPACK_OBJECT_ARRAY; object.via.array.size = 4; object.via.array.ptr = bin;
	for (i = 0; i < 4; i++) {
		bin[i].type = MSGPACK_OBJECT_BIN; bin[i].via.bin.ptr = "";
		bin[i].via.bin.size = TMTV_SNAPSHOT_MAX_BYTES / 4 - 8;
	}
	ASSERT(tmtv_snapshot_object_bounded(object));
	bin[3].via.bin.size += 32; ASSERT(!tmtv_snapshot_object_bounded(object));
	bin[3].via.bin.size = UINT_MAX; ASSERT(!tmtv_snapshot_object_bounded(object));
	for (i = 0; i < 13; i++) {
		nested[i].type = MSGPACK_OBJECT_ARRAY;
		nested[i].via.array.size = 1; nested[i].via.array.ptr = &nested[i + 1];
	}
	nested[13].type = MSGPACK_OBJECT_NIL;
	ASSERT(!tmtv_snapshot_object_bounded(nested[0]));
	nested[12].type = MSGPACK_OBJECT_NIL;
	ASSERT(tmtv_snapshot_object_bounded(nested[0]));
}

TEST(wire_snapshot_media_is_atomic_in_bulk_and_bytewise_transport)
{
	int i;
	for (i = 26; i <= 29; i++) { wire_case(i, 0); wire_case(i, 1); }
#ifdef ENABLE_SIXEL
	for (i = 30; i <= 33; i++) { wire_case(i, 0); wire_case(i, 1); }
#endif
}
