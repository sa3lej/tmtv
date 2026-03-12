#include <libssh/libssh.h>
#include <libssh/server.h>
#include <libssh/callbacks.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <netinet/tcp.h>
#include <sys/wait.h>
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <signal.h>
#include <poll.h>
#include <event2/event.h>
#include <arpa/inet.h>
#ifndef IPPROTO_TCP
#include <netinet/in.h>
#endif

#include <time.h>
#include "tmate.h"

/*
 * Connection limits (CVE-2021-44512 DoS mitigation)
 */
#define MAX_CHILDREN       100
#define RATE_WINDOW_SEC    30
#define RATE_MAX_PER_IP    10
#define CONN_HISTORY_SIZE  512

struct conn_record {
	char ip[64];
	time_t ts;
};

static struct conn_record conn_history[CONN_HISTORY_SIZE];
static int conn_history_pos;

static bool check_rate_limit(const char *ip)
{
	time_t now = time(NULL);
	int count = 0;

	for (int i = 0; i < CONN_HISTORY_SIZE; i++) {
		if (conn_history[i].ts > 0 &&
		    now - conn_history[i].ts < RATE_WINDOW_SEC &&
		    strcmp(conn_history[i].ip, ip) == 0)
			count++;
	}

	strlcpy(conn_history[conn_history_pos].ip, ip,
		sizeof(conn_history[0].ip));
	conn_history[conn_history_pos].ts = now;
	conn_history_pos = (conn_history_pos + 1) % CONN_HISTORY_SIZE;

	return count < RATE_MAX_PER_IP;
}

static int get_peer_ip(int fd, char *dst, size_t len)
{
	struct sockaddr_storage sa;
	socklen_t sa_len = sizeof(sa);

	if (getpeername(fd, (struct sockaddr *)&sa, &sa_len) < 0)
		return -1;

	switch (sa.ss_family) {
	case AF_INET:
		if (!inet_ntop(AF_INET,
			       &((struct sockaddr_in *)&sa)->sin_addr,
			       dst, len))
			return -1;
		break;
	case AF_INET6:
		if (!inet_ntop(AF_INET6,
			       &((struct sockaddr_in6 *)&sa)->sin6_addr,
			       dst, len))
			return -1;
		break;
	default:
		return -1;
	}
	return 0;
}

/*
 * SSH algorithm policy — modern algorithms only.
 * Reviewed 2026-03-08 against libssh 0.10.x capabilities.
 */
#define TMTV_SSH_CIPHERS \
	"chacha20-poly1305@openssh.com," \
	"aes256-gcm@openssh.com," \
	"aes128-gcm@openssh.com," \
	"aes256-ctr," \
	"aes192-ctr," \
	"aes128-ctr"

#define TMTV_SSH_KEX \
	"curve25519-sha256," \
	"curve25519-sha256@libssh.org," \
	"ecdh-sha2-nistp256," \
	"ecdh-sha2-nistp384," \
	"ecdh-sha2-nistp521," \
	"diffie-hellman-group18-sha512," \
	"diffie-hellman-group16-sha512"

#define TMTV_SSH_HOSTKEYS \
	"ssh-ed25519," \
	"ecdsa-sha2-nistp256," \
	"ecdsa-sha2-nistp384," \
	"ecdsa-sha2-nistp521," \
	"rsa-sha2-512," \
	"rsa-sha2-256"

char *get_ssh_conn_string(const char *session_token)
{
	char port_arg[16] = {0};
	char *ret;

	int ssh_port_advertized = tmate_settings->ssh_port_advertized == -1 ?
		tmate_settings->ssh_port :
		tmate_settings->ssh_port_advertized;

	if (ssh_port_advertized != 22)
		sprintf(port_arg, " -p%d", ssh_port_advertized);
	xasprintf(&ret, "ssh%s %s@%s", port_arg, session_token, tmate_settings->tmate_host);
	return ret;
}

