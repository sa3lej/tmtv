---
title: Install tmtv
nav: Install
order: 2
description: Install tmtv on Linux or macOS with the install script, Homebrew or release binaries, verify checksums, build from source and upgrade to 3.0.
updated: 2026-11-13
---

# Install

Only the person hosting a session needs tmtv. Guests who join over SSH or in a
browser need nothing new.

## Install script

```sh
curl -fsSL https://tmtv.se/install.sh | sh
```

The script picks the published release for your operating system and
architecture, downloads it with the release's `checksums.txt`, verifies the
SHA-256 checksum and installs nothing if any step fails.
[Read install.sh](/install.sh) before running it if you prefer.

## Homebrew (macOS)

```sh
brew install sa3lej/tap/tmtv
```

## Release binaries

Each 3.x [GitHub release](https://github.com/sa3lej/tmtv/releases/latest) has
tmtv binaries for Linux (amd64, arm64, armv7, armv6, i386) and macOS (Intel
and Apple Silicon), relay binaries for Linux, and a `checksums.txt` file. Download the binary for
your platform and check it:

```sh
sha256sum --check --ignore-missing checksums.txt
```

On macOS use `shasum -a 256 --check --ignore-missing checksums.txt`.

## Build from source

On Debian or Ubuntu:

```sh
sudo apt-get install build-essential autoconf automake pkg-config \
  libevent-dev libncurses-dev libssh-dev libmsgpack-dev libbsd-dev \
  bison libutf8proc-dev
sh autogen.sh
./configure --enable-sixel
make -j$(nproc)
```

This builds `tmtv` and the relay, `tmtv-server`. On macOS (host only):

```sh
brew install autoconf automake pkg-config libevent libssh msgpack-c bison utf8proc
export PATH="$(brew --prefix bison)/bin:$PATH"
sh autogen.sh
./configure --enable-utf8proc --enable-sixel
make -j$(sysctl -n hw.ncpu)
```

Fully static Linux binaries can be built with Docker:

```sh
docker build --build-arg PLATFORM=amd64 --output type=local,dest=./out .
```

Supported platforms are `amd64`, `arm64`, `arm/v7`, `arm/v6` and `i386`.

## Upgrading to 3.0

tmtv 3.0 uses sharing protocol 11, which does not work with 2.x hosts or
relays. The public relay at tmtv.se already runs 3.0, so if you use it, upgrade
your hosting client. If you run your own relay, upgrade the relay and its web
assets together with your hosts.

Replacing the executable does not upgrade a host that is already running.
Save your work and restart it; restarting ends the local tmux server, may stop
programs in its panes, and gives the session new links. See the
[3.0 release notes](/releases/3-0/) for details.
