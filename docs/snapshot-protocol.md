# Reconnect text-grid codec, version 1

The grid and pane-screen codecs are built into host and relay but are not yet
wired to a transport opcode or capability. The required-upgrade compatibility
decision was agreed on 2026-10-21; transport negotiation is a subsequent stage.

The legacy snapshot conflates text bytes with cells and truncates style fields.
The new representation gives every cell explicit bytes and width, keeps RGB,
256-colour and theme colour identifiers, underline colour and modern attributes,
and includes line wrapping, used-cell counts and OSC 133 markers. It carries
host-selected widths rather than recomputing widths using relay locale tables.

## Concrete grid representation

MessagePack arrays (all integers nonnegative):

```
grid = [1, width, height, history_count, scrolled_count, grid_flags, rows]
row  = [used_cell_count, line_flags, osc133, cells]
osc133 = [prompt_column, command_column, output_start, output_end, exit_status]
cell = [BIN utf8_bytes, width, flags, attributes, foreground, background, underline]
```

Dimensions and row lengths are explicit. History rows precede visible rows.
Rows may be wider than the current grid after a terminal resize. Allocator flags,
extended-cell offsets, UTF-8 intern handles and process-relative line timestamps
are never copied between processes. Logical cleared, selected, padding, tab and
no-palette flags survive. Tabs carry empty bytes and their column width; padding
carries empty bytes and width zero. Wide characters and tabs require following
padding cells. UTF-8 bytes may include combining marks, variation selectors and
ZWJ sequences up to 31 bytes per text cell. The tmux cell buffer is 32 bytes,
but its packed internal handle has a five-bit length, so 32-byte text payloads
are rejected before interning. Tab widths may use the full 32-column buffer.
Invalid scalar encodings and
C0/C1/DEL controls are rejected. This validates encoding and bounds, not a new
Unicode grapheme segmentation or width algorithm.

The pane-screen envelope below carries identity, current and saved screen state.
Both codecs validate complete objects before allocating grids or interning cell
data. Failed encoding appends no bytes to the caller's buffer.

## Limits and scope

- Default proposed history retention remains 100 rows, explicitly truncated at
  the oldest end. The codec allows at most 2000 history rows, matching the relay.
- Width, height and individual row cell counts are at most 10,000. A grid's total
  cell allocation cost is at most 262,144, including minimum row allocations at
  the current screen width. Encoded output is at most 16 MiB including bytes
  already in the destination buffer. The pane envelope enforces these limits
  across the complete pair of grids, rather than independently per grid.
- Hyperlink handles are process-local. A grid containing a nonzero link handle
  is rejected, rather than silently copied or stripped. Portable hyperlink tables
  need a separate representation before those grids can use this codec.
- SIXEL/image lists belong to screens, not grids; this codec carries no image
  payload and makes no image-reconnect preservation claim. The pane envelope
  explicitly rejects active or saved image state.
- Colour identifiers are preserved; theme definitions, terminal capabilities and
  actual browser rendering are separate session/viewer concerns.

## Pane-screen envelope, version 1

```
pane = [1, pane_id, current_grid, cursor, modes, cursor_style, scroll_region,
        tabs, saved_grid_or_nil, saved_cursor, saved_cell_or_nil, saved_flags]
cursor = [x, y]
modes = [current_mode, default_mode]
cursor_style = [current_style, default_style, current_colour, default_colour]
scroll_region = [top, bottom]
tabs = [sorted_unique_column_indices]
saved_cursor = [x, y]
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
A source containing current/saved SIXEL images or nonportable hyperlink handles
is explicitly rejected, with no partial append. Applying a validated image-free
snapshot disposes of stale base-screen images, including saved alternate images.

This screen-only envelope does not contain terminal-parser escape buffers,
current parser SGR state, or DECSC parser state. Transport integration must handle
parser continuation before enabling the live path. Pane/window metadata such as
title, path, progress and mode-specific selection remain outside this envelope.
An all-pane transport frame must also impose an aggregate budget across panes;
per-pane validation is not a complete session budget.

## Required-upgrade decision, agreed 2026-10-21

The new snapshot path requires upgraded host and relay, without a lossy legacy
fallback. Permit enough handshake for an outdated host to receive a clear
new-version/update-required notice before refusing sharing. Release-version
comparison is separate from negotiated protocol support: the existing normal
update notice runs in relay `tmate_ready`, while old-protocol rejection runs
in `tmate_header` and currently queues a notice before terminating. Notice
delivery before refusal must be verified in the negotiation stage, rather than
assuming a queued message has reached the client. The local terminal remains
usable after refusal. No release/deployment is implied by this local decision.

Grid and pane-screen codec tests exercise real terminal-parser state and full
production host/relay objects, including bytewise/bulk streams and alternate
screen exit after resize. Authenticated transport/browser rendering for the NEW
snapshot path is not active or validated yet.
