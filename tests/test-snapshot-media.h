static int
media_unpack(struct screen *s, msgpack_sbuffer *sb, msgpack_unpacked *un)
{
	msgpack_sbuffer_init(sb); msgpack_unpacked_init(un);
	return (tmtv_snapshot_pane_encode(1, s, 100, sb) == 0 &&
	    msgpack_unpack_next(un, sb->data, sb->size, NULL) ==
	    MSGPACK_UNPACK_SUCCESS);
}

static void
media_parse(struct screen *s, const char *bytes)
{
	struct input_ctx *input = input_init(NULL, NULL, NULL, NULL);

	input_parse_screen(input, s, NULL, NULL, (const u_char *)bytes,
	    strlen(bytes));
	input_free(input);
}

static msgpack_object
media_bin(const char *text, size_t size)
{
	msgpack_object o;

	o.type = MSGPACK_OBJECT_BIN;
	o.via.bin.ptr = text;
	o.via.bin.size = size;
	return (o);
}

static msgpack_object
media_table(msgpack_object *entries, u_int count)
{
	msgpack_object o;

	o.type = MSGPACK_OBJECT_ARRAY;
	o.via.array.ptr = entries;
	o.via.array.size = count;
	return (o);
}

static msgpack_object
media_entry(u_int inner, const char *uri, size_t uri_size,
    const char *id, size_t id_size)
{
	msgpack_object *item = xcalloc(3, sizeof *item);

	item[0].type = MSGPACK_OBJECT_POSITIVE_INTEGER;
	item[0].via.u64 = inner;
	item[1] = media_bin(uri, uri_size);
	item[2] = media_bin(id, id_size);
	return (media_table(item, 3));
}

TEST(media_snapshot_links_survive_table_replacement_and_alternate_screen)
{
	struct window_pane *wp = create_pane();
	struct screen source;
	struct grid_cell gc;
	struct tmtv_snapshot_pane *p;
	const char *uri, *id, *external;
	msgpack_sbuffer sb, left, right;
	msgpack_unpacked un;

	screen_init(&source, 80, 24, 0);
	media_parse(&source, "\033]8;id=named;https://example.invalid/a\007N\033]8;;\007");
	screen_alternate_on(&source, (struct grid_cell *)&grid_default_cell, 0);
	media_parse(&source, "\033[H\033]8;;https://example.invalid/anon\007A\033]8;;\007");
	ASSERT(media_unpack(&source, &sb, &un));
	p = tmtv_snapshot_pane_decode(un.data); ASSERT_NOT_NULL(p);
	p->id = wp->id; ASSERT_EQ(tmtv_snapshot_pane_apply(wp, p), 0);
	grid_get_cell(wp->base.saved_grid, 0, 0, &gc);
	ASSERT(gc.link != 0);
	ASSERT(hyperlinks_get(wp->base.hyperlinks, gc.link, &uri, &id, &external));
	ASSERT_STR_EQ(uri, "https://example.invalid/a");
	ASSERT_STR_EQ(id, "named"); ASSERT_NOT_NULL(external);
	grid_get_cell(wp->base.grid, 0, 0, &gc);
	ASSERT(gc.link != 0);
	ASSERT(hyperlinks_get(wp->base.hyperlinks, gc.link, &uri, &id, &external));
	ASSERT_STR_EQ(uri, "https://example.invalid/anon");
	ASSERT_STR_EQ(id, ""); ASSERT_NOT_NULL(external);
	msgpack_sbuffer_init(&left); msgpack_sbuffer_init(&right);
	ASSERT_EQ(tmtv_snapshot_pane_encode(wp->id, &source, 100, &left), 0);
	ASSERT_EQ(tmtv_snapshot_pane_encode(wp->id, &wp->base, 100, &right), 0);
	ASSERT_EQ(left.size, right.size);
	ASSERT(memcmp(left.data, right.data, left.size) == 0);
	tmtv_snapshot_pane_free(p); screen_free(&source); destroy_pane(wp);
	msgpack_sbuffer_destroy(&sb); msgpack_sbuffer_destroy(&left);
	msgpack_sbuffer_destroy(&right); msgpack_unpacked_destroy(&un);
}

