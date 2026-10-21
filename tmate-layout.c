/* Versioned layout codec shared by the host and relay. No live mutations on decode. */
#include "tmux.h"
#include "tmate-layout.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

static const int pane_flags[] = { PANE_ZOOMED, PANE_FLOATOVERZOOM,
    PANE_CAPTUREALLKEYS, PANE_CLOSEONCLICK, PANE_CLOSEONCANCEL, PANE_INPUTOFF };

int
tmtv_layout_pane_flags(struct window_pane *wp)
{
	u_int i;
	int flags = 0;
	for (i = 0; i < nitems(pane_flags); i++)
		if (wp->flags & pane_flags[i])
			flags |= 1 << i;
	return (flags);
}

void
tmtv_layout_set_pane_flags(struct window_pane *wp, int flags)
{
	u_int i;
	for (i = 0; i < nitems(pane_flags); i++) {
		wp->flags &= ~pane_flags[i];
		if (flags & (1 << i))
			wp->flags |= pane_flags[i];
	}
}

static void
pack_geom(msgpack_packer *pk, const struct layout_geometry *g)
{
	msgpack_pack_array(pk, 4);
	msgpack_pack_unsigned_int(pk, g->sx);
	msgpack_pack_unsigned_int(pk, g->sy);
	msgpack_pack_int(pk, g->xoff);
	msgpack_pack_int(pk, g->yoff);
}

static int
pack_tree(msgpack_packer *pk, struct layout_cell *root)
{
	struct layout_cell **cells, *child, *lc;
	int *parents;
	u_int count = 0, i;
	if (root == NULL) {
		msgpack_pack_array(pk, 0);
		return (0);
	}
	cells = xcalloc(TMTV_LAYOUT_MAX_CELLS, sizeof *cells);
	parents = xcalloc(TMTV_LAYOUT_MAX_CELLS, sizeof *parents);
	cells[count++] = root;
	parents[0] = -1;
	for (i = 0; i < count; i++) {
		TAILQ_FOREACH(child, &cells[i]->cells, entry) {
			if (count == TMTV_LAYOUT_MAX_CELLS) {
				free(cells); free(parents);
				return (-1);
			}
			parents[count] = i;
			cells[count++] = child;
		}
	}
	msgpack_pack_array(pk, count);
	for (i = 0; i < count; i++) {
		lc = cells[i];
		msgpack_pack_array(pk, 6);
		msgpack_pack_int(pk, parents[i]);
		msgpack_pack_int(pk, lc->type == LAYOUT_LEFTRIGHT ? 0 :
		    lc->type == LAYOUT_TOPBOTTOM ? 1 : 2);
		msgpack_pack_int(pk, !!(lc->flags & LAYOUT_CELL_FLOATING));
		pack_geom(pk, &lc->g);
		if (lc->fg.sx == UINT_MAX)
			msgpack_pack_array(pk, 0);
		else
			pack_geom(pk, &lc->fg);
		msgpack_pack_int(pk, lc->wp == NULL ? -1 : (int)lc->wp->id);
	}
	free(cells); free(parents);
	return (0);
}

