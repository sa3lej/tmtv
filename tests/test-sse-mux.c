/*
 * Unit tests for SSE IPC message framing and registry.
 *
 * Tests that newline-delimited IPC messages are correctly parsed
 * even when coalesced in a single read() on SOCK_STREAM socketpairs.
 */

#include "test-harness.h"
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

/* Suppress logging macros before including server header */
#define TMATE_SSE_MUX_TEST
#define tmate_info(fmt, ...) do { (void)(fmt); } while (0)
#define tmate_debug(fmt, ...) do { (void)(fmt); } while (0)
#define tmate_fatal(fmt, ...) do { (void)(fmt); abort(); } while (0)

/* We only need the SSE registry types from the server header.
 * Define the types directly to avoid pulling in all dependencies. */
#define SSE_IPC_MSG_REGISTER   "REG"
#define SSE_TOKEN_MAX    96

struct sse_registry_entry {
	char token[SSE_TOKEN_MAX];
	int  ipc_fd;
	pid_t pid;
};

#define SSE_MAX_SESSIONS 256

struct sse_registry {
	struct sse_registry_entry entries[SSE_MAX_SESSIONS];
	int count;
};

/* --- Include the SSE mux source directly for testing --- */

/* We need the registry functions. Re-implement the minimal set
 * here to avoid pulling in the entire server build. */

static void test_registry_init(struct sse_registry *reg)
{
	memset(reg, 0, sizeof(*reg));
}

static void test_registry_add(struct sse_registry *reg,
			      const char *token, int ipc_fd, pid_t pid)
{
	struct sse_registry_entry *e;

	for (int i = 0; i < reg->count; i++) {
		if (strcmp(reg->entries[i].token, token) == 0) {
			reg->entries[i].ipc_fd = ipc_fd;
			reg->entries[i].pid = pid;
			return;
		}
	}

	if (reg->count >= (int)(sizeof(reg->entries) / sizeof(reg->entries[0])))
		return;

	e = &reg->entries[reg->count++];
	snprintf(e->token, sizeof(e->token), "%s", token);
	e->ipc_fd = ipc_fd;
	e->pid = pid;
}

static int test_registry_lookup(struct sse_registry *reg, const char *token)
{
	for (int i = 0; i < reg->count; i++) {
		if (strcmp(reg->entries[i].token, token) == 0)
			return reg->entries[i].ipc_fd;
	}
	return -1;
}

/*
 * Simulate the parse_ipc_line + handle_ipc_registrations logic
 * from tmate-ssh-server.c
 */
static void sim_parse_ipc_line(struct sse_registry *reg, char *line,
			       int ipc_fd, pid_t pid)
{
	if (strncmp(line, SSE_IPC_MSG_REGISTER " ", 4) == 0) {
		char *saveptr;
		char *tok = strtok_r(line + 4, " ", &saveptr);
		while (tok) {
			test_registry_add(reg, tok, ipc_fd, pid);
			tok = strtok_r(NULL, " ", &saveptr);
		}
	}
}

static void sim_handle_ipc(struct sse_registry *reg, char *buf,
			    int ipc_fd, pid_t pid)
{
	char *saveptr;
	char *line = strtok_r(buf, "\n", &saveptr);
	while (line) {
		sim_parse_ipc_line(reg, line, ipc_fd, pid);
		line = strtok_r(NULL, "\n", &saveptr);
	}
}

/* --- Tests --- */

TEST(single_reg_message)
{
	struct sse_registry reg;
	test_registry_init(&reg);

	char buf[] = "REG token1 token2\n";
	sim_handle_ipc(&reg, buf, 5, 100);

	ASSERT_EQ(reg.count, 2);
	ASSERT(test_registry_lookup(&reg, "token1") == 5);
	ASSERT(test_registry_lookup(&reg, "token2") == 5);
}

TEST(coalesced_reg_messages)
{
	struct sse_registry reg;
	test_registry_init(&reg);

	/* Two REG messages coalesced into one read */
	char buf[] = "REG randomtok rotok\nREG dev 12345-dev ro-dev\n";
	sim_handle_ipc(&reg, buf, 5, 100);

	ASSERT_EQ(reg.count, 5);
	ASSERT(test_registry_lookup(&reg, "randomtok") == 5);
	ASSERT(test_registry_lookup(&reg, "rotok") == 5);
	ASSERT(test_registry_lookup(&reg, "dev") == 5);
	ASSERT(test_registry_lookup(&reg, "12345-dev") == 5);
	ASSERT(test_registry_lookup(&reg, "ro-dev") == 5);
	/* "REG" should NOT be registered as a token */
	ASSERT(test_registry_lookup(&reg, "REG") == -1);
}

TEST(three_coalesced_messages)
{
	struct sse_registry reg;
	test_registry_init(&reg);

	/* Three messages coalesced */
	char buf[] = "REG a b\nREG c d\nREG e f\n";
	sim_handle_ipc(&reg, buf, 7, 200);

	ASSERT_EQ(reg.count, 6);
	ASSERT(test_registry_lookup(&reg, "a") == 7);
	ASSERT(test_registry_lookup(&reg, "b") == 7);
	ASSERT(test_registry_lookup(&reg, "c") == 7);
	ASSERT(test_registry_lookup(&reg, "d") == 7);
	ASSERT(test_registry_lookup(&reg, "e") == 7);
	ASSERT(test_registry_lookup(&reg, "f") == 7);
}

