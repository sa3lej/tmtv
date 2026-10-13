#include <ctype.h>
#include <signal.h>
#include <unistd.h>
#include "tmate.h"
#include "tmate-protocol.h"

char *tmate_left_status, *tmate_right_status;

/*
 * Compare two semver version strings "X.Y.Z".
 * Returns <0 if a < b, 0 if equal, >0 if a > b.
 * Malformed strings are treated as version 0.0.0.
 */
int
tmtv_version_compare(const char *a, const char *b)
{
	int a_major = 0, a_minor = 0, a_patch = 0;
	int b_major = 0, b_minor = 0, b_patch = 0;

	if (a != NULL)
		sscanf(a, "%d.%d.%d", &a_major, &a_minor, &a_patch);
	if (b != NULL)
		sscanf(b, "%d.%d.%d", &b_major, &b_minor, &b_patch);

	if (a_major != b_major)
		return a_major - b_major;
	if (a_minor != b_minor)
		return a_minor - b_minor;
	return a_patch - b_patch;
}

void tmate_send_web_url(struct tmate_session *session)
{
	if (!tmate_has_websocket())
		return;

	const char *url_token = session->session_token_named ?
				session->session_token_named :
				(session->session_token_stable ?
				 session->session_token_stable : session->session_token);
	const char *path = session->link_ttl > 0 ? "j" : "s";
	char *url;
	xasprintf(&url, "https://%s/%s/%s",
		  tmate_settings->tmate_host,
		  path, url_token);
	tmate_notify("web session: %s", url);
	tmate_set_env("tmtv_web", url);
	free(url);
}

#define AUTHORIZED_KEYS_ONLY_ERROR_MSG_1 "Server requires authorized_keys but none are given."
#define AUTHORIZED_KEYS_ONLY_ERROR_MSG_2 "Use '-a FILENAME' to specify an authorized_keys file."
#define AUTHORIZED_KEYS_ONLY_ERROR_MSG_3 "Press <Ctrl-c><Ctrl-d> to exit."

static void tmate_send_session_urls(struct tmate_session *session)
{
	char *ssh_conn_str;
	const char *rw_token, *ro_token;

	/*
	 * Use named tokens if a session name was set during config,
	 * otherwise fall back to random tokens.
	 */
	rw_token = session->session_token_rw_named ?
		   session->session_token_rw_named :
		   (session->session_token_stable ?
		    session->session_token_stable : session->session_token);
	ro_token = session->session_token_ro;

	tmate_notify("Note: clear your terminal before sharing readonly access");

	ssh_conn_str = get_ssh_conn_string(ro_token);
	tmate_notify("ssh session read only: %s", ssh_conn_str);
	tmate_set_env("tmtv_ssh_ro", ssh_conn_str);
	free(ssh_conn_str);

	ssh_conn_str = get_ssh_conn_string(rw_token);
	tmate_notify("ssh session: %s", ssh_conn_str);
	tmate_set_env("tmtv_ssh", ssh_conn_str);
	free(ssh_conn_str);

	if (session->web_sharing_enabled)
		tmate_send_web_url(session);

	/* Notify user about TTL if set */
	if (session->link_ttl > 0) {
		if (session->link_ttl >= 60)
			tmate_notify("Session expires in %dm",
				     session->link_ttl / 60);
		else
			tmate_notify("Session expires in %ds",
				     session->link_ttl);
	}
}

static void tmate_ready(struct tmate_session *session,
			__unused struct tmate_unpacker *uk)
{
	/* This message is also used by the websocket server */
	if (session->awaiting_identity) {
		session->identity_ready_pending = true;
		return;
	}

	/*
	 * We only start accepting connections once the host is ready, this
	 * way we have the authorized keys loaded correctly
	 */

	if (session->authorized_keys) {
		int count = get_num_authorized_keys(session->authorized_keys);
		tmate_info("Restricting ssh access, num_keys=%d", count);
	} else if (tmate_settings->authorized_keys_only) {
		tmate_notify(AUTHORIZED_KEYS_ONLY_ERROR_MSG_1);
		tmate_notify(AUTHORIZED_KEYS_ONLY_ERROR_MSG_2);
		tmate_notify(AUTHORIZED_KEYS_ONLY_ERROR_MSG_3);
		tmate_notify("%s", "");
		kill(getpid(), SIGTERM);
	}

	tmate_info("Session ready token=%s ip=%s version=%s",
		   session->obfuscated_session_token,
		   session->ssh_client.ip_address,
		   session->client_version);

	/*
	 * Send URLs now — after all config (session_name, web_sharing,
	 * authorized_keys) has been processed. This avoids sending
	 * stale random tokens that get replaced by named ones.
	 */
	tmate_send_session_urls(session);
	session->urls_sent = true;

	/* Notify if client version is older than server version */
	if (session->client_version != NULL &&
	    strcmp(session->client_version, "unknown") != 0 &&
	    tmtv_version_compare(session->client_version, TMTV_VERSION) < 0) {
		tmate_notify("Update available: tmtv %s (you have %s). "
			     "curl -fsSL https://tmtv.se/install.sh | sh",
			     TMTV_VERSION, session->client_version);
	}

	/*
	 * Signal the client that URLs are ready. The client's
	 * handle_ready() shows the startup overlay, so this must
	 * come AFTER the URLs are sent.
	 */
	tmate_send_client_ready();

	/* Initialize viewer count format variables */
	tmate_set_env("tmtv_ssh_viewers", "0");
	tmate_set_env("tmtv_web_viewers", "0");

	server_add_accept(0);
}

static void tmate_header(struct tmate_session *session,
			 struct tmate_unpacker *uk)
{
	session->client_protocol_version = unpack_int(uk);

	if (session->client_protocol_version >= 3) {
		session->client_version = unpack_string(uk);
	} else {
		session->client_version = xstrdup("unknown");
	}

	tmate_info("tmtv client version=%s, protocol=%d",
		   session->client_version, session->client_protocol_version);

