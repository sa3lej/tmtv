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

	/* Virtual PTY client fields */
	s.vpty_master_fd = -1;
	s.vpty_child_pid = -1;
	s.ev_vpty_read = NULL;
	s.vpty_active = false;
	s.jail_sock_name = NULL;

	ASSERT(s.ssh_client.role == TMATE_ROLE_DAEMON);
	ASSERT(s.client_protocol_version == TMATE_PROTOCOL_VERSION);
	ASSERT(s.vpty_master_fd == -1);
	ASSERT(s.vpty_active == false);
	ASSERT(s.jail_sock_name == NULL);
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
	ASSERT(TMATE_TOKEN_LEN == 8);
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

/* Verify input socket struct fields exist */
TEST(input_socket_fields)
{
	struct tmate_session s;
	memset(&s, 0, sizeof(s));

	s.input_mode_enabled = true;
	s.input_mirror = true;
	s.next_viewer_id = 42;

	ASSERT(s.input_mode_enabled == true);
	ASSERT(s.input_mirror == true);
	ASSERT(s.next_viewer_id == 42);

	/* Default zero-init should give disabled, mirror=false, id=0 */
	memset(&s, 0, sizeof(s));
	ASSERT(s.input_mode_enabled == false);
	ASSERT(s.input_mirror == false);
	ASSERT(s.next_viewer_id == 0);
}

/* Verify ws_client has viewer_id field */
TEST(ws_client_viewer_id)
{
	struct ws_client wc;
	memset(&wc, 0, sizeof(wc));

	wc.viewer_id = 7;
	ASSERT(wc.viewer_id == 7);

	memset(&wc, 0, sizeof(wc));
	ASSERT(wc.viewer_id == 0);
}

/* Verify input socket protocol constants */
TEST(input_socket_protocol_constants)
{
	/* OUT messages (host -> server) must be at the end of the enum */
	ASSERT(TMATE_OUT_INPUT_MODE > TMATE_OUT_SESSION_MODE);

	/* IN messages (server -> host) must be at the end of the enum */
	ASSERT(TMATE_IN_USER_JOIN > TMATE_IN_EXEC_CMD);
	ASSERT(TMATE_IN_USER_LEAVE > TMATE_IN_USER_JOIN);
	ASSERT(TMATE_IN_USER_INPUT > TMATE_IN_USER_LEAVE);

	/* Existing constants must be unchanged (protocol compatibility) */
	ASSERT(TMATE_OUT_HEADER == 0);
	ASSERT(TMATE_OUT_VIEWER_COUNT == 14);
	ASSERT(TMATE_OUT_SESSION_MODE == 15);
	ASSERT(TMATE_OUT_INPUT_MODE == 16);

	ASSERT(TMATE_IN_NOTIFY == 0);
	ASSERT(TMATE_IN_EXEC_CMD == 7);
	ASSERT(TMATE_IN_USER_JOIN == 8);
	ASSERT(TMATE_IN_USER_LEAVE == 9);
	ASSERT(TMATE_IN_USER_INPUT == 10);
}

int main(void)
{
	TEST_SUITE_BEGIN("Server header compatibility (tmux 3.6a)");

	RUN_TEST(session_struct_fields);
	RUN_TEST(settings_struct);
	RUN_TEST(ssh_client_struct);
	RUN_TEST(protocol_constants);
	RUN_TEST(encoder_struct);
	RUN_TEST(input_socket_fields);
	RUN_TEST(ws_client_viewer_id);
	RUN_TEST(input_socket_protocol_constants);

	TEST_SUITE_END();
}
