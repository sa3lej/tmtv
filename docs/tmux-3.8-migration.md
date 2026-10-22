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

## Command queue disconnect prerequisite

The `cmdq_get_cmd` accessor and early `CLIENT_DEAD` rejection in
`cmdq_fire_command` match pinned official 3.8 exactly. The guard runs before
logging, target lookup, command execution and the fork's relay-forwarding
branch. Existing tmtv routing remains in place. Hook/monitor integration and
broader command-queue changes remain separate stages.

Three cases in `tests/test-cmd-queue.c` link the complete production host,
renaming only its entrypoint. They parse real `set-option` commands and drain
production queues, verifying live-client/global execution, command identity,
disconnect before execution, grouped cancellation, disconnect during queue
draining, cleanup callbacks and balanced queue-held client references. Client
fixtures own production queues and environments; no parser, execution,
allocation or reference-cleanup stubs are used. Both disconnect cases failed
before the guard because the commands still changed options.

Both Linux SIXEL binaries build and all 192 standard tests pass. The three
new cases also pass ASan/UBSan with leak detection, instrumenting
`cmd-queue.c`, the test and renamed entrypoint; other host objects keep their
normal build. Normal objects and binaries were restored, checked for absence
of sanitizer symbols and the full suite passed again. These tests do not
exercise actual transport disconnection, authenticated relay forwarding,
viewers or macOS. Runtime remains 3.6a; floating protocol and stale wiring
validation remain outstanding. This stage is local only, with no push, tag,
release or deployment.

## Option hook firing metadata prerequisite

The firing count/time fields, accessors and exact `options_search` lookup are
copied from pinned 3.8 `options.c`. Existing `notify.c` and `cmdq_insert_hook`
paths account once after resolving an option, before parsing/inserting its
commands, matching pinned `hooks_insert` semantics. Inherited hooks update the
owning entry; local entries have independent metadata. Updating a value retains
metadata; removing/recreating the entry resets it. This stage preserves the
existing numeric array API and fork relay command routing.

Four cases in `tests/test-options-hooks.c` link the complete production host
with only the program entrypoint renamed. They exercise immediate custom hook
execution, deferred window notifications using production window/pane lifetime
APIs, multi-entry command hook execution and `CMDQ_STATE_NOHOOKS` suppression,
parent/local metadata and removal reset, and exact table lookup. Removing both
production accounting calls made three integration cases fail; the API and
inheritance case still passed. The fixtures initialize the production event
base, socket identity, option tables and environment; there are no production
parser, queue, notification or options stubs.

Linux SIXEL host and relay builds and all 196 standard tests pass. All four
cases also pass ASan/UBSan with leak detection, instrumenting `options.c`,
`notify.c`, `cmd-queue.c`, `window.c`, the test and renamed entrypoint; other
linked host objects retain normal compilation. Leak detection initially found
562 bytes in 16 allocations: custom notification hooks retained parser-owned
command lists after queue insertion took its own references. Releasing the
parser reference after insertion fixes that production leak while the tests
still verify hook commands execute. Normal objects/binaries are restored for
the final suite; sanitizer symbols are checked separately.

This does not port monitor ownership/lifecycle: `options_get_monitor_data`,
`options_set_monitor_data` and `options_remove` monitor cleanup require the
actual hooks/monitor modules. Reconcile those dependencies next, including the
same custom-hook parser ownership when importing `hooks.c`. Event hook sinks
are not registered and existing notify callers are not replaced in this stage.
No full 3.8 runtime, authenticated relay/viewer, macOS or floating-pane
validation is claimed. Remote pushes, tags, releases and deployment remain held.

## Monitor ownership and hook lifecycle prerequisite

`monitor.c` matches pinned official 3.8 exactly; both Linux host and relay now
compile it and `hooks.c`. Option-owned monitor data and removal cleanup match
the pinned APIs. The only differences from pinned `hooks.c` release the parser
reference after queue insertion in both custom and expanded array hooks, and
free expanded hook parse errors. This preserves the previously verified custom
hook leak fix. Existing notify callers and command-hook routing are unchanged;
startup does not yet register the new built-in hook sinks or expose monitor
command syntax.

