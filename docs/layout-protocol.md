# Exact layout protocol v2

This is the implemented host/relay wire contract. Both components require a
coordinated upgrade with layout capability 2 (introduced in protocol 7, retained
in protocol 8). Capability 1
peers cannot represent aliases and are explicitly rejected; there is no implicit
change to the meaning of a capability-1 frame. The SSH terminal and SSE viewer continue
to receive the relay's composed terminal output; opcode 19 is private to the
host/relay connection and is excluded from browser broadcasts and replay.

## Negotiation and readiness

The host sends its protocol-8 header and existing configuration messages. A
protocol-8 relay replies using SET_ENV with `tmtv_layout_capability=2` and
`tmtv_snapshot_capability=1`. After both acknowledgements, the host sends exact
layout, the bounded screen/parser snapshot and READY. The relay holds READY
until both valid frames are installed and the reconnect identity gate passes.
It refuses mismatched host protocols and ignores legacy rectangle layouts from
protocol-7-or-later hosts. The complete snapshot handshake and delivered update
notice are specified in [snapshot-protocol.md](snapshot-protocol.md).

An unsupported capability or a missing acknowledgement after five seconds ends
sharing with `Relay update required: exact layout protocol v2 is unsupported`.
The message is available in the host format `#{tmtv_layout_error}` as well as
normal status/log output. Local terminals remain usable. There is no lossy
fallback and no automatic retry of an unsupported peer. Restart sharing after
upgrading that peer. Capability and last-sent state reset on each transport;
reconnect sends the latest complete state after the fresh acknowledgement.

## MessagePack schema

All arrays have the exact length shown. Missing pane references use -1. IDs are
nonnegative signed-32-bit integers. Wire enums and flags are independent of
internal tmux enum and flag values.

```text
frame = [19, 2, active_window_index, [window, ...]]
window = [index, name, width, height, active_pane, modal_pane, modal_last_pane,
          was_zoomed_pane, flags, [pane, ...], [cell, ...], [saved_cell, ...],
          [pane_id_in_front_to_back_order, ...], [last_active_pane_id, ...],
          [border_lines, border_status], window_id]
pane = [id, width, height, x_offset, y_offset, flags, border_lines, border_status]
cell = [parent_index, type, floating, geometry, remembered_float_geometry, pane_id]
geometry = [width, height, signed_x_offset, signed_y_offset]
```

Window flags: bit 0 = zoomed, bit 1 = was-zoomed.

Pane flags: bit 0 = zoomed, bit 1 = float-over-zoom, bit 2 = capture-all-keys,
bit 3 = close-on-click, bit 4 = close-on-cancel, bit 5 = input-off.

Cell types: 0 = left/right node, 1 = top/bottom node, 2 = pane leaf. Nodes have
pane_id -1 and cannot float. Cells are listed with parents before children;
the sole root has parent_index -1 and all other parent indices refer backwards.
Sibling order follows array order. `remembered_float_geometry` is either a
geometry or `[]` for the core's unset sentinel. This preserves hidden tiled
placement independently of the current floating rectangle.

The saved tree is present exactly when the window is zoomed. Every pane must
occur once in that saved tree, or in the current tree for an unzoomed window.
Hidden panes may be absent from a zoomed current tree. Each pane's current and
saved leaf pointers are restored independently. Z order contains every pane
exactly once; pane list order, active pane and last-active order are separate.
Border values are the tmux-3.8 pane-border-lines/status option enums; this protocol
version is tied to that core's option contract.

## Validation and installation

The encoder constructs a whole frame before queuing it. A rejected queue write
does not update its last-sent cache. Comparing complete serialized state also
detects stacking/modal/flag changes that leave all rectangles and active pane
unchanged. The encoder checks after each server loop as well as normal layout
notifications.

The decoder first builds a detached plan, validating version, field types,
lengths, dimensions, flag masks, reference membership, unique IDs, complete
z order, tree topology and leaf coverage. Limits are 512 windows, 1000 total
panes, 4000 cells per tree, depth 1000, names up to 4096 bytes, and an encoded
host frame up to 2 MiB. Dimensions are 1..WINDOW_MAXIMUM; offsets may be signed
within that bound. Invalid frames leave the existing session untouched.
Canonical window records require globally unique pane IDs and window IDs.
Each later link to the same physical window is `[index, window_id]`, referencing
a canonical record rather than duplicating panes or trees. References must resolve
to a full record; dangling aliases, duplicate indices and duplicate full window IDs
are rejected. The selected index may refer to an alias. Window IDs remain stable
when the first link is removed, windows are renumbered or indices are reused.

Installation runs synchronously before returning to the event loop. Each physical
window is pinned and rebuilt once; links are rebound by stable host window ID,
so index reuse cannot move a surviving window onto a different graph. It retains
existing panes by ID, including grid/mode/input state, moves them between target
windows if needed, creates missing panes, removes absent panes, installs both
trees and their leaf links, then applies order/active/modal state and redraws.
Pending PTY data is replayed only after the complete graph exists. Reconnect
screen/mode snapshots likewise visit each physical window once, avoiding duplicate
pane records and repeated literal-view contents through aliases.

This extension transports layout state. The companion snapshot protocol defines
history, colour/attribute and image limits. Full browser DOM
and non-Linux platform validation remain separate coverage from the isolated
SSH/SSE integration tests.