static int pty_request(__unused ssh_session session,
		       __unused ssh_channel channel,
		       __unused const char *term,
		       int width, int height,
		       __unused int pxwidth, __unused int pwheight,
		       void *userdata)
{
	struct tmate_ssh_client *client = userdata;

	client->winsize_pty.ws_col = width;
	client->winsize_pty.ws_row = height;

	return 0;
}

static int shell_request(__unused ssh_session session,
			 __unused ssh_channel channel,
			 void *userdata)
{
	struct tmate_ssh_client *client = userdata;

	if (client->role)
		return 1;

	client->role = TMATE_ROLE_PTY_CLIENT;

	return 0;
}

static int subsystem_request(__unused ssh_session session,
			     __unused ssh_channel channel,
			     const char *subsystem, void *userdata)
{
	struct tmate_ssh_client *client = userdata;

	if (client->role)
		return 1;

	if (!strcmp(subsystem, "tmate"))
		client->role = TMATE_ROLE_DAEMON;

	return 0;
}

static int exec_request(__unused ssh_session session,
			__unused ssh_channel channel,
			__unused const char *command,
			__unused void *userdata)
{
	/* Exec role required external websocket server (removed) */
	return 1;
}

static ssh_channel channel_open_request_cb(ssh_session session, void *userdata)
{
	struct tmate_ssh_client *client = userdata;

	if (!client->username) {
		/* The authentication did not go through yet */
		return NULL;
	}

	if (client->channel) {
		/*
		 * We already have a channel, and we don't support multi
		 * channels yet. Returning NULL means the channel request will
		 * be denied.
		 */
		return NULL;
	}

	client->channel = ssh_channel_new(session);
	if (!client->channel)
		tmate_fatal("Error getting channel");

	memset(&client->channel_cb, 0, sizeof(client->channel_cb));
	ssh_callbacks_init(&client->channel_cb);
	client->channel_cb.userdata = client;
	client->channel_cb.channel_pty_request_function = pty_request;
	client->channel_cb.channel_shell_request_function = shell_request;
	client->channel_cb.channel_subsystem_request_function = subsystem_request;
	client->channel_cb.channel_exec_request_function = exec_request;
	ssh_set_channel_callbacks(client->channel, &client->channel_cb);

	return client->channel;
}

static int auth_pubkey_cb(__unused ssh_session session,
			  const char *user,
			  ssh_key pubkey,
			  char signature_state, void *userdata)
{
	struct tmate_ssh_client *client = userdata;

	switch (signature_state) {
	case SSH_PUBLICKEY_STATE_VALID:
		/* If session requires a password, deny pubkey auth —
		 * force the client to use password authentication */
		if (tmate_session_requires_password(user)) {
			tmate_debug("Auth denied (pubkey): password required "
				    "ip=%s user=%s", client->ip_address, user);
			return SSH_AUTH_DENIED;
		}

		free(client->username);
		client->username = xstrdup(user);

		const char *key_type = ssh_key_type_to_char(ssh_key_type(pubkey));

		char *b64_key;
		if (ssh_pki_export_pubkey_base64(pubkey, &b64_key) != SSH_OK)
			tmate_fatal("error getting public key");

		char *pubkey64;
		xasprintf(&pubkey64, "%s %s", key_type, b64_key);
		free(b64_key);

		if (!would_tmate_session_allow_auth(user, pubkey64)) {
			tmate_info("Auth denied: ip=%s user=%s",
				   client->ip_address, user);
			free(pubkey64);
			return SSH_AUTH_DENIED;
		}

		client->pubkey = pubkey64;

		return SSH_AUTH_SUCCESS;
	case SSH_PUBLICKEY_STATE_NONE:
		return SSH_AUTH_SUCCESS;
	default:
		tmate_info("Auth denied: ip=%s user=%s state=%d",
			   client->ip_address, user, signature_state);
		return SSH_AUTH_DENIED;
	}
}

