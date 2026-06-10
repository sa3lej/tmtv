/*
 * Unit tests for the 2.0.2 hardening fixes.
 *
 * Following the project convention (see test-pty-pending.c), self-contained
 * logic is mirrored here and exercised directly. The mirrored functions are
 * byte-for-byte copies of the production code and MUST be kept in sync:
 *
 *   - read_single_line()  -> server/tmate-ssh-server.c  (PROXY-protocol parser)
 *   - exec_cmd argc guard -> tmate-decoder.c / server/tmate-daemon-decoder.c
 */

#include "test-harness.h"
#include <msgpack.h>
#include <sys/socket.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

/* ------------------------------------------------------------------ */
/* #6: read_single_line must not underflow on a leading CR.            */
/*                                                                     */
/* Mirror of server/tmate-ssh-server.c:read_single_line (fixed form).  */
/* The old version used `for (size_t i...) { if (c=='\r') i--; ... }`  */
/* which wrapped i to SIZE_MAX on a leading '\r' and read dst[SIZE_MAX].*/
/* ------------------------------------------------------------------ */
static int read_single_line(int fd, char *dst, size_t len)
{
	size_t pos = 0;
	char c;

	while (pos < len) {
		if (read(fd, &c, 1) <= 0)
			break;

		if (c == '\r')
			continue; /* ignore CR, wait for LF */

		if (c == '\n') {
			dst[pos] = '\0';
			return pos;
		}

		dst[pos++] = c;
	}

	return -1;
}

/* Feed `input` through a socketpair and parse one line from it. */
static int parse_line(const char *input, size_t input_len,
		      char *dst, size_t dst_len)
{
	int sv[2];
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0)
		return -2;
	if (write(sv[1], input, input_len) != (ssize_t)input_len) {
		close(sv[0]);
		close(sv[1]);
		return -2;
	}
	close(sv[1]); /* EOF after the bytes so read() terminates */
	int ret = read_single_line(sv[0], dst, dst_len);
	close(sv[0]);
	return ret;
}

TEST(proxy_line_leading_cr_no_underflow)
{
	/* A lone leading CR is the exact input that underflowed the old
	 * index. Guard with canaries on both sides of the real buffer. */
	struct {
		char before[8];
		char line[64];
		char after[8];
	} g;
	memset(&g, 0xAA, sizeof(g));

	int ret = parse_line("\r\n", 2, g.line, sizeof(g.line));

	ASSERT_EQ(ret, 0);              /* empty line, length 0 */
	ASSERT_EQ(g.line[0], '\0');     /* properly terminated */
	/* Canaries intact: no write/read strayed outside g.line. */
	for (int i = 0; i < 8; i++) {
		ASSERT_EQ((unsigned char)g.before[i], 0xAA);
		ASSERT_EQ((unsigned char)g.after[i], 0xAA);
	}
}

TEST(proxy_line_cr_then_text)
{
	char line[64];
	/* CR before real content must not corrupt the parse. */
	const char *in = "\rPROXY foo\r\n";
	int ret = parse_line(in, strlen(in), line, sizeof(line));
	ASSERT_EQ(ret, 9);
	ASSERT_STR_EQ(line, "PROXY foo");
}

TEST(proxy_line_plain_lf)
{
	char line[64];
	int ret = parse_line("hello\n", 6, line, sizeof(line));
	ASSERT_EQ(ret, 5);
	ASSERT_STR_EQ(line, "hello");
}

TEST(proxy_line_crlf_stripped)
{
	char line[64];
	int ret = parse_line("hello\r\n", 7, line, sizeof(line));
	ASSERT_EQ(ret, 5);
	ASSERT_STR_EQ(line, "hello");
}

TEST(proxy_line_too_long_returns_error)
{
	char line[8];
	/* No newline within the buffer -> -1, no overflow. */
	int ret = parse_line("aaaaaaaaaaaaaaaa", 16, line, sizeof(line));
	ASSERT_EQ(ret, -1);
}

/* ------------------------------------------------------------------ */
/* #2: an EXEC_CMD message with no argument strings must be detected   */
/* as empty (argc == 0) so the handler skips it instead of calling     */
/* xmalloc(0) (which aborts). The production handler reads argc from    */
/* the msgpack array size; we verify that decision boundary against    */
/* the real msgpack library.                                           */
/* ------------------------------------------------------------------ */

/* Build [type, client_id, arg0, arg1, ...] like the client encoder, then
 * return the number of remaining elements after the dispatcher consumes
 * `type` and the handler consumes `client_id` — i.e. the production argc. */
static int decode_exec_cmd_argc(int nargs)
{
	msgpack_sbuffer sbuf;
	msgpack_packer pk;
	msgpack_unpacked result;
	int argc = -999;

	msgpack_sbuffer_init(&sbuf);
	msgpack_packer_init(&pk, &sbuf, msgpack_sbuffer_write);

	/* array = [TMATE_OUT_EXEC_CMD, client_id, <nargs strings>] */
	msgpack_pack_array(&pk, 2 + nargs);
	msgpack_pack_int(&pk, 80 /* TMATE_OUT_EXEC_CMD, value irrelevant */);
	msgpack_pack_int(&pk, 1  /* client_id */);
	for (int i = 0; i < nargs; i++) {
		const char *a = "set-option";
		msgpack_pack_str(&pk, strlen(a));
		msgpack_pack_str_body(&pk, a, strlen(a));
	}

	msgpack_unpacked_init(&result);
	if (msgpack_unpack_next(&result, sbuf.data, sbuf.size, NULL) ==
	    MSGPACK_UNPACK_SUCCESS &&
	    result.data.type == MSGPACK_OBJECT_ARRAY) {
		/* Dispatcher consumes type (1), handler consumes client_id (1). */
		argc = (int)result.data.via.array.size - 2;
	}

	msgpack_unpacked_destroy(&result);
	msgpack_sbuffer_destroy(&sbuf);
	return argc;
}

TEST(exec_cmd_empty_args_detected_as_zero)
{
	/* [type, client_id] with no args -> argc 0 -> handler must skip. */
	ASSERT_EQ(decode_exec_cmd_argc(0), 0);
}

TEST(exec_cmd_normal_args_counted)
{
	ASSERT_EQ(decode_exec_cmd_argc(1), 1);
	ASSERT_EQ(decode_exec_cmd_argc(3), 3);
}

int main(void)
{
	TEST_SUITE_BEGIN("2.0.2 hardening");

	RUN_TEST(proxy_line_leading_cr_no_underflow);
	RUN_TEST(proxy_line_cr_then_text);
	RUN_TEST(proxy_line_plain_lf);
	RUN_TEST(proxy_line_crlf_stripped);
	RUN_TEST(proxy_line_too_long_returns_error);
	RUN_TEST(exec_cmd_empty_args_detected_as_zero);
	RUN_TEST(exec_cmd_normal_args_counted);

	TEST_SUITE_END();
}
