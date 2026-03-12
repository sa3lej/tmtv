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
 *
 * Stub implementations of client-side tmate functions.
 *
 * The tmux core files (session.c, server-client.c, window.c, cfg.c, server.c)
 * have #ifdef TMATE hooks that call client-side tmate functions. In the server
 * binary, these operations are handled differently through the daemon
 * encoder/decoder, so these stubs provide no-op implementations.
 */

#include "tmate.h"

int tmate_foreground;

void tmate_sync_layout(__unused struct tmate_session *session,
		       __unused struct session *s)
{
}

void tmate_pty_data(__unused struct tmate_session *session,
		    __unused struct window_pane *wp,
		    __unused const char *buf, __unused size_t len)
{
}

void tmate_write_fin(__unused struct tmate_session *session)
{
}

void tmate_status(__unused struct tmate_session *session,
		  __unused const char *left, __unused const char *right)
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

/* Recording is client-only; server never records. */

void tmtv_recording_start(__unused const char *token,
			  __unused u_int width, __unused u_int height)
{
}

void tmtv_recording_write(__unused const char *buf, __unused size_t len)
{
}

void tmtv_recording_resize(__unused u_int width, __unused u_int height)
{
}

void tmtv_recording_stop(void)
{
}

int tmtv_recording_active(void)
{
	return (0);
}
