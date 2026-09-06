#!/usr/bin/env bash
# Reject incomplete release artifacts before uploading a draft.
set -euo pipefail
cd "${1:?usage: verify-release-assets.sh ARTIFACT_DIRECTORY}"
expected=(tmtv-macos-arm64 tmtv-macos-amd64)
for arch in amd64 arm64v8 arm32v7 arm32v6 i386; do
  expected+=("tmtv-linux-$arch" "tmtv-server-linux-$arch")
done
for binary in "${expected[@]}"; do
  if ! test -s "$binary"; then
    echo "Missing or empty release binary: $binary" >&2
    exit 1
  fi
done
diff <(printf '%s\n' "${expected[@]}" | sort) \
  <(find . -mindepth 1 -maxdepth 1 ! -name checksums.txt -printf '%f\n' | sort)
sha256sum "${expected[@]}" > checksums.txt
sha256sum --check checksums.txt
