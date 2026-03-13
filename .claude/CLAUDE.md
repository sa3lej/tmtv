# tmtv — Development Guide

## Project Identity

tmtv is a **terminal sharing tool** for IT professionals. It lets users share their terminal sessions over SSH (read-write and read-only) and the web (view-only via xterm.js). It is a fork of tmate, rebased on **tmux 3.6a**, bringing nine years of tmux improvements — popup menus, extended keys, sixel graphics — that tmate never had.

**The core product is terminal sharing over SSH.** Web viewing is a convenience layer — useful, but secondary. Every design decision, every feature, and every priority must reflect this: SSH first, web second.

**Vision:** Become the best pair programming tool in the world for terminal-native developers. Not another IDE plugin — the tool that makes sharing a terminal session as effortless as sharing a link, with the full power of modern tmux underneath.

**Creator:** Lars-Erik Jonsson (SA3LEJ)
**License:** ISC (same as tmux)
**Repository:** https://github.com/sa3lej/tmtv
**Website:** https://tmtv.se
**Current version:** 1.4.5

---

## Git Strategy

**All development happens on the `staging` branch.** Never commit directly to `main`.

### Workflow

1. **Create or switch to `staging`**: `git checkout staging` (create from main if it doesn't exist: `git checkout -b staging main`)
2. **Develop on staging**: Make all changes, commits, and fixes on the `staging` branch. Push to `origin/staging` — this triggers CI builds for Linux amd64 + macOS arm64 and deploys to the staging server for testing.
3. **Test on staging**: Verify on `staging.tmtv.se` (integration tests, manual testing, web preview).
4. **PR to main (squashed)**: When ready for release, create a PR from `staging` → `main`. Squash-merge with a clean release commit message. The PR title follows the pattern from history: `Release v1.2.6` or `Release v1.2.6 — Short headline`. The PR body contains the release notes.
5. **Tag the release**: After merge to main, tag: `git tag v1.2.6 && git push origin v1.2.6`. The tag triggers the full release pipeline.
6. **Rebase staging onto main** (MANDATORY): After the merge and tag, immediately rebase staging so it starts clean from the new main. Skipping this causes divergence, merge conflicts, and broken history.
   ```bash
   git checkout main
   git pull origin main
   git checkout staging
   git rebase main
   git push --force-with-lease origin staging
   ```
   **The release is NOT complete until this step is done.** Every agent must treat this as part of the release sequence, not an optional cleanup.
7. **Update the webpage**: The landing page (`site/src/pages/index.astro`) must be updated to include the new version in the changelog section. This change is part of the staging branch work before the PR — it ships with the release, not after.

### CI/CD Triggers (`.github/workflows/build.yml`)

| Trigger | What runs |
|---------|-----------|
| Push to `staging` | Build Linux amd64 + macOS arm64 (with tests). Fast feedback loop. |
| Push to `main` | Build Astro site → deploy web assets to production via SCP. |
| Tag `v*` | Full build matrix (Linux amd64/arm64/arm32v6/arm32v7/i386, macOS arm64/amd64) → tests → deploy server + web to production → create GitHub Release → update Homebrew tap. |

### GitHub CLI (`gh`) Reference

All agents use the `gh` CLI for GitHub operations. Never use the GitHub web UI for tasks that `gh` can handle.

#### Creating PRs (staging to main)

```bash
# Create a release PR with title and body
gh pr create --base main --head staging --title "Release v1.3.4" --body "$(cat <<'EOF'
## Release v1.3.4 — Catchy headline here

### What's new
- **Feature:** Description
- **Fix:** Description

### Test results
- Unit tests: XX passed
- Integration tests: XX passed
EOF
)"
```

#### Monitoring CI pipelines

```bash
# List recent workflow runs for a branch
gh run list --branch staging --limit 5

# Watch a run in real-time (blocks until complete)
gh run watch <run-id>

# View logs for failed steps only
gh run view <run-id> --log-failed
```

#### Merging PRs

```bash
# Squash-merge a release PR (keep staging branch)
gh pr merge <number> --squash --delete-branch=false
```

**Always** use `--delete-branch=false` — staging is a permanent branch, never delete it.

#### Releases

```bash
# Create a release from a tag (CI usually does this, but for manual use)
gh release create v1.3.4 --title "v1.3.4" --notes "Release notes here..."

# View an existing release
gh release view v1.3.4
```

#### PR checks and comments

```bash
# View CI check status for a PR
gh pr checks <number>

# View PR review comments
gh api repos/sa3lej/tmtv/pulls/<number>/comments
```

### Release Notes Voice

Study the existing releases for tone. Headlines are benefit-first and punchy:
- "Detach and come back." (v1.2.5)
- "Refresh without regret." (v1.2.4)
- "The viewer gets a makeover." (v1.2.3)
- "The status bar that keeps its promises." (v1.2.2)
- "Your fish finally have the right colors." (v1.1.1)
- "Hello, world." (v1.0.0)

GitHub Release notes include: a catchy headline, a "What's new" section with bold-labeled bullet points, test counts, and a link to the full changelog. The landing page changelog mirrors this with one-line summaries per version.

### Branching Rules

- **`staging`** — Active development. All agents work here. CI builds and deploys to staging server.
- **`main`** — Production. Only receives squash-merged PRs from staging. CI deploys web to production.
- **`v*` tags** — Release triggers. Full cross-platform builds, release artifacts, Homebrew tap update.
- **Never** push directly to `main`. Never tag without a merged PR.

---

## Issue Tracking — Beads

This project uses **bd (Beads)** for ALL issue tracking. Do NOT use markdown TODOs, task lists, plan files, or other tracking methods.

### Core Workflow

```bash
# Start every session with context
bd prime

# Find ready work
bd ready

# Claim before working
bd update <id> --claim

# Discover new work while coding? File it immediately
bd create "Title" -p <0-3> -t <task|bug|feature|epic>

# Link dependencies
bd dep add <child> <parent>

# Close when done
bd update <id> --close
```

### Priority Scale

- **P0** — Critical: blocks release, security issue, data loss
- **P1** — High: important feature or bug, needed soon
- **P2** — Medium: valuable improvement, can wait
- **P3** — Low: nice-to-have, backlog

### Rules

- **NEVER** use `bd edit` — it opens an interactive editor that AI agents cannot use.
- **NEVER** pollute the production database with test issues. Use `BEADS_DB=/tmp/test.db` for testing.
- **ALWAYS** use `-f` flags with `cp`, `mv`, `rm` to avoid interactive prompts that hang agents.
- When the user says **"let's land the plane"**, you MUST complete ALL steps including `git push`. The plane is NOT landed until push succeeds. NEVER stop before pushing.

---

## Architecture

```
  You (tmtv client)          tmtv-server              Viewers
 +-----------------+      +----------------+       +------------+
 | terminal + tmux |--SSH-->| relay + auth  |--SSH-->| read/write |
 |                 |      |                |       +------------+
 |                 |      |    SSE stream  |--HTTP->| browser    |
 +-----------------+      +----------------+       +------------+
```

### Repository Structure

```
tmtv/
├── STAGING.md                  # Staging deploy, test, and verify workflow
├── *.c / *.h                  # C source — tmux 3.6a fork with tmtv extensions
├── server/                     # tmtv-server (SSH relay, SSE endpoint)
├── site/                       # Landing page + viewer (Astro + Tailwind) ← EDIT HERE
│   ├── astro.config.mjs        # Astro: static output, @astrojs/tailwind integration
│   ├── tailwind.config.mjs     # Tailwind: custom tmtv color palette + fonts
│   ├── package.json            # astro ^5.17.1, tailwindcss ^3.4.19, @astrojs/tailwind ^6.0.2
│   ├── src/
│   │   ├── layouts/
│   │   │   ├── Base.astro      # Main layout (skip-to-content, OG tags, global CSS)
│   │   │   └── Viewer.astro    # Viewer layout (Caddy template placeholders for <title>)
│   │   ├── pages/
│   │   │   ├── index.astro     # Landing page (~19K — all content lives here)
│   │   │   └── viewer.astro    # Web viewer page shell (loads xterm.js + viewer.js)
│   │   └── styles/
│   │       └── global.css      # @tailwind directives + Google Fonts (JetBrains Mono, Inter)
│   └── public/                 # Static assets (screenshots, install.sh, favicons)
├── web/                        # Web viewer runtime (NOT built by Astro)
│   ├── viewer.js               # Vanilla JS: SSE client, xterm.js, msgpack, full-screen rendering
│   ├── index.html              # ⚠ LEGACY — do not edit, use site/src/pages/index.astro
│   ├── landing.html            # ⚠ LEGACY — do not edit
│   ├── viewer.html             # ⚠ LEGACY — do not edit, use site/src/pages/viewer.astro
│   └── nginx-tmtv.conf         # ⚠ LEGACY — Caddy is the only supported proxy
├── scripts/
│   └── staging-deploy.sh        # Automated staging deploy with safety checks + verification
├── deploy/
│   └── Caddyfile               # Production Caddy config (TLS, SSE proxy, templates)
├── docs/                       # Feature documentation (input-socket.md, etc.)
├── tests/                      # Unit + integration tests
│   ├── Makefile                # `make all` runs local tests
│   ├── test-integration.sh     # `TEST_HOST=<host> sh test-integration.sh` for remote tests
│   ├── test-msgpack.c          # Unit: msgpack serialization
│   ├── test-server-header.c    # Unit: server header parsing
│   └── test-*.sh               # Functional + integration test scripts
├── .github/workflows/
│   └── build.yml               # CI/CD: build, test, deploy, release (single workflow)
├── Dockerfile                  # Alpine 3.20 static binary build (multi-arch via buildx)
├── Makefile.am                 # Autotools build system
├── configure.ac                # Autoconf configuration
└── example_tmtv.conf           # Example tmux/tmtv config
```

---

## Frontend — NEVER Edit HTML Directly

All user-facing pages are built with **Astro** and **Tailwind CSS**, deployed through CI/CD. **You must NEVER edit raw HTML files in `web/` for page content.** Edit the Astro source in `site/src/` and let the pipeline build and deploy.

The one exception is `web/viewer.js` — this is vanilla JavaScript that runs the terminal viewer. It is deployed directly by CI (not processed by Astro) and loaded in the viewer page via `<script is:inline src="/viewer.js">`.

### Stack

| Component | Technology | Version |
|-----------|-----------|---------|
| Site framework | **Astro** | ^5.17.1 |
| CSS framework | **Tailwind CSS** | ^3.4.19 |
| Tailwind integration | **@astrojs/tailwind** | ^6.0.2 |
| Terminal renderer | **xterm.js** | 5.5.0 (CDN) |
| Fonts | JetBrains Mono + Inter | Google Fonts |
| Build output | Static HTML | `site/dist/` |

### Astro Config

```javascript
// site/astro.config.mjs
export default defineConfig({
  integrations: [tailwind({ applyBaseStyles: false })],
  output: 'static',
  build: { inlineStylesheets: 'always' },
});
```

### Tailwind Design Tokens (`site/tailwind.config.mjs`)

```javascript
colors: {
  tmtv: {
    bg: '#0c0c16',                          // Page background
    surface: 'rgba(255, 255, 255, 0.03)',   // Card surfaces
    border: 'rgba(255, 255, 255, 0.06)',    // Subtle borders
    blue: '#6c9efc',                        // Primary accent
    purple: '#c084fc',                      // Secondary accent
    green: '#3ddc84',                       // Success
    red: '#ff6b6b',                         // Error
  },
}
fontFamily: {
  mono: ['"JetBrains Mono"', '"Fira Code"', '"SF Mono"', 'Menlo', 'monospace'],
  sans: ['Inter', '-apple-system', 'sans-serif'],
}
```

### Where to Edit What

| I want to change... | Edit this | NEVER touch |
|---------------------|-----------|-------------|
| Landing page content/layout/changelog | `site/src/pages/index.astro` | `web/index.html`, `web/landing.html` |
| Viewer page HTML shell/styles | `site/src/pages/viewer.astro` | `web/viewer.html` |
| Viewer terminal logic (JS) | `web/viewer.js` | — |
| Page head, meta tags, layout | `site/src/layouts/Base.astro` or `Viewer.astro` | — |
| Design tokens/colors/fonts | `site/tailwind.config.mjs` | — |
| CSS reset/fonts | `site/src/styles/global.css` | — |
| Caddy routing/proxy | `deploy/Caddyfile` | `web/nginx-tmtv.conf` |

### Viewer Architecture

The viewer uses **full-screen streaming via a virtual PTY**. The server attaches a virtual PTY client to the tmux session, captures the complete rendered screen (status bar, pane borders, colors — everything the host sees), and streams it over SSE. The web viewer renders this into a single xterm.js instance. Web viewers see exactly what SSH viewers see.

- **HTML shell**: Built by Astro (`site/src/pages/viewer.astro` → `site/dist/viewer/index.html`). Contains DOM structure, CSS, xterm.js CDN import (`@xterm/xterm@5.5.0`), and Caddy template placeholders for dynamic session titles.
- **Runtime JS**: `web/viewer.js` — vanilla JS, no build step. SSE connection, msgpack parsing, single xterm.js terminal instance, themes (default macOS Terminal.app Clear Dark palette, TV retro). Deployed directly by CI alongside Astro output.
- **Caddy templates**: `Viewer.astro` uses `{{placeholder \`http.regexp.viewer.1\`}}` — Caddy server-side directives that inject the session name into `<title>` and OpenGraph tags for link previews (Slack, Discord, iMessage).
- **Virtual PTY**: The server-side virtual PTY client is excluded from SSH viewer counts so it doesn't inflate the `#{tmtv_ssh_viewers}` format variable.

### Building Locally

```bash
cd site
npm ci
npm run dev      # Dev server at localhost:4321
npm run build    # Production build → site/dist/
npm run preview  # Preview production build
```

---

## Environments

### Production — tmtv.se

- **Reverse proxy:** Caddy (see `deploy/Caddyfile`)
- **Web root:** `/var/www/tmtv`
- **SSH:** Port 22 (tmtv sessions)
- **SSE:** Port 4002 (internal only, proxied by Caddy at `/ws/*`)
- **Deploys:** Automated on `main` push (web) or tag push (everything)

### Staging — staging.tmtv.se

- **Access:** `ssh ubuntu@staging.tmtv.se`
- **Purpose:** Test deployments, integration testing, preview changes before production
- **CI trigger:** Push to `staging` branch builds Linux amd64 + macOS arm64 and deploys to staging
- **Standard workflow:** See `STAGING.md` for the complete deploy, test, and verify procedure
- **Deploy scripts:** `deploy/staging.sh` (original) or `scripts/staging-deploy.sh` (with safety checks and verification)
- **Integration tests:** Must run ON the staging server with `TEST_HOST=localhost WEB_HOST=staging.tmtv.se`. Use `scripts/staging-deploy.sh --test-only` (quick) or `--test-only --full-tests` (with Playwright). See `STAGING.md` section 3 for manual commands.
- **Playwright:** Installed at `/opt/tmtv-tests/` on staging. The test script automatically uses this directory via `TMTV_PLAYWRIGHT_DIR`.

### Deploy Pipeline Detail

Web deploy (main push or tag):
1. CI builds Astro: `cd site && npm ci && npm run build`
2. SCP to staging directory on server:
   - `site/dist/index.html` → landing page
   - `site/dist/viewer/index.html` → renamed to `viewer.html`
   - `web/viewer.js` → viewer runtime (deployed directly, not from Astro)
   - `site/dist/install.sh` + static assets (PNG, GIF, favicons)
3. CI runs `sudo /usr/local/bin/tmtv-deploy-web` to atomically swap staging → live

Server deploy (tag only):
1. CI downloads `linux-amd64` build artifacts
2. SCP `tmtv-server-linux-amd64` and `tmtv-linux-amd64` to staging directory
3. CI runs `sudo /usr/local/bin/tmtv-deploy-server` to replace binaries and restart

---

## Priority Framework

### Tier 1 — Terminal Sharing (SSH) — THE CORE PRODUCT

This is what tmtv is. It must be bulletproof. All Tier 1 work takes priority over everything else.

- Session lifecycle: create, attach, detach, reattach, destroy
- SSH tunneling: read-write tokens, read-only tokens
- tmux 3.6a compatibility: all tmux commands, features, keybindings
- Named sessions (`tmtv-session-name`)
- Viewer counting (SSH viewers — `#{tmtv_ssh_viewers}`)
- Session security: token isolation, SSH crypto (Ed25519, ECDSA, RSA-SHA256/512, no weak algos), input validation
- Client stability: no crashes, no hangs, graceful error handling, detach/reattach
- Cross-platform builds: Linux (amd64, arm64, arm32v7, arm32v6, i386), macOS (arm64, amd64)
- Static binaries: zero external dependencies on Linux (musl/Alpine)
- Self-hosting: easy server setup with clear docs

### Tier 2 — Web Viewing

Useful but secondary. The browser viewer should work reliably, but new features here don't take priority over SSH improvements.

- Full-screen SSE streaming via virtual PTY — web viewers see exactly what SSH viewers see
- xterm.js rendering (256-color, RGB, Unicode, WebGL) in a single terminal instance
- Screen dump on late-join and window switch (no blank screens)
- Web input: Ctrl+B 0/1/2/3 (or custom prefix + number) for window switching from the browser
- Themes (default macOS Terminal.app Clear Dark, TV retro via `?theme=tv`)
- Reconnect on connection loss
- Dynamic page titles for link previews (Caddy templates)
- Web viewer count (`#{tmtv_web_viewers}`)

### Tier 3 — Pair Programming Features

The strategic direction. These transform tmtv from a sharing tool into a collaboration tool. Validate with research, track in Beads.

- Per-user input API (`TMTV_INPUT_SOCKET`) for multiplayer apps, chat, collaborative tools
- Shared clipboard between host and viewers
- Lightweight voice channel (audio alongside terminal, not video)
- Annotations / attention markers in the terminal
- Session persistence across server restarts
- Team features: rooms, auth, ACLs, persistent channels
- API for programmatic session management
- End-to-end encryption option

### Tier 4 — Landing Page & Marketing

Updated with each release. The landing page changelog must stay current. New features get documented in the README and reflected on the website as part of the release PR.

---

## Agent Roles

This project is developed with a multi-agent team. Agents coordinate through Beads.

### Coder (Principal Engineer)

**Owns:** All C source code (tmux fork + tmtv extensions), tmtv-server, `web/viewer.js`.

- Client C code: session lifecycle, SSH tunnels, web sharing toggle, named sessions, format variables, viewer counting
- Server C code: SSH relay, auth, SSE endpoint, session management, screen dump logic
- Viewer JS (`web/viewer.js`): xterm.js integration, SSE client, msgpack, full-screen rendering, themes
- Tests: `tests/` — unit tests (C), functional tests (shell), integration tests (SSH/SSE/Playwright)
- Security: SSH crypto, input validation, session isolation

**Standards:** Test-driven. `-Wall -Wextra` clean. Valgrind-clean. Follow tmux code conventions.

**Testing rule:** Every new feature MUST have integration tests that exercise the actual new behavior BEFORE it is considered done. Existing tests passing is NOT sufficient — if no test covers the new code path, write one. Test both happy path and failure path. If a feature touches SSH and web, test both. A feature shipped without tests is a feature shipped broken. (Learned in v1.2.6: password protection shipped completely broken with zero test coverage.)

### DevOps (Senior DevOps Engineer)

**Owns:** CI/CD (`.github/workflows/build.yml`), Dockerfile, deploy scripts, Caddy config, staging server, release automation.

- GitHub Actions: build matrix, test automation, artifact publishing, deploy jobs, Homebrew tap
- Static binary builds via Alpine Docker (multi-arch with QEMU/buildx)
- Astro site build in CI: `cd site && npm ci && npm run build`
- Caddy reverse proxy: TLS, SSE proxy (`/ws/*`), Caddy templates for dynamic titles
- Staging environment: `ubuntu@staging.tmtv.se`
- Release pipeline: tag → build all platforms → GitHub Release → Homebrew update

**Standards:** Everything in code. Pipelines are production code. Reproducible builds. Staging mirrors production.

### Web Designer (Senior, WCAG 2.1)

**Owns:** Landing page (`site/src/`), viewer page shell (`site/src/pages/viewer.astro`), Tailwind configuration, visual design, accessibility.

**NEVER edit HTML files directly.** All changes go through Astro source in `site/src/`. Files in `web/index.html`, `web/landing.html`, `web/viewer.html` are legacy.

- Landing page: Astro components, visual hierarchy, responsive layout, color system, changelog section
- Viewer shell: DOM structure, CSS, loading/error states, themes
- WCAG 2.1 AA conformance (already achieved in v1.2.0 — maintain and extend)
- Tailwind tokens: colors, typography, spacing in `tailwind.config.mjs`

**Standards:** WCAG 2.1 AA baseline. 4.5:1 contrast for body text. Keyboard navigation. Screen reader tested. Mobile-first. `prefers-reduced-motion` respected.

### Marketing (Senior Strategist)

**Owns:** Positioning, messaging, README, website copy, changelog entries, release notes.

- Product positioning: tmtv as the modern tmate successor, terminal-native pair programming tool
- Changelog entries: benefit-first, catchy one-liners matching established voice
- GitHub Release notes: headline + "What's new" bullets + test counts
- README: structured for quick-start users and self-hosting admins
- SEO: terminal sharing, pair programming, tmux sharing
- Competitive differentiation: vs tmate (dead, tmux 2.4), vs VS Code Live Share (IDE-locked), vs Tuple (proprietary)

**Standards:** Audience is terminal-native IT professionals. Clear beats clever. Every claim testable.

### Researcher (Senior Analyst)

**Owns:** Competitive intelligence, technical investigation, feature prioritization evidence.

- Competitive landscape: tmate, VS Code Live Share, Tuple, Duckly, Code With Me, JetBrains
- Technical research: xterm.js capabilities, SSE vs WebSocket, WebRTC, tmux protocol
- User research: pair programming workflows for terminal-native developers
- Feature prioritization: evidence-based recommendations

**Standards:** Every claim sourced. Confidence levels stated. Name what you don't know.

---

## Agent Coordination

Agents coordinate through Beads, not direct communication.

1. **Cross-domain work**: If you find work outside your domain, create a bead for the right agent. Example: Coder finds broken contrast → `bd create "Viewer status bar fails WCAG AA contrast" -p 1 -t bug`
2. **Dependencies**: Use `bd dep add <child> <parent>` when your work depends on another agent's work.
3. **Handoffs**: When completing work another agent needs, add a note: `bd update <id> --note "CSS changes in viewer.astro lines 45-80, ready for Web Designer review"`
4. **Release coordination**: Before creating a release PR, all agents ensure their changes are on `staging`, tested, and the landing page changelog is updated.

---

## Testing

### Local (no server needed)

```bash
cd tests && make all    # Unit + functional tests
```

### Integration (requires live tmtv-server)

Tests MUST run ON the staging server, not from a local machine. `TEST_HOST=localhost`
puts the script in local mode. `WEB_HOST=staging.tmtv.se` is required so that HTTPS
web tests use the domain that Caddy's TLS cert is issued for (it resolves to 127.0.0.1
on the staging box).

