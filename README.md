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
- Web viewer with xterm.js and WebGL rendering
- Late-join support — browser viewers see current terminal state
- Server-Sent Events streaming — works through proxies and firewalls
- Zero config — just type `tmtv`

## Format variables

Session URLs are available as tmux format variables:

| Variable | Description |
|----------|-------------|
| `#{tmtv_ssh}` | SSH connection string (read-write) |
| `#{tmtv_ssh_ro}` | SSH connection string (read-only) |
| `#{tmtv_web}` | Web viewer URL |
| `#{tmtv_session_name}` | Named session name (if set) |

Example: `tmtv display-message -p '#{tmtv_web}'`

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

Web sharing is off by default. If you want browser-based viewing, add `-z 4002` to the server flags to enable SSE streaming on a local port, then put a reverse proxy in front for TLS and routing.

**Caddy** (recommended — automatic TLS):

```
your.host.com {
    handle /s/* {
        root * /var/www/tmtv
        try_files /viewer.html
        file_server
    }

    handle /ws/* {
        reverse_proxy 127.0.0.1:4002 {
            flush_interval -1
        }
    }

    handle {
        root * /var/www/tmtv
        file_server
    }
}
```

**nginx**:

```nginx
server {
    listen 443 ssl;
    server_name your.host.com;

    root /var/www/tmtv;

    location /s/ {
        try_files /viewer.html =404;
    }

    location ~ ^/ws/ {
        proxy_pass http://127.0.0.1:4002;
        proxy_buffering off;
        proxy_cache off;
        proxy_set_header Connection '';
        proxy_http_version 1.1;
        chunked_transfer_encoding off;
    }
}
```

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

```sh
cd tests && make all
```

## License

ISC license, same as tmux.

## Acknowledgments

- [tmux](https://github.com/tmux/tmux) by Nicholas Marriott and contributors
- [tmate](https://github.com/tmate-io/tmate) by Rene Dumont and contributors
