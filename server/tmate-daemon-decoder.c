#include <ctype.h>
#include <signal.h>
#include <unistd.h>
#include "tmate.h"
#include "tmate-protocol.h"

char *tmate_left_status, *tmate_right_status;

#define AUTHORIZED_KEYS_ONLY_ERROR_MSG_1 "Server requires authorized_keys but none are given."
#define AUTHORIZED_KEYS_ONLY_ERROR_MSG_2 "Use '-a FILENAME' to specify an authorized_keys file."
#define AUTHORIZED_KEYS_ONLY_ERROR_MSG_3 "Press <Ctrl-c><Ctrl-d> to exit."

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
		/* Inform the user that this connexion isn't happening,
		 * and what to do about it.
		 */

		tmate_notify(AUTHORIZED_KEYS_ONLY_ERROR_MSG_1);
		tmate_notify(AUTHORIZED_KEYS_ONLY_ERROR_MSG_2);
		tmate_notify(AUTHORIZED_KEYS_ONLY_ERROR_MSG_3);
		tmate_notify("");
		/*
		 * tmux 3.6a: server_send_exit() is static. Use kill(getpid(), SIGTERM)
		 * to trigger clean shutdown instead.
		 */
		kill(getpid(), SIGTERM);
	}
	server_add_accept(0);
}

static void tmate_header(struct tmate_session *session,
			 struct tmate_unpacker *uk)
{
	char *ssh_conn_str;

	session->client_protocol_version = unpack_int(uk);

	if (session->client_protocol_version >= 3) {
		session->client_version = unpack_string(uk);
	} else {
		session->client_version = xstrdup("1.8.5");
	}

	if (session->client_protocol_version < 6) {
		/* older clients don't send a ready message */
		tmate_ready(session, NULL);
	}

	if (session->client_protocol_version < 5) {
		session->daemon_encoder.mpac_version = 4;
	}

	tmate_info("tmate version=%s, protocol=%d",
		   session->client_version, session->client_protocol_version);

	if (tmate_has_websocket())
		tmate_send_websocket_header(session);

	ssh_conn_str = get_ssh_conn_string(session->session_token_ro);
	tmate_notify("Note: clear your terminal before sharing readonly access");
	tmate_notify("ssh session read only: %s", ssh_conn_str);
	tmate_set_env("tmate_ssh_ro", ssh_conn_str);
	free(ssh_conn_str);

	ssh_conn_str = get_ssh_conn_string(session->session_token);
	tmate_notify("ssh session: %s", ssh_conn_str);
	tmate_set_env("tmate_ssh", ssh_conn_str);
	free(ssh_conn_str);

	if (tmate_has_websocket()) {
		char *web_url;
		int port = tmate_settings->web_port > 0 ?
			   tmate_settings->web_port :
			   tmate_settings->websocket_port;
		if (port == 80)
			xasprintf(&web_url, "http://%s/s/%s",
				  tmate_settings->tmate_host,
				  session->session_token);
		else
			xasprintf(&web_url, "http://%s:%d/s/%s",
				  tmate_settings->tmate_host,
				  port, session->session_token);
		tmate_notify("web session: %s", web_url);
		tmate_set_env("tmate_web", web_url);
		free(web_url);
	}

	tmate_send_client_ready();
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
			/* Pane in the wrong window */
			tmate_fatal("Pane id=%u in the wrong window", id);
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

	active_pane_id = unpack_int(w_uk);
	wp = window_pane_find_by_id(active_pane_id);
	if (!wp || wp->window != w)
		tmate_fatal("Invalid active_pane_id recevied");
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
	if (!wl)
		tmate_fatal("no valid active window");

	session_set_current(s, wl);
	server_redraw_window(wl->window);
}

static void tmate_sync_layout(__unused struct tmate_session *session,
			      struct tmate_unpacker *uk)
{
	struct session *s;

	int sx = unpack_int(uk);
	int sy = unpack_int(uk);

	tmate_info("sync_layout: sx=%d sy=%d", sx, sy);

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
}

static void tmate_pty_data(__unused struct tmate_session *session,
			   struct tmate_unpacker *uk)
{
	struct window_pane *wp;
	const char *buf;
	size_t len;
	int id;

	id = unpack_int(uk);
	unpack_buffer(uk, &buf, &len);

	wp = window_pane_find_by_id(id);
	if (!wp)
		tmate_fatal("can't find pane id=%d (pty_data)", id);

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

static void tmate_status(__unused struct tmate_session *session,
			 struct tmate_unpacker *uk)
{
	struct client *c;

	free(tmate_left_status);
	free(tmate_right_status);
	tmate_left_status = unpack_string(uk);
	tmate_right_status = unpack_string(uk);

	TAILQ_FOREACH(c, &clients, entry)
		c->flags |= CLIENT_REDRAWSTATUS;
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
	if (!wp)
		tmate_fatal("can't find pane id=%d (copy_mode)", id);

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

static void tmate_fin(__unused struct tmate_session *session,
		      __unused struct tmate_unpacker *uk)
{
	session->fin_received = true;
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
	if (!wp)
		tmate_fatal("can't find pane id=%d (snapshot restore)", id);
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
			   struct tmate_unpacker *uk)
{
	struct tmate_unpacker panes_uk, pane_uk;

	unpack_array(uk, &panes_uk);
	while (panes_uk.argc > 0) {
		unpack_array(&panes_uk, &pane_uk);
		restore_snapshot_pane(&pane_uk);
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
	default: tmate_fatal("Bad message type: %d", cmd);
	}
}
