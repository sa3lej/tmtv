/* Shared codec tested against full production relay/grid/UTF-8 objects. */
#include "../tmate-snapshot.h"

static struct grid *
snapshot_fixture(void)
{
	struct grid *gd = grid_create(12, 5, 2000);
	struct grid_cell gc = grid_default_cell;
	u_int x;
	gd->sy = 3; gd->hsize = gd->hscrolled = 2;
	utf8_set(&gc.data, 'H');
	grid_set_cell(gd, 0, 0, &gc);
	gc.attr = GRID_ATTR_STRIKETHROUGH|GRID_ATTR_OVERLINE|GRID_ATTR_UNDERSCORE_3;
	gc.fg = COLOUR_FLAG_RGB|0x123456;
	gc.bg = COLOUR_FLAG_256|234;
	gc.us = COLOUR_FLAG_RGB|0xabcdef;
	memset(&gc.data, 0, sizeof gc.data);
	memcpy(gc.data.data, "e\xcc\x81", 3);
	gc.data.have = gc.data.size = 3; gc.data.width = 1;
	grid_set_cell(gd, 0, 2, &gc);
	memset(&gc.data, 0, sizeof gc.data);
	memcpy(gc.data.data, "\xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x92\xbb", 11);
	gc.data.have = gc.data.size = 11; gc.data.width = 2;
	grid_set_cell(gd, 1, 2, &gc);
	grid_set_padding(gd, 2, 2, gc.bg);
	gc = grid_default_cell;
	gc.fg = COLOUR_FLAG_THEME|4;
	grid_set_tab(&gc, 6);
	grid_set_cell(gd, 0, 3, &gc);
	for (x = 1; x < 6; x++) grid_set_padding(gd, x, 3, 8);
	gd->linedata[2].flags |= GRID_LINE_WRAPPED|GRID_LINE_START_PROMPT;
	gd->linedata[2].osc133_data.prompt_col = 1;
	gd->linedata[2].osc133_data.cmd_col = 3;
	gd->linedata[2].osc133_data.out_start_col = 5;
	gd->linedata[2].osc133_data.out_end_col = USHRT_MAX;
	gd->linedata[2].osc133_data.exit_status = 17;
	return (gd);
}

static int
snapshot_unpack(struct grid *gd, u_int history, msgpack_sbuffer *sb,
    msgpack_unpacked *un)
{
	msgpack_sbuffer_init(sb);
	msgpack_unpacked_init(un);
	return (tmtv_snapshot_grid_encode(gd, history, sb) == 0 &&
	    msgpack_unpack_next(un, sb->data, sb->size, NULL) == MSGPACK_UNPACK_SUCCESS);
}

TEST(snapshot_grid_roundtrip_preserves_text_and_styles)
{
	struct grid *source = snapshot_fixture(), *gd;
	struct grid_cell a, b;
	msgpack_sbuffer sb;
	msgpack_unpacked un;
	u_int x, y;
	ASSERT(snapshot_unpack(source, 100, &sb, &un));
	gd = tmtv_snapshot_grid_decode(un.data, 2000);
	ASSERT_NOT_NULL(gd);
	ASSERT_EQ(gd->sx, 12); ASSERT_EQ(gd->sy, 3);
	ASSERT_EQ(gd->hsize, 2); ASSERT_EQ(gd->hscrolled, 2);
	for (y = 0; y < 5; y++) {
		ASSERT_EQ(gd->linedata[y].cellused, source->linedata[y].cellused);
		ASSERT_EQ(gd->linedata[y].flags & ~GRID_LINE_EXTENDED,
		    source->linedata[y].flags & ~GRID_LINE_EXTENDED);
		ASSERT(memcmp(&gd->linedata[y].osc133_data,
		    &source->linedata[y].osc133_data, sizeof(struct osc133_data)) == 0);
		for (x = 0; x < source->linedata[y].cellsize; x++) {
			grid_get_cell(source, x, y, &a); grid_get_cell(gd, x, y, &b);
			ASSERT_EQ(a.attr, b.attr); ASSERT_EQ(a.fg, b.fg);
			ASSERT_EQ(a.bg, b.bg); ASSERT_EQ(a.us, b.us);
			ASSERT_EQ(a.flags & ~GRID_FLAG_EXTENDED, b.flags & ~GRID_FLAG_EXTENDED);
			if (!(a.flags & GRID_FLAG_PADDING)) {
				ASSERT_EQ(a.data.width, b.data.width);
				ASSERT_EQ(a.data.size, b.data.size);
				ASSERT(memcmp(a.data.data, b.data.data, a.data.size) == 0);
			}
		}
	}
	grid_destroy(gd); grid_destroy(source);
	msgpack_unpacked_destroy(&un); msgpack_sbuffer_destroy(&sb);
}