TEST(media_snapshot_rejects_dangling_duplicate_and_control_links)
{
	struct window_pane *wp = create_pane();
	struct screen source;
	struct tmtv_snapshot_pane *p;
	msgpack_sbuffer sb;
	msgpack_unpacked un;
	msgpack_object *v, entries[2], bad;

	entries[0] = media_entry(1, "https://example.invalid/a", 25, "named", 5);
	entries[1] = media_entry(2, "https://example.invalid/a", 25, "named", 5);
	bad = media_table(entries, 2);
	ASSERT(!hyperlinks_snapshot_validate(bad));
	entries[1].via.array.ptr[1] = media_bin("\001", 1);
	entries[1].via.array.ptr[2] = media_bin("", 0);
	ASSERT(!hyperlinks_snapshot_validate(bad));
	free(entries[0].via.array.ptr); free(entries[1].via.array.ptr);

	screen_init(&source, 80, 24, 0);
	media_parse(&source, "\033]8;id=named;https://example.invalid/a\007N\033]8;;\007");
	ASSERT(media_unpack(&source, &sb, &un));
	v = un.data.via.array.ptr;
	v[2].via.array.ptr[6].via.array.ptr[0].via.array.ptr[3].via.array.ptr[0].via.array.ptr[7].via.u64 = 2;
	ASSERT(tmtv_snapshot_pane_decode(un.data) == NULL);
	msgpack_sbuffer_clear(&sb);
	ASSERT_EQ(tmtv_snapshot_pane_encode(wp->id, &source, 100, &sb), 0);
	msgpack_unpack_next(&un, sb.data, sb.size, NULL);
	p = tmtv_snapshot_pane_decode(un.data); ASSERT_NOT_NULL(p);
	tmtv_snapshot_pane_free(p); screen_free(&source); destroy_pane(wp);
	msgpack_sbuffer_destroy(&sb); msgpack_unpacked_destroy(&un);
}

TEST(media_snapshot_detached_links_do_not_evict_live_links)
{
	struct screen source, live;
	struct tmtv_snapshot_pane *p;
	const char *uri;
	char sequence[128];
	msgpack_sbuffer sb;
	msgpack_unpacked un;
	u_int i, first = 0;

	screen_init(&source, 80, 24, 0);
	media_parse(&source, "\033]8;id=named;https://example.invalid/a\007N\033]8;;\007");
	ASSERT(media_unpack(&source, &sb, &un));
	screen_init(&live, 80, 24, 0);
	for (i = 0; i < TMTV_SNAPSHOT_MAX_LINKS; i++) {
		snprintf(sequence, sizeof sequence, "https://example.invalid/live/%u", i);
		u_int id = hyperlinks_put(live.hyperlinks, sequence, NULL);
		ASSERT(id != 0); if (i == 0) first = id;
	}
	p = tmtv_snapshot_pane_decode(un.data); ASSERT_NOT_NULL(p);
	ASSERT(hyperlinks_get(live.hyperlinks, first, &uri, NULL, NULL));
	ASSERT_STR_EQ(uri, "https://example.invalid/live/0");
	hyperlinks_snapshot_activate(p->hyperlinks);
	ASSERT(!hyperlinks_get(live.hyperlinks, first, &uri, NULL, NULL));
	ASSERT(hyperlinks_get(p->hyperlinks, 1, &uri, NULL, NULL));
	tmtv_snapshot_pane_free(p);
	ASSERT(hyperlinks_put(live.hyperlinks, "https://example.invalid/new", NULL));
	ASSERT(hyperlinks_get(live.hyperlinks, first + 1, &uri, NULL, NULL));
	ASSERT(hyperlinks_put(live.hyperlinks, "https://example.invalid/newer", NULL));
	ASSERT(!hyperlinks_get(live.hyperlinks, first + 1, &uri, NULL, NULL));
	screen_free(&source); screen_free(&live);
	msgpack_sbuffer_destroy(&sb); msgpack_unpacked_destroy(&un);
}

