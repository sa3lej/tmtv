# tmtv

Terminal sharing for IT professionals. Share and view terminal sessions in real time via SSH and the browser.

tmtv is a fork of [tmate](https://tmate.io) rebased on [tmux 3.6a](https://github.com/tmux/tmux). It pairs a **client** (`tmtv`) that runs your terminal with a **server** (`tmtv-server`) that relays the session to SSH viewers and a web interface.

## How it works

```
  You (tmtv client)          tmtv-server            Viewers
 +-----------------+      +----------------+     +------------+
 | terminal + tmux |--SSH-->| relay + auth  |--SSH-->| read/write |
 |                 |      |                |     +------------+
 |                 |      |    SSE stream  |--HTTP->| browser    |
 +-----------------+      +----------------+     +------------+
```

1. Start `tmtv-server` on a host reachable by both you and your viewers
2. Run `tmtv` on your machine -- it connects to the server over SSH
3. The server prints connection strings: an SSH command for terminal viewers and a web URL for browser viewers
4. Share either link with your audience

## Features

- Full tmux 3.6a terminal multiplexer
- Instant session sharing over SSH with read-write and read-only tokens
- Web viewer at `/s/SESSION_TOKEN` with xterm.js and WebGL rendering
- Late-join support -- browser viewers see current terminal state on connect
- Server-Sent Events (SSE) streaming -- works through proxies and firewalls
- Landing page for entering session tokens

## Building

### Dependencies

- C compiler (gcc or clang)
- autoconf, automake, pkg-config, bison
- libevent 2.x, ncurses, libssh (>= 0.8.4), msgpack-c
- libbsd (Linux only)
- utf8proc (optional, better Unicode)

### Linux (Debian/Ubuntu)

```sh
sudo apt-get install build-essential autoconf automake pkg-config \
  libevent-dev libncurses-dev libssh-dev libmsgpack-dev libbsd-dev \
  bison libutf8proc-dev

sh autogen.sh
./configure
make -j$(nproc)
```

This produces two binaries: `tmtv` (client) and `tmtv-server`.

### macOS (client only)

```sh
brew install autoconf automake pkg-config libevent libssh msgpack-c bison utf8proc

export PATH="$(brew --prefix bison)/bin:$PATH"
sh autogen.sh
./configure --enable-utf8proc
make -j$(sysctl -n hw.ncpu)
```

### Docker

```sh
docker build -t tmtv .
docker run --rm tmtv tmtv -V
```

## Running tests

```sh
cd tests && make all
```

## Server setup

### Generate SSH host keys

```sh
mkdir -p /path/to/keys
ssh-keygen -t rsa -f /path/to/keys/ssh_host_rsa_key -N ''
ssh-keygen -t ed25519 -f /path/to/keys/ssh_host_ed25519_key -N ''
```

### Start the server

```sh
tmtv-server -k /path/to/keys -p 2222 -z 4002 -w 8080 -h your.host.com -v
```

### Server flags

| Flag | Description | Default |
|------|-------------|---------|
| `-k` | Path to SSH host keys directory | `keys` |
| `-p` | SSH listen port for client and viewer connections | `22` |
| `-h` | Hostname advertised in connection strings | system hostname |
| `-z` | SSE (Server-Sent Events) port for web viewer streaming | disabled |
| `-w` | Web port shown in the URL notification (typically your reverse proxy port) | disabled |
| `-b` | Bind address (e.g. `127.0.0.1` to listen on loopback only) | all interfaces |
| `-q` | SSH port advertised to clients (if different from `-p`, e.g. behind NAT) | same as `-p` |
| `-A` | Authorized keys only -- reject clients without a matching public key | off |
| `-x` | Enable PROXY protocol (for load balancers like HAProxy) | off |
| `-v` | Increase log verbosity (repeat for more: `-vv`) | quiet |

### Reverse proxy (nginx)

To serve the web viewer and proxy SSE on a single port:

```nginx
server {
    listen 8080;

    # Landing page
    location = / {
        root /var/www/tmtv;
        try_files /landing.html =404;
    }

    # Web viewer at /s/TOKEN
    location /s/ {
        root /var/www/tmtv;
        try_files /viewer.html =404;
    }

    # SSE stream proxy
    location /ws {
        proxy_pass http://127.0.0.1:4002;
        proxy_http_version 1.1;
        proxy_set_header Connection '';
        proxy_buffering off;
        proxy_cache off;
        chunked_transfer_encoding off;
    }
}
```

### systemd service

```ini
[Unit]
Description=tmtv terminal sharing server
After=network.target

[Service]
ExecStart=/usr/local/bin/tmtv-server -k /path/to/keys -p 2222 -z 4002 -w 8080 -h your.host.com -v
Restart=on-failure

[Install]
WantedBy=multi-user.target
```

## Client usage

### Starting a shared session

```sh
tmtv -F your.host.com -p 2222
```

On connect, the server displays:

```
ssh TOKEN@your.host.com -p 2222        # read-write access
ssh ro-TOKEN@your.host.com -p 2222     # read-only access
web session: http://your.host.com:8080/s/TOKEN
```

Share any of these with your viewers.

The `-F` flag sets the server hostname. You can also set it in `~/.tmtv.conf`:

```
set -g tmtv-server-host your.host.com
set -g tmtv-server-port 2222
set -g tmtv-server-rsa-fingerprint SHA256:...
set -g tmtv-server-ed25519-fingerprint SHA256:...
set -g tmtv-web-sharing on   # auto-enable web sharing on connect
```

Get the fingerprints from your server keys:

```sh
ssh-keygen -l -E SHA256 -f /path/to/keys/ssh_host_rsa_key.pub
ssh-keygen -l -E SHA256 -f /path/to/keys/ssh_host_ed25519_key.pub
```

### Web sharing toggle

Web sharing (SSE streaming to browsers) is off by default. Toggle it per session:

```sh
# Enable web sharing -- server replies with the viewer URL
tmtv set -g tmtv-web-sharing on
# [tmtv] Web sharing enabled: http://your.host.com:8080/s/TOKEN

# Disable web sharing -- drops all browser viewers
tmtv set -g tmtv-web-sharing off
# [tmtv] Web sharing disabled
```

SSH sharing (read-write and read-only tokens) is always available regardless of this setting.

To auto-enable web sharing on every session, add to `~/.tmtv.conf`:

```
set -g tmtv-web-sharing on
```

### Using tmtv as tmux

tmtv is a full tmux replacement. All tmux commands work:

```sh
tmtv new-session -s work
tmtv split-window -h
tmtv list-sessions
tmtv attach -t work
```

## Web viewer

The web viewer uses xterm.js with WebGL rendering. Static files are in `web/`:

- `web/landing.html` -- landing page with session token input
- `web/index.html` -- terminal viewer (served at `/s/TOKEN`)
- `web/nginx-tmtv.conf` -- example nginx configuration

The viewer connects to the SSE endpoint at `/ws?token=SESSION_TOKEN` and renders the terminal stream in the browser. Late-joining viewers receive a screen dump of the current terminal state.

### Viewer features

- **View-only badge**: Yellow "VIEW ONLY" indicator so viewers know the session is read-only
- **Session metadata**: Titlebar shows pane count and session duration (live timer)
- **Auto-reconnect**: If the SSE connection drops, the viewer retries with exponential backoff (up to 5 attempts) before showing "Connection lost"
- **Session ended**: Clean distinction between connection loss and session termination

## Architecture

- **tmtv** (client): tmux with tmate hooks that forward PTY data over SSH to the server
- **tmtv-server**: accepts SSH connections, relays terminal data between the session owner and viewers, serves SSE stream for web viewers
- **Protocol**: msgpack-encoded messages over SSH channels (PTY data, layout sync, notifications)
- **Web streaming**: Server-Sent Events with base64-encoded msgpack -- works through any HTTP proxy

## License

tmtv is released under the [ISC license](https://en.wikipedia.org/wiki/ISC_license), same as tmux.

## Acknowledgments

- [tmux](https://github.com/tmux/tmux) by Nicholas Marriott and contributors
- [tmate](https://github.com/tmate-io/tmate) by Rene Dumont and contributors
