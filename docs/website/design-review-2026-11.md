# tmtv.se design review and decisions (November 2026)

The owner found the site "too AI". On 2026-11-13 three independent reviewers, each
with a different perspective, critiqued the live site (develop `cf5dfd39`, byte-identical
to production) from Playwright screenshots at 375×812, 812×375 and 1440×900 and from
the `site/` source:

1. Brand and typography (editorial/print designer)
2. UX, information architecture and accessibility (mobile-first, WCAG 2.2)
3. Developer-tool product craft (documentation-forward open-source CLI sites)

The full reviews are kept with the private project evidence. This file records the findings
they agreed on and how each one is resolved.

## Where the reviewers agree

| # | Finding | Raised by | Resolution |
|---|---------|-----------|------------|
| 1 | The hero is a scripted mock-up ("Alex/Sam run `npm test`", labelled *Illustrated demo*). The site never shows real tmtv output. | 1, 2, 3 | Remove `CollaborationDemo`. Lead with the real first-run transcript (`tmtv` → `ssh TOKEN@tmtv.se`) taken from the README, and later a real host/guest capture. |
| 2 | Template tics: `01 / GET STARTED` kickers, all-caps letter-spaced mono labels, a serif-italic accent word ("*shared.*"), the ✳ ribbon band, rows of three numbered cards, slogan headlines ending in a full stop, ↗/↘ glyphs everywhere. | 1, 2, 3 | Remove all of them. Use sentence case and descriptive headings ("Install", "How sharing works", "Security"). |
| 3 | The copy is slogan-led ("Your shell, your tools, together.") while the useful facts sit in the README. | 1, 3 | Plain first-person README voice: say what a command does and what it prints. |
| 4 | There is no documentation on the site (`/docs/` is 404). Usage is only in a 600-line README behind a footer link. | 2, 3 | Add `/docs/` (getting started), `/docs/install/`, `/docs/sharing/`, `/docs/joining/`, `/docs/self-hosting/` and `/docs/security/`, written from the README and checked against it. |
| 5 | Mobile navigation shows only "Releases": a CSS rule hides the other links and there is no menu. | 2 | Docs · Releases · GitHub, visible at every width without a menu. |
| 6 | Google Fonts loads Inter in 5 weights and JetBrains Mono in 3 from a third-party CDN, which is render-blocking. | 1, 3 | Self-host IBM Plex Sans and IBM Plex Mono (OFL, WOFF2, two weights each) with system fallbacks. |
| 7 | Contrast failures: the join placeholder (2.41:1), the join border (2.70:1), viewer error text (~3.2:1) and the viewer divider (~2.5:1). Many mobile links are 18–25 px tall. | 2 | Darken the palette tokens (below). Tap targets of at least 44 px for nav, buttons and inputs. |
| 8 | The viewer looks like a different product (navy gradients, macOS traffic-light dots). The loading pill overlaps the wordmark. `/viewer` shows a raw Caddy placeholder in its `<title>`. | 1, 2 | Give the viewer chrome the same palette and type, and fix the `/viewer` title. Do not change `web/viewer.js` behaviour. The "Reconnecting" state shown for unknown sessions is viewer behaviour and is out of scope for the website project. |
| 9 | `/releases/3-0/` is about 8,900 px tall on mobile, has no table of contents, and buries the breaking upgrade. The social card still says "Release preview". | 2, 3 | Put the upgrade warning first and add an "On this page" list. Regenerate the social card. |
| 10 | Existing assets (asciiquarium GIF, wooden-TV mock-up) do not show sharing. | 3 | Do not use them on the homepage. Real captures come later, generated from an actual tmtv session. |
| 11 | There is no comparison with tmate, upterm or plain tmux sharing. | 3 | Add a short, honest comparison. Every claim must be verifiable from the README or upstream docs, including when *not* to use tmtv. |

## Kept

- The green-on-paper palette family and the dark terminal surface.
- The honest security note ("SSH transport is not end-to-end encryption through an untrusted relay"). This voice sets the tone for the rest of the site.
- The skip link and the visible focus rings, which already pass.
- All existing URLs: `/`, `/releases/`, `/releases/3-0/`, `/viewer`, `/s/*`, `/j/*`, `/install.sh`, `/llms.txt`, `/robots.txt`, `/sitemap.xml`.

## Direction: "a well-kept man page"

- **Layout:** one left-aligned column of about 68ch with a wide gutter on desktop, typeset rather than decorated. Docs get a sidebar on desktop and an inline "On this page" list on mobile.
- **Type:** IBM Plex Sans 400/600 and IBM Plex Mono 400/500, self-hosted, on an 18 px base with a 1.25 scale. Sentence case everywhere.
- **Palette:** paper `#f4f2ec`, ink `#1a1d1a`, secondary `#4a514b`, rule `#d6d3c8`, link `#2f5d3a`, terminal `#121412` with text `#d8dcd2`, and an accent `#9fc53a` used only for the cursor and status lines. Every text pair must reach WCAG AA (4.5:1).
- **Homepage order:** plain one-line description → install command → first-run transcript → common tasks (passwords, web sharing, named sessions, recording) → how sharing works and what the relay sees → comparison → join a session → factual footer.

## Discoverability decisions (not design, but recorded here)

- Each page gets a title, description, canonical URL and Open Graph data. JSON-LD uses `SoftwareApplication` plus `WebSite`, and `TechArticle` with `BreadcrumbList` on docs pages.
- The sitemap is generated with `lastmod`. `llms.txt` lists every docs page, `llms-full.txt` holds the full docs as plain markdown, and each docs page is also served as `.md`. Everything is readable without JavaScript.
- Production deploys need a website-only workflow that reuses `scripts/package-site.sh` and `scripts/deploy-web.sh` with the existing secrets and host, with green CI as a precondition. It must not touch relay binaries.
