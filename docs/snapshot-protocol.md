# Negotiated reconnect snapshots

Protocol 11 requires exact layout capability `2` and reconnect snapshot capability
`2`. Both host and relay must be upgraded. These codecs are active on initial
sharing and host reconnect; there is no lossy legacy fallback for a new peer.

The legacy snapshot conflates text bytes with cells and truncates style fields.
The new representation gives every cell explicit bytes and width, keeps RGB,
256-colour and theme colour identifiers, underline colour and modern attributes,
and includes line wrapping, used-cell counts and OSC 133 markers. It carries
host-selected widths rather than recomputing widths using relay locale tables.

## Concrete grid representation

MessagePack arrays (all integers nonnegative):

```
grid = [2, width, height, history_count, scrolled_count, grid_flags, rows]
row  = [used_cell_count, line_flags, osc133, cells]
osc133 = [prompt_column, command_column, output_start, output_end, exit_status]
cell = [BIN utf8_bytes, width, flags, attributes, foreground, background, underline, link_reference]
```

Dimensions and row lengths are explicit. History rows precede visible rows.
Rows may be wider than the current grid after a terminal resize. Allocator flags,
extended-cell offsets, UTF-8 intern handles and process-relative line timestamps
are never copied between processes. Logical cleared, selected, padding, tab and
no-palette flags survive. Tabs carry empty bytes and their column width; padding
carries empty bytes and width zero. Wide characters and tabs require following
padding cells. UTF-8 bytes may include combining marks, variation selectors and
ZWJ sequences up to 32 bytes per text cell. The packed process-local handle
uses six length bits and two width bits, preserving the full 32-byte buffer.
Larger text payloads are rejected before interning. Tab widths may use the full
32-column buffer.
Invalid scalar encodings and
C0/C1/DEL controls are rejected. This validates encoding and bounds, not a new
Unicode grapheme segmentation or width algorithm.

The pane-screen envelope below carries identity, current and saved screen state.
Both codecs validate complete objects before allocating grids or interning cell
data. Failed encoding appends no bytes to the caller's buffer.

Completed OSC metadata (title, optional path and each saved title) is untrusted
on receipt, even though a normal host has already escaped it. Each string must
be valid UTF-8, at most 16384 bytes, and contain no C0/C1/DEL controls, embedded
NUL or `#(` command expansion. Invalid metadata rejects the entire snapshot
before grids, parser state or modes are installed. Already escaped text,
including literal backslashes, is preserved byte for byte; it is not escaped
again on each reconnect. A partial OSC in parser continuation remains parser
state and is subject to the ordinary terminal parser when completed.

## Limits and scope

- Default history retention is 100 rows, explicitly truncated at
  the oldest end. The codec allows at most 2000 history rows, matching the relay.
- Width, height and individual row cell counts are at most 10,000. A grid's total
  cell allocation cost is at most 262,144, including minimum row allocations at
  the current screen width. Encoded output is at most 16 MiB including bytes
  already in the destination buffer. The pane envelope enforces these limits
  across the complete pair of grids, rather than independently per grid.
- Links use screen-scoped tables with at most 4999 entries across the entire
  frame, including independent mode backings. References are validated against
  their owning table, never resolved through a receiver's old table. Tables are
  decoded detached from the global eviction list; commit assigns fresh terminal
  external IDs. Named URI/ID pairs are unique; anonymous links remain distinct.
- SIXEL uses bounded decoded pixels rather than supplied DCS strings. Up to 19
  images and 8,388,608 pixels are allowed across current/saved/backing screens,
  also subject to the complete 16 MiB encoded-frame cap. Both peers must have
  SIXEL enabled to accept nonempty image lists. A build lacking it refuses them.
- Colour identifiers are preserved; theme definitions, terminal capabilities and
  actual browser rendering are separate session/viewer concerns.

## Pane-screen envelope, version 3