Five cases in `tests/test-monitor.c` link the complete production host with
only its entrypoint renamed. Bounded real libevent timers exercise monitor
format evaluation, custom hook parsing, event payload dispatch, queue execution,
initial notification, change detection, false-value filtering and firing
metadata. Lifecycle cases verify replacement balances session references,
inherited monitor ownership survives local removal, option removal and options
free cancel timers/sinks, and destroyed sessions remain retained safely until
an external monitor is destroyed. Pane/window scans and same-name subscription
replacement use real production session/window/pane allocators.

Review found a reference cycle when session options own a monitor that retains
that same session: deferred `session_free` could never reach option cleanup.
`session_destroy` now removes its local option monitors before dropping its
owner reference. An external monitor anchor lets the regression inspect the
remaining reference and cleared option state before final deferred freeing.
Linking the prior production `session.c` makes only that regression fail
(expected one reference, observed two). External monitor retention remains
unchanged. Existing notification and fork recording teardown paths are kept.

Both Linux SIXEL builds and all 201 standard cases pass. The five new cases
also pass ASan/UBSan with leak detection, instrumenting monitor, hooks, options,
events, payload, window, session, command queue, test and renamed entrypoint;
other linked host objects retain normal compilation. Sanitized objects/binary
are isolated under `.deps/tmux38-monitor-validation`; normal production objects
were never replaced. Symbol inspection confirms no sanitizer references in
normal host, relay and monitor test binaries. The fixture drains deferred
session frees through the production event loop; its initial omission produced
a 389-byte leak report and was corrected before final validation.

These checks do not cover client control subscriptions, single-pane monitors,
expanded built-in array monitor hooks, mutation of subscriptions from within a
callback, authenticated relay/viewers or macOS. Broader core/header migration,
hook sink registration/notify caller replacement, floating layout/protocol,
recording/clipboard/SIXEL/synchronized-rendering end-to-end and stale wiring
validation remain outstanding. Runtime still reports 3.6a. Local commits only;
remote pushes, tags, release and deployment remain held.

## Control subscriptions through the shared monitor API

Control mode now creates a client-owned monitor set and uses the pinned 3.8
`control_add_sub`/`control_remove_sub` adapters. The old per-control-client
subscription trees, cached values and timer are removed. The subscription
change callback preserves the session/window/pane output shapes, using the
existing `control_write` path until the broader notification guard migration.
`control_stop` destroys the monitor before freeing transport state and adopts
the pinned null-state guard and pointer clearing for repeated stop calls.

The `refresh-client -B` helper matches pinned 3.8 exactly and uses
`monitor_parse`. Known session (`name::format`), pane, all-pane, window and
all-window selectors remain supported; unknown selectors no longer silently
become session subscriptions. A bare name removes its subscription. Other
refresh options, fork clipboard behavior, command routing and existing
read-only gates are unchanged.

Four cases in `tests/test-control-subscriptions.c` link the entire production
host with only its entrypoint renamed. Real socket output and bounded libevent
timers verify exact subscription lines, initial/change notifications,
unchanged-value suppression, notification of false values, same-name
replacement, removal, all-pane/all-window scans, single-window session
isolation, following the client's new session, detached-session suppression,
and stop cancellation of a pending timer. The single-pane case sends the
actual refresh command through the control input socket, production parser
and queue, then verifies a closed pane is ignored. Repeated stop clears client
state safely. Replacing only the refresh helper with its previous parser
(adapting enum names) fails the unknown-selector case and passes the other
three cases.

Linux SIXEL host and relay builds succeed. All 205 standard cases pass
(198 C/shell and seven Python). The four control cases also pass ASan/UBSan
with leak detection using isolated instrumented control, refresh, command
parser, format, monitor, hooks, options, events, event payload, window, session,
command queue, test and renamed entrypoint objects; other host objects retain
normal compilation. Normal production objects are never replaced. Logs,
negative-control source/binary and sanitizer reproduction script are under
`.deps/tmux38-control-validation`. Normal host, relay and control test have no
sanitizer references. The existing relay build warnings for unchecked `write`
results remain; this stage introduces no compiler errors.