TEST(snapshot_history_is_explicitly_bounded)
{
	struct grid *source = snapshot_fixture(), *gd;
	msgpack_sbuffer sb;
	msgpack_unpacked un;
	ASSERT(snapshot_unpack(source, 1, &sb, &un));
	gd = tmtv_snapshot_grid_decode(un.data, 2000);
	ASSERT_NOT_NULL(gd);
	ASSERT_EQ(gd->hsize, 1); ASSERT_EQ(gd->hscrolled, 1);
	ASSERT_EQ(gd->linedata[0].cellsize, 0);
	grid_destroy(gd);
	ASSERT(tmtv_snapshot_grid_decode(un.data, 0) == NULL);
	grid_destroy(source); msgpack_unpacked_destroy(&un); msgpack_sbuffer_destroy(&sb);
}

TEST(snapshot_old_width_rows_survive_resize)
{
	struct grid *source = grid_create(12, 2, 2000), *gd;
	msgpack_sbuffer sb;
	msgpack_unpacked un;
	grid_set_cell(source, 11, 0, &grid_default_cell);
	source->sx = 4;
	ASSERT(snapshot_unpack(source, 0, &sb, &un));
	gd = tmtv_snapshot_grid_decode(un.data, 2000);
	ASSERT_NOT_NULL(gd);
	ASSERT_EQ(gd->sx, 4); ASSERT_EQ(gd->linedata[0].cellused, 12);
	ASSERT_EQ(gd->linedata[0].cellsize, 12);
	grid_destroy(gd); grid_destroy(source);
	msgpack_unpacked_destroy(&un); msgpack_sbuffer_destroy(&sb);
}

