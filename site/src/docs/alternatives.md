---
title: tmtv compared with tmate, Upterm, ttyd and others
nav: Alternatives
order: 7
description: How tmtv relates to tmate, Upterm, ttyd, asciinema, Tuple and plain tmux over SSH, and when another tool is the better choice.
updated: 2026-11-13
---

# Alternatives

Terminal sharing is well served already. This page describes how tmtv relates
to other tools and when you should use something else. Claims about other
projects come from their own documentation as of October 2026; check the links
for current details.

## What tmtv is

A tmux 3.8 with sharing built in: separate read-write and read-only SSH
invitations, an optional browser viewer, recording, and a relay you can run
yourself. It descends from [tmate](https://github.com/tmate-io/tmate), and its
relay builds on tmate's server.

## Other tools

**[tmate](https://github.com/tmate-io/tmate/blob/master/README.md)** is the
established instant terminal-sharing project tmtv grew out of. Its README
reports that its public service shut down in July 2025 and describes a server
rewrite; self-hosting it remains an option. If you use
[action-tmate](https://github.com/mxschmitt/action-tmate) to debug CI,
[action-tmtv](https://github.com/sa3lej/action-tmtv) is a one-line replacement.

**[Upterm](https://upterm.dev/)** shares terminals over SSH and supports
self-hosting, hosting a single command, and authorized-key and identity
integrations. Choose it if you want those identity features rather than a
tmux session.

**[ttyd](https://github.com/tsl0922/ttyd)** shares a terminal through a browser,
with authentication, Unicode/IME and SIXEL support. Choose it if the browser is
the only way in you need.

**[asciinema](https://docs.asciinema.org/)** records terminal sessions, and its
[server can stream live](https://docs.asciinema.org/manual/server/streaming/).
tmtv writes asciinema `.cast` recordings, so the two work together. For
broadcasting a terminal to many people who never type, asciinema streaming is
worth a look.

**[Tuple](https://tuple.app/)** is a pairing application with screen sharing,
remote control, audio and video, and end-to-end encryption. If you need to share
graphical applications, talk while you work, or end-to-end encryption, a tool
like that serves you better than tmtv.

**Plain tmux over your own SSH server** needs nothing new: give a colleague an
account on your machine and attach to the same tmux session. Nothing passes
through a third-party relay, which makes it the right choice when even a
self-hosted relay is too much exposure.

## When tmtv fits

- You live in tmux and want others in the same session without changing your
  setup.
- Guests should join with plain `ssh`, with a clear choice between typing and
  watching.
- Sometimes a browser link is easier for an observer.
- You want the option of running the relay yourself.
