#include <sys/socket.h>
#include <netinet/tcp.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#ifndef IPPROTO_TCP
#include <netinet/in.h>
#endif

#include <openssl/evp.h>
#include <event2/listener.h>

#include "tmate.h"
#include "tmate-protocol.h"

/*
 * SSE (Server-Sent Events) listener for browser-based terminal viewing.
 *
 * Browsers connect via plain HTTP, receive base64-encoded msgpack data
 * as SSE events (same TMATE_CTL_* protocol payload).
 *
 * SSE is ideal for view-only streaming: it's one-way (server to browser),
 * works through any HTTP proxy/CDN, and browsers have built-in
 * auto-reconnect via the EventSource API.
 */

#define CONTROL_PROTOCOL_VERSION 2

#define SSE_MAX_HEADER_SIZE 4096

#define pack(what, ...) _pack(&tmate_session->websocket_encoder, what, ##__VA_ARGS__)

#define pack_string_or_nil(str) ({ \
	if (str) \
		pack(string, str); \
	else \
		pack(nil); \
})

/* --- SSE data sending --- */

static void sse_send_data(struct bufferevent *bev,
			  const unsigned char *data, size_t len)
{
	struct evbuffer *out = bufferevent_get_output(bev);
	int b64_len = 4 * ((len + 2) / 3);
	unsigned char *b64 = xmalloc(b64_len + 1);

	EVP_EncodeBlock(b64, data, len);

	evbuffer_add(out, "data: ", 6);
	evbuffer_add(out, b64, b64_len);
	evbuffer_add(out, "\n\n", 2);

	free(b64);
}

/* --- HTTP request parsing (minimal: just wait for headers) --- */

/* Find needle in haystack, searching at most slen bytes */
static char *find_in_mem(const char *s, const char *find, size_t slen)
{
	size_t flen = strlen(find);
	if (flen == 0)
		return (char *)s;
	for (; slen >= flen; s++, slen--) {
		if (memcmp(s, find, flen) == 0)
			return (char *)s;
	}
	return NULL;
}

static int sse_do_handshake(struct ws_client *wc)
{
	struct evbuffer *input = bufferevent_get_input(wc->bev);
	size_t len = evbuffer_get_length(input);
	char *data;
	char *header_end;

	if (len > SSE_MAX_HEADER_SIZE)
		return -1;

	data = (char *)evbuffer_pullup(input, len);
	if (!data)
		return 0;

	header_end = find_in_mem(data, "\r\n\r\n", len);
	if (!header_end)
		return 0; /* need more data */

	static const char *response =
		"HTTP/1.1 200 OK\r\n"
		"Content-Type: text/event-stream\r\n"
		"Cache-Control: no-cache\r\n"
		"Connection: keep-alive\r\n"
		"Access-Control-Allow-Origin: *\r\n"
		"X-Accel-Buffering: no\r\n"
		"\r\n";

	evbuffer_drain(input, (header_end - data) + 4);
	bufferevent_write(wc->bev, response, strlen(response));

	return 1;
}

/* --- Snapshot sending --- */

static void do_snapshot(unsigned int max_history_lines,
			struct window_pane *pane)
{
	struct screen *screen;
	struct grid *grid;
	struct grid_line *line;
	struct grid_cell gc;
	unsigned int line_i, i;
	unsigned int max_lines;
	size_t str_len;

	screen = &pane->base;
	grid = screen->grid;

	pack(array, 4);
	pack(int, pane->id);

	pack(array, 2);
	pack(int, screen->cx);
	pack(int, screen->cy);

	pack(unsigned_int, screen->mode);

	max_lines = max_history_lines + grid->sy;

#define grid_num_lines(grid) (grid->hsize + grid->sy)

	if (grid_num_lines(grid) > max_lines)
		line_i = grid_num_lines(grid) - max_lines;
	else
		line_i = 0;

	pack(array, grid_num_lines(grid) - line_i);
	for (; line_i < grid_num_lines(grid); line_i++) {
		line = &grid->linedata[line_i];

		pack(array, 2);
		str_len = 0;
		for (i = 0; i < line->cellsize; i++) {
			grid_get_cell(grid, i, line_i, &gc);
			str_len += gc.data.size;
		}

		pack(str, str_len);
		for (i = 0; i < line->cellsize; i++) {
			grid_get_cell(grid, i, line_i, &gc);
			pack(str_body, gc.data.data, gc.data.size);
		}

		pack(array, line->cellsize);
		for (i = 0; i < line->cellsize; i++) {
			grid_get_cell(grid, i, line_i, &gc);
			pack(unsigned_int, ((gc.flags << 24) |
					    (gc.attr  << 16) |
					    (gc.bg    << 8)  |
					     gc.fg        ));
		}
	}
}

