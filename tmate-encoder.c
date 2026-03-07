#include <sys/utsname.h>
#include "tmate.h"
#include "tmate-protocol.h"

#define pack(what, ...) _pack(&tmate_session.encoder, what, ##__VA_ARGS__)

void tmate_write_header(void)
{
	pack(array, 3);
	pack(int, TMATE_OUT_HEADER);
	pack(int, TMATE_PROTOCOL_VERSION);
	pack(string, VERSION);
}

void tmate_write_uname(void)
{
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

void tmate_write_ready(void)
{
	pack(array, 1);
	pack(int, TMATE_OUT_READY);
}

void tmate_sync_layout(void)
{
	struct session *s;
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
	 * We only allow one session, it makes our lives easier.
	 * Especially when the HTML5 client will come along.
	 * We make no distinction between a winlink and its window except
	 * that we send the winlink idx to draw the status bar properly.
	 */

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

		}
		pack(int, active_pane_id);
	}

	if (s->curw)
		active_window_idx = s->curw->idx;

	pack(int, active_window_idx);
}

/* TODO add a buffer for pty_data ? */

#define TMATE_MAX_PTY_SIZE (16*1024)

void tmate_pty_data(struct window_pane *wp, const char *buf, size_t len)
{
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
static void __tmate_exec_cmd_args(int argc, const char **argv);

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
		__tmate_exec_cmd_args(sc->cmds[i].argc, (const char **)sc->cmds[i].argv);
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

static void __tmate_exec_cmd_args(int argc, const char **argv)
{
	int i;

	pack(array, argc + 1);
	pack(int, TMATE_OUT_EXEC_CMD);

	for (i = 0; i < argc; i++)
		pack(string, argv[i]);
}

void tmate_exec_cmd_args(int argc, const char **argv)
{
	__tmate_exec_cmd_args(argc, argv);
	append_saved_cmd(&tmate_session, argc, argv);
}

void tmate_set_val(const char *name, const char *value)
{
	char *buf;
	xasprintf(&buf, "%s=%s", name, value);
	tmate_exec_cmd_args(3, (const char *[]){"set-option", "tmate-set", buf});
	free(buf);
}

void tmate_exec_cmd(struct cmd *cmd)
{
	int argc;
	char **argv;

	extract_cmd(cmd, &argc, &argv);
	tmate_exec_cmd_args(argc, (const char **)argv);
	cmd_free_argv(argc, argv);
}

void tmate_failed_cmd(int client_id, const char *cause)
{
	pack(array, 3);
	pack(int, TMATE_OUT_FAILED_CMD);
	pack(int, client_id);
	pack(string, cause);
}

void tmate_status(const char *left, const char *right)
{
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

void tmate_sync_copy_mode(struct window_pane *wp)
{
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

void tmate_write_copy_mode(struct window_pane *wp, const char *str)
{
	pack(array, 3);
	pack(int, TMATE_OUT_WRITE_COPY_MODE);
	pack(int, wp->id);
	pack(string, str);
}

void tmate_write_fin(void)
{
	pack(array, 1);
	pack(int, TMATE_OUT_FIN);
}

static void do_snapshot_grid(struct grid *grid, unsigned int max_history_lines)
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
			 * In tmux 3.6a, gc.fg/gc.bg are int (not u_char).
			 * Pack as array of [fg, bg, attr, flags] for
			 * compatibility with wider color values.
			 */
			pack(array, 4);
			pack(int, gc.fg);
			pack(int, gc.bg);
			pack(unsigned_int, gc.attr);
			pack(unsigned_int, gc.flags);
		}
	}

}

static void do_snapshot_pane(struct window_pane *wp, unsigned int max_history_lines)
{
	struct screen *screen = &wp->base;

	pack(array, 4);
	pack(int, wp->id);

	pack(unsigned_int, screen->mode);

	pack(array, 3);
	pack(int, screen->cx);
	pack(int, screen->cy);
	do_snapshot_grid(screen->grid, max_history_lines);

	if (wp->base.saved_grid) {
		pack(array, 3);
		pack(int, wp->base.saved_cx);
		pack(int, wp->base.saved_cy);
		do_snapshot_grid(wp->base.saved_grid, max_history_lines);
	} else {
		pack(nil);
	}
}

static void tmate_send_session_snapshot(unsigned int max_history_lines)
{
	struct session *s;
	struct winlink *wl;
	struct window *w;
	struct window_pane *pane;
	int num_panes;

	pack(array, 2);
	pack(int, TMATE_OUT_SNAPSHOT);

	s = RB_MIN(sessions, &sessions);
	if (!s)
		tmate_fatal("no session?");

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
			do_snapshot_pane(pane, max_history_lines);
	}
}

static void tmate_send_reconnection_data(struct tmate_session *session)
{
	if (!session->reconnection_data)
		return;

	pack(array, 2);
	pack(int, TMATE_OUT_RECONNECT);
	pack(string, session->reconnection_data);
}

#define RECONNECTION_MAX_HISTORY_LINE 300

void tmate_send_reconnection_state(struct tmate_session *session)
{
	/* Start with a fresh encoder */
	tmate_encoder_destroy(&session->encoder);
	tmate_encoder_init(&session->encoder, NULL, session);

	tmate_write_header();
	tmate_send_reconnection_data(session);
	replay_saved_cmd(session);
	/* TODO send all option variables */
	tmate_write_uname();
	tmate_write_ready();

	tmate_sync_layout();
	tmate_send_session_snapshot(RECONNECTION_MAX_HISTORY_LINE);
}