/* Mutate independently valid objects so each check exercises its named bound. */
static void
snapshot_bad(int which)
{
	struct grid *source = snapshot_fixture(), *gd;
	msgpack_sbuffer sb;
	msgpack_unpacked un;
	msgpack_object *v, *row, *c, *pad;
	char bad[40];
	ASSERT(snapshot_unpack(source, 100, &sb, &un));
	v = un.data.via.array.ptr;
	row = v[6].via.array.ptr[2].via.array.ptr;
	c = row[3].via.array.ptr[0].via.array.ptr;
	pad = row[3].via.array.ptr[2].via.array.ptr;
	switch (which) {
	case 0: v[0].via.u64 = TMTV_SNAPSHOT_GRID_VERSION + 1; break;
	case 1: v[1].via.u64 = 0; break;
	case 2: v[2].via.u64 = UINT64_MAX; break;
	case 3: v[3].via.u64 = 2001; break;
	case 4: v[4].via.u64 = 3; break;
	case 5: v[5].via.u64 = 2; break;
	case 6: v[6].via.array.size--; break;
	case 7: row[0].via.u64 = 13; break;
	case 8: row[1].via.u64 |= GRID_LINE_EXTENDED; break;
	case 9: row[2].via.array.ptr[4].via.u64 = 256; break;
	case 10: c[0].type = MSGPACK_OBJECT_STR; break;
	case 11: memset(bad, 'x', sizeof bad); c[0].via.bin.ptr = bad; c[0].via.bin.size = 32; break;
	case 12: c[1].via.u64 = 3; break;
	case 13: c[2].via.u64 = GRID_FLAG_EXTENDED; break;
	case 14: c[3].via.u64 = 0x10000; break;
	case 15: c[4].via.u64 = COLOUR_FLAG_RGB|COLOUR_FLAG_256|1; break;
	case 16: c[5].type = MSGPACK_OBJECT_NEGATIVE_INTEGER; c[5].via.i64 = -1; break;
	case 17: c[6].via.u64 = COLOUR_FLAG_THEME|10; break;
	case 18: pad[2].via.u64 = 0; break; /* missing wide-cell padding */
	case 19: c[2].via.u64 = GRID_FLAG_PADDING; c[0].via.bin.size = 0; c[1].via.u64 = 0; break;
	case 20: c[0].via.bin.ptr = "\xc0\xaf"; c[0].via.bin.size = 2; break;
	case 21: c[0].via.bin.ptr = "\xed\xa0\x80"; c[0].via.bin.size = 3; break;
	case 22: c[0].via.bin.ptr = "\xf4\x90\x80\x80"; c[0].via.bin.size = 4; break;
	case 23: c[0].via.bin.ptr = "\xe2\x80"; c[0].via.bin.size = 2; break;
	case 24: c[0].via.bin.ptr = "\033"; c[0].via.bin.size = 1; break;
	case 25: c[0].via.bin.ptr = "\xc2\x9b"; c[0].via.bin.size = 2; break;
	case 26: c[0].via.bin.ptr = "\177"; c[0].via.bin.size = 1; break;
	case 27: c[0].via.bin.ptr = ""; c[0].via.bin.size = 1; break;
	case 28: c[0].via.bin.size = 0; break;
	case 29: row[3].via.array.ptr[1].via.array.ptr[1].via.u64 = 1; break;
	}
	gd = tmtv_snapshot_grid_decode(un.data, 2000);
	ASSERT(gd == NULL);
	/* The caller's source and its Unicode/style state remain untouched. */
	ASSERT_EQ(source->linedata[2].cellused, 3);
	grid_destroy(source); msgpack_unpacked_destroy(&un); msgpack_sbuffer_destroy(&sb);
}
#define BAD_TEST(name, index) TEST(snapshot_rejects_##name) { snapshot_bad(index); }
BAD_TEST(version, 0)
BAD_TEST(zero_width, 1)
BAD_TEST(overflow_height, 2)
BAD_TEST(history_overflow, 3)
BAD_TEST(scrolled_overflow, 4)
BAD_TEST(grid_flags, 5)
BAD_TEST(row_count, 6)
BAD_TEST(used_overflow, 7)
BAD_TEST(storage_flags, 8)
BAD_TEST(osc_overflow, 9)
BAD_TEST(nonbinary_cell, 10)
BAD_TEST(oversized_cell, 11)
BAD_TEST(unsupported_width, 12)
BAD_TEST(cell_storage_flags, 13)
BAD_TEST(attribute_overflow, 14)
BAD_TEST(mixed_colour_types, 15)
BAD_TEST(negative_colour, 16)
BAD_TEST(unknown_theme_slot, 17)
BAD_TEST(missing_padding, 18)
BAD_TEST(orphan_padding, 19)
BAD_TEST(overlong_utf8, 20)
BAD_TEST(surrogate, 21)
BAD_TEST(out_of_range_scalar, 22)
BAD_TEST(truncated_utf8, 23)
BAD_TEST(escape, 24)
BAD_TEST(c1_control, 25)
BAD_TEST(del, 26)
BAD_TEST(nul, 27)
BAD_TEST(empty_text, 28)
BAD_TEST(extra_padding, 29)
#undef BAD_TEST

TEST(snapshot_cost_bounds_both_encoder_and_decoder)
{
	struct grid *source = grid_create(10000, 27, 0);
	msgpack_sbuffer sb;
	msgpack_unpacked un;
	msgpack_object *v, rows[27];
	u_int y;
	msgpack_sbuffer_init(&sb);
	for (y = 0; y < 27; y++) grid_set_cell(source, 0, y, &grid_default_cell);
	ASSERT_EQ(tmtv_snapshot_grid_encode(source, 0, &sb), -1);
	ASSERT_EQ(sb.size, 0);
	grid_destroy(source);
	source = grid_create(1, 1, 0);
	grid_set_cell(source, 0, 0, &grid_default_cell);
	msgpack_sbuffer_destroy(&sb);
	ASSERT(snapshot_unpack(source, 0, &sb, &un));
	v = un.data.via.array.ptr;
	for (y = 0; y < 27; y++) rows[y] = v[6].via.array.ptr[0];
	v[1].via.u64 = 10000; v[2].via.u64 = 27;
	v[6].via.array.ptr = rows; v[6].via.array.size = 27;
	ASSERT(tmtv_snapshot_grid_decode(un.data, 0) == NULL);
	grid_destroy(source); msgpack_unpacked_destroy(&un); msgpack_sbuffer_destroy(&sb);
}

