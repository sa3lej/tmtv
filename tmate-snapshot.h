/* Bounded text-grid snapshot codec. Not a negotiated transport opcode. */
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
#endif
