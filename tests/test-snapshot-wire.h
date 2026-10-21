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
	if (variant == 25) {
		cell = grid_default_cell; utf8_set(&cell.data, 'H');
		for (i = 0; i < 1700; i++) {
			grid_set_cell(source.grid, 0, source.grid->hsize, &cell);
			grid_scroll_history(source.grid, 8);
		}
	}
	input = input_init(NULL, NULL, NULL, NULL);
	input_parse_screen(input, &source, NULL, NULL, (u_char *)"NEW\033[38;2;17;34;", strlen("NEW\033[38;2;17;34;"));
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
	case -1: case 25: break;
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
	if (variant == -1) {
		ASSERT(a->base.grid != ga && b->base.grid != gb);
		grid_get_cell(b->base.grid, 0, 0, &cell); ASSERT_EQ(cell.data.data[0], 'N');
		/* The restored parser is immediately ready for following PTY bytes. */
		input_parse_screen(b->ictx, &b->base, NULL, NULL, (u_char *)"51mC", 4);
		grid_get_cell(b->base.grid, 3, 0, &cell); ASSERT_EQ(cell.data.data[0], 'C');
		ASSERT_EQ(cell.fg, COLOUR_FLAG_RGB | 0x112233);
		ASSERT_EQ(input_snapshot_encode(b->ictx, &changed), 0);
	} else ASSERT(a->base.grid == ga && b->base.grid == gb);
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
