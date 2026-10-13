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

## Portable compatibility stage

The first implementation stage imports `compat/base64.c`, `compat/getline.c`,
`compat/getopt_long.c`, `compat/getpeereid.c` and `compat/imsg-buffer.c` verbatim
from the pinned 3.8 commit. `compat.h` adopts the matching unsigned input
prototype for `b64_ntop`. This brings in the line-buffer growth overflow guard,
correct socket credential length type and upstream declaration cleanups.
It does not change the reported core version or pane/protocol behavior.

`tests/test-compat.c` compiles the actual production base64, line-reader and
peer-credential fallback sources under private symbol names, even when libc
provides native alternatives. Tests cover binary round trips and padding,
invalid base64 and undersized destinations with a guard byte, growth across
multiple buffer reallocations, final lines without newline, and credentials
from a real Unix socket pair. Platforms with credential APIs additionally
check invalid-descriptor rejection without overwriting the output identity.

Linux host and relay builds with `--enable-sixel` passed after this import.
All 162 standard tests passed (the previous 158 plus four fallback tests).
The four fallback tests also passed AddressSanitizer and UndefinedBehaviorSanitizer
with leak detection enabled. These checks validate this limited stage, not
3.8 floating panes or full migrated host/relay compatibility. Source wiring
assertions were not rerun; their supplied stale baseline failures remain
unresolved. macOS and other platform validation is still outstanding.

`compat/fgetln.c` and its declaration were retained here until the history
reader stage below migrated the caller and build selection together.
`compat/systemd.c` remains unchanged in this
stage: the upstream cgroup dependency behavior requires separate validation,
and this host has no `libsystemd` development package. The fuzz-only macros
in the upstream `compat.h` change also remain for the later fuzz/core stage.

## JSON layout support stage

`json.c`, `xmalloc.c` and `xmalloc.h` now match the pinned official 3.8
blobs exactly. Both production source lists include the parser and `tmux.h`
exposes its upstream API. This prepares for JSON layouts without activating
layout reconstruction, floating panes or a sharing protocol extension.
The host and relay still report the 3.6a core version.

`tests/test-json.c` links the production parser, allocation helpers and
recallocarray fallback. Five tests cover pane-shaped object-array round trips,
escape preservation, signed 64-bit boundaries, malformed/unsupported input,
duplicate keys, excessive nesting, token-array growth, lookup type/missing-key
errors, binary-safe memory duplication and the fatal multiplication-overflow
guard in `xcalloc`. The parser is upstream's restricted JSON subset: arrays
contain objects, strings cannot be empty, and escapes are preserved rather
than decoded. It must not be treated as a general-purpose JSON decoder.

Linux host and relay SIXEL builds passed and both binaries contain the parser
and memory-copy symbols. All 167 standard tests passed. All five new tests
also passed AddressSanitizer and UndefinedBehaviorSanitizer with leak detection.
GCC emits a possible-uninitialized warning for the upstream tokenizer's `loc`
under the test optimization flags; empty input returns before tokenization,
and whitespace-only rejection is exercised by the sanitized tests. The
upstream implementation is retained. The existing relay unchecked-write
warnings remain. macOS, source wiring baseline reconciliation, full 3.8 core
integration and authenticated floating-layout/viewer validation remain
outstanding; this support stage does not establish those results.

## Prompt history reader stage

The history loader in `status.c` now uses the pinned 3.8 `prompt-history.c`
implementation, with only existing `status_prompt_*` API names substituted.
It owns and frees a reusable `getline` buffer, strips newline terminators,
and handles final lines without a newline. The last `fgetln` caller, fallback
source, compatibility declaration and configure selection have migrated
together. This does not yet move the prompt subsystem into the new 3.8 modules.

`tests/test-prompt-history.py` starts isolated production host servers and
queries `show-prompt-history`. Four tests cover all four prompt types, legacy
and unknown type prefixes, embedded colons, blank entries, a 64 KiB line,
buffer reuse, final lines without newline, and empty/missing files. Both Linux
SIXEL binaries build and the standard suite passes 171 tests (167 existing
plus four history tests). These tests also pass with the production portable
`getline` linked into both binaries; symbol inspection confirms the fallback
is present and `fgetln` is absent. Relay history loading is not exercised as
an authenticated viewer scenario by these tests.

