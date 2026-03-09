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
 */

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

/* Forward declaration */
static void sse_send_data(struct bufferevent *bev,
			  const unsigned char *data, size_t len);

/* --- PTY replay buffer ---
 * Message-level ring buffer: each entry is a complete encoder write
 * (one or more complete msgpack messages).  New SSE clients receive
 * these on connect so they see the current screen state.
 *
 * Using message-level granularity avoids splitting msgpack framing
 * when the buffer wraps — every replayed SSE event is well-formed.
 */
#define PTY_REPLAY_MAX_MSGS  4096
#define PTY_REPLAY_MAX_BYTES (256 * 1024)

static struct {
	unsigned char *data;
	size_t         len;
} pty_replay_msgs[PTY_REPLAY_MAX_MSGS];
static int    pty_replay_head;        /* next write slot */
static int    pty_replay_count;       /* messages stored */
static size_t pty_replay_total_bytes;

static void pty_replay_init(void)
{
	memset(pty_replay_msgs, 0, sizeof(pty_replay_msgs));
	pty_replay_head = 0;
	pty_replay_count = 0;
	pty_replay_total_bytes = 0;
}

static void pty_replay_append(const unsigned char *data, size_t len)
{
	/* Evict oldest messages until we have room */
	while (pty_replay_count > 0 &&
	       pty_replay_total_bytes + len > PTY_REPLAY_MAX_BYTES) {
		int tail = (pty_replay_head - pty_replay_count +
			    PTY_REPLAY_MAX_MSGS) % PTY_REPLAY_MAX_MSGS;
		pty_replay_total_bytes -= pty_replay_msgs[tail].len;
		free(pty_replay_msgs[tail].data);
		pty_replay_msgs[tail].data = NULL;
		pty_replay_msgs[tail].len = 0;
		pty_replay_count--;
	}

	/* Overwrite slot if ring is full */
	if (pty_replay_msgs[pty_replay_head].data) {
		pty_replay_total_bytes -= pty_replay_msgs[pty_replay_head].len;
		free(pty_replay_msgs[pty_replay_head].data);
	}

	pty_replay_msgs[pty_replay_head].data = xmalloc(len);
	memcpy(pty_replay_msgs[pty_replay_head].data, data, len);
	pty_replay_msgs[pty_replay_head].len = len;
	pty_replay_total_bytes += len;

	pty_replay_head = (pty_replay_head + 1) % PTY_REPLAY_MAX_MSGS;
	if (pty_replay_count < PTY_REPLAY_MAX_MSGS)
		pty_replay_count++;
}

static void pty_replay_send(struct bufferevent *bev)
{
	int i, idx, tail;

	if (pty_replay_count == 0)
		return;

	tail = (pty_replay_head - pty_replay_count +
		PTY_REPLAY_MAX_MSGS) % PTY_REPLAY_MAX_MSGS;
	for (i = 0; i < pty_replay_count; i++) {
		idx = (tail + i) % PTY_REPLAY_MAX_MSGS;
		if (pty_replay_msgs[idx].data)
			sse_send_data(bev, pty_replay_msgs[idx].data,
				      pty_replay_msgs[idx].len);
	}
}

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

/* --- Grid-based screen dump for new client catch-up ---
 *
 * Instead of replaying raw PTY data (which loses bytes at SSH read
 * boundaries), read the server's tmux grid and emit VT100 escape
 * sequences that xterm.js can render directly.  This is wrapped in
 * a msgpack PTY_DATA message and sent as a single SSE event.
 */

/*
 * Send a SYNC_LAYOUT message to an SSE client so the browser knows
 * the terminal dimensions and window names.
 * Format: [CTL_DEAMON_OUT_MSG, [OUT_SYNC_LAYOUT, sx, sy, [[idx, name, [], -1], ...], active_idx]]
 */