static int auth_none_cb(__unused ssh_session session, const char *user, void *userdata)
{
	struct tmate_ssh_client *client = userdata;

	/* If session has a password, deny none-auth to force password prompt */
	if (tmate_session_requires_password(user)) {
		tmate_debug("Auth denied (none): password required ip=%s user=%s",
			    client->ip_address, user);
		return SSH_AUTH_DENIED;
	}

	if (!would_tmate_session_allow_auth(user, NULL)) {
		tmate_info("Auth denied (none): ip=%s user=%s",
			   client->ip_address, user);
		return SSH_AUTH_DENIED;
	}

	free(client->username);
	client->username = xstrdup(user);
	client->pubkey = NULL;

	return SSH_AUTH_SUCCESS;
}

static int auth_password_cb(__unused ssh_session session,
			    const char *user,
			    const char *password, void *userdata)
{
	struct tmate_ssh_client *client = userdata;

	if (!would_tmate_session_allow_password(user, password)) {
		tmate_info("Auth denied (password): ip=%s user=%s",
			   client->ip_address, user);
		return SSH_AUTH_DENIED;
	}

	free(client->username);
	client->username = xstrdup(user);
	client->pubkey = NULL;

	return SSH_AUTH_SUCCESS;
}

static struct ssh_server_callbacks_struct ssh_server_cb = {
	.auth_pubkey_function = auth_pubkey_cb,
	.auth_none_function = auth_none_cb,
	.auth_password_function = auth_password_cb,
	.channel_open_request_session_function = channel_open_request_cb,
};

static void on_ssh_read(__unused evutil_socket_t fd, __unused short what, void *arg)
{
	struct tmate_ssh_client *client = arg;
	ssh_execute_message_callbacks(client->session);

	if (!ssh_is_connected(client->session)) {
		tmate_debug("ssh disconnected");

		/*
		 * tmux 3.6a / libevent2: ev_ssh is a pointer (struct event *)
		 */
		if (client->ev_ssh) {
			event_del(client->ev_ssh);
			event_free(client->ev_ssh);
			client->ev_ssh = NULL;
		}

		/* For graceful tmux client termination */
		request_server_termination();
	}
}

static void register_on_ssh_read(struct tmate_ssh_client *client)
{
	/*
	 * tmux 3.6a / libevent2: use event_new() instead of event_set()
	 */
	client->ev_ssh = event_new(tmate_session->ev_base,
				   ssh_get_fd(client->session),
				   EV_READ | EV_PERSIST,
				   on_ssh_read, client);
	event_add(client->ev_ssh, NULL);
}

static void handle_sigalrm(__unused int sig)
{
	tmate_fatal_quiet("Connection grace period (%ds) passed", TMATE_SSH_GRACE_PERIOD);
}

