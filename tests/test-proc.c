/* Real Unix sockets, production peer callbacks and imsg descriptor transport. */
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>
#include "../proc.c"
#include "test-harness.h"

struct received {
	int messages, disconnected, type, version, fd;
	char data[64];
};

static void
dispatch(struct imsg *msg, void *arg)
{
	struct received *r = arg;
	size_t size;
	if (msg == NULL) {
		r->disconnected++;
		return;
	}
	r->messages++;
	r->type = msg->hdr.type;
	r->version = msg->hdr.peerid;
	r->fd = imsg_get_fd(msg);
	size = imsg_get_len(msg);
	if (size >= sizeof r->data)
		abort();
	if (size != 0 && imsg_get_data(msg, r->data, size) != 0)
		abort();
	r->data[size] = '\0';
}

static int
pump(struct received *r, int disconnect)
{
	/* Nonblocking polling is bounded even when the expected event never occurs. */
	int i;
	for (i = 0; i < 1000; i++) {
		event_loop(EVLOOP_NONBLOCK);
		if (disconnect ? r->disconnected : r->messages)
			return (1);
		usleep(1000);
	}
	return (0);
}

TEST(peer_credentials_and_auth_descriptor)
{
	struct tmuxproc tp = {0};
	struct received a = {.fd = -1}, b = {.fd = -1};
	struct tmuxpeer *sender, *receiver;
	int pair[2], pipefd[2];
	char byte;
	TAILQ_INIT(&tp.peers);
	ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);
	sender = proc_add_peer(&tp, pair[0], dispatch, &a);
	receiver = proc_add_peer(&tp, pair[1], dispatch, &b);
	ASSERT_EQ(proc_get_peer_uid(receiver), getuid());
	ASSERT_EQ(proc_get_peer_gid(receiver), getgid());
	ASSERT_EQ(pipe(pipefd), 0);
	ASSERT_EQ(write(pipefd[1], "X", 1), 1);
	close(pipefd[1]);
	ASSERT_EQ(proc_send(sender, MSG_IDENTIFY_TMATE_AUTH_PUBKEY,
	    pipefd[0], "auth-key", 8), 0);
	ASSERT(pump(&b, 0));
	ASSERT_EQ(b.messages, 1);
	ASSERT_EQ(b.type, MSG_IDENTIFY_TMATE_AUTH_PUBKEY);
	ASSERT_EQ(b.version, PROTOCOL_VERSION);
	ASSERT_STR_EQ(b.data, "auth-key");
	ASSERT(b.fd >= 0);
	ASSERT_EQ(read(b.fd, &byte, 1), 1);
	ASSERT_EQ(byte, 'X');
	close(b.fd);
	proc_remove_peer(sender);
	proc_remove_peer(receiver);
	ASSERT(TAILQ_EMPTY(&tp.peers));
}

TEST(version_mismatch_rejected_before_dispatch)
{
	struct tmuxproc tp = {0};
	struct received r = {.fd = -1};
	struct tmuxpeer *peer;
	struct imsgbuf remote;
	struct imsg reply;
	int pair[2], i, got = 0;
	TAILQ_INIT(&tp.peers);
	ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);
	peer = proc_add_peer(&tp, pair[0], dispatch, &r);
	ASSERT_EQ(imsgbuf_init(&remote, pair[1]), 0);
	ASSERT_EQ(imsg_compose(&remote, MSG_IDENTIFY_TMATE_AUTH_NONE,
	    PROTOCOL_VERSION + 1, -1, -1, NULL, 0), 1);
	ASSERT_EQ(imsgbuf_flush(&remote), 0);
	ASSERT(pump(&r, 1));
	ASSERT_EQ(r.messages, 0);
	ASSERT_EQ(proc_send(peer, MSG_READY, -1, NULL, 0), -1);
	ASSERT_EQ(imsgbuf_read(&remote), 1);
	for (i = 0; i < 2; i++) {
		got = imsgbuf_get(&remote, &reply);
		if (got == 1) break;
	}
	ASSERT_EQ(got, 1);
	ASSERT_EQ(reply.hdr.type, MSG_VERSION);
	ASSERT_EQ(reply.hdr.peerid, PROTOCOL_VERSION);
	imsg_free(&reply);
	imsgbuf_clear(&remote);
	close(pair[1]);
	proc_remove_peer(peer);
}

TEST(disconnect_reports_null_message)
{
	struct tmuxproc tp = {0};
	struct received r = {.fd = -1};
	struct tmuxpeer *peer;
	int pair[2];
	TAILQ_INIT(&tp.peers);
	ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);
	peer = proc_add_peer(&tp, pair[0], dispatch, &r);
	close(pair[1]);
	ASSERT(pump(&r, 1));
	ASSERT_EQ(r.messages, 0);
	proc_remove_peer(peer);
}

TEST(non_socket_identity_fails_closed)
{
	struct tmuxproc tp = {0};
	struct received r = {.fd = -1};
	struct tmuxpeer *peer;
	int pipefd[2];
	TAILQ_INIT(&tp.peers);
	ASSERT_EQ(pipe(pipefd), 0);
	peer = proc_add_peer(&tp, pipefd[0], dispatch, &r);
	ASSERT_EQ(proc_get_peer_uid(peer), (uid_t)-1);
	ASSERT_EQ(proc_get_peer_gid(peer), (gid_t)-1);
	proc_remove_peer(peer);
	close(pipefd[1]);
}

int main(void)
{
	struct event_base *base = event_init();
	if (base == NULL) return 1;
	TEST_SUITE_BEGIN("Production IPC peers");
	RUN_TEST(peer_credentials_and_auth_descriptor);
	RUN_TEST(version_mismatch_rejected_before_dispatch);
	RUN_TEST(disconnect_reports_null_message);
	RUN_TEST(non_socket_identity_fails_closed);
	event_base_free(base);
	TEST_SUITE_END();
}
