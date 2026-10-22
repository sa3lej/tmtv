# Isolated relay and browser checks

These opt-in Linux checks package the local production host and relay into a
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
matching Chromium outside the checkout. Set `NODE_PATH` to its `node_modules`
directory. Build the production viewer with `npm ci && npm run build` in `site`.
Then run:

```sh
NODE_PATH=/path/to/browser-deps/node_modules \
make -C tests relay-browser-e2e PYTHON=/path/to/venv/bin/python \
  E2E_ARGS='--artifacts /path/to/private/browser-artifacts'
```

`--web-root /path/to/site/dist` permits using an already built viewer from another
worktree. Explicit `--host-binary` and `--relay-binary` paths are also supported.
Before container startup, the runner copies local Chromium, Node, Playwright and
fonts and fetches only the exact pinned public jsDelivr assets in the built HTML.
The browser itself runs offline. External fonts are blocked; DejaVu is the test
fallback. Chromium's own sandbox is disabled inside this disposable container.
There is no remote debugging port or Docker socket mounted into it.

A test-only HTTP server serves the unmodified built viewer and proxies its SSE,
HEAD and input requests to the real loopback relay. Local control endpoints write
to the actual host pane PTY and cut its encrypted TCP transport. Tests assert the
real DOM's computed attributes, OSC 8 link activation, decoded SIXEL canvas pixels,
and restoration through reconnects, plus read-only input rejection. They do not
substitute a terminal parser, renderer, MessagePack codec or EventSource. Passive
terminal observation is restricted to the test page. Screenshots and browser logs
are retained under the artifact directory's `browser/` subdirectory.

This is Chromium/Linux coverage; it does not establish Firefox, WebKit, native
terminal capability discovery or macOS support. The ordinary pyte transport suite
remains separate and can be run without browser dependencies.

The browser tests delay real WebAssembly initialization by 500 ms to reproduce
first-replay image loss, and exercise the renderer's bounded startup failure.
The production viewer gates its VT queue on the pinned image addon's decoder;
addon-image 0.9.0 exposes no public readiness promise, so that compatibility shim
uses its private handler reference. Revalidate it when changing the CDN pin.
OSC 8 activation uses Shift-click to bypass terminal application mouse reporting.
Small viewports keep the minimum font size and allow horizontal scrolling when
all columns cannot fit. Browser fixtures allow 768 MiB and 256 processes/threads;
the ordinary transport fixture keeps its 512 MiB/128 limits.