static void sse_send_sync_layout(struct bufferevent *bev)
{
	struct session *s;
	struct winlink *wl;
	struct window *w;
	int num_windows = 0;
	int active_idx = -1;
	msgpack_sbuffer sbuf;
	msgpack_packer pk;

	s = RB_MIN(sessions, &sessions);
	if (!s)
		return;

	RB_FOREACH(wl, winlinks, &s->windows) {
		if (wl->window)
			num_windows++;
	}
	if (!num_windows)
		return;

	if (s->curw)
		active_idx = s->curw->idx;

	/* Get size from active window */
	int sx = 80, sy = 24;
	if (s->curw && s->curw->window) {
		sx = s->curw->window->sx;
		sy = s->curw->window->sy;
	}

	msgpack_sbuffer_init(&sbuf);
	msgpack_packer_init(&pk, &sbuf, msgpack_sbuffer_write);

	/* Outer: [CTL_DEAMON_OUT_MSG, inner] */
	msgpack_pack_array(&pk, 2);
	msgpack_pack_int(&pk, 1);  /* CTL_DEAMON_OUT_MSG */

	/* Inner: [OUT_SYNC_LAYOUT, sx, sy, windows, active_idx] */
	msgpack_pack_array(&pk, 5);
	msgpack_pack_int(&pk, 1);  /* OUT_SYNC_LAYOUT */
	msgpack_pack_int(&pk, sx);
	msgpack_pack_int(&pk, sy);

	/* Windows array */
	msgpack_pack_array(&pk, num_windows);
	RB_FOREACH(wl, winlinks, &s->windows) {
		struct window_pane *wp;
		int num_panes = 0;
		int active_pane_id = -1;

		w = wl->window;
		if (!w)
			continue;

		TAILQ_FOREACH(wp, &w->panes, entry)
			num_panes++;

		/* [idx, name, [[pane_id, sx, sy, xoff, yoff], ...], active_pane_id] */
		msgpack_pack_array(&pk, 4);
		msgpack_pack_int(&pk, wl->idx);
		msgpack_pack_str(&pk, strlen(w->name));
		msgpack_pack_str_body(&pk, w->name, strlen(w->name));

		msgpack_pack_array(&pk, num_panes);
		TAILQ_FOREACH(wp, &w->panes, entry) {
			msgpack_pack_array(&pk, 5);
			msgpack_pack_int(&pk, wp->id);
			msgpack_pack_int(&pk, wp->sx);
			msgpack_pack_int(&pk, wp->sy);
			msgpack_pack_int(&pk, wp->xoff);
			msgpack_pack_int(&pk, wp->yoff);
			if (wp == w->active)
				active_pane_id = wp->id;
		}
		msgpack_pack_int(&pk, active_pane_id);
	}

	msgpack_pack_int(&pk, active_idx);

	sse_send_data(bev, (unsigned char *)sbuf.data, sbuf.size);
	msgpack_sbuffer_destroy(&sbuf);
}

static void sse_send_pane_dump(struct bufferevent *bev,
			       struct window_pane *pane)
{
	struct screen *screen;
	struct grid *grid;
	struct grid_cell gc;
	unsigned int y, x, sx, sy;
	struct evbuffer *vt;
	unsigned char *vt_data;
	size_t vt_len;
	int prev_fg, prev_bg, prev_attr;

	screen = pane->screen;
	grid = screen->grid;
	sx = grid->sx;
	sy = grid->sy;

	vt = evbuffer_new();
	if (!vt)
		return;

	/* Reset terminal and home cursor */
	evbuffer_add(vt, "\033[!p", 4);     /* DECSTR soft reset */
	evbuffer_add(vt, "\033[H", 3);      /* cursor home */
	evbuffer_add(vt, "\033[?25l", 6);   /* hide cursor */

	prev_fg = -1;
	prev_bg = -1;
	prev_attr = -1;

