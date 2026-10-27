#!/bin/sh
# Package an already-built static site, including nested release pages and assets.
# This command is local only: it does not upload, install or restart anything.
set -eu
if [ "$#" -ne 1 ]; then
    echo "Usage: sh scripts/package-site.sh /absolute/path/to/tmtv-web.tar.gz" >&2
    exit 2
fi
case "$1" in /*) ;; *) echo "Output must be an absolute path" >&2; exit 2 ;; esac
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
for path in index.html viewer/index.html releases/index.html releases/3-0/index.html social-card.png sitemap.xml install.sh; do
    test -s "$REPO_ROOT/site/dist/$path" || { echo "Missing build output: $path" >&2; exit 1; }
done
STAGE_DIR=$(mktemp -d)
trap 'rm -rf "$STAGE_DIR"' EXIT HUP INT TERM
cp -R "$REPO_ROOT/site/dist/." "$STAGE_DIR/"
# Caddy's existing /s/ and /j/ routes expect these top-level viewer files.
cp "$REPO_ROOT/site/dist/viewer/index.html" "$STAGE_DIR/viewer.html"
cp "$REPO_ROOT/web/viewer.js" "$STAGE_DIR/viewer.js"
tar -czf "$1" -C "$STAGE_DIR" .
printf 'Packaged complete site: %s\n' "$1"
