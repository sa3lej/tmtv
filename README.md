# tmtv

Terminal sharing for IT professionals. Share your terminal over SSH and the web — built on tmux 3.6a.

## Quick start

```sh
# Install
curl -fsSL https://tmtv.se/install.sh | sh

# Start sharing
tmtv
```

That's it. tmtv connects to tmtv.se and prints your session tokens:

```
ssh TOKEN@tmtv.se              # read-write access
ssh ro-TOKEN@tmtv.se           # read-only access
```

### Enable web sharing

SSH sharing is always on. Web sharing is opt-in:

```sh
tmtv set -g tmtv-web-sharing on
# [tmtv] web session: https://tmtv.se/s/TOKEN
```

Share the URL — viewers watch in their browser, no install needed.

To auto-enable web sharing, add to `~/.tmtv.conf`:

```
set -g tmtv-web-sharing on
```

### Interactive web viewers

By default, web viewers can only watch. To let RW web viewers type:

```sh
tmtv set -g tmtv-web-input on
```

When enabled, web viewers connected via the RW token can type in the terminal. RO token viewers remain view-only. A badge in the viewer titlebar shows the session mode ("interactive" or "view-only").

### Named sessions

Pick a memorable name instead of a random token:

```
# In ~/.tmtv.conf
set -g tmtv-session-name demo
```

Your session is now at:
- `ssh 12345-demo@tmtv.se` (SSH read-write, random prefix)
- `ssh ro-demo@tmtv.se` (SSH read-only)
- `https://tmtv.se/s/demo` (web)

Names must be 3-32 characters, alphanumeric and hyphens only.

### Password-protected sessions

Require a password for viewers to join:

```sh
tmtv -p secret
```

SSH viewers must authenticate with the password — public key authentication alone is rejected. Web viewers see a password prompt before the terminal loads. Passwords are SHA-256 hashed server-side and never stored in plain text.

Can also be set in config:

```
# In ~/.tmtv.conf
set -g tmtv-session-password secret
```

## GitHub Action

Debug CI failures by SSH-ing into your runner. Drop-in replacement for `action-tmate`:

```yaml
- name: Debug via tmtv
  if: failure()
  uses: sa3lej/action-tmtv@v1
  with:
    limit-access-to-actor: true
```

Migrating from action-tmate? One-line diff:

```diff
- uses: mxschmitt/action-tmate@v3
+ uses: sa3lej/action-tmtv@v1
```

