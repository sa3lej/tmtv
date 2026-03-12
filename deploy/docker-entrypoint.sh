#!/bin/sh
# Copyright (c) 2026 Lars-Erik Jonsson <l@jonsson.es>
# ISC license — see LICENSE for details.
#
# Docker entrypoint for tmtv-server.
# Generates SSH host keys on first run, then starts the server.

set -e

KEYS_DIR="${TMTV_KEYS_DIR:-/data/keys}"
SSH_PORT="${TMTV_SSH_PORT:-2222}"
SSE_PORT="${TMTV_SSE_PORT:-4002}"
HOSTNAME="${TMTV_HOSTNAME:-}"
IDLE_TIMEOUT="${TMTV_IDLE_TIMEOUT:-3600}"
MAX_LIFETIME="${TMTV_MAX_LIFETIME:-86400}"
LOG_LEVEL="${TMTV_LOG_LEVEL:-0}"

# Generate SSH host keys if they don't exist
mkdir -p "$KEYS_DIR"
if [ ! -f "$KEYS_DIR/ssh_host_ed25519_key" ]; then
    echo "Generating Ed25519 host key..."
    ssh-keygen -t ed25519 -f "$KEYS_DIR/ssh_host_ed25519_key" -N "" -q
fi
if [ ! -f "$KEYS_DIR/ssh_host_rsa_key" ]; then
    echo "Generating RSA host key..."
    ssh-keygen -t rsa -b 4096 -f "$KEYS_DIR/ssh_host_rsa_key" -N "" -q
fi

# Build command line
CMD="tmtv-server -k $KEYS_DIR -p $SSH_PORT -z $SSE_PORT"

if [ -n "$HOSTNAME" ]; then
    CMD="$CMD -h $HOSTNAME"
fi
if [ "$IDLE_TIMEOUT" -gt 0 ] 2>/dev/null; then
    CMD="$CMD -T $IDLE_TIMEOUT"
fi
if [ "$MAX_LIFETIME" -gt 0 ] 2>/dev/null; then
    CMD="$CMD -L $MAX_LIFETIME"
fi

# Add verbosity flags
i=0
while [ "$i" -lt "$LOG_LEVEL" ]; do
    CMD="$CMD -v"
    i=$((i + 1))
done

echo "Starting: $CMD"
exec $CMD