```
pane = [3, pane_id, current_grid, cursor, modes, cursor_style, scroll_region,
        tabs, saved_grid_or_nil, saved_cursor, saved_cell_or_nil, saved_flags, metadata,
        links, current_images, saved_images]
cursor = [x, y]
modes = [current_mode, default_mode]
cursor_style = [current_style, default_style, current_colour, default_colour]
scroll_region = [top, bottom]
tabs = [sorted_unique_column_indices]
saved_cursor = [x, y]
metadata = [BIN title, BIN path_or_nil, [progress_state, progress], title_stack]
title_stack = [BIN most_recent_saved_title, ...]
links = [[positive_reference, BIN normalized_uri, BIN normalized_internal_id], ...]
images = [[column, row, bitmap], ...]
bitmap = [1, width, height, cell_pixel_width, cell_pixel_height, p2,
          has_raster_attributes, raster_width, raster_height, used_colours,
          [colour_register, ...], [BIN big_endian_uint16_pixel_row, ...]]
```

The two grids share one 262,144 allocation-cell budget and one 16 MiB envelope
limit. Saved grids have no history; the current alternate grid retains earlier
normal-screen history but has history collection disabled. Saved grids may have
different dimensions after a resize. Cursors allow `x == width` for pending wrap;
the paired `UINT_MAX, UINT_MAX` saved-cursor sentinel means no saved cursor. Unset
cursor colours are `-1`. A saved style uses the grid cell representation and
cannot be a tab or padding. Uninitialized saved fields are omitted when inert.

Decode returns a detached plan only after validating BOTH grids and all metadata.
Apply requires the existing pane ID and current geometry to match. It replaces
only base-screen grid/cursor/mode/scroll/tab/saved state; the pane and screen
objects, mode stack and independently owned copy/view backing remain intact.
Old queued screen writes are discarded before replacing their grids. Old sync
state is cancelled; a restored synchronized-output mode gets a fresh timeout.
Grid and saved-style references must exist in the screen's link table. URI and
internal ID strings preserve their already normalized values without a second
escape pass; UTF-8 and control characters are validated before use. URI strings
are at most 1024 bytes, internal IDs at most 64 KiB, table references at most
INT_MAX and strictly increasing. Reference zero means no link.

Bitmaps preserve transparent pixels (index zero), palette registers, p2, raster
attributes and original cell pixel geometry. Dimensions are 1..10000, each row
has exactly width*2 bytes and every pixel index fits the declared used-colour
range. Registers preserve undefined slots or valid HLS/RGB definitions. No raw
host DCS is replayed: the receiver regenerates terminal output from validated
pixels. Image plans remain outside global image ownership until commit. Applying
an image-free snapshot disposes of stale current and saved alternate images.
These global cache limits deliberately count duplicated tables/backings; an
excessive frame is refused rather than evicting part of its new state.

Titles, path and the ten-entry title stack are bounded to 16 KiB per string;
embedded NUL is rejected. Progress is -1 or 0..100, with state 0..4.

## Parser continuation and all-pane frame

```
frame = [20, 2, [[pane, parser, modes], ...]]
modes = [[1_or_2, backing_pane, copy_state], [3, nil, nil], ...]
copy_state = [scroll_offset, cursor_x, cursor_y, selection, selection_x,
              selection_distance_from_bottom, rectangle]
```

Modes are ordered top first. Copy (1), literal view (2) and clock (3) can appear
once each. Copy/view own independent backing screens, so live output beneath a
frozen selection remains separate. Backing history expands beyond the default
100 rows to include the viewport and selected line, up to 2000 rows; an excessive
selection or unknown mode is refused explicitly.

Copy/view cursor and selection columns may equal the backing width: copy-mode
commands can place them just after a full-width line. Ordinary state updates
accept the same end-of-line position as exact snapshots; columns greater than
the width and rows outside the screen are rejected. This copy-mode position is
distinct from the base terminal screen's pending-wrap cursor.