static void tmate_send_snapshot(void)
{
	struct session *s;
	struct winlink *wl;
	struct window *w;
	struct window_pane *pane;
	int num_panes;

	pack(array, 2);
	pack(int, TMATE_CTL_SNAPSHOT);

	s = RB_MIN(sessions, &sessions);
	if (!s) {
		pack(array, 0);
		return;
	}

	num_panes = 0;
	RB_FOREACH(wl, winlinks, &s->windows) {
		w = wl->window;
		if (!w)
			continue;
		TAILQ_FOREACH(pane, &w->panes, entry)
			num_panes++;
	}

	pack(array, num_panes);
	RB_FOREACH(wl, winlinks, &s->windows) {
		w = wl->window;
		if (!w)
			continue;
		TAILQ_FOREACH(pane, &w->panes, entry)
			do_snapshot(TMATE_HLIMIT, pane);
	}
}

/* --- Client management --- */

static void ws_client_free(struct ws_client *wc)
{
	tmate_info("SSE client disconnected");
	TAILQ_REMOVE(&wc->session->ws_clients, wc, entry);
	if (wc->bev)
		bufferevent_free(wc->bev);
	free(wc);
}

static void on_ws_client_read(__unused struct bufferevent *bev, void *arg)
{
	struct ws_client *wc = arg;

	if (!wc->handshake_done) {
		int ret = sse_do_handshake(wc);
		if (ret < 0) {
			tmate_info("SSE handshake failed");
			ws_client_free(wc);
			return;
		}
		if (ret == 0)
			return; /* need more data */

		wc->handshake_done = true;
		tmate_info("SSE client connected");

		/* Send snapshot to new client */
		tmate_send_snapshot();
		return;
	}

	/* SSE is server-push only; drain any client data */
	struct evbuffer *input = bufferevent_get_input(wc->bev);
	evbuffer_drain(input, evbuffer_get_length(input));
}

static void on_ws_client_event(__unused struct bufferevent *bev,
			       short events, void *arg)
{
	struct ws_client *wc = arg;

	if (events & (BEV_EVENT_EOF | BEV_EVENT_ERROR))
		ws_client_free(wc);
}

static void on_ws_accept(__unused struct evconnlistener *listener,
			 evutil_socket_t fd,
			 __unused struct sockaddr *addr,
			 __unused int socklen, void *arg)
{
	struct tmate_session *session = arg;
	struct ws_client *wc;
	int flag = 1;

	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

	wc = xcalloc(1, sizeof(*wc));
	wc->session = session;
	wc->handshake_done = false;
	wc->bev = bufferevent_socket_new(session->ev_base, fd,
					 BEV_OPT_CLOSE_ON_FREE);
	if (!wc->bev) {
		free(wc);
		close(fd);
		return;
	}

	bufferevent_setcb(wc->bev, on_ws_client_read, NULL,
			  on_ws_client_event, wc);
	bufferevent_enable(wc->bev, EV_READ | EV_WRITE);

	TAILQ_INSERT_TAIL(&session->ws_clients, wc, entry);
	tmate_debug("SSE connection accepted, awaiting HTTP request");
}

/* --- Encoder broadcast callback --- */

static void on_websocket_encoder_write(void *userdata, struct evbuffer *buffer)
{
	struct tmate_session *session = userdata;
	struct ws_client *wc;
	size_t len;
	unsigned char *data;

	len = evbuffer_get_length(buffer);
	if (len == 0)
		return;

	data = evbuffer_pullup(buffer, len);

	TAILQ_FOREACH(wc, &session->ws_clients, entry) {
		if (wc->handshake_done)
			sse_send_data(wc->bev, data, len);
	}

	evbuffer_drain(buffer, len);
}

/* --- Protocol handlers (kept for future interactive mode) --- */

static void ctl_daemon_fwd_msg(__unused struct tmate_session *session,
			       struct tmate_unpacker *uk)
{
	if (uk->argc != 1)
		tmate_decoder_error();
	tmate_send_mc_obj(&uk->argv[0]);
}

static void ctl_pane_keys(__unused struct tmate_session *session,
			  struct tmate_unpacker *uk)
{
	int pane_id;
	char *str;

	pane_id = unpack_int(uk);
	str = unpack_string(uk);

	/* View-only mode: ignore keyboard input from browsers */
	tmate_debug("Ignoring pane keys from websocket (view-only mode)");

	free(str);
	(void)pane_id;
}

static void ctl_resize(struct tmate_session *session,
		       struct tmate_unpacker *uk)
{
	session->websocket_sx = (u_int)unpack_int(uk);
	session->websocket_sy = (u_int)unpack_int(uk);
	recalculate_sizes();
}

static void ctl_daemon_request_snapshot(__unused struct tmate_session *session,
					struct tmate_unpacker *uk)
{
	/* Browser explicitly requested a snapshot */
	(void)uk; /* max_history_lines ignored, we use TMATE_HLIMIT */
	tmate_send_snapshot();
}

