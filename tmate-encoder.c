#include <sys/utsname.h>
#include <openssl/rand.h>
#include <openssl/crypto.h>
#include "tmate.h"
#include "tmate-protocol.h"
#include "tmate-layout.h"
#include "tmate-snapshot.h"

/*
 * The pack() macro is defined locally in each function scope via the session
 * parameter, eliminating the old file-level macro that hardcoded the global
 * tmate_session.  Every encoder function now takes a struct tmate_session *
 * so the caller controls which session's encoder is used.
 */

#define PACK(session) \
	struct tmate_encoder *__enc __attribute__((unused)) = &(session)->encoder
#define pack(what, ...) _pack(__enc, what, ##__VA_ARGS__)

void tmate_write_header(struct tmate_session *session)
{
	PACK(session);
	pack(array, 3);
	pack(int, TMATE_OUT_HEADER);
	pack(int, TMATE_PROTOCOL_VERSION);
	pack(string, TMTV_VERSION);
}

void tmate_write_session_identity(struct tmate_session *session)
{
	PACK(session);
	if (!session->reconnect_secret[0]) {
		unsigned char secret[32];
		if (RAND_bytes(secret, sizeof(secret)) != 1)
			tmate_fatal("Cannot generate session identity");
		for (unsigned i = 0; i < sizeof(secret); i++)
			snprintf(session->reconnect_secret + 2*i, 3, "%02x", secret[i]);
		OPENSSL_cleanse(secret, sizeof(secret));
	}
	pack(array, 2);
	pack(int, TMATE_OUT_SESSION_IDENTITY);
	pack(string, session->reconnect_secret);
}

void tmate_write_uname(struct tmate_session *session)
{
	PACK(session);
	struct utsname name;
	if (uname(&name) < 0) {
		tmate_debug("uname() failed");
		return;
	}

	pack(array, 6);
	pack(int, TMATE_OUT_UNAME);
	pack(string, name.sysname);
	pack(string, name.nodename);
	pack(string, name.release);
	pack(string, name.version);
	pack(string, name.machine);
}

void tmate_write_ready(struct tmate_session *session)
{
	if (session->layout_failed) return;
	if ((session->layout_required && !session->layout_capability) ||
	    (session->snapshot_required && !session->snapshot_capability)) {
		session->layout_ready_pending = true;
		return;
	}
	PACK(session);
	pack(array, 1);
	pack(int, TMATE_OUT_READY);
}

