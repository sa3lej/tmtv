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

/*
 * SSE multiplexing: main process owns port 4002, routes incoming SSE
 * connections to the correct daemon child via fd passing (SCM_RIGHTS).
 *
 * Before this, each daemon fork tried to bind port 4002 independently,
 * meaning only the first session could web-share. Now the main process
 * binds once, reads the HTTP GET to extract the token, looks up which
 * daemon owns it, and passes the raw TCP fd over a Unix socketpair.
 */

#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <stdio.h>
#include <time.h>

#include "tmate.h"

/* Best-effort write for HTTP error/status responses on closing fds. */
static void
sse_write_response(int fd, const void *buf, size_t len)
{
	ssize_t __unused n = write(fd, buf, len);
}

/* --- Session registry --- */

void sse_registry_init(struct sse_registry *reg)
{
	memset(reg, 0, sizeof(*reg));
}

void sse_registry_add(struct sse_registry *reg,
		      const char *token, int ipc_fd, pid_t pid)
{
	struct sse_registry_entry *e;

	/*
	 * If this token already exists (e.g. named session token after client
	 * reconnection), replace the old entry so lookups hit the new daemon.
	 */
	for (int i = 0; i < reg->count; i++) {
		if (strcmp(reg->entries[i].token, token) == 0) {
			tmate_info("SSE registry: replacing token %.4s... "
			    "old pid=%d fd=%d -> new pid=%d fd=%d",
			    token, reg->entries[i].pid, reg->entries[i].ipc_fd,
			    pid, ipc_fd);
			reg->entries[i].ipc_fd = ipc_fd;
			reg->entries[i].pid = pid;
			return;
		}
	}

	if (reg->count >= (int)(sizeof(reg->entries) / sizeof(reg->entries[0]))) {
		tmate_info("SSE registry full, cannot register token %.4s...", token);
		return;
	}

	e = &reg->entries[reg->count++];
	strlcpy(e->token, token, sizeof(e->token));
	e->ipc_fd = ipc_fd;
	e->pid = pid;

	tmate_debug("SSE registry: added token %.4s... ipc_fd=%d pid=%d (total=%d)",
		    token, ipc_fd, pid, reg->count);
}

int sse_registry_lookup(struct sse_registry *reg, const char *token)
{
	for (int i = 0; i < reg->count; i++) {
		if (strcmp(reg->entries[i].token, token) == 0)
			return reg->entries[i].ipc_fd;
	}
	return -1;
}

void sse_registry_remove_by_pid(struct sse_registry *reg, pid_t pid)
{
	int i = 0;
	while (i < reg->count) {
		if (reg->entries[i].pid == pid) {
			tmate_debug("SSE registry: removing token %.4s... (pid=%d)",
				    reg->entries[i].token, pid);
			reg->entries[i] = reg->entries[reg->count - 1];
			reg->count--;
		} else {
			i++;
		}
	}
}

void sse_registry_remove_by_fd(struct sse_registry *reg, int ipc_fd)
{
	int i = 0;
	while (i < reg->count) {
		if (reg->entries[i].ipc_fd == ipc_fd) {
			reg->entries[i] = reg->entries[reg->count - 1];
			reg->count--;
		} else {
			i++;
		}
	}
}

/* --- fd passing via SCM_RIGHTS --- */

int sse_send_fd(int ipc_fd, int fd_to_pass)
{
	struct msghdr msg;
	struct iovec iov;
	char buf[1] = {'F'}; /* dummy byte; sendmsg requires at least 1 byte */
	char cmsgbuf[CMSG_SPACE(sizeof(int))];
	struct cmsghdr *cmsg;

	memset(&msg, 0, sizeof(msg));

	iov.iov_base = buf;
	iov.iov_len = 1;
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;

	msg.msg_control = cmsgbuf;
	msg.msg_controllen = sizeof(cmsgbuf);

	cmsg = CMSG_FIRSTHDR(&msg);
	cmsg->cmsg_level = SOL_SOCKET;
	cmsg->cmsg_type = SCM_RIGHTS;
	cmsg->cmsg_len = CMSG_LEN(sizeof(int));
	memcpy(CMSG_DATA(cmsg), &fd_to_pass, sizeof(int));

	if (sendmsg(ipc_fd, &msg, 0) < 0) {
		tmate_info("sse_send_fd: sendmsg failed: %s", strerror(errno));
		return -1;
	}
	return 0;
}

