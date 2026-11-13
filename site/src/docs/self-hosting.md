---
title: Self-hosting a tmtv relay
nav: Self-hosting
order: 5
description: Run your own tmtv relay with Docker Compose or systemd, add the browser viewer behind Caddy, open the right ports and point hosts at it.
updated: 2026-11-13
---

# Self-hosting

The relay, `tmtv-server`, is what hosts and guests connect to. Running your own
means your terminal data only passes through a machine you control. The relay
and every hosting client must run the same major version: 3.0 hosts need a 3.0
relay with matching web assets.

## Docker Compose

From a checkout of the repository:

```sh
docker compose up -d
```

This starts the relay with SSH on port 2222. Set `TMTV_DOMAIN` to your
hostname so printed links use it instead of `localhost`. If you change
`TMTV_SSH_PORT`, printed links still say `-p 2222`, so tell guests the real
port. Hosts point at it (see
[Pointing hosts at your relay](#pointing-hosts-at-your-relay)) and guests join
with `ssh TOKEN@your.host -p 2222`. SSH host keys persist in a Docker volume.

```sh
TMTV_DOMAIN=share.example.com docker compose --profile tls up -d
```

adds Caddy with automatic TLS in front of the relay's internal web stream
(`/ws/*`) and health check (`/healthz`). To serve the browser viewer pages as
well, use the Caddyfile and site build described under
[Browser viewer](#browser-viewer).

| Variable | Meaning | Default |
|----------|---------|---------|
| `TMTV_DOMAIN` | Hostname in connection strings and the TLS certificate | `localhost` |
| `TMTV_SSH_PORT` | SSH port on the Docker host | `2222` |
| `TMTV_IDLE_TIMEOUT` | End a session after this many seconds with no viewers and no terminal activity | `3600` |
| `TMTV_MAX_LIFETIME` | Maximum session lifetime in seconds | `86400` |
| `TMTV_LOG_LEVEL` | Log verbosity, 0 for quiet | `0` |

## systemd

Build `tmtv-server` or download it from a release (Linux binaries only; see
[Install](/docs/install/)),
then create host keys and a service:

```sh
sudo mkdir -p /etc/tmtv/keys
sudo ssh-keygen -t ed25519 -f /etc/tmtv/keys/ssh_host_ed25519_key -N ''
sudo ssh-keygen -t rsa -b 3072 -f /etc/tmtv/keys/ssh_host_rsa_key -N ''
sudo cp tmtv-server /usr/local/bin/
```

```ini
# /etc/systemd/system/tmtv-server.service
[Unit]
Description=tmtv relay server
After=network.target

[Service]
ExecStart=/usr/local/bin/tmtv-server -k /etc/tmtv/keys -p 22 -h your.host.com -v
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

SSH sharing now works: guests join with `ssh TOKEN@your.host.com`.

## Browser viewer

Web sharing needs the relay's web stream and a TLS reverse proxy. Add `-z 4002`
to the server flags, then put [Caddy](https://caddyserver.com) in front using
[`deploy/Caddyfile`](https://github.com/sa3lej/tmtv/blob/develop/deploy/Caddyfile)
with your hostname in place of `tmtv.se`. It serves the viewer at `/s/TOKEN`
and `/j/TOKEN` and fills in the session name for link previews.

Build the site and install the complete bundle, from the same release as the
relay:

```sh
npm ci --prefix site && npm run build --prefix site
sh scripts/package-site.sh "$PWD/tmtv-web.tar.gz"
sudo mkdir -p /var/www/tmtv
sudo tar -xzf tmtv-web.tar.gz -C /var/www/tmtv
```

The bundle contains the built site plus the `viewer.html` and `viewer.js` that
the Caddyfile expects.

## Ports

| Port | Purpose |
|------|---------|
| 22 (or 2222 with Docker) | SSH for hosts and guests |
| 443 | HTTPS for the browser viewer |
| 80 | HTTP for certificate issuance and redirects, if Caddy handles TLS |

Do not expose the web stream port (4002); Caddy proxies it.

## Server flags

| Flag | Meaning | Default |
|------|---------|---------|
| `-k` | SSH host keys directory | `keys` |
| `-p` | SSH listen port | `2222` |
| `-q` | SSH port shown in connection strings | same as `-p` |
| `-h` | Hostname in connection strings | system hostname |
| `-z` | Web stream (SSE) port | off |
| `-b` | Bind address | all interfaces |
| `-A` | Require authorized keys from hosts | off |
| `-x` | Accept the PROXY protocol behind a load balancer | off |
| `-V` | Print the version and exit | |
| `-v` | More logging | quiet |

The relay uses libssh with modern algorithms only: curve25519, ECDH and
diffie-hellman-group16/18-sha512 key exchange; ChaCha20-Poly1305, AES-GCM and AES-CTR ciphers; Ed25519, ECDSA and RSA
host keys with SHA-2 signatures. 3DES, SHA-1 Diffie-Hellman groups and ssh-rsa
with SHA-1 are disabled.

## Pointing hosts at your relay

```
# ~/.tmtv.conf
set -g tmtv-server-host your.host.com
set -g tmtv-server-port 22
set -g tmtv-server-rsa-fingerprint SHA256:...
set -g tmtv-server-ed25519-fingerprint SHA256:...
```

Get the fingerprints on the relay:

```sh
ssh-keygen -lf /etc/tmtv/keys/ssh_host_ed25519_key.pub
ssh-keygen -lf /etc/tmtv/keys/ssh_host_rsa_key.pub
```

## Upgrading

Upgrade the relay, its web assets and all hosting clients together, then restart
running hosts; restarted hosts get new links. Keep the previous binaries and web
bundle until the new version works. The [3.0 release notes](/releases/3-0/)
describe the protocol change.
