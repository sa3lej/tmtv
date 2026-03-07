/*
 * Stub implementations of client-side tmate functions.
 *
 * The tmux core files (session.c, server-client.c, window.c, cfg.c, server.c)
 * have #ifdef TMATE hooks that call client-side tmate functions. In the server
 * binary, these operations are handled differently through the daemon
 * encoder/decoder, so these stubs provide no-op implementations.
 */

#include "tmate.h"

int tmate_foreground;

void tmate_sync_layout(void)
{
}

void tmate_pty_data(__unused struct window_pane *wp,
		    __unused const char *buf, __unused size_t len)
{
}

void tmate_write_fin(void)
{
}

void tmate_session_init(struct event_base *base)
{
	/*
	 * Re-register SSH/websocket events on the (possibly reinited) base.
	 * tmate_daemon_init() was already called in tmate_spawn_daemon(),
	 * but server_start() may have called event_reinit() since then.
	 * The encoder's ev_buffer needs to be re-added to the new base.
	 */
	tmate_session->ev_base = base;
}

void tmate_session_start(void)
{
}
