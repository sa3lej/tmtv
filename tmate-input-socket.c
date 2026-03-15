/*
 * Copyright (c) 2026 Lars-Erik Jonsson <l@jonsson.es>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF MIND, USE, DATA OR PROFITS, WHETHER IN
 * AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT
 * OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 * Client-side Unix domain socket for per-user input events.
 *
 * When an application connects to the TMTV_INPUT_SOCKET and subscribes,
 * it receives per-user join/leave/input events from the server.  This
 * enables games, chat apps, and other multi-user terminal programs to
 * receive individual viewer input rather than the merged key stream.
 */

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <msgpack.h>
#include <event.h>

#include "tmate.h"
#include "tmate-protocol.h"

/* Message types for the local socket protocol (socket -> app) */
enum tmtv_input_msg_types {
	TMTV_INPUT_MSG_USER_JOIN,
	TMTV_INPUT_MSG_USER_LEAVE,
	TMTV_INPUT_MSG_USER_INPUT,
	TMTV_INPUT_MSG_USER_LIST,
};

/* Message types from app -> socket (app commands) */
enum tmtv_input_cmd_types {
	TMTV_INPUT_CMD_SUBSCRIBE,
	TMTV_INPUT_CMD_SET_MIRROR,
};

/* User tracking */
struct input_user {
	int			 id;
	char			*name;
	bool			 readonly;
	char			*type;	/* "ssh" or "web" */
	TAILQ_ENTRY(input_user)	 entry;
};

/*
 * Input socket receive buffer.
 * Messages are length-prefixed: [4-byte big-endian length][msgpack payload].
 * We accumulate bytes here until we have a complete message, then parse it.
 */
#define INPUT_APP_BUFSIZE 8192
#define INPUT_APP_MAX_MSG 65536

struct input_app_buf {
	char			 data[INPUT_APP_BUFSIZE];
	size_t			 len;
};

/* Input socket client -- an app connected to TMTV_INPUT_SOCKET */
struct input_app {
	int			 fd;
	struct event		*ev_read;
	struct input_app_buf	 buf;
	bool			 subscribed;
	bool			 mirror;
	TAILQ_ENTRY(input_app)	 entry;
};

static TAILQ_HEAD(, input_app) input_apps = TAILQ_HEAD_INITIALIZER(input_apps);
static TAILQ_HEAD(, input_user) input_users = TAILQ_HEAD_INITIALIZER(input_users);
static int input_listen_fd = -1;
static struct event *ev_input_accept;
static char input_socket_path[256];

/* Forward declarations */
static void input_app_free(struct input_app *app);
static int  input_app_dispatch(struct input_app *app, const char *data,
			       size_t len);

/*
 * Send a length-prefixed msgpack message to a connected app.
 * Framing: 4-byte big-endian length + msgpack payload.
 */
static void
input_send_to_app(struct input_app *app, msgpack_sbuffer *sbuf)
{
	uint32_t len = htonl(sbuf->size);

	if (write(app->fd, &len, 4) != 4 ||
	    write(app->fd, sbuf->data, sbuf->size) != (ssize_t)sbuf->size) {
		tmate_info("input socket: write failed, disconnecting app");
		input_app_free(app);
	}
}

/* Broadcast a message to all subscribed apps. */
static void
input_broadcast(msgpack_sbuffer *sbuf)
{
	struct input_app *app, *tmp;

	TAILQ_FOREACH_SAFE(app, &input_apps, entry, tmp) {
		if (app->subscribed)
			input_send_to_app(app, sbuf);
	}
}

/*
 * Count the number of currently subscribed apps.
 */
static int
input_count_subscribers(void)
{
	struct input_app *a;
	int count = 0;

	TAILQ_FOREACH(a, &input_apps, entry)
		if (a->subscribed)
			count++;
	return count;
}

/* --- Events from the server --- */

