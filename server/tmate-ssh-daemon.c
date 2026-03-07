#include "tmate.h"
#include <errno.h>
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

	close_fds_except((int[]){session->tmux_socket_fd,
				 ssh_get_fd(session->ssh_client.session),
				 STDERR_FILENO}, 3);

	get_in_jail();

	event_reinit(session->ev_base);

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

static void handle_session_name_options(const char *name, __unused const char *val)
{
	if (tmate_has_websocket())
		return;

	if (!strcmp(name, "tmate-api-key") ||
	    !strcmp(name, "tmate-session-name") ||
	    !strcmp(name, "tmate-session-name-ro")) {
		static bool warned;
		if (!warned) {
			tmate_info("Named sessions are not supported (no websocket server)");
			tmate_notify("Named sessions are not supported (no websocket server)");
		}
		warned = true;
	}
}

void tmate_hook_set_option(const char *name, const char *val)
{
	tmate_hook_set_option_auth(name, val);
	handle_session_name_options(name, val);

}
