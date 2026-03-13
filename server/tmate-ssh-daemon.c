#include <sys/stat.h>
#include "tmate.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>

struct tmate_session _tmate_session, *tmate_session = &_tmate_session;

extern void tmate_send_fin_to_ws_clients(struct tmate_session *session);

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

	/* Track PTY activity for idle timeout */
	session->last_pty_activity = time(NULL);

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
			tmate_info("daemon_encoder_write: FAILED (%zd bytes): %s",
				   len, ssh_get_error(session->ssh_client.session));
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
	client->channel_cb.channel_data_function = on_ssh_channel_read;
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
	if (s->session_token_rw_named)
		unlinkat(dirfd, s->session_token_rw_named, 0);
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

/*
 * Idle timeout / max lifetime timer.
 * Runs every 30s in the daemon event loop and terminates the session if:
 *  - idle_timeout > 0 and no PTY data for that many seconds with no viewers
 *  - max_lifetime > 0 and the session has been alive for that many seconds
 */
#define IDLE_CHECK_INTERVAL_SEC 30

static int count_all_viewers(struct tmate_session *session)
{
	int ssh_rw = 0, ssh_ro = 0, web = 0;
	struct client *c;
	struct ws_client *wc;

	TAILQ_FOREACH(c, &clients, entry) {
		if (!(c->flags & CLIENT_IDENTIFIED))
			continue;
		if (c->readonly)
			ssh_ro++;
		else
			ssh_rw++;
	}

	if (tmate_has_websocket()) {
		TAILQ_FOREACH(wc, &session->ws_clients, entry) {
			if (wc->handshake_done && !wc->is_post)
				web++;
		}
	}

	return ssh_rw + ssh_ro + web;
}

static void on_idle_timer(__unused evutil_socket_t fd,
			  __unused short what, void *arg)
{
	struct tmate_session *session = arg;
	time_t now = time(NULL);
	int max_lifetime = tmate_settings->max_lifetime;
	int idle_timeout = tmate_settings->idle_timeout;

	/* Check per-session TTL first (client-controlled) */
	if (session->link_ttl > 0 &&
	    now - session->session_start >= session->link_ttl) {
		tmate_info("Session TTL expired (%d seconds)",
			   session->link_ttl);
		tmate_notify("Session expired (TTL: %ds)",
			     session->link_ttl);
		tmate_send_fin_to_ws_clients(session);
		request_server_termination();
		return;
	}

	/* Check max lifetime (server-controlled) */
	if (max_lifetime > 0 &&
	    now - session->session_start >= max_lifetime) {
		tmate_info("Session exceeded max lifetime (%ds), terminating",
			   max_lifetime);
		tmate_notify("Session ended: max lifetime (%ds) reached",
			     max_lifetime);
		tmate_send_fin_to_ws_clients(session);
		request_server_termination();
		return;
	}

	/* Check idle timeout — only when no viewers are connected */
	if (idle_timeout > 0 &&
	    now - session->last_pty_activity >= idle_timeout &&
	    count_all_viewers(session) == 0) {
		tmate_info("Session idle for %ds with no viewers, terminating",
			   idle_timeout);
		tmate_notify("Session ended: idle timeout (%ds) with no viewers",
			     idle_timeout);
		tmate_send_fin_to_ws_clients(session);
		request_server_termination();
		return;
	}
}

static void start_idle_timer_event(struct tmate_session *session)
{
	if (session->ev_idle_timer)
		return; /* already running */

	if (session->session_start == 0) {
		session->session_start = time(NULL);
		session->last_pty_activity = session->session_start;
	}

	struct timeval tv = { IDLE_CHECK_INTERVAL_SEC, 0 };
	session->ev_idle_timer = event_new(session->ev_base, -1,
					   EV_PERSIST, on_idle_timer,
					   session);
	if (session->ev_idle_timer) {
		event_add(session->ev_idle_timer, &tv);
		tmate_info("Idle timer started (idle=%ds, lifetime=%ds, ttl=%ds)",
			   tmate_settings->idle_timeout,
			   tmate_settings->max_lifetime,
			   session->link_ttl);
	}
}

static void setup_idle_timer(struct tmate_session *session)
{
	if (tmate_settings->idle_timeout <= 0 &&
	    tmate_settings->max_lifetime <= 0)
		return;

	session->session_start = time(NULL);
	session->last_pty_activity = session->session_start;

	start_idle_timer_event(session);
}

/*
 * Ensure the idle timer is running. Called when per-session TTL
 * is set after session creation (the timer may not have been
 * started if the server has no global idle_timeout/max_lifetime).
 */
void tmate_ensure_idle_timer(struct tmate_session *session)
{
	start_idle_timer_event(session);
}

extern int server_fd;

/*
 * Register session tokens with the main process via the IPC socketpair.
 * The main process uses these to route incoming SSE connections.
 */