void
tmtv_input_on_user_join(int user_id, const char *name,
			bool readonly, const char *type)
{
	struct input_user *u;
	msgpack_sbuffer sbuf;
	msgpack_packer pk;

	/* Deduplicate: if user_id already exists, update in place.
	 * This prevents duplicates from repeated INPUT_MODE enables
	 * or SSE reconnects. */
	TAILQ_FOREACH(u, &input_users, entry) {
		if (u->id == user_id) {
			free(u->name);
			free(u->type);
			u->name = xstrdup(name);
			u->readonly = readonly;
			u->type = xstrdup(type);
			goto broadcast;
		}
	}

	/* New user — track it */
	u = xcalloc(1, sizeof(*u));
	u->id = user_id;
	u->name = xstrdup(name);
	u->readonly = readonly;
	u->type = xstrdup(type);
	TAILQ_INSERT_TAIL(&input_users, u, entry);

broadcast:

	/* Broadcast to subscribed apps */
	msgpack_sbuffer_init(&sbuf);
	msgpack_packer_init(&pk, &sbuf, msgpack_sbuffer_write);

	msgpack_pack_array(&pk, 5);
	msgpack_pack_int(&pk, TMTV_INPUT_MSG_USER_JOIN);
	msgpack_pack_int(&pk, user_id);
	msgpack_pack_string(&pk, name);
	msgpack_pack_boolean(&pk, readonly);
	msgpack_pack_string(&pk, type);

	input_broadcast(&sbuf);
	msgpack_sbuffer_destroy(&sbuf);
}

void
tmtv_input_on_user_leave(int user_id)
{
	struct input_user *u, *tmp;
	msgpack_sbuffer sbuf;
	msgpack_packer pk;

	/* Remove all entries with this user_id (defensive against
	 * duplicates that may exist from earlier bugs). */
	TAILQ_FOREACH_SAFE(u, &input_users, entry, tmp) {
		if (u->id == user_id) {
			TAILQ_REMOVE(&input_users, u, entry);
			free(u->name);
			free(u->type);
			free(u);
		}
	}

	/* Broadcast to subscribed apps */
	msgpack_sbuffer_init(&sbuf);
	msgpack_packer_init(&pk, &sbuf, msgpack_sbuffer_write);

	msgpack_pack_array(&pk, 2);
	msgpack_pack_int(&pk, TMTV_INPUT_MSG_USER_LEAVE);
	msgpack_pack_int(&pk, user_id);

	input_broadcast(&sbuf);
	msgpack_sbuffer_destroy(&sbuf);
}

void
tmtv_input_on_user_input(int user_id, int pane_id, key_code key)
{
	msgpack_sbuffer sbuf;
	msgpack_packer pk;

	msgpack_sbuffer_init(&sbuf);
	msgpack_packer_init(&pk, &sbuf, msgpack_sbuffer_write);

	msgpack_pack_array(&pk, 4);
	msgpack_pack_int(&pk, TMTV_INPUT_MSG_USER_INPUT);
	msgpack_pack_int(&pk, user_id);
	msgpack_pack_int(&pk, pane_id);
	msgpack_pack_uint64(&pk, (uint64_t)key);

	input_broadcast(&sbuf);
	msgpack_sbuffer_destroy(&sbuf);
}

/* --- Sending mode to server --- */

/*
 * Send TMATE_OUT_INPUT_MODE to the server.
 * This tells the server to start/stop sending USER_* events.
 */
void
tmtv_input_send_mode(bool enabled, bool mirror)
{
	struct tmate_session *session = &tmate_session;
	struct tmate_encoder *enc = &session->encoder;

	_pack(enc, array, 3);
	_pack(enc, int, TMATE_OUT_INPUT_MODE);
	_pack(enc, boolean, enabled);
	_pack(enc, boolean, mirror);
}

/* --- App connection handling --- */

/*
 * Send the current user list to a newly subscribed app.
 */
static void
send_user_list(struct input_app *app)
{
	struct input_user *u;
	int count = 0;
	msgpack_sbuffer sbuf;
	msgpack_packer pk;

	TAILQ_FOREACH(u, &input_users, entry)
		count++;

	msgpack_sbuffer_init(&sbuf);
	msgpack_packer_init(&pk, &sbuf, msgpack_sbuffer_write);

	msgpack_pack_array(&pk, 2);
	msgpack_pack_int(&pk, TMTV_INPUT_MSG_USER_LIST);
	msgpack_pack_array(&pk, count);

	TAILQ_FOREACH(u, &input_users, entry) {
		msgpack_pack_array(&pk, 4);
		msgpack_pack_int(&pk, u->id);
		msgpack_pack_string(&pk, u->name);
		msgpack_pack_boolean(&pk, u->readonly);
		msgpack_pack_string(&pk, u->type);
	}

	input_send_to_app(app, &sbuf);
	msgpack_sbuffer_destroy(&sbuf);
}