void tmate_sync_layout(struct tmate_session *session, struct session *s)
{
	PACK(session);
	struct winlink *wl;
	struct window *w;
	struct window_pane *wp;
	int num_panes = 0;
	int num_windows = 0;
	int active_pane_id;
	int active_window_idx = -1;

	if (session->layout_required) {
		msgpack_sbuffer frame;
		if (!session->layout_capability || session->layout_failed)
			return;
		if (s == NULL) s = tmate_find_session(session);
		if (s == NULL || s->curw == NULL) return;
		msgpack_sbuffer_init(&frame);
		if (tmtv_layout_encode(s, &frame) != 0) {
			const char *message = "Cannot share this layout (unsupported state or size limit)";
			msgpack_sbuffer_destroy(&frame);
			tmate_set_env(session, "tmtv_layout_error", message);
			tmate_snapshot_error(session, message);
			return;
		}
		if ((frame.size != session->layout_sent_len ||
		    memcmp(frame.data, session->layout_sent, frame.size) != 0) &&
		    tmate_encoder_write_frame(&session->encoder, frame.data, frame.size) == 0) {
			free(session->layout_sent);
			session->layout_sent = xmalloc(frame.size);
			memcpy(session->layout_sent, frame.data, frame.size);
			session->layout_sent_len = frame.size;
			if (s != NULL && s->curw != NULL && tmtv_recording_active())
				tmtv_recording_resize(s->curw->window->sx, s->curw->window->sy);
		}
		msgpack_sbuffer_destroy(&frame);
		return;
	}

	/*
	 * TODO this can get a little heavy.
	 * We are shipping the full layout whenever a window name changes,
	 * that is, at every shell command.
	 * Might be better to do something incremental.
	 */

	/*
	 * If no tmux session was provided, fall back to the first one.
	 * This preserves backward compatibility with callers that don't
	 * have a session reference handy.
	 */
	if (!s)
		s = RB_MIN(sessions, &sessions);
	if (!s)
		return;

	num_windows = 0;
	RB_FOREACH(wl, winlinks, &s->windows) {
		if (wl->window)
			num_windows++;
	}

	if (!num_windows)
		return;

	pack(array, 5);
	pack(int, TMATE_OUT_SYNC_LAYOUT);

	/* Session no longer has sx/sy in tmux 3.6a; use active window */
	if (s->curw && s->curw->window) {
		pack(int, s->curw->window->sx);
		pack(int, s->curw->window->sy);
		if (tmtv_recording_active())
			tmtv_recording_resize(s->curw->window->sx,
					      s->curw->window->sy);
	} else {
		pack(int, 80);
		pack(int, 24);
	}

	pack(array, num_windows);
	RB_FOREACH(wl, winlinks, &s->windows) {
		w = wl->window;
		if (!w)
			continue;

		w->tmate_last_sync_active_pane = NULL;
		active_pane_id = -1;

		if (active_window_idx == -1)
			active_window_idx = wl->idx;

		/* Track synced name for change detection */
		free(w->tmate_last_sync_name);
		w->tmate_last_sync_name = xstrdup(w->name);
		w->tmate_last_sync_sx = w->sx;
		w->tmate_last_sync_sy = w->sy;

		pack(array, 4);
		pack(int, wl->idx);
		pack(string, w->name);

		num_panes = 0;
		TAILQ_FOREACH(wp, &w->panes, entry)
			num_panes++;

		pack(array, num_panes);
		TAILQ_FOREACH(wp, &w->panes, entry) {
			pack(array, 5);
			pack(int, wp->id);
			pack(int, wp->sx);
			pack(int, wp->sy);
			pack(int, wp->xoff);
			pack(int, wp->yoff);

			if (wp == w->active) {
				w->tmate_last_sync_active_pane = wp;
				active_pane_id = wp->id;
			}

			/* Remember first pane as fallback */
			if (active_pane_id == -1)
				active_pane_id = wp->id;
		}
		pack(int, active_pane_id);
	}

	if (s->curw)
		active_window_idx = s->curw->idx;

	pack(int, active_window_idx);
}

/* TODO add a buffer for pty_data ? */

#define TMATE_MAX_PTY_SIZE (16*1024)

void tmate_pty_data(struct tmate_session *session, struct window_pane *wp,
		    const char *buf, size_t len)
{
	if (session->snapshot_required && (!session->snapshot_capability || !session->layout_capability || session->layout_failed)) return;
	PACK(session);
	size_t to_write;

	while (len > 0) {
		to_write = len < TMATE_MAX_PTY_SIZE ? len : TMATE_MAX_PTY_SIZE;

		pack(array, 3);
		pack(int, TMATE_OUT_PTY_DATA);
		pack(int, wp->id);
		pack(str, to_write);
		pack(str_body, buf, to_write);

		buf += to_write;
		len -= to_write;
	}
}

extern const struct cmd_entry cmd_bind_key_entry;
extern const struct cmd_entry cmd_unbind_key_entry;
extern const struct cmd_entry cmd_set_option_entry;
extern const struct cmd_entry cmd_set_window_option_entry;

static const struct cmd_entry *replicated_cmds[] = {
	&cmd_bind_key_entry,
	&cmd_unbind_key_entry,
	&cmd_set_option_entry,
	&cmd_set_window_option_entry,
	NULL
};

int tmate_should_replicate_cmd(const struct cmd_entry *cmd)
{
	const struct cmd_entry **ptr;

	for (ptr = replicated_cmds; *ptr; ptr++)
		if (*ptr == cmd)
			return 1;
	return 0;
}

#define sc (&session->saved_tmux_cmds)
#define SAVED_TMUX_CMD_INITIAL_SIZE 256
static void __tmate_exec_cmd_args(struct tmate_session *session,
				  int argc, const char **argv);

static void append_saved_cmd(struct tmate_session *session,
			     int argc, const char **argv)
{
	if (!sc->cmds) {
		sc->capacity = SAVED_TMUX_CMD_INITIAL_SIZE;
		sc->cmds = xmalloc(sizeof(*sc->cmds) * sc->capacity);
		sc->tail = 0;
	}

	if (sc->tail == sc->capacity) {
		sc->capacity *= 2;
		sc->cmds = xrealloc(sc->cmds, sizeof(*sc->cmds) * sc->capacity);
	}

	sc->cmds[sc->tail].argc = argc;
	sc->cmds[sc->tail].argv = cmd_copy_argv(argc, (char **)argv);

	sc->tail++;
}