See [action-tmtv](https://github.com/sa3lej/action-tmtv) for full docs.

## How it works

```
  You (tmtv client)          tmtv-server            Viewers
 +-----------------+      +----------------+     +------------+
 | terminal + tmux |--SSH-->| relay + auth  |--SSH-->| read/write |
 |                 |      |                |     +------------+
 |                 |      |    SSE stream  |--HTTP->| browser    |
 +-----------------+      +----------------+     +------------+
```

## Features

- Full tmux 3.6a terminal multiplexer
- Instant session sharing over SSH with read-write and read-only tokens
- Password-protected sessions — viewers must authenticate before connecting
- Web viewer with xterm.js and WebGL rendering
- Interactive web input — RW web viewers can type when enabled by the host
- Late-join support — browser viewers see current terminal state
- Server-Sent Events streaming — works through proxies and firewalls
- Live viewer counts — `S:N W:N` in tmux status bar and web titlebar
- Dynamic page titles — link previews show the session name (Slack, Discord, iMessage)
- Read-only viewers can disconnect cleanly with Ctrl-Q
- Zero config — just type `tmtv`

## Format variables

Session URLs are available as tmux format variables:

| Variable | Description |
|----------|-------------|
| `#{tmtv_ssh}` | SSH connection string (read-write) |
| `#{tmtv_ssh_ro}` | SSH connection string (read-only) |
| `#{tmtv_web}` | Web viewer URL |
| `#{tmtv_session_name}` | Named session name (if set) |
| `#{tmtv_ssh_viewers}` | Number of connected SSH viewers |
| `#{tmtv_web_viewers}` | Number of connected web viewers |

Example: `tmtv display-message -p '#{tmtv_web}'`

## Status bar

tmtv prepends live viewer counts to the standard tmux status bar. The default `status-right` is:

```
S:#{tmtv_ssh_viewers} W:#{tmtv_web_viewers} "#{=21:pane_title}" %H:%M %d-%b-%y
```

This gives you something like:

```
S:2 W:1 "myhost" 14:30 09-Mar-26
```

Everything after the viewer counts is the stock tmux default — pane title, 12-hour clock, and date. This is set via the tmux options system, so you can override it in `~/.tmtv.conf` like any tmux option.

### Customizing the status bar

Add to `~/.tmtv.conf`:

```
set -g status-right 'S:#{tmtv_ssh_viewers} W:#{tmtv_web_viewers} "#{=21:pane_title}" %H:%M %Y-%m-%d'
```

This keeps the viewer counts and pane title, switches to a 24-hour clock (`%H:%M`), and uses ISO date format (`%Y-%m-%d`).

Any valid tmux `status-right` format works — tmtv is a full tmux, so you have access to all tmux format variables alongside the tmtv-specific ones. See `man tmux` for the complete list of format strings.

## Using tmtv as tmux

tmtv is a full tmux replacement. All tmux commands work:

```sh
tmtv new-session -s work
tmtv split-window -h
tmtv list-sessions
tmtv attach -t work
```

## Self-hosting

Run your own tmtv relay server instead of using tmtv.se.

### Docker Compose (recommended)

```sh
docker compose up -d
```

That's it — SSH sharing on port 2222. Clients connect via `ssh TOKEN@your.host.com -p 2222`.

For production with TLS and web viewing:

```sh
TMTV_DOMAIN=share.example.com docker compose --profile tls up -d
```

SSH host keys persist in a Docker volume. Configuration via environment variables:

| Variable | Description | Default |
|----------|-------------|---------|
| `TMTV_DOMAIN` | Hostname in connection strings | `localhost` |
| `TMTV_SSH_PORT` | SSH listen port | `2222` |
| `TMTV_IDLE_TIMEOUT` | Disconnect idle sessions (seconds) | `3600` |
| `TMTV_MAX_LIFETIME` | Maximum session lifetime (seconds) | `86400` |
| `TMTV_LOG_LEVEL` | Log verbosity (0=quiet, 1+=verbose) | `0` |

### Building from source

#### Linux (Debian/Ubuntu)

```sh
sudo apt-get install build-essential autoconf automake pkg-config \
  libevent-dev libncurses-dev libssh-dev libmsgpack-dev libbsd-dev \
  bison libutf8proc-dev

sh autogen.sh
./configure
make -j$(nproc)
```

This produces `tmtv` (client) and `tmtv-server`.

#### macOS (client only)

```sh
brew install autoconf automake pkg-config libevent libssh msgpack-c bison utf8proc

export PATH="$(brew --prefix bison)/bin:$PATH"
sh autogen.sh
./configure --enable-utf8proc
make -j$(sysctl -n hw.ncpu)
```

### Server setup

#### 1. Generate SSH host keys

```sh
mkdir -p /etc/tmtv/keys
ssh-keygen -t ed25519 -f /etc/tmtv/keys/ssh_host_ed25519_key -N ''
ssh-keygen -t rsa -b 3072 -f /etc/tmtv/keys/ssh_host_rsa_key -N ''
```

#### 2. Install the binary

```sh
sudo cp tmtv-server /usr/local/bin/
```

#### 3. Create a systemd service

```ini
# /etc/systemd/system/tmtv-server.service
[Unit]
Description=tmtv relay server
After=network.target

[Service]
ExecStart=/usr/local/bin/tmtv-server \
  -k /etc/tmtv/keys \
  -p 22 \
  -h your.host.com \
  -v
Restart=always
NoNewPrivileges=yes
ProtectSystem=strict
ReadWritePaths=/tmp

[Install]
WantedBy=multi-user.target
```

```sh
sudo systemctl daemon-reload
sudo systemctl enable --now tmtv-server
```

That's it — SSH sharing works now. Clients connect via `ssh TOKEN@your.host.com`.

#### 4. Web viewer (optional)

Web sharing is off by default. If you want browser-based viewing, add `-z 4002` to the server flags to enable SSE streaming on a local port, then put Caddy in front for TLS and routing.

A production-ready Caddyfile is provided in [`deploy/Caddyfile`](deploy/Caddyfile). It includes Caddy templates for dynamic page titles (link previews show the session name in Slack, Discord, iMessage). Replace `tmtv.se` with your hostname.

Copy the web viewer from the `web/` directory:

```sh
sudo mkdir -p /var/www/tmtv
sudo cp web/viewer.html web/viewer.js /var/www/tmtv/
```

#### 5. Firewall

Open these ports:

| Port | Protocol | Purpose |
|------|----------|---------|
| 22 | TCP | tmtv SSH (clients and viewers) |
| 443 | TCP | HTTPS (web viewer and landing page) |

Port 4002 (SSE) should NOT be exposed — the reverse proxy handles it.

#### Server flags

| Flag | Description | Default |
|------|-------------|---------|
| `-k` | SSH host keys directory | `keys` |
| `-p` | SSH listen port | `2222` |
| `-q` | SSH port shown in connection strings | same as `-p` |
| `-h` | Hostname in connection strings | system hostname |
| `-z` | SSE port for web viewer | disabled |
| `-b` | Bind address | all interfaces |
| `-A` | Require authorized keys (reject clients without `-a`) | off |
| `-x` | Enable PROXY protocol (for use behind load balancers) | off |
| `-V` | Print version and exit | — |
| `-v` | Increase log verbosity | quiet |

#### SSH security

tmtv-server uses libssh (>= 0.9.5) with modern defaults:

- **Key exchange**: curve25519-sha256, ecdh-sha2-nistp256/384/521
- **Ciphers**: chacha20-poly1305, aes256-gcm, aes128-gcm, aes256-ctr, aes128-ctr
- **Host keys**: Ed25519, ECDSA, RSA (SHA-256/SHA-512 signatures only)
- **No weak algorithms**: no 3DES, no DH group1/group14-sha1, no ssh-rsa with SHA-1

### Client config for self-hosted server

```
# ~/.tmtv.conf
set -g tmtv-server-host your.host.com
set -g tmtv-server-port 22
set -g tmtv-server-rsa-fingerprint SHA256:...
set -g tmtv-server-ed25519-fingerprint SHA256:...
```

Get fingerprints:

```sh
ssh-keygen -lf /etc/tmtv/keys/ssh_host_ed25519_key.pub
ssh-keygen -lf /etc/tmtv/keys/ssh_host_rsa_key.pub
```

### Running tests

#### Unit and functional tests

Run locally — no server required:

```sh
cd tests && make all
```

#### Integration tests

Run against a live tmtv-server to test SSH, SSE, web viewer, pane operations, viewer counts, and more (36+ tests).

**Build machine requirements** (where you run the tests):
- `ssh`, `curl`
- `npx` + Playwright for visual tests: `npm i -D playwright && npx playwright install chromium`

**Test machine requirements** (the server):
- `tmtv-server` running via systemd
- `tmtv` client binary
- Caddy (or any reverse proxy) on port 8080 with `/s/*` → `viewer.html` and `/ws/*` → SSE proxy
- `expect` for SSH RW/RO tests: `apt install expect`
- SSH host keys in `/root/keys/`

```sh
# Deploy and test
make && \
  scp tmtv-server root@<host>:/usr/local/bin/tmtv-server && \
  scp tmtv root@<host>:/root/tmtv && \
  scp web/viewer.html web/viewer.js root@<host>:/var/www/tmtv/ && \
  ssh root@<host> systemctl restart tmtv-server && \
  cd tests && TEST_HOST=<host> make integration
```

Quick mode skips slow tests (Playwright, web sharing toggle):

```sh
TEST_HOST=<host> sh test-integration.sh --quick
```

## License

ISC license, same as tmux.

## Support

If tmtv is useful to you, [buy me a coffee](https://buymeacoffee.com/lejo).

## Acknowledgments

- [tmux](https://github.com/tmux/tmux) by Nicholas Marriott and contributors
- [tmate](https://github.com/tmate-io/tmate) by Rene Dumont and contributors
