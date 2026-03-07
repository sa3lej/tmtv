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

void tmate_session_init(__unused struct event_base *base)
{
}

void tmate_session_start(void)
{
}