int
tmtv_layout_encode(struct session *s, msgpack_sbuffer *buffer)
{
	msgpack_packer pk;
	struct winlink *wl;
	struct window *w;
	struct window_pane *wp;
	u_int count, nw = 0, total = 0;
	if (s == NULL || s->curw == NULL)
		return (-1);
	RB_FOREACH(wl, winlinks, &s->windows) {
		if (wl->window->active == NULL || strlen(wl->window->name) > 4096)
			return (-1);
		nw++;
	}
	if (nw == 0 || nw > TMTV_LAYOUT_MAX_WINDOWS)
		return (-1);
	msgpack_packer_init(&pk, buffer, msgpack_sbuffer_write);
	msgpack_pack_array(&pk, 4);
	msgpack_pack_int(&pk, TMTV_LAYOUT_OPCODE);
	msgpack_pack_int(&pk, TMTV_LAYOUT_VERSION);
	msgpack_pack_int(&pk, s->curw->idx);
	msgpack_pack_array(&pk, nw);
	RB_FOREACH(wl, winlinks, &s->windows) {
		w = wl->window;
		count = window_count_panes(w, 1);
		if (count == 0 || count > TMTV_LAYOUT_MAX_PANES - total)
			return (-1);
		total += count;
		msgpack_pack_array(&pk, 15);
		msgpack_pack_int(&pk, wl->idx);
		msgpack_pack_str(&pk, strlen(w->name));
		msgpack_pack_str_body(&pk, w->name, strlen(w->name));
		msgpack_pack_unsigned_int(&pk, w->sx);
		msgpack_pack_unsigned_int(&pk, w->sy);
#define PACK_PANE(p) msgpack_pack_int(&pk, (p) == NULL ? -1 : (int)(p)->id)
		PACK_PANE(w->active); PACK_PANE(w->modal);
		PACK_PANE(w->modal_last); PACK_PANE(w->was_zoomed);
#undef PACK_PANE
		msgpack_pack_int(&pk, !!(w->flags & WINDOW_ZOOMED) |
		    (!!(w->flags & WINDOW_WASZOOMED) << 1));
		msgpack_pack_array(&pk, count);
		TAILQ_FOREACH(wp, &w->panes, entry) {
			msgpack_pack_array(&pk, 8);
			msgpack_pack_unsigned_int(&pk, wp->id);
			msgpack_pack_unsigned_int(&pk, wp->sx);
			msgpack_pack_unsigned_int(&pk, wp->sy);
			msgpack_pack_int(&pk, wp->xoff);
			msgpack_pack_int(&pk, wp->yoff);
			msgpack_pack_int(&pk, tmtv_layout_pane_flags(wp));
			msgpack_pack_int(&pk, options_get_number(wp->options, "pane-border-lines"));
			msgpack_pack_int(&pk, options_get_number(wp->options, "pane-border-status"));
		}
		if (pack_tree(&pk, w->layout_root) != 0 ||
		    pack_tree(&pk, w->saved_layout_root) != 0)
			return (-1);
		msgpack_pack_array(&pk, count);
		TAILQ_FOREACH(wp, &w->z_index, zentry)
			msgpack_pack_unsigned_int(&pk, wp->id);
		count = 0;
		TAILQ_FOREACH(wp, &w->last_panes, sentry) count++;
		msgpack_pack_array(&pk, count);
		TAILQ_FOREACH(wp, &w->last_panes, sentry)
			msgpack_pack_unsigned_int(&pk, wp->id);
		msgpack_pack_array(&pk, 2);
		msgpack_pack_int(&pk, options_get_number(w->options, "pane-border-lines"));
		msgpack_pack_int(&pk, options_get_number(w->options, "pane-border-status"));
	}
	return (buffer->size <= 2 * 1024 * 1024 ? 0 : -1);
}

static int
number(msgpack_object obj, int min, int max, int *value)
{
	int64_t n;
	if (obj.type == MSGPACK_OBJECT_POSITIVE_INTEGER) {
		if (obj.via.u64 > (uint64_t)max) return (0);
		n = obj.via.u64;
	} else if (obj.type == MSGPACK_OBJECT_NEGATIVE_INTEGER)
		n = obj.via.i64;
	else return (0);
	if (n < min || n > max) return (0);
	*value = n;
	return (1);
}

static int
array(msgpack_object obj, u_int count)
{
	return (obj.type == MSGPACK_OBJECT_ARRAY && obj.via.array.size == count);
}

static int
geometry(msgpack_object obj, struct layout_geometry *g)
{
	int sx, sy;
	msgpack_object *a;
	if (!array(obj, 4)) return (0);
	a = obj.via.array.ptr;
	if (!number(a[0], 1, WINDOW_MAXIMUM, &sx) ||
	    !number(a[1], 1, WINDOW_MAXIMUM, &sy) ||
	    !number(a[2], -WINDOW_MAXIMUM, WINDOW_MAXIMUM, &g->xoff) ||
	    !number(a[3], -WINDOW_MAXIMUM, WINDOW_MAXIMUM, &g->yoff)) return (0);
	g->sx = sx; g->sy = sy;
	return (1);
}

static int
pane_index(struct tmtv_layout_window *w, int id)
{
	u_int i;
	for (i = 0; i < w->count; i++)
		if (w->panes[i].id == id) return (i);
	return (-1);
}

static int
parse_order(msgpack_object obj, struct tmtv_layout_window *w, int **order,
    u_int *count, int complete)
{
	u_int i, j, n;
	int id;
	if (obj.type != MSGPACK_OBJECT_ARRAY) return (0);
	n = obj.via.array.size;
	if (n > w->count || (complete && n != w->count)) return (0);
	*order = xcalloc(n ? n : 1, sizeof **order); *count = n;
	for (i = 0; i < n; i++) {
		if (!number(obj.via.array.ptr[i], 0, INT_MAX, &id) || pane_index(w, id) < 0)
			return (0);
		for (j = 0; j < i; j++) if ((*order)[j] == id) return (0);
		(*order)[i] = id;
	}
	return (1);
}

