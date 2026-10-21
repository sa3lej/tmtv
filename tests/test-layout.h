/* Production core -> shared codec -> streaming relay dispatcher. */
/* Preserve MessagePack STR when mutating valid frames. The relay's legacy
 * forwarding helper deliberately rewrites strings as BIN. */
#undef msgpack_pack_object
static void
exact_pack_object(msgpack_sbuffer *sb, msgpack_object obj)
{
	struct tmate_encoder encoder = { .mpac_version = 5 };
	msgpack_sbuffer_init(sb);
	msgpack_packer_init(&encoder.pk, sb, msgpack_sbuffer_write);
	msgpack_pack_object(&encoder.pk, obj);
}

static struct session *
exact_fixture(struct window_pane **base, struct window_pane **floating)
{
	struct session *s;
	struct window *w;
	struct winlink *wl;
	struct layout_cell *lc;
	struct layout_geometry g = { .sx = 30, .sy = 8, .xoff = 4, .yoff = 3 };
	*base = create_pane();
	w = (*base)->window;
	s = session_create(NULL, "exact-layout", "/", environ_create(),
	    options_create(global_s_options), NULL);
	wl = session_attach(s, w, 0, NULL);
	s->curw = wl;
	window_set_active_pane(w, *base, 0);
	lc = layout_floating_pane(w, NULL, &g);
	*floating = window_add_pane(w, NULL, 100, 0);
	layout_assign_pane(lc, *floating, 0);
	return (s);
}

static void
exact_cleanup(struct session *s, struct window_pane *base)
{
	session_destroy(s, 0, __func__);
	while (cmdq_next(NULL) != 0) ;
	event_loop(EVLOOP_NONBLOCK);
	destroy_pane(base);
}

static void
exact_dispatch(msgpack_sbuffer *sb, int fragmented)
{
	struct tmate_decoder decoder;
	tmate_decoder_init(&decoder, read_message, NULL);
	decode_buffer(&decoder, sb, fragmented);
	msgpack_unpacker_destroy(&decoder.unpacker);
}

TEST(exact_layout_preserves_floating_state_and_pane_identity)
{
	struct window_pane *base, *floating;
	struct session *s = exact_fixture(&base, &floating);
	struct window *w = base->window;
	struct grid *grid = floating->base.grid;
	msgpack_sbuffer before, after;
	struct layout_cell *root;
	u_int id = floating->id;

	w->modal = floating; w->modal_last = base;
	floating->flags |= PANE_CAPTUREALLKEYS|PANE_CLOSEONCANCEL|PANE_INPUTOFF;
	window_pane_set_mode(floating, NULL, &window_clock_mode, NULL, NULL, NULL);
	msgpack_sbuffer_init(&before); msgpack_sbuffer_init(&after);
	ASSERT_EQ(tmtv_layout_encode(s, &before), 0);
	root = w->layout_root;
	/* Change only stacking order before restoring the saved production frame. */
	TAILQ_REMOVE(&w->z_index, floating, zentry);
	TAILQ_INSERT_TAIL(&w->z_index, floating, zentry);
	exact_dispatch(&before, 1);
	ASSERT(w->layout_root != root);
	ASSERT(window_pane_find_by_id(id) == floating);
	ASSERT(floating->base.grid == grid);
	ASSERT(TAILQ_FIRST(&floating->modes)->mode == &window_clock_mode);
	ASSERT(window_pane_is_floating(floating));
	ASSERT(w->modal == floating && w->modal_last == base && w->active == base);
	ASSERT(floating->flags & PANE_CAPTUREALLKEYS);
	ASSERT(floating->flags & PANE_INPUTOFF);
	ASSERT_EQ(tmtv_layout_encode(s, &after), 0);
	ASSERT_EQ(before.size, after.size);
	ASSERT(memcmp(before.data, after.data, before.size) == 0);
	/* Repeated restore must not duplicate panes or mode stack entries. */
	exact_dispatch(&before, 0);
	ASSERT_EQ(window_count_panes(w, 1), 2);
	ASSERT(TAILQ_NEXT(TAILQ_FIRST(&floating->modes), entry) == NULL);
	msgpack_sbuffer_destroy(&before); msgpack_sbuffer_destroy(&after);
	exact_cleanup(s, base);
}