This stage does not exercise authenticated relay or SSH/web clients, read-only
command rejection, subscription mutation from callbacks, or macOS. Broader
pinned control I/O changes (notification guards, reply backpressure, flushing
and per-window sizes) remain unported. Monitor command syntax, string-key
option arrays, expanded built-in monitor hooks and startup sink registration/
notify replacement remain ahead, alongside the full core/header reconciliation,
floating-pane protocol and rendering/input integration. Both built runtimes
still report tmux 3.6a. Remote push, tags, release and deployment remain held.


## Monitor hook commands and custom events

The `set-hook` parser and the `-B` monitor and `-E` event command helpers
match pinned official 3.8 exactly. Monitor commands now traverse the real
command queue and target/scope resolution before creating option-owned monitors.
The command suppresses initial delivery, supports `-T` false-value filtering,
replacement without a new command, append and bare-name or full-subscription
removal. Ordinary custom `set-hook @name` registers its event sink; `set-hook -E`
fires a payload with the resolved client/session/window/pane target. Event hook
commands use their usual command-level format expansion, such as `set-option -F`.

This bounded port retains numeric option arrays. Fork option replication,
recording toggles and command routing/read-only gates are unchanged. Existing
helper branches for `-F` and `-o` remain upstream code, but this stage does not
extend the pinned `set-hook` argument flags to expose them. Built-in hook
startup registration and notification caller replacement moved to the next
stage.

Five added cases in `tests/test-monitor.c` use complete production-host linkage
with only the tmux entrypoint renamed. Actual parser/queue execution and Unix
socket-backed control replies verify invalid subscription/name/arity errors,
unchanged reference ownership on rejection, session/global/pane/window scopes,
no initial delivery, changed and false values, replacement/append/removal,
option-owned cleanup, custom event execution and target metadata. A legacy
numeric built-in hook exercises indexed assignment/removal and `-R` execution;
firing accounting is checked as a delta because intentional invalid commands
also fire `command-error`. The five earlier monitor ownership/timer cases remain.
All ten monitor cases pass. A negative control ignoring `-T` fails its false-value
regression (expected zero fires, observed one).

Both Linux SIXEL production binaries build and all 210 standard cases pass
(203 C/shell and 7 Python). All ten complete-host monitor cases also pass
ASan/UBSan with leak detection using isolated command, parser, format, monitor,
hooks, options, events/payload, control, window, session, queue, test and renamed
entrypoint objects; other linked objects are normal. Normal production objects
were never replaced, and normal host/relay/monitor test binaries have no
sanitizer references. Logs and isolated reproducer scripts are under
`.deps/tmux38-hook-command-validation`. Diagnostic fixture corrections initialize
and free the server message log, use `set-option -F` for event formats and compare
legacy hook count deltas. The final suite and sanitizer checks ran after those
corrections. This stage introduced no compiler warnings; the three earlier relay
unused-write warnings were not recompiled by this incremental build.

No authenticated relay/viewer, read-only rejection, callback subscription
mutation, expanded built-in monitor hooks or macOS coverage is claimed. Pinned
string-key arrays and monitor display/introspection remain separate command/options
work; wider control I/O, full core/header reconciliation, floating panes/protocol,
security/feature E2E and supported-platform validation remain open. Both runtimes
still report tmux 3.6a. This work is local only; remote pushes, tags, release,
deployment and final release approval remain held.

## Notification event sinks and startup hooks

The pinned 3.8 notifier is now the production path. `hooks_build_events`
registers sinks for every built-in hook option, `control_build_events`
registers the 14 control notification sinks, and `server_start` wires both
before session creation. The obsolete `notify.c` module is gone. The former
direct notification calls now emit event payloads, including paste-buffer
changes and deletions.

