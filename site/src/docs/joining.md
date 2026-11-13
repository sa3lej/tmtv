---
title: Joining a tmtv session
nav: Joining
order: 4
description: How to join a tmtv session over SSH or in a browser, what read-only means, how to leave, and what to check when a link does not work.
updated: 2026-11-13
---

# Joining a session

You do not need tmtv to join. The host sends you an `ssh` command, a web link,
or both.

## Over SSH

Run the command you were sent:

```sh
ssh TOKEN@tmtv.se       # can type
ssh ro-TOKEN@tmtv.se    # can only watch
```

If the host set a password, SSH asks for it. A read-only guest can leave
cleanly with `Ctrl-Q`.

## In a browser

Open the link you were sent, such as `https://tmtv.se/s/TOKEN`, or paste the
link, token or session name into the form at [tmtv.se/#join](/#join). If the
host set a password, the page asks for it before showing the terminal.

Browser viewers can only type if the host has turned on web input. The title
bar shows "interactive" or "view-only". Viewers on the host's printed web link
also receive what the host copies.

## Read-only and read-write

- Read-write guests can type in the host's terminal and share its clipboard.
- Read-only guests see everything but cannot type and never get the clipboard.

## When a link does not work

- **The session ended.** Links stop working when the host's tmtv server ends.
  If the host restarted tmtv, ask for the new links.
- **Web sharing is off.** A browser link only works after the host runs
  `tmtv set -g tmtv-web-sharing on`. SSH links work regardless.
- **Wrong relay.** A link for a self-hosted relay must be opened on that
  relay's address, not on tmtv.se.
- **Version mismatch.** The public relay runs tmtv 3.0, so the host needs a
  3.0 client. Guests never need to upgrade.
