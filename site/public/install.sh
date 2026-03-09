#!/bin/sh
# tmtv install script — https://tmtv.se
# Usage: curl -fsSL https://tmtv.se/install.sh | sh
set -e

REPO="sa3lej/tmtv"
BINARY="tmtv"

# Pick an install directory that's already in PATH
detect_install_dir() {
  for dir in /usr/local/bin "$HOME/.local/bin" "$HOME/bin"; do
    case ":$PATH:" in
      *":${dir}:"*) [ -d "$dir" ] && [ -w "$dir" ] && echo "$dir" && return ;;
    esac
  done
  # fallback
  echo "$HOME/.local/bin"
}

# --- helpers ---

say() { printf '  %s\n' "$*"; }
err() { printf '  ERROR: %s\n' "$*" >&2; exit 1; }

need() {
  command -v "$1" >/dev/null 2>&1 || err "$1 is required but not found"
}

# --- detect platform ---

detect_os() {
  case "$(uname -s)" in
    Linux*)  echo "linux" ;;
    Darwin*) echo "macos" ;;
    *)       err "Unsupported OS: $(uname -s)" ;;
  esac
}

detect_arch() {
  OS="$1"
  case "$(uname -m)" in
    x86_64|amd64)    echo "amd64" ;;
    aarch64|arm64)
      if [ "$OS" = "linux" ]; then echo "arm64v8"; else echo "arm64"; fi ;;
    armv7l)          echo "arm32v7" ;;
    armv6l)          echo "arm32v6" ;;
    i686|i386)       echo "i386" ;;
    *)               err "Unsupported architecture: $(uname -m)" ;;
  esac
}

# --- resolve latest version ---

latest_version() {
  need curl
  curl -fsSL "https://api.github.com/repos/${REPO}/releases/latest" \
    | grep '"tag_name"' \
    | head -1 \
    | sed 's/.*"tag_name": *"//;s/".*//'
}

# --- main ---

main() {
  need curl

  OS="$(detect_os)"
  ARCH="$(detect_arch "$OS")"

  INSTALL_DIR="$(detect_install_dir)"

  printf '\n  tmtv installer\n\n'
  say "OS:   $OS"
  say "Arch: $ARCH"

  say "Fetching latest version..."
  VERSION="$(latest_version)"
  [ -z "$VERSION" ] && err "Could not determine latest version"
  say "Version: $VERSION"

  ASSET="${BINARY}-${OS}-${ARCH}"
  URL="https://github.com/${REPO}/releases/download/${VERSION}/${ASSET}"
  CHECKSUMS_URL="https://github.com/${REPO}/releases/download/${VERSION}/checksums.txt"

  say "Downloading ${ASSET}..."
  TMPDIR="$(mktemp -d)"
  trap 'rm -rf "$TMPDIR"' EXIT

  curl -fsSL -o "${TMPDIR}/${ASSET}" "$URL" \
    || err "Download failed. Binary may not exist for ${OS}-${ARCH}."

  # verify checksum if sha256sum is available
  if command -v sha256sum >/dev/null 2>&1; then
    say "Verifying checksum..."
    if curl -fsSL -o "${TMPDIR}/checksums.txt" "$CHECKSUMS_URL" 2>/dev/null; then
      EXPECTED="$(grep "${ASSET}$" "${TMPDIR}/checksums.txt" | awk '{print $1}')"
      if [ -n "$EXPECTED" ]; then
        ACTUAL="$(sha256sum "${TMPDIR}/${ASSET}" | awk '{print $1}')"
        [ "$EXPECTED" = "$ACTUAL" ] || err "Checksum mismatch (expected ${EXPECTED}, got ${ACTUAL})"
        say "Checksum OK"
      fi
    fi
  elif command -v shasum >/dev/null 2>&1; then
    say "Verifying checksum..."
    if curl -fsSL -o "${TMPDIR}/checksums.txt" "$CHECKSUMS_URL" 2>/dev/null; then
      EXPECTED="$(grep "${ASSET}$" "${TMPDIR}/checksums.txt" | awk '{print $1}')"
      if [ -n "$EXPECTED" ]; then
        ACTUAL="$(shasum -a 256 "${TMPDIR}/${ASSET}" | awk '{print $1}')"
        [ "$EXPECTED" = "$ACTUAL" ] || err "Checksum mismatch (expected ${EXPECTED}, got ${ACTUAL})"
        say "Checksum OK"
      fi
    fi
  fi

  chmod +x "${TMPDIR}/${ASSET}"

  # install
  mkdir -p "$INSTALL_DIR"
  say "Installing to ${INSTALL_DIR}/${BINARY}..."
  if [ -w "$INSTALL_DIR" ]; then
    mv "${TMPDIR}/${ASSET}" "${INSTALL_DIR}/${BINARY}"
  else
    sudo mv "${TMPDIR}/${ASSET}" "${INSTALL_DIR}/${BINARY}"
  fi

  say "Done! Run 'tmtv' to start a session."
  case ":$PATH:" in
    *":${INSTALL_DIR}:"*) ;;
    *) say "Note: Add ${INSTALL_DIR} to your PATH if 'tmtv' is not found." ;;
  esac
  printf '\n'
}

main
