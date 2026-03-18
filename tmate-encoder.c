#include <sys/utsname.h>
#include "tmate.h"
#include "tmate-protocol.h"

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
	for (i = 0; i < sc->tail; i++)
		__tmate_exec_cmd_args(session, sc->cmds[i].argc,
				      (const char **)sc->cmds[i].argv);
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
	PACK(session);
	struct window_mode_entry *wme;

	pack(array, 3);
	pack(int, TMATE_OUT_SYNC_COPY_MODE);
	pack(int, wp->id);

	/*
	 * In tmux 3.6a, mode data is accessed via the modes list.
	 * The internal window_copy_mode_data struct is private to
	 * window-copy.c, so we can only send a minimal sync.
	 */
	wme = TAILQ_FIRST(&wp->modes);
	if (wme == NULL || wme->mode != &window_copy_mode) {
		pack(array, 0);
		return;
	}

	/* Send minimal copy mode info - full sync would require
	 * exposing window-copy.c internals via accessor functions */
	pack(array, 0);
}

void tmate_write_copy_mode(struct tmate_session *session,
			   struct window_pane *wp, const char *str)
{
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
			 * Truncates fg/bg to 8 bits (palette colors).
			 */
			pack(unsigned_int, ((gc.flags << 24) |
					    (gc.attr  << 16) |
					    ((gc.bg & 0xFF) << 8) |
					    (gc.fg & 0xFF)));
		}
	}

}

static void do_snapshot_pane(struct tmate_encoder *__enc,
			     struct window_pane *wp,
			     unsigned int max_history_lines)
{
	struct screen *screen = &wp->base;

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
					unsigned int max_history_lines)
{
	PACK(session);
	struct winlink *wl;
	struct window *w;
	struct window_pane *pane;
	int num_panes;

	pack(array, 2);
	pack(int, TMATE_OUT_SNAPSHOT);

	/*
	 * If no tmux session was provided, fall back to the first one.
	 */
	if (!s)
		s = RB_MIN(sessions, &sessions);
	if (!s) {
		tmate_debug("snapshot requested with no sessions, skipping");
		return;
	}

	num_panes = 0;
	RB_FOREACH(wl, winlinks, &s->windows) {
		w = wl->window;
		if (!w)
			continue;

		TAILQ_FOREACH(pane, &w->panes, entry)
			num_panes++;
	}

	pack(array, num_panes);
	RB_FOREACH(wl, winlinks, &s->windows) {
		w = wl->window;
		if (!w)
			continue;

		TAILQ_FOREACH(pane, &w->panes, entry)
			do_snapshot_pane(__enc, pane, max_history_lines);
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

	tmate_sync_layout(session, NULL);
	tmate_send_session_snapshot(session, NULL,
				    RECONNECTION_MAX_HISTORY_LINE);

	/* Send current status so late-joining viewers get it immediately */
	tmate_expand_status();
}