static int
parse_tree(msgpack_object obj, struct tmtv_layout_window *w,
    struct tmtv_layout_tree *tree, int saved)
{
	msgpack_object *a;
	struct layout_cell *lc, *parent;
	u_int i, j, n, children[TMTV_LAYOUT_MAX_CELLS] = {0};
	u_int depth[TMTV_LAYOUT_MAX_CELLS] = {0};
	int p, type, floating, id;
	if (obj.type != MSGPACK_OBJECT_ARRAY) return (0);
	n = obj.via.array.size;
	if (n > TMTV_LAYOUT_MAX_CELLS || (!saved && n == 0)) return (0);
	tree->cells = xcalloc(n ? n : 1, sizeof *tree->cells);
	tree->panes = xcalloc(n ? n : 1, sizeof *tree->panes);
	for (i = 0; i < n; i++) {
		if (!array(obj.via.array.ptr[i], 6)) return (0);
		a = obj.via.array.ptr[i].via.array.ptr;
		if (!number(a[0], -1, i ? (int)i-1 : -1, &p) ||
		    (i == 0 ? p != -1 : p < 0) ||
		    !number(a[1], 0, 2, &type) || !number(a[2], 0, 1, &floating) ||
		    !number(a[5], -1, INT_MAX, &id)) return (0);
		if ((type == 2) != (id >= 0) || (floating && type != 2)) return (0);
		if (id >= 0 && pane_index(w, id) < 0) return (0);
		for (j = 0; id >= 0 && j < i; j++)
			if (tree->panes[j] == id) return (0);
		parent = p == -1 ? NULL : tree->cells[p];
		if (parent != NULL && parent->type == LAYOUT_WINDOWPANE) return (0);
		depth[i] = p == -1 ? 0 : depth[p] + 1;
		if (depth[i] > 1000) return (0);
		lc = layout_create_cell(parent);
		tree->cells[i] = lc; tree->panes[i] = id; tree->count++;
		lc->type = type == 0 ? LAYOUT_LEFTRIGHT : type == 1 ? LAYOUT_TOPBOTTOM : LAYOUT_WINDOWPANE;
		lc->flags = floating ? LAYOUT_CELL_FLOATING : 0;
		if (!geometry(a[3], &lc->g) || (!array(a[4], 0) && !geometry(a[4], &lc->fg)))
			return (0);
		if (parent != NULL) {
			TAILQ_INSERT_TAIL(&parent->cells, lc, entry);
			children[p]++;
		}
	}
	for (i = 0; i < n; i++)
		if (tree->cells[i]->type != LAYOUT_WINDOWPANE && children[i] == 0) return (0);
	return (1);
}

void
tmtv_layout_free(struct tmtv_layout *plan)
{
	u_int i, j, k;
	struct tmtv_layout_tree *tree;
	if (plan == NULL) return;
	for (i = 0; i < plan->count; i++) {
		struct tmtv_layout_window *w = &plan->windows[i];
		for (j = 0; j < 2; j++) {
			tree = j ? &w->saved : &w->current;
			/* Detached cells contain no live pane pointers. Free iteratively. */
			for (k = 0; k < tree->count; k++) free(tree->cells[k]);
			free(tree->cells); free(tree->panes);
		}
		free(w->name); free(w->panes); free(w->zorder); free(w->last);
	}
	free(plan->windows); free(plan);
}

