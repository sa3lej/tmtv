# Reconnect text-grid codec, version 1

This is a locally implemented codec proposal. It is built into host and relay
but is not wired to a transport opcode or capability yet. Existing sharing and
reconnect behaviour is unchanged until the compatibility decision is agreed.

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
are rejected before interning. Tab widths may use the full 32-column buffer. Invalid scalar encodings and
C0/C1/DEL controls are rejected. This validates encoding and bounds, not a new
Unicode grapheme segmentation or width algorithm.

The proposed production envelope will also need pane identity, screen/cursor and
saved-screen state. The grid codec itself owns no panes or live screens. It
validates the entire object before allocating or interning cell data and returns
a detached grid. Failed encoding appends no bytes to the caller's buffer.

## Limits and scope

- Default proposed history retention remains 100 rows, explicitly truncated at
  the oldest end. The codec allows at most 2000 history rows, matching the relay.
- Width, height and individual row cell counts are at most 10,000. A grid's total
  cell allocation cost is at most 262,144, including minimum row allocations at
  the current screen width. Encoded output is at most 16 MiB including bytes
  already in the destination buffer. A future pane envelope must enforce this
  same limit across the complete frame, rather than independently per grid.
- Hyperlink handles are process-local. A grid containing a nonzero link handle
  is rejected, rather than silently copied or stripped. Portable hyperlink tables
  need a separate representation before those grids can use this codec.
- SIXEL/image lists belong to screens, not grids; this codec carries no image
  payload and makes no image-reconnect preservation claim. A future screen
  envelope must report or reject unsupported image state explicitly.
- Colour identifiers are preserved; theme definitions, terminal capabilities and
  actual browser rendering are separate session/viewer concerns.

## Compatibility decision to review

Recommended: extend the existing explicit capability handshake so both owned
host and relay must support this snapshot version. An unsupported relay gets an
update-required sharing error while the local terminal continues. Do not silently
fall back to the old lossy snapshot. Old clients can keep using the existing
service until the coordinated new-version rollout.

Alternative: negotiate the codec when available but retain legacy snapshots for
older peers. This permits mixed versions at the cost of the existing colour,
attribute and combined-Unicode losses on those connections.

The grid codec can be validated independently of this choice. A new live wire
path, full envelope, authenticated reconnect rendering and deployment remain
separate from the codec validation documented here.