	if (session->client_protocol_version < TMATE_PROTOCOL_VERSION) {
		tmate_notify("Client too old (protocol %d, need %d). "
			     "Please update: curl -fsSL https://tmtv.se/install.sh | sh",
			     session->client_protocol_version,
			     TMATE_PROTOCOL_VERSION);
		tmate_fatal("Rejecting client with protocol version %d (minimum %d)",
			    session->client_protocol_version,
			    TMATE_PROTOCOL_VERSION);
		return;
	}

	if (tmate_has_websocket())
		tmate_send_websocket_header(session);

	/* URLs and client_ready are sent in tmate_ready(), after all
	 * config (session_name, web_sharing) has been processed. */
}

static void tmate_uname(struct tmate_session *session,
			struct tmate_unpacker *uk)
{
	char *sysname = unpack_string(uk);
	char *nodename = unpack_string(uk);
	char *release = unpack_string(uk);
	char *version = unpack_string(uk);
	char *machine = unpack_string(uk);
	tmate_info("sysname=%s machine=%s release=%s version=%s nodename=%s",
		   sysname, machine, release, version, nodename);
}

extern u_int next_window_pane_id;

/* Recover tiled legacy rectangles using full one-cell-border cuts. A cut
 * cannot cross any pane, and both sides must fill their assigned rectangle.
 * This recovers split semantics, not the original grouping of collinear cuts.
 * Failed candidates never attach leaves to panes; overlaps and non-tiled
 * floating layouts retain the legacy fallback.
 */
static int
tmate_layout_xcompare(const void *a, const void *b)
{
	const struct window_pane *pa = *(struct window_pane * const *)a;
	const struct window_pane *pb = *(struct window_pane * const *)b;

	return ((pa->xoff > pb->xoff) - (pa->xoff < pb->xoff));
}

static int
tmate_layout_ycompare(const void *a, const void *b)
{
	const struct window_pane *pa = *(struct window_pane * const *)a;
	const struct window_pane *pb = *(struct window_pane * const *)b;

	return ((pa->yoff > pb->yoff) - (pa->yoff < pb->yoff));
}

/* tmux groups adjacent splits of the same orientation in one node. */
static void
tmate_layout_append(struct layout_cell *root, struct layout_cell *lc)
{
	struct layout_cell *child;

	if (lc->type == root->type) {
		while ((child = TAILQ_FIRST(&lc->cells)) != NULL) {
			TAILQ_REMOVE(&lc->cells, child, entry);
			child->parent = root;
			TAILQ_INSERT_TAIL(&root->cells, child, entry);
		}
		layout_free_cell(lc);
	} else {
		lc->parent = root;
		TAILQ_INSERT_TAIL(&root->cells, lc, entry);
	}
}

static struct layout_cell *
tmate_layout_tree(struct window_pane **panes, u_int count,
    u_int x, u_int y, u_int sx, u_int sy)
{
	struct layout_cell *root, *left, *right;
	struct window_pane *wp;
	u_int axis, i, end, start, limit, cut;

	if (count == 1) {
		wp = panes[0];
		if (wp->xoff != x || wp->yoff != y || wp->sx != sx || wp->sy != sy)
			return (NULL);
		root = layout_create_cell(NULL);
		layout_set_size(root, sx, sy, x, y);
		root->type = LAYOUT_WINDOWPANE;
		root->wp = wp; /* Attach only after the complete tree succeeds. */
		return (root);
	}
	for (axis = 0; axis < 2; axis++) {
		/* Sort only this local pointer array, never the pane list. */
		qsort(panes, count, sizeof *panes,
		    axis ? tmate_layout_ycompare : tmate_layout_xcompare);
		start = axis ? y : x;
		limit = start + (axis ? sy : sx);
		end = start;
		for (i = 1; i < count; i++) {
			wp = panes[i - 1];
			cut = axis ? wp->yoff + wp->sy : wp->xoff + wp->sx;
			if (cut > end)
				end = cut;
			cut = axis ? panes[i]->yoff : panes[i]->xoff;
			if (cut != end + 1 || end <= start || cut >= limit)
				continue;
			left = tmate_layout_tree(panes, i, x, y,
			    axis ? sx : end - x, axis ? end - y : sy);
			right = tmate_layout_tree(panes + i, count - i,
			    axis ? x : cut, axis ? cut : y,
			    axis ? sx : limit - cut, axis ? limit - cut : sy);
			if (left == NULL || right == NULL) {
				/* Panes have no attached cells until the whole tree succeeds. */
				if (left != NULL)
					layout_free_cell(left);
				if (right != NULL)
					layout_free_cell(right);
				return (NULL);
			}
			root = layout_create_cell(NULL);
			layout_set_size(root, sx, sy, x, y);
			layout_make_node(root, axis ? LAYOUT_TOPBOTTOM : LAYOUT_LEFTRIGHT);
			tmate_layout_append(root, left);
			tmate_layout_append(root, right);
			return (root);
		}
	}
	return (NULL);
}

static void
tmate_layout_attach(struct layout_cell *lc)
{
	struct layout_cell *child;

	if (lc->type == LAYOUT_WINDOWPANE)
		lc->wp->layout_cell = lc;
	else {
		TAILQ_FOREACH(child, &lc->cells, entry)
			tmate_layout_attach(child);
	}
}

static void
tmate_layout_root(struct window *w)
{
	struct window_pane *panes[TMATE_MAX_PANES], *wp;
	struct layout_cell *root, *lc;
	u_int count = 0, x = UINT_MAX, y = UINT_MAX, right = 0, bottom = 0;

	TAILQ_FOREACH(wp, &w->panes, entry) {
		panes[count++] = wp;
		if (wp->xoff < x)
			x = wp->xoff;
		if (wp->yoff < y)
			y = wp->yoff;
		if (wp->xoff + wp->sx > right)
			right = wp->xoff + wp->sx;
		if (wp->yoff + wp->sy > bottom)
			bottom = wp->yoff + wp->sy;
	}
	if (w->layout_root != NULL)
		layout_free(w);
	if (count == 0)
		return;
	root = tmate_layout_tree(panes, count, x, y, right - x, bottom - y);
	if (root != NULL) {
		w->layout_root = root;
		tmate_layout_attach(root);
		return;
	}
	root = w->layout_root = layout_create_cell(NULL);
	layout_set_size(root, w->sx, w->sy, 0, 0);
	layout_make_node(root, LAYOUT_LEFTRIGHT);
	/* Restore legacy list order after the unsuccessful spatial sort. */
	TAILQ_FOREACH(wp, &w->panes, entry) {
		lc = layout_create_cell(root);
		layout_set_size(lc, wp->sx, wp->sy, wp->xoff, wp->yoff);
		layout_make_leaf(lc, wp);
		TAILQ_INSERT_TAIL(&root->cells, lc, entry);
	}
}

