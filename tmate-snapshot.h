/* Bounded grid, screen and parser state for negotiated reconnect snapshots. */
#ifndef TMTV_SNAPSHOT_H
#define TMTV_SNAPSHOT_H
#include <msgpack.h>
#define TMTV_SNAPSHOT_GRID_VERSION 2
#define TMTV_SNAPSHOT_MAX_UTF8 UTF8_SIZE
#define TMTV_SNAPSHOT_HISTORY 100
#define TMTV_SNAPSHOT_MAX_HISTORY 2000
#define TMTV_SNAPSHOT_MAX_CELLS 262144
#define TMTV_SNAPSHOT_MAX_BYTES (16 * 1024 * 1024)
/* On failure encode leaves the destination untouched; decode owns a new grid.
 * Link references require the owning screen table; images belong to screens.
 */
int tmtv_snapshot_grid_encode(struct grid *, u_int, msgpack_sbuffer *);
struct grid *tmtv_snapshot_grid_decode(msgpack_object, u_int);
/* Shared screen-envelope validation helpers; no pane mutation. */
int tmtv_snapshot_grid_validate(msgpack_object, u_int, u_int *);
int tmtv_snapshot_colour(u_int);
int tmtv_snapshot_cell_encode(const struct grid_cell *, msgpack_packer *);
int tmtv_snapshot_cell_decode(msgpack_object, struct grid_cell *);

#define TMTV_SNAPSHOT_PANE_VERSION 3
#define TMTV_SNAPSHOT_MAX_LINKS 4999
#define TMTV_SNAPSHOT_MAX_IMAGES 19
#define TMTV_SNAPSHOT_MAX_PIXELS (TMTV_SNAPSHOT_MAX_BYTES / 2)
struct tmtv_snapshot_image {
	u_int px, py;
	struct sixel_image *data;
};
int tmtv_snapshot_text(const u_char *, u_int);
int hyperlinks_snapshot_encode(struct hyperlinks *, msgpack_packer *);
int hyperlinks_snapshot_validate(msgpack_object);
int hyperlinks_snapshot_has(msgpack_object, u_int);
struct hyperlinks *hyperlinks_snapshot_decode(msgpack_object);
void hyperlinks_snapshot_activate(struct hyperlinks *);
int tmtv_snapshot_media_budget(msgpack_object, u_int *, u_int *, u_int *);
#ifdef ENABLE_SIXEL
int sixel_snapshot_encode(struct sixel_image *, msgpack_packer *);
int sixel_snapshot_validate(msgpack_object, u_int *);
struct sixel_image *sixel_snapshot_decode(msgpack_object);
#endif
struct tmtv_snapshot_pane {
	u_int id, cx, cy, saved_cx, saved_cy, rupper, rlower;
	int mode, default_mode, saved_flags, ccolour, default_ccolour;
	enum screen_cursor_style cstyle, default_cstyle;
	struct grid *grid, *saved_grid;
	struct grid_cell saved_cell;
	bitstr_t *tabs;
	char *title, *path, *titles[10];
	u_int ntitles;
	struct hyperlinks *hyperlinks;
	struct tmtv_snapshot_image *images[2];
	u_int nimages[2];
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
#define TMTV_SNAPSHOT_VERSION 2
#define TMTV_SNAPSHOT_CAPABILITY "2"
#define TMTV_SNAPSHOT_OPCODE 20
#define TMTV_SNAPSHOT_MAX_PARSER_BYTES 65536
struct input_snapshot;
int input_snapshot_encode(struct input_ctx *, msgpack_sbuffer *);
struct input_snapshot *input_snapshot_decode(msgpack_object);
int input_snapshot_links_validate(struct input_snapshot *, msgpack_object);
void input_snapshot_apply(struct input_ctx *, struct input_snapshot *);
void input_snapshot_free(struct input_snapshot *);
#endif
