#include <libssh/server.h>
#include <errno.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <signal.h>
#include "tmate.h"

extern int server_fd;

static void ssh_echo(struct tmate_ssh_client *ssh_client, const char *str);

static int on_ssh_channel_read(__unused ssh_session _session,
			       __unused ssh_channel channel,
			       void *_data, uint32_t total_len,
			       __unused int is_stderr, void *userdata)
{
	struct tmate_session *session = userdata;
	char *data = _data;
	size_t written = 0;
	ssize_t len;

	if (session->readonly) {
		/* Let read-only viewers quit with Ctrl-Q */
		if (total_len == 1 && *(unsigned char *)data == 0x11) {
			ssh_echo(&session->ssh_client,
				 "\r\nDisconnecting read-only session.\r\n");
			ssh_channel_close(session->ssh_client.channel);
		}
		return total_len;
	}

	/*
	 * Write to PTY in non-blocking mode. Avoids two fcntl() syscalls
	 * per keystroke that the old setblocking(1)/setblocking(0) pattern
	 * incurred. EAGAIN is retried with a short poll() to avoid spinning.
	 */
	while (total_len) {
		len = write(session->pty, data, total_len);
		if (len < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				struct pollfd pfd = {
					.fd = session->pty,
					.events = POLLOUT
				};
				poll(&pfd, 1, 100);
				continue;
			}
			tmate_fatal("Error writing to pty");
		}

		total_len -= len;
		written += len;
		data += len;
	}

	return written;
}

static int on_pty_window_change(__unused ssh_session _session,
				__unused ssh_channel channel,
				int width, int height,
				__unused int pxwidth, __unused int pxheight,
				void *userdata)
{
	struct tmate_session *session = userdata;
	struct winsize ws;

	ws.ws_col = width;
	ws.ws_row = height;

	ioctl(session->pty, TIOCSWINSZ, &ws);
	kill(0, SIGWINCH);

	return 0;
}

static void on_pty_event(struct tmate_session *session)
{
	ssize_t len, written;
	char buf[4096];

	for (;;) {
		len = read(session->pty, buf, sizeof(buf));
		if (len < 0) {
			if (errno == EAGAIN)
				return;
			tmate_fatal("pty read error");
		}

		if (len == 0)
			tmate_fatal("pty reached EOF");

		written = ssh_channel_write(session->ssh_client.channel, buf, len);
		if (written < 0)
			tmate_fatal("Error writing to channel: %s",
				    ssh_get_error(session->ssh_client.session));
		if (len != written)
			tmate_fatal("Cannot write %d bytes, wrote %d",
				    (int)len, (int)written);
	}
}

static void __on_pty_event(__unused evutil_socket_t fd, __unused short what, void *arg)
{
	on_pty_event(arg);
}

static void tmate_flush_pty(struct tmate_session *session)
{
	on_pty_event(session);
	close(session->pty);
}

static void tmate_client_pty_init(struct tmate_session *session)
{
	struct tmate_ssh_client *client = &session->ssh_client;

	ioctl(session->pty, TIOCSWINSZ, &session->ssh_client.winsize_pty);

	memset(&client->channel_cb, 0, sizeof(client->channel_cb));
	ssh_callbacks_init(&client->channel_cb);
	client->channel_cb.userdata = session;
	client->channel_cb.channel_data_function = on_ssh_channel_read;
	client->channel_cb.channel_pty_window_change_function = on_pty_window_change;
	ssh_set_channel_callbacks(client->channel, &client->channel_cb);

	setblocking(session->pty, 0);
	/*
	 * tmux 3.6a / libevent2: ev_pty is a pointer (struct event *)
	 */
	session->ev_pty = event_new(session->ev_base, session->pty,
				    EV_READ | EV_PERSIST, __on_pty_event, session);
	event_add(session->ev_pty, NULL);
}

static void random_sleep(void)
{
	struct timespec ts;
	ts.tv_sec = 0;
	ts.tv_nsec = 50000000 + (tmate_get_random_long() % 150000000);
	nanosleep(&ts, NULL);
}

#define BAD_TOKEN_ERROR_STR						\
"Invalid session token"						 "\r\n"

#define EXPIRED_TOKEN_ERROR_STR						\
"Invalid or expired session token"				 "\r\n"