static void register_tokens_with_main(struct tmate_session *session)
{
	char msg[512];
	int n;

	if (session->ipc_fd < 0)
		return;

	n = snprintf(msg, sizeof(msg), "%s %s",
		     SSE_IPC_MSG_REGISTER, session->session_token);

	if (session->session_token_ro)
		n += snprintf(msg + n, sizeof(msg) - n, " %s",
			      session->session_token_ro);

	/* Named tokens are registered later via tmate_register_session_name */

	sse_ipc_send_msg(session->ipc_fd, msg);
	tmate_info("Registered tokens with main process via IPC");
}

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

	/* Allow viewer forks (running as nobody) to connect to the socket */
	chmod(socket_path, 0777);

	create_session_ro_symlink(session);

	/*
	 * Needed to initialize the database used in tty-term.c.
	 * We won't have access to it once in the jail.
	 */
	setup_ncurse(STDOUT_FILENO, "screen-256color");

	tmate_daemon_init(session);

	/*
	 * SSE multiplexing: the main process owns port 4002 and passes
	 * accepted SSE fds to us via the IPC socketpair. We register our
	 * tokens so the main process knows how to route connections.
	 *
	 * No need to call tmate_bind_websocket_socket() — the daemon
	 * no longer binds the SSE port directly.
	 */
	register_tokens_with_main(session);

	/* Open sessions dir fd before jail for post-jail named session symlinks */
	session->sessions_dir_fd = open(TMATE_WORKDIR "/sessions",
					O_RDONLY | O_DIRECTORY);

	{
		int keep_fds[7];
		int nfds = 0;
		keep_fds[nfds++] = session->tmux_socket_fd;
		keep_fds[nfds++] = ssh_get_fd(session->ssh_client.session);
		keep_fds[nfds++] = STDERR_FILENO;
		keep_fds[nfds++] = tmate_get_urandom_fd();
		if (session->ipc_fd >= 0)
			keep_fds[nfds++] = session->ipc_fd;
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

	/* Set up IPC receiver to accept SSE fds from main process */
	tmate_setup_ipc_receiver(session);

	/* Start idle/lifetime timer if configured */
	setup_idle_timer(session);

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
	struct stat st;
	char *rw_named, *ro_named;
	char prefix[6];
	char *ssh_conn_str;
	char *old_ro;
	char *actual_name;
	int suffix;

	if (!is_valid_session_name(name)) {
		tmate_notify("Invalid session name (3-32 alphanumeric/hyphens)");
		return;
	}

	if (session->sessions_dir_fd < 0) {
		tmate_notify("Named sessions unavailable (no sessions dir fd)");
		return;
	}

	/*
	 * Auto-increment: if "name" is taken, try "name-1", "name-2", etc.
	 * This supports multiple sessions sharing the same base name.
	 *
	 * Before auto-numbering, check if the existing symlink points to a
	 * dead socket. This happens when the client reconnects — the first
	 * daemon's atexit cleanup hasn't run yet, but the socket is already
	 * closed. Remove stale symlinks to let the reconnecting client
	 * reclaim its name.
	 */
	actual_name = xstrdup(name);
	xasprintf(&ro_named, "ro-%s", actual_name);

	/* Check if existing symlink is stale (points to dead socket) */
	if (fstatat(session->sessions_dir_fd, actual_name, &st,
		    AT_SYMLINK_NOFOLLOW) == 0) {
		char target[TMATE_TOKEN_LEN + 1];
		ssize_t len = readlinkat(session->sessions_dir_fd, actual_name,
					 target, sizeof(target) - 1);
		if (len > 0) {
			target[len] = '\0';
			/* Check if the target socket is alive */
			struct stat sock_st;
			if (fstatat(session->sessions_dir_fd, target, &sock_st,
				    0) < 0) {
				/* Target socket is dead — remove stale symlinks */
				char *stale_ro;
				xasprintf(&stale_ro, "ro-%s", actual_name);
				unlinkat(session->sessions_dir_fd, actual_name, 0);
				unlinkat(session->sessions_dir_fd, stale_ro, 0);
				/* Also remove RW symlink (XXXXX-name pattern) */
				/* We don't know the exact prefix, skip it —
				 * it will be cleaned by the dying daemon */
				free(stale_ro);
				tmate_info("Reclaimed stale session name: %s",
					   actual_name);
			}
		}
	}

	for (suffix = 1;
	     fstatat(session->sessions_dir_fd, actual_name, &st, AT_SYMLINK_NOFOLLOW) == 0 ||
	     fstatat(session->sessions_dir_fd, ro_named, &st, AT_SYMLINK_NOFOLLOW) == 0;
	     suffix++) {
		free(actual_name);
		free(ro_named);
		xasprintf(&actual_name, "%s-%d", name, suffix);
		if (!is_valid_session_name(actual_name)) {
			tmate_notify("Session name '%s' is too long to auto-number", name);
			free(actual_name);
			return;
		}
		xasprintf(&ro_named, "ro-%s", actual_name);
		if (suffix > 99) {
			tmate_notify("Too many sessions with name '%s'", name);
			free(actual_name);
			free(ro_named);
			return;
		}
	}

	name = actual_name;

	/* Generate RW token: <5 random digits>-<name> */
	snprintf(prefix, sizeof(prefix), "%05d",
		 (int)(labs(tmate_get_random_long()) % 100000));
	xasprintf(&rw_named, "%s-%s", prefix, name);

	/* Create web symlink: <name> -> <session_token> */
	if (symlinkat(session->session_token, session->sessions_dir_fd, name) < 0) {
		tmate_info("Named session symlink failed: %s", strerror(errno));
		tmate_notify("Named session unavailable (%s)", strerror(errno));
		free((char *)name);
		free(rw_named);
		free(ro_named);
		return;
	}

	/* Create RW symlink: <digits>-<name> -> <session_token> */
	if (symlinkat(session->session_token, session->sessions_dir_fd, rw_named) < 0) {
		tmate_info("RW named symlink failed: %s", strerror(errno));
		unlinkat(session->sessions_dir_fd, name, 0);
		tmate_notify("Named session unavailable (%s)", strerror(errno));
		free((char *)name);
		free(rw_named);
		free(ro_named);
		return;
	}

	/* Create RO symlink: ro-<name> -> <session_token> */
	if (symlinkat(session->session_token, session->sessions_dir_fd, ro_named) < 0)
		tmate_info("RO named symlink failed: %s", strerror(errno));

	/* Remove old random RO symlink */
	old_ro = (char *)session->session_token_ro;
	if (old_ro)
		unlinkat(session->sessions_dir_fd, old_ro, 0);

	/* Update session state */
	session->session_token_named = xstrdup(name);
	session->session_token_rw_named = rw_named;
	free(old_ro);
	session->session_token_ro = ro_named;

	tmate_info("Named session registered: %s -> %s",
		   name, session->obfuscated_session_token);

	/* Register named tokens with main process for SSE routing */
	if (session->ipc_fd >= 0) {
		char msg[256];
		snprintf(msg, sizeof(msg), "%s %s %s %s",
			 SSE_IPC_MSG_REGISTER, name, rw_named, ro_named);
		sse_ipc_send_msg(session->ipc_fd, msg);
	}

	tmate_set_env("tmtv_session_name", name);

	/*
	 * Only send URLs if tmate_ready() has already fired.
	 * During startup, tmate_ready() sends the final URLs
	 * after all config is processed. At runtime, we need
	 * to notify the client of the new URLs immediately.
	 */
	if (session->urls_sent) {
		ssh_conn_str = get_ssh_conn_string(rw_named);
		tmate_notify("ssh session: %s", ssh_conn_str);
		tmate_set_env("tmtv_ssh", ssh_conn_str);
		free(ssh_conn_str);

		ssh_conn_str = get_ssh_conn_string(ro_named);
		tmate_notify("ssh session read only: %s", ssh_conn_str);
		tmate_set_env("tmtv_ssh_ro", ssh_conn_str);
		free(ssh_conn_str);

		tmate_send_web_url(session);
	}

	free(actual_name);
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
	tmate_info("Web sharing toggled: %s", enabled ? "on" : "off");
	if (enabled) {
		if (tmate_session->urls_sent)
			tmate_send_web_url(tmate_session);
	} else {
		tmate_notify("Web sharing disabled");
		tmate_disconnect_ws_clients(tmate_session);
	}
}

static void handle_web_input_option(const char *name, const char *val)
{
	if (strcmp(name, "tmtv-web-input") != 0)
		return;

	bool enabled = val && (!strcmp(val, "on") || !strcmp(val, "1"));
	tmate_session->web_input_enabled = enabled;
	tmate_info("Web input toggled: %s", enabled ? "on" : "off");
	if (enabled)
		tmate_notify("Web input enabled — RW web viewers can type");
	else
		tmate_notify("Web input disabled");
}

static void handle_link_ttl_option(const char *name, const char *val)
{
	if (strcmp(name, "tmtv-link-ttl") != 0)
		return;

	int ttl = val ? atoi(val) : 0;
	tmate_session->link_ttl = ttl > 0 ? ttl : 0;
	if (ttl > 0) {
		tmate_info("Session TTL set via set-option: %d seconds", ttl);
		tmate_ensure_idle_timer(tmate_session);
	} else {
		tmate_info("Session TTL cleared via set-option");
	}
}

void tmate_hook_set_option(const char *name, const char *val)
{
	tmate_hook_set_option_auth(name, val);
	handle_session_name_options(name, val);
	handle_web_sharing_option(name, val);
	handle_web_input_option(name, val);
	handle_link_ttl_option(name, val);
}