TEST(no_trailing_newline)
{
	struct sse_registry reg;
	test_registry_init(&reg);

	/* Message without trailing newline (partial read) */
	char buf[] = "REG tok1 tok2";
	sim_handle_ipc(&reg, buf, 5, 100);

	ASSERT_EQ(reg.count, 2);
	ASSERT(test_registry_lookup(&reg, "tok1") == 5);
	ASSERT(test_registry_lookup(&reg, "tok2") == 5);
}

TEST(ipc_writev_adds_newline)
{
	int sv[2];
	char buf[256];
	ssize_t n;

	ASSERT(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

	/* Simulate sse_ipc_send_msg: writev with newline */
	const char *msg = "REG hello world";
	struct iovec iov[2];
	iov[0].iov_base = (void *)msg;
	iov[0].iov_len = strlen(msg);
	iov[1].iov_base = "\n";
	iov[1].iov_len = 1;
	n = writev(sv[0], iov, 2);
	ASSERT(n == (ssize_t)(strlen(msg) + 1));

	/* Read on the other end */
	n = read(sv[1], buf, sizeof(buf) - 1);
	ASSERT(n > 0);
	buf[n] = '\0';

	/* Should contain the newline */
	ASSERT(strchr(buf, '\n') != NULL);

	struct sse_registry reg;
	test_registry_init(&reg);
	sim_handle_ipc(&reg, buf, 3, 42);
	ASSERT_EQ(reg.count, 2);
	ASSERT(test_registry_lookup(&reg, "hello") == 3);
	ASSERT(test_registry_lookup(&reg, "world") == 3);

	close(sv[0]);
	close(sv[1]);
}

TEST(two_rapid_writes_coalesce)
{
	int sv[2];
	char buf[512];
	ssize_t n;

	ASSERT(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

	/* Write two messages rapidly (simulating the real bug) */
	const char *msg1 = "REG randomtoken rotok";
	struct iovec iov1[2] = {
		{ .iov_base = (void *)msg1, .iov_len = strlen(msg1) },
		{ .iov_base = "\n", .iov_len = 1 }
	};
	ASSERT(writev(sv[0], iov1, 2) > 0);

	const char *msg2 = "REG dev 99999-dev ro-dev";
	struct iovec iov2[2] = {
		{ .iov_base = (void *)msg2, .iov_len = strlen(msg2) },
		{ .iov_base = "\n", .iov_len = 1 }
	};
	ASSERT(writev(sv[0], iov2, 2) > 0);

	/* Brief delay not needed — SOCK_STREAM coalesces even without
	 * one if both writes complete before the reader runs.
	 * But we read in a single call to simulate the real scenario. */
	n = read(sv[1], buf, sizeof(buf) - 1);
	ASSERT(n > 0);
	buf[n] = '\0';

	struct sse_registry reg;
	test_registry_init(&reg);
	sim_handle_ipc(&reg, buf, 8, 300);

	/* All 5 tokens must be registered (not 3 with "REG" as a token) */
	ASSERT_EQ(reg.count, 5);
	ASSERT(test_registry_lookup(&reg, "randomtoken") == 8);
	ASSERT(test_registry_lookup(&reg, "rotok") == 8);
	ASSERT(test_registry_lookup(&reg, "dev") == 8);
	ASSERT(test_registry_lookup(&reg, "99999-dev") == 8);
	ASSERT(test_registry_lookup(&reg, "ro-dev") == 8);
	ASSERT(test_registry_lookup(&reg, "REG") == -1);

	close(sv[0]);
	close(sv[1]);
}

TEST(replacement_on_reconnect)
{
	struct sse_registry reg;
	test_registry_init(&reg);

	/* Session 1 registers "dev" */
	char buf1[] = "REG dev\n";
	sim_handle_ipc(&reg, buf1, 5, 100);
	ASSERT_EQ(reg.count, 1);
	ASSERT(test_registry_lookup(&reg, "dev") == 5);

	/* Session 1 reconnects — same token, new fd */
	char buf2[] = "REG dev\n";
	sim_handle_ipc(&reg, buf2, 9, 200);
	ASSERT_EQ(reg.count, 1);  /* replaced, not duplicated */
	ASSERT(test_registry_lookup(&reg, "dev") == 9);
}

int main(void)
{
	TEST_SUITE_BEGIN("SSE IPC message framing");

	RUN_TEST(single_reg_message);
	RUN_TEST(coalesced_reg_messages);
	RUN_TEST(three_coalesced_messages);
	RUN_TEST(no_trailing_newline);
	RUN_TEST(ipc_writev_adds_newline);
	RUN_TEST(two_rapid_writes_coalesce);
	RUN_TEST(replacement_on_reconnect);

	TEST_SUITE_END();
}