/*
 * Safe msgpack validation helpers for the input socket protocol.
 *
 * These return -1 on malformed input instead of calling fatalx().
 * The tmate_decoder / unpack_* functions were designed for the SSH
 * protocol where a bad message is catastrophic.  For the local input
 * socket, a bad message from a buggy app must disconnect that app,
 * not crash the entire tmtv client.
 */
static int
input_app_dispatch(struct input_app *app, const char *data, size_t len)
{
	msgpack_unpacked result;
	msgpack_object *arr;
	int cmd;

	msgpack_unpacked_init(&result);
	if (msgpack_unpack_next(&result, data, len, NULL)
	    != MSGPACK_UNPACK_SUCCESS) {
		tmate_info("input socket: malformed msgpack from app");
		msgpack_unpacked_destroy(&result);
		return -1;
	}

	if (result.data.type != MSGPACK_OBJECT_ARRAY ||
	    result.data.via.array.size < 1) {
		tmate_info("input socket: message is not an array");
		msgpack_unpacked_destroy(&result);
		return -1;
	}

	arr = result.data.via.array.ptr;

	if (arr[0].type != MSGPACK_OBJECT_POSITIVE_INTEGER &&
	    arr[0].type != MSGPACK_OBJECT_NEGATIVE_INTEGER) {
		tmate_info("input socket: command type is not an integer");
		msgpack_unpacked_destroy(&result);
		return -1;
	}

	cmd = (int)arr[0].via.i64;

	switch (cmd) {
	case TMTV_INPUT_CMD_SUBSCRIBE:
		app->subscribed = true;
		send_user_list(app);

		/* First subscriber: enable input mode on the server */
		if (input_count_subscribers() == 1)
			tmtv_input_send_mode(true, app->mirror);
		break;

	case TMTV_INPUT_CMD_SET_MIRROR:
		if (result.data.via.array.size < 2) {
			tmate_info("input socket: SET_MIRROR missing argument");
			msgpack_unpacked_destroy(&result);
			return -1;
		}
		if (arr[1].type != MSGPACK_OBJECT_BOOLEAN) {
			tmate_info("input socket: SET_MIRROR arg not boolean");
			msgpack_unpacked_destroy(&result);
			return -1;
		}
		app->mirror = arr[1].via.boolean;
		tmtv_input_send_mode(true, app->mirror);
		break;

	default:
		tmate_info("input socket: unknown command %d", cmd);
		break;
	}

	msgpack_unpacked_destroy(&result);
	return 0;
}

/* --- Accept loop and app lifecycle --- */

/*
 * Process complete length-prefixed messages from the app's receive buffer.
 * Returns -1 if the app should be disconnected (malformed data).
 */
static int
input_app_process_buf(struct input_app *app)
{
	uint32_t msg_len;

	while (app->buf.len >= 4) {
		memcpy(&msg_len, app->buf.data, 4);
		msg_len = ntohl(msg_len);

		if (msg_len > INPUT_APP_MAX_MSG) {
			tmate_info("input socket: message too large (%u bytes)",
				   msg_len);
			return -1;
		}

		if (app->buf.len < 4 + msg_len)
			break; /* need more data */

		if (input_app_dispatch(app, app->buf.data + 4, msg_len) < 0)
			return -1;

		/* Consume the message */
		size_t consumed = 4 + msg_len;
		app->buf.len -= consumed;
		if (app->buf.len > 0)
			memmove(app->buf.data, app->buf.data + consumed,
				app->buf.len);
	}

	return 0;
}

static void
on_app_read(__unused evutil_socket_t fd, __unused short what, void *arg)
{
	struct input_app *app = arg;
	ssize_t n;
	size_t space;

	space = sizeof(app->buf.data) - app->buf.len;
	if (space == 0) {
		tmate_info("input socket: app buffer full, disconnecting");
		input_app_free(app);
		return;
	}

	n = read(app->fd, app->buf.data + app->buf.len, space);
	if (n <= 0) {
		input_app_free(app);
		return;
	}

	app->buf.len += n;

	if (input_app_process_buf(app) < 0)
		input_app_free(app);
}

