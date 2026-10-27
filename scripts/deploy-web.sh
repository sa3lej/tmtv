#!/bin/sh
# Upload one already-built complete bundle, install, verify, and roll back on failure.
# Invocation deploys content. --provision-helper is for admin-owned staging only.
set -eu
if [ "$#" -lt 4 ] || [ "$#" -gt 5 ]; then
    echo 'Usage: deploy-web.sh BUNDLE USER@HOST PORT HTTPS_ORIGIN [--provision-helper]' >&2
    exit 2
fi
BUNDLE=$1 TARGET=$2 PORT=$3 ORIGIN=$4
case "$TARGET" in ''|-*|*[!a-zA-Z0-9_.@:-]*) echo 'Invalid SSH target' >&2; exit 2 ;; esac
case "$PORT" in ''|*[!0-9]*) echo 'Invalid SSH port' >&2; exit 2 ;; esac
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT HUP INT TERM
ssh_target() {
    if [ -n "${DEPLOY_IDENTITY_FILE:-}" ]; then
        ssh -i "$DEPLOY_IDENTITY_FILE" -o BatchMode=yes -o StrictHostKeyChecking=yes -p "$PORT" "$TARGET" "$@"
    else
        ssh -o BatchMode=yes -o StrictHostKeyChecking=yes -p "$PORT" "$TARGET" "$@"
    fi
}
upload() {
    if [ -n "${DEPLOY_IDENTITY_FILE:-}" ]; then
        scp -i "$DEPLOY_IDENTITY_FILE" -o BatchMode=yes -o StrictHostKeyChecking=yes -P "$PORT" "$1" "$TARGET:$2"
    else
        scp -o BatchMode=yes -o StrictHostKeyChecking=yes -P "$PORT" "$1" "$TARGET:$2"
    fi
}
# Copy first so the bytes hashed and uploaded cannot change with another build.
cp "$BUNDLE" "$TMP/tmtv-web.tar.gz"
SHA=$(python3 -c 'import hashlib,sys;print(hashlib.sha256(open(sys.argv[1],"rb").read()).hexdigest())' "$TMP/tmtv-web.tar.gz")
if [ "${5:-}" = '--provision-helper' ]; then
    ssh_target 'mkdir -p staging'
    upload "$SCRIPT_DIR/../deploy/tmtv-deploy-web.py" 'staging/tmtv-deploy-web.py'
    ssh_target 'sudo -n install -o root -g root -m 0755 staging/tmtv-deploy-web.py /usr/local/bin/tmtv-deploy-web.new && sudo -n mv -f /usr/local/bin/tmtv-deploy-web.new /usr/local/bin/tmtv-deploy-web'
elif [ "$#" -eq 5 ]; then
    echo 'Unknown option' >&2; exit 2
fi
[ "$(ssh_target 'sudo -n /usr/local/bin/tmtv-deploy-web --check')" = 'tmtv-web-bundle-v1' ] || {
    echo 'Install deploy/tmtv-deploy-web.py as the root-owned server helper first.' >&2; exit 1;
}
ssh_target 'mkdir -p staging'
upload "$TMP/tmtv-web.tar.gz" "staging/.tmtv-web-$SHA.tar.gz"
ssh_target "mv -f staging/.tmtv-web-$SHA.tar.gz staging/tmtv-web.tar.gz && sudo -n /usr/local/bin/tmtv-deploy-web --bundle $SHA" > "$TMP/receipt.json"
cat "$TMP/receipt.json"
ID=$(python3 -c 'import json,re,sys;v=json.load(open(sys.argv[1]))["deployment_id"];assert re.fullmatch("[a-f0-9]{32}",v);print(v)' "$TMP/receipt.json")
if ! python3 "$SCRIPT_DIR/verify-web.py" "$TMP/tmtv-web.tar.gz" "$ORIGIN"; then
    echo "Web verification failed; rolling back deployment $ID" >&2
    ssh_target "sudo -n /usr/local/bin/tmtv-deploy-web --rollback $ID" || {
        echo "Rollback failed or superseded: inspect deployment $ID before retrying." >&2; exit 1;
    }
    exit 1
fi