	for (y = 0; y < sy; y++) {
		unsigned int grid_y = grid->hsize + y;

		for (x = 0; x < sx; x++) {
			grid_get_cell(grid, x, grid_y, &gc);

			int fg = gc.fg;
			int bg = gc.bg;
			int attr = gc.attr;

			if (fg != prev_fg || bg != prev_bg || attr != prev_attr) {
				char sgr[96];
				int n = 0;
				n += snprintf(sgr + n, sizeof(sgr) - n, "\033[0");

				if (attr & GRID_ATTR_BRIGHT)
					n += snprintf(sgr + n, sizeof(sgr) - n, ";1");
				if (attr & GRID_ATTR_DIM)
					n += snprintf(sgr + n, sizeof(sgr) - n, ";2");
				if (attr & GRID_ATTR_ITALICS)
					n += snprintf(sgr + n, sizeof(sgr) - n, ";3");
				if (attr & GRID_ATTR_UNDERSCORE)
					n += snprintf(sgr + n, sizeof(sgr) - n, ";4");
				if (attr & GRID_ATTR_BLINK)
					n += snprintf(sgr + n, sizeof(sgr) - n, ";5");
				if (attr & GRID_ATTR_REVERSE)
					n += snprintf(sgr + n, sizeof(sgr) - n, ";7");
				if (attr & GRID_ATTR_HIDDEN)
					n += snprintf(sgr + n, sizeof(sgr) - n, ";8");
				if (attr & GRID_ATTR_STRIKETHROUGH)
					n += snprintf(sgr + n, sizeof(sgr) - n, ";9");
				if (attr & GRID_ATTR_OVERLINE)
					n += snprintf(sgr + n, sizeof(sgr) - n, ";53");

				if (fg & COLOUR_FLAG_RGB) {
					n += snprintf(sgr + n, sizeof(sgr) - n,
						";38;2;%d;%d;%d",
						(fg >> 16) & 0xff,
						(fg >> 8) & 0xff,
						fg & 0xff);
				} else if (fg & COLOUR_FLAG_256) {
					n += snprintf(sgr + n, sizeof(sgr) - n,
						";38;5;%d", fg & 0xff);
				} else if (fg >= 90 && fg <= 97) {
					n += snprintf(sgr + n, sizeof(sgr) - n,
						";%d", fg);
				} else if (fg >= 0 && fg <= 7) {
					n += snprintf(sgr + n, sizeof(sgr) - n,
						";%d", 30 + fg);
				}
				/* 8=default, 9=terminal: reset handles it */

				if (bg & COLOUR_FLAG_RGB) {
					n += snprintf(sgr + n, sizeof(sgr) - n,
						";48;2;%d;%d;%d",
						(bg >> 16) & 0xff,
						(bg >> 8) & 0xff,
						bg & 0xff);
				} else if (bg & COLOUR_FLAG_256) {
					n += snprintf(sgr + n, sizeof(sgr) - n,
						";48;5;%d", bg & 0xff);
				} else if (bg >= 90 && bg <= 97) {
					n += snprintf(sgr + n, sizeof(sgr) - n,
						";%d", bg + 10);
				} else if (bg >= 0 && bg <= 7) {
					n += snprintf(sgr + n, sizeof(sgr) - n,
						";%d", 40 + bg);
				}
				/* 8=default, 9=terminal: reset handles it */

				n += snprintf(sgr + n, sizeof(sgr) - n, "m");
				evbuffer_add(vt, sgr, n);

				prev_fg = fg;
				prev_bg = bg;
				prev_attr = attr;
			}

			if (gc.data.size == 1 &&
			    ((unsigned char)gc.data.data[0] < 0x20 ||
			     (unsigned char)gc.data.data[0] == 0x7f)) {
				evbuffer_add(vt, " ", 1);
			} else if (gc.data.size > 0) {
				evbuffer_add(vt, gc.data.data, gc.data.size);
			} else {
				evbuffer_add(vt, " ", 1);
			}
		}

		if (y < sy - 1)
			evbuffer_add(vt, "\r\n", 2);
	}

