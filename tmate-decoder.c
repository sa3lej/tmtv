#include "tmate.h"
#include "tmate-protocol.h"

static void handle_notify(__unused struct tmate_session *session,
			  struct tmate_unpacker *uk)
{
	char *msg = unpack_string(uk);
	cfg_add_cause("[tmtv] %s", msg);
	tmate_status_message("%s", msg);
	free(msg);
}

static void handle_legacy_pane_key(__unused struct tmate_session *_session,
				   struct tmate_unpacker *uk)
{
	struct session *s;
	struct window *w;
	struct window_pane *wp;

	int key = unpack_int(uk);

	s = RB_MIN(sessions, &sessions);
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

static void handle_pane_key(__unused struct tmate_session *_session,
			    struct tmate_unpacker *uk)
{
	struct session *s;
	struct window_pane *wp;

	int pane_id = unpack_int(uk);
	key_code key = unpack_int(uk);

	s = RB_MIN(sessions, &sessions);
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
		tmate_failed_cmd(client_id, pr->error);
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
		tmate_failed_cmd(client_id, pr->error);
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

	tmate_set_env(name, value);
	maybe_save_reconnection_data(session, name, value);

	free(name);
	free(value);
}

static void handle_ready(struct tmate_session *session,
			 __unused struct tmate_unpacker *uk)
{
	session->tmate_env_ready = 1;
	/* In tmux 3.6a, signal_waiting_clients doesn't exist.
	 * Use notify_session to signal readiness instead. */
	{
		struct session *s = RB_MIN(sessions, &sessions);
		if (s != NULL)
			notify_session("tmtv-ready", s);
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
	default: tmate_info("Bad message type: %d", cmd);
	}
}
