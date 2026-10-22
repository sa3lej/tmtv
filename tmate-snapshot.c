/* Shared host/relay text-grid codec; all validation precedes grid allocation. */
#include "tmux.h"
#include "tmate-snapshot.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define CELL_FLAGS (GRID_FLAG_PADDING|GRID_FLAG_SELECTED|GRID_FLAG_NOPALETTE|GRID_FLAG_CLEARED|GRID_FLAG_TAB)
#define LINE_FLAGS (GRID_LINE_WRAPPED|GRID_LINE_OSC133_FLAGS)

static int
number(msgpack_object o, u_int max, u_int *out)
{
	if (o.type != MSGPACK_OBJECT_POSITIVE_INTEGER || o.via.u64 > max)
		return (0);
	*out = o.via.u64;
	return (1);
}

static int
array(msgpack_object o, u_int n)
{
	return (o.type == MSGPACK_OBJECT_ARRAY && o.via.array.size == n);
}

int
tmtv_snapshot_colour(u_int c)
{
	if (c <= 9 || (c >= 90 && c <= 97)) return (1);
	if ((c & ~0xffU) == COLOUR_FLAG_256) return (1);
	if ((c & ~0xffffffU) == COLOUR_FLAG_RGB) return (1);
	return ((c & ~0xffU) == COLOUR_FLAG_THEME && (c & 0xff) < 10);
}

/* Validate scalar encoding without using the receiver's locale/width table.
 * A cell may contain several scalars (combining marks, variation selectors,
 * ZWJ emoji). Its width belongs to the host. Control bytes never enter a tty.
 */
int
tmtv_snapshot_text(const u_char *s, u_int n)
{
	u_int i = 0, c, more, min;
	while (i < n) {
		c = s[i++];
		if (c < 0x80) { more = 0; min = 0; }
		else if (c >= 0xc2 && c <= 0xdf) { c &= 0x1f; more = 1; min = 0x80; }
		else if (c >= 0xe0 && c <= 0xef) { c &= 0xf; more = 2; min = 0x800; }
		else if (c >= 0xf0 && c <= 0xf4) { c &= 7; more = 3; min = 0x10000; }
		else return (0);
		if (more > n - i) return (0);
		while (more--) {
			if ((s[i] & 0xc0) != 0x80) return (0);
			c = (c << 6) | (s[i++] & 0x3f);
		}
		if (c < min || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff) ||
		    c < 0x20 || (c >= 0x7f && c <= 0x9f)) return (0);
	}
	return (n != 0);
}

int
tmtv_snapshot_cell_decode(msgpack_object o, struct grid_cell *gc)
{
	msgpack_object *v;
	u_int width, flags, attr, fg, bg, us, size, link;
	if (!array(o, 8)) return (0);
	v = o.via.array.ptr;
	if (v[0].type != MSGPACK_OBJECT_BIN || v[0].via.bin.size > TMTV_SNAPSHOT_MAX_UTF8 ||
	    !number(v[1], UTF8_SIZE, &width) ||
	    !number(v[2], CELL_FLAGS, &flags) || (flags & ~CELL_FLAGS) ||
	    !number(v[3], 0x7fff, &attr) ||
	    !number(v[4], INT_MAX, &fg) || !tmtv_snapshot_colour(fg) ||
	    !number(v[5], INT_MAX, &bg) || !tmtv_snapshot_colour(bg) ||
	    !number(v[6], INT_MAX, &us) || !tmtv_snapshot_colour(us) ||
	    !number(v[7], INT_MAX, &link)) return (0);
	size = v[0].via.bin.size;
	if (flags & GRID_FLAG_PADDING) {
		if (width || size || (flags & GRID_FLAG_TAB)) return (0);
	} else if (flags & GRID_FLAG_TAB) {
		if (width == 0 || size) return (0);
	} else if (width == 0 || width > 2 ||
	    !tmtv_snapshot_text((const u_char *)v[0].via.bin.ptr, size)) return (0);
	memset(gc, 0, sizeof *gc);
	gc->flags = flags; gc->attr = attr; gc->link = link;
	gc->fg = fg; gc->bg = bg; gc->us = us;
	gc->data.width = width;
	gc->data.size = gc->data.have = size;
	if (size) memcpy(gc->data.data, v[0].via.bin.ptr, size);
	if (flags & GRID_FLAG_TAB) grid_set_tab(gc, width);
	return (1);
}