static void replay_saved_cmd(struct tmate_session *session)
{
	unsigned int i;
	for (i = 0; i < sc->tail; i++) {
		const char **argv = (const char **)sc->cmds[i].argv;
		if (session->reconnect_name && sc->cmds[i].argc == 3 &&
		    !strcmp(argv[0], "set-option") && !strcmp(argv[1], "tmtv-set") &&
		    !strncmp(argv[2], "session_name=", 13)) {
			char *value;
			xasprintf(&value, "session_name=%s", session->reconnect_name);
			__tmate_exec_cmd_args(session, 3,
			    (const char *[]){argv[0], argv[1], value});
			free(value);
		} else
			__tmate_exec_cmd_args(session, sc->cmds[i].argc, argv);
	}
}
#undef sc

static void extract_cmd(struct cmd *cmd, int *_argc, char ***_argv)
{
	/*
	 * In tmux 3.6a, struct args is opaque. Use args_print() to
	 * serialize the command, then split it back into argv.
	 */
	char *args_str;
	char *cmdline;
	const struct cmd_entry *entry = cmd_get_entry(cmd);

	args_str = args_print(cmd_get_args(cmd));
	if (args_str != NULL && *args_str != '\0')
		xasprintf(&cmdline, "%s %s", entry->name, args_str);
	else
		cmdline = xstrdup(entry->name);
	free(args_str);

	*_argv = cmd_copy_argv(1, (char *[]){cmdline});
	*_argc = 1;
	free(cmdline);
}

static void __tmate_exec_cmd_args(struct tmate_session *session,
				  int argc, const char **argv)
{
	PACK(session);
	int i;

	pack(array, argc + 1);
	pack(int, TMATE_OUT_EXEC_CMD);

	for (i = 0; i < argc; i++)
		pack(string, argv[i]);
}

void tmate_exec_cmd_args(struct tmate_session *session,
			 int argc, const char **argv)
{
	__tmate_exec_cmd_args(session, argc, argv);
	append_saved_cmd(session, argc, argv);
}

void tmate_set_val(struct tmate_session *session,
		   const char *name, const char *value)
{
	char *buf;
	xasprintf(&buf, "%s=%s", name, value);
	tmate_exec_cmd_args(session, 3,
			    (const char *[]){"set-option", "tmtv-set", buf});
	free(buf);
}

void tmate_exec_cmd(struct tmate_session *session, struct cmd *cmd)
{
	int argc;
	char **argv;

	extract_cmd(cmd, &argc, &argv);
	tmate_exec_cmd_args(session, argc, (const char **)argv);
	cmd_free_argv(argc, argv);
}

void tmate_failed_cmd(struct tmate_session *session,
		      int client_id, const char *cause)
{
	PACK(session);
	pack(array, 3);
	pack(int, TMATE_OUT_FAILED_CMD);
	pack(int, client_id);
	pack(string, cause);
}

void tmate_status(struct tmate_session *session,
		  const char *left, const char *right)
{
	PACK(session);
	static char *old_left, *old_right;

	if (old_left  && !strcmp(old_left,  left) &&
	    old_right && !strcmp(old_right, right))
		return;

	pack(array, 3);
	pack(int, TMATE_OUT_STATUS);
	pack(string, left);
	pack(string, right);

	free(old_left);
	free(old_right);
	old_left = xstrdup(left);
	old_right = xstrdup(right);
}

/*
 * Expand status-left and status-right without requiring an attached client.
 * This is the mechanism that ensures detached sessions (new-session -d)
 * still send OUT_STATUS to the server for web viewers and SSH viewers.
 */
void tmate_expand_status(void)
{
	struct session		*s;
	struct format_tree	*ft;
	char			*left, *right;

	s = RB_MIN(sessions, &sessions);
	if (s == NULL)
		return;

	ft = format_create(NULL, NULL, FORMAT_NONE, FORMAT_STATUS);
	format_defaults(ft, NULL, s, NULL, NULL);

	left = format_expand_time(ft, options_get_string(s->options,
	    "status-left"));
	right = format_expand_time(ft, options_get_string(s->options,
	    "status-right"));

	tmate_status(&tmate_session, left, right);

	free(right);
	free(left);
	format_free(ft);
}