TEST(exact_aliases_create_one_shared_window_and_keep_active_index)
{
	struct window_pane *base, *floating;
	struct session *s = exact_fixture(&base, &floating);
	struct window *w = base->window;
	msgpack_sbuffer before, after, forward;
	msgpack_unpacked result;
	msgpack_object *records, tmp;
	size_t offset = 0;
	u_int id = base->id, fid = floating->id;
	ASSERT(session_attach(s, w, 9, NULL) != NULL);
	session_set_current(s, winlink_find_by_index(&s->windows, 9));
	msgpack_sbuffer_init(&before); msgpack_sbuffer_init(&after);
	ASSERT_EQ(tmtv_layout_encode(s, &before), 0);
	msgpack_unpacked_init(&result);
	ASSERT(msgpack_unpack_next(&result, before.data, before.size, &offset));
	records = result.data.via.array.ptr[3].via.array.ptr;
	tmp = records[0]; records[0] = records[1]; records[1] = tmp;
	exact_pack_object(&forward, result.data);
	msgpack_unpacked_destroy(&result);
	exact_cleanup(s, base);
	/* An alias may precede its full record on the wire. */
	exact_dispatch(&forward, 1);
	s = RB_MIN(sessions, &sessions);
	base = window_pane_find_by_id(id);
	floating = window_pane_find_by_id(fid);
	w = base->window;
	window_add_ref(w, __func__);
	ASSERT(winlink_find_by_index(&s->windows, 9)->window == w);
	ASSERT(winlink_find_by_index(&s->windows, 0)->window == w);
	ASSERT_EQ(s->curw->idx, 9);
	ASSERT_EQ(window_count_panes(w, 1), 2);
	ASSERT(floating->window == w && window_pane_is_floating(floating));
	ASSERT_EQ(tmtv_layout_encode(s, &after), 0);
	ASSERT_EQ(before.size, after.size);
	ASSERT(memcmp(before.data, after.data, before.size) == 0);
	exact_dispatch(&before, 0);
	ASSERT_EQ(window_count_panes(w, 1), 2);
	msgpack_sbuffer_destroy(&before); msgpack_sbuffer_destroy(&after);
	msgpack_sbuffer_destroy(&forward);
	exact_cleanup(s, base);
}

