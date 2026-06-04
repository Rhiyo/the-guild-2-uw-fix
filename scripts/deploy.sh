#!/usr/bin/env bash
# Deploy the built d3d9.dll + default uw_fix.ini into the live game directory.
#
# Requires GUILD2_LIVE_DIR (see scripts/env.example.sh). Run from the repo root,
# or via `make deploy`.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
dll="$repo_root/build/d3d9.dll"
ini="$repo_root/dist/uw_fix.ini"

if [[ -z "${GUILD2_LIVE_DIR:-}" ]]; then
    echo "error: GUILD2_LIVE_DIR is not set." >&2
    echo "       source scripts/env.example.sh  (after editing the path), then retry." >&2
    exit 1
fi
if [[ ! -f "$GUILD2_LIVE_DIR/GuildII.exe" ]]; then
    echo "error: GuildII.exe not found in GUILD2_LIVE_DIR:" >&2
    echo "       $GUILD2_LIVE_DIR" >&2
    exit 1
fi
if [[ ! -f "$dll" ]]; then
    echo "error: $dll not found -- run 'make' first." >&2
    exit 1
fi

cp -v "$dll" "$GUILD2_LIVE_DIR/d3d9.dll"
# Don't clobber a user-customised ini; only install the default if none exists.
if [[ -f "$GUILD2_LIVE_DIR/uw_fix.ini" ]]; then
    echo "note: $GUILD2_LIVE_DIR/uw_fix.ini already exists -- left as-is."
else
    cp -v "$ini" "$GUILD2_LIVE_DIR/uw_fix.ini"
fi

echo "Deployed to: $GUILD2_LIVE_DIR"