static void tmate_sync_window_panes(struct window *w,
				    struct tmate_unpacker *w_uk,
				    int *num_panes)
{
	struct tmate_unpacker uk, tmp_uk;
	struct window_pane *wp, *wp_tmp;
	int active_pane_id;
	u_int seen_ids[TMATE_MAX_PANES];
	int seen_count = 0;
	int i, found;

	/* Release every leaf while its pane is still alive. Closing individual
	 * old leaves later would resize survivors over received rectangles. */
	if (w->layout_root != NULL) {
		layout_free(w);
		w->layout_root = NULL;
	}

	unpack_each(&uk, &tmp_uk, w_uk) {
		if (++(*num_panes) > TMATE_MAX_PANES)
			tmate_fatal("Too many opened panes (max=%d)", TMATE_MAX_PANES);

		int id = unpack_int(&uk);
		u_int sx = unpack_int(&uk);
		u_int sy = unpack_int(&uk);
		u_int xoff = unpack_int(&uk);
		u_int yoff = unpack_int(&uk);

		wp = window_pane_find_by_id(id);
		if (wp && wp->window != w) {
			/* Pane migrated between windows during a race.
			 * Remove it from the old window so it can be
			 * recreated in the correct one below. */
			tmate_info("Pane id=%u in wrong window, "
				   "moving to window idx=%d",
				   id, w->id);
			if (wp->layout_cell != NULL)
				layout_close_pane(wp);
			window_remove_pane(wp->window, wp);
			wp = NULL;
		}

		if (!wp) {
			next_window_pane_id = id;
			/*
			 * tmux 3.6a: window_add_pane(window, other_pane, hlimit, flags)
			 * Pass NULL for other_pane, 0 for flags.
			 */
			wp = window_add_pane(w, NULL, TMATE_HLIMIT, 0);
			wp->ictx = input_init(wp, NULL, &wp->palette);
			window_set_active_pane(w, wp, 0);

			/* Replay any pty_data that arrived before this
			 * sync_windows created the pane. */
			pty_pending_replay(id);
		}

		if (seen_count < TMATE_MAX_PANES)
			seen_ids[seen_count++] = id;

		if (wp->xoff != xoff || wp->yoff != yoff ||
		    wp->sx != sx || wp->sy != sy) {
			wp->xoff = xoff;
			wp->yoff = yoff;
			window_pane_resize(wp, sx, sy);

			wp->flags |= PANE_REDRAW;
		}
	}

	/* Remove panes not in the sync message */
	TAILQ_FOREACH_SAFE(wp, &w->panes, entry, wp_tmp) {
		found = 0;
		for (i = 0; i < seen_count; i++) {
			if (wp->id == seen_ids[i]) {
				found = 1;
				break;
			}
		}
		if (!found)
			window_remove_pane(w, wp);
	}

	/* Pane leaves are also required by incremental tty rendering. */
	tmate_layout_root(w);

	active_pane_id = unpack_int(w_uk);
	wp = window_pane_find_by_id(active_pane_id);
	if (!wp || wp->window != w) {
		/* Fall back to first pane instead of crashing.
		 * This can happen transiently during window detach
		 * when sync_layout fires with stale active pane state. */
		tmate_info("Invalid active_pane_id %d, falling back to "
			   "first pane", active_pane_id);
		wp = TAILQ_FIRST(&w->panes);
	}
	if (wp)
		window_set_active_pane(w, wp, 0);
}

static void tmate_sync_windows(struct session *s, int sx, int sy,
			       struct tmate_unpacker *s_uk)
{
	struct tmate_unpacker uk, tmp_uk;
	struct winlink *wl, *wl_tmp;
	struct window *w;
	int active_window_idx;
	int num_panes = 0;
	int seen_idxs[512];
	int seen_count = 0;
	int i, found;

	unpack_each(&uk, &tmp_uk, s_uk) {
		int idx    = unpack_int(&uk);
		char *name = unpack_string(&uk);

		wl = winlink_find_by_index(&s->windows, idx);
		if (!wl) {
			/*
			 * tmux 3.6a: no session_new(). Create a window directly
			 * and link it.
			 */
			w = window_create(sx, sy, 0, 0);
			wl = session_attach(s, w, idx, NULL);
			if (!wl)
				tmate_fatal("can't create window idx=%d", idx);
			window_set_name(w, name);
			/* Disable automatic rename so client-sent names stick */
			options_set_number(w->options, "automatic-rename", 0);
		}

		if (seen_count < 512)
			seen_idxs[seen_count++] = idx;
		w = wl->window;

		/* Resize window to match client dimensions */
		if (w->sx != (u_int)sx || w->sy != (u_int)sy) {
			tmate_info("sync_windows: resizing window idx=%d from %ux%u to %dx%d",
				   idx, w->sx, w->sy, sx, sy);
			window_resize(w, sx, sy, -1, -1);
		}

		window_set_name(w, name);
		free(name);

		tmate_sync_window_panes(w, &uk, &num_panes);
	}

	/* Remove windows not in the sync message */
	RB_FOREACH_SAFE(wl, winlinks, &s->windows, wl_tmp) {
		found = 0;
		for (i = 0; i < seen_count; i++) {
			if (wl->idx == seen_idxs[i]) {
				found = 1;
				break;
			}
		}
		if (!found)
			session_detach(s, wl);
	}

