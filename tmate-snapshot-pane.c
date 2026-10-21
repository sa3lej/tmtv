/* Versioned pane screen state, staged separately from transport negotiation. */
#include "tmux.h"
#include "tmate-snapshot.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define SCREEN_MODES 0x1fffff

static int
array(msgpack_object o, u_int size)
{
	return (o.type == MSGPACK_OBJECT_ARRAY && o.via.array.size == size);
}

static int
number(msgpack_object o, u_int max, u_int *out)
{
	if (o.type != MSGPACK_OBJECT_POSITIVE_INTEGER || o.via.u64 > max) return (0);
	*out = o.via.u64;
	return (1);
}

static int
cursor_colour(msgpack_object o, int *out)
{
	u_int value;
	if (o.type == MSGPACK_OBJECT_NEGATIVE_INTEGER && o.via.i64 == -1) {
		*out = -1;
		return (1);
	}
	if (!number(o, INT_MAX, &value) || !tmtv_snapshot_colour(value)) return (0);
	*out = value;
	return (1);
}

/* Validate both grids and all metadata before any grid allocation or interning.
 * The aggregate cell budget covers current AND saved grids.
 */
static int
validate(msgpack_object o, struct tmtv_snapshot_pane *p)
{
	msgpack_object *v, *cursor, *modes, *style, *region, *saved, *tabs, *g;
	u_int version, mode, default_mode, cstyle, default_cstyle, saved_flags;
	u_int sx, sy, saved_sx, saved_sy, cost, saved_cost = 0, i, tab, last = 0;
	if (!array(o, 12)) return (0);
	v = o.via.array.ptr;
	if (!number(v[0], TMTV_SNAPSHOT_PANE_VERSION, &version) ||
	    version != TMTV_SNAPSHOT_PANE_VERSION || !number(v[1], INT_MAX, &p->id) ||
	    !tmtv_snapshot_grid_validate(v[2], TMTV_SNAPSHOT_MAX_HISTORY, &cost) ||
	    !array(v[3], 2) || !array(v[4], 2) || !array(v[5], 4) ||
	    !array(v[6], 2) || v[7].type != MSGPACK_OBJECT_ARRAY || !array(v[9], 2))
		return (0);
	g = v[2].via.array.ptr; sx = g[1].via.u64; sy = g[2].via.u64;
	cursor = v[3].via.array.ptr; modes = v[4].via.array.ptr;
	style = v[5].via.array.ptr; region = v[6].via.array.ptr;
	saved = v[9].via.array.ptr;
	if (!number(cursor[0], sx, &p->cx) || !number(cursor[1], sy - 1, &p->cy) ||
	    !number(modes[0], SCREEN_MODES, &mode) ||
	    !number(modes[1], SCREEN_MODES, &default_mode) ||
	    !number(style[0], SCREEN_CURSOR_BAR, &cstyle) ||
	    !number(style[1], SCREEN_CURSOR_BAR, &default_cstyle) ||
	    !cursor_colour(style[2], &p->ccolour) ||
	    !cursor_colour(style[3], &p->default_ccolour) ||
	    !number(region[0], sy - 1, &p->rupper) ||
	    !number(region[1], sy - 1, &p->rlower) || p->rupper > p->rlower ||
	    !number(saved[0], UINT_MAX, &p->saved_cx) ||
	    !number(saved[1], UINT_MAX, &p->saved_cy) ||
	    !number(v[11], GRID_HISTORY, &saved_flags)) return (0);
	p->mode = mode; p->default_mode = default_mode;
	p->cstyle = cstyle; p->default_cstyle = default_cstyle;
	p->saved_flags = saved_flags;
	saved_sx = saved_sy = WINDOW_MAXIMUM;
	if (v[8].type != MSGPACK_OBJECT_NIL) {
		if (!tmtv_snapshot_grid_validate(v[8], 0, &saved_cost) ||
		    v[8].via.array.ptr[5].via.u64 != 0 || (g[5].via.u64 & GRID_HISTORY)) return (0);
		saved_sx = v[8].via.array.ptr[1].via.u64;
		saved_sy = v[8].via.array.ptr[2].via.u64;
	} else if (saved_flags != 0) return (0);
	if (cost + saved_cost > TMTV_SNAPSHOT_MAX_CELLS) return (0);
	/* A pending-wrap cursor may be at sx. UINT_MAX is a paired sentinel. */
	if (p->saved_cx == UINT_MAX || p->saved_cy == UINT_MAX) {
		if (p->saved_cx != UINT_MAX || p->saved_cy != UINT_MAX) return (0);
	} else if (p->saved_cx > saved_sx || p->saved_cy >= saved_sy) return (0);
	p->saved_cell = grid_default_cell;
	if (v[10].type != MSGPACK_OBJECT_NIL) {
		if (!tmtv_snapshot_cell_decode(v[10], &p->saved_cell) ||
		    (p->saved_cell.flags & (GRID_FLAG_PADDING|GRID_FLAG_TAB))) return (0);
	} else if (v[8].type != MSGPACK_OBJECT_NIL || p->saved_cx != UINT_MAX) return (0);
	if (v[7].via.array.size > sx) return (0);
	tabs = v[7].via.array.ptr;
	for (i = 0; i < v[7].via.array.size; i++) {
		if (!number(tabs[i], sx - 1, &tab) || (i && tab <= last)) return (0);
		last = tab;
	}
	return (1);
}