static void tmate_dispatch_websocket_message(struct tmate_session *session,
					     struct tmate_unpacker *uk)
{
	int cmd = unpack_int(uk);
	switch (cmd) {
#define dispatch(c, f) case c: f(session, uk); break
	dispatch(TMATE_CTL_DEAMON_FWD_MSG,	ctl_daemon_fwd_msg);
	dispatch(TMATE_CTL_REQUEST_SNAPSHOT,	ctl_daemon_request_snapshot);
	dispatch(TMATE_CTL_PANE_KEYS,		ctl_pane_keys);
	dispatch(TMATE_CTL_RESIZE,		ctl_resize);
	default: tmate_info("Bad websocket message type: %d", cmd);
	}
}

/* --- Public API --- */

void tmate_websocket_exec(__unused struct tmate_session *session,
			  __unused const char *command)
{
	/* Exec via websocket is no longer supported (no external server) */
}

void tmate_notify_client_join(__unused struct tmate_session *session,
			      struct client *c)
{
	tmate_info("Client joined (pid=%d)", c->pid);

	if (!tmate_has_websocket())
		return;

	c->flags |= CLIENT_TMATE_NOTIFIED_JOIN;

	pack(array, 5);
	pack(int, TMATE_CTL_CLIENT_JOIN);
	pack(int, c->pid);
	pack(string, c->ip_address);
	pack_string_or_nil(c->pubkey);
	pack(boolean, c->readonly);
}

void tmate_notify_client_left(__unused struct tmate_session *session,
			      struct client *c)
{
	if (!(c->flags & CLIENT_IDENTIFIED))
		return;

	tmate_info("Client left (pid=%d)", c->pid);

	if (!tmate_has_websocket())
		return;

	if (!(c->flags & CLIENT_TMATE_NOTIFIED_JOIN))
		return;

	c->flags &= ~CLIENT_TMATE_NOTIFIED_JOIN;

	pack(array, 2);
	pack(int, TMATE_CTL_CLIENT_LEFT);
	pack(int, c->pid);
}

void tmate_send_websocket_daemon_msg(__unused struct tmate_session *session,
				     struct tmate_unpacker *uk)
{
	int i;

	if (!tmate_has_websocket())
		return;

	pack(array, 2);
	pack(int, TMATE_CTL_DEAMON_OUT_MSG);

	pack(array, uk->argc);
	for (i = 0; i < uk->argc; i++)
		pack(object, uk->argv[i]);
}

void tmate_send_websocket_header(struct tmate_session *session)
{
	if (!tmate_has_websocket())
		return;

	pack(array, 9);
	pack(int, TMATE_CTL_HEADER);
	pack(int, CONTROL_PROTOCOL_VERSION);
	pack(string, session->ssh_client.ip_address);
	pack_string_or_nil(session->ssh_client.pubkey);
	pack(string, session->session_token);
	pack(string, session->session_token_ro);

	char *ssh_cmd_fmt = get_ssh_conn_string("%s");
	pack(string, ssh_cmd_fmt);
	free(ssh_cmd_fmt);

	pack(string, session->client_version);
	pack(int, session->client_protocol_version);
}

void tmate_init_websocket(struct tmate_session *session)
{
	if (!tmate_has_websocket())
		return;

	session->websocket_sx = -1;
	session->websocket_sy = -1;
	session->ws_listen_fd = -1;

	TAILQ_INIT(&session->ws_clients);

	tmate_encoder_init(&session->websocket_encoder,
			   on_websocket_encoder_write, session);
}

void tmate_bind_websocket_socket(struct tmate_session *session)
{
	struct sockaddr_in sin;
	int port, fd, flag = 1;

	if (!tmate_has_websocket())
		return;

	port = tmate_settings->websocket_port;

	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		tmate_info("Cannot create SSE socket: %s", strerror(errno));
		return;
	}

	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));
	evutil_make_socket_nonblocking(fd);

	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = htonl(INADDR_ANY);
	sin.sin_port = htons(port);

	if (bind(fd, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
		tmate_info("Cannot bind SSE socket on port %d: %s",
			   port, strerror(errno));
		close(fd);
		return;
	}

	if (listen(fd, 128) < 0) {
		tmate_info("Cannot listen on SSE socket: %s", strerror(errno));
		close(fd);
		return;
	}

	session->ws_listen_fd = fd;
	tmate_info("SSE socket bound on port %d (fd=%d)", port, fd);
}

void tmate_start_websocket_listener(struct tmate_session *session)
{
	if (!tmate_has_websocket())
		return;

	if (session->ws_listen_fd < 0) {
		tmate_info("No SSE socket to register");
		return;
	}

	session->ws_listener = evconnlistener_new(
		session->ev_base,
		on_ws_accept, session,
		LEV_OPT_CLOSE_ON_FREE,
		0, /* backlog: already listening */
		session->ws_listen_fd);

	if (!session->ws_listener) {
		tmate_info("Cannot create SSE listener: %s", strerror(errno));
		return;
	}

	tmate_info("SSE listener registered on fd=%d", session->ws_listen_fd);
}