	active_window_idx = unpack_int(s_uk);
	wl = winlink_find_by_index(&s->windows, active_window_idx);
	if (!wl) {
		/* Active window index not found — can happen transiently
		 * when windows are destroyed between sync messages.
		 * Fall back to the first window. */
		tmate_info("Active window idx=%d not found, falling back "
			   "to first window", active_window_idx);
		wl = RB_MIN(winlinks, &s->windows);
	}
	if (!wl)
		return; /* no windows at all — nothing to do */

	{
		struct winlink *old_wl = s->curw;
		session_set_current(s, wl);
		server_redraw_window(wl->window);

		/* When the active window changes, SSE viewers need a screen
		 * dump of the new window's content.  PTY data is only sent
		 * for new output, so without this, web viewers see a blank
		 * screen after a window switch.
		 * Skip when vpty is active — it sends full-screen output. */
		if (old_wl != wl) {
			if (tmate_session->vpty_active)
				sse_force_vpty_redraw(tmate_session);
			else
				sse_broadcast_screen_dump(tmate_session);
		}
	}
}

/* Check the complete layout before creating, resizing or removing any pane. */
static int snapshot_uint(msgpack_object, u_int *);

static int
layout_active_id(msgpack_object obj)
{
	return ((obj.type == MSGPACK_OBJECT_NEGATIVE_INTEGER && obj.via.i64 == -1) ||
	    (obj.type == MSGPACK_OBJECT_POSITIVE_INTEGER && obj.via.u64 <= INT_MAX));
}

static int
layout_valid(const struct tmate_unpacker *uk)
{
	msgpack_object *windows, *window, *panes, *pane;
	u_int sx, sy, value[5], ids[TMATE_MAX_PANES], indexes[512];
	u_int i, j, k, n, count = 0;

	if (uk->argc != 4 || !snapshot_uint(uk->argv[0], &sx) ||
	    !snapshot_uint(uk->argv[1], &sy) || sx == 0 || sy == 0 ||
	    sx > WINDOW_MAXIMUM || sy > WINDOW_MAXIMUM ||
	    uk->argv[2].type != MSGPACK_OBJECT_ARRAY ||
	    !layout_active_id(uk->argv[3]))
		return (0);
	n = uk->argv[2].via.array.size;
	if (n == 0 || n > nitems(indexes))
		return (0);
	windows = uk->argv[2].via.array.ptr;
	for (i = 0; i < n; i++) {
		if (windows[i].type != MSGPACK_OBJECT_ARRAY ||
		    windows[i].via.array.size != 4)
			return (0);
		window = windows[i].via.array.ptr;
		if (!snapshot_uint(window[0], &indexes[i]) || indexes[i] > INT_MAX ||
		    (window[1].type != MSGPACK_OBJECT_STR && window[1].type != MSGPACK_OBJECT_BIN) ||
		    window[2].type != MSGPACK_OBJECT_ARRAY || !layout_active_id(window[3]))
			return (0);
		for (j = 0; j < i; j++) {
			if (indexes[i] == indexes[j])
				return (0);
		}
		if (window[2].via.array.size == 0 ||
		    window[2].via.array.size > TMATE_MAX_PANES - count)
			return (0);
		panes = window[2].via.array.ptr;
		for (j = 0; j < window[2].via.array.size; j++) {
			if (panes[j].type != MSGPACK_OBJECT_ARRAY || panes[j].via.array.size != 5)
				return (0);
			pane = panes[j].via.array.ptr;
			for (k = 0; k < 5; k++) {
				if (!snapshot_uint(pane[k], &value[k]))
					return (0);
			}
			if (value[0] > INT_MAX || value[1] == 0 || value[2] == 0 ||
			    value[1] > WINDOW_MAXIMUM || value[2] > WINDOW_MAXIMUM ||
			    value[3] > WINDOW_MAXIMUM - value[1] ||
			    value[4] > WINDOW_MAXIMUM - value[2])
				return (0);
			for (k = 0; k < count; k++) {
				if (ids[k] == value[0])
					return (0);
			}
			ids[count++] = value[0];
		}
	}
	return (1);
}

static void tmate_sync_layout(__unused struct tmate_session *session,
			      struct tmate_unpacker *uk)
{
	struct session *s;

	int sx, sy;

	if (!layout_valid(uk)) {
		tmate_debug("Ignoring invalid layout");
		return;
	}
	sx = unpack_int(uk);
	sy = unpack_int(uk);

	s = RB_MIN(sessions, &sessions);
	if (!s) {
		/*
		 * tmux 3.6a: session_create(prefix, name, cwd,
		 *     environ, options, termios)
		 * No sx/sy args — set on the session struct after.
		 */
		struct options *oo = options_create(global_s_options);
		struct environ *env = environ_create();
		s = session_create(NULL, "default", "/",
				   env, oo, NULL);
		if (!s)
			tmate_fatal("can't create main session");
	}

	tmate_sync_windows(s, sx, sy, uk);

	/* Discard pending buffers for panes that never appeared.
	 * After sync_windows, all valid panes exist — anything
	 * still pending was a temporary pane that was already gone. */
	pty_pending_flush_stale();

	/* Resize the virtual PTY client to match the host's dimensions */
	if (tmate_has_websocket())
		sse_vpty_resize(session, sx, sy);
}

/*
 * Buffer pty_data for panes that don't exist yet.  The host sends
 * pty_data before sync_windows when panes are created (e.g. Claude
 * agents, screenshot processes).  We buffer the data and replay it
 * once sync_windows creates the pane.
 *
 * Dynamic linked list — no fixed slot limit.  Each pane's buffer is
 * capped at PTY_PENDING_MAX bytes; total memory across all panes is
 * capped at PTY_PENDING_TOTAL_MAX.
 */
#define PTY_PENDING_MAX       (128 * 1024)        /* per pane */
#define PTY_PENDING_TOTAL_MAX (2 * 1024 * 1024)   /* all panes combined */

struct pty_pending {
	TAILQ_ENTRY(pty_pending) entry;
	int     id;
	u_char *data;
	size_t  len;
	size_t  cap;
	int     count;
};

static TAILQ_HEAD(, pty_pending) pty_pending_list =
    TAILQ_HEAD_INITIALIZER(pty_pending_list);
static size_t pty_pending_total;

