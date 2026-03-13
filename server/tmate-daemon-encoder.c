#include "tmate.h"
#include "tmate-protocol.h"

#define pack(what, ...) _pack(&tmate_session->daemon_encoder, what, ##__VA_ARGS__)

static void __tmate_notify(const char *msg)
{
	pack(array, 2);
	pack(int, TMATE_IN_NOTIFY);
	pack(string, msg);
}

void tmate_notify(const char *fmt, ...)
{
	va_list ap;
	char msg[1024];

	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);

	__tmate_notify(msg);
}

static void __tmate_notify_later(__unused evutil_socket_t fd,
				 __unused short what, void *arg)
{
	char *msg = arg;
	__tmate_notify(msg);
}

void tmate_notify_later(int timeout, const char *fmt, ...)
{
	struct timeval tv;
	va_list ap;
	char *msg;

	tv.tv_sec = timeout;
	tv.tv_usec = 0;

	va_start(ap, fmt);
	xvasprintf(&msg, fmt, ap);
	va_end(ap);

	/*
	 * tmux 3.6a / libevent2: ev_notify_timer is a pointer.
	 * Allocate if needed, then use evtimer_assign.
	 */
	if (tmate_session->ev_notify_timer == NULL)
		tmate_session->ev_notify_timer = evtimer_new(
			tmate_session->ev_base, __tmate_notify_later, msg);
	else
		evtimer_assign(tmate_session->ev_notify_timer,
			       tmate_session->ev_base,
			       __tmate_notify_later, msg);
	evtimer_add(tmate_session->ev_notify_timer, &tv);
}

void tmate_send_client_ready(void)
{
	if (tmate_session->client_protocol_version < 4)
		return;

	pack(array, 1);
	pack(int, TMATE_IN_READY);
}

/* Local env storage for format variable injection into status bar */
struct tmate_env_entry {
	TAILQ_ENTRY(tmate_env_entry) entry;
	char *name;
	char *value;
};

static TAILQ_HEAD(, tmate_env_entry) tmate_env_list =
    TAILQ_HEAD_INITIALIZER(tmate_env_list);

void tmate_set_env(const char *name, const char *value)
{
	struct tmate_env_entry *e;

	/* Store locally for format variable resolution */
	TAILQ_FOREACH(e, &tmate_env_list, entry) {
		if (!strcmp(e->name, name)) {
			free(e->value);
			e->value = xstrdup(value);
			goto send;
		}
	}
	e = xmalloc(sizeof(*e));
	e->name = xstrdup(name);
	e->value = xstrdup(value);
	TAILQ_INSERT_HEAD(&tmate_env_list, e, entry);

send:
	/* Send to client over the wire */
	if (tmate_session->client_protocol_version < 4)
		return;

	pack(array, 3);
	pack(int, TMATE_IN_SET_ENV);
	pack(string, name);
	pack(string, value);
}

void tmate_format(struct format_tree *ft)
{
	struct tmate_env_entry *e;

	TAILQ_FOREACH(e, &tmate_env_list, entry) {
		format_add(ft, e->name, "%s", e->value);
	}
}

void tmate_client_resize(u_int sx, u_int sy)
{
	pack(array, 3);
	pack(int, TMATE_IN_RESIZE);
	/* cast to signed, -1 == no clients */
	pack(int, sx);
	pack(int, sy);
}

void tmate_client_legacy_pane_key(__unused int pane_id, int key)
{
	/*
	 * We don't specify the pane id because the current active pane is
	 * behind, so we'll let master send the key to its active pane.
	 */

	pack(array, 2);
	pack(int, TMATE_IN_LEGACY_PANE_KEY);
	pack(int, key);
}

void tmate_client_pane_key(int pane_id, key_code key)
{
	if (key == KEYC_NONE || key == KEYC_UNKNOWN)
		return;

	/* Mouse keys not supported yet */
	if (KEYC_IS_MOUSE(key))
		return;

	if (tmate_session->client_protocol_version < 5) {
		tmate_translate_legacy_key(pane_id, key);
		return;
	}

	if (tmate_session->client_protocol_version == 5 && key & KEYC_BASE) {
		if ((key & KEYC_MASK_KEY) >= (KEYC_BSPACE & KEYC_MASK_KEY))
			key -= 9;
	}

	pack(array, 3);
	pack(int, TMATE_IN_PANE_KEY);
	pack(int, pane_id);
	pack(uint64, key);
}

extern const struct cmd_entry cmd_bind_key_entry;
extern const struct cmd_entry cmd_unbind_key_entry;
extern const struct cmd_entry cmd_set_option_entry;
extern const struct cmd_entry cmd_set_window_option_entry;
extern const struct cmd_entry cmd_detach_client_entry;
extern const struct cmd_entry cmd_attach_session_entry;

