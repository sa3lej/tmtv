# Isolated relay and browser checks

These Linux checks run in `.github/workflows/relay-e2e.yml`, called by the Build
workflow for develop, staging and release tags. They can also be run locally
with the commands below. They package the local production host and relay into a
disposable Docker container. Its network is disabled, root filesystem read-only,
resources bounded, and its only writable volume temporary. No production server,
SSH configuration, credentials, published port or host directory is used.
`SYS_ADMIN` is necessary for the relay's actual session jail; there is no test
bypass. All fixture keys/tokens are generated inside the container and discarded.

Install `requirements-relay-e2e.txt` in a virtualenv outside the checkout. Run:

```sh
make -C tests relay-e2e PYTHON=/path/to/venv/bin/python \
  E2E_ARGS='--artifacts /path/to/private/artifacts'
```

For actual browser rendering, install Playwright (tested with 1.59.1) and its
matching browsers outside the checkout (`playwright install chromium firefox webkit`).
Set `NODE_PATH` to its `node_modules`
directory. Build the production viewer with `npm ci && npm run build` in `site`.
Then run:

```sh
NODE_PATH=/path/to/browser-deps/node_modules \
make -C tests relay-browser-e2e PYTHON=/path/to/venv/bin/python \
  E2E_ARGS='--artifacts /path/to/private/browser-artifacts'
```

`--web-root /path/to/site/dist` permits using an already built viewer from another
worktree. Explicit `--host-binary` and `--relay-binary` paths are also supported.
Select `--browser-engine firefox` or `--browser-engine webkit` in `E2E_ARGS` to
run the same checks in those engines; the default is Chromium. Use a separate
artifact directory for each run. The runner requires the browser revision that
matches the installed Playwright package and prints its actual version.
Before container startup, the runner copies the selected browser, Node, Playwright
and fonts and fetches only the exact pinned public jsDelivr assets in the built HTML.
The browser itself runs offline. External fonts are blocked; DejaVu is the test
fallback. Chromium's own sandbox is disabled inside this disposable container.
There is no remote debugging port or Docker socket mounted into it.
WebKit additionally needs installed Mesa EGL/software rasterizer libraries and
the GLVND Mesa configuration, which the runner copies into the container. Browser
library symlinks are preserved to avoid loading aliased libraries twice.

A test-only HTTP server serves the unmodified built viewer and proxies its SSE,
HEAD and input requests to the real loopback relay. Local control endpoints write
to the actual host pane PTY and cut its encrypted TCP transport. Tests assert the
real DOM's computed attributes, OSC 8 link activation, decoded SIXEL canvas pixels,
and restoration through reconnects, plus read-only input rejection. They do not
substitute a terminal parser, renderer, MessagePack codec or EventSource. Passive
terminal observation is restricted to the test page. Screenshots and browser logs
are retained under the artifact directory's `browser/` subdirectory, including
`launch.log` with browser process diagnostics.

These are Linux engine checks; WebKit here does not establish native Safari,
macOS, iOS or native terminal capability discovery support. The ordinary pyte
transport suite remains separate and can be run without browser dependencies.

The browser tests delay real WebAssembly initialization by 500 ms to reproduce
first-replay image loss, and exercise the renderer's bounded startup failure.
The production viewer gates its VT queue on the pinned image addon's decoder;
addon-image 0.9.0 exposes no public readiness promise, so that compatibility shim
uses its private handler reference. Revalidate it when changing the CDN pin.
OSC 8 activation uses Shift-click to bypass terminal application mouse reporting.
Small viewports keep the minimum font size and allow horizontal scrolling when
all columns cannot fit. Browser fixtures allow 768 MiB and 256 processes/threads;
the ordinary transport fixture keeps its 512 MiB/128 limits.

## Native host validation

After building the production host with SIXEL and utf8proc enabled, run the
portable host subset on the actual target system:

```sh
python3 tests/run-native-host-tests.py --expected-arch arm64 \
  --artifacts /path/outside/checkout/native-host
```

Use `x86_64` for Intel, or omit the architecture assertion for a local smoke run.
The runner records the real OS/architecture, source commit and dirty state,
production binary hash, exit status and log hash. It isolates the normal user
configuration and uses a short temporary directory for Unix sockets. The host
subset exercises full-object mode/layout/snapshot/parser/media fixtures, pane
lifetimes, event/control/command paths and actual host socket commands. It excludes
Linux relay/jail tests; those remain covered by the Linux `all` target and isolated
relay fixture. No installation, publication or deployment occurs.

The Darwin configure path requires jemalloc by default. These builds install
and enable it explicitly, including its archive in static release linkage;
utf8proc and SIXEL remain enabled.

Develop validation has separate native Apple Silicon and Intel jobs. Release
packaging uses the same host subset instead of trying to link Linux relay tests
on Darwin. Build additionally validates static release packaging and all three Linux browser engines.

This runner does not establish Safari support. Native Safari still needs an
actual macOS browser session against an isolated Linux relay and production
viewer: verify styled text and OSC 8 activation, SIXEL pixels, host disconnect/
reconnect and reload, saved/alternate-screen images, small viewports and read-only
input rejection, matching the Linux browser fixture. Playwright WebKit on Linux
or macOS is not Apple's Safari binary. Retain exact OS/browser versions and
logs/screenshots, and keep missing native coverage explicit.