/* The cost includes grid_expand_line's minimum allocation at screen width. */
static int
validate(msgpack_object o, u_int *allocation)
{
	msgpack_object *v, *rows, *r, *cells, *osc;
	struct grid_cell gc;
	u_int version, sx, sy, history, scrolled, flags, used, lf, n, x, y, z;
	u_int cost = 0, pending, value, lines;
	if (!array(o, 7)) return (0);
	v = o.via.array.ptr;
	if (!number(v[0], TMTV_SNAPSHOT_GRID_VERSION, &version) ||
	    version != TMTV_SNAPSHOT_GRID_VERSION ||
	    !number(v[1], WINDOW_MAXIMUM, &sx) || sx == 0 ||
	    !number(v[2], WINDOW_MAXIMUM, &sy) || sy == 0 ||
	    !number(v[3], TMTV_SNAPSHOT_MAX_HISTORY, &history) ||
	    !number(v[4], history, &scrolled) ||
	    !number(v[5], GRID_HISTORY, &flags) ||
	    !array(v[6], sy + history)) return (0);
	rows = v[6].via.array.ptr;
	lines = sy + history;
	for (y = 0; y < lines; y++) {
		if (!array(rows[y], 4)) return (0);
		r = rows[y].via.array.ptr;
		if (!number(r[0], WINDOW_MAXIMUM, &used) ||
		    !number(r[1], LINE_FLAGS, &lf) || (lf & ~LINE_FLAGS) ||
		    !array(r[2], 5) || r[3].type != MSGPACK_OBJECT_ARRAY) return (0);
		osc = r[2].via.array.ptr;
		for (z = 0; z < 5; z++)
			if (!number(osc[z], z == 4 ? UCHAR_MAX : USHRT_MAX, &value)) return (0);
		n = r[3].via.array.size;
		if (n > WINDOW_MAXIMUM || used > n) return (0);
		if (n) {
			cost += n > sx ? n : sx;
			if (cost > TMTV_SNAPSHOT_MAX_CELLS) return (0);
		}
		cells = r[3].via.array.ptr;
		pending = 0;
		for (x = 0; x < n; x++) {
			if (!tmtv_snapshot_cell_decode(cells[x], &gc)) return (0);
			if (gc.flags & GRID_FLAG_PADDING) {
				if (pending == 0) return (0);
				pending--;
			} else {
				if (pending) return (0);
				pending = gc.data.width - 1;
				if (pending > n - x - 1) return (0);
			}
		}
		if (pending) return (0);
	}
	if (allocation != NULL) *allocation = cost;
	return (1);
}

int
tmtv_snapshot_grid_validate(msgpack_object o, u_int hlimit, u_int *allocation)
{
	return (hlimit <= TMTV_SNAPSHOT_MAX_HISTORY && validate(o, allocation) &&
	    o.via.array.ptr[3].via.u64 <= hlimit);
}

