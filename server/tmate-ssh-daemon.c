#include <sys/stat.h>
#include "tmate.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>

struct tmate_session _tmate_session, *tmate_session = &_tmate_session;

static void on_daemon_decoder_read(void *userdata, struct tmate_unpacker *uk)
{
	struct tmate_session *session = userdata;
	tmate_dispatch_daemon_message(session, uk);
	tmate_send_websocket_daemon_msg(session, uk);
}

static int on_ssh_channel_read(__unused ssh_session _session,
			       __unused ssh_channel channel,
			       void *_data, uint32_t total_len,
			       __unused int is_stderr, void *userdata)
{
	struct tmate_session *session = userdata;
	char *data = _data;
	size_t written = 0;
	char *buf;
	size_t len;

	while (total_len) {
		tmate_decoder_get_buffer(&session->daemon_decoder, &buf, &len);

		if (len == 0)
			tmate_fatal("No more room in client decoder. Message too big?");

		if (len > total_len)
			len = total_len;

		memcpy(buf, data, len);

		tmate_decoder_commit(&session->daemon_decoder, len);

		total_len -= len;
		written += len;
		data += len;
	}

	return written;
}

static void on_daemon_encoder_write(void *userdata, struct evbuffer *buffer)
{
	struct tmate_session *session = userdata;
	ssize_t len, written;
	unsigned char *buf;

	for(;;) {
		len = evbuffer_get_length(buffer);
		if (!len)
			break;

		buf = evbuffer_pullup(buffer, -1);

		written = ssh_channel_write(session->ssh_client.channel, buf, len);
		if (written < 0) {
			tmate_info("Error writing to channel: %s",
				   ssh_get_error(session->ssh_client.session));
			request_server_termination();
			break;
		}

		evbuffer_drain(buffer, written);
	}
}

static void tmate_daemon_init(struct tmate_session *session)
{
	struct tmate_ssh_client *client = &session->ssh_client;

	memset(&client->channel_cb, 0, sizeof(client->channel_cb));
	ssh_callbacks_init(&client->channel_cb);
	client->channel_cb.userdata = session;
	client->channel_cb.channel_data_function = on_ssh_channel_read,
	ssh_set_channel_callbacks(client->channel, &client->channel_cb);

	tmate_encoder_init(&session->daemon_encoder, on_daemon_encoder_write, session);
	tmate_decoder_init(&session->daemon_decoder, on_daemon_decoder_read, session);

	tmate_init_websocket(session);
}

static void handle_sigterm(__unused int sig)
{
	request_server_termination();
}

static void cleanup_session_files(void)
{
	struct tmate_session *s = tmate_session;
	int dirfd = s->sessions_dir_fd;

	if (dirfd < 0)
		return;

	if (s->session_token)
		unlinkat(dirfd, s->session_token, 0);
	if (s->session_token_ro)
		unlinkat(dirfd, s->session_token_ro, 0);
	if (s->session_token_named)
		unlinkat(dirfd, s->session_token_named, 0);
}

/*
 * Crash handler for child (session) processes.
 * cleanup_session_files() only calls unlinkat() which is async-signal-safe.
 */
static void handle_crash_cleanup(int sig)
{
	cleanup_session_files();

	/* Re-raise with default handler to get proper exit status */
	signal(sig, SIG_DFL);
	raise(sig);
}

/* We skip letters that are hard to distinguish when reading */
static char rand_tmate_token_digits[] = "abcdefghjkmnpqrstuvwxyz"
				        "ABCDEFGHJKLMNPQRSTUVWXYZ"
				        "23456789";

#define NUM_DIGITS (sizeof(rand_tmate_token_digits) - 1)

static char *get_random_token(void)
{
	struct random_stream rs;
	char *token = xmalloc(TMATE_TOKEN_LEN + 1);
	int i;
	unsigned char c;

	random_stream_init(&rs);

	for (i = 0; i < TMATE_TOKEN_LEN; i++) {
		do {
			c = *random_stream_get(&rs, 1);
		} while (c >= NUM_DIGITS);

		token[i] = rand_tmate_token_digits[c];
	}

	token[i] = 0;

	return token;
}

static void create_session_ro_symlink(struct tmate_session *session)
{
	char *tmp, *token, *session_ro_path;

#ifdef DEVENV
	tmp = xstrdup("READONLYTOKENFORDEVENV000");
#else
	tmp = get_random_token();
#endif
	xasprintf(&token, "ro-%s", tmp);
	free(tmp);

	session->session_token_ro = token;

	xasprintf(&session_ro_path, TMATE_WORKDIR "/sessions/%s",
		  session->session_token_ro);

	unlink(session_ro_path);
	if (symlink(session->session_token, session_ro_path) < 0)
		tmate_fatal("Cannot create read-only symlink");
	free(session_ro_path);
}

extern int server_fd;