#define TMATE_STATUS_INTERVAL 2  /* seconds */

static void on_status_timer(__unused evutil_socket_t fd, __unused short what,
			    void *arg)
{
	struct tmate_session *session = arg;
	struct timeval tv = { .tv_sec = TMATE_STATUS_INTERVAL, .tv_usec = 0 };

	tmate_expand_status();

	/* Re-arm the timer */
	evtimer_add(session->ev_status_timer, &tv);
}

void tmate_start_status_timer(struct tmate_session *session)
{
	struct timeval tv = { .tv_sec = TMATE_STATUS_INTERVAL, .tv_usec = 0 };

	if (session->ev_status_timer != NULL)
		return;

	session->ev_status_timer = evtimer_new(session->ev_base,
	    on_status_timer, session);
	if (session->ev_status_timer == NULL)
		tmate_fatal("out of memory");
	evtimer_add(session->ev_status_timer, &tv);
}

void tmate_sync_copy_mode(struct tmate_session *session,
			  struct window_pane *wp)
{
	if (session->snapshot_required && (!session->snapshot_capability || !session->layout_capability || session->layout_failed)) return;
	PACK(session);
	struct window_mode_entry *wme;
	struct window_copy_state state;

	/* An empty state means copy/view is inactive; active state follows. */
	wme = TAILQ_FIRST(&wp->modes);
	if (wme != NULL && window_copy_get_state(wp, &state)) {
		pack(array, 3);
		pack(int, TMATE_OUT_SYNC_COPY_MODE);
		pack(int, wp->id);
		pack(array, 6);
		pack(int, state.view_mode == 0);
		pack(int, state.oy);
		pack(int, state.cx);
		pack(int, state.cy);
		if (state.selection) {
			pack(array, 3);
			pack(int, state.selx);
			pack(int64, state.transformed_sely);
			pack(int, state.rectangle);
		} else
			pack(array, 0);
		pack(array, 0);
		return;
	}

	pack(array, 3);
	pack(int, TMATE_OUT_SYNC_COPY_MODE);
	pack(int, wp->id);
	pack(array, 0);
}

void tmate_write_copy_mode(struct tmate_session *session,
			   struct window_pane *wp, const char *str)
{
	if (session->snapshot_required && (!session->snapshot_capability || !session->layout_capability || session->layout_failed)) return;
	PACK(session);
	pack(array, 3);
	pack(int, TMATE_OUT_WRITE_COPY_MODE);
	pack(int, wp->id);
	pack(string, str);
}

/*
 * Broadcast clipboard data to the server for relay to RW viewers.
 * Called from paste_add() when tmtv-shared-clipboard is on.
 * Capped at 100KB to avoid flooding the SSH channel.
 */
#define TMATE_CLIPBOARD_MAX (100 * 1024)

void tmate_clipboard_broadcast(struct tmate_session *session,
			       const char *data, size_t len)
{
	PACK(session);

	if (len == 0 || len > TMATE_CLIPBOARD_MAX)
		return;

	pack(array, 2);
	pack(int, TMATE_OUT_CLIPBOARD);
	pack(str, len);
	pack(str_body, data, len);
}

void tmate_write_fin(struct tmate_session *session)
{
	if (session->fin_sent)
		return;
	session->fin_sent = true;

	PACK(session);
	pack(array, 1);
	pack(int, TMATE_OUT_FIN);
}

static void do_snapshot_grid(struct tmate_encoder *__enc,
			     struct grid *grid,
			     unsigned int max_history_lines)
{
	struct grid_line *line;
	struct grid_cell gc;
	unsigned int line_i, i;
	unsigned int max_lines;
	size_t str_len;

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
		str_len = 0;
		for (i = 0; i < line->cellsize; i++) {
			grid_get_cell(grid, i, line_i, &gc);
			str_len += gc.data.size;
		}

		pack(str, str_len);
		for (i = 0; i < line->cellsize; i++) {
			grid_get_cell(grid, i, line_i, &gc);
			pack(str_body, gc.data.data, gc.data.size);
		}

		pack(array, line->cellsize);
		for (i = 0; i < line->cellsize; i++) {
			grid_get_cell(grid, i, line_i, &gc);
			/*
			 * Pack as single int matching server decoder format:
			 * (flags << 24) | (attr << 16) | (bg << 8) | fg
			 * Each field is one byte. Modern attributes cannot be
			 * represented here and must not spill into the flags byte.
			 * Shift unsigned values so GRID_FLAG_TAB is well defined.
			 * Truncates fg/bg to 8 bits (palette colors).
			 */
			pack(unsigned_int, (((uint32_t)gc.flags << 24) |
					    ((uint32_t)(gc.attr & 0xFF) << 16) |
					    ((uint32_t)(gc.bg & 0xFF) << 8) |
					    (uint32_t)(gc.fg & 0xFF)));
		}
	}

}

