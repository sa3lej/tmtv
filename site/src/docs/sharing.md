---
title: Sharing options
nav: Sharing options
order: 3
description: Every tmtv sharing option - web sharing, web input, passwords, session names, recording, expiry, the shared clipboard, status bar and format variables.
updated: 2026-11-13
---

# Sharing options

Settings go in `~/.tmtv.conf`, which tmtv reads like a tmux configuration file.
Web sharing, web input, recording and the shared clipboard can also be changed
in a running session by putting `tmtv` in front of the line, for example
`tmtv set -g tmtv-recording on`.

## All options at once

```
# ~/.tmtv.conf

# Web sharing (off by default; SSH is always on)
set -g tmtv-web-sharing on

# Let read-write web viewers type (off by default)
set -g tmtv-web-input on

# A memorable name instead of a random token
set -g tmtv-session-name demo

# Password for all viewers, SSH and web
set -g tmtv-session-password secret

# Shared clipboard (on by default; read-only viewers never get it)
set -g tmtv-shared-clipboard on

# Record the terminal to an asciinema .cast file
set -g tmtv-recording on

# Session lifetime in seconds (0 means no limit)
set -g tmtv-link-ttl 3600
```

## Web sharing

SSH sharing is always on. Browser sharing is off until you enable it:

```sh
tmtv set -g tmtv-web-sharing on
# [tmtv] web session: https://tmtv.se/s/TOKEN
```

Browser viewers see the same screen as SSH viewers, drawn with xterm.js, and
can join late and still see the current state.

## Web input

By default browser viewers can only watch, although viewers on the printed
link do receive the [shared clipboard](#shared-clipboard). To let them type:

```sh
tmtv set -g tmtv-web-input on
```

After that, anyone with the printed web link can type. The viewer's title bar
shows whether a session is "interactive" or "view-only".

## Passwords

```sh
tmtv -p secret
```

or `set -g tmtv-session-password secret` in the config (up to 128
characters). SSH viewers must authenticate with the password; a public key
alone is rejected. Browser viewers get a password prompt before the terminal
loads.

The relay receives the password and keeps only a salted PBKDF2-SHA256 hash in
memory. Browsers send it in the stream request's URL and keep it for the tab's
session, so on a self-hosted relay do not log query strings at your proxy.

## Session names

```
set -g tmtv-session-name demo
```

gives links like these:

- `ssh 0123456789abcdef0123456789abcdef-demo@tmtv.se` (read-write; the
  32-character prefix is random)
- `ssh ro-demo@tmtv.se` (read-only)
- `https://tmtv.se/s/demo` (web; counts as read-write)

Names are 3 to 32 letters, digits or hyphens. A name is easy to guess and is
not a secret: anyone who guesses it can watch over SSH and in the browser, and
can type in the browser if web input is on. Add a password when that matters.

The read-only and web links stay the same when you restart tmtv with the same
name, so people who had them can rejoin. Names are not reserved: while your
host is offline someone else can use the name, and your host then gets a
suffixed name such as `demo-1`. tmtv warns you when your read-write link
changes.

## Recording

```sh
tmtv set -g tmtv-recording on
```

Recordings are saved to `~/.tmtv/recordings/` in asciinema `.cast` format. Play
them back with `asciinema play ~/.tmtv/recordings/demo-1741737600.cast`.

A key binding to toggle recording with `prefix + R`:

```
bind R if -F '#{==:#{tmtv-recording},1}' \
  'set -g tmtv-recording off; display "Recording stopped"' \
  'set -g tmtv-recording on; display "Recording started"'
```

## Expiry

```
set -g tmtv-link-ttl 3600
```

The relay ends the session once that many seconds have passed since it started,
checking every 30 seconds. The host is told the expiry time when it connects.
A host that is still running may reconnect afterwards, so stop tmtv yourself
when access must end. With an expiry set, the printed web link uses `/j/`
instead of `/s/`.

## Shared clipboard

On by default: what the host copies reaches read-write SSH viewers and
read-write browser viewers, including browser viewers who cannot type because
web input is off. Read-write viewers can copy back to the host (from a browser
only with web input on). Read-only viewers never get clipboard access. Turn it off with
`tmtv set -g tmtv-shared-clipboard off`.

## Reconnects and restarts

If the connection to the relay drops, the same running host reconnects with
exponential backoff and keeps its SSH and web links, unless another host took
its session name meanwhile. Detaching and reattaching also keeps them.

Ending the host process (exit every shell or `tmtv kill-server`) and starting
a new one creates new random tokens and a new read-write SSH link; nothing is
saved to disk for resuming. With a session name, the read-only and web links
come back the same if the name is free.

## Status bar and format variables

tmtv puts viewer counts at the start of the tmux status bar:

```
S:2 W:1 "myhost" 14:30 09-Mar-26
```

These tmux format variables are available:

| Variable | Value |
|----------|-------|
| `#{tmtv_ssh}` | SSH command (read-write) |
| `#{tmtv_ssh_ro}` | SSH command (read-only) |
| `#{tmtv_web}` | Web viewer URL |
| `#{tmtv_session_name}` | Session name, if set |
| `#{tmtv_ssh_viewers}` | Connected SSH viewers |
| `#{tmtv_web_viewers}` | Connected web viewers |

For example `tmtv display-message -p '#{tmtv_web}'` prints the web link. To
change the status line, set `status-right` in `~/.tmtv.conf`:

```
set -g status-right 'S:#{tmtv_ssh_viewers} W:#{tmtv_web_viewers} "#{=21:pane_title}" %H:%M %Y-%m-%d'
```

## Using tmtv as tmux

tmtv is a full tmux 3.8, so tmux commands work as usual:

```sh
tmtv new-session -s work
tmtv split-window -h
tmtv attach -t work
```

## Per-user input

Programs running inside a session can connect to the Unix socket named in
`TMTV_INPUT_SOCKET` to receive join and leave events and per-viewer keystrokes,
for multiplayer games, chat or collaborative tools. See the
[input socket protocol](https://github.com/sa3lej/tmtv/blob/develop/docs/input-socket.md).