static void client_bootstrap(struct tmate_session *_session)
{
	struct tmate_ssh_client *client = &_session->ssh_client;
	long grace_period = TMATE_SSH_GRACE_PERIOD;
	ssh_event mainloop;
	ssh_session session = client->session;

	tmate_debug("Bootstrapping ssh client ip=%s", client->ip_address);

	_session->ev_base = osdep_event_init();

	/* new process group, we don't want to die with our parent (upstart) */
	setpgid(0, 0);

	{
	int flag = 1;
	int fd = ssh_get_fd(session);
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
	flag = 0x10;  /* IPTOS_LOWDELAY */
	setsockopt(fd, IPPROTO_IP, IP_TOS, &flag, sizeof(flag));
	}

	/* WebSocket listener is started later in tmate_spawn_daemon() */

	ssh_server_cb.userdata = client;
	ssh_callbacks_init(&ssh_server_cb);
	ssh_set_server_callbacks(client->session, &ssh_server_cb);

	ssh_options_set(session, SSH_OPTIONS_TIMEOUT, &grace_period);
	ssh_options_set(session, SSH_OPTIONS_COMPRESSION, "yes");
	ssh_options_set(session, SSH_OPTIONS_CIPHERS_C_S, TMTV_SSH_CIPHERS);
	ssh_options_set(session, SSH_OPTIONS_CIPHERS_S_C, TMTV_SSH_CIPHERS);
	ssh_options_set(session, SSH_OPTIONS_KEY_EXCHANGE, TMTV_SSH_KEX);
	ssh_options_set(session, SSH_OPTIONS_HOSTKEYS, TMTV_SSH_HOSTKEYS);

	ssh_set_auth_methods(client->session, SSH_AUTH_METHOD_NONE |
					      SSH_AUTH_METHOD_PUBLICKEY |
					      SSH_AUTH_METHOD_PASSWORD);

	tmate_info("Exchanging DH keys ip=%s", client->ip_address);
	if (ssh_handle_key_exchange(session) < 0)
		tmate_fatal("Error doing the key exchange: %s", ssh_get_error(session));
	tmate_info("Key exchange done ip=%s", client->ip_address);

	mainloop = ssh_event_new();
	ssh_event_add_session(mainloop, session);

	while (!client->role) {
		if (ssh_event_dopoll(mainloop, -1) == SSH_ERROR)
			tmate_fatal("Error polling ssh socket: %s", ssh_get_error(session));
	}

	alarm(0);

	/* The latency callback is set later */
	start_keepalive_timer(client, TMATE_SSH_KEEPALIVE_SEC * 1000);
	register_on_ssh_read(client);

	switch (client->role) {
	case TMATE_ROLE_DAEMON:		tmate_spawn_daemon(_session);		break;
	case TMATE_ROLE_PTY_CLIENT:	tmate_spawn_pty_client(_session);	break;
	case TMATE_ROLE_EXEC:		tmate_spawn_exec(_session);		break;
	}
	/* never reached */
}

static int get_client_ip_socket(int fd, char *dst, size_t len)
{
	struct sockaddr_storage sa;
	socklen_t sa_len = sizeof(sa);

	if (getpeername(fd, (struct sockaddr *)&sa, &sa_len) < 0)
		return -1;

	switch (((struct sockaddr *)&sa)->sa_family) {
	case AF_INET:
		if (!inet_ntop(AF_INET, &((struct sockaddr_in *)&sa)->sin_addr,
			       dst, len))
			return -1;
		break;
	case AF_INET6:
		if (!inet_ntop(AF_INET6, &((struct sockaddr_in6 *)&sa)->sin6_addr,
			       dst, len))
			return -1;
		break;
	default:
		return -1;
	}

	return 0;
}

static int read_single_line(int fd, char *dst, size_t len)
{
	/*
	 * This reads exactly one line from fd.
	 * We cannot read bytes after the new line.
	 * We could use recv() with MSG_PEEK to do this more efficiently.
	 */
	for (size_t i = 0; i < len; i++) {
		if (read(fd, &dst[i], 1) <= 0)
			break;

		if (dst[i] == '\r')
			i--;

		if (dst[i] == '\n') {
			dst[i] = '\0';
			return i;
		}
	}

	return -1;
}

static int get_client_ip_proxy_protocol(int fd, char *dst, size_t len)
{
	char header[110];
	int tok_num;

#define SIGNATURE "PROXY "
	ssize_t ret = read(fd, header, sizeof(SIGNATURE)-1);
	if (ret <= 0)
		tmate_fatal_quiet("Disconnected, health checker?");
	if (ret != sizeof(SIGNATURE)-1)
		return -1;
	if (memcmp(header, SIGNATURE, sizeof(SIGNATURE)-1))
		return -1;
#undef SIGNATURE

	if (read_single_line(fd, header, sizeof(header)) < 0)
		return -1;

	tmate_debug("proxy header: %s", header);

	tok_num = 0;
	for (char *tok = strtok(header, " "); tok; tok = strtok(NULL, " "), tok_num++) {
		if (tok_num == 1)
			strlcpy(dst, tok, len);
	}

	if (tok_num != 5)
		return -1;

	return 0;
}

static int get_client_ip(int fd, char *dst, size_t len)
{
	if (tmate_settings->use_proxy_protocol)
		return get_client_ip_proxy_protocol(fd, dst, len);
	else
		return get_client_ip_socket(fd, dst, len);
}