Parser version 2 contains current and DECSC-saved cell styles/character sets,
saved cursor/origin mode, a portable state-table index, intermediate/raw parameter
buffers, collected OSC/DCS bytes and terminator, parsed parameters, partial UTF-8,
last character for REP, flags, pending control-client bytes and palette overrides.
The layout is a fixed 15-field array; no pointers or internal struct bytes cross
the wire. Logical buffered input plus pending bytes is bounded to 64 KiB, parsed
parameters to 24 and their strings to 63 bytes. The encoder bounds the complete
parser object to 64 KiB. State is installed directly, without replaying already
executed C0 controls or replies. Outstanding device/clipboard requests are
cancelled; a partial escape receives a fresh five-second parser timeout.
Completed ground-state scratch buffers are omitted because their lengths may be
stale after the normal parser shrinks its allocation.

One frame must contain each pane in the current layout exactly once, with matching
IDs and geometry. Linked window aliases do not duplicate screens. All base/saved
grids and mode backings share a 262,144-cell allocation budget and a 16 MiB frame
budget, with at most 1000 panes. Nesting and object counts are bounded too. The
relay validates the entire frame before grid allocation/interning or replacing
any live pane. A malformed later pane leaves earlier panes unchanged. Valid
frames replace screens and parser state together, then restore mode stacks.

## Required-upgrade handshake

The relay advertises `tmtv_snapshot_capability=2` and
`tmtv_layout_capability=2`. The host withholds PTY/mode traffic and READY until
both capabilities arrive, then sends exact layout and the complete snapshot.
The relay withholds sharing URLs until it has both valid layout and snapshot.
Capabilities reset for every transport; late capability messages cannot revive
a failed negotiation. Missing/unsupported capability refuses sharing and leaves
the local terminal usable.

An incompatible host may complete SSH and send its header. The relay sends the
normal NOTIFY plus `tmtv_update_required`, including required/actual protocol and
an install command. It stops dispatching that client's messages, drains its
encoder with bounded retries and flushes SSH before closing outside the data
callback. A genuine previous protocol-7 host is tested receiving and displaying
the notice before refusal. A new host against the previous relay reports a relay
update requirement without publishing links. The normal release-version checker
in `tmate_ready` remains separate and unchanged.

The private opcode 20 is excluded from raw web/replay forwarding; SSH and SSE
viewers render the restored relay screens through the existing terminal path.
No staging, production activation or release is implied by local protocol work.

Validation uses full production host/relay objects, every-byte parser split
points, malformed frame mutations and authenticated isolated SSH/SSE viewers.
RGB foreground/background, italics, strike, combining/wide Unicode and partial
CSI/UTF-8 continuations are parsed through real reconnects. Browser DOM rendering,
other terminal attributes, terminal capability discovery and supported-platform
validation remain separate work. Production-object tests cover portable link
namespaces, active parser links, current/saved SIXEL pixels/palettes, resource
limits and atomic malformed-media rejection. SSH/SSE tests additionally inspect
real renderer OSC 8/DCS output across reconnect and alternate-screen exit. This
is transport evidence, not browser clickability or pixel-canvas verification.

Sharing protocol 10 also requires the corrected 32-byte cell implementation on
both host and relay. Older peers are refused by the existing version gate before
sharing: protocol 9 peers can corrupt a 32-byte live cell or reject its snapshot.
Snapshot arrays and resource budgets are unchanged; the text length bound now
matches the existing 32-byte cell buffer. The process-local packed handles are
never transmitted and the local tmux IPC version is unchanged.

Sharing protocol 11 sends non-ASCII keyboard input as a tagged Unicode scalar
(`0x100000000 | codepoint`) instead of a process-local packed UTF-8 handle.
Modifiers are retained. The host reconstructs its own UTF-8 key, including local
cache entries for four-byte characters. The tag keeps private-use Unicode scalars
separate from the legacy special-key numbers; ASCII, literal bytes and function
keys keep their existing representation. Both host and relay must be upgraded.
Live layout encoding failures now send FIN, expose `tmtv_layout_error`, and stop
further PTY/READY traffic, leaving the local terminal usable.
