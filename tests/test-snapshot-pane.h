#include "snapshot-pane-fixture.h"

static int
pane_unpack(u_int id, struct screen *s, msgpack_sbuffer *sb, msgpack_unpacked *un)
{
	msgpack_sbuffer_init(sb); msgpack_unpacked_init(un);
	return (tmtv_snapshot_pane_encode(id, s, 100, sb) == 0 &&
	    msgpack_unpack_next(un, sb->data, sb->size, NULL) == MSGPACK_UNPACK_SUCCESS);
}

TEST(pane_snapshot_restores_active_saved_screen_and_exit_style)
{
	struct window_pane *wp = create_pane();
	struct screen source;
	struct tmtv_snapshot_pane *p;
	msgpack_sbuffer sb;
	msgpack_unpacked un;
	pane_fixture_screen(&source);
	ASSERT(pane_fixture_expected(&source));
	ASSERT(pane_unpack(wp->id, &source, &sb, &un));
	p = tmtv_snapshot_pane_decode(un.data);
	ASSERT_NOT_NULL(p); ASSERT_EQ(tmtv_snapshot_pane_apply(wp, p), 0);
	ASSERT(wp->screen == &wp->base); ASSERT(pane_fixture_expected(&wp->base));
	ASSERT(pane_fixture_exit(&wp->base));
	tmtv_snapshot_pane_free(p); screen_free(&source); destroy_pane(wp);
	msgpack_sbuffer_destroy(&sb); msgpack_unpacked_destroy(&un);
}

TEST(pane_snapshot_keeps_copy_clock_and_frozen_backing)
{
	struct window_pane *wp = create_pane();
	struct screen source;
	struct grid_cell gc = grid_default_cell;
	struct window_mode_entry *copy, *clock;
	struct screen *backing, *top;
	struct args *args = args_create();
	struct tmtv_snapshot_pane *p;
	msgpack_sbuffer sb;
	msgpack_unpacked un;
	utf8_set(&gc.data, 'F'); grid_set_cell(wp->base.grid, 0, 0, &gc);
	window_pane_set_mode(wp, wp, &window_copy_mode, NULL, NULL, args);
	copy = TAILQ_FIRST(&wp->modes); backing = copy->mode->get_screen(copy);
	window_pane_set_mode(wp, wp, &window_clock_mode, NULL, NULL, NULL);
	clock = TAILQ_FIRST(&wp->modes); top = wp->screen;
	pane_fixture_screen(&source);
	ASSERT(pane_unpack(wp->id, &source, &sb, &un));
	p = tmtv_snapshot_pane_decode(un.data);
	ASSERT_NOT_NULL(p); ASSERT_EQ(tmtv_snapshot_pane_apply(wp, p), 0);
	ASSERT(wp->screen == top && TAILQ_FIRST(&wp->modes) == clock);
	ASSERT(TAILQ_NEXT(clock, entry) == copy && copy->mode->get_screen(copy) == backing);
	grid_get_cell(backing->grid, 0, backing->grid->hsize, &gc);
	ASSERT_EQ(gc.data.data[0], 'F'); ASSERT(pane_fixture_expected(&wp->base));
	tmtv_snapshot_pane_free(p); screen_free(&source); args_free(args); destroy_pane(wp);
	msgpack_sbuffer_destroy(&sb); msgpack_unpacked_destroy(&un);
}

TEST(pane_snapshot_rejects_identity_and_geometry_before_mutation)
{
	struct window_pane *wp = create_pane();
	struct screen source;
	struct grid *original = wp->base.grid;
	bitstr_t *tabs = wp->base.tabs;
	struct tmtv_snapshot_pane *p;
	msgpack_sbuffer sb;
	msgpack_unpacked un;
	pane_fixture_screen(&source);
	ASSERT(pane_unpack(wp->id + 1, &source, &sb, &un));
	p = tmtv_snapshot_pane_decode(un.data); ASSERT_NOT_NULL(p);
	ASSERT_EQ(tmtv_snapshot_pane_apply(wp, p), -1);
	ASSERT(wp->base.grid == original && wp->base.tabs == tabs);
	p->id = wp->id; wp->sx--;
	ASSERT_EQ(tmtv_snapshot_pane_apply(wp, p), -1);
	ASSERT(wp->base.grid == original && wp->base.tabs == tabs); wp->sx++;
	tmtv_snapshot_pane_free(p); screen_free(&source); destroy_pane(wp);
	msgpack_sbuffer_destroy(&sb); msgpack_unpacked_destroy(&un);
}