static void do_snapshot_pane(struct tmate_encoder *__enc,
			     struct window_pane *wp,
			     unsigned int max_history_lines, int copy_backing)
{
	struct screen *screen = &wp->base;
	struct window_mode_entry *wme = TAILQ_FIRST(&wp->modes);

	if (copy_backing && wme != NULL && wme->mode == &window_copy_mode)
		screen = wme->mode->get_screen(wme);

	pack(array, 4);
	pack(int, wp->id);

	pack(unsigned_int, screen->mode);

	pack(array, 3);
	pack(int, screen->cx);
	pack(int, screen->cy);
	do_snapshot_grid(__enc, screen->grid, max_history_lines);

	if (wp->base.saved_grid) {
		pack(array, 3);
		pack(int, wp->base.saved_cx);
		pack(int, wp->base.saved_cy);
		do_snapshot_grid(__enc, wp->base.saved_grid, max_history_lines);
	} else {
		pack(nil);
	}
}

static void tmate_send_session_snapshot(struct tmate_session *session,
					struct session *s,
					unsigned int max_history_lines, int copy_backing)
{
	PACK(session);
	struct winlink *wl;
	struct window *w;
	struct window_pane *pane;
	int num_panes;

	/*
	 * If no tmux session was provided, fall back to the first one.
	 */
	if (!s)
		s = RB_MIN(sessions, &sessions);
	if (!s) {
		tmate_debug("snapshot requested with no sessions, skipping");
		return;
	}

	/* Resolve the session before starting a message: never leave a partial array. */
	pack(array, 2);
	pack(int, TMATE_OUT_SNAPSHOT);

	num_panes = 0;
	RB_FOREACH(wl, winlinks, &s->windows) {
		if (session->layout_required && !tmtv_layout_first_link(s, wl))
			continue;
		w = wl->window;
		if (!w)
			continue;

		TAILQ_FOREACH(pane, &w->panes, entry)
			num_panes++;
	}

	pack(array, num_panes);
	RB_FOREACH(wl, winlinks, &s->windows) {
		if (session->layout_required && !tmtv_layout_first_link(s, wl))
			continue;
		w = wl->window;
		if (!w)
			continue;

		TAILQ_FOREACH(pane, &w->panes, entry)
			do_snapshot_pane(__enc, pane, max_history_lines, copy_backing);
	}
}

static void tmate_send_reconnection_data(struct tmate_session *session)
{
	PACK(session);
	if (!session->reconnection_data)
		return;

	pack(array, 2);
	pack(int, TMATE_OUT_RECONNECT);
	pack(string, session->reconnection_data);
}

/*
 * History lines to include in reconnection snapshot.  Keep this modest
 * to avoid flooding the SSH channel immediately on reconnect — the
 * visible screen content is the priority, scrollback is secondary.
 * 100 lines is enough to reconstruct the visible state of most
 * terminal layouts without causing a cascade under backpressure.
 */
#define RECONNECTION_MAX_HISTORY_LINE 100

/* The legacy view operation is literal text. Replay its bounded backing before
 * restoring the cursor/selection; base snapshots alone contain shell output,
 * not the independent view buffer. Rich parsed styles require negotiation. */
static void
tmate_send_view_snapshot(struct tmate_session *session, struct window_pane *wp)
{
	struct window_mode_entry *wme = TAILQ_FIRST(&wp->modes);
	struct screen *backing;
	u_int y, start;
	char *line;

	if (wme == NULL || wme->mode != &window_view_mode)
		return;
	backing = wme->mode->get_screen(wme);
	start = screen_hsize(backing) > RECONNECTION_MAX_HISTORY_LINE ?
	    screen_hsize(backing) - RECONNECTION_MAX_HISTORY_LINE : 0;
	for (y = start; y <= screen_hsize(backing) + backing->cy; y++) {
		line = grid_string_cells(backing->grid, 0, y,
		    screen_size_x(backing), NULL, 0, backing);
		tmate_write_copy_mode(session, wp, line);
		free(line);
	}
}