TEST(exact_alias_canonical_removal_and_index_reuse_preserve_objects)
{
	struct window_pane *base, *floating, *other;
	struct session *s = exact_fixture(&base, &floating);
	struct window *w = base->window, *ow;
	struct grid *grid = floating->base.grid;
	msgpack_sbuffer before, update;
	msgpack_unpacked result;
	msgpack_object *frame, *records;
	size_t offset = 0;

	ASSERT(session_attach(s, w, 9, NULL) != NULL);
	session_set_current(s, winlink_find_by_index(&s->windows, 9));
	window_pane_set_mode(floating, NULL, &window_clock_mode, NULL, NULL, NULL);
	msgpack_sbuffer_init(&before);
	ASSERT_EQ(tmtv_layout_encode(s, &before), 0);
	msgpack_unpacked_init(&result);
	ASSERT(msgpack_unpack_next(&result, before.data, before.size, &offset));
	frame = result.data.via.array.ptr;
	frame[3].via.array.size = 1;
	frame[3].via.array.ptr[0].via.array.ptr[0].via.u64 = 9;
	{
		msgpack_object payload = result.data;
		struct tmtv_layout *plan;
		payload.via.array.ptr++; payload.via.array.size--;
		plan = tmtv_layout_decode(payload);
		ASSERT(plan != NULL);
		tmtv_layout_free(plan);
	}
	exact_pack_object(&update, result.data);
	ASSERT(RB_MIN(sessions, &sessions) == s);
	exact_dispatch(&update, 1);
	ASSERT(winlink_find_by_index(&s->windows, 0) == NULL);
	ASSERT(s->curw->window == w && s->curw->idx == 9);
	ASSERT(floating->base.grid == grid);
	ASSERT(TAILQ_FIRST(&floating->modes)->mode == &window_clock_mode);
	msgpack_unpacked_destroy(&result);
	msgpack_sbuffer_destroy(&before); msgpack_sbuffer_destroy(&update);

	other = create_pane(); ow = other->window;
	window_set_active_pane(ow, other, 0);
	ASSERT(session_attach(s, ow, 0, NULL) != NULL);
	msgpack_sbuffer_init(&before);
	ASSERT_EQ(tmtv_layout_encode(s, &before), 0);
	msgpack_unpacked_init(&result); offset = 0;
	ASSERT(msgpack_unpack_next(&result, before.data, before.size, &offset));
	frame = result.data.via.array.ptr; records = frame[3].via.array.ptr;
	records[0].via.array.ptr[0].via.u64 = 9;
	records[1].via.array.ptr[0].via.u64 = 0;
	frame[2].via.u64 = 0;
	exact_pack_object(&update, result.data);
	exact_dispatch(&update, 0);
	ASSERT(s->curw->idx == 0 && s->curw->window == w);
	ASSERT(winlink_find_by_index(&s->windows, 9)->window == ow);
	ASSERT(other->window == ow && floating->window == w);
	ASSERT(floating->base.grid == grid);
	msgpack_unpacked_destroy(&result);
	msgpack_sbuffer_destroy(&before); msgpack_sbuffer_destroy(&update);
	exact_cleanup(s, base); destroy_pane(other);
}

TEST(exact_invalid_aliases_leave_all_links_and_panes_untouched)
{
	struct window_pane *base, *floating;
	struct session *s = exact_fixture(&base, &floating);
	struct window *w = base->window;
	struct layout_cell *root = w->layout_root;
	msgpack_sbuffer valid, invalid;
	msgpack_unpacked result;
	msgpack_object *frame, *records;
	msgpack_object duplicate[16];
	size_t offset;
	int variant;
	ASSERT(session_attach(s, w, 9, NULL) != NULL);
	msgpack_sbuffer_init(&valid);
	ASSERT_EQ(tmtv_layout_encode(s, &valid), 0);
	for (variant = 0; variant < 6; variant++) {
		msgpack_unpacked_init(&result); offset = 0;
		ASSERT(msgpack_unpack_next(&result, valid.data, valid.size, &offset));
		frame = result.data.via.array.ptr; records = frame[3].via.array.ptr;
		switch (variant) {
		case 0: records[1].via.array.ptr[1].via.u64 = INT_MAX; break;
		case 1: records[1].via.array.ptr[0].via.u64 = 0; break;
		case 2: records[0] = records[1]; frame[3].via.array.size = 1;
			frame[2].via.u64 = 9; break; /* alias without any full record */
		case 3: memcpy(duplicate, records[0].via.array.ptr, sizeof duplicate);
			duplicate[0].via.u64 = 9;
			records[1] = records[0]; records[1].via.array.ptr = duplicate;
			break; /* distinct indices must not duplicate a full window ID */
		case 4: records[1].via.array.size = 1; break;
		case 5: frame[1].via.u64 = 1; break; /* obsolete v1 must not be accepted */
		}
		exact_pack_object(&invalid, result.data);
		exact_dispatch(&invalid, 1);
		ASSERT(w->layout_root == root && base->window == w && floating->window == w);
		ASSERT(winlink_find_by_index(&s->windows, 9)->window == w);
		ASSERT(winlink_find_by_index(&s->windows, 0)->window == w);
		ASSERT_EQ(window_count_panes(w, 1), 2);
		msgpack_sbuffer_destroy(&invalid); msgpack_unpacked_destroy(&result);
	}
	msgpack_sbuffer_destroy(&valid);
	exact_cleanup(s, base);
}

