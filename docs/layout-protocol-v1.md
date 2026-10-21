# Exact layout protocol v1

This is the implemented host/relay wire contract. Both components require a
coordinated upgrade to host protocol 7. The SSH terminal and SSE viewer continue
to receive the relay's composed terminal output; opcode 19 is private to the
host/relay connection and is excluded from browser broadcasts and replay.

## Negotiation and readiness

The host sends its protocol-7 header and existing configuration messages. A
protocol-7 relay replies with the existing SET_ENV message containing
`tmtv_layout_capability=1`. Only after that acknowledgement does the host send
the new layout frame, existing screen/mode snapshots, and READY. The relay also
holds READY until a complete valid layout has been installed and the existing
reconnect-identity gate has passed. It rejects hosts below protocol 7 and ignores
legacy rectangle layouts from protocol-7 hosts.

An unsupported capability or a missing acknowledgement after five seconds ends
sharing with `Relay update required: exact layout protocol v1 is unsupported`.
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
frame = [19, 1, active_window_index, [window, ...]]
window = [index, name, width, height, active_pane, modal_pane, modal_last_pane,
          was_zoomed_pane, flags, [pane, ...], [cell, ...], [saved_cell, ...],
          [pane_id_in_front_to_back_order, ...], [last_active_pane_id, ...],
          [border_lines, border_status]]
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
Window records currently require distinct pane IDs; aliases linking the same
window at multiple session indices are not yet representable by this schema.

Installation runs synchronously before returning to the event loop. It retains
existing panes by ID, including grid/mode/input state, moves them between target
windows if needed, creates missing panes, removes absent panes, installs both
trees and their leaf links, then applies order/active/modal state and redraws.
Pending PTY data is replayed only after the complete graph exists.

This extension transports layout state. It does not change the existing cell
snapshot's history, colour/attribute or image fidelity limits. Full browser DOM
and non-Linux platform validation remain separate coverage from the isolated
SSH/SSE integration tests.