static void pty_pending_free(struct pty_pending *p)
{
	TAILQ_REMOVE(&pty_pending_list, p, entry);
	pty_pending_total -= p->len;
	free(p->data);
	free(p);
}

/* Replay buffered data into a now-existing pane */
void pty_pending_replay(int pane_id)
{
	struct pty_pending *p, *tmp;
	struct window_pane *wp;

	TAILQ_FOREACH_SAFE(p, &pty_pending_list, entry, tmp) {
		if (p->id != pane_id)
			continue;

		wp = window_pane_find_by_id(pane_id);
		if (wp && p->len > 0) {
			tmate_info("Replaying %zu bytes (%d messages) "
				   "for pane id=%d",
				   p->len, p->count, pane_id);
			input_parse_buffer(wp, p->data, p->len);
			wp->window->flags |= WINDOW_SILENCE;
		}
		pty_pending_free(p);
		return;
	}
}

/* Discard buffered data for panes that never appeared */
void pty_pending_flush_stale(void)
{
	struct pty_pending *p, *tmp;

	TAILQ_FOREACH_SAFE(p, &pty_pending_list, entry, tmp) {
		if (window_pane_find_by_id(p->id))
			continue; /* pane exists — will be replayed */
		if (p->count > 0)
			tmate_debug("Discarding %zu bytes (%d messages) "
				    "for pane id=%d (never synced)",
				    p->len, p->count, p->id);
		pty_pending_free(p);
	}
}

static void tmate_pty_data(__unused struct tmate_session *session,
			   struct tmate_unpacker *uk)
{
	struct window_pane *wp;
	struct pty_pending *p;
	const char *buf;
	size_t len;
	int id;

	id = unpack_int(uk);
	unpack_buffer(uk, &buf, &len);

	wp = window_pane_find_by_id(id);
	if (!wp) {
		/* Buffer data until sync_windows creates this pane */

		/* Find existing entry for this pane */
		TAILQ_FOREACH(p, &pty_pending_list, entry) {
			if (p->id == id)
				break;
		}

		if (!p) {
			/* Enforce total memory limit — evict oldest */
			while (pty_pending_total + len > PTY_PENDING_TOTAL_MAX
			       && !TAILQ_EMPTY(&pty_pending_list)) {
				struct pty_pending *oldest =
				    TAILQ_FIRST(&pty_pending_list);
				tmate_info("Pending buffer over total limit, "
					   "evicting pane id=%d "
					   "(%zu bytes, %d messages)",
					   oldest->id, oldest->len,
					   oldest->count);
				pty_pending_free(oldest);
			}

			p = xcalloc(1, sizeof(*p));
			p->id = id;
			TAILQ_INSERT_TAIL(&pty_pending_list, p, entry);
			tmate_debug("Buffering pty_data for unknown pane "
				    "id=%d (awaiting sync)", id);
		}

		/* Append data if within per-pane budget */
		if (p->len + len <= PTY_PENDING_MAX) {
			if (p->len + len > p->cap) {
				size_t newcap = p->cap ? p->cap * 2 : 8192;
				if (newcap < p->len + len)
					newcap = p->len + len;
				if (newcap > PTY_PENDING_MAX)
					newcap = PTY_PENDING_MAX;
				p->data = xrealloc(p->data, newcap);
				p->cap = newcap;
			}
			memcpy(p->data + p->len, buf, len);
			p->len += len;
			pty_pending_total += len;
		}

		p->count++;
		return;
	}

	input_parse_buffer(wp, (u_char *)buf, len);

	wp->window->flags |= WINDOW_SILENCE;
}

static void tmate_exec_cmd_str(__unused struct tmate_session *session,
			       struct tmate_unpacker *uk)
{
	struct cmd_parse_result *pr;
	struct cmd_parse_input pi;
	char *cmd_str;

	cmd_str = unpack_string(uk);

	tmate_debug("Local cmd: %s", cmd_str);

	memset(&pi, 0, sizeof pi);
	pr = cmd_parse_from_string(cmd_str, &pi);
	if (pr->status == CMD_PARSE_ERROR) {
		tmate_debug("parse error: %s", pr->error);
		free(pr->error);
		goto out;
	}

	cmdq_append(NULL, cmdq_get_command(pr->cmdlist, NULL));
	cmd_list_free(pr->cmdlist);
out:
	free(cmd_str);
}

extern void tmate_hook_set_option_auth(const char *name, const char *val);

static void tmate_exec_cmd(__unused struct tmate_session *session,
			   struct tmate_unpacker *uk)
{
	struct cmd_parse_result *pr;
	struct cmd_parse_input pi;
	char *cmd_str;
	int i;
	int argc;
	char **argv;
	size_t len;

	argc = uk->argc;
	/*
	 * An empty command array has nothing to run. Skip it instead of
	 * calling xmalloc(0) (which aborts) — a malformed/empty EXEC_CMD from
	 * the client must not be able to crash the session daemon.
	 */
	if (argc == 0) {
		tmate_debug("Ignoring empty exec_cmd");
		return;
	}
	argv = xmalloc(sizeof(char *) * argc);
	for (i = 0; i < argc; i++)
		argv[i] = unpack_string(uk);

	/*
	 * Intercept "set-option tmtv-set key=val" and process it
	 * synchronously. The command queue is async and would delay
	 * session_name/web_sharing until after tmate_ready().
	 */
	if (argc == 3 &&
	    !strcmp(argv[0], "set-option") &&
	    !strcmp(argv[1], "tmtv-set")) {
		/* Private options can contain credentials. Never log their values. */
		tmate_debug("Sync tmtv-set");
		tmate_hook_set_option_auth("tmtv-set", argv[2]);
		cmd_free_argv(argc, argv);
		return;
	}

	/*
	 * Log "set-option [-g] prefix[2] <value>" so integration tests
	 * can verify the prefix was synced.  The command still runs
	 * through the normal queue for actual option application.
	 */
	{
		const char *opt_name = NULL, *opt_value = NULL;
		for (i = 1; i < argc; i++) {
			if (argv[i][0] == '-')
				continue;
			if (!opt_name)
				opt_name = argv[i];
			else if (!opt_value)
				opt_value = argv[i];
		}
		if (opt_name && opt_value &&
		    (!strcmp(opt_name, "prefix") ||
		     !strcmp(opt_name, "prefix2")))
			tmate_info("Session %s set to %s",
				   opt_name, opt_value);
	}

