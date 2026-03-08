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
# [tmtv] Web sharing enabled: https://tmtv.se/s/TOKEN
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
- `ssh demo@tmtv.se` (SSH)
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

Generate SSH host keys:

```sh
mkdir -p /etc/tmtv/keys
ssh-keygen -t rsa -b 3072 -f /etc/tmtv/keys/ssh_host_rsa_key -N ''
ssh-keygen -t ed25519 -f /etc/tmtv/keys/ssh_host_ed25519_key -N ''
```

Start the server:

```sh
tmtv-server -k /etc/tmtv/keys -p 22 -z 4002 -h your.host.com -v
```

#### Server flags

| Flag | Description | Default |
|------|-------------|---------|
| `-k` | SSH host keys directory | `keys` |
| `-p` | SSH listen port | `2222` |
| `-h` | Hostname in connection strings | system hostname |
| `-z` | SSE port for web viewer | disabled |
| `-w` | Web port shown in URL notifications | disabled |
| `-b` | Bind address | all interfaces |
| `-v` | Increase log verbosity | quiet |

### Client config for self-hosted server

```
# ~/.tmtv.conf
set -g tmtv-server-host your.host.com
set -g tmtv-server-port 22
set -g tmtv-server-rsa-fingerprint SHA256:...
set -g tmtv-server-ed25519-fingerprint SHA256:...
```

Get fingerprints: `ssh-keygen -lf /etc/tmtv/keys/ssh_host_ed25519_key.pub`

### Running tests

```sh
cd tests && make all
```

## License

ISC license, same as tmux.

## Acknowledgments

- [tmux](https://github.com/tmux/tmux) by Nicholas Marriott and contributors
- [tmate](https://github.com/tmate-io/tmate) by Rene Dumont and contributors