	/* Reset attributes, position cursor, show it */
	{
		char cpos[32];
		int clen;
		evbuffer_add(vt, "\033[0m", 4);
		clen = snprintf(cpos, sizeof(cpos), "\033[%u;%uH",
				screen->cy + 1, screen->cx + 1);
		evbuffer_add(vt, cpos, clen);
		evbuffer_add(vt, "\033[?25h", 6);
	}

	vt_len = evbuffer_get_length(vt);
	vt_data = evbuffer_pullup(vt, vt_len);

	/* Wrap in msgpack: [CTL_DEAMON_OUT_MSG, [OUT_PTY_DATA, pane_id, <data>]] */
	{
		msgpack_sbuffer sbuf;
		msgpack_packer pk;

		msgpack_sbuffer_init(&sbuf);
		msgpack_packer_init(&pk, &sbuf, msgpack_sbuffer_write);

		msgpack_pack_array(&pk, 2);
		msgpack_pack_int(&pk, 1);  /* CTL_DEAMON_OUT_MSG */
		msgpack_pack_array(&pk, 3);
		msgpack_pack_int(&pk, 2);  /* OUT_PTY_DATA */
		msgpack_pack_int(&pk, pane->id);
		msgpack_pack_bin(&pk, vt_len);
		msgpack_pack_bin_body(&pk, vt_data, vt_len);

		sse_send_data(bev, (unsigned char *)sbuf.data, sbuf.size);

		msgpack_sbuffer_destroy(&sbuf);
	}

	evbuffer_free(vt);

	tmate_info("pane_dump: pane=%d %zu vt100 bytes, grid %ux%u",
		   pane->id, vt_len, sx, sy);
}