struct grid *
tmtv_snapshot_grid_decode(msgpack_object o, u_int hlimit)
{
	msgpack_object *v, *rows, *r, *osc;
	struct grid *gd;
	struct grid_line *gl;
	struct grid_cell gc;
	struct grid_cell_entry *gce;
	u_int x, y, lines;
	if (!tmtv_snapshot_grid_validate(o, hlimit, NULL)) return (NULL);
	v = o.via.array.ptr;
	if (v[3].via.u64 > hlimit) return (NULL);
	lines = v[2].via.u64 + v[3].via.u64;
	gd = grid_create(v[1].via.u64, lines, hlimit);
	gd->sy = v[2].via.u64; gd->hsize = v[3].via.u64;
	gd->hscrolled = v[4].via.u64; gd->flags = v[5].via.u64;
	rows = v[6].via.array.ptr;
	for (y = 0; y < lines; y++) {
		r = rows[y].via.array.ptr;
		for (x = 0; x < r[3].via.array.size; x++) {
			tmtv_snapshot_cell_decode(r[3].via.array.ptr[x], &gc);
			grid_set_cell(gd, x, y, &gc);
			/* grid_set_cell deliberately clears this storage marker. */
			if (gc.flags & GRID_FLAG_CLEARED) {
				gce = &gd->linedata[y].celldata[x];
				gce->flags |= GRID_FLAG_CLEARED;
				if (gce->flags & GRID_FLAG_EXTENDED)
					gd->linedata[y].extddata[gce->offset].flags |= GRID_FLAG_CLEARED;
			}
		}
		gl = &gd->linedata[y];
		gl->cellused = r[0].via.u64;
		gl->flags = (gl->flags & GRID_LINE_EXTENDED) | r[1].via.u64;
		osc = r[2].via.array.ptr;
		gl->osc133_data.prompt_col = osc[0].via.u64;
		gl->osc133_data.cmd_col = osc[1].via.u64;
		gl->osc133_data.out_start_col = osc[2].via.u64;
		gl->osc133_data.out_end_col = osc[3].via.u64;
		gl->osc133_data.exit_status = osc[4].via.u64;
	}
	return (gd);
}

/* Bounded MessagePack writer: never accumulate an oversized temporary frame. */
static int
write_bytes(void *data, const char *bytes, size_t length)
{
	msgpack_sbuffer *sb = data;
	if (length > TMTV_SNAPSHOT_MAX_BYTES - sb->size) return (-1);
	return (msgpack_sbuffer_write(sb, bytes, length));
}

int
tmtv_snapshot_grid_encode(struct grid *gd, u_int history, msgpack_sbuffer *out)
{
	msgpack_sbuffer sb;
	msgpack_packer pk;
	msgpack_unpacked parsed;
	struct grid_line *gl;
	struct grid_cell gc;
	u_int x, y, first, lines, n, cost = 0;
	int ok = 0;
	if (gd == NULL || !gd->sx || gd->sx > WINDOW_MAXIMUM || !gd->sy ||
	    gd->sy > WINDOW_MAXIMUM || history > TMTV_SNAPSHOT_MAX_HISTORY ||
	    gd->hsize > UINT_MAX - gd->sy || gd->hscrolled > gd->hsize ||
	    (gd->flags & ~GRID_HISTORY)) return (-1);
	if (history > gd->hsize) history = gd->hsize;
	first = gd->hsize - history; lines = gd->hsize + gd->sy;
	/* Check costs before encoding, including old-width history after resize. */
	for (y = first; y < lines; y++) {
		n = gd->linedata[y].cellsize;
		if (n > WINDOW_MAXIMUM) return (-1);
		if (n) cost += n > gd->sx ? n : gd->sx;
		if (cost > TMTV_SNAPSHOT_MAX_CELLS) return (-1);
	}
	msgpack_sbuffer_init(&sb);
	msgpack_packer_init(&pk, &sb, write_bytes);
#define PACK(type, value) do { if (msgpack_pack_##type(&pk, value)) goto done; } while (0)
	PACK(array, 7); PACK(unsigned_int, TMTV_SNAPSHOT_GRID_VERSION);
	PACK(unsigned_int, gd->sx); PACK(unsigned_int, gd->sy);
	PACK(unsigned_int, history);
	PACK(unsigned_int, gd->hscrolled < history ? gd->hscrolled : history);
	PACK(int, gd->flags); PACK(array, history + gd->sy);
	for (y = first; y < lines; y++) {
		gl = &gd->linedata[y];
		PACK(array, 4); PACK(unsigned_int, gl->cellused);
		PACK(unsigned_int, gl->flags & LINE_FLAGS); PACK(array, 5);
		PACK(unsigned_int, gl->osc133_data.prompt_col);
		PACK(unsigned_int, gl->osc133_data.cmd_col);
		PACK(unsigned_int, gl->osc133_data.out_start_col);
		PACK(unsigned_int, gl->osc133_data.out_end_col);
		PACK(unsigned_int, gl->osc133_data.exit_status);
		PACK(array, gl->cellsize);
		for (x = 0; x < gl->cellsize; x++) {
			grid_get_cell(gd, x, y, &gc);
			if (tmtv_snapshot_cell_encode(&gc, &pk) != 0) goto done;
		}
	}
	/* One validator defines both sides of the contract. No partial append. */
	msgpack_unpacked_init(&parsed);
	if (msgpack_unpack_next(&parsed, sb.data, sb.size, NULL) == MSGPACK_UNPACK_SUCCESS && validate(parsed.data, NULL) &&
	    out->size <= TMTV_SNAPSHOT_MAX_BYTES - sb.size &&
	    msgpack_sbuffer_write(out, sb.data, sb.size) == 0) ok = 1;
	msgpack_unpacked_destroy(&parsed);
done:
	msgpack_sbuffer_destroy(&sb);
	return (ok ? 0 : -1);
#undef PACK
}