static void ssh_echo(struct tmate_ssh_client *ssh_client,
		     const char *str)
{
	ssh_channel_write(ssh_client->channel, str, strlen(str));
}


/*
 * Note: get_socket_path() replaces '/' and '.' by '_' to
 * avoid wondering around the file system.
 */
static char valid_digits[] = "abcdefghijklmnopqrstuvwxyz"
                             "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
			     "0123456789-_/";

int tmate_validate_session_token(const char *token)
{
	int len;
	int i;

	len = strlen(token);
	if (len <= 2)
		return -1;

	for (i = 0; i < len; i++) {
		if (!strchr(valid_digits, token[i]))
			return -1;
	}

	return 0;
}

void tmate_spawn_pty_client(struct tmate_session *session)
{
	struct tmate_ssh_client *client = &session->ssh_client;
	char *argv_rw[] = {(char *)"attach", NULL};
	char *argv_ro[] = {(char *)"attach", (char *)"-r", NULL};
	char **argv = argv_rw;
	int argc = 1;
	char *token = client->username;
	struct stat fstat;
	int slave_pty;
	int ret;

	if (tmate_validate_session_token(token) < 0) {
		ssh_echo(client, BAD_TOKEN_ERROR_STR);
		tmate_fatal("Invalid token ip=%s", client->ip_address);
	}

	set_session_token(session, token);

	tmate_info("Spawning pty client ip=%s", client->ip_address);

	/*
	 * tmux 3.6a: client_connect() doesn't exist as a public function.
	 * Connect to the tmux socket directly.
	 */
	{
		struct sockaddr_un sa;
		int sock_fd;

		memset(&sa, 0, sizeof(sa));
		sa.sun_family = AF_UNIX;
		strlcpy(sa.sun_path, socket_path, sizeof(sa.sun_path));

		sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
		if (sock_fd < 0)
			goto connect_failed;

		if (connect(sock_fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
			close(sock_fd);
			goto connect_failed;
		}

		session->tmux_socket_fd = sock_fd;
		server_fd = sock_fd;
		goto connect_ok;

connect_failed:
		random_sleep(); /* for making timing attacks harder */
		ssh_echo(client, EXPIRED_TOKEN_ERROR_STR);
		tmate_fatal("Expired token ip=%s", client->ip_address);
	}
connect_ok:

	/*
	 * If we are connecting through a symlink, it means that we are a
	 * readonly client.
	 * 1) We mark the client as CLIENT_READONLY on the server
	 * 2) We prevent any input (aside from the window size) to go through
	 *    to the server.
	 */
	/*
	 * Read-only tokens always have the "ro-" prefix.  Named session
	 * tokens are also symlinks but should be read-write, so check
	 * the prefix instead of the symlink status.
	 */
	session->readonly = (strncmp(token, "ro-", 3) == 0);
	if (session->readonly) {
		argv = argv_ro;
		argc = 2;
		ssh_echo(client,
			 "Read-only session. Press Ctrl-Q to disconnect.\r\n");
	}

	if (openpty(&session->pty, &slave_pty, NULL, NULL, NULL) < 0)
		tmate_fatal("Cannot allocate pty");

	dup2(slave_pty, STDIN_FILENO);
	dup2(slave_pty, STDOUT_FILENO);
	dup2(slave_pty, STDERR_FILENO);

	setup_ncurse(slave_pty, "xterm-256color");
	setenv("TERM", "xterm-256color", 1);

	tmate_client_pty_init(session);

	close_fds_except((int[]){STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO,
				 session->tmux_socket_fd,
				 ssh_get_fd(session->ssh_client.session),
				 session->pty}, 6);
	/*
	 * Viewer forks don't enter the jail. They are already sandboxed:
	 * only 6 fds open, and they only proxy between SSH and the tmux
	 * socket. Skipping chroot avoids needing terminfo files in the jail.
	 */
	event_reinit(session->ev_base);

	/*
	 * tmux 3.6a: client_main(base, argc, argv, flags, feat)
	 * Added features parameter (0 = none).
	 */
	ret = client_main(session->ev_base, argc, argv,
			  CLIENT_UTF8, 0);
	tmate_flush_pty(session);
	exit(ret);
}