#ifdef ENABLE_SIXEL
TEST(media_snapshot_images_preserve_pixels_palette_and_saved_state)
{
	struct window_pane *wp = create_pane();
	struct screen source;
	struct tmtv_snapshot_pane *p;
	struct sixel_image *si;
	msgpack_sbuffer sb, left, right;
	msgpack_unpacked un;

	screen_init(&source, 80, 24, 0);
	si = sixel_parse("q#1;2;100;100;100~", strlen("q#1;2;100;100;100~"), 0, 8, 16);
	ASSERT_NOT_NULL(si); image_store(&source, si);
	screen_alternate_on(&source, (struct grid_cell *)&grid_default_cell, 0);
	si = sixel_parse("q#0;2;100;0;0~", strlen("q#0;2;100;0;0~"), 0, 8, 16); ASSERT_NOT_NULL(si);
	image_store(&source, si);
	ASSERT(!TAILQ_EMPTY(&source.images) && !TAILQ_EMPTY(&source.saved_images));
	ASSERT(media_unpack(&source, &sb, &un));
	p = tmtv_snapshot_pane_decode(un.data); ASSERT_NOT_NULL(p);
	p->id = wp->id; ASSERT_EQ(tmtv_snapshot_pane_apply(wp, p), 0);
	ASSERT(!TAILQ_EMPTY(&wp->base.images) && !TAILQ_EMPTY(&wp->base.saved_images));
	si = TAILQ_FIRST(&wp->base.images)->data;
	ASSERT_NOT_NULL(si); ASSERT_EQ(sixel_get_width(si), 1);
	ASSERT_EQ(sixel_get_height(si), 6);
	ASSERT_EQ(sixel_pixel_at(si, 0, 0), 1);
	ASSERT_EQ(sixel_get_colour(si, 0), (UINT32_C(2) << 25) | (100 << 16));
	si = TAILQ_FIRST(&wp->base.saved_images)->data;
	ASSERT_NOT_NULL(si); ASSERT_EQ(sixel_pixel_at(si, 0, 0), 2);
	msgpack_sbuffer_init(&left); msgpack_sbuffer_init(&right);
	ASSERT_EQ(tmtv_snapshot_pane_encode(wp->id, &source, 100, &left), 0);
	ASSERT_EQ(tmtv_snapshot_pane_encode(wp->id, &wp->base, 100, &right), 0);
	ASSERT_EQ(left.size, right.size);
	ASSERT(memcmp(left.data, right.data, left.size) == 0);
	tmtv_snapshot_pane_free(p); screen_free(&source); destroy_pane(wp);
	msgpack_sbuffer_destroy(&sb); msgpack_sbuffer_destroy(&left);
	msgpack_sbuffer_destroy(&right); msgpack_unpacked_destroy(&un);
}

TEST(media_snapshot_rejects_malformed_image_before_install)
{
	struct window_pane *wp = create_pane();
	struct screen source;
	struct tmtv_snapshot_pane *p;
	struct sixel_image *si;
	msgpack_sbuffer sb;
	msgpack_unpacked un;
	msgpack_object *v;

	screen_init(&source, 80, 24, 0);
	si = sixel_parse("q#1;2;100;100;100~", strlen("q#1;2;100;100;100~"), 0, 8, 16);
	ASSERT_NOT_NULL(si); image_store(&source, si);
	ASSERT(media_unpack(&source, &sb, &un));
	v = un.data.via.array.ptr;
	v[14].via.array.ptr[0].via.array.ptr[2].via.array.ptr[1].via.u64 = 0;
	ASSERT(tmtv_snapshot_pane_decode(un.data) == NULL);
	msgpack_unpack_next(&un, sb.data, sb.size, NULL);
	p = tmtv_snapshot_pane_decode(un.data); ASSERT_NOT_NULL(p);
	p->id = wp->id; ASSERT_EQ(tmtv_snapshot_pane_apply(wp, p), 0);
	ASSERT(!TAILQ_EMPTY(&wp->base.images));
	tmtv_snapshot_pane_free(p); screen_free(&source); destroy_pane(wp);
	msgpack_sbuffer_destroy(&sb); msgpack_unpacked_destroy(&un);
}
#endif

TEST(media_snapshot_frozen_mode_uses_its_own_link_table)
{
	struct window_pane *wp = create_pane();
	struct screen *back = xcalloc(1, sizeof *back);
	struct window_copy_state state = {0};
	struct grid_cell gc;
	const char *uri;
	media_parse(&wp->base, "\033]8;;https://example.invalid/live\007LIVE\033]8;;\007");
	screen_init(back, wp->sx, wp->sy, 2000);
	media_parse(back, "\033]8;;https://example.invalid/frozen\007FROZEN\033]8;;\007");
	state.view_mode = 1;
	window_copy_restore_snapshot(wp, back, &state);
	grid_get_cell(wp->screen->grid, 0, 0, &gc); ASSERT(gc.link != 0);
	ASSERT(hyperlinks_get(wp->screen->hyperlinks, gc.link, &uri, NULL, NULL));
	ASSERT_STR_EQ(uri, "https://example.invalid/frozen");
	destroy_pane(wp);
}