TEST(pane_snapshot_rearms_sync_and_replaces_old_write_queue)
{
	struct window_pane *wp = create_pane();
	struct screen source;
	struct screen_write_ctx ctx;
	struct tmtv_snapshot_pane *p;
	msgpack_sbuffer sb;
	msgpack_unpacked un;
	screen_write_start(&ctx, &wp->base);
	screen_write_puts(&ctx, &grid_default_cell, "OLD"); screen_write_stop(&ctx);
	ASSERT_NOT_NULL(wp->base.write_list);
	screen_write_start_sync(wp);
	pane_fixture_screen(&source); source.mode |= MODE_SYNC;
	ASSERT(pane_unpack(wp->id, &source, &sb, &un));
	p = tmtv_snapshot_pane_decode(un.data); ASSERT_NOT_NULL(p);
	ASSERT_EQ(tmtv_snapshot_pane_apply(wp, p), 0);
	ASSERT(wp->base.write_list == NULL);
	ASSERT(wp->base.mode & MODE_SYNC); ASSERT(evtimer_pending(&wp->sync_timer, NULL));
	screen_write_stop_sync(wp);
	tmtv_snapshot_pane_free(p); screen_free(&source); destroy_pane(wp);
	msgpack_sbuffer_destroy(&sb); msgpack_unpacked_destroy(&un);
}

static void
pane_bad(u_int which)
{
	struct screen source;
	msgpack_sbuffer sb;
	msgpack_unpacked un;
	msgpack_object *v, *saved;
	pane_fixture_screen(&source); ASSERT(pane_unpack(1, &source, &sb, &un));
	v = un.data.via.array.ptr; saved = v[8].via.array.ptr;
	switch (which) {
	case 0: v[0].via.u64 = TMTV_SNAPSHOT_PANE_VERSION + 1; break;
	case 1: v[1].via.u64 = (uint64_t)INT_MAX + 1; break;
	case 2: v[3].via.array.ptr[0].via.u64 = 81; break;
	case 3: v[3].via.array.ptr[1].via.u64 = 24; break;
	case 4: v[4].via.array.ptr[0].via.u64 = 0x200000; break;
	case 5: v[4].via.array.ptr[1].via.u64 = UINT64_MAX; break;
	case 6: v[5].via.array.ptr[0].via.u64 = 4; break;
	case 7: v[5].via.array.ptr[2].type = MSGPACK_OBJECT_NEGATIVE_INTEGER;
		v[5].via.array.ptr[2].via.i64 = -2; break;
	case 8: v[6].via.array.ptr[0].via.u64 = 20; break;
	case 9: v[7].via.array.ptr[0].via.u64 = 80; break;
	case 10: v[7].via.array.ptr[1].via.u64 = v[7].via.array.ptr[0].via.u64; break;
	case 11: saved[3].via.u64 = 1; break;
	case 12: saved[5].via.u64 = GRID_HISTORY; break;
	case 13: v[2].via.array.ptr[5].via.u64 = GRID_HISTORY; break;
	case 14: v[9].via.array.ptr[0].via.u64 = UINT_MAX; break;
	case 15: v[9].via.array.ptr[1].via.u64 = 24; break;
	case 16: v[10].type = MSGPACK_OBJECT_NIL; break;
	case 17: v[10].via.array.ptr[4].via.u64 = (uint64_t)INT_MAX + 1; break;
	case 18: v[11].via.u64 = 2; break;
	case 19: v[8].type = MSGPACK_OBJECT_NIL; break; /* inconsistent saved_flags */
	case 20: saved[6].via.array.size--; break;
	case 21: v[10].via.array.ptr[0].via.bin.size = 32; break;
	}
	ASSERT(tmtv_snapshot_pane_decode(un.data) == NULL);
	screen_free(&source); msgpack_sbuffer_destroy(&sb); msgpack_unpacked_destroy(&un);
}
#define BAD_PANE(name, index) TEST(pane_snapshot_rejects_##name) { pane_bad(index); }
BAD_PANE(version, 0)
BAD_PANE(overflow_id, 1)
BAD_PANE(cursor_x, 2)
BAD_PANE(cursor_y, 3)
BAD_PANE(mode, 4)
BAD_PANE(default_mode, 5)
BAD_PANE(cursor_style, 6)
BAD_PANE(cursor_colour, 7)
BAD_PANE(scroll_region, 8)
BAD_PANE(tab_range, 9)
BAD_PANE(duplicate_tab, 10)
BAD_PANE(saved_history, 11)
BAD_PANE(saved_history_flag, 12)
BAD_PANE(alternate_history_flag, 13)
BAD_PANE(unpaired_cursor_sentinel, 14)
BAD_PANE(saved_cursor_range, 15)
BAD_PANE(missing_saved_style, 16)
BAD_PANE(saved_colour, 17)
BAD_PANE(saved_flags, 18)
BAD_PANE(missing_saved_grid, 19)
BAD_PANE(saved_grid_rows, 20)
BAD_PANE(saved_style_length, 21)
#undef BAD_PANE

