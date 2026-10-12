# tmux 3.8 integration boundary

This comparison uses committed tmtv source at
`6ba247df9c1d0e828ce3f01cfc55ecb213e06b00`, before the migration. It does not
claim that the host, relay or viewers run tmux 3.8 yet.

## Official source pins

| Role | Official tag | Commit |
| --- | --- | --- |
| Existing portable core | 3.6a | `cc117b5048f77a4842820f8ebbe3a86e5c077224` |
| Migration target | 3.8 | `7f2a35ad3321f9ba57a1062ca73b1f3ff26aca53` |

The [official 3.8 tag](https://github.com/tmux/tmux/releases/tag/3.8) is an
annotated tag (`72c20dc22c24c34b8e39506f524c9c88da2f48aa`) pointing to the
target commit. The [3.8 changes](https://github.com/tmux/tmux/blob/7f2a35ad3321f9ba57a1062ca73b1f3ff26aca53/CHANGES)
include modal floating panes, stacking over zoomed panes, JSON layout strings
and theme changes. A common Git ancestor is not a suitable integration
baseline: the fork and portable upstream share older OpenBSD history.

Reproduce the inventory without importing or creating tags:

```sh
git fetch --no-tags https://github.com/tmux/tmux.git cc117b5048f77a4842820f8ebbe3a86e5c077224
git fetch --no-tags https://github.com/tmux/tmux.git 7f2a35ad3321f9ba57a1062ca73b1f3ff26aca53
python3 scripts/tmux38-inventory.py --fork 6ba247df9c1d0e828ce3f01cfc55ecb213e06b00
```

The script compares blob IDs, file modes and object types across three
committed trees. It reads Git only and does not fetch, check out, merge or write
files. Its scope is root C/headers, compatibility code, regression/fuzz inputs
and core build files. It also lists fork additions under host sharing, relay,
tests and web viewers. Site, deployment, release packaging, manuals and
repository metadata are outside this core inventory. `--fork HEAD` gives a
comparison for a later committed stage; uncommitted edits are excluded.

## Shared modifications

There are **29 core/build files changed by both tmtv and upstream**, and
**435 files changed only by upstream** within the defined core scope. Overlap
means both sides changed the file; it does not imply a textual merge conflict.
All existing tmtv modifications to shared core/build files need review.

| Integration surface | Files changed on both sides |
| --- | --- |
| Build and feature selection | `Makefile.am`, `configure.ac` |
| Startup, sessions and environment | `cfg.c`, `client.c`, `environ.c`, `log.c`, `server.c`, `session.c`, `tmux.c` |
| Commands, options and formats | `cmd-list-sessions.c`, `cmd-new-session.c`, `cmd-queue.c`, `cmd-set-option.c`, `format.c`, `options-table.c`, `status.c` |
| Rendering, clipboard and graphics | `image-sixel.c`, `image.c`, `input.c`, `paste.c`, `screen-redraw.c`, `screen-write.c`, `tty.c`, `utf8.c`, `window-copy.c` |
| Pane state, input and authentication IPC | `server-client.c`, `tmux-protocol.h`, `tmux.h`, `window.c` |

New upstream root C modules are `cmd-list-commands.c`, `events-payload.c`,
`events.c`, `fuzzy.c`, `hooks.c`, `json.c`, `monitor.c`, `prompt-history.c`,
`prompt.c`, `sort.c`, `tty-draw.c`, `window-border.c`, `window-panes.c`,
`window-switch.c` and `window-visible.c`. Upstream removes
`cmd-display-panes.c`, `notify.c` and `compat/fgetln.c`. Host and relay source
lists share the core and must change together; renaming the version alone
would leave the old implementation in place.

The fork integration inventory contains 12 host-sharing files, 21 relay files
(including its Dockerfile), 32 test files and four web-viewer files. These are
outside the upstream core, but depend on its APIs and state.

## Floating-pane protocol gap

`tmate-encoder.c:tmate_sync_layout` emits each pane as
`[id, sx, sy, xoff, yoff]`. Both protocol headers document that five-field
shape. It carries no floating/tiled distinction, stacking order, visibility,
zoom relationship or modal input policy. The geometry-only layout hash in
`server-client.c` cannot detect a stacking or modal-state change by itself.

`server/tmate-daemon-decoder.c:tmate_sync_window_panes` reconstructs every
pane as a leaf of one `LAYOUT_LEFTRIGHT` tree. That construction cannot
represent overlapping floating panes. Official 3.8 introduces per-window
`z_index`, `modal`, `modal_last` and `was_zoomed` state, pane visibility ranges,
and capture/close/float-over-zoom flags. These affect rendering and input, so
their observable semantics must cross the host/relay boundary. Internal flag
numbers alone are not a stable external protocol.

The relay emits the same old layout shape to SSE clients. Its composed-screen
path attaches a read-only tmux client and sends full-screen output as pane ID
`-1`; it therefore depends on correct relay reconstruction even when the web
viewer is not drawing individual panes. `web/index.html` also contains a
per-pane layout renderer. Both paths matter for compatibility.

Existing message IDs, legacy layout decoding and capability-negotiated
reconnect identity are compatibility constraints for a protocol extension.
Authentication IPC in `tmux-protocol.h` uses IDs starting at 900; upstream 3.8
uses tmux IPC protocol version 8. Local tmux IPC and the host/relay sharing
protocol are distinct and need separate compatibility validation.

## Preservation boundaries and validation evidence

Authentication and read-only checks cross `server-client.c`,
`server/tmate-auth-keys.c`, SSH authentication handlers and HTTP input routes.
Modal panes must not bypass those checks. Reconnect identity crosses the
host session/header encoder and relay resume/decoder code; pending PTY replay
must still reach the same pane after reconstruction.

Recording hooks in `window.c` and `tmate-encoder.c`, clipboard propagation in
`paste.c` and relay handlers, and raw SIXEL storage/freeing in `input.c`,
`image.c` and `tty.c` are fork behavior. The SIXEL allocation bound is also
fork behavior. Synchronized rendering adds host input, timer, screen-write
and redraw hooks; 3.8 has its own synchronization state, so the port must
reconcile lifetimes and timeout semantics rather than duplicate them.

The inventory has been checked against independent `git diff --name-only`
comparisons for both source pins and the baseline fork. This is source
inventory evidence, not host/relay execution evidence. The supplied baseline
reports successful Linux host/relay builds and 158 standard tests; source
wiring assertions have known stale failures. During this inventory stage,
`make -C tests unit server-unit` passed all 84 assertions on the unchanged
baseline binaries. This does not validate a migrated runtime. Migration acceptance requires
tests through actual encoders, decoders, authenticated viewers and composed
rendering, including overlap, hidden/modal panes, zoom, resize, reconnect,
read-only rejection, clipboard, recording, SIXEL and synchronized output.
Cross-platform acceptance also requires macOS builds, including Intel.
