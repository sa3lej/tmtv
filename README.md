# tmtv

Terminal sharing for developers. View and collaborate on terminal sessions in real time.

tmtv is a fork of [tmate](https://tmate.io) rebased on [tmux 3.6a](https://github.com/tmux/tmux). It adds instant terminal sharing via SSH and a web interface, while staying current with modern tmux features.

## Features

- Full tmux 3.6a functionality (terminal multiplexer)
- Instant terminal sharing via SSH (inherited from tmate)
- Session sharing with read-only and read-write access tokens
- Web interface for viewing active sessions (planned)
- Static binary builds via Docker

## Building

### Dependencies

- C compiler (gcc or clang)
- autoconf, automake, pkg-config, bison
- libevent 2.x
- ncurses
- libssh >= 0.8.4
- msgpack-c
- libbsd (Linux only, for fparseln)
- utf8proc (optional, for better Unicode support)

### Linux (Debian/Ubuntu)

```sh
sudo apt-get install build-essential autoconf automake pkg-config \
  libevent-dev libncurses-dev libssh-dev libmsgpack-dev libbsd-dev \
  bison libutf8proc-dev

sh autogen.sh
./configure
make -j$(nproc)
```

### macOS

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

This runs both unit tests (msgpack encoding/decoding) and functional tests (session management, window creation, key sending, pane splitting).

## Usage

tmtv is a drop-in replacement for tmux:

```sh
./tmtv new-session       # start a new session
./tmtv list-sessions     # list active sessions
./tmtv attach            # attach to a session
```

## Project status

tmtv is under active development. The tmux 3.6a rebase is in progress. Tmate session-sharing hooks are being integrated into the modern tmux codebase.

## License

tmtv is released under the [ISC license](https://en.wikipedia.org/wiki/ISC_license), same as tmux.

## Acknowledgments

- [tmux](https://github.com/tmux/tmux) by Nicholas Marriott and contributors
- [tmate](https://github.com/tmate-io/tmate) by Ren&eacute; Dumont and contributors
