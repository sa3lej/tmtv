---
title: Getting started with tmtv
nav: Getting started
order: 1
description: Install tmtv, start a shared tmux session, invite people over SSH or the browser, and stop sharing again.
updated: 2026-11-13
---

# Getting started

tmtv is tmux with sharing built in. You run it on your own machine; the people
you invite join over `ssh` or, if you allow it, in a browser. This page takes
you from installing it to ending your first shared session.

## What you need

- A Linux or macOS machine to host the session.
- Guests need an SSH client, or a browser if you turn web sharing on. They do
  not need tmtv.

## 1. Install

```sh
curl -fsSL https://tmtv.se/install.sh | sh
```

On macOS you can use Homebrew instead: `brew install sa3lej/tap/tmtv`. See
[Install](/docs/install/) for binaries, checksums, building from source and
upgrading.

## 2. Start a session

```sh
tmtv
```

tmtv starts tmux, connects to the public relay at tmtv.se and prints two
commands:

```
ssh TOKEN@tmtv.se     # read-write access
ssh ro-TOKEN@tmtv.se  # read-only access
```

## 3. Invite people

Send the first command to someone you want to type with and the second to
someone who should only watch. Anyone who has a token can join, so treat
tokens like passwords and send them privately.

The status bar shows who is connected: `S:2 W:1` means two SSH viewers and
one browser viewer.

## 4. Share in a browser (optional)

Web sharing is off until you turn it on:

```sh
tmtv set -g tmtv-web-sharing on
# [tmtv] web session: https://tmtv.se/s/TOKEN
```

People who open the link can watch without installing anything. They also
receive what you copy, unless you turn off the shared clipboard. To let them
type as well, run `tmtv set -g tmtv-web-input on`; after that anyone with the
printed web link can type.

## 5. Stop sharing

End the tmtv server: exit every shell in it, or run `tmtv kill-server`.
Detaching (`prefix d`) leaves the session running and shared.

Starting tmtv again creates new random tokens. If you use a
[session name](/docs/sharing/#session-names), the read-only and web links for
that name work again, so anyone who had them can rejoin. If only the network
drops, the same running host reconnects on its own and keeps its links.

## Next

- [Sharing options](/docs/sharing/): passwords, names, recording, expiry, the
  clipboard and status bar variables.
- [Joining a session](/docs/joining/): what guests see and how to leave.
- [Security](/docs/security/): what the relay can see and how to share safely.
- [Self-hosting](/docs/self-hosting/): run your own relay.