	/* Build a command string from argv and parse it */
	len = 0;
	for (i = 0; i < argc; i++)
		len += strlen(argv[i]) + 1;
	cmd_str = xmalloc(len);
	cmd_str[0] = '\0';
	for (i = 0; i < argc; i++) {
		if (i > 0)
			strlcat(cmd_str, " ", len);
		strlcat(cmd_str, argv[i], len);
	}

	tmate_debug("Local cmd: %s", cmd_str);

	memset(&pi, 0, sizeof pi);
	pr = cmd_parse_from_string(cmd_str, &pi);
	if (pr->status == CMD_PARSE_ERROR) {
		tmate_debug("parse error: %s", pr->error);
		free(pr->error);
		goto out;
	}

	cmdq_append(NULL, cmdq_get_command(pr->cmdlist, NULL));
	cmd_list_free(pr->cmdlist);

out:
	free(cmd_str);
	cmd_free_argv(argc, argv);
}

static void tmate_failed_cmd(__unused struct tmate_session *session,
			     struct tmate_unpacker *uk)
{
	struct client *c;
	int client_id;
	char *cause;

	client_id = unpack_int(uk);
	cause = unpack_string(uk);

	int cid = 0;
	TAILQ_FOREACH(c, &clients, entry) {
		if (c && cid == client_id) {
			*cause = toupper((u_char) *cause);
			/*
			 * tmux 3.6a: status_message_set(c, delay, ignore_styles,
			 *     ignore_keys, exact_position, fmt, ...)
			 */
			status_message_set(c, -1, 0, 0, 0, "%s", cause);
			break;
		}
		cid++;
	}

	free(cause);
}

static void tmate_status(struct tmate_session *session,
			 struct tmate_unpacker *uk)
{
	struct client *c;

	free(tmate_left_status);
	free(tmate_right_status);
	tmate_left_status = unpack_string(uk);
	tmate_right_status = unpack_string(uk);

	TAILQ_FOREACH(c, &clients, entry)
		c->flags |= CLIENT_REDRAWSTATUS;

	/* Broadcast status to web viewers via SSE */
	if (tmate_has_websocket()) {
		_pack(&session->websocket_encoder, array, 3);
		_pack(&session->websocket_encoder, int, TMATE_OUT_STATUS);
		_pack(&session->websocket_encoder, string, tmate_left_status ? tmate_left_status : "");
		_pack(&session->websocket_encoder, string, tmate_right_status ? tmate_right_status : "");
	}
}

static void tmate_sync_copy_mode(__unused struct tmate_session *session,
				 struct tmate_unpacker *uk)
{
	struct window_pane *wp;
	struct window_mode_entry *wme;
	struct tmate_unpacker cm_uk;
	int64_t pane_id;

	pane_id = unpack_int(uk);
	unpack_array(uk, &cm_uk);
	/* Nonempty legacy snapshots require separate state reconciliation. */
	if (cm_uk.argc != 0 || pane_id < 0 || pane_id > UINT_MAX)
		return;
	wp = window_pane_find_by_id((u_int)pane_id);
	if (wp == NULL)
		return;
	wme = TAILQ_FIRST(&wp->modes);
	if (wme != NULL && (wme->mode == &window_copy_mode ||
	    wme->mode == &window_view_mode))
		window_pane_reset_mode(wp);
}

static void tmate_write_copy_mode(__unused struct tmate_session *session,
				  struct tmate_unpacker *uk)
{
	struct window_pane *wp;
	int64_t id;
	char *str;

	id = unpack_int(uk);
	/* Validate the wire value before narrowing to the pane identifier. */
	if (id < 0 || id > UINT_MAX)
		return;
	wp = window_pane_find_by_id((u_int)id);
	if (!wp) {
		tmate_info("copy_mode for unknown pane id=%u, discarding",
		    (u_int)id);
		return;
	}

	str = unpack_string(uk);

	/*
	 * Wire output needs its own backing screen, as with command output.
	 * Bring view mode to the front even when another mode is active; never
	 * pass another mode's private data to window_copy_add. Repeated output
	 * reuses the existing view mode and preserves its accumulated lines.
	 */
	window_pane_set_mode(wp, NULL, &window_view_mode, NULL, NULL, NULL);
	if (TAILQ_FIRST(&wp->modes) != NULL &&
	    TAILQ_FIRST(&wp->modes)->mode == &window_view_mode)
		window_copy_add(wp, 0, "%s", str);
	free(str);
}

static void tmate_fin(struct tmate_session *session,
		      __unused struct tmate_unpacker *uk)
{
	tmate_info("Client sent FIN (graceful disconnect)");
	session->fin_received = true;

	/*
	 * Send FIN to SSE clients BEFORE termination. The subsequent
	 * tmate_send_websocket_daemon_msg() in on_daemon_decoder_read
	 * also forwards this, but request_server_termination() sends
	 * SIGTERM which may kill the process before that call runs or
	 * before the SSE data is flushed. Writing directly to socket
	 * fds here ensures viewers receive the FIN and show "Session
	 * ended" instead of entering a reconnect loop.
	 */
	tmate_send_fin_to_ws_clients(session);

	request_server_termination();
}

static void tmate_reconnect(__unused struct tmate_session *session,
			    __unused struct tmate_unpacker *uk)
{
	if (!tmate_has_websocket())
		tmate_fatal("Cannot do reconnections without the websocket server");
}

/* Legacy snapshot integers are unsigned fields, never narrowed before checks. */
static int
snapshot_uint(msgpack_object obj, u_int *value)
{
	if (obj.type != MSGPACK_OBJECT_POSITIVE_INTEGER || obj.via.u64 > UINT_MAX)
		return (0);
	*value = obj.via.u64;
	return (1);
}