TEST(exact_layout_creates_all_panes_before_activation)
{
	struct window_pane *base, *floating;
	struct session *s = exact_fixture(&base, &floating);
	msgpack_sbuffer before, after;
	u_int id = base->id, fid = floating->id;
	msgpack_sbuffer_init(&before); msgpack_sbuffer_init(&after);
	ASSERT_EQ(tmtv_layout_encode(s, &before), 0);
	exact_cleanup(s, base);
	ASSERT(RB_EMPTY(&sessions));
	exact_dispatch(&before, 1);
	s = RB_MIN(sessions, &sessions);
	base = window_pane_find_by_id(id);
	floating = window_pane_find_by_id(fid);
	ASSERT(base != NULL && floating != NULL && base->ictx != NULL);
	window_add_ref(base->window, __func__);
	ASSERT(window_pane_is_floating(floating));
	ASSERT_EQ(tmtv_layout_encode(s, &after), 0);
	ASSERT_EQ(before.size, after.size);
	ASSERT(memcmp(before.data, after.data, before.size) == 0);
	msgpack_sbuffer_destroy(&before); msgpack_sbuffer_destroy(&after);
	exact_cleanup(s, base);
}

TEST(exact_layout_moves_panes_between_windows_without_recreating)
{
	struct window_pane *base, *floating, *other = create_pane();
	struct session *s = exact_fixture(&base, &floating);
	struct window *w = base->window, *destination = other->window;
	struct grid *grid = floating->base.grid;
	struct layout_cell *lc;
	msgpack_sbuffer before, after;
	ASSERT(session_attach(s, destination, 1, NULL) != NULL);
	window_set_active_pane(destination, other, 0);
	msgpack_sbuffer_init(&before); msgpack_sbuffer_init(&after);
	ASSERT_EQ(tmtv_layout_encode(s, &before), 0);
	layout_close_pane(floating);
	TAILQ_REMOVE(&w->panes, floating, entry);
	TAILQ_REMOVE(&w->z_index, floating, zentry);
	floating->window = destination;
	options_set_parent(floating->options, destination->options);
	TAILQ_INSERT_TAIL(&destination->panes, floating, entry);
	TAILQ_INSERT_TAIL(&destination->z_index, floating, zentry);
	lc = layout_split_pane(other, LAYOUT_LEFTRIGHT, 30, 0);
	ASSERT(lc != NULL);
	layout_assign_pane(lc, floating, 0);
	exact_dispatch(&before, 1);
	ASSERT(floating->window == w && floating->base.grid == grid);
	ASSERT(window_pane_is_floating(floating));
	ASSERT_EQ(window_count_panes(destination, 1), 1);
	ASSERT_EQ(tmtv_layout_encode(s, &after), 0);
	ASSERT_EQ(before.size, after.size);
	ASSERT(memcmp(before.data, after.data, before.size) == 0);
	msgpack_sbuffer_destroy(&before); msgpack_sbuffer_destroy(&after);
	exact_cleanup(s, base);
	destroy_pane(other);
}

TEST(exact_layout_preserves_zoom_and_saved_tree)
{
	struct window_pane *base, *floating;
	struct session *s = exact_fixture(&base, &floating);
	struct window *w = base->window;
	msgpack_sbuffer before, after;

	ASSERT_EQ(window_zoom(base), 0);
	ASSERT(w->saved_layout_root != NULL);
	msgpack_sbuffer_init(&before); msgpack_sbuffer_init(&after);
	ASSERT_EQ(tmtv_layout_encode(s, &before), 0);
	window_unzoom(w, 0);
	exact_dispatch(&before, 0);
	ASSERT(w->flags & WINDOW_ZOOMED);
	ASSERT(base->layout_cell != NULL && base->saved_layout_cell != NULL);
	ASSERT(floating->saved_layout_cell != NULL);
	ASSERT_EQ(tmtv_layout_encode(s, &after), 0);
	ASSERT_EQ(before.size, after.size);
	ASSERT(memcmp(before.data, after.data, before.size) == 0);
	window_unzoom(w, 0);
	ASSERT(window_pane_is_floating(floating));
	ASSERT_EQ(window_count_panes(w, 1), 2);
	msgpack_sbuffer_destroy(&before); msgpack_sbuffer_destroy(&after);
	exact_cleanup(s, base);
}