TEST(snapshot_rejected_encode_preserves_destination)
{
	struct grid *source = grid_create(2, 1, 0);
	struct grid_cell gc = grid_default_cell;
	msgpack_sbuffer sb;
	msgpack_sbuffer_init(&sb);
	msgpack_sbuffer_write(&sb, "KEEP", 4);
	gc.link = UINT_MAX; grid_set_cell(source, 0, 0, &gc);
	ASSERT_EQ(tmtv_snapshot_grid_encode(source, 0, &sb), -1);
	ASSERT_EQ(sb.size, 4); ASSERT(memcmp(sb.data, "KEEP", 4) == 0);
	gc.link = 0; utf8_set(&gc.data, '\033'); grid_set_cell(source, 0, 0, &gc);
	ASSERT_EQ(tmtv_snapshot_grid_encode(source, 0, &sb), -1);
	ASSERT_EQ(sb.size, 4);
	grid_destroy(source); msgpack_sbuffer_destroy(&sb);
}

#include "snapshot-parser-fixture.h"
TEST(host_grid_codec_stream_roundtrip)
{
	msgpack_unpacker uk;
	msgpack_unpacked un;
	struct grid *gd;
	size_t n, chunk = snapshot_bulk ? 4096 : 1;
	int count = 0;
	msgpack_unpacker_init(&uk, 128);
	msgpack_unpacked_init(&un);
	while (1) {
		ASSERT(msgpack_unpacker_reserve_buffer(&uk, chunk));
		n = fread(msgpack_unpacker_buffer(&uk), 1, chunk, stdin);
		if (!n) break;
		msgpack_unpacker_buffer_consumed(&uk, n);
		while (msgpack_unpacker_next(&uk, &un) == MSGPACK_UNPACK_SUCCESS) {
			gd = tmtv_snapshot_grid_decode(un.data, 2000);
			ASSERT(snapshot_parser_expected(gd));
			grid_destroy(gd); count++;
		}
	}
	ASSERT_EQ(count, 1);
	ASSERT_EQ(msgpack_unpacker_message_size(&uk), 0);
	msgpack_unpacked_destroy(&un); msgpack_unpacker_destroy(&uk);
}

TEST(snapshot_preserves_host_width_and_full_style_bits)
{
	struct grid *source = grid_create(4, 1, 0), *gd;
	struct grid_cell gc = grid_default_cell;
	msgpack_sbuffer sb;
	msgpack_unpacked un;
	memcpy(gc.data.data, "\xe2\x94\x80", 3); /* host width override */
	gc.data.have = gc.data.size = 3; gc.data.width = 2;
	gc.attr = 0x7fff; gc.flags = GRID_FLAG_SELECTED|GRID_FLAG_NOPALETTE;
	gc.fg = COLOUR_FLAG_256|255; gc.bg = COLOUR_FLAG_RGB|0xffffff;
	gc.us = COLOUR_FLAG_256|0;
	grid_set_cell(source, 0, 0, &gc); grid_set_padding(source, 1, 0, gc.bg);
	ASSERT(snapshot_unpack(source, 0, &sb, &un));
	gd = tmtv_snapshot_grid_decode(un.data, 0);
	ASSERT_NOT_NULL(gd); grid_get_cell(gd, 0, 0, &gc);
	ASSERT_EQ(gc.data.width, 2); ASSERT_EQ(gc.attr, 0x7fff);
	ASSERT_EQ(gc.flags, GRID_FLAG_SELECTED|GRID_FLAG_NOPALETTE);
	ASSERT_EQ(gc.fg, COLOUR_FLAG_256|255); ASSERT_EQ(gc.bg, COLOUR_FLAG_RGB|0xffffff);
	ASSERT_EQ(gc.us, COLOUR_FLAG_256);
	grid_destroy(source); grid_destroy(gd);
	msgpack_unpacked_destroy(&un); msgpack_sbuffer_destroy(&sb);
}

