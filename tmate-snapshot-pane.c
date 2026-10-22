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

/* The grid contains table-local reference numbers, never receiver handles. */
static int
grid_links(msgpack_object grid, msgpack_object links)
{
	msgpack_object *rows = grid.via.array.ptr[6].via.array.ptr, *cells;
	u_int y, x, n = grid.via.array.ptr[6].via.array.size;
	for (y = 0; y < n; y++) {
		cells = rows[y].via.array.ptr[3].via.array.ptr;
		for (x = 0; x < rows[y].via.array.ptr[3].via.array.size; x++)
			if (!hyperlinks_snapshot_has(links, cells[x].via.array.ptr[7].via.u64)) return (0);
	}
	return (1);
}

/* Inputs have already passed pane validation. Session totals also cover saved
 * screens and frozen mode backings, preventing global-cache eviction of any
 * newly restored image/link from the same frame. */
int
tmtv_snapshot_media_budget(msgpack_object pane, u_int *links, u_int *images, u_int *pixels)
{
	msgpack_object *v = pane.via.array.ptr;
	u_int i, j, cost;
	if (*links > TMTV_SNAPSHOT_MAX_LINKS || *images > TMTV_SNAPSHOT_MAX_IMAGES ||
	    *pixels > TMTV_SNAPSHOT_MAX_PIXELS) return (0);
	*links += v[13].via.array.size;
	for (i = 14; i < 16; i++) {
		*images += v[i].via.array.size;
#ifdef ENABLE_SIXEL
		for (j = 0; j < v[i].via.array.size; j++) {
			if (!sixel_snapshot_validate(v[i].via.array.ptr[j].via.array.ptr[2], &cost)) return (0);
			if (cost > TMTV_SNAPSHOT_MAX_PIXELS - *pixels) return (0);
			*pixels += cost;
		}
#else
		(void)j; (void)cost;
#endif
	}
	return (*links <= TMTV_SNAPSHOT_MAX_LINKS && *images <= TMTV_SNAPSHOT_MAX_IMAGES);
}

static int
media_validate(msgpack_object *v)
{
	u_int i, j, px, py, links = 0, images = 0, pixels = 0;
	msgpack_object *im, pane;
	if (!hyperlinks_snapshot_validate(v[13]) || !grid_links(v[2], v[13]) ||
	    (v[8].type != MSGPACK_OBJECT_NIL && !grid_links(v[8], v[13])) ||
	    (v[10].type != MSGPACK_OBJECT_NIL &&
	     !hyperlinks_snapshot_has(v[13], v[10].via.array.ptr[7].via.u64))) return (0);
	for (i = 14; i < 16; i++) {
		if (v[i].type != MSGPACK_OBJECT_ARRAY || v[i].via.array.size > TMTV_SNAPSHOT_MAX_IMAGES) return (0);
		if (i == 15 && v[8].type == MSGPACK_OBJECT_NIL && v[i].via.array.size) return (0);
#ifndef ENABLE_SIXEL
		if (v[i].via.array.size) return (0);
#endif
		for (j = 0; j < v[i].via.array.size; j++) {
			if (!array(v[i].via.array.ptr[j], 3)) return (0);
			im = v[i].via.array.ptr[j].via.array.ptr;
			if (!number(im[0], WINDOW_MAXIMUM, &px) || !number(im[1], WINDOW_MAXIMUM, &py)) return (0);
#ifdef ENABLE_SIXEL
			if (!sixel_snapshot_validate(im[2], NULL)) return (0);
#endif
		}
	}
	pane.type = MSGPACK_OBJECT_ARRAY; pane.via.array.ptr = v; pane.via.array.size = 16;
	return (tmtv_snapshot_media_budget(pane, &links, &images, &pixels));
}

/* Validate both grids and all metadata before any grid allocation or interning.
 * The aggregate cell budget covers current AND saved grids.
 */
static int
validate(msgpack_object o, struct tmtv_snapshot_pane *p, u_int *allocation)
{
	msgpack_object *v, *cursor, *modes, *style, *region, *saved, *tabs, *g;
	u_int version, mode, default_mode, cstyle, default_cstyle, saved_flags;
	u_int sx, sy, saved_sx, saved_sy, cost, saved_cost = 0, i, tab, last = 0;
	if (!array(o, 16) || !tmtv_snapshot_object_bounded(o)) return (0);
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
	/* Completed OSC metadata is part of the screen, not parser scratch. */
	if (!array(v[12], 4)) return (0);
	msgpack_object *meta = v[12].via.array.ptr;
	u_int progress_state, progress;
	if (meta[0].type != MSGPACK_OBJECT_BIN || meta[0].via.bin.size > 16384 ||
	    memchr(meta[0].via.bin.ptr, 0, meta[0].via.bin.size) != NULL ||
	    (meta[1].type != MSGPACK_OBJECT_NIL &&
	     (meta[1].type != MSGPACK_OBJECT_BIN || meta[1].via.bin.size > 16384 ||
	      memchr(meta[1].via.bin.ptr, 0, meta[1].via.bin.size) != NULL)) ||
	    !array(meta[2], 2) ||
	    !number(meta[2].via.array.ptr[0], PROGRESS_BAR_PAUSED, &progress_state)) return (0);
	msgpack_object value = meta[2].via.array.ptr[1];
	if (value.type == MSGPACK_OBJECT_NEGATIVE_INTEGER && value.via.i64 == -1)
		p->progress_bar.progress = -1;
	else if (number(value, 100, &progress)) p->progress_bar.progress = progress;
	else return (0);
	p->progress_bar.state = progress_state;
	if (meta[3].type != MSGPACK_OBJECT_ARRAY || meta[3].via.array.size > 10) return (0);
	p->ntitles = meta[3].via.array.size;
	for (i = 0; i < p->ntitles; i++) {
		msgpack_object title = meta[3].via.array.ptr[i];
		if (title.type != MSGPACK_OBJECT_BIN || title.via.bin.size > 16384 ||
		    memchr(title.via.bin.ptr, 0, title.via.bin.size)) return (0);
	}
	if (!media_validate(v)) return (0);
	if (allocation != NULL) *allocation = cost + saved_cost;
	return (1);
}