static void ssh_log_function(int priority, const char *function,
			     const char *buffer, __unused void *userdata)
{
	/* tmux 3.6a: use log_debug instead of log_emit */
	tmate_debug("[ssh:%s] %s", function, buffer);
}

static inline int max_int(int a, int b)
{
	if (a < b)
		return b;
	return a;
}

static void ssh_import_key(ssh_bind bind, const char *keys_dir, const char *name)
{
	char path[PATH_MAX];
	ssh_key key = NULL;

	snprintf(path, sizeof(path), "%s/%s", keys_dir, name);

	if (access(path, F_OK) < 0)
		return;

	tmate_info("Loading key %s", path);

	if (ssh_pki_import_privkey_file(path, NULL, NULL, NULL, &key) != SSH_OK ||
	    key == NULL) {
		tmate_info("Failed to load key %s", path);
		return;
	}
	ssh_bind_options_set(bind, SSH_BIND_OPTIONS_IMPORT_KEY, key);
}

static ssh_bind prepare_ssh(const char *keys_dir, const char *bind_addr, int port)
{
	ssh_bind bind;
	int ssh_log_level;

	ssh_log_level = SSH_LOG_WARNING + max_int(log_get_level(), 0);

	ssh_set_log_callback(ssh_log_function);

	bind = ssh_bind_new();
	if (!bind)
		tmate_fatal("Cannot initialize ssh");

	/* Explicitly parse configuration to avoid automatic configuration file
	 * loading which could override options */
	ssh_bind_options_parse_config(bind, NULL);

	if (bind_addr)
		ssh_bind_options_set(bind, SSH_BIND_OPTIONS_BINDADDR, bind_addr);
	ssh_bind_options_set(bind, SSH_BIND_OPTIONS_BINDPORT, &port);
	ssh_bind_options_set(bind, SSH_BIND_OPTIONS_BANNER, TMATE_SSH_BANNER);
	ssh_bind_options_set(bind, SSH_BIND_OPTIONS_LOG_VERBOSITY, &ssh_log_level);
	ssh_bind_options_set(bind, SSH_BIND_OPTIONS_CIPHERS_C_S, TMTV_SSH_CIPHERS);
	ssh_bind_options_set(bind, SSH_BIND_OPTIONS_CIPHERS_S_C, TMTV_SSH_CIPHERS);
	ssh_bind_options_set(bind, SSH_BIND_OPTIONS_KEY_EXCHANGE, TMTV_SSH_KEX);

	ssh_import_key(bind, keys_dir, "ssh_host_rsa_key");
	ssh_import_key(bind, keys_dir, "ssh_host_ed25519_key");

	if (ssh_bind_listen(bind) < 0)
		tmate_fatal("Error listening to socket: %s", ssh_get_error(bind));

	tmate_info("Accepting connections on %s:%d", bind_addr ?: "", port);

	return bind;
}

static volatile sig_atomic_t pending_gc = 0;
static volatile sig_atomic_t active_children = 0;
static time_t server_start_time;

int tmate_server_get_active_sessions(void)
{
	return (int)active_children;
}

time_t tmate_server_get_start_time(void)
{
	return server_start_time;
}

/* Dead child pids collected in signal handler, processed in main loop.
 * Must be at least MAX_CHILDREN to avoid overflow if all children die
 * between poll() iterations. */
static volatile pid_t dead_pids[MAX_CHILDREN];
static volatile sig_atomic_t dead_pid_count = 0;

static void handle_sigchld(__unused int sig)
{
	int status;
	pid_t pid;

	while ((pid = waitpid(WAIT_ANY, &status, WNOHANG)) > 0) {
		if (active_children > 0)
			active_children--;
		if (dead_pid_count < MAX_CHILDREN)
			dead_pids[dead_pid_count++] = pid;
	}

	/* Schedule GC for next iteration of accept loop */
	pending_gc = 1;
}