```bash
# Upload and run — quick mode (no Playwright)
scp tests/test-integration.sh ubuntu@staging.tmtv.se:/tmp/test-integration.sh
scp tests/test-password-prompt.js ubuntu@staging.tmtv.se:/tmp/test-password-prompt.js
ssh ubuntu@staging.tmtv.se \
  'TEST_HOST=localhost WEB_HOST=staging.tmtv.se sh /tmp/test-integration.sh --quick'

# Full suite (includes Playwright visual tests)
ssh ubuntu@staging.tmtv.se \
  'TEST_HOST=localhost WEB_HOST=staging.tmtv.se sh /tmp/test-integration.sh'
```

Or use the deploy script (handles upload automatically):

```bash
scripts/staging-deploy.sh --test-only          # quick mode
scripts/staging-deploy.sh --test-only --full-tests  # includes Playwright
```

### Requirements for integration tests

- Server: tmtv-server running (systemd), tmtv client binary, Caddy on port 443, `expect` for SSH tests
- Playwright (for visual tests): installed at `/opt/tmtv-tests/` on the staging server.
  See `STAGING.md` section 3 for the setup commands if the server is rebuilt.

---

## Build

### Linux (Debian/Ubuntu)

```bash
sudo apt-get install build-essential autoconf automake pkg-config \
  libevent-dev libncurses-dev libssh-dev libmsgpack-dev libbsd-dev \
  bison libutf8proc-dev
sh autogen.sh && ./configure && make -j$(nproc)
# Produces: tmtv (client) and tmtv-server
```

### macOS (client only)

```bash
brew install autoconf automake pkg-config libevent libssh msgpack-c bison utf8proc
export PATH="$(brew --prefix bison)/bin:$PATH"
sh autogen.sh && ./configure --enable-utf8proc && make -j$(sysctl -n hw.ncpu)
```

### Static Linux binaries (Docker)

```bash
docker build --build-arg PLATFORM=amd64 --output type=local,dest=./out .
# Produces: out/build/tmtv and out/build/tmtv-server (fully static, musl)
```

---

## Reminders

- **tmtv is a terminal tool for terminal people.** Every feature should feel native to the terminal workflow.
- **SSH is the product. The web viewer is a bonus.** When in doubt, invest in the SSH experience.
- **Simplicity is the product.** `curl | sh` to install, `tmtv` to share. Don't add steps.
- **tmate is the cautionary tale.** It died stuck on tmux 2.4. tmtv must track tmux upstream.
- **The staging branch is where work happens.** Main only receives squash-merged PRs.
- **The landing page ships with the release.** Changelog updates are part of the PR, not a follow-up.
- **Beads is the memory.** File everything. Track everything. The issue database is the project's institutional knowledge.
- **Never edit HTML directly.** Astro builds the site. Tailwind styles it. CI/CD deploys it.
