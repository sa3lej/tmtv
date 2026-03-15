#include <ctype.h>
#include <signal.h>
#include <unistd.h>
#include "tmate.h"
#include "tmate-protocol.h"

extern void tmate_send_fin_to_ws_clients(struct tmate_session *session);

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
				session->session_token;
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
		   session->session_token;
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

	/*
	 * Rebuild layout tree so that wp->layout_cell is non-NULL for
	 * every pane.  tty_write()'s set_client_cb checks layout_cell
	 * and skips panes where it is NULL, which blocks all incremental
	 * screen updates to attached clients.
	 */
	{
		int pane_count = 0;
		TAILQ_FOREACH(wp, &w->panes, entry)
			pane_count++;

		if (w->layout_root != NULL)
			layout_free(w);

		if (pane_count == 1) {
			layout_init(w, TAILQ_FIRST(&w->panes));
		} else if (pane_count > 1) {
			struct layout_cell *root, *lc;
			root = w->layout_root = layout_create_cell(NULL);
			layout_set_size(root, w->sx, w->sy, 0, 0);
			root->type = LAYOUT_LEFTRIGHT;
			TAILQ_FOREACH(wp, &w->panes, entry) {
				lc = layout_create_cell(root);
				layout_set_size(lc, wp->sx, wp->sy,
						wp->xoff, wp->yoff);
				layout_make_leaf(lc, wp);
				TAILQ_INSERT_TAIL(&root->cells, lc, entry);
			}
		}
	}

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

static void tmate_sync_layout(__unused struct tmate_session *session,
			      struct tmate_unpacker *uk)
{
	struct session *s;

	int sx = unpack_int(uk);
	int sy = unpack_int(uk);

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
		tmate_debug("Sync tmtv-set: %s", argv[2]);
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
	/*
	 * tmux 3.6a: window_copy_mode_data is private to window-copy.c.
	 * We cannot directly manipulate copy mode state from outside.
	 * For now, consume the message but skip the copy mode sync.
	 * A proper fix would require adding accessor functions to window-copy.c.
	 */
	int pane_id;
	struct tmate_unpacker cm_uk;

	pane_id = unpack_int(uk);
	(void)pane_id;
	unpack_array(uk, &cm_uk);
	/* Consume remaining data silently */
}

static void tmate_write_copy_mode(__unused struct tmate_session *session,
				  struct tmate_unpacker *uk)
{
	struct window_pane *wp;
	int id;
	char *str;

	id = unpack_int(uk);
	wp = window_pane_find_by_id(id);
	if (!wp) {
		tmate_info("copy_mode for unknown pane id=%d, discarding", id);
		return;
	}

	str = unpack_string(uk);

	/*
	 * tmux 3.6a: window_pane_set_mode() takes 5 args and
	 * window_copy_init_for_output()/window_copy_add() signatures changed.
	 * Use the new API.
	 */
	if (TAILQ_EMPTY(&wp->modes))
		window_pane_set_mode(wp, NULL, &window_copy_mode, NULL, NULL);

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

static void restore_snapshot_grid(struct grid *grid, struct tmate_unpacker *uk)
{
	struct grid_cell gc;
	char *line_str;
	struct utf8_data *utf8_data;
	unsigned int i, line_i;
	unsigned int packed_flags;

	struct tmate_unpacker lines_uk, line_uk, line_flags_uk;

	memset(&gc, 0, sizeof gc);

	unpack_array(uk, &lines_uk);
	for (line_i = 0; lines_uk.argc > 0; line_i++) {
		while (line_i >= grid->hsize + grid->sy)
			grid_scroll_history(grid, 8);

		unpack_array(&lines_uk, &line_uk);
		line_str = unpack_string(&line_uk);
		utf8_data = utf8_fromcstr(line_str);
		free(line_str);

		unpack_array(&line_uk, &line_flags_uk);
		for (i = 0; line_flags_uk.argc > 0; i++) {
			utf8_copy(&gc.data, &utf8_data[i]);
			packed_flags = unpack_int(&line_flags_uk);
			gc.flags = (packed_flags >> 24) & 0xFF;
			gc.attr  = (packed_flags >> 16) & 0xFFFF;
			/*
			 * tmux 3.6a: fg/bg are int, not u_char.
			 * Old protocol packs them as single bytes.
			 */
			gc.bg    = (packed_flags >> 8)  & 0xFF;
			gc.fg    =  packed_flags        & 0xFF;
			grid_set_cell(grid, i, line_i, &gc);
		}
	}
}

static void restore_snapshot_pane(struct tmate_unpacker *uk)
{
	int id;
	struct window_pane *wp;
	struct tmate_unpacker grid_uk;
	struct screen *screen;

	id = unpack_int(uk);
	wp = window_pane_find_by_id(id);
	if (!wp) {
		tmate_info("snapshot restore for unknown pane id=%d, "
			   "discarding", id);
		return;
	}
	screen = &wp->base;
	screen_reinit(screen);
	wp->flags |= PANE_REDRAW;

	screen->mode = unpack_int(uk);

	unpack_array(uk, &grid_uk);
	screen->cx = unpack_int(&grid_uk);
	screen->cy = unpack_int(&grid_uk);
	grid_clear_history(screen->grid);
	restore_snapshot_grid(screen->grid, &grid_uk);

	if (screen->saved_grid != NULL) {
		grid_destroy(screen->saved_grid);
		screen->saved_grid = NULL;
	}

	if (unpack_peek_type(uk) == MSGPACK_OBJECT_NIL)
		return;

	unpack_array(uk, &grid_uk);
	screen->saved_cx = unpack_int(&grid_uk);
	screen->saved_cy = unpack_int(&grid_uk);
	screen->saved_grid = grid_create(screen->grid->sx, screen->grid->sy, 0);
	restore_snapshot_grid(screen->saved_grid, &grid_uk);
}

static void tmate_snapshot(__unused struct tmate_session *session,
			   __unused struct tmate_unpacker *uk)
{
	/*
	 * Snapshot restore is not needed — the server builds its own
	 * grid from PTY data, and SSE clients get screen dumps directly.
	 * Skip to avoid format mismatches between client/server.
	 */
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
	default: tmate_fatal("Bad message type: %d", cmd);
	}
}
