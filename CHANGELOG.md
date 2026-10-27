# Changelog

User-facing changes are grouped by release. The canonical publication history and
release assets are on [GitHub Releases](https://github.com/sa3lej/tmtv/releases).

## 3.0.0 — 2026-10-27

### Changed

- Move the terminal core to pinned tmux 3.8 sources.
- Introduce sharing protocol 11, requiring a coordinated host and relay upgrade.
- Transfer richer layout and terminal state in bounded snapshots, including
  supported floating panes, linked-window state, hyperlinks and SIXEL images.
- Encode Unicode input portably and send UTF-8 paste as literal bytes, respecting
  the receiving application's bracketed-paste setting.
- Report unsupported live sharing layouts explicitly instead of leaving viewers
  out of sync.

### Fixed

- Require successful SHA-256 verification before installing downloaded binaries;
  missing, invalid or ambiguous checksums and hash-tool failures now stop installation.

- Reject unsafe reconnect snapshot metadata atomically.
- Preserve valid full-width copy-mode cursor and selection positions across
  ordinary updates and reconnect snapshots.
- Wait for the browser image decoder before replaying SIXEL output.
- Preserve Darwin allocator initialization and link its C++ support for release builds.

### Release tooling and documentation

- Add develop validation and separate release publication from production deployment.
- Add a redesigned website, release log, upgrade guidance and current stable version labels.
- Refresh compatible website dependencies; the remaining dependency audit findings
  are documented in `docs/releasing.md` and are not claimed resolved.

See the [3.0 release notes](docs/releases/v3.0.0.md) for behaviour, limits,
upgrade interruption, validation limitations and rollback guidance.

## 2.0.4 — 2026-09-06

- Retain sharing credentials across automatic reconnects of the same running host.
  Full host restarts create fresh credentials; named-session collisions can still
  change links.

See the [published 2.0.4 notes](https://github.com/sa3lej/tmtv/releases/tag/v2.0.4)
and [earlier releases](https://github.com/sa3lej/tmtv/releases).