struct tmtv_snapshot_pane *
tmtv_snapshot_pane_decode(msgpack_object o)
{
	struct tmtv_snapshot_pane values, *p;
	msgpack_object *v;
	u_int i, sx;
	memset(&values, 0, sizeof values);
	if (!validate(o, &values)) return (NULL);
	p = xmalloc(sizeof *p); *p = values;
	v = o.via.array.ptr;
	p->grid = tmtv_snapshot_grid_decode(v[2], TMTV_SNAPSHOT_MAX_HISTORY);
	if (v[8].type != MSGPACK_OBJECT_NIL)
		p->saved_grid = tmtv_snapshot_grid_decode(v[8], 0);
	sx = p->grid->sx;
	if ((p->tabs = bit_alloc(sx)) == NULL) fatal("bit_alloc failed");
	for (i = 0; i < v[7].via.array.size; i++)
		bit_set(p->tabs, v[7].via.array.ptr[i].via.u64);
	return (p);
}

void
tmtv_snapshot_pane_free(struct tmtv_snapshot_pane *p)
{
	if (p == NULL) return;
	if (p->grid != NULL) grid_destroy(p->grid);
	if (p->saved_grid != NULL) grid_destroy(p->saved_grid);
	free(p->tabs); free(p);
}

static int
write_bytes(void *data, const char *bytes, size_t length)
{
	msgpack_sbuffer *sb = data;
	if (length > TMTV_SNAPSHOT_MAX_BYTES - sb->size) return (-1);
	return (msgpack_sbuffer_write(sb, bytes, length));
}