TEST(exact_layout_rejects_invalid_graphs_atomically)
{
	struct window_pane *base, *floating;
	struct session *s = exact_fixture(&base, &floating);
	struct window *w = base->window;
	struct layout_cell *root = w->layout_root;
	struct grid *grid = floating->base.grid;
	msgpack_sbuffer valid, invalid;
	msgpack_unpacked result;
	msgpack_object *frame, *window, *panes, *cells;
	struct tmate_encoder encoder = { .mpac_version = 5 };
	msgpack_packer *pk = &encoder.pk;
	size_t offset;
	int variant, count;

	msgpack_sbuffer_init(&valid);
	ASSERT_EQ(tmtv_layout_encode(s, &valid), 0);
	for (variant = 0; variant < 14; variant++) {
		msgpack_unpacked_init(&result); offset = 0;
		ASSERT(msgpack_unpack_next(&result, valid.data, valid.size, &offset));
		frame = result.data.via.array.ptr;
		window = frame[3].via.array.ptr[0].via.array.ptr;
		panes = window[9].via.array.ptr;
		cells = window[10].via.array.ptr;
		switch (variant) {
		case 0: frame[1].via.u64 = TMTV_LAYOUT_VERSION + 1; break;
		case 1: frame[2].via.u64 = 999; break; /* absent window */
		case 2: panes[1].via.array.ptr[0] = panes[0].via.array.ptr[0]; break;
		case 3: window[4].via.u64 = 999; break; /* active pane */
		case 4: window[5].type = MSGPACK_OBJECT_POSITIVE_INTEGER;
			window[5].via.u64 = 999; break; /* modal pane */
		case 5: window[12].via.array.ptr[1] = window[12].via.array.ptr[0]; break;
		case 6: cells[1].via.array.ptr[0].via.u64 = 1; break; /* cycle */
		case 7: cells[0].via.array.ptr[1].via.u64 = 2; break; /* leaf parent */
		case 8: cells[2].via.array.ptr[5].via.u64 = 999; break;
		case 9: panes[0].via.array.ptr[1].via.u64 = 0; break;
		case 10: panes[0].via.array.ptr[5].via.u64 = 64; break;
		case 11: window[8].via.u64 = 1; break; /* zoom without saved tree */
		case 12: window[10].via.array.size = 0; break;
		case 13: cells[0].via.array.ptr[3].via.array.ptr[2].type = MSGPACK_OBJECT_NEGATIVE_INTEGER;
			cells[0].via.array.ptr[3].via.array.ptr[2].via.i64 = INT_MIN; break;
		}
		msgpack_sbuffer_init(&invalid);
		msgpack_packer_init(pk, &invalid, msgpack_sbuffer_write);
		msgpack_pack_object(pk, result.data);
		msgpack_pack_array(pk, 3);
		msgpack_pack_int(pk, TMATE_OUT_WRITE_COPY_MODE);
		msgpack_pack_unsigned_int(pk, base->id);
		msgpack_pack_string(pk, "after");
		count = dispatched;
		exact_dispatch(&invalid, 1);
		ASSERT_EQ(dispatched, count + 2);
		ASSERT(w->layout_root == root && floating->base.grid == grid);
		ASSERT(w->active == base && window_count_panes(w, 1) == 2);
		ASSERT(line_equals(base, 0, "after"));
		msgpack_sbuffer_destroy(&invalid);
		msgpack_unpacked_destroy(&result);
	}
	msgpack_sbuffer_destroy(&valid);
	exact_cleanup(s, base);
}