TEST(pane_snapshot_combined_grid_budget_is_checked_before_allocation)
{
	struct screen source;
	msgpack_sbuffer sb;
	msgpack_unpacked un;
	msgpack_object *v, *base, *saved, rows[15], saved_rows[15];
	u_int i, cost;
	pane_fixture_screen(&source); ASSERT(pane_unpack(1, &source, &sb, &un));
	v = un.data.via.array.ptr; base = v[2].via.array.ptr; saved = v[8].via.array.ptr;
	for (i = 0; i < 15; i++) {
		rows[i] = base[6].via.array.ptr[0]; saved_rows[i] = saved[6].via.array.ptr[0];
	}
	base[1].via.u64 = saved[1].via.u64 = 10000;
	base[2].via.u64 = saved[2].via.u64 = 15;
	base[6].via.array.size = saved[6].via.array.size = 15;
	base[6].via.array.ptr = rows; saved[6].via.array.ptr = saved_rows;
	v[6].via.array.ptr[1].via.u64 = 14;
	ASSERT(tmtv_snapshot_grid_validate(v[2], 2000, &cost)); ASSERT_EQ(cost, 150000);
	ASSERT(tmtv_snapshot_grid_validate(v[8], 0, &cost)); ASSERT_EQ(cost, 150000);
	ASSERT(tmtv_snapshot_pane_decode(un.data) == NULL);
	screen_free(&source); msgpack_sbuffer_destroy(&sb); msgpack_unpacked_destroy(&un);
}

TEST(pane_snapshot_empty_screen_ignores_uninitialized_saved_fields)
{
	struct window_pane *wp = create_pane();
	msgpack_sbuffer sb;
	msgpack_unpacked un;
	struct tmtv_snapshot_pane *p;
	/* saved_cell/saved_flags have no meaning until the screen saves them. */
	memset(&wp->base.saved_cell, 0xa5, sizeof wp->base.saved_cell);
	wp->base.saved_flags = INT_MAX;
	ASSERT(pane_unpack(wp->id, &wp->base, &sb, &un));
	p = tmtv_snapshot_pane_decode(un.data); ASSERT_NOT_NULL(p);
	ASSERT(p->saved_grid == NULL && p->saved_cx == UINT_MAX && p->saved_cy == UINT_MAX);
	ASSERT_EQ(p->saved_cell.fg, 8); ASSERT_EQ(p->saved_flags, 0);
	tmtv_snapshot_pane_free(p); destroy_pane(wp);
	msgpack_sbuffer_destroy(&sb); msgpack_unpacked_destroy(&un);
}