struct tmtv_layout *
tmtv_layout_decode(msgpack_object obj)
{
	struct tmtv_layout *plan = NULL;
	struct tmtv_layout_window *w;
	struct tmtv_layout_pane *pane;
	msgpack_object *a, *b, *windows, *panes;
	u_int i, j, k, m, total = 0, n, count;
	int value, sx, sy;
	if (!array(obj, 3)) return (NULL);
	a = obj.via.array.ptr;
	if (!number(a[0], TMTV_LAYOUT_VERSION, TMTV_LAYOUT_VERSION, &value) ||
	    !number(a[1], 0, INT_MAX, &value) || a[2].type != MSGPACK_OBJECT_ARRAY)
		return (NULL);
	n = a[2].via.array.size;
	if (n == 0 || n > TMTV_LAYOUT_MAX_WINDOWS) return (NULL);
	plan = xcalloc(1, sizeof *plan); plan->active = value; plan->count = n;
	plan->windows = xcalloc(n, sizeof *plan->windows);
	windows = a[2].via.array.ptr;
	for (i = 0; i < n; i++) {
		w = &plan->windows[i];
		if (!array(windows[i], 15)) goto fail;
		a = windows[i].via.array.ptr;
		if (!number(a[0], 0, INT_MAX, &w->index) ||
		    a[1].type != MSGPACK_OBJECT_STR || a[1].via.str.size > 4096 ||
		    (a[1].via.str.size != 0 && memchr(a[1].via.str.ptr, 0, a[1].via.str.size) != NULL) ||
		    !number(a[2], 1, WINDOW_MAXIMUM, &sx) || !number(a[3], 1, WINDOW_MAXIMUM, &sy) ||
		    !number(a[4], 0, INT_MAX, &w->active) ||
		    !number(a[5], -1, INT_MAX, &w->modal) ||
		    !number(a[6], -1, INT_MAX, &w->modal_last) ||
		    !number(a[7], -1, INT_MAX, &w->was_zoomed) ||
		    !number(a[8], 0, 3, &w->flags) || a[9].type != MSGPACK_OBJECT_ARRAY) goto fail;
		for (j = 0; j < i; j++) if (plan->windows[j].index == w->index) goto fail;
		w->sx = sx; w->sy = sy;
		w->name = a[1].via.str.size == 0 ? xstrdup("") :
		    xstrndup(a[1].via.str.ptr, a[1].via.str.size);
		w->count = a[9].via.array.size;
		if (w->count == 0 || w->count > TMTV_LAYOUT_MAX_PANES - total) goto fail;
		total += w->count;
		w->panes = xcalloc(w->count, sizeof *w->panes);
		panes = a[9].via.array.ptr;
		for (j = 0; j < w->count; j++) {
			if (!array(panes[j], 8)) goto fail;
			b = panes[j].via.array.ptr; pane = &w->panes[j];
			if (!number(b[0], 0, INT_MAX, &pane->id) ||
			    !number(b[1], 1, WINDOW_MAXIMUM, &sx) || !number(b[2], 1, WINDOW_MAXIMUM, &sy) ||
			    !number(b[3], -WINDOW_MAXIMUM, WINDOW_MAXIMUM, &pane->xoff) ||
			    !number(b[4], -WINDOW_MAXIMUM, WINDOW_MAXIMUM, &pane->yoff) ||
			    !number(b[5], 0, TMTV_LAYOUT_PANE_FLAGS, &pane->flags) ||
			    !number(b[6], 0, PANE_LINES_NONE, &pane->border) ||
			    !number(b[7], 0, PANE_STATUS_BOTTOM_FLOATING, &pane->status)) goto fail;
			pane->sx = sx; pane->sy = sy;
			for (k = 0; k <= i; k++)
				for (m = 0; m < (k == i ? j : plan->windows[k].count); m++)
					if (plan->windows[k].panes[m].id == pane->id) goto fail;
		}
		if (pane_index(w, w->active) < 0 || (w->modal != -1 && pane_index(w, w->modal) < 0) ||
		    (w->modal_last != -1 && pane_index(w, w->modal_last) < 0) ||
		    (w->was_zoomed != -1 && pane_index(w, w->was_zoomed) < 0)) goto fail;
		if (!parse_tree(a[10], w, &w->current, 0) || !parse_tree(a[11], w, &w->saved, 1) ||
		    !parse_order(a[12], w, &w->zorder, &count, 1) ||
		    !parse_order(a[13], w, &w->last, &w->last_count, 0) || !array(a[14], 2)) goto fail;
		b = a[14].via.array.ptr;
		if (!number(b[0], 0, PANE_LINES_NONE, &w->border) ||
		    !number(b[1], 0, PANE_STATUS_BOTTOM_FLOATING, &w->status)) goto fail;
		if (!!(w->flags & 1) != !!w->saved.count) goto fail;
		for (j = 0; j < w->count; j++) {
			struct tmtv_layout_tree *tree = w->saved.count ? &w->saved : &w->current;
			for (k = 0; k < tree->count; k++) if (tree->panes[k] == w->panes[j].id) break;
			if (k == tree->count) goto fail;
		}
	}
	for (i = 0; i < n; i++) if (plan->windows[i].index == plan->active) return (plan);
fail:
	tmtv_layout_free(plan);
	return (NULL);
}
