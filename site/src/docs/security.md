---
title: tmtv security model
nav: Security
order: 6
description: What the tmtv relay can see, how tokens, read-only access, passwords and session names work, and how to share a terminal safely.
updated: 2026-11-13
---

# Security

tmtv gives other people access to a terminal on your machine. This page
describes what protects that access and where its limits are.

## What the relay can see

Your shell and programs run on your machine. tmtv sends the terminal to a relay,
which forwards it to your guests. **The relay can read everything shown and
typed in a shared session.** SSH encrypts each connection to the relay, and
browsers use HTTPS, but there is no end-to-end encryption between you and your
guests through the relay.

Use the public relay at tmtv.se only for work you would be comfortable sharing
with its operator. For anything else, [run your own relay](/docs/self-hosting/)
or share over your own SSH server with plain tmux.

## Tokens are credentials

Anyone who has a token can join. Send tokens privately and do not post them
where others can read them. Automatic reconnects of the same running host keep
its tokens. Ending the host process and starting tmtv again creates new random
tokens; with a session name, though, the read-only and web links are the same
again, so people who had them can rejoin.

## Read-only and read-write

- Read-write guests (`ssh TOKEN@...`) can type in your terminal, which means they
  can run any command you can.
- Read-only guests (`ssh ro-TOKEN@...`) cannot type and never receive the shared
  clipboard.
- The printed web link is a read-write link. Browser viewers on it cannot type
  until you turn on `tmtv-web-input`, but they do receive what you copy while
  the shared clipboard is on.

## Passwords

`tmtv -p secret` or `set -g tmtv-session-password secret` requires a password
from every viewer, SSH and web. SSH viewers must authenticate with it; a public
key alone is rejected. The relay receives the password and keeps only a salted
PBKDF2-SHA256 hash in memory. Browsers send it in the stream request URL and
keep it for the tab's session, so a self-hosted proxy should not log query
strings.

## Session names

Named sessions (`tmtv-session-name`) are convenient but not secret. A name is
easy to guess, and the web link `/s/NAME` is read-write: anyone who guesses it
can watch, receives the shared clipboard, and can type if web input is on. The
links for a name also survive restarts, and names are not reserved while your
host is offline. Combine a name with a password. Read-write SSH links for named
sessions keep a 32-character random prefix.

## Limiting exposure

- Turn on web sharing and web input only when you need them.
- An expiry (`tmtv-link-ttl`) makes the relay end the session, but a running
  host may reconnect. When access must end, stop tmtv (`tmtv kill-server`).
- Turn off the shared clipboard with `tmtv set -g tmtv-shared-clipboard off` if
  you copy secrets.
- Do not share a terminal that shows secrets you would not hand to every guest.

## Install integrity

The install script verifies the SHA-256 checksum of the release binary against
the release's `checksums.txt` and installs nothing if the check fails. You can
[read it](/install.sh) first or verify binaries yourself, as described in
[Install](/docs/install/).

## Reporting a vulnerability

Please do not report security problems in a public issue. Contact the
maintainer, [Lars-Erik Jonsson](https://github.com/sa3lej), privately first.