static const struct cmd_entry *local_cmds[] = {
	&cmd_bind_key_entry,
	&cmd_unbind_key_entry,
	&cmd_set_option_entry,
	&cmd_set_window_option_entry,
	&cmd_detach_client_entry,
	&cmd_attach_session_entry,
	NULL
};

int tmate_should_exec_cmd_locally(const struct cmd_entry *cmd)
{
	const struct cmd_entry **ptr;

	for (ptr = local_cmds; *ptr; ptr++)
		if (*ptr == cmd)
			return 1;
	return 0;
}

void tmate_client_cmd_str(int client_id, const char *cmd)
{
	tmate_debug("Remote cmd (cid=%d): %s", client_id, cmd);

	pack(array, 3);
	pack(int, TMATE_IN_EXEC_CMD_STR);
	pack(int, client_id);
	pack(string, cmd);
}

/*
 * tmux 3.6a: struct args is opaque. Use args_print() + cmd_get_entry()
 * to serialize, matching the client-side extract_cmd pattern.
 */
static void extract_cmd(struct cmd *cmd, int *_argc, char ***_argv)
{
	char *args_str;
	char *cmdline;
	const struct cmd_entry *entry = cmd_get_entry(cmd);

	args_str = args_print(cmd_get_args(cmd));
	if (args_str != NULL && *args_str != '\0')
		xasprintf(&cmdline, "%s %s", entry->name, args_str);
	else
		cmdline = xstrdup(entry->name);
	free(args_str);

	*_argv = cmd_copy_argv(1, (char *[]){cmdline});
	*_argc = 1;
	free(cmdline);
}

void tmate_client_cmd_args(int client_id, int argc, const char **argv)
{
	int i;

	pack(array, argc + 2);
	pack(int, TMATE_IN_EXEC_CMD);
	pack(int, client_id);

	for (i = 0; i < argc; i++)
		pack(string, argv[i]);
}

void tmate_client_cmd(int client_id, struct cmd *cmd)
{
	char *cmd_str;
	int argc;
	char **argv;

	extract_cmd(cmd, &argc, &argv);
	cmd_str = cmd_print(cmd);
	if (tmate_session->client_protocol_version < 6) {
		tmate_client_cmd_str(client_id, cmd_str);
		free(cmd_str);
		cmd_free_argv(argc, argv);
		return;
	}
	tmate_debug("Remote cmd (cid=%d): %s", client_id, cmd_str);
	free(cmd_str);

	tmate_client_cmd_args(client_id, argc, (const char **)argv);
	cmd_free_argv(argc, argv);
}

void tmate_client_set_active_pane(int client_id, int win_idx, int pane_id)
{
	char target[64];
	sprintf(target, "%d.%d", win_idx, pane_id);
	tmate_client_cmd_args(client_id, 3, (const char *[]){"select-pane", "-t", target});
}

void tmate_send_mc_obj(msgpack_object *obj)
{
	pack(object, *obj);
}

int tmate_intercept_input_key(int pid, key_code key)
{
	if (!tmate_session->input_mode_enabled)
		return 0;

	tmate_send_user_input(tmate_session, pid, -1, key);
	if (!tmate_session->input_mirror)
		return 1; /* suppress normal key processing */
	return 0;
}

void tmate_send_user_join(struct tmate_session *session,
			  int user_id, const char *name,
			  bool readonly, const char *type)
{
	_pack(&session->daemon_encoder, array, 5);
	_pack(&session->daemon_encoder, int, TMATE_IN_USER_JOIN);
	_pack(&session->daemon_encoder, int, user_id);
	_pack(&session->daemon_encoder, string, name);
	_pack(&session->daemon_encoder, boolean, readonly);
	_pack(&session->daemon_encoder, string, type);
}

void tmate_send_user_leave(struct tmate_session *session, int user_id)
{
	_pack(&session->daemon_encoder, array, 2);
	_pack(&session->daemon_encoder, int, TMATE_IN_USER_LEAVE);
	_pack(&session->daemon_encoder, int, user_id);
}

void tmate_send_user_input(struct tmate_session *session,
			   int user_id, int pane_id, key_code key)
{
	_pack(&session->daemon_encoder, array, 4);
	_pack(&session->daemon_encoder, int, TMATE_IN_USER_INPUT);
	_pack(&session->daemon_encoder, int, user_id);
	_pack(&session->daemon_encoder, int, pane_id);
	_pack(&session->daemon_encoder, unsigned_int, (unsigned int)key);
}