TEST(snapshot_output_limit_is_atomic)
{
	struct grid *source = grid_create(1, 1, 0);
	msgpack_sbuffer sb;
	size_t remaining = TMTV_SNAPSHOT_MAX_BYTES - 1;
	char block[4096];
	msgpack_sbuffer_init(&sb); memset(block, 'K', sizeof block);
	while (remaining) {
		size_t n = remaining < sizeof block ? remaining : sizeof block;
		ASSERT_EQ(msgpack_sbuffer_write(&sb, block, n), 0); remaining -= n;
	}
	ASSERT_EQ(tmtv_snapshot_grid_encode(source, 0, &sb), -1);
	ASSERT_EQ(sb.size, TMTV_SNAPSHOT_MAX_BYTES - 1);
	ASSERT_EQ(sb.data[0], 'K'); ASSERT_EQ(sb.data[sb.size - 1], 'K');
	grid_destroy(source); msgpack_sbuffer_destroy(&sb);
}

TEST(snapshot_history_limit_is_atomic)
{
	struct grid *source = snapshot_fixture();
	msgpack_sbuffer sb;
	msgpack_sbuffer_init(&sb);
	ASSERT_EQ(tmtv_snapshot_grid_encode(source, 2001, &sb), -1);
	ASSERT_EQ(sb.size, 0);
	grid_destroy(source); msgpack_sbuffer_destroy(&sb);
}

TEST(snapshot_maximum_combined_cell_roundtrips)
{
	struct grid *source = grid_create(1, 1, 0), *gd;
	struct grid_cell gc = grid_default_cell, decoded;
	msgpack_sbuffer sb;
	msgpack_unpacked un;
	u_int x;
	memset(&gc.data, 0, sizeof gc.data); gc.data.data[0] = 'a';
	for (x = 1; x < 31; x += 2) {
		gc.data.data[x] = 0xcc; gc.data.data[x + 1] = 0x81;
	}
	gc.data.have = gc.data.size = 31; gc.data.width = 1;
	grid_set_cell(source, 0, 0, &gc);
	ASSERT(snapshot_unpack(source, 0, &sb, &un));
	gd = tmtv_snapshot_grid_decode(un.data, 0);
	ASSERT_NOT_NULL(gd); grid_get_cell(gd, 0, 0, &decoded);
	ASSERT_EQ(decoded.data.size, 31);
	ASSERT(memcmp(decoded.data.data, gc.data.data, 31) == 0);
	grid_destroy(source); grid_destroy(gd);
	msgpack_unpacked_destroy(&un); msgpack_sbuffer_destroy(&sb);
}

/* Collect only actual parsed nodes. Mutating raw MessagePack length headers can
 * allocate in the third-party parser before the grid codec receives an object.
 */
static void
snapshot_nodes(msgpack_object *o, msgpack_object **nodes, u_int *count)
{
	u_int i;
	if (*count == 512) abort();
	nodes[(*count)++] = o;
	if (o->type == MSGPACK_OBJECT_ARRAY)
		for (i = 0; i < o->via.array.size; i++)
			snapshot_nodes(&o->via.array.ptr[i], nodes, count);
}

TEST(snapshot_mutated_objects_remain_bounded_and_reencodable)
{
	static const uint64_t masks[] = { 1, 128, 65536, (uint64_t)1 << 32, (uint64_t)1 << 63 };
	struct grid *source = snapshot_fixture(), *gd;
	msgpack_sbuffer sb, canonical;
	msgpack_unpacked un;
	msgpack_object *nodes[512], saved;
	u_int i, j, count = 0;
	ASSERT(snapshot_unpack(source, 100, &sb, &un));
	snapshot_nodes(&un.data, nodes, &count);
	for (i = 0; i < count; i++) {
		saved = *nodes[i];
		for (j = 0; j < nitems(masks); j++) {
			*nodes[i] = saved;
			if (saved.type == MSGPACK_OBJECT_POSITIVE_INTEGER)
				nodes[i]->via.u64 ^= masks[j];
			else nodes[i]->type = MSGPACK_OBJECT_NIL;
			gd = tmtv_snapshot_grid_decode(un.data, 2000);
			if (gd != NULL) {
				msgpack_sbuffer_init(&canonical);
				ASSERT_EQ(tmtv_snapshot_grid_encode(gd, gd->hsize, &canonical), 0);
				grid_destroy(gd); msgpack_sbuffer_destroy(&canonical);
			}
		}
		*nodes[i] = saved;
	}
	grid_destroy(source); msgpack_unpacked_destroy(&un); msgpack_sbuffer_destroy(&sb);
}