`control-notify.c` follows the pinned event-payload callbacks. Its immediate
outbound writer is deliberately a local interim shim to the existing control
transport, so command reply guards preserve current tmtv semantics until
guard-aware notification transport and reply backpressure are ported. Paste
events are routed to `events_fire`, while `tmtv-ready` remains a fork event.
No authenticated relay/viewer or macOS coverage is claimed. Broader control
I/O, floating panes/protocol, full core/header reconciliation, supported
platforms, stale wiring reconciliation and E2E viewer validation remain open.
Both runtimes still report tmux 3.6a. This work is local only; remote pushes,
tags, release and deployment remain held.

## Guard-aware control transport and window sizes

Control notifications now use a dedicated production writer and never
interleave inside queued command reply guards. The transport tracks guard
depth, defers notifications until the outermost block closes, and releases them
in order. Replies share one bounded buffer: a disconnected control client is
marked and discarded at the pinned 64 MiB limit, with `discard-all` used on
forced client exit. Pane and reply output are optimized with contiguous write
ranges.

The 3.8 `refresh-client -C @window:WxH` and clear syntax now owns per-window
sizes in control transport, and resize calculations consume those explicit
sizes through the real production path. Respawned panes rebuild buffers by
resetting pane, control offset and queued offsets; existing control clients
receive the reset before further output. Command parser guards and normal
client exit paths route through the same transport state.

Eight complete-host control subscription cases cover nested-guard preservation
and ordered flushing, stop safety, explicit per-window set/get/clear,
64 MiB reply backpressure with discard and exit message, invalid subscription
rejection, pending-timer safety after stop, and single-pane socket handling.
Four complete-host option-hook cases cover callback hook accounting, arrayed
built-in hooks and `-E` custom event execution. The fresh Linux SIXEL host and
relay builds plus full existing suite pass 216 cases, including seven Python
cases. This stage's eight control cases also passed a fresh ASan/UBSan and leak
detection build; normal production and full-suite test binaries contain no
sanitizer references. The new diagnostic option-hook registration assertion
matches production behavior because direct custom event hooks require an
explicit sink.

No authenticated relay/SSH/web viewer, read-only isolation, reconnect,
recording, clipboard, SIXEL, synchronized rendering or floating-pane E2E is
claimed for this stage. Monitor display scaling and expanded built-in hook hooks,
broader 3.8 core/header reconciliation, negotiated protocol and macOS remain
open. Both runtimes still report tmux 3.6a. This work remains local; remote
push, tags, release, deployment and final release approval are held.

## Callback-safe subscription mutation

Monitor callbacks may now add or remove subscriptions safely. The monitor
scanner takes a bounded pointer snapshot before it invokes callbacks, so
callback-driven mutation of the underlying subscription tree no longer
invalidates the scan or frees an in-flight item. Callback-added subscriptions
are not evaluated in the same scan pass; they are picked up on the next timer
tick.

One complete-host regression in `tests/test-monitor.c` adds and removes
subscriptions from real change callbacks and asserts the deferred item is no
longer reported after its callback removal. This closes the previously
unvalidated callback-mutation path for monitor-owned subscriptions. It does not
change authenticated relay behavior or float/protocol integration. Monitor
scanner safety is the only new behavior in this stage; the other migration
limits listed above still apply. This work is local, untested against a
configured relay deployment, unpushed and undeployed.

## Full UTF-8 cell length

The fork deliberately differs from the pinned 3.8 `utf8.c` packed handle: the
24-bit inline/cache index remains, length gets six bits, and width plus one gets
two bits. Core widths are 0..2; a zero handle retains invalid width 0xff. This
fixes the pinned implementation's 32-byte length spilling into width. Both
host and relay use it; sharing protocol 10 requires corrected peers. Expanded
cells and local tmux IPC are unchanged. Snapshot text accepts all 32 bytes and
rejects larger payloads. Boundary tests cover every length and core width,
31/32-byte grid/snapshot restoration, parser split points and subsequent output.
