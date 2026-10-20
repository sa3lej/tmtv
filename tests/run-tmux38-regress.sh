#!/bin/sh
# Run selected upstream tests with tmtv sharing disabled before cfg_done.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
fixture=$(mktemp -d)
trap 'rm -rf "$fixture"' EXIT HUP INT TERM
printf 'set -g tmtv-server-host ""\n' > "$fixture/local.conf"
cat > "$fixture/tmtv" <<WRAPPER
#!/bin/sh
exec "$root/tmtv" -f "$fixture/local.conf" "\$@"
WRAPPER
chmod 700 "$fixture/tmtv"
export TEST_TMUX="$fixture/tmtv"
cd "$root/regress"
if [ "$#" -eq 0 ]; then
    set -- floating-pane-geometry.sh if-shell-nested.sh if-shell-error.sh \
        prompt-mechanics.sh copy-mode-commands.sh sync-output-copy-cursor.sh
fi
for test in "$@"; do
    case "$test" in
        */*|.*) echo "expected a regression basename" >&2; exit 1 ;;
    esac
    timeout 120 sh "$test"
    printf 'PASS upstream %s\n' "$test"
done