TEST(pane_snapshot_unsupported_saved_links_and_images_are_atomic)
{
	struct screen source;
	msgpack_sbuffer sb;
#ifdef ENABLE_SIXEL
	struct image image;
#endif
	pane_fixture_screen(&source); msgpack_sbuffer_init(&sb);
	msgpack_sbuffer_write(&sb, "KEEP", 4);
	source.saved_cell.link = 1;
	ASSERT_EQ(tmtv_snapshot_pane_encode(1, &source, 100, &sb), -1);
	source.saved_cell.link = 0;
#ifdef ENABLE_SIXEL
	memset(&image, 0, sizeof image);
	TAILQ_INSERT_HEAD(&source.images, &image, entry);
	ASSERT_EQ(tmtv_snapshot_pane_encode(1, &source, 100, &sb), -1);
	TAILQ_REMOVE(&source.images, &image, entry);
	TAILQ_INSERT_HEAD(&source.saved_images, &image, entry);
	ASSERT_EQ(tmtv_snapshot_pane_encode(1, &source, 100, &sb), -1);
	TAILQ_REMOVE(&source.saved_images, &image, entry);
#endif
	ASSERT_EQ(sb.size, 4); ASSERT(memcmp(sb.data, "KEEP", 4) == 0);
	screen_free(&source); msgpack_sbuffer_destroy(&sb);
}

TEST(pane_snapshot_saved_dimensions_survive_alternate_resize)
{
	struct window_pane *wp = create_pane();
	struct screen source;
	struct tmtv_snapshot_pane *p;
	msgpack_sbuffer sb;
	msgpack_unpacked un;
	pane_fixture_screen(&source); screen_resize(&source, 60, 12, 0);
	window_pane_resize(wp, 60, 12);
	ASSERT(source.saved_grid->sx == 80 && source.saved_grid->sy == 24);
	ASSERT(pane_unpack(wp->id, &source, &sb, &un));
	p = tmtv_snapshot_pane_decode(un.data); ASSERT_NOT_NULL(p);
	ASSERT_EQ(tmtv_snapshot_pane_apply(wp, p), 0);
	ASSERT_EQ(wp->base.grid->sx, 60); ASSERT_EQ(wp->base.saved_grid->sx, 80);
	ASSERT_EQ(wp->base.saved_grid->sy, 24);
	ASSERT(pane_fixture_exit(&wp->base));
	ASSERT_EQ(wp->base.grid->sx, 60); ASSERT_EQ(wp->base.grid->sy, 12);
	tmtv_snapshot_pane_free(p); screen_free(&source); destroy_pane(wp);
	msgpack_sbuffer_destroy(&sb); msgpack_unpacked_destroy(&un);
}

TEST(pane_snapshot_saved_cursor_style_survives_after_alternate_exit)
{
	struct window_pane *wp = create_pane();
	struct screen source;
	struct tmtv_snapshot_pane *p;
	msgpack_sbuffer sb;
	msgpack_unpacked un;
	pane_fixture_screen(&source); ASSERT(pane_fixture_exit(&source));
	ASSERT(pane_unpack(wp->id, &source, &sb, &un));
	p = tmtv_snapshot_pane_decode(un.data); ASSERT_NOT_NULL(p);
	ASSERT_EQ(tmtv_snapshot_pane_apply(wp, p), 0);
	ASSERT(wp->base.saved_grid == NULL);
	ASSERT_EQ(wp->base.saved_cx, 4);
	ASSERT_EQ(wp->base.saved_cell.fg, COLOUR_FLAG_RGB|0x123456);
	tmtv_snapshot_pane_free(p); screen_free(&source); destroy_pane(wp);
	msgpack_sbuffer_destroy(&sb); msgpack_unpacked_destroy(&un);
}

