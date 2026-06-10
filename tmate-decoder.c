#include "tmate.h"
#include "tmate-protocol.h"

/*
 * Find the tmux session associated with a tmate_session.
 * Searches the sessions tree for a session whose ->tmate pointer matches.
 * Falls back to RB_MIN (first session) for backward compatibility when
 * sessions haven't been linked yet (e.g., during early startup).
 * Returns NULL if no sessions exist.
 */
struct session *
tmate_find_session(struct tmate_session *ts)
{
	struct session *s;

	RB_FOREACH(s, sessions, &sessions) {
		if (s->tmate == ts)
			return (s);
	}

	/* Fallback: no session linked yet, use first available. */
	return (RB_MIN(sessions, &sessions));
}

static void handle_notify(struct tmate_session *session,
			  struct tmate_unpacker *uk)
{
	char *msg = unpack_string(uk);

	if (session->reconnected) {
		/* Suppress overlay/status bar spam on reconnect */
		tmate_debug("reconnect notify: %s", msg);
	} else {
		cfg_add_cause("[tmtv] %s", msg);
		tmate_status_message("%s", msg);
	}

	free(msg);
}

static void handle_legacy_pane_key(struct tmate_session *session,
				   struct tmate_unpacker *uk)
{
	struct session *s;
	struct window *w;
	struct window_pane *wp;

	int key = unpack_int(uk);

	s = tmate_find_session(session);
	if (!s)
		return;

	w = s->curw->window;
	if (!w)
		return;

	wp = w->active;
	if (!wp)
		return;

	window_pane_key(wp, NULL, s, s->curw, key, NULL);
}

static struct window_pane *find_window_pane(struct session *s, int pane_id)
{
	struct window *w;

	if (pane_id != -1)
		return window_pane_find_by_id(pane_id);

	w = s->curw->window;
	if (!w)
		return NULL;

	return w->active;
}

static void handle_pane_key(struct tmate_session *session,
			    struct tmate_unpacker *uk)
{
	struct session *s;
	struct window_pane *wp;

	int pane_id = unpack_int(uk);
	key_code key = unpack_int(uk);

	s = tmate_find_session(session);
	if (!s)
		return;

	wp = find_window_pane(s, pane_id);
	if (!wp)
		return;

	window_pane_key(wp, NULL, s, s->curw, key, NULL);
}

static void handle_resize(struct tmate_session *session,
			  struct tmate_unpacker *uk)
{
	session->min_sx = unpack_int(uk);
	session->min_sy = unpack_int(uk);
	recalculate_sizes();
}

static void handle_exec_cmd_str(__unused struct tmate_session *session,
				struct tmate_unpacker *uk)
{
	struct cmd_parse_result *pr;
	struct cmd_parse_input pi;
	u_int i;

	int client_id = unpack_int(uk);
	char *cmd_str = unpack_string(uk);

	memset(&pi, 0, sizeof pi);
	pr = cmd_parse_from_string(cmd_str, &pi);
	if (pr->status == CMD_PARSE_ERROR) {
		tmate_failed_cmd(&tmate_session, client_id, pr->error);
		free(pr->error);
		goto out;
	}

	cmdq_append(NULL, cmdq_get_command(pr->cmdlist, NULL));
	cmd_list_free(pr->cmdlist);

out:
	free(cmd_str);
}

static void handle_exec_cmd(__unused struct tmate_session *session,
			    struct tmate_unpacker *uk)
{
	struct cmd_parse_result *pr;
	struct cmd_parse_input pi;
	unsigned int argc;
	char **argv;
	char *cmd_str;
	u_int i;

	int client_id = unpack_int(uk);

	argc = uk->argc;
	/*
	 * A command with no arguments has nothing to run. Skip it instead of
	 * calling xmalloc(0) (which aborts) — a malformed/empty EXEC_CMD from
	 * the server must not be able to crash the client.
	 */
	if (argc == 0) {
		tmate_debug("Ignoring empty exec_cmd");
		return;
	}
	argv = xmalloc(sizeof(char *) * argc);
	for (i = 0; i < argc; i++)
		argv[i] = unpack_string(uk);

	/* Build a command string from argv */
	{
		size_t len = 0;
		for (i = 0; i < argc; i++)
			len += strlen(argv[i]) + 1;
		cmd_str = xmalloc(len);
		cmd_str[0] = '\0';
		for (i = 0; i < argc; i++) {
			if (i > 0)
				strlcat(cmd_str, " ", len);
			strlcat(cmd_str, argv[i], len);
		}
	}

	memset(&pi, 0, sizeof pi);
	pr = cmd_parse_from_string(cmd_str, &pi);
	if (pr->status == CMD_PARSE_ERROR) {
		tmate_failed_cmd(&tmate_session, client_id, pr->error);
		free(pr->error);
		goto out;
	}