int sse_recv_fd(int ipc_fd)
{
	struct msghdr msg;
	struct iovec iov;
	char buf[1];
	char cmsgbuf[CMSG_SPACE(sizeof(int))];
	struct cmsghdr *cmsg;
	int fd = -1;

	memset(&msg, 0, sizeof(msg));

	iov.iov_base = buf;
	iov.iov_len = 1;
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;

	msg.msg_control = cmsgbuf;
	msg.msg_controllen = sizeof(cmsgbuf);

	ssize_t n = recvmsg(ipc_fd, &msg, 0);
	if (n <= 0)
		return -1;

	cmsg = CMSG_FIRSTHDR(&msg);
	if (cmsg && cmsg->cmsg_level == SOL_SOCKET &&
	    cmsg->cmsg_type == SCM_RIGHTS &&
	    cmsg->cmsg_len == CMSG_LEN(sizeof(int))) {
		memcpy(&fd, CMSG_DATA(cmsg), sizeof(int));
	}

	return fd;
}

/* --- IPC text messages --- */

int sse_ipc_send_msg(int ipc_fd, const char *msg)
{
	size_t len = strlen(msg);
	ssize_t n = write(ipc_fd, msg, len);
	if (n < 0 || (size_t)n != len) {
		tmate_info("sse_ipc_send_msg: write failed: %s", strerror(errno));
		return -1;
	}
	return 0;
}

int sse_ipc_read_msg(int ipc_fd, char *buf, size_t len)
{
	ssize_t n = read(ipc_fd, buf, len - 1);
	if (n <= 0)
		return -1;
	buf[n] = '\0';
	return (int)n;
}

/* --- Main process SSE listener --- */

int sse_bind_listener(int port)
{
	struct sockaddr_in sin;
	int fd, flag = 1;

	fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
	if (fd < 0) {
		tmate_info("Cannot create SSE listener socket: %s",
			   strerror(errno));
		return -1;
	}

	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));

	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = htonl(INADDR_ANY);
	sin.sin_port = htons(port);

	if (bind(fd, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
		tmate_info("Cannot bind SSE listener on port %d: %s",
			   port, strerror(errno));
		close(fd);
		return -1;
	}

	if (listen(fd, 128) < 0) {
		tmate_info("Cannot listen on SSE listener: %s",
			   strerror(errno));
		close(fd);
		return -1;
	}

	tmate_info("SSE multiplexer bound on port %d (fd=%d)", port, fd);
	return fd;
}

/*
 * Extract token from an HTTP GET request line.
 * Expected: "GET /ws/<token> HTTP/1.1\r\n..."
 * or:       "GET /<token> HTTP/1.1\r\n..."
 *
 * Returns 0 on success, -1 on failure.
 * Token is written to dst (null-terminated, max dst_len-1 chars).
 */
static int extract_token_from_http(const char *buf, size_t buflen,
				   char *dst, size_t dst_len)
{
	const char *path_start, *path_end, *tok_start;
	size_t tok_len;

	if (buflen >= 5 && strncmp(buf, "POST ", 5) == 0) {
		path_start = buf + 5;
	} else if (buflen >= 4 && strncmp(buf, "GET ", 4) == 0) {
		path_start = buf + 4;
	} else {
		return -1;
	}
	path_end = memchr(path_start, ' ', buflen - 4);
	if (!path_end)
		return -1;

	/* Skip leading '/' */
	tok_start = path_start;
	if (*tok_start == '/')
		tok_start++;

	/* Skip "ws/" prefix if present */
	if ((size_t)(path_end - tok_start) > 3 &&
	    strncmp(tok_start, "ws/", 3) == 0)
		tok_start += 3;

	tok_len = path_end - tok_start;
	if (tok_len == 0 || tok_len >= dst_len)
		return -1;

	/* Strip query string (?password=...) — token is before '?' */
	{
		const char *qmark = memchr(tok_start, '?', tok_len);
		if (qmark)
			tok_len = qmark - tok_start;
	}

	/* Strip "/input" suffix (POST input endpoint) */
	if (tok_len > 6 && memcmp(tok_start + tok_len - 6, "/input", 6) == 0)
		tok_len -= 6;

	if (tok_len == 0 || tok_len >= dst_len)
		return -1;

	memcpy(dst, tok_start, tok_len);
	dst[tok_len] = '\0';
	return 0;
}

