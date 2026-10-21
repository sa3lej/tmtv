/* Exact layout state. Wire constants are independent of tmux's flag bits. */
#ifndef TMTV_LAYOUT_H
#define TMTV_LAYOUT_H
#include <msgpack.h>
#define TMTV_LAYOUT_VERSION 2
#define TMTV_LAYOUT_CAPABILITY "2"
#define TMTV_LAYOUT_OPCODE 19
#define TMTV_LAYOUT_MAX_PANES 1000
#define TMTV_LAYOUT_MAX_CELLS 4000
#define TMTV_LAYOUT_MAX_WINDOWS 512
#define TMTV_LAYOUT_PANE_FLAGS 63
struct tmtv_layout_pane {
	int id, xoff, yoff, flags, border, status;
	u_int sx, sy;
};
struct tmtv_layout_tree {
	u_int count;
	struct layout_cell **cells;
	int *panes;
};
struct tmtv_layout_window {
	int id, alias; /* alias is canonical plan index + 1, zero for full state */
	int index, active, modal, modal_last, was_zoomed, flags, border, status;
	char *name;
	u_int sx, sy, count, last_count;
	struct tmtv_layout_pane *panes;
	int *zorder, *last;
	struct tmtv_layout_tree current, saved;
};
struct tmtv_layout {
	int active;
	u_int count;
	struct tmtv_layout_window *windows;
};
int tmtv_layout_encode(struct session *, msgpack_sbuffer *);
struct tmtv_layout *tmtv_layout_decode(msgpack_object);
void tmtv_layout_free(struct tmtv_layout *);
int tmtv_layout_pane_flags(struct window_pane *);
void tmtv_layout_set_pane_flags(struct window_pane *, int);
u_int tmtv_layout_window_id(struct window *);
int tmtv_layout_first_link(struct session *, struct winlink *);
#endif
