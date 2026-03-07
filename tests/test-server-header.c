/*
 * Test that the server tmate.h compiles against tmux 3.6a and that
 * key structs/functions are correctly defined.
 *
 * This is a compile-and-run test: if it compiles, the header is
 * compatible with tmux 3.6a. The runtime checks verify struct
 * layout and symbol availability.
 */

#include "test-harness.h"

/* Must include tmux.h before server tmate.h, same as real code */
#include "../tmux.h"
#include "../server/tmate.h"

/* Verify tmate_session struct has expected fields */
TEST(session_struct_fields)
{
	struct tmate_session s;
	memset(&s, 0, sizeof(s));

	/* Core fields */
	s.ev_base = NULL;
	s.tmux_socket_fd = -1;

	/* Daemon encoder/decoder */
	s.daemon_encoder.pk.data = NULL;
	s.daemon_decoder.reader = NULL;

	/* Websocket */
	s.websocket_fd = -1;
	s.websocket_sx = 80;
	s.websocket_sy = 24;

	/* SSH client */
	s.ssh_client.role = TMATE_ROLE_DAEMON;

	/* Session tokens */
	s.session_token = "test-token";
	s.session_token_ro = "test-token-ro";

	/* Protocol state */
	s.client_protocol_version = TMATE_PROTOCOL_VERSION;
	s.fin_received = false;

	ASSERT(s.websocket_fd == -1);
	ASSERT(s.ssh_client.role == TMATE_ROLE_DAEMON);
	ASSERT(s.client_protocol_version == TMATE_PROTOCOL_VERSION);
}

/* Verify tmate_settings struct */
TEST(settings_struct)
{
	struct tmate_settings ts;
	memset(&ts, 0, sizeof(ts));

	ts.keys_dir = "/tmp/keys";
	ts.ssh_port = 2222;
	ts.websocket_port = 4002;
	ts.tmate_host = "localhost";
	ts.log_level = 0;
	ts.use_proxy_protocol = false;
	ts.authorized_keys_only = false;

	ASSERT(ts.ssh_port == 2222);
	ASSERT(ts.websocket_port == 4002);
}

/* Verify SSH client struct */
TEST(ssh_client_struct)
{
	struct tmate_ssh_client sc;
	memset(&sc, 0, sizeof(sc));

	sc.role = TMATE_ROLE_PTY_CLIENT;
	sc.session = NULL;
	sc.channel = NULL;
	sc.username = NULL;
	sc.pubkey = NULL;

	ASSERT(sc.role == TMATE_ROLE_PTY_CLIENT);
	ASSERT(TMATE_ROLE_DAEMON == 1);
	ASSERT(TMATE_ROLE_PTY_CLIENT == 2);
	ASSERT(TMATE_ROLE_EXEC == 3);
}

/* Verify protocol constants */
TEST(protocol_constants)
{
	ASSERT(TMATE_PROTOCOL_VERSION >= 6);
	ASSERT(TMATE_TOKEN_LEN == 25);
}

/* Verify encoder struct has ev_buffer as pointer (tmux 3.6a libevent2 pattern) */
TEST(encoder_struct)
{
	struct tmate_encoder enc;
	memset(&enc, 0, sizeof(enc));

	enc.ev_buffer = NULL;  /* Must be a pointer, not embedded struct */
	enc.ready_callback = NULL;
	enc.userdata = NULL;
	enc.buffer = NULL;
	enc.ev_active = false;

	ASSERT(enc.ev_buffer == NULL);
	ASSERT(enc.ev_active == false);
}

int main(void)
{
	TEST_SUITE_BEGIN("Server header compatibility (tmux 3.6a)");

	RUN_TEST(session_struct_fields);
	RUN_TEST(settings_struct);
	RUN_TEST(ssh_client_struct);
	RUN_TEST(protocol_constants);
	RUN_TEST(encoder_struct);

	TEST_SUITE_END();
}