void sse_handle_connection(int sse_listen_fd, struct sse_registry *reg)
{
	struct sockaddr_storage sa;
	socklen_t sa_len = sizeof(sa);
	int client_fd;
	char buf[1024];
	ssize_t n;
	char token[SSE_TOKEN_MAX];
	int ipc_fd;

	client_fd = accept(sse_listen_fd, (struct sockaddr *)&sa, &sa_len);
	if (client_fd < 0) {
		if (errno != EINTR && errno != EAGAIN)
			tmate_info("SSE accept failed: %s", strerror(errno));
		return;
	}

	/* Set a short read timeout for the HTTP request line */
	{
		struct timeval tv = { 1, 0 }; /* 1 second */
		setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	}

	/*
	 * Read just enough to get the GET line. We use MSG_PEEK so the
	 * daemon receives the full HTTP request including the GET line
	 * and can do its own handshake validation.
	 */
	n = recv(client_fd, buf, sizeof(buf) - 1, MSG_PEEK);
	if (n <= 0) {
		close(client_fd);
		return;
	}
	buf[n] = '\0';

	/* Need at least the first HTTP line (ends with \r\n) */
	if (!memchr(buf, '\n', n)) {
		close(client_fd);
		return;
	}

	/* Handle CORS preflight (OPTIONS) directly — no daemon needed */
	if (n >= 8 && strncmp(buf, "OPTIONS ", 8) == 0) {
		/* Consume the peeked data */
		(void)recv(client_fd, buf, sizeof(buf) - 1, 0);
		static const char *cors =
			"HTTP/1.1 204 No Content\r\n"
			"Access-Control-Allow-Origin: *\r\n"
			"Access-Control-Allow-Methods: GET, POST\r\n"
			"Access-Control-Allow-Headers: Content-Type, X-Tmtv-Input\r\n"
			"Access-Control-Max-Age: 86400\r\n"
			"Content-Length: 0\r\n"
			"\r\n";
		sse_write_response(client_fd, cors, strlen(cors));
		close(client_fd);
		return;
	}

	/* Handle health check endpoint directly — no daemon needed */
	if (n >= 4 && strncmp(buf, "GET ", 4) == 0) {
		const char *path = buf + 4;
		const char *path_end = memchr(path, ' ', n - 4);
		size_t path_len = path_end ? (size_t)(path_end - path) : 0;

		if (path_len == 8 && memcmp(path, "/healthz", 8) == 0) {
			/* Consume the peeked data */
			(void)recv(client_fd, buf, sizeof(buf) - 1, 0);

			time_t now = time(NULL);
			time_t start = tmate_server_get_start_time();
			long uptime = (long)(now - start);
			int sessions = tmate_server_get_active_sessions();

			char body[256];
			int body_len = snprintf(body, sizeof(body),
				"{\"status\":\"ok\","
				"\"version\":\"%s\","
				"\"uptime_seconds\":%ld,"
				"\"active_sessions\":%d}",
				TMTV_VERSION, uptime, sessions);

			char resp[512];
			int resp_len = snprintf(resp, sizeof(resp),
				"HTTP/1.1 200 OK\r\n"
				"Content-Type: application/json\r\n"
				"Access-Control-Allow-Origin: *\r\n"
				"Content-Length: %d\r\n"
				"Connection: close\r\n"
				"\r\n"
				"%s", body_len, body);

			sse_write_response(client_fd, resp, resp_len);
			close(client_fd);
			return;
		}
	}

	if (extract_token_from_http(buf, n, token, sizeof(token)) < 0) {
		tmate_debug("SSE mux: cannot extract token from request");
		/* Send 404 and close */
		static const char *not_found =
			"HTTP/1.1 404 Not Found\r\n"
			"Content-Length: 0\r\n"
			"Connection: close\r\n"
			"\r\n";
		sse_write_response(client_fd, not_found, strlen(not_found));
		close(client_fd);
		return;
	}

	ipc_fd = sse_registry_lookup(reg, token);
	if (ipc_fd < 0) {
		tmate_debug("SSE mux: token %.4s... not found in registry", token);
		static const char *not_found =
			"HTTP/1.1 404 Not Found\r\n"
			"Content-Length: 0\r\n"
			"Connection: close\r\n"
			"\r\n";
		sse_write_response(client_fd, not_found, strlen(not_found));
		close(client_fd);
		return;
	}

	/* Clear the receive timeout before passing fd */
	{
		struct timeval tv = { 0, 0 };
		setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	}

	tmate_debug("SSE mux: routing token %.4s... to ipc_fd=%d", token, ipc_fd);

	if (sse_send_fd(ipc_fd, client_fd) < 0) {
		tmate_info("SSE mux: failed to pass fd for token %.4s...", token);
		static const char *err =
			"HTTP/1.1 502 Bad Gateway\r\n"
			"Content-Length: 0\r\n"
			"Connection: close\r\n"
			"\r\n";
		sse_write_response(client_fd, err, strlen(err));
	}

	/* Main process closes its copy; daemon has the fd now */
	close(client_fd);
}