int
tmtv_snapshot_pane_validate(msgpack_object o, u_int *allocation)
{
	struct tmtv_snapshot_pane p = {0};
	return (validate(o, &p, allocation));
}

struct tmtv_snapshot_pane *
tmtv_snapshot_pane_decode(msgpack_object o)
{
	struct tmtv_snapshot_pane values, *p;
	msgpack_object *v;
	u_int i, j, sx;
	memset(&values, 0, sizeof values);
	if (!validate(o, &values, NULL)) return (NULL);
	p = xmalloc(sizeof *p); *p = values;
	v = o.via.array.ptr;
	p->hyperlinks = hyperlinks_snapshot_decode(v[13]);
#ifdef ENABLE_SIXEL
	for (i = 0; i < 2; i++) {
		p->nimages[i] = v[14+i].via.array.size;
		if (p->nimages[i]) p->images[i] = xcalloc(p->nimages[i], sizeof *p->images[i]);
		for (j = 0; j < p->nimages[i]; j++) {
			msgpack_object *im = v[14+i].via.array.ptr[j].via.array.ptr;
			p->images[i][j].px = im[0].via.u64; p->images[i][j].py = im[1].via.u64;
			p->images[i][j].data = sixel_snapshot_decode(im[2]);
		}
	}
#else
	(void)j;
#endif
	p->grid = tmtv_snapshot_grid_decode(v[2], TMTV_SNAPSHOT_MAX_HISTORY);
	if (v[8].type != MSGPACK_OBJECT_NIL)
		p->saved_grid = tmtv_snapshot_grid_decode(v[8], 0);
	msgpack_object *meta = v[12].via.array.ptr;
	p->title = xstrndup(meta[0].via.bin.ptr, meta[0].via.bin.size);
	if (meta[1].type != MSGPACK_OBJECT_NIL)
		p->path = xstrndup(meta[1].via.bin.ptr, meta[1].via.bin.size);
	for (i = 0; i < p->ntitles; i++) {
		msgpack_object title = meta[3].via.array.ptr[i];
		p->titles[i] = xstrndup(title.via.bin.ptr, title.via.bin.size);
	}
	sx = p->grid->sx;
	if ((p->tabs = bit_alloc(sx)) == NULL) fatal("bit_alloc failed");
	for (i = 0; i < v[7].via.array.size; i++)
		bit_set(p->tabs, v[7].via.array.ptr[i].via.u64);
	return (p);
}