	cmdq_append(NULL, cmdq_get_command(pr->cmdlist, NULL));
	cmd_list_free(pr->cmdlist);

out:
	free(cmd_str);
	cmd_free_argv(argc, argv);
}

static void maybe_save_reconnection_data(struct tmate_session *session,
				const char *name, const char *value)
{
	if (!strcmp(name, "tmate_reconnection_data")) {
		free(session->reconnection_data);
		session->reconnection_data = xstrdup(value);
	}
}

static void handle_set_env(struct tmate_session *session,
			   struct tmate_unpacker *uk)
{
	char *name = unpack_string(uk);
	char *value = unpack_string(uk);

	tmate_set_env(session, name, value);
	maybe_save_reconnection_data(session, name, value);

	free(name);
	free(value);
}

static void handle_user_join(__unused struct tmate_session *session,
			     struct tmate_unpacker *uk)
{
	int user_id = unpack_int(uk);
	char *name = unpack_string(uk);
	bool readonly = unpack_bool(uk);
	char *type = unpack_string(uk);

	tmtv_input_on_user_join(user_id, name, readonly, type);

	free(name);
	free(type);
}

static void handle_user_leave(__unused struct tmate_session *session,
			      struct tmate_unpacker *uk)
{
	int user_id = unpack_int(uk);

	tmtv_input_on_user_leave(user_id);
}

static void handle_user_input(__unused struct tmate_session *session,
			      struct tmate_unpacker *uk)
{
	int user_id = unpack_int(uk);
	int pane_id = unpack_int(uk);
	key_code key = (key_code)unpack_int(uk);

	tmtv_input_on_user_input(user_id, pane_id, key);
}

/*
 * Phase 2: Receive clipboard data from a viewer via the server.
 * Update the host's paste buffer and system clipboard.
 *
 * paste_add_from_remote() sets the tmux paste buffer but does NOT
 * emit OSC 52 to the terminal.  Without the OSC 52, the host's
 * system clipboard stays stale and Shift+Insert / middle-click
 * paste returns old or garbage data.  Write OSC 52 to every
 * attached client's tty so the terminal updates its selection.
 */
static void handle_clipboard(__unused struct tmate_session *session,
			     struct tmate_unpacker *uk)
{
	const char *buf;
	size_t len;
	char *data;
	struct client *c;

	unpack_buffer(uk, &buf, &len);

	if (len == 0 || len > 100 * 1024)
		return;

	tmate_debug("Clipboard from viewer: %zu bytes", len);

	data = xmalloc(len);
	memcpy(data, buf, len);
	paste_add_from_remote(data, len);

	/* Update the host's system clipboard via OSC 52 */
	TAILQ_FOREACH(c, &clients, entry) {
		if (c->flags & CLIENT_IDENTIFIED && c->tty.term != NULL)
			tty_set_selection(&c->tty, "", buf, len);
	}
}

static void handle_ready(struct tmate_session *session,
			 __unused struct tmate_unpacker *uk)
{
	session->tmate_env_ready = 1;

	/*
	 * Show the startup overlay on first connect only.
	 * It contains the tip lines (added in tmate_session_start)
	 * plus the SSH/web URLs (added by handle_notify).
	 * On reconnect, env vars are still updated via SET_ENV
	 * but we skip the overlay to avoid spamming the user.
	 */
	{
		struct session *s = tmate_find_session(session);
		if (s != NULL) {
			if (!session->reconnected)
				cfg_show_causes(s);
			notify_session("tmtv-ready", s);
		}
	}
}

void tmate_dispatch_slave_message(struct tmate_session *session,
				  struct tmate_unpacker *uk)
{
	int cmd = unpack_int(uk);
	switch (cmd) {
#define dispatch(c, f) case c: f(session, uk); break
	dispatch(TMATE_IN_NOTIFY,		handle_notify);
	dispatch(TMATE_IN_LEGACY_PANE_KEY,	handle_legacy_pane_key);
	dispatch(TMATE_IN_RESIZE,		handle_resize);
	dispatch(TMATE_IN_EXEC_CMD_STR,		handle_exec_cmd_str);
	dispatch(TMATE_IN_SET_ENV,		handle_set_env);
	dispatch(TMATE_IN_READY,		handle_ready);
	dispatch(TMATE_IN_PANE_KEY,		handle_pane_key);
	dispatch(TMATE_IN_EXEC_CMD,		handle_exec_cmd);
	dispatch(TMATE_IN_USER_JOIN,		handle_user_join);
	dispatch(TMATE_IN_USER_LEAVE,		handle_user_leave);
	dispatch(TMATE_IN_USER_INPUT,		handle_user_input);
	dispatch(TMATE_IN_CLIPBOARD,		handle_clipboard);
	default: tmate_info("Bad message type: %d", cmd);
	}
}