/* Storage handles and allocation flags are normalized before packing. */
int
tmtv_snapshot_cell_encode(const struct grid_cell *gc, msgpack_packer *pk)
{
	u_int n = gc->flags & (GRID_FLAG_TAB|GRID_FLAG_PADDING) ? 0 : gc->data.size;
	if (gc->link > INT_MAX || n > TMTV_SNAPSHOT_MAX_UTF8) return (-1);
	if (msgpack_pack_array(pk, 8) || msgpack_pack_bin(pk, n) ||
	    (n && msgpack_pack_bin_body(pk, gc->data.data, n)) ||
	    msgpack_pack_unsigned_int(pk, gc->flags & GRID_FLAG_PADDING ? 0 : gc->data.width) ||
	    msgpack_pack_unsigned_int(pk, gc->flags & CELL_FLAGS) ||
	    msgpack_pack_unsigned_int(pk, gc->attr) ||
	    msgpack_pack_int(pk, gc->fg) || msgpack_pack_int(pk, gc->bg) ||
	    msgpack_pack_int(pk, gc->us) || msgpack_pack_unsigned_int(pk, gc->link)) return (-1);
	return (0);
}

/* Bound the complete session object before constructing any pane/parser plans.
 * Depth is fixed by this schema; unknown maps/extensions are never accepted. */
static int
snapshot_object_size(msgpack_object o, size_t *size, u_int depth, u_int *nodes)
{
	u_int i;
	uint64_t v;
	size_t n;
	if (depth > 12 || ++*nodes > TMTV_SNAPSHOT_MAX_CELLS * 10U + 100000) return (0);
	switch (o.type) {
	case MSGPACK_OBJECT_ARRAY:
		n = o.via.array.size < 16 ? 1 : o.via.array.size <= 65535 ? 3 : 5;
		if (n > TMTV_SNAPSHOT_MAX_BYTES - *size) return (0); *size += n;
		for (i = 0; i < o.via.array.size; i++)
			if (!snapshot_object_size(o.via.array.ptr[i], size, depth + 1, nodes)) return (0);
		return (1);
	case MSGPACK_OBJECT_BIN:
		n = (size_t)o.via.bin.size + (o.via.bin.size <= 255 ? 2U : o.via.bin.size <= 65535 ? 3U : 5U); break;
	case MSGPACK_OBJECT_NIL: n = 1; break;
	case MSGPACK_OBJECT_POSITIVE_INTEGER:
		v = o.via.u64; n = v <= 127 ? 1 : v <= 255 ? 2 : v <= 65535 ? 3 : v <= UINT32_MAX ? 5 : 9; break;
	case MSGPACK_OBJECT_NEGATIVE_INTEGER:
		n = o.via.i64 >= -32 ? 1 : o.via.i64 >= INT8_MIN ? 2 : o.via.i64 >= INT16_MIN ? 3 : o.via.i64 >= INT32_MIN ? 5 : 9; break;
	default: return (0);
	}
	if (n > TMTV_SNAPSHOT_MAX_BYTES - *size) return (0);
	*size += n; return (1);
}
int
tmtv_snapshot_object_bounded(msgpack_object o)
{
	size_t size = 0;
	u_int nodes = 0;
	return (snapshot_object_size(o, &size, 0, &nodes));
}