/*
 * Decode into an independent grid. The legacy wire has no cell byte lengths,
 * dimensions or line flags. Accept only unambiguous scalar cells and compact
 * padding sentinels; combined glyphs and multi-column tabs need the negotiated
 * format. Never guess boundaries and shift subsequent styles onto other cells.
 */
static struct grid *
snapshot_grid(msgpack_object obj, struct grid *current, u_int *cx, u_int *cy,
    int saved)
{
	struct grid *gd = NULL;
	struct grid_cell gc;
	msgpack_object *screen, *rows, *row, *styles;
	const u_char *bytes;
	size_t offset, length;
	u_int lines, cells, x, y, packed, padding;
	enum utf8_state state;

	if (obj.type != MSGPACK_OBJECT_ARRAY || obj.via.array.size != 3)
		return (NULL);
	screen = obj.via.array.ptr;
	if (!snapshot_uint(screen[0], cx) || !snapshot_uint(screen[1], cy) ||
	    screen[2].type != MSGPACK_OBJECT_ARRAY)
		return (NULL);
	/* cx == sx is the legitimate pending-wrap cursor position. */
	if (!(saved && *cx == UINT_MAX && *cy == UINT_MAX) &&
	    (*cx > current->sx || *cy >= current->sy))
		return (NULL);
	lines = screen[2].via.array.size;
	if (current->sx == 0 || current->sy == 0 || lines < current->sy ||
	    lines - current->sy > TMATE_HLIMIT ||
	    (saved && lines != current->sy))
		return (NULL);
	gd = grid_create(current->sx, lines, saved ? 0 : current->hlimit);
	gd->sy = current->sy;
	gd->hsize = lines - gd->sy;
	rows = screen[2].via.array.ptr;
	for (y = 0; y < lines; y++) {
		if (rows[y].type != MSGPACK_OBJECT_ARRAY || rows[y].via.array.size != 2)
			goto invalid;
		row = rows[y].via.array.ptr;
		if ((row[0].type != MSGPACK_OBJECT_STR && row[0].type != MSGPACK_OBJECT_BIN) ||
		    row[1].type != MSGPACK_OBJECT_ARRAY)
			goto invalid;
		bytes = (const u_char *)row[0].via.str.ptr;
		length = row[0].via.str.size;
		cells = row[1].via.array.size;
		if (cells > gd->sx)
			goto invalid;
		styles = row[1].via.array.ptr;
		offset = padding = 0;
		for (x = 0; x < cells; x++) {
			if (!snapshot_uint(styles[x], &packed) || offset == length)
				goto invalid;
			gc = grid_default_cell;
			gc.flags = (packed >> 24) & ~GRID_FLAG_EXTENDED;
			gc.attr = (packed >> 16) & 0xff;
			gc.bg = (packed >> 8) & 0xff;
			gc.fg = packed & 0xff;
			if (gc.flags & GRID_FLAG_FG256)
				gc.fg |= COLOUR_FLAG_256;
			if (gc.flags & GRID_FLAG_BG256)
				gc.bg |= COLOUR_FLAG_256;
			if (gc.flags & GRID_FLAG_PADDING) {
				if (padding == 0 || bytes[offset++] != '!' ||
				    (gc.flags & GRID_FLAG_TAB))
					goto invalid;
				padding--;
				grid_set_padding(gd, x, y);
				continue;
			}
			if (padding != 0)
				goto invalid;
			if (bytes[offset] < 0x80) {
				if (bytes[offset] < 0x20 || bytes[offset] == 0x7f)
					goto invalid;
				utf8_set(&gc.data, bytes[offset++]);
			} else {
				state = utf8_open(&gc.data, bytes[offset++]);
				while (state == UTF8_MORE && offset < length)
					state = utf8_append(&gc.data, bytes[offset++]);
				if (state != UTF8_DONE || gc.data.width == 0)
					goto invalid;
			}
			if (gc.flags & GRID_FLAG_TAB) {
				/* A one-column tab is the only unambiguous legacy tab. */
				if (gc.data.size != 1 || gc.data.data[0] != ' ' ||
				    (x + 1 < cells && styles[x + 1].type == MSGPACK_OBJECT_POSITIVE_INTEGER &&
				    (styles[x + 1].via.u64 & ((uint64_t)GRID_FLAG_PADDING << 24))))
					goto invalid;
				grid_set_tab(&gc, 1);
			}
			padding = gc.data.width - 1;
			if (padding > cells - x - 1)
				goto invalid;
			grid_set_cell(gd, x, y, &gc);
		}
		if (offset != length || padding != 0)
			goto invalid;
	}
	return (gd);
invalid:
	grid_destroy(gd);
	return (NULL);
}

static void tmate_snapshot(__unused struct tmate_session *session,
			   struct tmate_unpacker *uk)
{
	msgpack_object *panes, *pane;
	struct window_pane *wp;
	struct screen *s;
	struct grid *base, *saved;
	u_int i, id, mode, cx, cy, saved_cx, saved_cy;

	if (uk->argc != 1 || uk->argv[0].type != MSGPACK_OBJECT_ARRAY)
		return;
	panes = uk->argv[0].via.array.ptr;
	for (i = 0; i < uk->argv[0].via.array.size; i++) {
		if (panes[i].type != MSGPACK_OBJECT_ARRAY || panes[i].via.array.size != 4)
			continue;
		pane = panes[i].via.array.ptr;
		if (!snapshot_uint(pane[0], &id) || !snapshot_uint(pane[1], &mode) ||
		    mode > INT_MAX || (wp = window_pane_find_by_id(id)) == NULL)
			continue;
		s = &wp->base;
		base = snapshot_grid(pane[2], s->grid, &cx, &cy, 0);
		if (base == NULL)
			continue;
		saved = NULL;
		saved_cx = saved_cy = UINT_MAX;
		if (pane[3].type != MSGPACK_OBJECT_NIL) {
			saved = snapshot_grid(pane[3], s->grid, &saved_cx, &saved_cy, 1);
			if (saved == NULL) {
				grid_destroy(base);
				continue;
			}
		}
		/* Both grids are valid before any pane-owned state is replaced. */
		base->flags = s->saved_grid != NULL ? s->saved_flags : s->grid->flags;
		s->saved_flags = base->flags;
		if (saved != NULL)
			base->flags &= ~GRID_HISTORY;
		grid_destroy(s->grid);
		if (s->saved_grid != NULL)
			grid_destroy(s->saved_grid);
		s->grid = base;
		s->saved_grid = saved;
		s->cx = cx;
		s->cy = cy;
		s->saved_cx = saved_cx;
		s->saved_cy = saved_cy;
		s->mode = mode;
		/* Copy/view modes own cloned backing grids; never replace their data. */
		wp->flags |= PANE_REDRAW;
	}
}