/*
 * Garbage-collect stale session entries.
 *
 * For sockets: try connect(); if ECONNREFUSED or ENOENT, the session
 * is dead — remove it.  For symlinks: stat() to check if the target
 * (the socket) still exists; if dangling, remove.
 *
 * Safety: connect() to a live UNIX socket + immediate close is harmless.
 * tmux just sees a client connect and disconnect — this happens routinely
 * (e.g. `tmux list-sessions`).
 */
static void gc_stale_sessions(void)
{
	DIR *dir;
	struct dirent *ent;
	char path[PATH_MAX];
	struct stat st;
	int removed = 0;

	dir = opendir(TMATE_WORKDIR "/sessions");
	if (!dir)
		return;

	while ((ent = readdir(dir)) != NULL) {
		if (ent->d_name[0] == '.')
			continue;

		snprintf(path, sizeof(path), TMATE_WORKDIR "/sessions/%s",
			 ent->d_name);

		if (lstat(path, &st) < 0) {
			unlink(path);
			removed++;
			continue;
		}

		if (S_ISLNK(st.st_mode)) {
			/* Symlink (ro-TOKEN or named session) — check target */
			if (stat(path, &st) < 0) {
				/* Dangling symlink: target gone */
				unlink(path);
				removed++;
			}
			continue;
		}

		if (S_ISSOCK(st.st_mode)) {
			/* Socket — try connect to check liveness */
			struct sockaddr_un addr;
			int sock;

			sock = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
			if (sock < 0)
				continue;

			memset(&addr, 0, sizeof(addr));
			addr.sun_family = AF_UNIX;
			strlcpy(addr.sun_path, path, sizeof(addr.sun_path));

			if (connect(sock, (struct sockaddr *)&addr,
				    sizeof(addr)) < 0 &&
			    errno != EINPROGRESS && errno != EAGAIN) {
				/* ECONNREFUSED / ENOENT = dead */
				close(sock);
				unlink(path);
				removed++;
			} else {
				close(sock);
			}
			continue;
		}

		/* Unknown file type — remove */
		unlink(path);
		removed++;
	}

	closedir(dir);

	if (removed)
		tmate_info("Session GC: removed %d stale entries", removed);
}

/*
 * Process IPC registration messages from daemon children.
 * Format: "REG <token1> <token2> ...\n"
 * Each token is registered in the SSE registry for routing.
 */
static void handle_ipc_registrations(struct sse_registry *reg,
				     int ipc_fd, pid_t pid)
{
	char buf[512];
	int n;

	n = sse_ipc_read_msg(ipc_fd, buf, sizeof(buf));
	if (n <= 0) {
		/* Daemon closed the IPC fd — clean up */
		tmate_debug("IPC fd=%d closed by child", ipc_fd);
		sse_registry_remove_by_fd(reg, ipc_fd);
		close(ipc_fd);
		return;
	}

	/* Parse "REG token1 token2 token3 ..." */
	if (strncmp(buf, SSE_IPC_MSG_REGISTER " ", 4) == 0) {
		char *saveptr;
		char *tok = strtok_r(buf + 4, " \n", &saveptr);
		while (tok) {
			sse_registry_add(reg, tok, ipc_fd, pid);
			tok = strtok_r(NULL, " \n", &saveptr);
		}
	}
}

/*
 * IPC fd tracking: we need to map ipc_fds back to child pids for
 * poll() and registration handling.
 */
struct ipc_child {
	int   ipc_fd;
	pid_t pid;
};

#define MAX_IPC_CHILDREN MAX_CHILDREN