static void
input_app_free(struct input_app *app)
{
	bool was_subscribed = app->subscribed;

	TAILQ_REMOVE(&input_apps, app, entry);

	if (app->ev_read) {
		event_del(app->ev_read);
		event_free(app->ev_read);
	}
	close(app->fd);
	free(app);

	/* If no subscribers remain, disable input mode on server.
	 * Check after free so input_count_subscribers() doesn't count
	 * the app we just removed. */
	if (was_subscribed && input_count_subscribers() == 0)
		tmtv_input_send_mode(false, true);

	tmate_info("input socket: app disconnected");
}

static void
on_input_accept(__unused evutil_socket_t fd, __unused short what,
		__unused void *arg)
{
	struct input_app *app;
	int client_fd;
	int flags;

	client_fd = accept(input_listen_fd, NULL, NULL);
	if (client_fd < 0) {
		if (errno != EAGAIN && errno != EWOULDBLOCK)
			tmate_info("input socket: accept failed: %s",
				   strerror(errno));
		return;
	}

	flags = fcntl(client_fd, F_GETFL);
	fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);

	app = xcalloc(1, sizeof(*app));
	app->fd = client_fd;
	app->mirror = true;
	app->buf.len = 0;

	app->ev_read = event_new(tmate_session.ev_base, client_fd,
				 EV_READ | EV_PERSIST, on_app_read, app);
	event_add(app->ev_read, NULL);

	TAILQ_INSERT_TAIL(&input_apps, app, entry);
	tmate_info("input socket: app connected");
}

/* --- Public API --- */

/*
 * Create the Unix socket before the session starts.
 * Path: /tmp/tmtv-input-<pid>.sock
 * Sets TMTV_INPUT_SOCKET env var so child processes can find it.
 */
void
tmtv_input_socket_create(void)
{
	struct sockaddr_un sa;
	int fd, flags;

	snprintf(input_socket_path, sizeof(input_socket_path),
		 "/tmp/tmtv-input-%d.sock", getpid());

	/* Clean up stale socket */
	unlink(input_socket_path);

	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		tmate_info("input socket: socket() failed: %s",
			   strerror(errno));
		return;
	}

	memset(&sa, 0, sizeof(sa));
	sa.sun_family = AF_UNIX;
	strlcpy(sa.sun_path, input_socket_path, sizeof(sa.sun_path));

	if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		tmate_info("input socket: bind() failed: %s",
			   strerror(errno));
		close(fd);
		return;
	}

	/* Only owner can connect */
	chmod(input_socket_path, 0700);

	if (listen(fd, 5) < 0) {
		tmate_info("input socket: listen() failed: %s",
			   strerror(errno));
		close(fd);
		unlink(input_socket_path);
		return;
	}

	flags = fcntl(fd, F_GETFL);
	fcntl(fd, F_SETFL, flags | O_NONBLOCK);

	input_listen_fd = fd;
	setenv("TMTV_INPUT_SOCKET", input_socket_path, 1);
	/* Also propagate to tmux's global_environ so spawned shells
	 * inherit it.  setenv() only updates the C-level environ,
	 * but global_environ was already snapshot before this runs. */
	if (global_environ != NULL)
		environ_set(global_environ, "TMTV_INPUT_SOCKET", 0,
			    "%s", input_socket_path);
	tmate_info("input socket: listening on %s", input_socket_path);
}

/*
 * Start accepting connections. Called after the event base is initialized.
 */
void
tmtv_input_socket_start(struct event_base *base)
{
	if (input_listen_fd < 0)
		return;

	ev_input_accept = event_new(base, input_listen_fd,
				    EV_READ | EV_PERSIST,
				    on_input_accept, NULL);
	event_add(ev_input_accept, NULL);
}

/*
 * Cleanup on exit.
 */
void
tmtv_input_socket_cleanup(void)
{
	struct input_app *app, *tmp;

	TAILQ_FOREACH_SAFE(app, &input_apps, entry, tmp)
		input_app_free(app);

	if (ev_input_accept) {
		event_del(ev_input_accept);
		event_free(ev_input_accept);
		ev_input_accept = NULL;
	}

	if (input_listen_fd >= 0) {
		close(input_listen_fd);
		input_listen_fd = -1;
	}

	if (input_socket_path[0] != '\0')
		unlink(input_socket_path);
}

bool
tmtv_input_has_subscribers(void)
{
	return (input_count_subscribers() > 0);
}
