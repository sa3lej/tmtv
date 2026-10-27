# tmtv 3.0: market research and launch positioning

Research date: 27 October 2026. This is a review of public product documentation,
not customer interviews, market-share research or proof of product-market fit.
Product claims should be checked again at publication time.

## The position to earn

**Your terminal. Better together.**

Open-source terminal sharing for developers, operators and teachers who want
someone else in the same working session. SSH provides a familiar way to join;
optional browser access helps observers participate without a local installation.
The 3.0 release story is a modern tmux core with more faithful shared state and
Unicode input, supported by explicit access controls and candid upgrade guidance.

Generic terminal sharing is already well served. The useful proposition is the
combination of the tmux workflow, SSH and browser invitations, and attention to
what survives transport and reconnects. None of those individual features should
be advertised as an industry first.

## Alternatives and what they teach us

| Product | Evidence from its own documentation | Implication for tmtv |
| --- | --- | --- |
| [tmate](https://github.com/tmate-io/tmate/blob/master/README.md) | An established instant terminal-sharing project. Its current README reports the public service shutdown in July 2025 and describes a server rewrite; self-hosting remains relevant. | Honour the lineage. Explain our own maintained release and tmux baseline; do not call tmate dead or unmaintained. |
| [Upterm](https://upterm.dev/) | SSH-based terminal sharing, self-hosting, command hosting and authorized-key/identity integrations. | SSH access alone is not differentiation. Show the tmux session and optional browser experience; do not imply competitors lack access controls. |
| [ttyd](https://github.com/tsl0922/ttyd) | Shares a terminal through a browser and documents Unicode/IME, SIXEL and authentication features. | Browser access and graphics are not unique. Explain how a host invites SSH and web participants into its tmtv workflow. |
| [asciinema](https://docs.asciinema.org/) | Records and shares terminal sessions; its [server supports live streaming](https://docs.asciinema.org/manual/server/streaming/). | Treat it as both an ecosystem companion and an alternative for broadcasting. Never describe it as recording-only. |
| [Tuple](https://tuple.app/) | A pairing product with desktop screen sharing, remote control, audio/video and end-to-end encryption. | For GUI work and calls, a screen-sharing tool serves a different need. Do not suggest tmtv supplies those features or equivalent end-to-end encryption. |

## Audiences and proof

| Audience | Problem to lead with | Demonstration to capture from the final candidate |
| --- | --- | --- |
| Developers and maintainers | “Can you look at this command with me?” | Start a host, join over SSH, take turns typing, resize a split layout. |
| Operations teams | “Follow along without changing the session.” | Invite a read-only observer and show that attempted input is rejected. Use a disposable example, not production secrets. |
| Teachers and presenters | “Let everyone see the terminal clearly.” | Explicitly enable web sharing, join in a browser, then record a short workflow as an asciinema cast. |
| Existing tmtv users | “What improves, and what will the upgrade disrupt?” | Show Unicode input and a reconnect restoring supported state; explain coordinated relay cutover and fresh links after restart. |

The homepage prioritizes the first successful session: understand the product,
install the current release, invite a guest. A separate join form serves people
who already have an invitation. The interactive terminal on the page is explicitly
an illustration, not fabricated evidence of a live product session.

## Claims to use and claims to avoid

Use concrete wording: optional web viewer, separate read-only/read-write tokens,
self-hostable relay, tmux 3.8 in the upcoming release, bounded supported snapshots,
Unicode scalar input, literal UTF-8 paste.

Avoid unsupported speed or bandwidth comparisons, adoption counters, testimonials,
“the most secure,” “all terminal state,” “unlimited history,” and guarantees that
links survive every restart. The relay processes session data. Browser typing
requires both a write-capable token and the host's input opt-in. Read-only viewers
do not receive shared clipboard access.

The new website distinguishes the published stable release from the upcoming
candidate. Recording, browser access, clipboard support and reconnect identity
are existing capabilities; do not present them all as inventions of 3.0.

## Proposed launch sequence

This is a publication proposal, not permission to post or deploy.

1. Finish final-candidate validation and operator rollout preparation. Record native
   Safari, deployed staging, the remaining robustness review and dependency findings
   accurately. Keep publication and production cutover separate.
2. Prepare a 30–45 second recording using synthetic commands and disposable tokens:
   host → SSH collaborator → browser observer → Unicode example. Capture real output;
   avoid a cinematic mockup masquerading as a recording.
3. Publish the GitHub release with the upgrade warning near the top, verified assets
   and checksums. Update website version labels only after the relevant publication
   and public-service state are verified.
4. Announce the production relay cutover and explain exactly when hosts should switch.
   A website preview can precede this without changing its stable installer label.
5. After the release works end to end, share a short, technical demonstration with
   relevant tmux/open-source/operations communities. Consider Show HN using a concrete
   demonstration and candid limitations. Follow each community's rules; no mass posting.

## What would count as traction

Suggested evaluation questions, not installed tracking: can a newcomer understand
who installs what; can they reach a first shared session; do observers understand
read-only access; do upgraders understand the host/relay requirement? Ask a few
people outside the project to perform those tasks and note where they hesitate.

If aggregate release download counts and voluntarily reported successful sessions
are available, compare them after launch. Downloads are not active users. No analytics,
telemetry or third-party tracking has been added by this preparation work.