void tmate_send_reconnection_state(struct tmate_session *session)
{
	/* Start with a fresh encoder */
	tmate_encoder_destroy(&session->encoder);
	tmate_encoder_init(&session->encoder, NULL, session);

	tmate_write_header(session);
	tmate_send_reconnection_data(session);
	replay_saved_cmd(session);
	/* TODO send all option variables */
	tmate_write_uname(session);
	tmate_write_ready(session);

	if (!session->layout_required || session->layout_capability)
		tmate_send_reconnection_snapshot(session);
}

void tmate_send_reconnection_snapshot(struct tmate_session *session)
{
	struct session *s;
	struct winlink *wl;
	struct window *w;
	struct window_pane *wp;
	struct window_mode_entry *wme;
	int copy_backing = 0;
	if (session->snapshot_required) {
		msgpack_sbuffer frame;
		if (!session->snapshot_capability || !session->layout_capability || session->layout_failed) return;
		tmate_sync_layout(session, NULL);
		if (session->layout_failed) return;
		msgpack_sbuffer_init(&frame);
		if (tmate_snapshot_encode(tmate_find_session(session), &frame) != 0 ||
		    tmate_encoder_write_frame(&session->encoder, frame.data, frame.size) != 0)
			tmate_snapshot_error(session, "Cannot share this bounded snapshot (unsupported state or size limit)");
		msgpack_sbuffer_destroy(&frame);
		tmate_expand_status();
		return;
	}


	tmate_sync_layout(session, NULL);
	s = tmate_find_session(session);
	if (s != NULL)
		RB_FOREACH(wl, winlinks, &s->windows) {
			if (session->layout_required && !tmtv_layout_first_link(s, wl))
				continue;
			TAILQ_FOREACH(wp, &wl->window->panes, entry) {
				wme = TAILQ_FIRST(&wp->modes);
				if (wme != NULL && wme->mode == &window_copy_mode)
					copy_backing = 1;
			}
		}
	/* Clone the frozen copy grid first using existing snapshot/state
	 * messages, then restore the independent live base below. */
	tmate_send_session_snapshot(session, NULL,
				    RECONNECTION_MAX_HISTORY_LINE, copy_backing);

	s = tmate_find_session(session);
	if (s != NULL)
		RB_FOREACH(wl, winlinks, &s->windows) {
			if (session->layout_required && !tmtv_layout_first_link(s, wl))
				continue;
			w = wl->window;
			TAILQ_FOREACH(wp, &w->panes, entry) {
				tmate_send_view_snapshot(session, wp);
				tmate_sync_copy_mode(session, wp);
			}
		}
	if (copy_backing)
		tmate_send_session_snapshot(session, NULL,
		    RECONNECTION_MAX_HISTORY_LINE, 0);

	/* Send current status so late-joining viewers get it immediately */
	tmate_expand_status();
}