void
tmtv_snapshot_pane_free(struct tmtv_snapshot_pane *p)
{
	u_int i, j;
	if (p == NULL) return;
	if (p->grid != NULL) grid_destroy(p->grid);
	if (p->saved_grid != NULL) grid_destroy(p->saved_grid);
	for (i = 0; i < p->ntitles; i++) free(p->titles[i]);
	if (p->hyperlinks != NULL) hyperlinks_free(p->hyperlinks);
	for (i = 0; i < 2; i++) {
#ifdef ENABLE_SIXEL
		for (j = 0; j < p->nimages[i]; j++)
			if (p->images[i][j].data != NULL) sixel_free(p->images[i][j].data);
#else
		(void)j;
#endif
		free(p->images[i]);
	}
	free(p->tabs); free(p->title); free(p->path); free(p);
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
	if (s->grid->sx == 0 || s->grid->sx > WINDOW_MAXIMUM) return (-1);
	if (s->tabs != NULL)
		for (i = 0; i < s->grid->sx; i++) if (bit_test(s->tabs, i)) count++;
	msgpack_sbuffer_init(&sb); msgpack_packer_init(&pk, &sb, write_bytes);
#define PACK(type, value) do { if (msgpack_pack_##type(&pk, value)) goto done; } while (0)
	PACK(array, 16); PACK(unsigned_int, TMTV_SNAPSHOT_PANE_VERSION); PACK(unsigned_int, id);
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
	PACK(array, 4);
	if (s->title == NULL || strlen(s->title) > 16384 ||
	    (s->path != NULL && strlen(s->path) > 16384)) goto done;
	PACK(bin, strlen(s->title));
	if (msgpack_pack_bin_body(&pk, s->title, strlen(s->title))) goto done;
	if (s->path == NULL) {
		if (msgpack_pack_nil(&pk)) goto done;
	} else {
		PACK(bin, strlen(s->path));
		if (msgpack_pack_bin_body(&pk, s->path, strlen(s->path))) goto done;
	}
	PACK(array, 2); PACK(int, s->progress_bar.state); PACK(int, s->progress_bar.progress);
	if (s->ntitles > 10) goto done;
	PACK(array, s->ntitles);
	for (i = 0; i < s->ntitles; i++) {
		const char *title = screen_title_at(s, i);
		if (title == NULL || strlen(title) > 16384) goto done;
		PACK(bin, strlen(title));
		if (msgpack_pack_bin_body(&pk, title, strlen(title))) goto done;
	}
	if (hyperlinks_snapshot_encode(s->hyperlinks, &pk)) goto done;
	for (i = 0; i < 2; i++) {
#ifdef ENABLE_SIXEL
		const struct images *list = i ? &s->saved_images : &s->images;
		struct image *im;
		u_int ni = 0, sx, sy;
		TAILQ_FOREACH(im, list, entry) if (++ni > TMTV_SNAPSHOT_MAX_IMAGES) goto done;
		PACK(array, ni);
		TAILQ_FOREACH(im, list, entry) {
			if (im->data == NULL) goto done;
			sixel_size_in_cells(im->data, &sx, &sy);
			if (sx != im->sx || sy != im->sy) goto done;
			PACK(array, 3); PACK(unsigned_int, im->px); PACK(unsigned_int, im->py);
			if (sixel_snapshot_encode(im->data, &pk)) goto done;
		}
#else
		PACK(array, 0);
#endif
	}
	msgpack_unpacked_init(&un);
	memset(&values, 0, sizeof values);
	if (msgpack_unpack_next(&un, sb.data, sb.size, NULL) == MSGPACK_UNPACK_SUCCESS &&
	    validate(un.data, &values, NULL) && out->size <= TMTV_SNAPSHOT_MAX_BYTES - sb.size &&
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
	if (wp == NULL || p == NULL || p->grid == NULL || p->id != wp->id ||
	    p->grid->sx != wp->sx || p->grid->sy != wp->sy) return (-1);
	s = &wp->base;
	/* Only after identity/geometry checks: discard queued writes to old grids. */
	if (event_initialized(&wp->sync_timer)) evtimer_del(&wp->sync_timer);
	free(wp->sync_dirty); wp->sync_dirty = NULL; wp->sync_dirty_size = 0;
	tmtv_snapshot_screen_apply(s, p);
	if (s->mode & MODE_SYNC) screen_write_start_sync(wp);
	/* Mode screens and their cloned copy/view backing are separate owners. */
	wp->flags |= PANE_REDRAW;
	return (0);
}

/* The same field transfer is used for independently owned mode backings. */
void
tmtv_snapshot_screen_apply(struct screen *s, struct tmtv_snapshot_pane *p)
{
#ifdef ENABLE_SIXEL
	struct image *im;
	u_int j;
#endif
	u_int i;
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
	if (s->hyperlinks != NULL) hyperlinks_free(s->hyperlinks);
	s->hyperlinks = p->hyperlinks; p->hyperlinks = NULL;
	hyperlinks_snapshot_activate(s->hyperlinks);
#ifdef ENABLE_SIXEL
	for (i = 0; i < 2; i++) for (j = 0; j < p->nimages[i]; j++) {
		im = image_store(s, p->images[i][j].data);
		p->images[i][j].data = NULL;
		im->px = p->images[i][j].px; im->py = p->images[i][j].py;
		/* Canonical output from validated pixels, never untrusted raw DCS. */
		im->raw_data = sixel_print(im->data, NULL, &im->raw_len);
		if (i) {
			TAILQ_REMOVE(&s->images, im, entry);
			im->list = &s->saved_images; TAILQ_INSERT_TAIL(&s->saved_images, im, entry);
		}
	}
#endif
	s->cx = p->cx; s->cy = p->cy;
	s->mode = p->mode; s->default_mode = p->default_mode;
	s->cstyle = p->cstyle; s->default_cstyle = p->default_cstyle;
	s->ccolour = p->ccolour; s->default_ccolour = p->default_ccolour;
	s->rupper = p->rupper; s->rlower = p->rlower;
	s->saved_cx = p->saved_cx; s->saved_cy = p->saved_cy;
	s->saved_cell = p->saved_cell; s->saved_flags = p->saved_flags;
	free(s->title); free(s->path); screen_free_titles(s);
	for (i = p->ntitles; i > 0; i--) {
		s->title = p->titles[i - 1]; screen_push_title(s);
		free(s->title); p->titles[i - 1] = NULL;
	}
	s->title = p->title; s->path = p->path;
	p->title = p->path = NULL; s->progress_bar = p->progress_bar;
}
