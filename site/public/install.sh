#!/bin/sh
# tmtv install script — https://tmtv.se
# Usage: curl -fsSL https://tmtv.se/install.sh | sh
set -e

REPO="sa3lej/tmtv"
INSTALL_DIR="/usr/local/bin"
BINARY="tmtv"

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
    Darwin*) echo "darwin" ;;
    *)       err "Unsupported OS: $(uname -s)" ;;
  esac
}

detect_arch() {
  case "$(uname -m)" in
    x86_64|amd64)    echo "amd64" ;;
    aarch64|arm64)   echo "arm64" ;;
    armv7l)          echo "armv7" ;;
    armv6l)          echo "armv6" ;;
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
  ARCH="$(detect_arch)"

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
  say "Installing to ${INSTALL_DIR}/${BINARY}..."
  if [ -w "$INSTALL_DIR" ]; then
    mv "${TMPDIR}/${ASSET}" "${INSTALL_DIR}/${BINARY}"
  else
    sudo mv "${TMPDIR}/${ASSET}" "${INSTALL_DIR}/${BINARY}"
  fi

  say "Done! Run 'tmtv' to start a session."
  printf '\n'
}

main
