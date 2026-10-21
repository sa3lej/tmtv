/* Bounded grid, screen and parser state for negotiated reconnect snapshots. */
#ifndef TMTV_SNAPSHOT_H
#define TMTV_SNAPSHOT_H
#include <msgpack.h>
#define TMTV_SNAPSHOT_GRID_VERSION 1
#define TMTV_SNAPSHOT_MAX_UTF8 31 /* utf8_char has a five-bit byte length */
#define TMTV_SNAPSHOT_HISTORY 100
#define TMTV_SNAPSHOT_MAX_HISTORY 2000
#define TMTV_SNAPSHOT_MAX_CELLS 262144
#define TMTV_SNAPSHOT_MAX_BYTES (16 * 1024 * 1024)
/* On failure encode leaves the destination untouched; decode owns a new grid.
 * Hyperlink handles and images require separate session/screen lifetime state.
 */
int tmtv_snapshot_grid_encode(struct grid *, u_int, msgpack_sbuffer *);
struct grid *tmtv_snapshot_grid_decode(msgpack_object, u_int);
/* Shared screen-envelope validation helpers; no pane mutation. */
int tmtv_snapshot_grid_validate(msgpack_object, u_int, u_int *);
int tmtv_snapshot_colour(u_int);
int tmtv_snapshot_cell_encode(const struct grid_cell *, msgpack_packer *);
int tmtv_snapshot_cell_decode(msgpack_object, struct grid_cell *);

#define TMTV_SNAPSHOT_PANE_VERSION 2
struct tmtv_snapshot_pane {
	u_int id, cx, cy, saved_cx, saved_cy, rupper, rlower;
	int mode, default_mode, saved_flags, ccolour, default_ccolour;
	enum screen_cursor_style cstyle, default_cstyle;
	struct grid *grid, *saved_grid;
	struct grid_cell saved_cell;
	bitstr_t *tabs;
	char *title, *path, *titles[10];
	u_int ntitles;
	struct progress_bar progress_bar;
};
int tmtv_snapshot_pane_encode(u_int, const struct screen *, u_int, msgpack_sbuffer *);
struct tmtv_snapshot_pane *tmtv_snapshot_pane_decode(msgpack_object);
/* Successful apply consumes grid/tab ownership, keeping the pane/screen objects. */
int tmtv_snapshot_pane_apply(struct window_pane *, struct tmtv_snapshot_pane *);
void tmtv_snapshot_pane_free(struct tmtv_snapshot_pane *);
void tmtv_snapshot_screen_apply(struct screen *, struct tmtv_snapshot_pane *);
int tmtv_snapshot_pane_validate(msgpack_object, u_int *);
int tmtv_snapshot_object_bounded(msgpack_object);
#define TMTV_SNAPSHOT_VERSION 1
#define TMTV_SNAPSHOT_CAPABILITY "1"
#define TMTV_SNAPSHOT_OPCODE 20
#define TMTV_SNAPSHOT_MAX_PARSER_BYTES 65536
struct input_snapshot;
int input_snapshot_encode(struct input_ctx *, msgpack_sbuffer *);
struct input_snapshot *input_snapshot_decode(msgpack_object);
void input_snapshot_apply(struct input_ctx *, struct input_snapshot *);
void input_snapshot_free(struct input_snapshot *);
#endif