/* Complete, private session snapshot. A single frame is either queued or absent. */
static int snapshot_write(void *data, const char *bytes, size_t len)
{
	msgpack_sbuffer *sb = data;
	if (len > TMTV_SNAPSHOT_MAX_BYTES - sb->size) return (-1);
	return (msgpack_sbuffer_write(sb, bytes, len));
}
int
tmate_snapshot_encode(struct session *s, msgpack_sbuffer *out)
{
	msgpack_sbuffer sb;
	msgpack_packer pk;
	struct winlink *wl;
	struct window_pane *wp;
	struct window_mode_entry *wme;
	struct window_copy_state cs;
	struct screen *backing, capture;
	msgpack_unpacked un;
	msgpack_object *entries, *entry, *modes, *mode;
	u_int count = 0, nm, kind, history, i, j, cost, total = 0, links = 0, images = 0, pixels = 0;
	int ok = 0;
	struct input_snapshot *parser;
	if (s == NULL) return (-1);
	RB_FOREACH(wl, winlinks, &s->windows) if (tmtv_layout_first_link(s, wl))
		TAILQ_FOREACH(wp, &wl->window->panes, entry) count++;
	if (!count || count > TMTV_LAYOUT_MAX_PANES) return (-1);
	msgpack_sbuffer_init(&sb); msgpack_packer_init(&pk, &sb, snapshot_write);
#define SPACK(type, ...) do { if (msgpack_pack_##type(&pk, ##__VA_ARGS__)) goto done; } while (0)
	SPACK(array, 3); SPACK(int, TMTV_SNAPSHOT_OPCODE); SPACK(int, TMTV_SNAPSHOT_VERSION); SPACK(array, count);
	RB_FOREACH(wl, winlinks, &s->windows) if (tmtv_layout_first_link(s, wl)) {
		TAILQ_FOREACH(wp, &wl->window->panes, entry) {
			SPACK(array, 3);
			if (tmtv_snapshot_pane_encode(wp->id, &wp->base, TMTV_SNAPSHOT_HISTORY, &sb) ||
			    input_snapshot_encode(wp->ictx, &sb)) goto done;
			nm = 0; TAILQ_FOREACH(wme, &wp->modes, entry) nm++;
			if (nm > 3) goto done;
			SPACK(array, nm);
			TAILQ_FOREACH(wme, &wp->modes, entry) {
				kind = wme->mode == &window_copy_mode ? 1 : wme->mode == &window_view_mode ? 2 :
				    wme->mode == &window_clock_mode ? 3 : 0;
				if (!kind) goto done;
				SPACK(array, 3); SPACK(int, kind);
				if (kind == 3) { SPACK(nil); SPACK(nil); continue; }
				if (!window_copy_get_mode_state(wme, &cs)) goto done;
				backing = wme->mode->get_screen(wme);
				history = TMTV_SNAPSHOT_HISTORY;
				if (cs.oy > history) history = cs.oy;
				if (cs.selection && cs.transformed_sely >= (int64_t)backing->grid->sy &&
				    cs.transformed_sely - backing->grid->sy + 1 > history)
					history = cs.transformed_sely - backing->grid->sy + 1;
				capture = *backing;
				capture.rupper = 0; capture.rlower = capture.grid->sy - 1;
				if (history > TMTV_SNAPSHOT_MAX_HISTORY ||
				    tmtv_snapshot_pane_encode(wp->id, &capture, history, &sb)) goto done;
				SPACK(array, 7); SPACK(unsigned_int, cs.oy); SPACK(unsigned_int, cs.cx);
				SPACK(unsigned_int, cs.cy); SPACK(int, cs.selection); SPACK(unsigned_int, cs.selx);
				SPACK(int64, cs.transformed_sely); SPACK(int, cs.rectangle);
			}
		}
	}
	msgpack_unpacked_init(&un);
	if (msgpack_unpack_next(&un, sb.data, sb.size, NULL) == MSGPACK_UNPACK_SUCCESS &&
	    tmtv_snapshot_object_bounded(un.data)) {
		entries = un.data.via.array.ptr[2].via.array.ptr;
		for (i = 0; i < count; i++) {
			entry = entries[i].via.array.ptr;
			if (!tmtv_snapshot_pane_validate(entry[0], &cost) ||
			    !tmtv_snapshot_media_budget(entry[0], &links, &images, &pixels)) break;
			parser = input_snapshot_decode(entry[1]);
			if (parser == NULL) break;
			int valid_links = input_snapshot_links_validate(parser, entry[0].via.array.ptr[13]);
			input_snapshot_free(parser);
			if (!valid_links) break;
			total += cost;
			modes = entry[2].via.array.ptr;
			for (j = 0; j < entry[2].via.array.size; j++) {
				mode = modes[j].via.array.ptr;
				if (mode[1].type == MSGPACK_OBJECT_NIL) continue;
				if (!tmtv_snapshot_pane_validate(mode[1], &cost) ||
				    !tmtv_snapshot_media_budget(mode[1], &links, &images, &pixels)) break;
				total += cost;
			}
			if (j != entry[2].via.array.size || total > TMTV_SNAPSHOT_MAX_CELLS) break;
		}
		if (i == count && out->size <= TMTV_SNAPSHOT_MAX_BYTES - sb.size &&
		    msgpack_sbuffer_write(out, sb.data, sb.size) == 0) ok = 1;
	}
	msgpack_unpacked_destroy(&un);
done:
	msgpack_sbuffer_destroy(&sb); return (ok ? 0 : -1);
#undef SPACK
}