TEST(pane_snapshot_both_grids_share_output_byte_limit)
{
	struct screen source;
	msgpack_sbuffer sb;
	char block[4096];
	size_t remaining = TMTV_SNAPSHOT_MAX_BYTES - 1;
	pane_fixture_screen(&source); msgpack_sbuffer_init(&sb); memset(block, 'K', sizeof block);
	while (remaining) {
		size_t n = remaining < sizeof block ? remaining : sizeof block;
		ASSERT_EQ(msgpack_sbuffer_write(&sb, block, n), 0); remaining -= n;
	}
	ASSERT_EQ(tmtv_snapshot_pane_encode(1, &source, 100, &sb), -1);
	ASSERT_EQ(sb.size, TMTV_SNAPSHOT_MAX_BYTES - 1);
	ASSERT_EQ(sb.data[0], 'K'); ASSERT_EQ(sb.data[sb.size - 1], 'K');
	screen_free(&source); msgpack_sbuffer_destroy(&sb);
}

TEST(host_pane_codec_stream_roundtrip)
{
	struct window_pane *wp = create_pane();
	msgpack_unpacker uk;
	msgpack_unpacked un;
	struct tmtv_snapshot_pane *p;
	size_t n, chunk = snapshot_bulk ? 4096 : 1;
	int count = 0;
	msgpack_unpacker_init(&uk, 128); msgpack_unpacked_init(&un);
	while (1) {
		ASSERT(msgpack_unpacker_reserve_buffer(&uk, chunk));
		n = fread(msgpack_unpacker_buffer(&uk), 1, chunk, stdin);
		if (!n) break;
		msgpack_unpacker_buffer_consumed(&uk, n);
		while (msgpack_unpacker_next(&uk, &un) == MSGPACK_UNPACK_SUCCESS) {
			p = tmtv_snapshot_pane_decode(un.data); ASSERT_NOT_NULL(p);
			ASSERT_EQ(tmtv_snapshot_pane_apply(wp, p), 0);
			ASSERT(pane_fixture_expected(&wp->base));
			ASSERT(pane_fixture_exit(&wp->base));
			tmtv_snapshot_pane_free(p); count++;
		}
	}
	ASSERT_EQ(count, 1); ASSERT_EQ(msgpack_unpacker_message_size(&uk), 0);
	destroy_pane(wp); msgpack_unpacked_destroy(&un); msgpack_unpacker_destroy(&uk);
}

#ifdef ENABLE_SIXEL
TEST(pane_snapshot_clears_stale_current_and_saved_image_owners)
{
	struct window_pane *wp = create_pane();
	struct screen source, scratch;
	struct tmtv_snapshot_pane *p;
	struct sixel_image *si;
	msgpack_sbuffer sb;
	msgpack_unpacked un;
	u_int i;
	si = sixel_parse("q~", 2, 0, 8, 16); ASSERT_NOT_NULL(si);
	image_store(&wp->base, si);
	screen_alternate_on(&wp->base, (struct grid_cell *)&grid_default_cell, 0);
	si = sixel_parse("q~", 2, 0, 8, 16); ASSERT_NOT_NULL(si);
	image_store(&wp->base, si);
	ASSERT(!TAILQ_EMPTY(&wp->base.images) && !TAILQ_EMPTY(&wp->base.saved_images));
	pane_fixture_screen(&source); ASSERT(pane_unpack(wp->id, &source, &sb, &un));
	p = tmtv_snapshot_pane_decode(un.data); ASSERT_NOT_NULL(p);
	p->id++; ASSERT_EQ(tmtv_snapshot_pane_apply(wp, p), -1); p->id--;
	ASSERT(!TAILQ_EMPTY(&wp->base.images) && !TAILQ_EMPTY(&wp->base.saved_images));
	ASSERT_EQ(tmtv_snapshot_pane_apply(wp, p), 0);
	ASSERT(TAILQ_EMPTY(&wp->base.images) && TAILQ_EMPTY(&wp->base.saved_images));
	tmtv_snapshot_pane_free(p); destroy_pane(wp);
	/* Force global image eviction after the old owner has been destroyed. */
	screen_init(&scratch, 80, 24, 0);
	for (i = 0; i < 25; i++) {
		si = sixel_parse("q~", 2, 0, 8, 16); ASSERT_NOT_NULL(si);
		image_store(&scratch, si);
	}
	screen_free(&scratch); screen_free(&source);
	msgpack_sbuffer_destroy(&sb); msgpack_unpacked_destroy(&un);
}
#endif