static void sse_send_screen_dump(struct bufferevent *bev)
{
	struct session *s;
	struct winlink *wl;
	struct window *w;
	struct window_pane *wp;

	s = RB_MIN(sessions, &sessions);
	if (!s || !s->curw)
		return;

	wl = s->curw;
	w = wl->window;
	if (!w)
		return;

	/* Dump all panes in the active window */
	TAILQ_FOREACH(wp, &w->panes, entry) {
		if (wp->screen)
			sse_send_pane_dump(bev, wp);
	}
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

static int sse_validate_token(struct ws_client *wc, const char *path,
			      size_t path_len)
{
	struct tmate_session *s = wc->session;
	const char *token;
	size_t tlen;

	/* Strip leading slash: "/TOKEN" or "/ws/TOKEN" -> "TOKEN" */
	if (path_len > 0 && path[0] == '/') {
		path++;
		path_len--;
	}
	/* Strip "ws/" prefix if present (nginx proxies full URI) */
	if (path_len > 3 && strncmp(path, "ws/", 3) == 0) {
		path += 3;
		path_len -= 3;
	}

	if (path_len == 0)
		return -1;

	/* Check against all session tokens */
	token = s->session_token;
	if (token) {
		tlen = strlen(token);
		if (tlen == path_len && memcmp(path, token, tlen) == 0)
			return 0;
	}

	token = s->session_token_ro;
	if (token) {
		tlen = strlen(token);
		if (tlen == path_len && memcmp(path, token, tlen) == 0)
			return 0;
	}

	token = s->session_token_named;
	if (token) {
		tlen = strlen(token);
		if (tlen == path_len && memcmp(path, token, tlen) == 0)
			return 0;
	}

	return -1;
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

	/* Parse GET path from "GET /path HTTP/..." */
	static const char *reject =
		"HTTP/1.1 403 Forbidden\r\n"
		"Content-Length: 0\r\n"
		"Connection: close\r\n"
		"\r\n";

	if (strncmp(data, "GET ", 4) == 0) {
		const char *path_start = data + 4;
		const char *path_end = memchr(path_start, ' ',
					      header_end - path_start);
		if (path_end) {
			if (sse_validate_token(wc, path_start,
					       path_end - path_start) < 0) {
				tmate_debug("SSE token rejected");
				evbuffer_drain(input, (header_end - data) + 4);
				bufferevent_write(wc->bev, reject,
						  strlen(reject));
				return -1;
			}
		}
	}

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

	screen = pane->screen;
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

		/*
		 * Compute str_len, replacing control chars (< 0x20) with
		 * space to keep cell alignment with attrs array.
		 */
		str_len = 0;
		for (i = 0; i < line->cellsize; i++) {
			grid_get_cell(grid, i, line_i, &gc);
			str_len += gc.data.size;
		}

		pack(str, str_len);
		for (i = 0; i < line->cellsize; i++) {
			grid_get_cell(grid, i, line_i, &gc);
			/*
			 * Replace control characters (ESC, etc.) with space.
			 * The grid may contain raw escape bytes from the
			 * tmate client protocol; these must not reach the
			 * web viewer.
			 */
			if (gc.data.size == 1 &&
			    ((unsigned char)gc.data.data[0] < 0x20 ||
			     (unsigned char)gc.data.data[0] == 0x7f)) {
				char sp = ' ';
				pack(str_body, &sp, 1);
			} else {
				pack(str_body, gc.data.data, gc.data.size);
			}
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

static void tmate_send_snapshot_ex(unsigned int max_history)
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
			do_snapshot(max_history, pane);
	}
}

static void tmate_send_snapshot(void)
{
	tmate_send_snapshot_ex(TMATE_HLIMIT);
}

/* --- Viewer counting --- */

static int count_web_viewers(struct tmate_session *session)
{
	struct ws_client *wc;
	int count = 0;
	TAILQ_FOREACH(wc, &session->ws_clients, entry) {
		if (wc->handshake_done)
			count++;
	}
	return count;
}

static void count_ssh_viewers(int *rw, int *ro)
{
	struct client *c;
	*rw = 0;
	*ro = 0;
	TAILQ_FOREACH(c, &clients, entry) {
		if (!(c->flags & CLIENT_IDENTIFIED))
			continue;
		if (c->readonly)
			(*ro)++;
		else
			(*rw)++;
	}
}

static void sse_send_viewer_count(struct bufferevent *bev,
				  int ssh_rw, int ssh_ro, int web)
{
	msgpack_sbuffer sbuf;
	msgpack_packer pk;

	msgpack_sbuffer_init(&sbuf);
	msgpack_packer_init(&pk, &sbuf, msgpack_sbuffer_write);

	msgpack_pack_array(&pk, 2);
	msgpack_pack_int(&pk, TMATE_CTL_DEAMON_OUT_MSG);

	msgpack_pack_array(&pk, 4);
	msgpack_pack_int(&pk, TMATE_OUT_VIEWER_COUNT);
	msgpack_pack_int(&pk, ssh_rw);
	msgpack_pack_int(&pk, ssh_ro);
	msgpack_pack_int(&pk, web);

	sse_send_data(bev, (unsigned char *)sbuf.data, sbuf.size);
	msgpack_sbuffer_destroy(&sbuf);
}

void tmate_broadcast_viewer_count(struct tmate_session *session)
{
	struct ws_client *wc;
	int ssh_rw, ssh_ro, web;
	char buf[32];

	if (!tmate_has_websocket())
		return;

	count_ssh_viewers(&ssh_rw, &ssh_ro);
	web = count_web_viewers(session);

	/* Update format variables for status bar */
	snprintf(buf, sizeof(buf), "%d", ssh_rw + ssh_ro);
	tmate_set_env("tmtv_ssh_viewers", buf);
	snprintf(buf, sizeof(buf), "%d", web);
	tmate_set_env("tmtv_web_viewers", buf);

	/* Broadcast to all SSE clients */
	TAILQ_FOREACH(wc, &session->ws_clients, entry) {
		if (wc->handshake_done)
			sse_send_viewer_count(wc->bev, ssh_rw, ssh_ro, web);
	}

	/* Trigger status bar redraw for SSH viewers */
	struct client *c;
	TAILQ_FOREACH(c, &clients, entry)
		c->flags |= CLIENT_REDRAWSTATUS;
}

/* --- Client management --- */

static void ws_client_free(struct ws_client *wc)
{
	struct tmate_session *session = wc->session;
	bool was_connected = wc->handshake_done;

	tmate_info("SSE client disconnected");
	TAILQ_REMOVE(&session->ws_clients, wc, entry);
	if (wc->bev)
		bufferevent_free(wc->bev);
	free(wc);

	if (was_connected)
		tmate_broadcast_viewer_count(session);
}

void tmate_disconnect_ws_clients(struct tmate_session *session)
{
	struct ws_client *wc, *tmp;
	TAILQ_FOREACH_SAFE(wc, &session->ws_clients, entry, tmp)
		ws_client_free(wc);
	tmate_debug("All SSE clients disconnected (web sharing disabled)");
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

		/* Send layout sync so browser knows terminal size + window names */
		sse_send_sync_layout(wc->bev);
		/* Send current screen state from tmux grid */
		sse_send_screen_dump(wc->bev);
		/* Broadcast updated viewer counts to all clients */
		tmate_broadcast_viewer_count(wc->session);
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

	if (!session->web_sharing_enabled) {
		close(fd);
		tmate_debug("SSE connection rejected: web sharing is disabled");
		return;
	}

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

	/* Store in replay buffer for new clients */
	pty_replay_append(data, len);

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

/* --- Periodic snapshot timer --- */

#define SSE_SNAPSHOT_INTERVAL_MS 50

static bool ws_has_clients(struct tmate_session *session)
{
	struct ws_client *wc;
	TAILQ_FOREACH(wc, &session->ws_clients, entry) {
		if (wc->handshake_done)
			return true;
	}
	return false;
}

static void on_snapshot_timer(__unused evutil_socket_t fd,
			      __unused short what, void *arg)
{
	struct tmate_session *session = arg;

	if (!ws_has_clients(session))
		return;

	/* Snapshots disabled — the tmate server grid contains raw
	 * escape sequences (not parsed characters).  SSE clients
	 * receive PTY data directly and xterm.js handles parsing. */
	(void)session;
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

	tmate_broadcast_viewer_count(session);
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

	tmate_broadcast_viewer_count(session);
}

void tmate_send_websocket_daemon_msg(__unused struct tmate_session *session,
				     struct tmate_unpacker *uk)
{
	struct ws_client *wc;
	msgpack_sbuffer sbuf;
	msgpack_packer pk;
	int i;

	if (!tmate_has_websocket())
		return;

	/* Pack the message into a temporary buffer and send directly
	 * to all SSE clients, bypassing the encoder event mechanism
	 * which can be broken by fork/event_reinit. */
	msgpack_sbuffer_init(&sbuf);
	msgpack_packer_init(&pk, &sbuf, msgpack_sbuffer_write);

	msgpack_pack_array(&pk, 2);
	msgpack_pack_int(&pk, TMATE_CTL_DEAMON_OUT_MSG);

	msgpack_pack_array(&pk, uk->argc);
	for (i = 0; i < uk->argc; i++)
		_msgpack_pack_object(&pk, uk->argv[i]);

	/* Store in replay buffer */
	pty_replay_append((unsigned char *)sbuf.data, sbuf.size);

	/* Broadcast to all connected SSE clients */
	TAILQ_FOREACH(wc, &session->ws_clients, entry) {
		if (wc->handshake_done)
			sse_send_data(wc->bev, (unsigned char *)sbuf.data, sbuf.size);
	}

	msgpack_sbuffer_destroy(&sbuf);
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

	pty_replay_init();
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

	/* Periodic snapshot timer for live updates */
	{
		struct timeval tv = { 0, SSE_SNAPSHOT_INTERVAL_MS * 1000 };
		session->ev_ws_snapshot = event_new(session->ev_base, -1,
						    EV_PERSIST, on_snapshot_timer,
						    session);
		event_add(session->ev_ws_snapshot, &tv);
		tmate_info("SSE snapshot timer started (%dms)", SSE_SNAPSHOT_INTERVAL_MS);
	}
}