On glibc, forcing an otherwise unnecessary fallback exposes its native
`getdelim` declaration. The test-only preinclude reads that declaration before
renaming the fallback's private helper; the pinned production source stays
unchanged. Reproduce from the repository root:

```sh
./configure --enable-sixel ac_cv_func_getline=no \
  CPPFLAGS="-include $PWD/tests/test-getline-config.h"
make -j4
make -C tests prompt-history
```

Restore the native configuration with `./configure --enable-sixel` and force
relinking with `make -W status.o -W tmtv_server-status.o tmtv tmtv-server`;
changing configure's fallback library list alone may leave existing binaries
linked with the fallback. The native configuration and binaries were restored
and checked with `nm` after this stage. Both binaries
still report tmux 3.6a. Full 3.8 core, floating layout/protocol, authenticated
viewer, macOS and stale wiring-baseline validation remain outstanding.

## Shared child environment stage

`environ.c` now adopts all changes from the pinned 3.8 implementation:
NULL-safe environment cleanup, const-correct parsing and retention of inherited
terminal variables for configuration jobs. Its only differences from that
source are the existing defensive logging allocation check and tmtv's child
identity hooks (`TMTV`, isolated `TMUX`, and `TMUX_PROGRAM`). Configuration
`if-shell` and `run-shell` jobs can now inspect the outside terminal; ordinary
panes still receive `default-terminal` and the multiplexer terminal metadata.

`tests/test-child-environment.py` executes the production host's startup,
configuration job and pane spawn paths on isolated local sockets. Three cases
inspect actual child environments: inherited terminal metadata and socket
identity, explicitly unset and hidden variables, and pane terminal overrides
with matching session identity. The two configuration cases failed against
the previous binary because `TERM` was removed, then passed after this port.
Snapshots are written atomically to avoid reading partial child output.

Validation: both Linux host and relay SIXEL binaries build, `make -C tests all`
passes all 174 standard cases, and the three new cases pass again after the
atomic snapshot change. Relay compilation uses the same migrated environment
implementation; authenticated relay child execution is not covered here.
macOS, complete 3.8 core/header integration, floating layouts/protocol and
viewer validation remain outstanding. Runtime version remains 3.6a. This
stage is committed locally, with no push, release, tag or deployment.

## IPC peer stage

`proc.c` now matches the pinned official 3.8 blob exactly, and `tmux.h`
exposes `proc_get_peer_gid`. Peers retain both Unix credentials, setting both
identities to their invalid sentinel if credential lookup fails. Message
extraction uses `imsgbuf_get`, matching the production compatibility library's
current API. Fork authentication message IDs and IPC version 8 remain in
`tmux-protocol.h`. The upstream optional jemalloc diagnostic is included;
this build does not enable or validate jemalloc.

`tests/test-proc.c` compiles the production peer implementation and links the
actual imsg, allocation, logging and credential sources. Four cases use real
Unix sockets and the libevent dispatcher: current uid/gid plus authentication
public-key message and descriptor transfer, rejection of a mismatched IPC
version before authentication dispatch with a version reply, disconnect
notification, and invalid uid/gid on a non-socket descriptor. Bounded polling
prevents a missing event from hanging the suite. Unused process startup and
daemonization functions are discarded by the linker; this is IPC transport
validation, not end-to-end authentication or daemon lifecycle validation.

Both Linux host and relay SIXEL builds pass and contain the new group accessor.
All 178 standard tests pass (174 existing plus four IPC cases); the four new
cases also pass AddressSanitizer and UndefinedBehaviorSanitizer with leak
detection. Existing relay unchecked-write warnings remain. Full 3.8 header,
event/pane lifetime and floating protocol integration, authenticated viewer
validation, macOS and stale wiring-baseline reconciliation are outstanding.
Runtime still reports the 3.6a core. This stage is local only, with no push,
tag, release or deployment.

## Deferred pane lifetime prerequisite