void tmate_ssh_server_main(struct tmate_session *session, const char *keys_dir,
			   const char *bind_addr, int port)
{
	struct tmate_ssh_client *client = &session->ssh_client;
	ssh_bind bind;
	pid_t pid;
	int fd;
	int sse_listen_fd = -1;
	struct sse_registry sse_reg;
	struct ipc_child ipc_children[MAX_IPC_CHILDREN];
	int num_ipc_children = 0;

	tmate_catch_sigsegv();

	server_start_time = time(NULL);

	sse_registry_init(&sse_reg);

	/* Use sigaction without SA_RESTART so poll() returns EINTR
	 * on SIGCHLD, giving us a chance to run GC. */
	{
		struct sigaction sa;
		memset(&sa, 0, sizeof(sa));
		sa.sa_handler = handle_sigchld;
		sigemptyset(&sa.sa_mask);
		sa.sa_flags = 0; /* no SA_RESTART */
		sigaction(SIGCHLD, &sa, NULL);
	}

	bind = prepare_ssh(keys_dir, bind_addr, port);

	/* Bind SSE listener in main process (before any forks) */
	if (tmate_has_websocket()) {
		sse_listen_fd = sse_bind_listener(tmate_settings->websocket_port);
		if (sse_listen_fd < 0)
			tmate_info("SSE multiplexer disabled — web viewing unavailable");
	}

	client->session = ssh_new();
	client->channel = NULL;
	client->winsize_pty.ws_col = 80;
	client->winsize_pty.ws_row = 24;
	session->session_token = "init";
	session->ipc_fd = -1;
	session->ev_ipc = NULL;

	if (!client->session)
		tmate_fatal("Cannot initialize session");

	for (;;) {
		/* Handle dead children: clean up IPC fds and registry */
		if (pending_gc) {
			pending_gc = 0;
			gc_stale_sessions();

			/* Process dead pids collected in signal handler.
			 * Block SIGCHLD to prevent race with the handler
			 * modifying dead_pids while we read it. */
			sigset_t block, prev;
			sigemptyset(&block);
			sigaddset(&block, SIGCHLD);
			sigprocmask(SIG_BLOCK, &block, &prev);
			int ndead = dead_pid_count;
			dead_pid_count = 0;
			sigprocmask(SIG_SETMASK, &prev, NULL);
			for (int d = 0; d < ndead; d++) {
				pid_t dpid = dead_pids[d];
				sse_registry_remove_by_pid(&sse_reg, dpid);
				for (int i = 0; i < num_ipc_children; i++) {
					if (ipc_children[i].pid == dpid) {
						close(ipc_children[i].ipc_fd);
						ipc_children[i] = ipc_children[num_ipc_children - 1];
						num_ipc_children--;
						break;
					}
				}
			}
		}

		/*
		 * Build poll set: SSH listener + SSE listener + IPC fds
		 * Index 0: SSH listener
		 * Index 1: SSE listener (if active)
		 * Index 2..N: IPC fds from daemon children
		 */
		int nfds = 0;
		struct pollfd pfds[2 + MAX_IPC_CHILDREN];

		pfds[nfds].fd = ssh_bind_get_fd(bind);
		pfds[nfds].events = POLLIN;
		nfds++;

		int sse_poll_idx = -1;
		if (sse_listen_fd >= 0) {
			sse_poll_idx = nfds;
			pfds[nfds].fd = sse_listen_fd;
			pfds[nfds].events = POLLIN;
			nfds++;
		}

		int ipc_poll_start = nfds;
		for (int i = 0; i < num_ipc_children; i++) {
			pfds[nfds].fd = ipc_children[i].ipc_fd;
			pfds[nfds].events = POLLIN;
			nfds++;
		}

		int ret = poll(pfds, nfds, 60000);
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			tmate_fatal("Error in poll: %s", strerror(errno));
		}
		if (ret == 0) {
			/* Periodic GC on timeout — catches stale entries
			 * even when no children die (e.g. unclean exit). */
			gc_stale_sessions();
			continue;
		}

		/* Check IPC fds for registration messages (process first
		 * so tokens are available before SSE connections arrive) */
		for (int i = 0; i < num_ipc_children; i++) {
			int pidx = ipc_poll_start + i;
			if (pfds[pidx].revents & (POLLIN | POLLHUP | POLLERR)) {
				if (pfds[pidx].revents & POLLIN) {
					handle_ipc_registrations(&sse_reg,
						ipc_children[i].ipc_fd,
						ipc_children[i].pid);
				} else {
					/* HUP/ERR: child gone */
					sse_registry_remove_by_fd(&sse_reg,
						ipc_children[i].ipc_fd);
					close(ipc_children[i].ipc_fd);
					ipc_children[i] = ipc_children[num_ipc_children - 1];
					num_ipc_children--;
					i--; /* re-check swapped entry */
				}
			}
		}

		/* Check SSE listener for incoming browser connections */
		if (sse_poll_idx >= 0 &&
		    (pfds[sse_poll_idx].revents & POLLIN)) {
			sse_handle_connection(sse_listen_fd, &sse_reg);
		}

		/* Check SSH listener for incoming SSH connections */
		if (!(pfds[0].revents & POLLIN))
			continue;

		fd = accept(ssh_bind_get_fd(bind), NULL, NULL);
		if (fd < 0) {
			if (errno == EINTR)
				continue;
			tmate_fatal("Error accepting connection");
		}

		/* Enforce max children limit */
		if (active_children >= MAX_CHILDREN) {
			tmate_info("Max connections reached (%d), rejecting",
				   MAX_CHILDREN);
			close(fd);
			continue;
		}

		/* Per-IP rate limiting (uses socket peer, not proxy header) */
		{
			char peer_ip[64];
			if (get_peer_ip(fd, peer_ip, sizeof(peer_ip)) == 0 &&
			    !check_rate_limit(peer_ip)) {
				tmate_info("Rate limit exceeded for %s", peer_ip);
				close(fd);
				continue;
			}
		}

		/*
		 * Create IPC socketpair for SSE fd passing.
		 * parent keeps ipc_pair[0], child keeps ipc_pair[1].
		 */
		int ipc_pair[2] = { -1, -1 };
		if (tmate_has_websocket() && sse_listen_fd >= 0) {
			if (socketpair(AF_UNIX, SOCK_STREAM, 0, ipc_pair) < 0) {
				tmate_info("Cannot create IPC socketpair: %s",
					   strerror(errno));
				/* Continue without SSE for this session */
			}
		}

		if ((pid = fork()) < 0) {
			tmate_info("Fork failed: %s", strerror(errno));
			close(fd);
			if (ipc_pair[0] >= 0) {
				close(ipc_pair[0]);
				close(ipc_pair[1]);
			}
			continue;
		}

		if (pid) {
			/* Parent process */
			active_children++;
			close(fd);

			if (ipc_pair[0] >= 0) {
				close(ipc_pair[1]); /* close child end */

				if (num_ipc_children < MAX_IPC_CHILDREN) {
					ipc_children[num_ipc_children].ipc_fd = ipc_pair[0];
					ipc_children[num_ipc_children].pid = pid;
					num_ipc_children++;
				} else {
					tmate_info("Too many IPC children, closing ipc_fd");
					close(ipc_pair[0]);
				}
			}
			continue;
		}

		/* Child process */

		/* Close parent resources */
		if (ipc_pair[0] >= 0)
			close(ipc_pair[0]); /* close parent end */
		if (sse_listen_fd >= 0)
			close(sse_listen_fd); /* child doesn't own the listener */

		/* Close all other IPC fds from parent's tracking */
		for (int i = 0; i < num_ipc_children; i++)
			close(ipc_children[i].ipc_fd);

		/* Store child's IPC fd in session for later use */
		session->ipc_fd = ipc_pair[1];

		signal(SIGALRM, handle_sigalrm);
		alarm(TMATE_SSH_GRACE_PERIOD);

		if (get_client_ip(fd, client->ip_address, sizeof(client->ip_address)) < 0) {
			if (tmate_settings->use_proxy_protocol)
				tmate_fatal("Proxy header invalid. Load balancer may be misconfigured");
			else
				tmate_fatal("Error getting client IP from connection");
		}

		tmate_debug("Connection accepted ip=%s", client->ip_address);

		if (ssh_bind_accept_fd(bind, client->session, fd) < 0)
			tmate_fatal("Error accepting connection: %s", ssh_get_error(bind));

		ssh_bind_free(bind);

		client_bootstrap(session);
		/* never reached */
	}
}