int
tmtv_snapshot_pane_encode(u_int id, const struct screen *s, u_int history,
    msgpack_sbuffer *out)
{
	msgpack_sbuffer sb;
	msgpack_packer pk;
	msgpack_unpacked un;
	struct tmtv_snapshot_pane values;
	u_int i, count = 0;
	int ok = 0;
	if (s == NULL || s->grid == NULL || id > INT_MAX) return (-1);
#ifdef ENABLE_SIXEL
	/* Explicitly unsupported, never silently drop active or saved images. */
	if (!TAILQ_EMPTY(&s->images) || !TAILQ_EMPTY(&s->saved_images)) return (-1);
#endif
	if (s->grid->sx == 0 || s->grid->sx > WINDOW_MAXIMUM) return (-1);
	if (s->tabs != NULL)
		for (i = 0; i < s->grid->sx; i++) if (bit_test(s->tabs, i)) count++;
	msgpack_sbuffer_init(&sb); msgpack_packer_init(&pk, &sb, write_bytes);
#define PACK(type, value) do { if (msgpack_pack_##type(&pk, value)) goto done; } while (0)
	PACK(array, 12); PACK(unsigned_int, TMTV_SNAPSHOT_PANE_VERSION); PACK(unsigned_int, id);
	if (tmtv_snapshot_grid_encode(s->grid, history, &sb)) goto done;
	PACK(array, 2); PACK(unsigned_int, s->cx); PACK(unsigned_int, s->cy);
	PACK(array, 2); PACK(int, s->mode); PACK(int, s->default_mode);
	PACK(array, 4); PACK(int, s->cstyle); PACK(int, s->default_cstyle);
	PACK(int, s->ccolour); PACK(int, s->default_ccolour);
	PACK(array, 2); PACK(unsigned_int, s->rupper); PACK(unsigned_int, s->rlower);
	PACK(array, count);
	if (s->tabs != NULL)
		for (i = 0; i < s->grid->sx; i++) if (bit_test(s->tabs, i)) PACK(unsigned_int, i);
	if (s->saved_grid == NULL) {
		if (msgpack_pack_nil(&pk)) goto done;
	} else if (tmtv_snapshot_grid_encode(s->saved_grid, 0, &sb)) goto done;
	PACK(array, 2); PACK(unsigned_int, s->saved_cx); PACK(unsigned_int, s->saved_cy);
	if (s->saved_grid != NULL || s->saved_cx != UINT_MAX) {
		if (tmtv_snapshot_cell_encode(&s->saved_cell, &pk)) goto done;
	} else if (msgpack_pack_nil(&pk)) goto done;
	PACK(int, s->saved_grid == NULL ? 0 : s->saved_flags);
	msgpack_unpacked_init(&un);
	memset(&values, 0, sizeof values);
	if (msgpack_unpack_next(&un, sb.data, sb.size, NULL) == MSGPACK_UNPACK_SUCCESS &&
	    validate(un.data, &values) && out->size <= TMTV_SNAPSHOT_MAX_BYTES - sb.size &&
	    msgpack_sbuffer_write(out, sb.data, sb.size) == 0) ok = 1;
	msgpack_unpacked_destroy(&un);
done:
	msgpack_sbuffer_destroy(&sb);
	return (ok ? 0 : -1);
#undef PACK
}

int
tmtv_snapshot_pane_apply(struct window_pane *wp, struct tmtv_snapshot_pane *p)
{
	struct screen *s;
#ifdef ENABLE_SIXEL
	struct image *im;
#endif
	if (wp == NULL || p == NULL || p->grid == NULL || p->id != wp->id ||
	    p->grid->sx != wp->sx || p->grid->sy != wp->sy) return (-1);
	s = &wp->base;
	/* Only after identity/geometry checks: discard queued writes to old grids. */
	if (event_initialized(&wp->sync_timer)) evtimer_del(&wp->sync_timer);
	free(wp->sync_dirty); wp->sync_dirty = NULL; wp->sync_dirty_size = 0;
	if (s->write_list != NULL) { screen_write_free_list(s); s->write_list = NULL; }
	screen_clear_selection(s);
#ifdef ENABLE_SIXEL
	TAILQ_CONCAT(&s->images, &s->saved_images, entry);
	TAILQ_FOREACH(im, &s->images, entry) im->list = &s->images;
	image_free_all(s);
#endif
	grid_destroy(s->grid);
	if (s->saved_grid != NULL) grid_destroy(s->saved_grid);
	s->grid = p->grid; s->saved_grid = p->saved_grid;
	free(s->tabs); s->tabs = p->tabs;
	p->grid = p->saved_grid = NULL; p->tabs = NULL;
	s->cx = p->cx; s->cy = p->cy;
	s->mode = p->mode; s->default_mode = p->default_mode;
	s->cstyle = p->cstyle; s->default_cstyle = p->default_cstyle;
	s->ccolour = p->ccolour; s->default_ccolour = p->default_ccolour;
	s->rupper = p->rupper; s->rlower = p->rlower;
	s->saved_cx = p->saved_cx; s->saved_cy = p->saved_cy;
	s->saved_cell = p->saved_cell; s->saved_flags = p->saved_flags;
	if (s->mode & MODE_SYNC) screen_write_start_sync(wp);
	/* Mode screens and their cloned copy/view backing are separate owners. */
	wp->flags |= PANE_REDRAW;
	return (0);
}