Pane allocation now starts with one owner reference, and the reference API
matches pinned 3.8. Destruction removes the pane from the lookup tree, marks
it destroyed, tears down modes, closes PTY/pipe descriptors and input contexts,
and cancels resize/sync timers immediately. Screen, options and metadata
freeing waits for the last reference. This prepares retained event payloads;
current notification callers have not migrated. A retained destroyed pane's
window pointer is not a usable target without a separate window reference.
The fork recording callback and synchronized rendering cleanup are preserved.

`tests/test-window-lifetime.c` links all host production objects, with only
`tmux.c`'s entrypoint renamed. There are no lifecycle or cleanup stubs. Three
cases cover two payload-style references, lookup/list removal, real descriptor
closure, input cleanup, queued resize cleanup, cancellation of a production
sync timer and a resize sentinel timer, pane retention across whole-window
destruction, and ordinary unretained multi-pane destruction. The resize timer
sentinel checks cancellation; it does not exercise resize scheduling.

Both Linux SIXEL binaries build and all 181 standard tests pass. The three
lifetime cases also pass ASan/UBSan with leak detection, instrumenting the
production `window.c`, test entrypoint and renamed `tmux.c`; other linked host
objects retain their normal build. Normal host/relay objects and binaries were
restored afterwards. These checks do not establish authenticated relay,
recording, floating protocol or macOS compatibility. Full event payload and
notification migration, header/core reconciliation and end-to-end validation
remain outstanding. Runtime remains 3.6a; this stage is local only.

## Retained event payload prerequisite

`events-payload.c` matches the pinned official 3.8 blob exactly. Both host and
relay source lists now compile it, and `tmux.h` exposes the matching opaque
payload types, callbacks and API. Existing notification callers remain on
`notify.c`; the new event dispatcher and hook subsystem are a later stage.
Winlink session ownership was already initialized on all production allocation
paths and needed no change.

`tests/test-event-payload.c` links the complete production host with only its
entrypoint renamed. Four cases exercise typed values and format expansion,
private format exclusion, owned pointer replacement/free callbacks, repeated
pane-item replacement, retention across whole-window destruction, inferred
session/window targets, balanced target replacement, dead-session recovery,
and fallback from a removed target pane to the active live pane. Fixture
sessions/windows use production allocators; destruction and queued notification
cleanup also run through production code. These tests introduce no stubs.

Both Linux SIXEL binaries build and all 185 standard cases pass. The four new
cases also pass ASan/UBSan with leak detection, instrumenting the payload,
window, session and command-target implementations plus the test and renamed
entrypoint and the reallocarray fallback; other linked host objects retain
their normal build. Native configuration, objects and binaries were rebuilt
from clean state, checked for absence of sanitizer symbols, and the standard
suite passed again. These checks do not validate event dispatch or
notification migration, client payload lifecycle, authenticated relay/viewer
execution, floating protocol or macOS. Runtime still reports 3.6a, and stale
wiring-baseline reconciliation remains outstanding. This stage is local only,
with no push, tag, release or deployment.

## Event dispatcher prerequisite

`events.c` matches pinned official 3.8 exactly and is compiled into both host
and relay. The matching opaque sink and callback API is exposed in `tmux.h`.
Existing notification callers still use `notify.c`; registering production
hook/monitor sinks and switching notification callers remain separate stages.

Four complete-host-linked cases in `tests/test-events.c` use real dispatcher,
payload and pane lifecycle code, with only the program entrypoint renamed.
They verify event-name matching and ordered delivery, payload consumption with
and without sinks, self-removal and pending-sink removal during delivery,
idempotent removal while deferred, nested dispatch, generation boundaries for
newly registered sinks, and removal remaining effective across nested events.
The production pane wrapper is exercised while its callback removes the pane;
retained pane/window references balance when dispatch consumes the payload.
No dispatch, allocation or lifecycle stubs are used.

Both Linux SIXEL binaries build and all 189 standard tests pass. The four new
cases also pass ASan/UBSan with leak detection, instrumenting `events.c`,
`events-payload.c`, `window.c`, the test and renamed entrypoint; other linked
production objects retain their normal build. Normal objects/binaries were
restored and checked for absence of sanitizer symbols, then the full standard
suite passed again. These checks do not establish notification/hook migration,
client/session/winlink wrapper behavior, authenticated relay/viewer execution,
floating protocol or macOS compatibility. Runtime remains 3.6a. This stage is
local only, with no push, tag, release or deployment.