/*
 * Handle clipboard data from the host.
 * Phase 1: Deliver to RW SSH viewers via OSC 52 (direct tty write).
 * Phase 3: Web RW viewers get a separate broadcast (see below).
 *
 * RO viewers (both SSH and web) are excluded — they never receive
 * clipboard data. The vpty client is also excluded.
 */
static void tmate_clipboard(struct tmate_session *session,
			     struct tmate_unpacker *uk)
{
	struct client *c;
	const char *buf;
	size_t len;
	char *data_copy;

	unpack_buffer(uk, &buf, &len);

	if (len == 0 || len > 100 * 1024)
		return;

	tmate_debug("Clipboard from host: %zu bytes", len);

	/* Update the server's paste buffer (suppresses re-broadcast) */
	data_copy = xmalloc(len);
	memcpy(data_copy, buf, len);
	paste_add_from_remote(data_copy, len);

	/*
	 * Emit OSC 52 directly to each RW SSH viewer's tty.
	 * Skip RO viewers and the virtual PTY client.
	 */
	TAILQ_FOREACH(c, &clients, entry) {
		if (c == NULL)
			continue;
		if (!(c->flags & CLIENT_IDENTIFIED))
			continue;
		if (c->flags & CLIENT_READONLY)
			continue;
		if (c->flags & CLIENT_TMATE_VPTY)
			continue;
		if (c->tty.term == NULL)
			continue;
		tty_set_selection(&c->tty, "", buf, len);
	}

	/* Broadcast to RW web viewers only */
	if (tmate_has_websocket())
		tmate_send_clipboard_to_rw_web(session, buf, len);
}

static void tmate_input_mode(struct tmate_session *session,
			     struct tmate_unpacker *uk)
{
	bool enabled = unpack_bool(uk);
	bool mirror = unpack_bool(uk);
	bool was_enabled = session->input_mode_enabled;

	session->input_mode_enabled = enabled;
	session->input_mirror = mirror;

	tmate_info("Input mode: enabled=%d mirror=%d", enabled, mirror);

	/* Only send viewer list on transition from disabled to enabled.
	 * Repeated enable calls (e.g. from SET_MIRROR) must not
	 * generate duplicate USER_JOIN events. */
	if (enabled && !was_enabled) {
		/* Send current viewer list to host */
		struct client *c;
		TAILQ_FOREACH(c, &clients, entry) {
			if (!(c->flags & CLIENT_IDENTIFIED))
				continue;
			if (c->flags & CLIENT_TMATE_VPTY)
				continue;
			if (c->readonly)
				continue;
			char name[64];
			snprintf(name, sizeof(name), "ssh-%d", c->pid);
			tmate_send_user_join(session, c->pid, name,
					     false, "ssh");
		}

		if (tmate_has_websocket()) {
			struct ws_client *wc;
			TAILQ_FOREACH(wc, &session->ws_clients, entry) {
				if (wc->handshake_done && !wc->is_post &&
			    wc->viewer_id > 0 && !wc->readonly)
					tmate_send_user_join(session,
							     wc->viewer_id,
							     "web",
							     false, "web");
			}
		}
	}
}

extern void tmate_register_session_identity(struct tmate_session *, const char *);

static void tmate_session_identity(struct tmate_session *session,
				  struct tmate_unpacker *uk)
{
	char *secret = unpack_string(uk);
	tmate_register_session_identity(session, secret);
	/* This value is neither stored in tmux options nor forwarded to viewers. */
	freezero(secret, strlen(secret));
	if (session->identity_ready_pending) {
		session->identity_ready_pending = false;
		tmate_ready(session, uk);
	}
}

void tmate_dispatch_daemon_message(struct tmate_session *session,
				   struct tmate_unpacker *uk)
{
	/* We make a copy because we are mutating it */
	struct tmate_unpacker uk_copy = *uk;
	uk = &uk_copy;

	int cmd = unpack_int(uk);
	switch (cmd) {
#define dispatch(c, f) case c: f(session, uk); break
	dispatch(TMATE_OUT_HEADER,		tmate_header);
	dispatch(TMATE_OUT_SYNC_LAYOUT,		tmate_sync_layout);
	dispatch(TMATE_OUT_PTY_DATA,		tmate_pty_data);
	dispatch(TMATE_OUT_EXEC_CMD_STR,	tmate_exec_cmd_str);
	dispatch(TMATE_OUT_FAILED_CMD,		tmate_failed_cmd);
	dispatch(TMATE_OUT_STATUS,		tmate_status);
	dispatch(TMATE_OUT_SYNC_COPY_MODE,	tmate_sync_copy_mode);
	dispatch(TMATE_OUT_WRITE_COPY_MODE,	tmate_write_copy_mode);
	dispatch(TMATE_OUT_FIN,			tmate_fin);
	dispatch(TMATE_OUT_READY,		tmate_ready);
	dispatch(TMATE_OUT_RECONNECT,		tmate_reconnect);
	dispatch(TMATE_OUT_SNAPSHOT,		tmate_snapshot);
	dispatch(TMATE_OUT_EXEC_CMD,		tmate_exec_cmd);
	dispatch(TMATE_OUT_UNAME,		tmate_uname);
	dispatch(TMATE_OUT_INPUT_MODE,		tmate_input_mode);
	dispatch(TMATE_OUT_CLIPBOARD,		tmate_clipboard);
	dispatch(TMATE_OUT_SESSION_IDENTITY,	tmate_session_identity);
	default: tmate_fatal("Bad message type: %d", cmd);
	}
}