void tmate_spawn_daemon(struct tmate_session *session)
{
	struct tmate_ssh_client *client = &session->ssh_client;
	char *token;
	char *cause = NULL;
	int fd;

#ifdef DEVENV
	token = xstrdup("SUPERSECURETOKENFORDEVENV");
#else
	token = get_random_token();
#endif

	set_session_token(session, token);
	free(token);

	tmate_info("Spawning daemon ip=%s", client->ip_address);

	/*
	 * tmux 3.6a: server_create_socket(flags, &cause)
	 * Returns fd or -1.
	 */
	fd = server_create_socket(0, &cause);
	if (fd < 0) {
		if (cause) {
			tmate_fatal("Cannot create tmux socket: %s", cause);
			free(cause);
		} else
			tmate_fatal("Cannot create tmux socket");
	}
	session->tmux_socket_fd = fd;
	server_fd = fd;

	create_session_ro_symlink(session);

	/*
	 * Needed to initialize the database used in tty-term.c.
	 * We won't have access to it once in the jail.
	 */
	setup_ncurse(STDOUT_FILENO, "screen-256color");

	tmate_daemon_init(session);

	/* Bind SSE socket before jail (CLONE_NEWNET isolates network) */
	tmate_bind_websocket_socket(session);

	/* Open sessions dir fd before jail for post-jail named session symlinks */
	session->sessions_dir_fd = open(TMATE_WORKDIR "/sessions",
					O_RDONLY | O_DIRECTORY);

	{
		int keep_fds[5];
		int nfds = 0;
		keep_fds[nfds++] = session->tmux_socket_fd;
		keep_fds[nfds++] = ssh_get_fd(session->ssh_client.session);
		keep_fds[nfds++] = STDERR_FILENO;
		if (session->ws_listen_fd >= 0)
			keep_fds[nfds++] = session->ws_listen_fd;
		if (session->sessions_dir_fd >= 0)
			keep_fds[nfds++] = session->sessions_dir_fd;
		close_fds_except(keep_fds, nfds);
	}

	get_in_jail();
	atexit(cleanup_session_files);
	signal(SIGSEGV, handle_crash_cleanup);
	signal(SIGABRT, handle_crash_cleanup);

	event_reinit(session->ev_base);

	/* Re-create websocket encoder event on the new base (old one
	 * was orphaned by fork + event_reinit) */
	if (tmate_has_websocket())
		tmate_encoder_rebind(&session->websocket_encoder, session->ev_base);

	/* Start WebSocket listener after event_reinit (correct base) */
	tmate_start_websocket_listener(session);

	signal(SIGTERM, handle_sigterm);

	/*
	 * tmux 3.6a: server_start(proc, flags, base, lockfd, lockfile)
	 * We pass NULL for proc (standalone server), CLIENT_NOFORK flags,
	 * the session's event base, -1 for no lock fd, NULL for no lock file.
	 */
	server_start(NULL, CLIENT_NOFORK, session->ev_base, -1, NULL);
	/* never reached */
}

static bool is_valid_session_name(const char *name)
{
	size_t len = strlen(name);
	if (len < 3 || len > 32)
		return false;
	for (const char *c = name; *c; c++) {
		if (!isalnum((unsigned char)*c) && *c != '-')
			return false;
	}
	return true;
}

extern void tmate_send_web_url(struct tmate_session *session);

void tmate_register_session_name(struct tmate_session *session,
				 const char *name)
{
	char *named_path;
	struct stat st;
	bool created = false;

	if (!is_valid_session_name(name)) {
		tmate_notify("Invalid session name (3-32 alphanumeric/hyphens)");
		return;
	}

	if (session->sessions_dir_fd < 0) {
		tmate_notify("Named sessions unavailable (no sessions dir fd)");
		return;
	}

	/* Check if name is taken */
	if (fstatat(session->sessions_dir_fd, name, &st, AT_SYMLINK_NOFOLLOW) == 0) {
		tmate_notify("Session name '%s' is already taken", name);
		return;
	}

	/* Create symlink via pre-jail fd (survives chroot + namespace) */
	unlinkat(session->sessions_dir_fd, name, 0);
	if (symlinkat(session->session_token, session->sessions_dir_fd, name) < 0) {
		tmate_info("Named session symlink failed: %s", strerror(errno));
		tmate_notify("Named session unavailable (%s)", strerror(errno));
		return;
	}

	session->session_token_named = xstrdup(name);
	tmate_info("Named session registered: %s -> %s",
		   name, session->obfuscated_session_token);
	tmate_notify("Session name set: %s", name);

	/* Send updated URLs using the named token */
	tmate_set_env("tmtv_session_name", name);

	char *ssh_conn_str;
	ssh_conn_str = get_ssh_conn_string(name);
	tmate_set_env("tmtv_ssh", ssh_conn_str);
	free(ssh_conn_str);

	if (session->web_sharing_enabled)
		tmate_send_web_url(session);
}

static void handle_session_name_options(const char *name,
					__unused const char *val)
{
	/* Named sessions via set-option are handled through tmate_set_val
	 * transport (session_name key in tmate_set). This handler is only
	 * for the server-side set-option path which doesn't apply. */
	(void)name;
}

extern void tmate_disconnect_ws_clients(struct tmate_session *session);

static void handle_web_sharing_option(const char *name, const char *val)
{
	if (strcmp(name, "tmtv-web-sharing") != 0)
		return;

	bool enabled = val && (!strcmp(val, "on") || !strcmp(val, "1"));
	tmate_session->web_sharing_enabled = enabled;
	if (enabled) {
		tmate_send_web_url(tmate_session);
	} else {
		tmate_notify("Web sharing disabled");
		tmate_disconnect_ws_clients(tmate_session);
	}
}

void tmate_hook_set_option(const char *name, const char *val)
{
	tmate_hook_set_option_auth(name, val);
	handle_session_name_options(name, val);
	handle_web_sharing_option(name, val);
}