TEST(media_snapshot_aggregate_cache_bounds_are_explicit)
{
	struct screen source;
	msgpack_sbuffer sb;
	msgpack_unpacked un;
	u_int links, images, pixels;
	screen_init(&source, 80, 24, 0);
	media_parse(&source, "\033]8;id=n;https://example.invalid/a\007LINK\033]8;;\007");
	ASSERT(media_unpack(&source, &sb, &un));
	links = TMTV_SNAPSHOT_MAX_LINKS; images = pixels = 0;
	ASSERT(!tmtv_snapshot_media_budget(un.data, &links, &images, &pixels));
	links = 0; images = UINT_MAX; pixels = 0;
	ASSERT(!tmtv_snapshot_media_budget(un.data, &links, &images, &pixels));
	links = images = 0; pixels = UINT_MAX;
	ASSERT(!tmtv_snapshot_media_budget(un.data, &links, &images, &pixels));
	screen_free(&source); msgpack_sbuffer_destroy(&sb); msgpack_unpacked_destroy(&un);
}

#ifdef ENABLE_SIXEL
TEST(media_snapshot_image_fields_and_pixel_indices_are_bounded)
{
	struct screen source;
	struct sixel_image *si;
	msgpack_sbuffer sb;
	msgpack_unpacked un;
	msgpack_object *v, saved, *field;
	u_int i;
	screen_init(&source, 80, 24, 0);
	si = sixel_parse("q#0;2;100;0;0~", strlen("q#0;2;100;0;0~"), 1, 8, 16);
	ASSERT_NOT_NULL(si); image_store(&source, si);
	ASSERT(media_unpack(&source, &sb, &un));
	v = un.data.via.array.ptr[14].via.array.ptr[0].via.array.ptr[2].via.array.ptr;
	for (i = 0; i < 16; i++) {
		field = &v[i < 12 ? i : 11]; saved = *field;
		switch (i) {
		case 0: field->via.u64 = 2; break;
		case 1: field->via.u64 = 0; break;
		case 2: field->via.u64 = 10001; break;
		case 3: field->via.u64 = 0; break;
		case 4: field->via.u64 = 10001; break;
		case 5: field->type = MSGPACK_OBJECT_NEGATIVE_INTEGER; field->via.i64 = -1; break;
		case 6: field->via.u64 = 2; break;
		case 7: case 8: field->via.u64 = 10001; break;
		case 9: field->via.u64 = SIXEL_COLOUR_REGISTERS + 2; break;
		case 10: field->via.array.size = 2; break;
		case 11: field->via.array.size--; break;
		case 12: field = &v[11].via.array.ptr[0]; saved = *field;
			field->via.bin.size++; break;
		case 13: field = &v[11].via.array.ptr[0]; saved = *field;
			field->via.bin.ptr = "\377\377"; break;
		case 14: field = &v[10].via.array.ptr[0]; saved = *field;
			field->via.u64 = (2 << 25) | (101 << 16); break;
		case 15: field = &v[10].via.array.ptr[0]; saved = *field;
			field->via.u64 |= 1U << 24; break;
		}
		ASSERT(tmtv_snapshot_pane_decode(un.data) == NULL);
		*field = saved;
	}
	u_int links = 0, images = TMTV_SNAPSHOT_MAX_IMAGES, pixels = 0;
	ASSERT(!tmtv_snapshot_media_budget(un.data, &links, &images, &pixels));
	links = images = 0; pixels = TMTV_SNAPSHOT_MAX_PIXELS - 5;
	ASSERT(!tmtv_snapshot_media_budget(un.data, &links, &images, &pixels));
	screen_free(&source); msgpack_sbuffer_destroy(&sb); msgpack_unpacked_destroy(&un);
}
#endif

#ifdef ENABLE_SIXEL
TEST(media_snapshot_canonical_sixel_reserves_exact_buffer_terminator)
{
	char input[8400], *raw;
	const char *prefix = "q#0;2;100;0;0";
	struct sixel_image *si;
	size_t i, n, offset = strlen(prefix), size;
	int hit = 0;
	memcpy(input, prefix, offset);
	for (n = 8100; n < 8230; n++) {
		for (i = 0; i < n; i++) input[offset+i] = i % 2 ? '@' : '~';
		si = sixel_parse(input, offset+n, 0, 1, 6); ASSERT_NOT_NULL(si);
		raw = sixel_print(si, NULL, &size); ASSERT_NOT_NULL(raw);
		ASSERT_EQ(strlen(raw), size);
		if (size == 8192) hit = 1;
		free(raw); sixel_free(si);
	}
	ASSERT(hit);
}
#endif
