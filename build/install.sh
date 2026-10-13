#!/usr/bin/env sh
# nxport installer (Linux) - run: sh install.sh
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"
SRC="$DIR/nxport-1.0.0-linux-x64"
[ -f "$SRC" ] || SRC="$DIR/nxport-linux"
[ -f "$SRC" ] || { echo "nxport binary not found next to install.sh"; exit 1; }
sudo install -m755 "$SRC" /usr/local/bin/nxport
echo "Installed: /usr/local/bin/nxport"
echo "Run: nxport"
